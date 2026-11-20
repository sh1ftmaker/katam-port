// Save states and rewind (platform/states.c): a loaded state and a rewound
// frame must be the frames they were the first time.
//
//   node tools/states_test.mjs <katam-node.js> <rom.gba>
//
// One instance, keys scripted by frame (A through the title, then nothing).
// Pass 1 records every presented frame's state hash (the GBA map).  At SAVE_AT it
// saves slot 1; at LOAD_AT it loads it, and the next frames must repeat the
// hashes from just after the save.  Later it holds rewind for RW_FRAMES
// presented frames, each of which must be a frame from pass 1 (same frame
// number, same state), going backwards.  ~8 s.

import { createRequire } from 'module';
import path from 'path';
import fs from 'fs';

const require = createRequire(import.meta.url);
const [, , modulePath, romPath] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/states_test.mjs <katam-node.js> <rom.gba>');
    process.exit(2);
}
const createKatam = require(path.resolve(modulePath));
const rom = fs.readFileSync(romPath);
global.requestAnimationFrame = (cb) => setImmediate(cb);

const SAVE_AT = 1000, LOAD_AT = 1300, CHECK = 60, RW_AT = 1600, RW_FRAMES = 60, END = 1800;
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0], [850, 1], [870, 0]];
function keysAt(f) {
    if (f >= 300 && f < 440) return ((f - 300) % 8) < 4 ? 1 : 0;
    let m = 0;
    for (const [at, k] of PRESSES) if (f >= at) m = k;
    return f < 900 ? m : 0;
}
// The game's state at the present (not the picture: the party HUD's
// open/closed cards are the host's own animation, not part of a state).
const REGIONS = [[0x02000000, 0x40000], [0x03000000, 0x8000], [0x04000000, 0x400],
                 [0x05000000, 0x400], [0x06000000, 0x18000], [0x07000000, 0x400]];
function stateHash(heap) {
    let h = 0;
    for (const [b, l] of REGIONS) h = (Math.imul(h, 31) + fnv(heap.subarray(b, b + l))) >>> 0;
    return h;
}
function fnv(bytes) {
    let h = 0x811c9dc5;
    for (let i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193); }
    return h >>> 0;
}

const first = new Map();                // frame -> hash, before the load
let blob = null, notes = [], fails = [];
let phase = 'run', loadCheck = [], rw = [], presented = 0, rwLeft = 0;
let resolveDone; const done = new Promise((r) => { resolveDone = r; });
let resolveRom;

const Module = {
    portRomReady: new Promise((r) => { resolveRom = r; }),
    portAudioRate: 0,
    print: () => {}, printErr: () => {},
    portStatesNote: (m) => notes.push(m),
    portStatesStore: (slot, bytes) => { blob = bytes; Module._PortStSetHave(1); },
    portStatesFetch: (slot) => {
        const p = Module._malloc(blob.length);
        Module.HEAPU8.set(blob, p);
        Module._PortStLoadBlob(slot, p, blob.length);
    },
    portPresent(ptr, w, h) {
        const f = Module._PortFrameNumber();
        const hash = stateHash(Module.HEAPU8);
        presented++;
        if (phase === 'run') {
            if (!first.has(f)) first.set(f, hash);
            if (f === SAVE_AT) Module._PortStRequestSave(1);
            if (f === LOAD_AT) { Module._PortStRequestLoad(1); phase = 'afterload'; }
        } else if (phase === 'afterload') {
            loadCheck.push([f, hash]);
            if (loadCheck.length >= CHECK) phase = 'replay';
        } else if (phase === 'replay') {
            if (f === RW_AT) { Module._PortStSetRewindHeld(1); phase = 'rewind'; rwLeft = RW_FRAMES; }
        } else if (phase === 'rewind') {
            rw.push([f, hash]);
            if (--rwLeft === 0) { Module._PortStSetRewindHeld(0); phase = 'tail'; }
        } else if (phase === 'tail' && presented > 3000) {
            phase = 'end';
            resolveDone();
        }
        if (phase === 'tail' && f >= END) { phase = 'end'; resolveDone(); }
        Module._PortSetKeys(keysAt(f + 1));
    },
    onRuntimeInitialized() {
        Module.HEAPU8.set(rom, 0x08000000);
        Module._PortRomLoaded(rom.length);
        Module._PortStSetPrefs(1, 1, 3, 2, 1);
        resolveRom();
    },
};
createKatam(Module);
const t0 = Date.now();
await done;

if (!blob) fails.push('no state was stored');
else console.log(`[states] slot blob ${blob.length} bytes`);
// The load: the first frame after it is the frame after the save.
const saveFrame = loadCheck.length ? loadCheck[0][0] : -1;
if (saveFrame < SAVE_AT || saveFrame > SAVE_AT + 2) fails.push(`load resumed at frame ${saveFrame}, not ~${SAVE_AT + 1}`);
let bad = 0;
for (const [f, h] of loadCheck) if (first.get(f) !== h) bad++;
console.log(`[states] load: resumed at ${saveFrame}, ${loadCheck.length - bad}/${loadCheck.length} frames identical`);
if (bad) fails.push(`${bad} frames after the load differ`);
// The rewind: backwards, every picture a frame from before.
let back = 0, rbad = 0;
for (let i = 0; i < rw.length; i++) {
    const [f, h] = rw[i];
    if (i && f < rw[i - 1][0]) back++;
    const want = first.get(f);
    if (want !== undefined && want !== h) rbad++;
}
const span = rw.length ? rw[0][0] - rw[rw.length - 1][0] : 0;
console.log(`[states] rewind: ${rw.length} frames, ${span} game frames back, ${rbad} states differ`);
if (span < RW_FRAMES) fails.push(`rewind went back only ${span} frames`);
if (back < rw.length - 2) fails.push('rewind did not run backwards');
if (rbad) fails.push(`${rbad} rewound frames differ from the originals`);
if (!notes.includes('STATE 1 SAVED') || !notes.includes('STATE 1 LOADED')) fails.push('notes: ' + notes.join(' | '));
// The guard.
if (Module._PortStAllowed() !== 1) fails.push('not allowed offline');

console.log(`[states] ${Date.now() - t0} ms`);
if (fails.length) {
    console.error('STATES TEST FAILED:\n  ' + fails.join('\n  '));
    process.exit(1);
}
console.log('STATES TEST PASSED');
process.exit(0);
