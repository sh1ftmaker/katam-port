// SessionCore: the protocol-v2 room.  Lobby, seats, the deadline sequencer,
// the event column and watermark, the input archive, the hash referee,
// donor election and the world-sync relay -- everything with a decision in
// it, with no socket and no clock of its own.
//
// Hosted by the Cloudflare SessionRoom Durable Object (server.mjs), by the
// node dev relay (dev-relay.mjs), and driven directly by the unit tests.
// The host supplies:
//
//   send(connId, data)            data: string (JSON) or Uint8Array
//   close(connId, code, reason)
//   now()                         ms
//   onOccupancy(n)                connected members, for the directory
//
// and calls open / message / closed as sockets come and go, and tick()
// whenever nextWakeIn() says (the deadline timer).
//
// The model is docs/design/NETPLAY-V2.md "Model v2.1": nobody waits for
// anybody.  Each frame is sealed when every live seat's input has arrived,
// or `deadlineMs` after the first one did, whichever is first; a seat that
// missed it is filled with its latest known input ("repeat last") and the
// late value is discarded -- the sealed stream is the truth, for everyone,
// the late sender included.  Peers that drift (late inputs, slow devices,
// a desync, a join) are brought back by a world sync from an elected donor,
// relayed here as opaque chunks.  Seat count is a session parameter.

import {
    T_INPUT, T_FRAMES, T_ARCHIVE, T_CHUNK, decodeInput, encodeFrames,
    decodeChunk, KEYS_MASK, CHUNK_BYTES, T_POSE_UP, poseUpOk, poseDown,
} from './wire.mjs';
import { RateLimiter, randomToken, cryptoRng } from './codes.mjs';

export const PROTO_VERSION = 2;

// Protocol ceiling on seats: u16 in the header, a bitmask per frame.  The
// sim's own limit (4 Kirbys today, PORT_MAX_SEATS) is a separate session
// parameter, `kirbys`, and a client's caps.seats.
export const MAX_SEATS = 1024;

export const FRAME_MS = 1000 / 59.7275;   // the GBA's frame period

// What this room does beyond the base protocol, in `welcome.features`.  A
// client must not send a feature's messages to a room that does not list
// it: an older room closes the socket on an unknown binary type.
export const FEATURES = Object.freeze(['pose']);

// Co-op rooms (hello.sim 'coop/...'): every player runs their own game and
// the room relays poses; such a room never starts (no START, no sequencer).
export const isCoopSim = (sim) => typeof sim === 'string' && sim.startsWith('coop/');

// POSE_UP rate cap, per member: a client sends ~30/s.
export const POSE_RATE = 40, POSE_BURST = 20;

export const DEFAULT_PARAMS = Object.freeze({
    seats: 4,               // input columns (netsim: `seats`, u8 today)
    kirbys: 4,              // sim slots a seat can drive
    deadlineMs: 100,        // seal a frame this long after its first input
    eventLead: 30,          // events stamped this far past the newest input
    hashEvery: 30,          // checkpoint period, frames
    inputStart: 2200,       // first networked frame (boot runs locally before)
    boot: 'world-v2',       // how peers reach inputStart (web/rb_boot.js)
    view: { w: 240, h: 160 },   // session view (spawn margin), max of requests
    syncEveryMs: 10000,     // background world sync per peer; 0 = off
    lagSyncFrames: 90,      // sync a peer whose checkpoints lag by this much
    minSyncGapMs: 2000,     // per-peer floor between syncs
    syncTimeoutMs: 8000,
    donorPolicy: 'majority',    // 'majority' | 'rtt' | 'host'
    hashWaitMs: 1500,       // judge a checkpoint after this long
    dropToAiMs: 1500,       // a silent seat's Kirby goes to the AI
    reconnectGraceMs: 60000,    // ... and the seat stays reserved this long
    hostGraceMs: 10000,     // host role passes on after this long away
    maxSpectators: 16,
    flushMs: 0,             // coalesce sealed-frame broadcasts (big rooms)
    joinSyncDelayMs: 3000,
    excuseAfter: 20,        // frames sealed late in a row before a seat
                            // stops being waited for at all (until on time)
    rejoinAfter: 10,        // ... and on time this many in a row to return
    rosterMs: 250,          // coalesce roster broadcasts in rooms > 8
});

// User-settable before START (host 'settings'); the rest are policy.
const SETTABLE = ['seats', 'deadlineMs', 'syncEveryMs', 'donorPolicy', 'flushMs',
                  'hashEvery', 'lagSyncFrames', 'maxSpectators'];

const VIEW_MIN_W = 240, VIEW_MIN_H = 160, VIEW_MAX_W = 1024, VIEW_MAX_H = 512;

// The spawn branch's rounding (platform/view.c RoundView): whole 16-px
// cells a side, i.e. multiples of 32, clamped.
export function roundView(v, lo, hi) {
    v = Math.ceil(Math.max(lo, Math.min(hi, v | 0)) / 32) * 32;
    return Math.max(lo, Math.min(hi, v));
}

export function sessionViewOf(requests) {
    let w = VIEW_MIN_W, h = VIEW_MIN_H;
    for (const r of requests) {
        if (!r) continue;
        w = Math.max(w, roundView(r.w, VIEW_MIN_W, VIEW_MAX_W));
        h = Math.max(h, roundView(r.h, VIEW_MIN_H, VIEW_MAX_H));
    }
    return { w: Math.min(w, VIEW_MAX_W), h: Math.min(h, VIEW_MAX_H) };
}

// Donor election.  `cands`: members holding the reference state.  The
// policies are evaluated in the design doc; majority is the default:
// longest clean hash history, then lowest RTT to the room, then seated
// longest.  'rtt' puts RTT first; 'host' prefers the host when eligible.
export function electDonor(cands, policy = 'majority', hostMid = -1) {
    if (!cands.length)
        return null;
    const byRtt = (a, b) => (a.rtt ?? 1e9) - (b.rtt ?? 1e9);
    const byClean = (a, b) => (b.cleanStreak || 0) - (a.cleanStreak || 0);
    const bySeat = (a, b) => (a.seatedAt ?? 1e15) - (b.seatedAt ?? 1e15) || a.mid - b.mid;
    const list = cands.slice();
    if (policy === 'host') {
        const h = list.find((m) => m.mid === hostMid);
        if (h) return h;
    }
    if (policy === 'rtt')
        list.sort((a, b) => byRtt(a, b) || byClean(a, b) || bySeat(a, b));
    else
        list.sort((a, b) => byClean(a, b) || byRtt(a, b) || bySeat(a, b));
    return list[0];
}

