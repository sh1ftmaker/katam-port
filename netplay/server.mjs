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
import {
    RoomCore, tagWords, validClientWords, joinedMsg, peerMsg, errorMsg,
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
    }

    onConnect(conn) {
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
        if (typeof message === 'string')
            return;                             /* clients send no control    */
        const bytes = new Uint8Array(message);
        if (!validClientWords(bytes)) {
            conn.send(errorMsg('malformed'));
            conn.close(4002, 'malformed');
            return;
        }
        const s = conn.state;
        if (!s || typeof s.slot !== 'number')
            return;
        this.broadcast(tagWords(s.slot, bytes), [conn.id]);
    }

    onClose(conn) {
        const s = conn.state;
        if (!s || typeof s.slot !== 'number')
            return;
        if (this.core.slots[s.slot] === conn.id) {
            this.core.leave(conn.id);
            this.broadcast(peerMsg(s.slot, false));
        }
    }
}

export default {
    fetch: (req, env) =>
        routePartykitRequest(req, env).then(
            (r) => r ?? new Response('katam-port netplay relay', { status: 200 })),
};
