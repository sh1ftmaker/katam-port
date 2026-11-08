// The Cloudflare room: partyserver over a Durable Object, one instance per
// room name, deployed with `wrangler deploy` to the account's own workers --
// there is no PartyKit platform in this (docs/NETPLAY.md §1 says why).
//
// This file is deliberately the same forty lines as dev-relay.mjs wearing a
// different socket API.  Everything with a decision in it lives in
// protocol.mjs; if the two servers can disagree about behaviour, the bug is
// that something decided here.
//
// Hibernation is on: an idle room costs nothing, sockets survive the DO
// being evicted, and the slot map is rebuilt in onStart from the connection
// state that partyserver persists per socket.  A room at 60 messages a
// second never hibernates mid-game.
//
// Path B will add MSG_INPUT relay and the append-only log in ctx.storage;
// the message space in protocol.mjs already reserves the types.

import { Server, routePartykitRequest } from 'partyserver';
import { SessionCore } from './session.mjs';
import { DirectoryService, CORS } from './directory.mjs';
import {
    RoomCore, tagWords, validClientWords, joinedMsg, peerMsg, errorMsg,
    validClientInput, validClientPayload, decodeTaggedInput,
    encodeLogBatch, LOG_BATCH,
} from './protocol.mjs';

export class GameRoom extends Server {
    static options = { hibernate: true };

    onStart() {
        this.core = new RoomCore();
        for (const conn of this.getConnections()) {
            const s = conn.state;
            if (s && typeof s.slot === 'number')
                this.core.slots[s.slot] = conn.id;
        }
        /* Path B history, in memory.  An active room never hibernates (60
         * messages a second resets the idle clock), so this survives any
         * session that is actually being played; a room that hibernated has
         * no session left to rejoin.  Durable storage for it is future work
         * and protocol.mjs reserves nothing about it. */
        this.inputs = [];
        this.assigns = [];
        this.latest = 0;
    }

    onConnect(conn) {
        /* A reconnect with the same id replaces the dead socket -- which may
         * still look open here if the client noticed the death first.  Same
         * rule as dev-relay.mjs. */
        for (const other of this.getConnections())
            if (other !== conn && other.id === conn.id) {
                try { other.close(4001, 'replaced'); } catch (e) { /* gone */ }
            }
        const slot = this.core.join(conn.id);
        if (slot === -1) {
            conn.send(errorMsg('room full'));
            conn.close(4000, 'room full');
            return;
        }
        conn.setState({ slot });
        conn.send(joinedMsg(slot, this.core.online()));
        this.broadcast(peerMsg(slot, true), [conn.id]);
    }

    onMessage(conn, message) {
        const s = conn.state;
        if (!s || typeof s.slot !== 'number')
            return;
        if (typeof message === 'string') {
            let m;
            try { m = JSON.parse(message); } catch (e) { return; }
            if (m.type === 'history') {
                for (const a of this.assigns)
                    conn.send(JSON.stringify(a));
                for (let i = 0; i < this.inputs.length; i += LOG_BATCH)
                    conn.send(encodeLogBatch(this.inputs.slice(i, i + LOG_BATCH)));
                conn.send(JSON.stringify({ type: 'history-done',
                                           latest: this.latest }));
            } else if (m.type === 'assign'
                       && typeof m.frame === 'number'
                       && typeof m.slot === 'number'
                       && typeof m.peer === 'number') {
                const a = { type: 'assign', frame: m.frame,
                            slot: m.slot, peer: m.peer };
                /* The seal (a departed peer's stream-ending decree) rides
                 * along, stored and relayed verbatim: late joiners need it
                 * as much as the survivors do. */
                if (m.seal && typeof m.seal.player === 'number'
                    && typeof m.seal.from === 'number'
                    && typeof m.seal.keys === 'number')
                    a.seal = { player: m.seal.player, from: m.seal.from,
                               keys: m.seal.keys };
                this.assigns.push(a);
                this.broadcast(JSON.stringify(a));           /* sender too */
            }
            return;
        }
        const bytes = new Uint8Array(message);
        if (validClientWords(bytes) || validClientPayload(bytes)) {
            this.broadcast(tagWords(s.slot, bytes), [conn.id]);
            return;
        }
        if (validClientInput(bytes)) {
            const tagged = tagWords(s.slot, bytes);
            const rec = decodeTaggedInput(tagged);
            this.inputs.push(rec);
            if (rec.frame > this.latest)
                this.latest = rec.frame;
            this.broadcast(tagged, [conn.id]);
            return;
        }
        conn.send(errorMsg('malformed'));
        conn.close(4002, 'malformed');
    }