function cleanName(s) {
    s = String(s || '').replace(/[\u0000-\u001f\u007f<>]/g, '').trim().slice(0, 20);
    return s || 'Player';
}

export class SessionCore {
    constructor(opts = {}) {
        this.send = opts.send || (() => {});
        this.closeConn = opts.close || (() => {});
        this.now = opts.now || (() => Date.now());
        this.rng = opts.rng || cryptoRng;
        this.onOccupancy = opts.onOccupancy || (() => {});
        this.log = opts.log || (() => {});
        this.code = opts.code || '';
        this.roomId = opts.roomId || '';
        this.params = { ...DEFAULT_PARAMS, ...(opts.params || {}) };
        this.phase = 'lobby';
        this.version = null;          // {proto, sim, rom, content}, from the creator
        this.locked = false;
        this.approval = false;
        this.hostMid = -1;
        this.hostAwaySince = 0;
        this.members = new Map();     // mid -> member
        this.byIdentity = new Map();  // identity -> mid
        this.conns = new Map();       // connId -> {mid|-1, ip}
        this.banned = new Set();
        this.nextMid = 1;
        this.helloLimit = new RateLimiter({ capacity: 20, refillPerSec: 0.5, now: this.now });
        this.poseLimit = new RateLimiter({ capacity: POSE_BURST, refillPerSec: POSE_RATE, now: this.now });
        this._resetSequencer();
        this.stats = {
            sealed: 0, fills: 0, lateInputs: 0, framesMsgs: 0, bytesDown: 0, bytesUp: 0,
            desyncs: 0, syncs: 0, syncBytes: 0, syncFails: 0, sealLatencyMs: 0,
            sealLatencyMax: 0, deadlineSeals: 0, poses: 0, poseDrops: 0,
        };
        this.nextStatsAt = 0;
        this.nextSyncId = 1;
        this.clock = null;            // {f, t}: the session's frame clock
        this.syncs = new Map();       // id -> sync
        this.checks = new Map();      // frame -> {at, reports: Map mid->h}
        this.reference = null;        // {f, h, mids}
        this.dirty = false;           // roster changed
    }

    _resetSequencer() {
        const p = this.params;
        this.frontier = p.inputStart;
        this.vals = new Uint16Array(p.seats);
        this.pending = new Map();     // frame -> {keys: Map seat->keys, firstAt}
        this.seatMid = new Array(p.seats).fill(-1);
        this.seatLiveFrom = new Array(p.seats).fill(0);
        this.kirby = new Array(p.kirbys).fill(-1);    // kirby -> seat
        this.archive = [];            // [frame, seat, keys] change records
        this.events = [];
        this.seq = 0;
        this.head = p.inputStart;     // newest input frame heard
        this.wm = p.inputStart + p.eventLead;
        this.out = [];                // sealed frames not yet broadcast
        this.outFirst = this.frontier;
        this.outBase = new Uint16Array(p.seats);
        this.outKey = true;
        this.flushAt = 0;
    }

    // --- transport-facing ---------------------------------------------------

    open(connId, meta = {}) {
        this.conns.set(connId, { mid: -1, ip: meta.ip || '' });
    }

    closed(connId) {
        const c = this.conns.get(connId);
        this.conns.delete(connId);
        if (!c || c.mid < 0)
            return;
        const m = this.members.get(c.mid);
        if (!m || m.connId !== connId)
            return;
        m.connId = null;
        m.leftAt = this.now();
        m.streaming = false;
        this._abortSyncsOf(m);
        if (m.role === 'pending') {
            this._forget(m);
        } else if (m.seat >= 0 && this.phase === 'lobby') {
            this._unseatLobby(m);
        }
        if (m.mid === this.hostMid)
            this.hostAwaySince = this.now();
        this._occupancy();
        this.dirty = true;
        this._flushRoster();
    }

    message(connId, data) {
        const c = this.conns.get(connId);
        if (!c)
            return;
        if (typeof data === 'string') {
            this.stats.bytesUp += data.length;
            if (data.length > 64 * 1024)
                return this._fatal(connId, 'malformed', 'message too large');
            let msg;
            try { msg = JSON.parse(data); } catch (e) {
                return this._fatal(connId, 'malformed', 'bad JSON');
            }
            if (!msg || typeof msg.t !== 'string')
                return this._fatal(connId, 'malformed', 'no type');
            if (c.mid < 0) {
                if (msg.t !== 'hello')
                    return this._fatal(connId, 'malformed', 'hello first');
                return this._hello(connId, c, msg);
            }
            const m = this.members.get(c.mid);
            if (!m || m.connId !== connId)
                return;
            m.lastSeen = this.now();
            return this._control(m, msg);
        }
        const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
        this.stats.bytesUp += bytes.length;
        if (c.mid < 0)
            return this._fatal(connId, 'malformed', 'hello first');
        const m = this.members.get(c.mid);
        if (!m || m.connId !== connId)
            return;
        m.lastSeen = this.now();
        m.bytesUp += bytes.length;
        if (bytes[0] === T_INPUT) {
            const inp = decodeInput(bytes);
            if (!inp)
                return this._fatal(connId, 'malformed', 'bad INPUT');
            return this._input(m, inp);
        }
        if (bytes[0] === T_CHUNK) {
            const ch = decodeChunk(bytes);
            if (!ch)
                return this._fatal(connId, 'malformed', 'bad CHUNK');
            return this._chunk(m, ch, bytes);
        }
        if (bytes[0] === T_POSE_UP)
            return this._pose(m, bytes);
        return this._fatal(connId, 'malformed', 'unknown binary type');
    }

    // Milliseconds until tick() has work, or null.
    nextWakeIn() {
        const t = this.now();
        let at = Infinity;
        if (this.clock && this.phase === 'playing' && (this.pending.size || this._expected(this.frontier).length))
            at = Math.min(at, this._dueAt(this.frontier) + this.params.deadlineMs);
        if (this.out.length && this.params.flushMs)
            at = Math.min(at, this.flushAt);
        if (this.members.size)
            at = Math.min(at, this.nextStatsAt || t + 250);
        if (this.rosterPending)
            at = Math.min(at, (this.rosterAt || 0) + this.params.rosterMs);
        if (at === Infinity)
            return null;
        return Math.max(0, at - t);
    }

