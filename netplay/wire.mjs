// Protocol v2 binary codecs -- shared by the room (session.mjs), the client
// (web/net_session.js carries its own copy of the decoder; tests check the
// two agree) and the tools.
//
// Little-endian throughout.  docs/design/NETPLAY-V2.md "Model v2.1" and §8.
//
// The canonical per-frame record, agreed with the engine (netsim): a sealed
// frame is
//
//     u32 frame; u16 keys[seats]          // seat order; bits 0-9 keys, rest 0
//
// and reaches the engine through PortNetSealFrame(frame, keys, seats).  On
// the wire the room compresses runs of those records against the previous
// frame, because input changes on ~4% of frames: a frame where nobody
// changed costs ceil(seats/8) bytes, at any seat count.
//
//   INPUT   client -> room   (one seat's own column; the room knows whose)
//       u8  0x20
//       u32 firstFrame
//       u8  n                     1..32 frames
//       n x u16 keys
//
//   FRAMES  room -> client   (sealed frames, every seat; also ARCHIVE 0x22)
//       u8  0x21 | 0x22
//       u8  flags                 bit0 KEY: reset the delta base to all-zero
//                                 before the first frame
//       u16 seats                 seat count of this session
//       u32 firstFrame
//       u16 n                     frames that follow, consecutive
//       u32 watermark             no event will be stamped below this frame
//       n x { mask[ceil(seats/8)] bit s = seat s changed;
//             u16 keys for each set bit, ascending seat }
//
//   CHUNK   both ways         (snapshot / sync payload, relayed opaquely)
//       u8  0x30
//       u32 transferId
//       u16 index
//       u16 count
//       ... up to CHUNK_BYTES of payload
//
// Seat indices are u16 in the header and implied by bit position in the
// mask, so nothing here caps a session at 4, or at 255.

export const T_INPUT = 0x20;
export const T_FRAMES = 0x21;
export const T_ARCHIVE = 0x22;
export const T_CHUNK = 0x30;

export const F_KEY = 1;

export const MAX_INPUT_FRAMES = 32;
export const CHUNK_BYTES = 16 * 1024;
export const KEYS_MASK = 0x03FF;

export function maskBytes(seats) { return (seats + 7) >> 3; }

export function encodeInput(firstFrame, keys) {
    const n = keys.length;
    const buf = new Uint8Array(6 + 2 * n);
    const dv = new DataView(buf.buffer);
    buf[0] = T_INPUT;
    dv.setUint32(1, firstFrame >>> 0, true);
    buf[5] = n;
    for (let i = 0; i < n; i++)
        dv.setUint16(6 + 2 * i, keys[i] & KEYS_MASK, true);
    return buf;
}

export function decodeInput(bytes) {
    if (bytes.length < 6 || bytes[0] !== T_INPUT)
        return null;
    const n = bytes[5];
    if (n < 1 || n > MAX_INPUT_FRAMES || bytes.length !== 6 + 2 * n)
        return null;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const first = dv.getUint32(1, true);
    const keys = new Array(n);
    for (let i = 0; i < n; i++)
        keys[i] = dv.getUint16(6 + 2 * i, true) & KEYS_MASK;
    return { first, keys };
}

// frames: array of Uint16Array(seats), consecutive from firstFrame.
// base: Uint16Array(seats), the frame before firstFrame (ignored with KEY).
export function encodeFrames(type, seats, firstFrame, frames, base, watermark, key = false) {
    const mb = maskBytes(seats);
    let size = 14;
    let prev = key ? new Uint16Array(seats) : base;
    for (const f of frames) {
        size += mb;
        for (let s = 0; s < seats; s++)
            if (f[s] !== prev[s])
                size += 2;
        prev = f;
    }
    const buf = new Uint8Array(size);
    const dv = new DataView(buf.buffer);
    buf[0] = type;
    buf[1] = key ? F_KEY : 0;
    dv.setUint16(2, seats, true);
    dv.setUint32(4, firstFrame >>> 0, true);
    dv.setUint16(8, frames.length, true);
    dv.setUint32(10, watermark >>> 0, true);
    let o = 14;
    prev = key ? new Uint16Array(seats) : base;
    for (const f of frames) {
        const m = o;
        o += mb;
        for (let s = 0; s < seats; s++)
            if (f[s] !== prev[s]) {
                buf[m + (s >> 3)] |= 1 << (s & 7);
                dv.setUint16(o, f[s], true);
                o += 2;
            }
        prev = f;
    }
    return buf;
}

// Stateful decoder: holds the delta base across messages.
export class FrameDecoder {
    constructor() { this.base = null; this.seats = 0; this.next = 0; }

    // -> {type, firstFrame, watermark, frames: [Uint16Array]} or null.
    decode(bytes) {
        if (bytes.length < 14 || (bytes[0] !== T_FRAMES && bytes[0] !== T_ARCHIVE))
            return null;
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const key = bytes[1] & F_KEY;
        const seats = dv.getUint16(2, true);
        const first = dv.getUint32(4, true);
        const n = dv.getUint16(8, true);
        const wm = dv.getUint32(10, true);
        if (key || !this.base || this.seats !== seats) {
            if (!key && this.base)
                return null;          // seat count changed without a keyframe
            this.base = new Uint16Array(seats);
            this.seats = seats;
        }
        const mb = maskBytes(seats);
        const frames = [];
        let o = 14;
        let prev = this.base;
        for (let i = 0; i < n; i++) {
            if (o + mb > bytes.length)
                return null;
            const cur = prev.slice();
            const m = o;
            o += mb;
            for (let s = 0; s < seats; s++)
                if (bytes[m + (s >> 3)] & (1 << (s & 7))) {
                    if (o + 2 > bytes.length)
                        return null;
                    cur[s] = dv.getUint16(o, true);
                    o += 2;
                }
            frames.push(cur);
            prev = cur;
        }
        if (o !== bytes.length)
            return null;
        this.base = prev;
        this.next = first + n;
        return { type: bytes[0], firstFrame: first, watermark: wm, frames };
    }
}

export function encodeChunk(id, index, count, payload) {
    const buf = new Uint8Array(9 + payload.length);
    const dv = new DataView(buf.buffer);
    buf[0] = T_CHUNK;
    dv.setUint32(1, id >>> 0, true);
    dv.setUint16(5, index, true);
    dv.setUint16(7, count, true);
    buf.set(payload, 9);
    return buf;
}

export function decodeChunk(bytes) {
    if (bytes.length < 9 || bytes[0] !== T_CHUNK || bytes.length > 9 + CHUNK_BYTES)
        return null;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const id = dv.getUint32(1, true);
    const index = dv.getUint16(5, true);
    const count = dv.getUint16(7, true);
    if (count < 1 || index >= count)
        return null;
    return { id, index, count, payload: bytes.subarray(9) };
}

export function splitChunks(id, payload) {
    const count = Math.max(1, Math.ceil(payload.length / CHUNK_BYTES));
    const out = [];
    for (let i = 0; i < count; i++)
        out.push(encodeChunk(id, i, count,
            payload.subarray(i * CHUNK_BYTES, (i + 1) * CHUNK_BYTES)));
    return out;
}
