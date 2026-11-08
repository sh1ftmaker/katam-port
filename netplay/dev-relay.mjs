// The local relay: the same room the Cloudflare worker runs, on a plain node
// WebSocket server.  For development and for the headless two-instance tests
// -- nothing about Milestone A should require a Cloudflare account, and a
// transport bug is a lot easier to look at when the server is forty lines in
// the same process as the test.
//
//   node netplay/dev-relay.mjs [port]          one room per URL path
//
// or from a test:
//
//   import { startRelay } from './dev-relay.mjs';
//   const relay = await startRelay(0);          // port 0: pick a free one
//   ... ws://127.0.0.1:${relay.port}/anything ...
//   relay.close();
//
// The URL path names the room, so `/kirby` and `/dedede` are different
// cables.  A client may pass `?id=<stable id>` to survive a reconnect with
// its slot intact, the same convention partysocket's `_pk` gives the
// Cloudflare room.

import http from 'http';
import { WebSocketServer } from 'ws';
import { SessionCore } from './session.mjs';
import { DirectoryService, CORS } from './directory.mjs';
import {
    RoomCore, tagWords, validClientWords, joinedMsg, peerMsg, errorMsg,
    validClientInput, validClientPayload, decodeTaggedInput,
    encodeLogBatch, LOG_BATCH,
} from './protocol.mjs';

