// The frame codec (netplay/framecodec.mjs) against real gameplay: roundtrip
// exactness on every frame, and the sizes docs/SERVER-RENDER.md §1 promises.
//
//   node tools/framecodec_test.mjs <katam-node.js> <rom.gba>
//
// One instance boots the scripted world (the same C-export recipe as
// tools/seat_test.mjs), then Kirby runs right for most of the run so the
// screen actually scrolls -- the case a plain XOR delta loses to raw.  (It
// is flying: the hub is too small to run through.)  Every
// presented frame is encoded, decoded by an independent decoder, and
// compared word for word.  PASS is every frame equal; the sizes are printed
// per mode for the record.

import { createRequire } from 'module';
import path from 'path';
import fs from 'fs';
import zlib from 'zlib';
import { createEncoder, createDecoder, rgbaTo555, scrollRegs, MODE_RAW, MODE_XOR, MODE_MC }
    from '../netplay/framecodec.mjs';

const require = createRequire(import.meta.url);
const [, , modulePath, romPath] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/framecodec_test.mjs <katam-node.js> <rom.gba>');
    process.exit(2);
}
const createKatam = require(path.resolve(modulePath));
const rom = fs.readFileSync(romPath);
global.requestAnimationFrame = (cb) => setImmediate(cb);

const F0 = 2200, START = 2231, END = 3100, DEPTH = 16, AHEAD = 8;
const EV_PLAYERS = 1, EV_NETPLAY = 3, EV_STORY = 4;
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                 [810, 0x80], [825, 0], [850, 1], [870, 0]];
function bootMask(f) {
    if (f >= 300 && f < 440) return ((f - 300) % 8) < 4 ? 1 : 0;
    let mask = 0;
    for (const [at, m] of PRESSES) if (f >= at) mask = m;
    return f < 900 ? mask : 0;
}
// Player 0 flies up and right from 2300 to 3000 (Up is fly); the
// others stand.  Long enough for rooms to stream and the screen to scroll.
function keysFor(slot, f) {
    if (f < F0) return bootMask(f);
    if (slot !== 0) return 0;
    return (f >= 2300 && f < 3000) ? (0x10 | 0x40) : 0;
}

const deflate = (b) => zlib.deflateRawSync(b, { level: 6 });
const inflate = (b) => zlib.inflateRawSync(b);
const enc = createEncoder(deflate), dec = createDecoder(inflate);
const sizes = { [MODE_RAW]: [], [MODE_XOR]: [], [MODE_MC]: [] };
const all = [];
let mismatches = 0, encodeMs = 0, decodeMs = 0, scrolled = 0;
let prevRegs = null;

let resolveDone; const done = new Promise((r) => { resolveDone = r; });
let confirmed = -1, finished = false;
const Module = {
    portRomReady: Promise.resolve(),
    portAudioRate: 0, print() {}, printErr() {},
    portPresent(ptr, w, h) {
        if (finished) return;
        const frame = Module._PortFrameNumber();
        for (; confirmed < frame + AHEAD; confirmed++)
            Module._PortRbConfirmInput(0, confirmed + 1, keysFor(0, confirmed + 1));
        if (frame >= START) {
            const px = rgbaTo555(Module.HEAPU8.subarray(ptr, ptr + w * h * 4));
            const regs = scrollRegs(Module.HEAPU8, 0x04000000);
            if (process.env.SAVE_AT && process.env.SAVE_AT.split(',').map(Number).includes(frame)) {
                const rgb = Buffer.alloc(w * h * 3), src = Module.HEAPU8.subarray(ptr, ptr + w * h * 4);
                for (let i = 0, j = 0; i < w * h; i++, j += 3) { rgb[j] = src[i * 4]; rgb[j + 1] = src[i * 4 + 1]; rgb[j + 2] = src[i * 4 + 2]; }
                fs.writeFileSync(`build/codec-${frame}.ppm`, Buffer.concat([Buffer.from(`P6\n${w} ${h}\n255\n`), rgb]));
            }
            if (prevRegs && (regs[0] !== prevRegs[0] || regs[1] !== prevRegs[1])) scrolled++;
            prevRegs = regs;
            let t = performance.now();
            const r = enc.encode(px, regs);
            encodeMs += performance.now() - t;
            t = performance.now();
            const back = dec.decode(r.packet);
            decodeMs += performance.now() - t;
            for (let i = 0; i < px.length; i++)
                if (px[i] !== back[i]) { mismatches++; break; }
            sizes[r.mode].push(r.packet.length);
            all.push(r.packet.length);
        }
        if (frame >= END) { finished = true; resolveDone(); }
    },
    onRuntimeInitialized() {
        Module.HEAPU8.set(rom, 0x08000000);
        Module._PortRomLoaded(rom.length);
        Module._PortRbInit(DEPTH, 1);
        Module._PortRbSetSelf(0);
        Module._PortRbScheduleEvent(0, EV_PLAYERS, 1, 0);
        Module._PortRbScheduleEvent(0, EV_STORY, 3, 0);
        Module._PortRbScheduleEvent(F0, EV_PLAYERS, 4, 0);
        Module._PortRbScheduleEvent(F0, EV_NETPLAY, 0, 0);
        for (; confirmed < F0 + AHEAD; confirmed++)
            Module._PortRbConfirmInput(0, confirmed + 1, keysFor(0, confirmed + 1));
        Module._PortRbReplayTo(F0);
    },
};
createKatam(Module);
await done;

function stats(v) {
    if (!v.length) return 'none';
    const s = [...v].sort((a, b) => a - b);
    const mean = s.reduce((a, b) => a + b, 0) / s.length;
    return `${s.length} frames, mean ${mean.toFixed(0)} B, p50 ${s[s.length >> 1]}, p95 ${s[Math.floor(s.length * 0.95)]}, max ${s[s.length - 1]}`;
}
const n = all.length;
console.log(`[codec] ${n} frames ${START}..${END}, ${scrolled} with BG0 scrolled; encode ${(encodeMs / n).toFixed(2)} ms/frame, decode ${(decodeMs / n).toFixed(2)} ms/frame`);
console.log(`[codec] raw   : ${stats(sizes[MODE_RAW])}`);
console.log(`[codec] xor   : ${stats(sizes[MODE_XOR])}`);
console.log(`[codec] mc    : ${stats(sizes[MODE_MC])}`);
console.log(`[codec] all   : ${stats(all)}  =>  ${(all.reduce((a, b) => a + b, 0) / n * 60 * 8 / 1e6).toFixed(2)} Mbit/s at 60 fps`);
if (mismatches) { console.error(`FRAME CODEC TEST FAILED: ${mismatches} frames did not roundtrip`); process.exit(1); }
console.log(`FRAME CODEC TEST PASSED: ${n} frames roundtrip exactly`);
process.exit(0);
