// The HTTP face of the code directory: create / look up / resume, with the
// edge rate limits.  Transport-free -- each method returns {status, body,
// headers?} -- so the Cloudflare Directory DO (server.mjs) and the node dev
// relay (dev-relay.mjs) answer identically.
//
//   POST /v2/rooms                  -> 201 {code, resume, ws}
//   GET  /v2/rooms/<CODE>           -> 200 {code, occupants} | 404 | 429
//   POST /v2/resume  {token}        -> 200 {code, resume, ws} | 404
//   GET  /v2/rooms/<CODE>/ws        -> WebSocket upgrade into the room
//                                      (the host resolves the code with
//                                      lookup() and forwards; same limits)
//
// The public game list (listing.mjs), opt-in per room:
//   GET  /v2/public                 -> 200 {v, games, every, ttl} | 429
//   POST /v2/public  {code, resume, name, n, mode, locked}
//                                   -> 200 {ok, listed} | 400 | 403 | 409 | 429
//   POST /v2/public/remove {code, resume} -> 200 {ok, removed} | 403 | 429

import { CodeDirectory, makeEdgeLimits, normalizeCode } from './codes.mjs';
import { ListService } from './listing.mjs';

export class DirectoryService {
    constructor(opts = {}) {
        this.now = opts.now || (() => Date.now());
        this.dir = opts.directory || new CodeDirectory({ now: this.now, rng: opts.rng,
                                                         idleMs: opts.idleMs });
        this.limits = makeEdgeLimits(this.now);
        this.pub = new ListService(this, opts.list || {});
    }

    _limited(kind, ip, cost = 1) {
        const l = this.limits[kind];
        if (l.take(ip || '?', cost))
            return null;
        const ra = Math.max(1, l.retryAfter(ip || '?', cost));
        return { status: 429, headers: { 'Retry-After': String(ra) },
                 body: { error: 'rate', msg: `Too many attempts. Try again in ${ra} s.`, retryAfter: ra } };
    }

    create(ip) {
        const lim = this._limited('create', ip);
        if (lim) return lim;
        this.dir.sweep();
        const a = this.dir.allocate();
        if (!a)
            return { status: 503, body: { error: 'full', msg: 'No free room codes right now.' } };
        return { status: 201, body: { code: a.code, resume: a.resumeToken }, roomId: a.roomId };
    }

    // -> {status, body, roomId?}.  Unknown codes cost the miss bucket too:
    // that bucket is what bounds guessing.
    lookup(code, ip) {
        const lim = this._limited('lookup', ip);
        if (lim) return lim;
        const c = normalizeCode(code);
        const e = c && this.dir.resolve(c);
        if (!e) {
            const miss = this._limited('miss', ip);
            if (miss) return miss;
            return { status: 404, body: { error: 'unknown', msg: 'No game with that code. Check the letters, or ask the host for a new code.' } };
        }
        return { status: 200, body: { code: e.code, occupants: e.occupants }, roomId: e.roomId };
    }

    resume(token, ip) {
        const lim = this._limited('lookup', ip);
        if (lim) return lim;
        const r = typeof token === 'string' && token.length >= 32 ? this.dir.resumeRoom(token) : null;
        if (!r) {
            const miss = this._limited('miss', ip);
            if (miss) return miss;
            return { status: 404, body: { error: 'unknown', msg: 'That saved world has expired.' } };
        }
        return { status: 200, body: { code: r.code, resume: r.resumeToken }, roomId: r.roomId };
    }

    touch(roomId, occupants) {
        this.dir.touch(roomId, occupants);
        this.pub.occupancy(roomId, occupants);
    }

    // The public list: in memory only, never part of toJSON / storage.
    publicList(ip) { return this.pub.get(ip); }
    publish(body, ip) { return this.pub.publish(body, ip); }
    unpublish(body, ip) { return this.pub.remove(body, ip); }
}

export const CORS = {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type',
};
