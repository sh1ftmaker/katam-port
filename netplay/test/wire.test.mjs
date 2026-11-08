import test from 'node:test';
import assert from 'node:assert/strict';
import {
    encodeInput, decodeInput, encodeFrames, FrameDecoder, T_FRAMES, splitChunks, decodeChunk,
    CHUNK_BYTES, maskBytes,
} from '../wire.mjs';
import { seededRng } from '../codes.mjs';

test('INPUT roundtrip and validation', () => {
    const b = encodeInput(123456, [1, 0x3FF, 0xFFFF]);
    assert.deepEqual(decodeInput(b), { first: 123456, keys: [1, 0x3FF, 0x3FF] });
    assert.equal(decodeInput(b.subarray(0, 7)), null);
    assert.equal(decodeInput(encodeInput(1, new Array(33).fill(0))), null);
});

test('FRAMES: random streams at 1, 4, 8, 16, 100, 1000 seats roundtrip across messages', () => {
    const rng = seededRng(42);
    for (const seats of [1, 4, 8, 16, 100, 1000]) {
        const dec = new FrameDecoder();
        let base = new Uint16Array(seats), f = 5000, key = true;
        const all = [];
        let bytes = 0;
        for (let msg = 0; msg < 40; msg++) {
            const frames = [];
            let cur = base;
            for (let i = 0; i < 1 + (rng(1)[0] & 7); i++) {
                cur = cur.slice();
                for (let s = 0; s < seats; s++) if ((rng(1)[0] & 31) === 0) cur[s] = (rng(2)[0] << 2) & 0x3FF;
                frames.push(cur);
            }
            const enc = encodeFrames(T_FRAMES, seats, f, frames, base, f + 30, key);
            bytes += enc.length;
            const d = dec.decode(enc);
            assert.equal(d.firstFrame, f);
            assert.equal(d.watermark, f + 30);
            d.frames.forEach((x, i) => assert.deepEqual([...x], [...frames[i]]));
            all.push(...frames);
            f += frames.length; base = frames.at(-1); key = false;
        }
        // ~3% change rate: well under a full u16 per seat per frame
        assert.ok(bytes / all.length < maskBytes(seats) + seats * 2 * 0.08 + 14, `${seats}: ${bytes / all.length}`);
    }
});

test('FRAMES: a delta without its base is refused', () => {
    const enc = encodeFrames(T_FRAMES, 4, 10, [new Uint16Array([1, 0, 0, 0])], new Uint16Array(4), 40, false);
    assert.equal(new FrameDecoder().decode(enc) !== null, true, 'fresh decoder treats first as key-less zero base');
    const d = new FrameDecoder();
    d.decode(encodeFrames(T_FRAMES, 4, 10, [new Uint16Array(4)], null, 40, true));
    assert.equal(d.decode(encodeFrames(T_FRAMES, 8, 11, [new Uint16Array(8)], new Uint16Array(8), 40, false)), null);
});

test('CHUNK split/reassemble', () => {
    const p = new Uint8Array(CHUNK_BYTES * 2 + 5).map((_, i) => i * 7);
    const ch = splitChunks(9, p).map(decodeChunk);
    assert.equal(ch.length, 3);
    const out = new Uint8Array(p.length);
    let o = 0;
    for (const c of ch) { out.set(c.payload, o); o += c.payload.length; }
    assert.deepEqual(out, p);
});
