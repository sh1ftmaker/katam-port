// The frame codec for server-side play (docs/SERVER-RENDER.md §1): a 240x160
// GBA picture as a small delta against the previous one, shared by the server
// (encode) and the thin client (decode).
//
// Pixels are the console's own 15-bit RGB555, because that is what the port's
// PPU expands from -- (c & 0x1F) << 3 per channel, platform/ppu.c -- so the
// conversion back is exact and a delta of 555 words is a delta of the real
// picture.  Three ways to say a frame, chosen per frame by measured size:
//
//   MODE_RAW   the 555 words themselves (a keyframe; also what a joiner gets)
//   MODE_XOR   XOR against the previous frame
//   MODE_MC    XOR against the previous frame *shifted* by (dx, dy) -- the
//              main background's scroll delta, which the server knows from
//              the BG?HOFS/VOFS registers.  A scroll moves every pixel, so a
//              plain XOR of a scrolling frame is bigger than the raw frame
//              (measured: 15.5 KB against 11.4 KB); shifted, the delta is
//              sprites and the newly revealed edge again.
//
// The delta bytes are then deflated by the caller (deflateRawSync in node,
// CompressionStream('deflate-raw') in a Worker or browser); this module is
// synchronous and pure so the same code runs everywhere.  Wire form:
//
//   byte 0      mode
//   byte 1, 2   dx, dy as int8 (MODE_MC only; 0 otherwise)
//   byte 3..    deflate-raw of W*H*2 bytes of little-endian u16
//
// The shift candidates come from the four BG layers' scroll deltas plus
// (0,0); the pick is by a cheap changed-word count over a row subsample, and
// only the winner is deflated.  A raw encode is tried as well whenever the
// winner is still large, so the worst case is the raw frame, never worse.

export const W = 240, H = 160, N = W * H;
export const MODE_RAW = 0, MODE_XOR = 1, MODE_MC = 2;
export const HEADER = 3;

/* The port's RGBA8888 framebuffer (u32 little-endian: R in the low byte)
 * back to the 555 words it was expanded from. */
export function rgbaTo555(rgba, out) {
    out = out || new Uint16Array(N);
    for (let i = 0, j = 0; i < N; i++, j += 4)
        out[i] = (rgba[j] >> 3) | ((rgba[j + 1] >> 3) << 5) | ((rgba[j + 2] >> 3) << 10);
    return out;
}

/* 555 words to RGBA8888, the PPU's expansion exactly. */
export function to555Rgba(px, out) {
    out = out || new Uint8ClampedArray(N * 4);
    for (let i = 0, j = 0; i < N; i++, j += 4) {
        const c = px[i];
        out[j] = (c & 0x1F) << 3; out[j + 1] = ((c >> 5) & 0x1F) << 3;
        out[j + 2] = ((c >> 10) & 0x1F) << 3; out[j + 3] = 255;
    }
    return out;
}

/* The four BG layers' scroll registers, from the IO block: [h0,v0,h1,v1,...]
 * The GBA keeps 9 bits of each. */
export function scrollRegs(io, base) {
    const r = new Int32Array(8);
    for (let i = 0; i < 8; i++)
        r[i] = (io[base + 0x10 + i * 2] | (io[base + 0x11 + i * 2] << 8)) & 0x1FF;
    return r;
}

/* prev shifted by (dx, dy), XORed into cur, written to out.  A pixel whose
 * source falls off the previous frame is XORed with 0, i.e. sent raw. */
function xorShifted(cur, prev, dx, dy, out) {
    for (let y = 0; y < H; y++) {
        const sy = y + dy;
        const o = y * W;
        if (sy < 0 || sy >= H) {
            for (let x = 0; x < W; x++) out[o + x] = cur[o + x];
            continue;
        }
        const s = sy * W;
        for (let x = 0; x < W; x++) {
            const sx = x + dx;
            out[o + x] = (sx < 0 || sx >= W) ? cur[o + x] : cur[o + x] ^ prev[s + sx];
        }
    }
    return out;
}

