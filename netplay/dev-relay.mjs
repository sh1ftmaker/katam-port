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

import { WebSocketServer } from 'ws';
import {
    RoomCore, tagWords, validClientWords, joinedMsg, peerMsg, errorMsg,
} from './protocol.mjs';

export function startRelay(port = 8787, opts = {}) {
    const log = opts.quiet ? () => {} : console.error.bind(console);
    const rooms = new Map();        /* path -> { core, conns: Map<id, ws> } */

    function room(path) {
        if (!rooms.has(path))
            rooms.set(path, { core: new RoomCore(), conns: new Map() });
        return rooms.get(path);
    }

    const wss = new WebSocketServer({ port, host: '127.0.0.1' });

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
            if (!isBinary) return;              /* clients send no control    */
            const bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
            if (!validClientWords(bytes)) {
                ws.send(errorMsg('malformed'));
                ws.close(4002, 'malformed');
                return;
            }
            const tagged = tagWords(slot, bytes);
            for (const [oid, other] of r.conns)
                if (oid !== id && other.readyState === other.OPEN)
                    other.send(tagged);
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
        wss.on('error', reject);
        wss.on('listening', () => {
            const actual = wss.address().port;
            log(`[relay] listening on ws://127.0.0.1:${actual}`);
            resolve({ port: actual, close: () => wss.close() });
        });
    });
}

/* CLI */
if (import.meta.url === `file://${process.argv[1]}`)
    startRelay(parseInt(process.argv[2] || '8787', 10));