    tick() {
        const t = this.now();
        this._advance();
        if (this.out.length && (!this.params.flushMs || t >= this.flushAt))
            this._flush();
        if (t >= this.nextStatsAt) {
            this.nextStatsAt = t + 250;
            this._housekeeping(t);
        }
        if (this.rosterPending)
            this._flushRoster();
    }

    // --- helpers --------------------------------------------------------------

    _tx(m, data) {
        if (!m.connId)
            return;
        const n = typeof data === 'string' ? data.length : data.length;
        m.bytesDown += n;
        this.stats.bytesDown += n;
        this.send(m.connId, data);
    }

    _json(m, obj) { this._tx(m, JSON.stringify(obj)); }

    _fatal(connId, code, msg, extra = {}) {
        this.send(connId, JSON.stringify({ t: 'error', code, msg, ...extra }));
        this.closeConn(connId, 4000, code);
        const c = this.conns.get(connId);
        if (c) c.dead = true;
    }

    _connected() { return [...this.members.values()].filter((m) => m.connId); }

    _occupancy() { this.onOccupancy(this._connected().length); }

    _forget(m) {
        this.members.delete(m.mid);
        this.poseLimit.buckets.delete(m.mid);
        if (this.byIdentity.get(m.identity) === m.mid)
            this.byIdentity.delete(m.identity);
    }

    publicMember(m) {
        return {
            mid: m.mid, name: m.name, role: m.role, seat: m.seat, ready: !!m.ready,
            connected: !!m.connId, kirby: m.seat >= 0 ? this.kirby.indexOf(m.seat) : -1,
            platform: m.platform, rtt: m.rtt ?? null, host: m.mid === this.hostMid,
            behind: m.seat >= 0 && m.latest ? Math.max(0, this.frontier - 1 - m.latest.frame) : 0,
        };
    }

    roster() {
        return {
            t: 'roster', phase: this.phase, host: this.hostMid, locked: this.locked,
            approval: this.approval, seats: this.params.seats, kirbys: this.params.kirbys,
            view: this.params.view, frontier: this.frontier,
            members: [...this.members.values()].map((m) => this.publicMember(m)),
        };
    }

    _flushRoster(force = false) {
        if (!this.dirty)
            return;
        // Big rooms: a roster is O(members) and goes to every member, so a
        // burst of joins would cost O(N^2) per join.  Coalesce.
        const t = this.now();
        if (!force && this.members.size > 8 && t < (this.rosterAt || 0) + this.params.rosterMs) {
            this.rosterPending = true;
            return;
        }
        this.rosterAt = t;
        this.rosterPending = false;
        this.dirty = false;
        const r = JSON.stringify(this.roster());
        for (const m of this.members.values())
            if (m.connId && m.role !== 'pending')
                this._tx(m, r);
    }

    // --- hello and the version gate -----------------------------------------

    _hello(connId, c, h) {
        if (!this.helloLimit.take(c.ip || connId))
            return this._fatal(connId, 'rate', 'too many attempts; wait a minute');
        if (h.proto !== PROTO_VERSION)
            return this._fatal(connId, 'version', 'protocol version differs',
                               { field: 'proto', room: PROTO_VERSION, yours: h.proto });
        if (typeof h.identity !== 'string' || h.identity.length < 16 || h.identity.length > 128)
            return this._fatal(connId, 'malformed', 'identity');
        const mine = { proto: h.proto, sim: String(h.sim || ''), rom: String(h.rom || ''),
                       content: String(h.content || '') };
        const caps = h.caps || {};
        const capSeats = Math.min(MAX_SEATS, Math.max(1, caps.seats | 0 || 4));
        if (!this.version) {
            this.version = mine;
            if (h.create && typeof h.create === 'object')
                this._applySettings(h.create, capSeats, true);
            if (this.params.seats > capSeats)
                this._applySettings({ seats: capSeats }, capSeats, true);
        } else {
            for (const f of ['sim', 'rom', 'content'])
                if (this.version[f] !== mine[f])
                    return this._fatal(connId, 'version', `${f} differs`,
                                       { field: f, room: this.version[f], yours: mine[f] });
            if (capSeats < this.params.seats)
                return this._fatal(connId, 'version', 'client holds fewer seats',
                                   { field: 'seats', room: this.params.seats, yours: capSeats });
        }
        if (this.banned.has(h.identity))
            return this._fatal(connId, 'kicked', 'removed by the host');

        let m = this.members.get(this.byIdentity.get(h.identity));
        const t = this.now();
        if (m) {
            if (m.connId && m.connId !== connId) {
                const old = m.connId;
                m.connId = null;
                this.send(old, JSON.stringify({ t: 'error', code: 'replaced',
                                                msg: 'signed in from another tab' }));
                this.closeConn(old, 4001, 'replaced');
                const oc = this.conns.get(old);
                if (oc) oc.mid = -1;
            }
        } else {
            if (this.locked && this.members.size)
                return this._fatal(connId, 'locked', 'the host locked this lobby');
            const specs = [...this.members.values()].filter((x) => x.role === 'spectator').length;
            if (specs >= this.params.maxSpectators + this.params.seats)
                return this._fatal(connId, 'full', 'room is full');
            m = {
                mid: this.nextMid++, identity: h.identity, role: 'spectator', seat: -1,
                joinedAt: t, seatedAt: null, ready: false, cleanStreak: 0, rtt: null,
                latest: null, lateInputs: 0, fills: 0, bytesUp: 0, bytesDown: 0,
                hashes: new Map(), lastHashFrame: 0, lastSyncAt: 0, needsSync: null,
                sitAt: 0, streaming: false, wantKirby: -1, lateStreak: 0, onTime: 0,
            };
            this.members.set(m.mid, m);
            this.byIdentity.set(h.identity, m.mid);
            if (this.hostMid < 0)
                this.hostMid = m.mid;
            else if (this.approval)
                m.role = 'pending';
        }
        m.connId = connId;
        m.name = cleanName(h.name);
        m.platform = String(h.platform || '').slice(0, 24);
        m.caps = { seats: capSeats, sync: !!caps.sync };
        m.view = h.view && typeof h.view === 'object' ? { w: h.view.w | 0, h: h.view.h | 0 } : null;
        m.lastSeen = t;
        m.leftAt = 0;
        c.mid = m.mid;
        if (m.mid === this.hostMid)
            this.hostAwaySince = 0;
        this._welcome(m);
        if (m.role === 'pending') {
            const host = this.members.get(this.hostMid);
            if (host) this._json(host, { t: 'join_req', mid: m.mid, name: m.name });
        }
        this._occupancy();
        this.dirty = true;
        this._flushRoster();
        if (h.want === 'play' && m.role !== 'pending' && this.phase === 'lobby') {
            this._sit(m, {});
            this._flushRoster();
        }
    }