/* Cheap cost of a candidate: changed words on every 8th row. */
function shiftCost(cur, prev, dx, dy) {
    let n = 0;
    for (let y = 0; y < H; y += 8) {
        const sy = y + dy, o = y * W;
        if (sy < 0 || sy >= H) { n += W; continue; }
        const s = sy * W;
        for (let x = 0; x < W; x++) {
            const sx = x + dx;
            if (sx < 0 || sx >= W || cur[o + x] !== prev[s + sx]) n++;
        }
    }
    return n;
}

function wrap9(d) {                 /* a 9-bit register delta as a signed shift */
    d &= 0x1FF;
    return d >= 256 ? d - 512 : d;
}

/* A large delta is worth a second opinion from a raw encode: past this many
 * deflated bytes the raw frame (measured ~11 KB) may well be smaller. */
const RAW_RETRY_BYTES = 6000;

export function createEncoder(deflate) {
    let prev = null, prevRegs = null;
    const scratch = new Uint16Array(N);
    const bytes = new Uint8Array(scratch.buffer);

    function packet(mode, dx, dy, words) {
        const body = deflate(new Uint8Array(words.buffer, words.byteOffset, N * 2));
        const out = new Uint8Array(HEADER + body.length);
        out[0] = mode; out[1] = dx & 0xFF; out[2] = dy & 0xFF;
        out.set(body, HEADER);
        return out;
    }

    /* cur: Uint16Array of 555 words (a copy is kept); regs: scrollRegs().
     * Returns { packet: Uint8Array, mode, dx, dy }. */
    function encode(cur, regs, forceRaw) {
        let best;
        if (prev === null || forceRaw) {
            best = { mode: MODE_RAW, dx: 0, dy: 0, packet: packet(MODE_RAW, 0, 0, cur) };
        } else {
            /* Candidates: no shift, and each layer's scroll delta. */
            const cands = [[0, 0]];
            if (regs && prevRegs)
                for (let l = 0; l < 4; l++) {
                    const dx = wrap9(regs[l * 2] - prevRegs[l * 2]);
                    const dy = wrap9(regs[l * 2 + 1] - prevRegs[l * 2 + 1]);
                    if ((dx || dy) && Math.abs(dx) < 128 && Math.abs(dy) < 128
                        && !cands.some((c) => c[0] === dx && c[1] === dy))
                        cands.push([dx, dy]);
                }
            let pick = cands[0], pickCost = Infinity;
            for (const c of cands) {
                const cost = shiftCost(cur, prev, c[0], c[1]);
                if (cost < pickCost) { pickCost = cost; pick = c; }
            }
            const [dx, dy] = pick;
            xorShifted(cur, prev, dx, dy, scratch);
            const mode = (dx || dy) ? MODE_MC : MODE_XOR;
            best = { mode, dx, dy, packet: packet(mode, dx, dy, scratch) };
            if (best.packet.length > RAW_RETRY_BYTES) {
                const raw = packet(MODE_RAW, 0, 0, cur);
                if (raw.length < best.packet.length)
                    best = { mode: MODE_RAW, dx: 0, dy: 0, packet: raw };
            }
        }
        prev = prev || new Uint16Array(N);
        prev.set(cur);
        prevRegs = regs ? Int32Array.from(regs) : null;
        return best;
    }

    return { encode, reset() { prev = null; prevRegs = null; } };
}

export function createDecoder(inflate) {
    let prev = null;
    const cur = new Uint16Array(N);

    /* Returns the frame as a Uint16Array of 555 words (reused; copy to keep). */
    function decode(pkt) {
        const mode = pkt[0];
        const dx = (pkt[1] << 24) >> 24, dy = (pkt[2] << 24) >> 24;
        const body = inflate(pkt.subarray(HEADER));
        if (body.length !== N * 2) throw new Error('frame packet: bad length ' + body.length);
        const words = new Uint16Array(body.buffer, body.byteOffset, N);
        if (mode === MODE_RAW) {
            cur.set(words);
        } else {
            if (prev === null) throw new Error('frame packet: delta with no previous frame');
            /* The encoder wrote cur ^ shifted(prev); the same operation on
             * the delta gives cur back. */
            xorShifted(words, prev, dx, dy, cur);
        }
        prev = prev || new Uint16Array(N);
        prev.set(cur);
        return cur;
    }

    return { decode, reset() { prev = null; } };
}
