// The netplay wire protocol, and the room logic that is the same everywhere.
//
// One module, three consumers: the Cloudflare room (server.mjs), the local
// dev relay (dev-relay.mjs), and the tests.  The two servers are thin
// adapters over RoomCore so that "does the room behave" is a question about
// this file, answerable without a socket.
//
// The shape of the protocol is dictated by docs/NETPLAY.md §3, which is
// dictated by platform/sio.c: what crosses the wire is an *ordered stream of
// individual bus transfers* per unit, not frames, not fixed-16 blocks.  The
// lobby produces 1-2 transfers a frame and play produces 16, so every batch
// carries its own count, and a sequence number so that a duplicated or
// dropped batch is detected rather than silently corrupting the stream --
// exactly-once, in-order is the whole contract.
//
// Binary messages are little-endian throughout, matching the GBA.
//
//   client -> server   [ MSG_WORDS ] [ u32 seq ] [ u8 count ] [ count x u16 ]
//   server -> clients  [ MSG_WORDS ] [ u8 slot ] [ u32 seq ] [ u8 count ] [ count x u16 ]
//
// `seq` is the index of the first word in the batch within the sender's
// stream -- the sender's cumulative count of successful transfers before
// this batch.  The relay does not interpret it; receivers use it to drop a
// replayed batch after a reconnect and to detect a gap, which is a protocol
// error rather than something to paper over.
//
// Text messages are JSON control traffic, rare by construction:
//
//   server -> client   {type:"joined", slot, online:[bool x 4]}
//   server -> clients  {type:"peer", slot, online}
//   server -> client   {type:"error", reason}   ... and the socket closes.

export const PROTO_VERSION = 1;

export const MSG_WORDS = 0x01;   /* SIO transfer words, Path A            */
export const MSG_INPUT = 0x02;   /* reserved: per-frame keys, Path B      */
export const MSG_LOG   = 0x03;   /* reserved: input-log chunk, Path B     */

export const MAX_PLAYERS = 4;
export const MAX_BATCH   = 64;   /* words per message; 16 is a full frame */

/* --- binary message encode/decode ---------------------------------------- */

export function encodeWords(seq, words) {
    const n = words.length;
    const buf = new Uint8Array(1 + 4 + 1 + 2 * n);
    const dv = new DataView(buf.buffer);
    dv.setUint8(0, MSG_WORDS);
    dv.setUint32(1, seq >>> 0, true);
    dv.setUint8(5, n);
    for (let i = 0; i < n; i++)
        dv.setUint16(6 + 2 * i, words[i] & 0xFFFF, true);
    return buf;
}

/* The server-side form is the client form with the sender's slot spliced in
 * after the type byte, so the relay never re-encodes the payload. */
export function tagWords(slot, clientMsg) {
    const out = new Uint8Array(clientMsg.length + 1);
    out[0] = clientMsg[0];
    out[1] = slot;
    out.set(clientMsg.subarray(1), 2);
    return out;
}

/* Decode the server->client form.  Returns null on a malformed message --
 * the caller treats that as a protocol error, not as silence. */
export function decodeWords(bytes) {
    if (bytes.length < 7 || bytes[0] !== MSG_WORDS)
        return null;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const slot = dv.getUint8(1);
    const seq = dv.getUint32(2, true);
    const count = dv.getUint8(6);
    if (slot >= MAX_PLAYERS || bytes.length !== 7 + 2 * count)
        return null;
    const words = new Uint16Array(count);
    for (let i = 0; i < count; i++)
        words[i] = dv.getUint16(7 + 2 * i, true);
    return { slot, seq, words };
}

/* Validate the client->server form without decoding the payload.  The relay
 * only needs to know it is well-formed and how big it claims to be. */
export function validClientWords(bytes) {
    return bytes.length >= 6 && bytes[0] === MSG_WORDS
        && bytes.length === 6 + 2 * bytes[5]
        && bytes[5] <= MAX_BATCH;
}

/* --- the room ------------------------------------------------------------- */

/* Slot assignment is the one decision the server owns, because the port
 * cannot make it (docs/NETPLAY.md §3 item 4): slot 0 clocks the cable, the
 * game's own lobby classifier requires the occupied slots to be contiguous
 * from 0, and two units that both believe they are slot 0 never find each
 * other and never report why.
 *
 * A connection id that returns gets its old slot back if it is still free --
 * that is what makes a page reload or a network blip survivable once the
 * client passes a stable id.  Slots are otherwise lowest-free-first, which
 * keeps them contiguous as long as leavers are the highest slot; a session
 * that loses a middle slot is not contiguous any more, which Path A simply
 * inherits from the hardware (a cable with a missing middle unit does not
 * work either) and Path B replaces with the rollback slot map. */
export class RoomCore {
    constructor(maxPlayers = MAX_PLAYERS) {
        this.max = Math.min(maxPlayers, MAX_PLAYERS);
        this.slots = new Array(MAX_PLAYERS).fill(null);   /* conn id per slot */
    }

    /* -> slot, or -1 when the room is full. */
    join(id) {
        let slot = this.slots.indexOf(id);
        if (slot === -1)
            slot = this.slots.slice(0, this.max).indexOf(null);
        if (slot === -1)
            return -1;
        this.slots[slot] = id;
        return slot;
    }

    /* -> the slot freed, or -1 if the id was not in the room. */
    leave(id) {
        const slot = this.slots.indexOf(id);
        if (slot !== -1)
            this.slots[slot] = null;
        return slot;
    }

    slotOf(id) { return this.slots.indexOf(id); }

    online() { return this.slots.map((s) => s !== null); }

    occupied() { return this.slots.filter((s) => s !== null).length; }
}

/* --- control messages ----------------------------------------------------- */

export function joinedMsg(slot, online) {
    return JSON.stringify({ type: 'joined', proto: PROTO_VERSION, slot, online });
}

export function peerMsg(slot, online) {
    return JSON.stringify({ type: 'peer', slot, online });
}

export function errorMsg(reason) {
    return JSON.stringify({ type: 'error', reason });
}