    _welcome(m) {
        this._json(m, {
            t: 'welcome', mid: m.mid, role: m.role, seat: m.seat, code: this.code,
            phase: this.phase, host: this.hostMid, params: this.params,
            version: this.version, frontier: this.frontier, proto: PROTO_VERSION,
            features: FEATURES,
        });
    }

    // Co-op: relay a pose to everyone else, unparsed.  Malformed or over
    // the rate cap: dropped, never fatal (poses are a stream; the next one
    // supersedes it).
    _pose(m, bytes) {
        if (m.role === 'pending' || !poseUpOk(bytes) || !this.poseLimit.take(m.mid)) {
            this.stats.poseDrops++;
            return;
        }
        this.stats.poses++;
        const down = poseDown(m.mid, bytes);
        for (const o of this.members.values())
            if (o !== m && o.connId && o.role !== 'pending')
                this._tx(o, down);
    }

    // --- control messages -----------------------------------------------------

    _control(m, msg) {
        const isHost = m.mid === this.hostMid;
        if (m.role === 'pending' && msg.t !== 'ping' && msg.t !== 'leave')
            return;
        switch (msg.t) {
        case 'ping':
            if (typeof msg.rtt === 'number' && msg.rtt >= 0 && msg.rtt < 60000)
                m.rtt = Math.round(msg.rtt);
            this._json(m, { t: 'pong', c: msg.c, s: this.now() });
            return;
        case 'name':
            m.name = cleanName(msg.name);
            this.dirty = true;
            break;
        case 'ready':
            m.ready = !!msg.ready;
            this.dirty = true;
            break;
        case 'view':
            m.view = { w: msg.w | 0, h: msg.h | 0 };
            this._viewChanged();
            break;
        case 'sit':
            this._sit(m, msg);
            break;
        case 'stand':
        case 'leave':
            this._stand(m, msg.t === 'leave');
            if (msg.t === 'leave' && m.connId) {
                const id = m.connId;
                this.closeConn(id, 1000, 'left');
                this.closed(id);
                this._forget(m);
            }
            break;
        case 'archive':
            this._sendArchive(m);
            return;
        case 'hash':
            this._hash(m, msg);
            return;
        case 'syncme':
            this._startSync(m, 'ask');
            return;
        case 'sync_have':
        case 'snap':
        case 'snap_fail':
        case 'sync_done':
            this._syncMsg(m, msg);
            return;
        case 'signal':     // M6 seam: WebRTC signalling relay
            {
                const to = this.members.get(msg.to | 0);
                if (to && to.connId && msg.data !== undefined)
                    this._json(to, { t: 'signal', from: m.mid, data: msg.data });
            }
            return;
        case 'start':
            if (isHost && !(this.version && isCoopSim(this.version.sim))) this._start();
            break;
        case 'lock':
            if (isHost) { this.locked = !!msg.on; this.dirty = true; }
            break;
        case 'approval':
            if (isHost) { this.approval = !!msg.on; this.dirty = true; }
            break;
        case 'approve':
        case 'deny':
            if (isHost) this._decide(msg.mid | 0, msg.t === 'approve');
            break;
        case 'kick':
            if (isHost && (msg.mid | 0) !== m.mid) this._kick(msg.mid | 0);
            break;
        case 'settings':
            if (isHost && this.phase === 'lobby' && msg.params)
                this._applySettings(msg.params, m.caps.seats, false);
            break;
        default:
            return;     // unknown control: ignored, for forward compatibility
        }
        this._flushRoster();
    }

    _applySettings(p, capSeats, creating) {
        for (const k of SETTABLE) {
            if (p[k] === undefined) continue;
            if (k === 'donorPolicy') {
                if (['majority', 'rtt', 'host'].includes(p[k])) this.params.donorPolicy = p[k];
                continue;
            }
            const v = Number(p[k]);
            if (!Number.isFinite(v)) continue;
            if (k === 'seats') {
                const s = Math.max(1, Math.min(MAX_SEATS, capSeats, v | 0));
                if (s !== this.params.seats) {
                    const keep = this._seated();
                    this.params.seats = s;
                    this.params.kirbys = Math.min(DEFAULT_PARAMS.kirbys, s);
                    this._resetSequencer();
                    for (const x of keep) { x.seat = -1; x.role = 'spectator'; }
                    for (const x of keep) this._sit(x, {});
                }
            } else if (k === 'deadlineMs') this.params.deadlineMs = Math.max(16, Math.min(2000, v));
            else if (k === 'syncEveryMs') this.params.syncEveryMs = Math.max(0, Math.min(600000, v));
            else if (k === 'flushMs') this.params.flushMs = Math.max(0, Math.min(200, v));
            else if (k === 'hashEvery') this.params.hashEvery = Math.max(1, Math.min(600, v | 0));
            else if (k === 'lagSyncFrames') this.params.lagSyncFrames = Math.max(10, v | 0);
            else if (k === 'maxSpectators') this.params.maxSpectators = Math.max(0, Math.min(1000, v | 0));
        }
        if (p.inputStart !== undefined && creating)
            this.params.inputStart = Math.max(0, p.inputStart | 0), this._resetSequencer();
        this.dirty = true;
    }

    _seated() { return [...this.members.values()].filter((m) => m.seat >= 0); }

    _decide(mid, ok) {
        const m = this.members.get(mid);
        if (!m || m.role !== 'pending')
            return;
        if (!ok) {
            if (m.connId) this._fatal(m.connId, 'denied', 'the host declined');
            this._forget(m);
        } else {
            m.role = 'spectator';
            this._welcome(m);
        }
        this.dirty = true;
    }

    _kick(mid) {
        const m = this.members.get(mid);
        if (!m)
            return;
        this.banned.add(m.identity);
        this._stand(m, true);
        if (m.connId) {
            const id = m.connId;
            this._fatal(id, 'kicked', 'removed by the host');
            this.closed(id);
        }
        this._freeSeat(m);
        this._forget(m);
        this.dirty = true;
    }

    // --- seats ----------------------------------------------------------------

    _freeSeatIndex(pref) {
        if (pref >= 0 && pref < this.params.seats && this.seatMid[pref] < 0)
            return pref;
        return this.seatMid.indexOf(-1);
    }