    onClose(conn) {
        const s = conn.state;
        if (!s || typeof s.slot !== 'number')
            return;
        /* The id-equality below cannot tell a replaced socket from the last
         * one: a fast reconnect has two connections with one id, and the
         * old one closing must not free the slot the new one holds -- or
         * announce a leave nobody made. */
        for (const other of this.getConnections())
            if (other !== conn && other.id === conn.id)
                return;
        if (this.core.slots[s.slot] === conn.id) {
            this.core.leave(conn.id);
            this.broadcast(peerMsg(s.slot, false));
        }
        /* An emptied room's session is over -- dev-relay deletes the whole
         * room here.  Without this, the history outlives its players (until
         * the DO happens to be evicted), and the next group to reuse the
         * room name replays a ghost session and finds no host among
         * themselves. */
        if (this.core.occupied() === 0) {
            this.inputs = [];
            this.assigns = [];
            this.latest = 0;
        }
    }
}

// --- protocol v2 --------------------------------------------------------------
//
// Two more Durable Object classes; the v1 GameRoom above is untouched so
// pages already in the wild keep working.
//
//   Directory    one global instance (idFromName('global')): room codes,
//                resume tokens, the edge rate limits.  netplay/directory.mjs.
//   SessionRoom  one per room id (128-bit random, never shown to players):
//                a SessionCore (netplay/session.mjs) on plain WebSockets.
//
// SessionRoom deliberately does NOT use the hibernation API: SessionCore is
// in-memory state with a deadline timer, and a hibernated room would wake
// with an empty sequencer.  A connected room stays resident (billed by
// duration while sockets are open -- the design's ~$0.0065/h per 4-player
// room); an empty room persists its archive (exportState) and is evicted.

function json(status, body, headers = {}) {
    return new Response(JSON.stringify(body), {
        status, headers: { 'Content-Type': 'application/json', ...CORS, ...headers } });
}

function clientIp(req) {
    return req.headers.get('CF-Connecting-IP') || req.headers.get('X-Client-IP') || '?';
}

export class Directory {
    constructor(ctx, env) {
        this.ctx = ctx;
        this.env = env;
        this.svc = new DirectoryService({});
        this.saveTimer = null;
        ctx.blockConcurrencyWhile(async () => {
            const j = await ctx.storage.get('dir');
            if (j) this.svc.dir.load(j);
        });
    }

    _save() {
        if (this.saveTimer) return;
        this.saveTimer = setTimeout(() => {
            this.saveTimer = null;
            this.svc.dir.sweep();
            this.ctx.storage.put('dir', this.svc.dir.toJSON());
        }, 1000);
    }

    async fetch(req) {
        const url = new URL(req.url);
        const ip = req.headers.get('X-Client-IP') || '?';
        let out;
        if (url.pathname === '/create') {
            out = this.svc.create(ip);
        } else if (url.pathname === '/lookup') {
            out = this.svc.lookup(url.searchParams.get('code') || '', ip);
        } else if (url.pathname === '/resume') {
            out = this.svc.resume(url.searchParams.get('token') || '', ip);
        } else if (url.pathname === '/touch') {
            this.svc.touch(url.searchParams.get('room') || '', Number(url.searchParams.get('n')) | 0);
            this._save();
            return json(200, { ok: true });
        } else {
            return json(404, { error: 'unknown' });
        }
        if (out.status < 300) this._save();
        return json(out.status, { ...out.body, roomId: out.roomId }, out.headers);
    }
}

export class SessionRoom {
    constructor(ctx, env) {
        this.ctx = ctx;
        this.env = env;
        this.socks = new Map();
        this.nextConn = 1;
        this.timer = null;
        this.wakeAt = Infinity;
        this.core = null;
        this.lastSave = 0;
    }

    async _ensure(roomId, code) {
        if (this.core) {
            if (code && this.core.code !== code) this.core.code = code;
            return;
        }
        this.roomId = roomId;
        this.core = new SessionCore({
            roomId, code,
            send: (id, data) => {
                const ws = this.socks.get(id);
                if (ws) try { ws.send(data); } catch (e) { /* closing */ }
            },
            close: (id, c, why) => {
                const ws = this.socks.get(id);
                if (ws) try { ws.close(c, why); } catch (e) { /* closed */ }
            },
            onOccupancy: (n) => this._occupancy(n),
        });
        const saved = await this.ctx.storage.get('state');
        if (saved) this.core.importState(saved);
    }

