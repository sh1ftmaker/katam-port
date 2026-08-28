// Seats (platform/port/seats.h): four game states multiplexed through one
// instance must be what four instances would have computed.
//
//   node tools/seat_test.mjs <katam-node.js> <rom.gba>
//
// Five instances boot the same scripted world (the rb_boot.js recipe, driven
// here through the C exports alone: no relay, no lobby) with every input of
// every player confirmed up front, so nothing is ever predicted.  Four are
// the references: one real instance per seat, camera pinned to its Kirby by
// PortRbNetPlay at frame FC.  The fifth shuts its rollback session down at
// FC and calls PortSeatInit(4) instead, and from then on presents four
// pictures a round.
//
// PASS is: for every frame from FC+1 to END, seat k's framebuffer is
// byte-identical to reference k's at the same game frame, and the game's own
// desync quantity agrees at END.  The four references also have to agree
// with each other on the desync quantity -- that is the precondition, not
// the test.

import { createRequire } from 'module';
import path from 'path';
import fs from 'fs';

const require = createRequire(import.meta.url);
const [, , modulePath, romPath] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/seat_test.mjs <katam-node.js> <rom.gba>');
    process.exit(2);
}
const createKatam = require(path.resolve(modulePath));
const rom = fs.readFileSync(romPath);
global.requestAnimationFrame = (cb) => setImmediate(cb);

const F0 = 2200, FC = 2230, END = 2900, DEPTH = 16, AHEAD = 8, SEATS = 4;
const EV_PLAYERS = 1, EV_NETPLAY = 3, EV_STORY = 4;
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                 [810, 0x80], [825, 0], [850, 1], [870, 0]];
function bootMask(f) {
    if (f >= 300 && f < 440) return ((f - 300) % 8) < 4 ? 1 : 0;
    let mask = 0;
    for (const [at, m] of PRESSES) if (f >= at) mask = m;
    return f < 900 ? mask : 0;
}
// Four different players once the world is live: 0 runs right, 1 runs left
// and jumps, 2 hops on the spot, 3 stands there.
function keysFor(slot, f) {
    if (f < F0) return bootMask(f);
    switch (slot) {
    case 0: return (f >= 2350 && f < 2500) ? 0x10 : 0;
    case 1: return ((f >= 2400 && f < 2600) ? 0x20 : 0) | ((f >= 2450 && f < 2456) ? 1 : 0);
    case 2: return (f >= 2300 && ((f - 2300) % 40) < 4) ? 1 : 0;
    default: return 0;
    }
}

function desyncQuantity(Module) {
    const dv = new DataView(Module.HEAPU8.buffer);
    const parts = [dv.getUint32(0x030068D8, true)];
    for (let i = 0; i < 4; i++) {
        parts.push(dv.getInt32(0x02020EE0 + 424 * i + 64, true));
        parts.push(dv.getInt32(0x02020EE0 + 424 * i + 68, true));
    }
    return parts.join(',');
}
function fnv(bytes) {
    let h = 0x811c9dc5;
    for (let i = 0; i < bytes.length; i++) { h ^= bytes[i]; h = Math.imul(h, 0x01000193); }
    return (h >>> 0).toString(16).padStart(8, '0');
}