    _sit(m, msg) {
        if (m.seat >= 0) {
            // A reconnect that is already seated in the lobby: nothing to do.
            if (this.phase === 'lobby' || m.seatLive)
                return;
        }
        const pref = m.seat >= 0 ? m.seat : (typeof msg.seat === 'number' ? msg.seat | 0 : -1);
        const seat = this._freeSeatIndex(m.seat >= 0 && this.seatMid[m.seat] === m.mid ? -1 : pref);
        const own = m.seat >= 0 && this.seatMid[m.seat] === m.mid;
        if (!own && seat < 0) {
            this._json(m, { t: 'notice', code: 'seats-full', msg: 'every seat is taken -- watching' });
            return;
        }
        if (!own) {
            m.seat = seat;
            this.seatMid[seat] = m.mid;
        }
        m.role = 'player';
        m.seatedAt = m.seatedAt ?? this.now();
        if (this.phase === 'lobby') {
            this._viewChanged();
            this.dirty = true;
            return;
        }
        // In play: a column goes live at an event frame; the Kirby the
        // identity drove before, if the AI still has it, else the lowest.
        const E = this._eventFrame();
        this.seatLiveFrom[m.seat] = E;
        m.seatLive = true;
        m.latest = null;
        this._event(E, 'seat', { seat: m.seat, mid: m.mid });
        let k = m.wantKirby >= 0 && this.kirby[m.wantKirby] < 0 ? m.wantKirby : this.kirby.indexOf(-1);
        if (k >= 0) {
            this.kirby[k] = m.seat;
            m.wantKirby = k;
            this._event(E, 'slot', { slot: k, seat: m.seat });
        }
        m.sitAt = this.now();
        m.needsSync = 'join';
        this._viewChanged();
        this.dirty = true;
    }

    _unseatLobby(m) {
        if (m.seat >= 0 && this.seatMid[m.seat] === m.mid)
            this.seatMid[m.seat] = -1;
        m.seat = -1;
        m.role = 'spectator';
        m.ready = false;
        this._viewChanged();
    }

    // Leave the seat; in play the column is sealed and the Kirby handed to
    // the AI at an event frame.  The seat stays reserved for the identity
    // (a reconnect), unless `free`.
    _stand(m, free) {
        if (m.seat < 0)
            return;
        if (this.phase === 'lobby') {
            this._unseatLobby(m);
            this.dirty = true;
            return;
        }
        if (m.seatLive)
            this._sealSeat(m);
        if (free)
            this._freeSeat(m);
        else {
            m.role = 'spectator';
        }
        this.dirty = true;
    }

    _sealSeat(m) {
        const E = this._eventFrame();
        m.seatLive = false;
        this.seatLiveFrom[m.seat] = Infinity;
        this._event(E, 'unseat', { seat: m.seat });
        const k = this.kirby.indexOf(m.seat);
        if (k >= 0) {
            this.kirby[k] = -1;
            this._event(E, 'slot', { slot: k, seat: -1 });
        }
        this._viewChanged();
    }

    _freeSeat(m) {
        if (m.seat >= 0 && this.seatMid[m.seat] === m.mid)
            this.seatMid[m.seat] = -1;
        m.seat = -1;
        m.role = 'spectator';
    }

    _viewChanged() {
        const v = sessionViewOf(this._seated().filter((m) => m.seat >= 0 && (this.phase === 'lobby' || m.seatLive))
                                            .map((m) => m.view));
        if (v.w === this.params.view.w && v.h === this.params.view.h)
            return;
        this.params.view = v;
        if (this.phase === 'playing')
            this._event(this._eventFrame(), 'view', { w: v.w, h: v.h });
        this.dirty = true;
    }

    // --- start ----------------------------------------------------------------

    _start() {
        if (this.phase !== 'lobby')
            return;
        this.phase = 'playing';
        const p = this.params;
        this._flushFramesNow();
        // Founders: seat s drives Kirby s (s < kirbys), live from inputStart.
        const E = p.inputStart;
        for (const m of this._seated()) {
            this.seatLiveFrom[m.seat] = E;
            m.seatLive = true;
            m.sitAt = this.now();
            this._event(E, 'seat', { seat: m.seat, mid: m.mid }, true);
            if (m.seat < p.kirbys) {
                this.kirby[m.seat] = m.seat;
                m.wantKirby = m.seat;
                this._event(E, 'slot', { slot: m.seat, seat: m.seat }, true);
            }
        }
        this._event(E, 'view', { w: p.view.w, h: p.view.h }, true);
        const startMsg = JSON.stringify({ t: 'start', params: p, frame: E,
                                          events: this.events });
        for (const m of this.members.values())
            if (m.connId && m.role !== 'pending') {
                this._tx(m, startMsg);
                m.streaming = true;
            }
        this.dirty = true;
    }

    _eventFrame() {
        const E = Math.max(this.wm, this.head + this.params.eventLead,
                           this.frontier + this.params.eventLead);
        return E;
    }

    _event(frame, kind, args, quiet = false) {
        const ev = { seq: ++this.seq, f: frame, kind, ...args };
        this.events.push(ev);
        if (frame > this.wm)
            this.wm = frame;
        if (quiet)
            return ev;
        const s = JSON.stringify({ t: 'ev', ...ev });
        for (const m of this.members.values())
            if (m.connId && m.streaming)
                this._tx(m, s);
        return ev;
    }

    // --- the sequencer ----------------------------------------------------------

    _input(m, inp) {
        if (this.phase !== 'playing' || m.seat < 0 || !m.seatLive)
            return;
        const seat = m.seat;
        const t = this.now();
        for (let i = 0; i < inp.keys.length; i++) {
            const f = inp.first + i;
            const k = inp.keys[i];
            if (!m.latest || f >= m.latest.frame)
                m.latest = { frame: f, keys: k };
            if (f < this.seatLiveFrom[seat])
                continue;
            if (f < this.frontier) {
                m.lateInputs++;
                this.stats.lateInputs++;
                continue;
            }
            if (f > this.frontier + 3600)
                continue;           // absurdly far ahead: ignore
            if (!this.clock)
                this.clock = { f, t };
            let p = this.pending.get(f);
            if (!p) {
                p = { keys: new Map(), firstAt: t };
                this.pending.set(f, p);
            }
            if (!p.keys.has(seat))
                p.keys.set(seat, k);
            // On time again for `rejoinAfter` frames in a row: waited for
            // again.  (A 30 Hz device is on time every other frame -- it
            // must not flap back into the expected set.)
            if (++m.onTime >= this.params.rejoinAfter)
                m.lateStreak = 0;
            if (f > this.head)
                this.head = f;
        }
        this._advance();
        if (this.out.length && !this.params.flushMs)
            this._flush();
    }