export function startRelay(port = 8787, opts = {}) {
    const log = opts.quiet ? () => {} : console.error.bind(console);
    const rooms = new Map();        /* path -> { core, conns: Map<id, ws> } */

    function room(path) {
        if (!rooms.has(path))
            rooms.set(path, {
                core: new RoomCore(), conns: new Map(),
                /* Path B: everything a late joiner replays.  inputs is the
                 * tagged binary records; assigns the seat-change control
                 * messages; latest the highest frame seen. */
                inputs: [], assigns: [], latest: 0,
            });
        return rooms.get(path);
    }

    const wss = new WebSocketServer({ noServer: true });
    const v2 = startV2(opts, log);
    const server = http.createServer((req, res) => v2.http(req, res));
    server.on('upgrade', (req, sock, head) => {
        if (req.url.startsWith('/v2/'))
            return v2.upgrade(req, sock, head);
        wss.handleUpgrade(req, sock, head, (ws) => wss.emit('connection', ws, req));
    });

    wss.on('connection', (ws, req) => {
        const url = new URL(req.url, 'http://relay');
        const r = room(url.pathname);
        const id = url.searchParams.get('id') || `anon-${Math.random()}`;

        const slot = r.core.join(id);
        if (slot === -1) {
            ws.send(errorMsg('room full'));
            ws.close(4000, 'room full');
            return;
        }
        /* A reconnect with the same id replaces the dead socket. */
        const old = r.conns.get(id);
        if (old && old !== ws)
            old.close(4001, 'replaced');
        r.conns.set(id, ws);

        log(`[relay] ${url.pathname}: slot ${slot} joined (${r.core.occupied()} in room)`);
        ws.send(joinedMsg(slot, r.core.online()));
        for (const [oid, other] of r.conns)
            if (oid !== id)
                other.send(peerMsg(slot, true));

        ws.on('message', (data, isBinary) => {
            if (!isBinary) {
                let m;
                try { m = JSON.parse(data.toString()); } catch (e) { return; }
                if (m.type === 'history') {
                    /* Everything the room has seen, then the frame to catch
                     * up to.  Assigns first: the joiner schedules them
                     * before it replays through them. */
                    for (const a of r.assigns)
                        ws.send(JSON.stringify(a));
                    for (let i = 0; i < r.inputs.length; i += LOG_BATCH)
                        ws.send(encodeLogBatch(r.inputs.slice(i, i + LOG_BATCH)));
                    ws.send(JSON.stringify({ type: 'history-done',
                                             latest: r.latest }));
                } else if (m.type === 'assign'
                           && typeof m.frame === 'number'
                           && typeof m.slot === 'number'
                           && typeof m.peer === 'number') {
                    const a = { type: 'assign', frame: m.frame,
                                slot: m.slot, peer: m.peer };
                    /* Seals ride along verbatim -- see server.mjs. */
                    if (m.seal && typeof m.seal.player === 'number'
                        && typeof m.seal.from === 'number'
                        && typeof m.seal.keys === 'number')
                        a.seal = { player: m.seal.player, from: m.seal.from,
                                   keys: m.seal.keys };
                    r.assigns.push(a);
                    log(`[relay] assign: slot ${a.slot} -> ` +
                        `${a.peer < 0 ? 'AI' : 'peer ' + a.peer} at frame ${a.frame}`);
                    for (const other of r.conns.values())
                        if (other.readyState === other.OPEN)
                            other.send(JSON.stringify(a));   /* sender too */
                }
                return;
            }
            const bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
            if (validClientWords(bytes) || validClientPayload(bytes)) {
                const tagged = tagWords(slot, bytes);
                for (const [oid, other] of r.conns)
                    if (oid !== id && other.readyState === other.OPEN)
                        other.send(tagged);
                return;
            }
            if (validClientInput(bytes)) {
                const tagged = tagWords(slot, bytes);
                const rec = decodeTaggedInput(tagged);
                r.inputs.push(rec);
                if (rec.frame > r.latest)
                    r.latest = rec.frame;
                for (const [oid, other] of r.conns)
                    if (oid !== id && other.readyState === other.OPEN)
                        other.send(tagged);
                return;
            }
            ws.send(errorMsg('malformed'));
            ws.close(4002, 'malformed');
        });

        ws.on('close', () => {
            if (r.conns.get(id) !== ws)
                return;                         /* replaced by a reconnect    */
            r.conns.delete(id);
            r.core.leave(id);
            log(`[relay] ${url.pathname}: slot ${slot} left (${r.core.occupied()} in room)`);
            for (const other of r.conns.values())
                other.send(peerMsg(slot, false));
            if (r.core.occupied() === 0)
                rooms.delete(url.pathname);
        });
    });

    return new Promise((resolve, reject) => {
        server.on('error', reject);
        server.listen(port, opts.host || '127.0.0.1', () => {
            const actual = server.address().port;
            log(`[relay] listening on ws://127.0.0.1:${actual} (v1 rooms by path, v2 under /v2/)`);
            resolve({
                port: actual, v2,
                close: () => { v2.close(); wss.close(); server.close();
                               for (const c of wss.clients) c.terminate(); },
            });
        });
    });
}

