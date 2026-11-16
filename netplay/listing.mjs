// The public game list ("server browser") -- opt-in, in memory only.
//
// A host that chose PUBLIC on HOST GAME publishes its room here and keeps it
// listed with a heartbeat; anyone can read the list.  A PRIVATE room (the
// default) never comes here at all.
//
// What one listing holds, and nothing else:
//   code     the room's 4-letter code (what a joiner needs)
//   name     the host's player-card name, sanitised (sanitizeName)
//   n        players in the game, 1..max, as the host reports it
//   max      the people the game holds: 4, or 8 in an 8-player game (the
//            host says; 4..16, else 4)
//   mod      the module it runs on, '16' for the 16-Kirby one (8-player
//            games: a joiner's page loads it first), absent otherwise
//   mode     'together' | 'roam'
//   locked   whether the host set a password (the password itself never
//            leaves the host's device: web/online_browse.js)
//   since    when it was first listed (the list's order)
//   heard    when the host was last heard from (expiry)
// plus, for the per-IP bound only, `owner`: a salted 32-bit hash of the
// publisher's address, never returned and never persisted.  No IP address,
// no token, no ROM or build detail is stored in a listing.
//
// Not persisted: the Directory DO (server.mjs) does not write any of this
// to storage, so a heartbeat costs no storage operation.  A DO restart
// empties the list; each public host's next heartbeat (every HEARTBEAT_MS)
// puts its room back.
//
// Ownership: publishing or removing a room needs its resume token (the
// secret POST /v2/rooms returned to the creator only), checked against the
// code directory -- nobody can list, rename or delist someone else's room.

import { normalizeCode, RateLimiter, cryptoRng } from './codes.mjs';

export const LIST_TTL_MS = 75 * 1000;        // gone this long after the last heartbeat
export const HEARTBEAT_MS = 25 * 1000;       // what the page is told to use
export const LIST_MAX = 200;                 // listings held at once, all IPs
export const LIST_PER_IP = 3;                // listings one address may hold
export const LIST_SHOWN = 50;                // listings one GET returns
export const NAME_MAX = 8;                   // platform/port/player_card.h PORT_CARD_NAME_MAX
export const MAX_PLAYERS = 4;                // a listing's max when its host gives none
export const MAX_SEATS = 16;                 // the most a host may say
const NAME_CHARS = /[^A-Z0-9 .!-]/g;          // platform/player_card.c kNameChars

// The player card's character set, at most NAME_MAX, single spaces, trimmed;
// '' (an unnamed card) is shown as PLAYER.
export function sanitizeName(s) {
    if (typeof s !== 'string')
        return 'PLAYER';
    const t = s.slice(0, 64).toUpperCase().replace(NAME_CHARS, '').replace(/ +/g, ' ').trim()
        .slice(0, NAME_MAX).trim();
    return t || 'PLAYER';
}