    _earliestPending() {
        let best = null;
        for (const [f, p] of this.pending)
            if (f >= this.frontier && (!best || p.firstAt < best.firstAt))
                best = p;
        return best;
    }

    // Seats whose input the sequencer waits for at frame f.
    _expected(f) {
        const out = [];
        for (let s = 0; s < this.params.seats; s++) {
            const mid = this.seatMid[s];
            if (mid < 0 || f < this.seatLiveFrom[s])
                continue;
            const m = this.members.get(mid);
            if (m && m.connId && m.seatLive && m.lateStreak < this.params.excuseAfter)
                out.push(s);
        }
        return out;
    }

    // The session clock: frame F is due at clock.t + (F - clock.f) * PERIOD,
    // anchored on the first input heard (and again after the room fell
    // idle).  A frame is sealed when every expected seat's input is in, or
    // `deadlineMs` after it was due -- whichever is first.  Due times come
    // from the clock, not from whoever sent first: a peer stamping its
    // inputs far ahead must not shorten everyone else's deadline.
    _dueAt(F) {
        return this.clock.t + (F - this.clock.f) * FRAME_MS;
    }

    clockFrame(t = this.now()) {
        return this.clock ? this.clock.f + (t - this.clock.t) / FRAME_MS : null;
    }

    _advance() {
        if (this.phase !== 'playing')
            return;
        const t = this.now();
        const dl = this.params.deadlineMs;
        for (;;) {
            const F = this.frontier;
            const p = this.pending.get(F);
            const exp = this._expected(F);
            if (!exp.length && !p) {
                if (!this.pending.size) this.clock = null;   // idle: re-anchor later
                return;
            }
            if (!this.clock)
                return;
            const due = this._dueAt(F);
            const all = p && exp.every((s) => p.keys.has(s));
            if (!all && t < due + dl)
                return;
            this._seal(F, p, exp, !all);
            if (!all)
                this.stats.deadlineSeals++;
            const lat = Math.max(0, t - due);
            this.stats.sealLatencyMs += lat;
            if (lat > this.stats.sealLatencyMax)
                this.stats.sealLatencyMax = lat;
        }
    }

    _seal(F, p, exp, late) {
        const v = this.vals.slice();
        for (let s = 0; s < this.params.seats; s++) {
            if (p && p.keys.has(s)) {
                v[s] = p.keys.get(s);
                continue;
            }
            const mid = this.seatMid[s];
            const m = mid >= 0 ? this.members.get(mid) : null;
            if (m && m.seatLive && F >= this.seatLiveFrom[s]) {
                // repeat last: the newest input this seat sent at or before
                // F, else its last sealed value.  Never waited for.
                if (m.connId) {
                    if (m.latest && m.latest.frame <= F)
                        v[s] = m.latest.keys;
                    m.fills++;
                    m.lateStreak++;
                    m.onTime = 0;
                    this.stats.fills++;
                } else {
                    v[s] = 0;     // gone: neutral until the seat is sealed
                }
            } else {
                v[s] = 0;
            }
        }
        for (let s = 0; s < this.params.seats; s++)
            if (v[s] !== this.vals[s])
                this.archive.push([F, s, v[s]]);
        this.vals = v;
        this.pending.delete(F);
        this.frontier = F + 1;
        this.stats.sealed++;
        if (!this.out.length) {
            this.outFirst = F;
            this.flushAt = this.now() + this.params.flushMs;
        }
        this.out.push(v);
        if (this.head < F)
            this.head = F;
    }

    _flush() {
        if (!this.out.length)
            return;
        const wm = Math.max(this.wm, this.head + this.params.eventLead);
        this.wm = wm;
        // Chunks of at most 1024 frames so a catch-up burst stays small.
        for (let i = 0; i < this.out.length; i += 1024) {
            const part = this.out.slice(i, i + 1024);
            const base = i ? this.out[i - 1] : this.outBase;
            const bytes = encodeFrames(T_FRAMES, this.params.seats, this.outFirst + i, part,
                                       base, wm, this.outKey);
            this.outKey = false;
            this.stats.framesMsgs++;
            for (const m of this.members.values())
                if (m.connId && m.streaming)
                    this._tx(m, bytes);
        }
        this.outBase = this.out[this.out.length - 1];
        this.out = [];
    }

    _flushFramesNow() { this._flush(); }

    // The whole sealed stream since inputStart, plus every event: what a
    // log-replay joiner needs.  After this the member is on the live stream.
    _sendArchive(m) {
        if (this.phase !== 'playing') {
            this._json(m, { t: 'notice', code: 'not-started', msg: 'the game has not started' });
            return;
        }
        this._flush();
        const p = this.params;
        const from = p.inputStart, to = this.frontier;
        this._json(m, { t: 'archive_begin', from, to, events: this.events });
        const cur = new Uint16Array(p.seats);
        let r = 0;
        let key = true;
        const BATCH = 4096;
        for (let f0 = from; f0 < to; f0 += BATCH) {
            const frames = [];
            const base = cur.slice();
            for (let f = f0; f < Math.min(to, f0 + BATCH); f++) {
                while (r < this.archive.length && this.archive[r][0] === f) {
                    cur[this.archive[r][1]] = this.archive[r][2];
                    r++;
                }
                frames.push(cur.slice());
            }
            this._tx(m, encodeFrames(T_ARCHIVE, p.seats, f0, frames, base, this.wm, key));
            key = false;
        }
        this._json(m, { t: 'archive_end', to });
        m.streaming = true;
        m.archivedAt = this.now();
        // The live stream continues from `to` with the shared delta base,
        // which equals `cur` now: the decoder state matches.
    }

    // --- hash referee -------------------------------------------------------------

