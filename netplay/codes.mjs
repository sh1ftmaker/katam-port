// Room codes, the code directory and edge rate limiting -- protocol v2.
//
// Pure JavaScript, no I/O: the Cloudflare Directory DO (server.mjs), the
// local dev relay (dev-relay.mjs) and the unit tests (test/) all drive the
// same classes.  docs/design/NETPLAY-V2.md §8 is the design.
//
// A room code is FOUR letters from a 19-letter, vowel-free alphabet:
//
//     B C D F G H J K M N P Q R S T V W X Z
//
// Why this alphabet:
//   - letters only, so a phone shows one keyboard and never switches to
//     digits mid-code; autocapitalise does the rest;
//   - no vowels (A E I O U) and no Y, so no four-letter code spells a word,
//     offensive or otherwise -- the blocklist below only has to catch the
//     handful of look-alike spellings (V for U);
//   - no L, so nothing is confused with I or 1; no O, so nothing with 0;
//   - 19^4 = 130,321 codes.  That is two orders of magnitude more than the
//     rooms that will ever be open at once, so allocation by random probe
//     almost never collides, and small enough to read aloud.
//
// A short code is guessable by design.  It is defended by (a) server-side
// allocation at random from the free codes (no vanity codes, no sequence),
// (b) rate limits per IP on lookups, with failed lookups costing more,
// (c) the host's lock / approve-joins / kick controls, and (d) expiry: a
// code points at its room only while the room is in use, plus an idle
// grace.  A persisted world is never reachable by its short code once that
// lapses -- it is resumed with a long random token (RESUME_TOKEN_BYTES).

export const CODE_ALPHABET = 'BCDFGHJKMNPQRSTVWXZ';
export const CODE_LENGTH = 4;
export const CODE_SPACE = CODE_ALPHABET.length ** CODE_LENGTH;   // 130321

// Input tolerance.  Case, spaces, dashes and dots are dropped; a pasted
// share link is reduced to its ?room= value; and the characters people
// plausibly type for an alphabet letter map to it: digits that look like
// letters, and U for V (the only vowel that resembles a code letter).
const LOOKALIKE = { '2': 'Z', '5': 'S', '6': 'G', '8': 'B', 'U': 'V' };

// Substrings never issued.  Vowel-free codes cannot spell words; these are
// the spellings that read as one anyway.
export const CODE_BLOCKLIST = ['FVCK', 'FVK', 'CVNT', 'KKK', 'NGGR', 'NGR',
                               'FGGT', 'FGT', 'SHT', 'PSSY', 'DCK', 'TWT'];

export function isBlocked(code) {
    for (const w of CODE_BLOCKLIST)
        if (code.includes(w))
            return true;
    return false;
}

export function isValidCode(s) {
    if (typeof s !== 'string' || s.length !== CODE_LENGTH)
        return false;
    for (const c of s)
        if (!CODE_ALPHABET.includes(c))
            return false;
    return !isBlocked(s);
}