function fnv1a(str, salt) {
    let h = (0x811c9dc5 ^ salt) >>> 0;
    for (let i = 0; i < str.length; i++) {
        h ^= str.charCodeAt(i);
        h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h;
}

// Listing and publishing limits, per IP, beside codes.mjs makeEdgeLimits:
//   list:    20 burst, one every 2 s (the page polls every 5 s while the
//            list is open);
//   publish: 10 burst, one every 10 s (a heartbeat every 25 s, plus a few
//            changes: a guest in, the mode, the password on or off).
export function makeListLimits(now = () => Date.now()) {
    return {
        list: new RateLimiter({ capacity: 20, refillPerSec: 0.5, now }),
        publish: new RateLimiter({ capacity: 10, refillPerSec: 0.1, now }),
    };
}

export class PublicList {
    constructor({ now = () => Date.now(), rng = cryptoRng, ttlMs = LIST_TTL_MS,
                  max = LIST_MAX, perIp = LIST_PER_IP, shown = LIST_SHOWN } = {}) {
        this.now = now;
        this.ttlMs = ttlMs;
        this.max = max;
        this.perIp = perIp;
        this.shown = shown;
        const b = rng(4);
        this.salt = ((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]) >>> 0;
        this.rooms = new Map();         // roomId -> listing
    }

    get size() { return this.rooms.size; }

    ownerOf(ip) { return fnv1a(String(ip || '?'), this.salt); }

    sweep() {
        const t = this.now();
        for (const [id, e] of this.rooms)
            if (t - e.heard >= this.ttlMs)
                this.rooms.delete(id);
    }

    // -> 'ok' | 'limit-ip' | 'limit-all'
    put(roomId, code, f, ip) {
        this.sweep();
        const t = this.now();
        const owner = this.ownerOf(ip);
        const old = this.rooms.get(roomId);
        if (!old) {
            let mine = 0;
            for (const e of this.rooms.values())
                if (e.owner === owner)
                    mine++;
            if (mine >= this.perIp)
                return 'limit-ip';
            if (this.rooms.size >= this.max)
                return 'limit-all';
        }
        let max = Math.floor(Number(f.max));
        if (!Number.isFinite(max) || max < MAX_PLAYERS || max > MAX_SEATS) max = MAX_PLAYERS;
        let n = Math.floor(Number(f.n));
        if (!Number.isFinite(n)) n = 1;
        n = Math.max(1, Math.min(max, n));
        this.rooms.set(roomId, {
            code, name: sanitizeName(f.name), n, max,
            mode: f.mode === 'roam' ? 'roam' : 'together',
            locked: f.locked === true,
            mod: f.mod === '16' ? '16' : null,
            since: old ? old.since : t, heard: t, owner,
        });
        return 'ok';
    }

    remove(roomId) { return this.rooms.delete(roomId); }

    // The room's server reports its connected count; 0 = everyone gone (the
    // host ended it, or closed the tab): off the list at once.
    occupancy(roomId, n) {
        if (n === 0)
            this.rooms.delete(roomId);
    }

    // What GET returns: open games first, then full ones, oldest listing
    // first within each (so new games append and the page's cursor stays).
    view() {
        this.sweep();
        const t = this.now();
        const all = [...this.rooms.values()];
        all.sort((a, b) => ((a.n >= a.max) - (b.n >= b.max)) || a.since - b.since
                           || (a.code < b.code ? -1 : 1));
        return all.slice(0, this.shown).map((e) => {
            const o = {
                code: e.code, name: e.name, n: e.n, max: e.max, mode: e.mode,
                locked: e.locked, full: e.n >= e.max,
                age: Math.max(0, Math.round((t - e.heard) / 1000)),
            };
            if (e.mod) o.mod = e.mod;
            return o;
        });
    }
}

// The HTTP face, for DirectoryService (directory.mjs): {status, body, headers?}.
//
//   GET  /v2/public                      -> 200 {v, games: [...], every, ttl}
//   POST /v2/public         {code, resume, name, n, mode, locked, [max, mod]}
//                                        -> 200 {ok, listed, every, ttl}
//                                         | 400 bad | 403 forbidden
//                                         | 409 limit | 429 rate
//   POST /v2/public/remove  {code, resume}
//                                        -> 200 {ok, removed} | 403 | 429
//
// 404 never comes from these: a 404 on /v2/public means a server without
// the list (the page says "needs a server update").
export class ListService {
    constructor(dirService, opts = {}) {
        this.svc = dirService;
        this.now = dirService.now;
        this.list = new PublicList({ now: this.now, rng: opts.rng, ttlMs: opts.ttlMs,
                                     max: opts.max, perIp: opts.perIp, shown: opts.shown });
        this.limits = makeListLimits(this.now);
    }

    _limited(kind, ip) {
        const l = this.limits[kind];
        if (l.take(ip || '?'))
            return null;
        const ra = Math.max(1, l.retryAfter(ip || '?'));
        return { status: 429, headers: { 'Retry-After': String(ra) },
                 body: { error: 'rate', msg: `Too many requests. Try again in ${ra} s.`, retryAfter: ra } };
    }

    // The room the caller owns: the code must resolve, and the resume token
    // must be that room's.
    _owned(f) {
        const code = normalizeCode(f && f.code);
        const e = code && this.svc.dir.resolve(code);
        const tok = f && typeof f.resume === 'string' ? f.resume : '';
        const r = tok.length >= 32 && this.svc.dir.resume.get(tok);
        if (!e || !r || r.roomId !== e.roomId)
            return null;
        return e;
    }

    get(ip) {
        const lim = this._limited('list', ip);
        if (lim) return lim;
        return { status: 200, body: { v: 1, games: this.list.view(),
                                      every: HEARTBEAT_MS / 1000, ttl: this.list.ttlMs / 1000 } };
    }

    publish(f, ip) {
        const lim = this._limited('publish', ip);
        if (lim) return lim;
        if (!f || typeof f !== 'object')
            return { status: 400, body: { error: 'bad', msg: 'Expected a JSON object.' } };
        const e = this._owned(f);
        if (!e)
            return { status: 403, body: { error: 'forbidden', msg: 'Not your game, or it has ended.' } };
        const r = this.list.put(e.roomId, e.code, f, ip);
        if (r !== 'ok')
            return { status: 409, body: { error: 'limit', msg: r === 'limit-ip'
                ? 'Too many public games from your network.' : 'The public list is full right now.' } };
        return { status: 200, body: { ok: true, listed: true, every: HEARTBEAT_MS / 1000,
                                      ttl: this.list.ttlMs / 1000 } };
    }

    remove(f, ip) {
        const lim = this._limited('publish', ip);
        if (lim) return lim;
        const e = this._owned(f);
        if (!e)
            return { status: 403, body: { error: 'forbidden', msg: 'Not your game, or it has ended.' } };
        return { status: 200, body: { ok: true, removed: this.list.remove(e.roomId) } };
    }

    occupancy(roomId, n) { this.list.occupancy(roomId, n); }
}