    _hash(m, msg) {
        const f = msg.f >>> 0;
        const h = String(msg.h || '').slice(0, 32);
        const p = this.params;
        if (this.phase !== 'playing' || !h || f < p.inputStart || (f - p.inputStart) % p.hashEvery || f >= this.frontier + 1)
            return;
        m.hashes.set(f, h);
        if (m.hashes.size > 16)
            m.hashes.delete(m.hashes.keys().next().value);
        if (f > m.lastHashFrame)
            m.lastHashFrame = f;
        let c = this.checks.get(f);
        if (!c) {
            c = { at: this.now(), reports: new Map(), done: false };
            this.checks.set(f, c);
        }
        if (c.done) {
            // A straggler: judge it against the verdict.
            if (this.reference && this.reference.f === f && h !== this.reference.h)
                this._mismatch(m, f);
            else if (this.reference && this.reference.f === f)
                m.cleanStreak++;
            return;
        }
        c.reports.set(m.mid, h);
        const waiting = this._seated().filter((x) => x.connId && x.seatLive && f >= this.seatLiveFrom[x.seat]);
        if (waiting.every((x) => c.reports.has(x.mid)))
            this._judge(f);
    }

    _judge(f) {
        const c = this.checks.get(f);
        if (!c || c.done)
            return;
        c.done = true;
        const groups = new Map();
        for (const [mid, h] of c.reports) {
            if (!groups.has(h)) groups.set(h, []);
            groups.get(h).push(mid);
        }
        if (!groups.size)
            return;
        // Majority; a tie goes to the group holding the best donor.
        let best = null;
        for (const [h, mids] of groups) {
            const cand = electDonor(mids.map((x) => this.members.get(x)).filter(Boolean),
                                    this.params.donorPolicy, this.hostMid);
            const score = [mids.length, cand ? cand.cleanStreak : 0, cand ? -(cand.rtt ?? 1e9) : 0];
            if (!best || score[0] > best.score[0]
                || (score[0] === best.score[0] && (score[1] > best.score[1]
                    || (score[1] === best.score[1] && score[2] > best.score[2]))))
                best = { h, mids, score };
        }
        if (!this.reference || f >= this.reference.f)
            this.reference = { f, h: best.h, mids: best.mids };
        for (const mid of best.mids) {
            const m = this.members.get(mid);
            if (m) m.cleanStreak++;
        }
        if (groups.size > 1) {
            this.stats.desyncs++;
            const g = [...groups].map(([h, mids]) => ({ h, mids }));
            const note = JSON.stringify({ t: 'desync', f, groups: g, ref: best.h });
            for (const m of this.members.values())
                if (m.connId && m.streaming) this._tx(m, note);
            for (const [h, mids] of groups)
                if (h !== best.h)
                    for (const mid of mids) {
                        const m = this.members.get(mid);
                        if (m) this._mismatch(m, f);
                    }
        }
        for (const k of this.checks.keys())
            if (k < f - 10 * this.params.hashEvery) this.checks.delete(k);
    }

    _mismatch(m, f) {
        m.cleanStreak = 0;
        this._startSync(m, 'mismatch');
    }

    // --- world sync -------------------------------------------------------------

    _donorFor(target) {
        const ref = this.reference;
        let cands = this._seated().concat([...this.members.values()].filter((x) => x.seat < 0))
            .filter((x) => x !== target && x.connId && x.caps && x.caps.sync
                    && x.role !== 'pending' && !x.syncBusy);
        if (ref)
            cands = cands.filter((x) => x.hashes.get(ref.f) === ref.h);
        else
            cands = [];
        return electDonor(cands, this.params.donorPolicy, this.hostMid);
    }

    _startSync(m, reason) {
        const t = this.now();
        if (!m.connId || !m.caps || !m.caps.sync || this.phase !== 'playing')
            return false;
        if (m.syncBusy)
            return false;
        if (reason !== 'join' && reason !== 'ask' && t - m.lastSyncAt < this.params.minSyncGapMs) {
            m.needsSync = m.needsSync || reason;
            return false;
        }
        const donor = this._donorFor(m);
        if (!donor) {
            m.needsSync = m.needsSync || reason;
            return false;
        }
        // The freshest checkpoint the donor holds: its own state there is
        // what it sends (it was clean at the reference).
        const f = donor.lastHashFrame;
        const id = this.nextSyncId++;
        const s = { id, f, target: m.mid, donor: donor.mid, reason, startedAt: t,
                    state: 'have', bytes: 0, chunks: 0, count: 0 };
        this.syncs.set(id, s);
        m.syncBusy = id;
        donor.syncBusy = id;
        m.needsSync = null;
        m.lastSyncAt = t;
        this._json(m, { t: 'sync_req', id, f, donor: donor.mid, reason });
        return true;
    }

    _endSync(s, ok, why) {
        this.syncs.delete(s.id);
        const tg = this.members.get(s.target), dn = this.members.get(s.donor);
        if (tg && tg.syncBusy === s.id) tg.syncBusy = 0;
        if (dn && dn.syncBusy === s.id) dn.syncBusy = 0;
        if (ok) {
            this.stats.syncs++;
            this.stats.syncBytes += s.bytes;
            if (tg) tg.lastSyncAt = this.now();
        } else {
            this.stats.syncFails++;
            for (const x of [tg, dn])
                if (x) this._json(x, { t: 'sync_cancel', id: s.id, why });
            if (tg && s.reason !== 'periodic') tg.needsSync = s.reason;
        }
    }

    _abortSyncsOf(m) {
        for (const s of [...this.syncs.values()])
            if (s.target === m.mid || s.donor === m.mid)
                this._endSync(s, false, 'peer left');
    }

    _syncMsg(m, msg) {
        const s = this.syncs.get(msg.id >>> 0);
        if (!s)
            return;
        if (msg.t === 'sync_have' && m.mid === s.target && s.state === 'have') {
            const dn = this.members.get(s.donor);
            if (!dn) return this._endSync(s, false, 'donor gone');
            s.state = 'snap';
            const req = { t: 'snap_req', id: s.id, f: s.f, to: m.mid, reason: s.reason };
            if (typeof msg.ps === 'number' && typeof msg.hashes === 'string' && msg.hashes.length < 200000) {
                req.ps = msg.ps | 0;
                req.hashes = msg.hashes;
            }
            if (typeof msg.base === 'number') req.base = msg.base >>> 0;
            this._json(dn, req);
        } else if (msg.t === 'snap' && m.mid === s.donor && s.state === 'snap') {
            const count = msg.count | 0;
            const size = msg.size >>> 0;
            if (count < 1 || count > 4096 || size > count * CHUNK_BYTES)
                return this._endSync(s, false, 'bad snapshot header');
            s.state = 'chunks';
            s.count = count;
            const tg = this.members.get(s.target);
            if (!tg) return this._endSync(s, false, 'target gone');
            this._json(tg, { t: 'snap', id: s.id, f: s.f, size, count, h: String(msg.h || ''),
                             kind: String(msg.kind || ''), from: m.mid, reason: s.reason });
        } else if (msg.t === 'snap_fail' && m.mid === s.donor) {
            this._endSync(s, false, String(msg.why || 'donor failed'));
        } else if (msg.t === 'sync_done' && m.mid === s.target) {
            this._endSync(s, !!msg.ok, String(msg.why || 'apply failed'));
        }
    }