    _occupancy(n) {
        const dir = this.env.Directory.get(this.env.Directory.idFromName('global'));
        this.ctx.waitUntil(dir.fetch(`https://dir/touch?room=${this.roomId}&n=${n}`));
        if (n === 0 || Date.now() - this.lastSave > 30000) this._save();
    }

    _save() {
        if (!this.core || this.core.phase !== 'playing') return;
        this.lastSave = Date.now();
        this.ctx.waitUntil(this.ctx.storage.put('state', this.core.exportState()));
    }

    _schedule() {
        const ms = this.core.nextWakeIn();
        if (ms === null) return;
        const at = Date.now() + ms;
        if (this.timer && at >= this.wakeAt - 1) return;
        clearTimeout(this.timer);
        this.wakeAt = at;
        this.timer = setTimeout(() => {
            this.timer = null;
            this.wakeAt = Infinity;
            this.core.tick();
            this._schedule();
        }, ms);
    }

    async fetch(req) {
        if (req.headers.get('Upgrade') !== 'websocket')
            return json(426, { error: 'websocket only' });
        await this._ensure(req.headers.get('X-Room-Id'), req.headers.get('X-Room-Code'));
        const pair = new WebSocketPair();
        const ws = pair[1];
        ws.accept();
        const id = 'c' + this.nextConn++;
        this.socks.set(id, ws);
        this.core.open(id, { ip: req.headers.get('X-Client-IP') || '?' });
        ws.addEventListener('message', (e) => {
            this.core.message(id, typeof e.data === 'string' ? e.data : new Uint8Array(e.data));
            this._schedule();
        });
        const gone = () => {
            if (!this.socks.has(id)) return;
            this.socks.delete(id);
            this.core.closed(id);
            this._schedule();
        };
        ws.addEventListener('close', gone);
        ws.addEventListener('error', gone);
        this._schedule();
        return new Response(null, { status: 101, webSocket: pair[0] });
    }
}

async function v2(req, env) {
    const url = new URL(req.url);
    if (req.method === 'OPTIONS')
        return new Response(null, { status: 204, headers: CORS });
    const ip = clientIp(req);
    const dir = env.Directory.get(env.Directory.idFromName('global'));
    const ask = (path) => dir.fetch(`https://dir${path}`, { headers: { 'X-Client-IP': ip } });
    const strip = async (r) => {
        const j = await r.json();
        delete j.roomId;                     // never leaves the worker
        return json(r.status, j, r.headers.get('Retry-After')
            ? { 'Retry-After': r.headers.get('Retry-After') } : {});
    };
    if (req.method === 'POST' && url.pathname === '/v2/rooms')
        return strip(await ask('/create'));
    if (req.method === 'POST' && url.pathname === '/v2/resume') {
        let tok = '';
        try { tok = (await req.json()).token || ''; } catch (e) { /* empty */ }
        return strip(await ask('/resume?token=' + encodeURIComponent(tok)));
    }
    let m = /^\/v2\/rooms\/([^/]+)$/.exec(url.pathname);
    if (req.method === 'GET' && m)
        return strip(await ask('/lookup?code=' + encodeURIComponent(decodeURIComponent(m[1]))));
    m = /^\/v2\/rooms\/([^/]+)\/ws$/.exec(url.pathname);
    if (m) {
        if (req.headers.get('Upgrade') !== 'websocket')
            return json(426, { error: 'websocket only' });
        const r = await ask('/lookup?code=' + encodeURIComponent(decodeURIComponent(m[1])));
        const j = await r.json();
        if (r.status !== 200)
            return json(r.status, { error: j.error, msg: j.msg });
        const room = env.SessionRoom.get(env.SessionRoom.idFromName(j.roomId));
        const h = new Headers(req.headers);
        h.set('X-Room-Id', j.roomId);
        h.set('X-Room-Code', j.code);
        h.set('X-Client-IP', ip);
        return room.fetch(new Request(req.url, { headers: h }));
    }
    return json(404, { error: 'unknown' });
}

export default {
    fetch: (req, env) => {
        if (new URL(req.url).pathname.startsWith('/v2/'))
            return v2(req, env);
        return routePartykitRequest(req, env).then(
            (r) => r ?? new Response('katam-port netplay relay', { status: 200, headers: CORS }));
    },
};