// --- protocol v2: the directory and SessionCore rooms ------------------------
//
// The same HTTP surface the Cloudflare worker serves (netplay/directory.mjs):
//   POST /v2/rooms, GET /v2/rooms/<CODE>, POST /v2/resume, and the
//   WebSocket at /v2/rooms/<CODE>/ws.
// opts.ipHeader: trust X-Forwarded-For (tests fake many IPs with it).
export function startV2(opts = {}, log = () => {}) {
    const service = opts.directory || new DirectoryService({ idleMs: opts.idleMs });
    const rooms = new Map();          /* roomId -> {core, socks: Map, timer} */
    const wss = new WebSocketServer({ noServer: true, maxPayload: 256 * 1024 });
    let nextConn = 1;

    function ipOf(req) {
        if (opts.ipHeader && req.headers['x-forwarded-for'])
            return String(req.headers['x-forwarded-for']).split(',')[0].trim();
        return req.socket.remoteAddress || '?';
    }

    function room(roomId, code) {
        let r = rooms.get(roomId);
        if (r)
            return r;
        r = { socks: new Map(), timer: null, wakeAt: Infinity };
        r.core = new SessionCore({
            roomId, code,
            params: opts.params,
            send: (id, data) => {
                const ws = r.socks.get(id);
                if (ws && ws.readyState === 1)
                    ws.send(data, { binary: typeof data !== 'string' });
            },
            close: (id, c, why) => {
                const ws = r.socks.get(id);
                if (ws) try { ws.close(c, why); } catch (e) { /* gone */ }
            },
            onOccupancy: (n) => service.touch(roomId, n),
            log,
        });
        rooms.set(roomId, r);
        return r;
    }

    function schedule(r) {
        const ms = r.core.nextWakeIn();
        if (ms === null)
            return;
        const at = Date.now() + ms;
        if (r.timer && at >= r.wakeAt - 1)
            return;
        clearTimeout(r.timer);
        r.wakeAt = at;
        r.timer = setTimeout(() => {
            r.timer = null;
            r.wakeAt = Infinity;
            r.core.tick();
            schedule(r);
        }, ms);
    }

    function reply(res, out) {
        res.writeHead(out.status, { 'Content-Type': 'application/json', ...CORS,
                                    ...(out.headers || {}) });
        res.end(JSON.stringify(out.body));
    }

    function http_(req, res) {
        const url = new URL(req.url, 'http://relay');
        if (req.method === 'OPTIONS') {
            res.writeHead(204, CORS);
            return res.end();
        }
        const ip = ipOf(req);
        if (req.method === 'POST' && url.pathname === '/v2/rooms') {
            const out = service.create(ip);
            if (out.roomId) room(out.roomId, out.body.code);
            return reply(res, out);
        }
        let m = /^\/v2\/rooms\/([^/]+)$/.exec(url.pathname);
        if (req.method === 'GET' && m)
            return reply(res, service.lookup(decodeURIComponent(m[1]), ip));
        if (req.method === 'POST' && url.pathname === '/v2/resume') {
            let body = '';
            req.on('data', (d) => { body += d; if (body.length > 4096) req.destroy(); });
            req.on('end', () => {
                let tok = '';
                try { tok = JSON.parse(body).token; } catch (e) { /* empty */ }
                reply(res, service.resume(tok, ip));
            });
            return;
        }
        if (url.pathname.startsWith('/v2/')) {
            res.writeHead(404, CORS);
            return res.end();
        }
        res.writeHead(200, { 'Content-Type': 'text/plain', ...CORS });
        res.end('katam-port netplay dev relay');
    }

    function upgrade(req, sock, head) {
        const url = new URL(req.url, 'http://relay');
        const m = /^\/v2\/rooms\/([^/]+)\/ws$/.exec(url.pathname);
        const ip = ipOf(req);
        const out = m ? service.lookup(decodeURIComponent(m[1]), ip) : { status: 404 };
        if (out.status !== 200) {
            sock.write(`HTTP/1.1 ${out.status} ${out.status === 429 ? 'Too Many Requests' : 'Not Found'}\r\n` +
                       'Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n');
            return sock.destroy();
        }
        const r = room(out.roomId, out.body.code);
        wss.handleUpgrade(req, sock, head, (ws) => {
            const id = 'c' + nextConn++;
            r.socks.set(id, ws);
            r.core.open(id, { ip });
            ws.on('message', (data, isBinary) => {
                r.core.message(id, isBinary
                    ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
                    : data.toString());
                schedule(r);
            });
            ws.on('close', () => {
                r.socks.delete(id);
                r.core.closed(id);
                schedule(r);
            });
            schedule(r);
        });
    }

    return {
        http: http_, upgrade, rooms, service,
        close() {
            for (const r of rooms.values()) {
                clearTimeout(r.timer);
                for (const ws of r.socks.values()) ws.terminate();
            }
            wss.close();
        },
    };
}

/* CLI */
if (import.meta.url === `file://${process.argv[1]}`)
    startRelay(parseInt(process.argv[2] || '8787', 10),
               { host: process.env.RELAY_HOST || '127.0.0.1' });