// -> the canonical code, or null.  Accepts "bcdf", "B-C D F", "8cdf",
// "https://host/?room=bcdf".
export function normalizeCode(input) {
    if (typeof input !== 'string')
        return null;
    let s = input.trim();
    const m = /[?&#]room=([^&#\s]+)/i.exec(s);
    if (m)
        s = decodeURIComponent(m[1]);
    s = s.toUpperCase().replace(/[\s\-_.]/g, '');
    let out = '';
    for (const ch of s)
        out += LOOKALIKE[ch] || ch;
    return isValidCode(out) ? out : null;
}

// Why an input is not a code, for the join box.  null = it is one.
export function explainCode(input) {
    if (normalizeCode(input))
        return null;
    const s = String(input || '').toUpperCase().replace(/[\s\-_.]/g, '');
    if (!s)
        return 'Enter the 4-letter code';
    if (s.length < CODE_LENGTH)
        return `${CODE_LENGTH - s.length} more letter${CODE_LENGTH - s.length > 1 ? 's' : ''}`;
    if (s.length > CODE_LENGTH && !/room=/i.test(String(input)))
        return `Codes are ${CODE_LENGTH} letters`;
    const bad = [...s].filter((c) => !CODE_ALPHABET.includes(LOOKALIKE[c] || c));
    if (bad.length)
        return `Codes never contain ${[...new Set(bad)].join(' ')} (no vowels, no digits)`;
    return 'Not a valid code';
}

export function codeFromIndex(i) {
    let s = '';
    for (let k = 0; k < CODE_LENGTH; k++) {
        s = CODE_ALPHABET[i % CODE_ALPHABET.length] + s;
        i = Math.floor(i / CODE_ALPHABET.length);
    }
    return s;
}

// Uniform random integer in [0, n) from a byte source.  `rng(nBytes)`
// returns a Uint8Array (crypto.getRandomValues in production, a seeded
// generator in tests).
export function randomBelow(rng, n) {
    const lim = Math.floor(0x100000000 / n) * n;
    for (;;) {
        const b = rng(4);
        const v = ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0;
        if (v < lim)
            return v % n;
    }
}

export function cryptoRng(n) {
    const b = new Uint8Array(n);
    globalThis.crypto.getRandomValues(b);
    return b;
}

// Deterministic generator for tests (xorshift32).
export function seededRng(seed = 1) {
    let x = seed >>> 0 || 1;
    return (n) => {
        const b = new Uint8Array(n);
        for (let i = 0; i < n; i++) {
            x ^= x << 13; x >>>= 0;
            x ^= x >>> 17;
            x ^= x << 5;  x >>>= 0;
            b[i] = x & 0xFF;
        }
        return b;
    };
}

export function randomToken(rng, bytes) {
    const b = rng(bytes);
    let s = '';
    for (const v of b)
        s += v.toString(16).padStart(2, '0');
    return s;
}

export const ROOM_ID_BYTES = 16;        // the Durable Object name: 128 bits
export const RESUME_TOKEN_BYTES = 24;   // 192 bits, never the short code

// --- rate limiting ---------------------------------------------------------
//
// A token bucket per key (an IP, an identity).  `take(key, cost)` spends
// `cost` tokens and says whether there were enough.  Buckets refill
// continuously; idle buckets are swept so memory follows active keys.
export class RateLimiter {
    constructor({ capacity = 10, refillPerSec = 1, now = () => Date.now() } = {}) {
        this.capacity = capacity;
        this.refill = refillPerSec;
        this.now = now;
        this.buckets = new Map();
    }

    _bucket(key) {
        const t = this.now();
        let b = this.buckets.get(key);
        if (!b) {
            b = { tokens: this.capacity, at: t };
            this.buckets.set(key, b);
        } else {
            b.tokens = Math.min(this.capacity, b.tokens + (t - b.at) / 1000 * this.refill);
            b.at = t;
        }
        return b;
    }

    take(key, cost = 1) {
        const b = this._bucket(key);
        if (b.tokens < cost)
            return false;
        b.tokens -= cost;
        return true;
    }

    // Seconds until `cost` tokens are available (for Retry-After).
    retryAfter(key, cost = 1) {
        const b = this._bucket(key);
        return b.tokens >= cost ? 0 : Math.ceil((cost - b.tokens) / this.refill);
    }

    sweep() {
        const t = this.now();
        for (const [k, b] of this.buckets)
            if (b.tokens + (t - b.at) / 1000 * this.refill >= this.capacity)
                this.buckets.delete(k);
    }
}

// The edge policy, one object so the DO and the dev relay agree:
//   lookups: 30 per IP burst, one every 2 s after that (any resolve);
//   misses:  6 per IP burst, one every 20 s (an unknown code) -- the
//            brute-force bound: ~180 guesses an hour per IP, against ~1e5
//            codes, each live one also lockable by its host;
//   creates: 6 per IP burst, one a minute.
export function makeEdgeLimits(now = () => Date.now()) {
    return {
        lookup: new RateLimiter({ capacity: 30, refillPerSec: 0.5, now }),
        miss: new RateLimiter({ capacity: 6, refillPerSec: 0.05, now }),
        create: new RateLimiter({ capacity: 6, refillPerSec: 1 / 60, now }),
    };
}

// --- the directory -----------------------------------------------------------
//
// code -> room, plus resume token -> room.  The room id (a 128-bit random
// hex string) is the Durable Object name; it is never shown to players.
//
// A code is live from allocation until its room has been EMPTY for
// `idleMs` (or was never joined at all for `idleMs`).  Only then can the
// same four letters be handed out again -- a returning player who typed an
// old code reaches either their own room or nothing, never a stranger's.
// Resume tokens live `resumeMs` past the room's last activity (7 days).
export const DIRECTORY_IDLE_MS = 20 * 60 * 1000;
export const RESUME_TTL_MS = 7 * 24 * 3600 * 1000;

export class CodeDirectory {
    constructor({ now = () => Date.now(), rng = cryptoRng,
                  idleMs = DIRECTORY_IDLE_MS, resumeMs = RESUME_TTL_MS } = {}) {
        this.now = now;
        this.rng = rng;
        this.idleMs = idleMs;
        this.resumeMs = resumeMs;
        this.codes = new Map();     // code -> {roomId, created, active, emptySince, occupants}
        this.byRoom = new Map();    // roomId -> code
        this.resume = new Map();    // token -> {roomId, lastActive}
    }

    get size() { return this.codes.size; }

    _free(code) {
        const e = this.codes.get(code);
        if (!e)
            return;
        this.codes.delete(code);
        if (this.byRoom.get(e.roomId) === code)
            this.byRoom.delete(e.roomId);
    }

    _expired(e, t) {
        return e.occupants === 0 && t - e.emptySince >= this.idleMs;
    }

    // -> {code, roomId, resumeToken} or null when the space is exhausted.
    // `roomId` is passed when re-binding a persisted room to a fresh code.
    allocate({ roomId = null, persist = true } = {}) {
        const t = this.now();
        let code = null;
        // Random probes first: with the directory far from full, the first
        // probe succeeds; the scan is the guarantee, not the path.
        for (let i = 0; i < 32 && !code; i++) {
            const c = codeFromIndex(randomBelow(this.rng, CODE_SPACE));
            if (isBlocked(c))
                continue;
            const e = this.codes.get(c);
            if (!e)
                code = c;
            else if (this._expired(e, t)) {
                this._free(c);
                code = c;
            }
        }
        if (!code) {
            const start = randomBelow(this.rng, CODE_SPACE);
            for (let k = 0; k < CODE_SPACE && !code; k++) {
                const c = codeFromIndex((start + k) % CODE_SPACE);
                if (isBlocked(c))
                    continue;
                const e = this.codes.get(c);
                if (!e)
                    code = c;
                else if (this._expired(e, t)) {
                    this._free(c);
                    code = c;
                }
            }
        }
        if (!code)
            return null;
        if (!roomId)
            roomId = randomToken(this.rng, ROOM_ID_BYTES);
        const old = this.byRoom.get(roomId);
        if (old)
            this._free(old);
        this.codes.set(code, { roomId, created: t, emptySince: t, occupants: 0 });
        this.byRoom.set(roomId, code);
        let resumeToken = null;
        if (persist) {
            resumeToken = randomToken(this.rng, RESUME_TOKEN_BYTES);
            this.resume.set(resumeToken, { roomId, lastActive: t });
        }
        return { code, roomId, resumeToken };
    }

    // -> entry or null.  An expired code resolves to nothing even before a
    // sweep has reclaimed it.
    resolve(code) {
        const c = normalizeCode(code);
        if (!c)
            return null;
        const e = this.codes.get(c);
        if (!e || this._expired(e, this.now()))
            return null;
        return { code: c, roomId: e.roomId, occupants: e.occupants };
    }

    codeOf(roomId) { return this.byRoom.get(roomId) || null; }

    // The room reports its head count on every join and leave.
    touch(roomId, occupants) {
        const t = this.now();
        const code = this.byRoom.get(roomId);
        if (code) {
            const e = this.codes.get(code);
            if (e.occupants > 0 && occupants === 0)
                e.emptySince = t;
            e.occupants = occupants;
            if (occupants > 0)
                e.emptySince = t;
        }
        for (const r of this.resume.values())
            if (r.roomId === roomId)
                r.lastActive = t;
    }

    // A persisted world, back under a fresh short code.
    resumeRoom(token) {
        const r = this.resume.get(token);
        const t = this.now();
        if (!r || t - r.lastActive >= this.resumeMs)
            return null;
        r.lastActive = t;
        const live = this.byRoom.get(r.roomId);
        if (live && !this._expired(this.codes.get(live), t))
            return { code: live, roomId: r.roomId, resumeToken: token };
        const a = this.allocate({ roomId: r.roomId, persist: false });
        return a && { code: a.code, roomId: r.roomId, resumeToken: token };
    }

    sweep() {
        const t = this.now();
        for (const [c, e] of this.codes)
            if (this._expired(e, t))
                this._free(c);
        for (const [k, r] of this.resume)
            if (t - r.lastActive >= this.resumeMs)
                this.resume.delete(k);
    }

    toJSON() {
        return { codes: [...this.codes], resume: [...this.resume] };
    }

    load(j) {
        this.codes = new Map(j.codes || []);
        this.byRoom = new Map();
        for (const [c, e] of this.codes)
            this.byRoom.set(e.roomId, c);
        this.resume = new Map(j.resume || []);
    }
}