// { frames: Map<frame -> hash>, per seat }, plus desync at END
function makeInstance(name, self, multiplex) {
    const inst = { name, logs: [], hashes: [], desync: [], done: null };
    for (let s = 0; s < SEATS; s++) { inst.hashes.push(new Map()); inst.desync.push(null); }
    let resolveDone; inst.done = new Promise((r) => { resolveDone = r; });
    let resolveRom;
    let finished = false;
    // Inputs are confirmed a little ahead of the frame loop rather than all
    // at once: the engine treats the end of the timeline as the present, so
    // a timeline confirmed to END would be replayed to END with the picture
    // off, and nothing would ever be presented.  AHEAD keeps every column
    // known past the stall gate's window.
    let confirmed = -1;
    function confirmThrough(f) {
        for (; confirmed < f; confirmed++)
            for (let s = 0; s < SEATS; s++)
                Module._PortRbConfirmInput(s, confirmed + 1, keysFor(s, confirmed + 1));
    }
    const Module = {
        portRomReady: new Promise((r) => { resolveRom = r; }),
        portAudioRate: 0,
        print: (t) => inst.logs.push(t),
        printErr: (t) => inst.logs.push(t),
        portPresent(ptr, w, h) {
            if (finished) return;
            const frame = Module._PortFrameNumber();
            if (Module._PortRbActive()) confirmThrough(frame + AHEAD);
            const seat = multiplex && Module._PortSeatActive() ? Module._PortSeatCurrent() : self;
            if (frame > FC) {
                inst.hashes[seat].set(frame, fnv(Module.HEAPU8.subarray(ptr, ptr + w * h * 4)));
                if (frame === END) inst.desync[seat] = desyncQuantity(Module);
            }
            if (frame === FC) {
                if (!multiplex) {
                    Module._PortRbNetPlay(self);
                } else if (!Module._PortSeatActive()) {
                    Module._PortRbShutdown();
                    for (let s = 0; s < SEATS; s++) Module._PortSeatSetInput(s, keysFor(s, FC));
                    if (!Module._PortSeatInit(SEATS)) { console.error('PortSeatInit failed'); process.exit(1); }
                    for (let s = 0; s < SEATS; s++) Module._PortSeatSetInput(s, keysFor(s, FC + 1));
                }
            } else if (multiplex && Module._PortSeatActive() && Module._PortSeatIsLast()) {
                for (let s = 0; s < SEATS; s++) Module._PortSeatSetInput(s, keysFor(s, frame + 1));
            }
            if (frame >= END && (!multiplex || Module._PortSeatIsLast())) {
                finished = true;
                resolveDone();
            }
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            if (!Module._PortRbInit(DEPTH, SEATS)) { console.error('PortRbInit failed'); process.exit(1); }
            Module._PortRbSetSelf(self);
            Module._PortRbScheduleEvent(0, EV_PLAYERS, 1, 0);
            Module._PortRbScheduleEvent(0, EV_STORY, 3, 0);
            Module._PortRbScheduleEvent(F0, EV_PLAYERS, 4, 0);
            Module._PortRbScheduleEvent(F0, EV_NETPLAY, 0, 0);
            confirmThrough(F0 + AHEAD);
            Module._PortRbReplayTo(F0);
            resolveRom();
        },
    };
    inst.Module = Module;
    createKatam(Module);
    return inst;
}

const t0 = Date.now();
const refs = [];
for (let s = 0; s < SEATS; s++) {
    const r = makeInstance(`ref${s}`, s, false);
    await r.done;
    console.log(`[test] ${r.name} reached ${END} (${Date.now() - t0} ms)`);
    refs.push(r);
}
const mux = makeInstance('seats', 0, true);
await mux.done;
console.log(`[test] seats reached ${END} (${Date.now() - t0} ms)`);

let ok = true;
for (let s = 1; s < SEATS; s++)
    if (refs[s].desync[s] !== refs[0].desync[0]) {
        console.error(`[test] PRECONDITION FAILED: ref${s} desync ${refs[s].desync[s]} != ref0 ${refs[0].desync[0]}`);
        ok = false;
    }
for (let s = 0; s < SEATS; s++) {
    const ref = refs[s].hashes[s], got = mux.hashes[s];
    let first = -1, mismatches = 0, compared = 0;
    for (let f = FC + 1; f <= END; f++) {
        if (!ref.has(f) || !got.has(f)) { if (first < 0) first = f; mismatches++; continue; }
        compared++;
        if (ref.get(f) !== got.get(f)) { if (first < 0) first = f; mismatches++; }
    }
    const dq = mux.desync[s] !== null && mux.desync[s] === refs[s].desync[s];
    console.log(`[test] seat ${s}: ${compared} frames compared, ${mismatches} mismatches` +
                (first >= 0 ? ` (first at ${first})` : '') + `, desync ${dq ? 'equal' : 'DIFFERENT'}`);
    if (mismatches || !dq) ok = false;
}
if (!ok) {
    console.error('SEAT TEST FAILED');
    for (const i of [...refs, mux]) for (const l of i.logs.slice(-8)) console.error(`  [${i.name}] ${l}`);
    process.exit(1);
}
console.log(`SEAT TEST PASSED: ${SEATS} seats bit-identical to ${SEATS} instances over frames ${FC + 1}..${END}`);
process.exit(0);