    _chunk(m, ch, raw) {
        const s = this.syncs.get(ch.id);
        if (!s || s.donor !== m.mid || s.state !== 'chunks' || ch.count !== s.count)
            return;
        const tg = this.members.get(s.target);
        if (!tg || !tg.connId)
            return this._endSync(s, false, 'target gone');
        s.bytes += raw.length;
        s.chunks++;
        this._tx(tg, raw);
        if (s.chunks >= s.count)
            s.state = 'applying';
    }

    // --- timers -----------------------------------------------------------------

    _housekeeping(t) {
        const p = this.params;
        // Silent seats: Kirby to the AI after dropToAiMs, seat freed after
        // the reconnect grace.
        for (const m of [...this.members.values()]) {
            if (!m.connId && m.leftAt) {
                if (m.seat >= 0 && m.seatLive && t - m.leftAt >= p.dropToAiMs)
                    this._sealSeat(m), this.dirty = true;
                if (t - m.leftAt >= p.reconnectGraceMs) {
                    if (this.phase === 'playing' && m.seatLive) this._sealSeat(m);
                    this._freeSeat(m);
                    this._forget(m);
                    this.dirty = true;
                }
            }
        }
        // Host away too long: the longest-seated connected member takes over.
        const host = this.members.get(this.hostMid);
        if ((!host || (!host.connId && this.hostAwaySince && t - this.hostAwaySince >= p.hostGraceMs))) {
            const next = [...this.members.values()].filter((x) => x.connId && x.role !== 'pending')
                .sort((a, b) => (a.seatedAt ?? 1e15) - (b.seatedAt ?? 1e15) || a.joinedAt - b.joinedAt)[0];
            if (next && next.mid !== this.hostMid) {
                this.hostMid = next.mid;
                this.hostAwaySince = 0;
                this.dirty = true;
            }
        }
        // Checkpoints nobody completed: judge on what arrived.
        for (const [f, c] of this.checks)
            if (!c.done && t - c.at >= p.hashWaitMs)
                this._judge(f);
        // Syncs that stalled.
        for (const s of [...this.syncs.values()])
            if (t - s.startedAt >= p.syncTimeoutMs)
                this._endSync(s, false, 'timeout');
        // Sync triggers: retry, lag, join, background.
        if (this.phase === 'playing' && this.reference) {
            for (const m of this.members.values()) {
                if (!m.connId || !m.caps || !m.caps.sync || m.syncBusy || m.role === 'pending')
                    continue;
                const watching = m.seat < 0 || !m.seatLive;
                if (m.needsSync === 'join' && t - m.sitAt < p.joinSyncDelayMs && m.lastHashFrame < this.reference.f)
                    continue;
                if (m.needsSync) {
                    this._startSync(m, m.needsSync);
                    continue;
                }
                if (!watching && (m.lastHashFrame || 0) + p.lagSyncFrames < this.reference.f
                    && t - (m.sitAt || 0) >= p.joinSyncDelayMs) {
                    this._startSync(m, 'lag');
                    continue;
                }
                // Background sync: never of the peer that would itself be
                // the donor -- the authority stays put unless a hash moves it.
                if (p.syncEveryMs > 0 && t - Math.max(m.lastSyncAt, m.sitAt || m.joinedAt) >= p.syncEveryMs) {
                    const self = this._donorFor(null);
                    if (self && self !== m)
                        this._startSync(m, 'periodic');
                    else
                        m.lastSyncAt = t;
                }
            }
        }
        // Per-peer stats, once a second.
        if (!this._statsTick || t - this._statsTick >= 1000) {
            this._statsTick = t;
            // `ahead`: how far this seat's newest input leads the slowest
            // other live seat's -- what a peer's rate dilation steers by
            // (it holds a frame now and then; the slow one fast-forwards).
            const live = this._seated().filter((x) => x.seatLive && x.connId && x.latest
                                                    && x.lateStreak < p.excuseAfter);
            for (const m of this.members.values()) {
                if (!m.connId || !m.streaming)
                    continue;
                let ahead = null;
                if (m.latest && m.seatLive) {
                    const others = live.filter((x) => x !== m).map((x) => x.latest.frame);
                    if (others.length) ahead = m.latest.frame - Math.min(...others);
                }
                this._json(m, {
                    t: 'stats', frontier: this.frontier, head: this.head, wm: this.wm,
                    clock: this.clock ? +this.clockFrame(t).toFixed(1) : null,
                    late: m.lateInputs, fills: m.fills, ahead,
                    sealed: this.stats.sealed, deadlineSeals: this.stats.deadlineSeals,
                    behind: m.latest ? this.frontier - 1 - m.latest.frame : null,
                });
            }
        }
        this._flushRoster();
    }

    // --- persistence (a resumable world) --------------------------------------

    exportState() {
        return {
            v: 1, phase: this.phase, params: this.params, version: this.version,
            frontier: this.frontier, vals: [...this.vals], archive: this.archive,
            events: this.events, seq: this.seq, head: this.head, wm: this.wm,
            kirby: this.kirby, banned: [...this.banned], code: this.code,
        };
    }

    importState(s) {
        if (!s || s.v !== 1)
            return;
        this.params = { ...DEFAULT_PARAMS, ...s.params };
        this._resetSequencer();
        this.phase = s.phase;
        this.version = s.version;
        this.frontier = s.frontier;
        this.vals = Uint16Array.from(s.vals);
        this.outBase = this.vals.slice();
        this.outFirst = this.frontier;
        this.archive = s.archive;
        this.events = s.events;
        this.seq = s.seq;
        this.head = s.head;
        this.wm = s.wm;
        this.banned = new Set(s.banned || []);
        // Every column is sealed on resume; the old seats are AI-driven
        // until their players sit again.
        this.kirby = s.kirby.map(() => -1);
        const E = this._eventFrame();
        for (let k = 0; k < s.kirby.length; k++)
            if (s.kirby[k] >= 0) this._event(E, 'slot', { slot: k, seat: -1 }, true);
    }
}

export function newIdentity(rng = cryptoRng) { return randomToken(rng, 16); }
