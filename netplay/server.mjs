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
    validClientInput, decodeTaggedInput, encodeLogBatch, LOG_BATCH,
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
                this.assigns.push(a);
                this.broadcast(JSON.stringify(a));           /* sender too */
            }
            return;
        }
        const bytes = new Uint8Array(message);
        if (validClientWords(bytes)) {
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
