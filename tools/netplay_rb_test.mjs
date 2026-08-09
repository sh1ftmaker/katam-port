// Milestone B: drop-in / drop-out over the rollback timeline.
//
//   node tools/netplay_rb_test.mjs build/katam-node.js <rom.gba>
//
// Three instances, one relay, no game lobby anywhere:
//
//   alpha and beta boot the same scripted single-player game, activate the
//   session at the same frame (PortRbInit + PortRbNetPlay), and play with
//   different inputs -- alpha's Kirby walks right while beta's walks left,
//   each instance simulating both.  Sync is judged by the game's own desync
//   quantity (gRngVal plus every Kirby's x and y) read at the same game
//   frame on each instance.
//
//   beta then disconnects; alpha's driver hands beta's Kirby to the AI via
//   a relayed timeline event.
//
//   gamma booted the same script but held its frame loop at the activation
//   frame; it now fetches the room's history, replays the whole session
//   with the picture off, takes the vacated seat, and plays -- and its
//   desync quantity must equal alpha's at the same frame.
//
// PASS is both equalities: alpha==beta while beta played, alpha==gamma
// after gamma joined.

import { createRequire } from 'module';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

const [, , modulePath, romPath] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/netplay_rb_test.mjs <katam-node.js> <rom.gba>');
    process.exit(2);
}

const { startRelay } = await import(path.join(here, '../netplay/dev-relay.mjs'));
const createKatam = require(path.resolve(modulePath));
const createRb = require(path.join(here, '../web/rb_net.js'));
const fs = require('fs');
const rom = fs.readFileSync(romPath);

// Delivery must beat the frame loop -- docs/NETPLAY.md §3a item 4.
const pendingRaf = [];
global.requestAnimationFrame = (cb) => setTimeout(cb, 1);

const F0 = 2200;                 /* session activation frame                */
const DEPTH = 16;
const CHECK1 = 2620;             /* alpha == beta here                      */
const B_LEAVES = 2700;
const C_JOINS_AT = 2950;         /* ...when alpha reaches this frame        */
const CHECK2 = 3230;             /* alpha == gamma here                     */
const END = 3400;

// The scripted boot: title -> FILE 1 -> ONE PLAYER, the plain-release mash
// variant (the §3a item 6 latch quirk, used deliberately this time: the
// extra A edge at 480 picks single player, which is exactly the world this
// path plays in).
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                 [810, 0x80], [825, 0], [850, 1], [870, 0]];
function bootMask(f) {
    if (f >= 300 && f < 440)
        return ((f - 300) % 8) < 4 ? 1 : 0;
    let mask = 0;
    for (const [at, m] of PRESSES)
        if (f >= at)
            mask = m;
    return f < 900 ? mask : 0;
}

// Per-instance play scripts, by game frame, applied through the driver.
const PLAY = {
    alpha: (f) => (f >= 2300 && f < 2450) ? 0x10 : 0,     /* RIGHT */
    beta:  (f) => (f >= 2350 && f < 2500) ? 0x20 : 0,     /* LEFT  */
    gamma: (f) => (f >= 3050 && f < 3150) ? 0x10 : 0,     /* RIGHT */
};

// The desync quantity, the same one the game's own detector hashes:
// gRngVal at 0x030068D8 plus each Kirby's x and y (0x02020EE0 + 424*i + 64
// and +68) -- docs/MULTIPLAYER.md §7.
function desyncQuantity(Module) {
    const dv = new DataView(Module.HEAPU8.buffer);
    const parts = [dv.getUint32(0x030068D8, true)];
    for (let i = 0; i < 4; i++) {
        parts.push(dv.getInt32(0x02020EE0 + 424 * i + 64, true));
        parts.push(dv.getInt32(0x02020EE0 + 424 * i + 68, true));
    }
    return parts.join(',');
}

const relay = await startRelay(0, { quiet: true });

function makeInstance(name, opts) {
    const inst = {
        name, frames: 0, logs: [], driver: null, Module: null,
        paused: false, resume: null, dead: false,
        checkpoints: {}, seatRequested: false, joined: !opts.joiner,
    };
    const log = (t) => inst.logs.push(t);

    let resolveRom;
    const Module = {
        portRomReady: new Promise((res) => { resolveRom = res; }),
        portAudioRate: 0,
        print: log,
        printErr: log,
        portPresent() {
            if (inst.dead)
                return;
            const f = ++inst.frames;
            const game = Module._PortFrameNumber();

            /* A joiner does not connect at boot: the room would seat it as
             * peer 2 while beta still holds peer 1, and this test's story
             * is gamma inheriting beta's seat.  Its driver is created at
             * join time, from the watcher. */
            if (f === 100 && !opts.joiner) {
                inst.driver = createRb({
                    Module,
                    url: `ws://127.0.0.1:${relay.port}/rbtest?id=${name}`,
                    log,
                });
            }

            if (game < F0) {
                Module._PortSetKeys(bootMask(f));
                return;
            }

            if (opts.joiner && !inst.joinStarted) {
                /* Hold the frame loop here -- the game suspends inside
                 * PortAwaitAnimationFrame -- until the test says join. */
                inst.paused = true;
                return;
            }

            if (!opts.joiner && game === F0) {
                if (!inst.driver.start(DEPTH, 2))
                    throw new Error(`${name}: start failed`);
                log(`[test] ${name}: session started at frame ${game}, ` +
                    `peer ${inst.driver.state.slot}`);
            }

            if (inst.driver && inst.driver.state.active
                && !Module._PortRbCatchingUp()) {
                if (opts.joiner && !inst.seatRequested) {
                    inst.seatRequested = true;
                    const seat = inst.driver.requestSeat();
                    log(`[test] ${name}: caught up at frame ${game}, ` +
                        `asked for slot ${seat}`);
                }
                inst.driver.frame(PLAY[name](game + 1));
            }

            for (const target of [CHECK1, CHECK2]) {
                if (game === target) {
                    inst.checkpoints[target] = desyncQuantity(Module);
                    Module._PortRbReport();
                }
            }
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            resolveRom();
        },
    };
    inst.Module = Module;

    /* A pausable rAF for this instance only. */
    const realRaf = global.requestAnimationFrame;
    Module.requestAnimationFrame = null;    /* documentation; emscripten uses global */
    createKatam(Module);
    return inst;
}

// Pausing one instance while a global rAF shim serves all three.  The park
// decision must happen at REGISTRATION time, not at fire time: an instance
// that sets `paused` inside its portPresent registers its next rAF in the
// same synchronous wasm stack, so the very next registration after a pause
// request is that instance's own -- whereas by fire time the queue holds
// everyone's callbacks and the wrong instance gets frozen (measured: alpha
// parked at 2200 while gamma ran free on borrowed callbacks).
const instances = [];
const baseRaf = global.requestAnimationFrame;
global.requestAnimationFrame = (cb) => {
    const owner = instances.find((i) => i.paused && !i.resume);
    if (owner) {
        owner.resume = cb;
        return;
    }
    baseRaf(cb);
};

const a = makeInstance('alpha', {});
const b = makeInstance('beta', {});
instances.push(a, b);
const c = makeInstance('gamma', { joiner: true });
instances.push(c);

let bClosed = false;
let cJoinKicked = false;
const started = Date.now();

const watch = setInterval(() => {
    const gameA = a.Module._PortFrameNumber ? a.Module._PortFrameNumber() : 0;
    const gameB = b.dead ? Infinity : (b.Module._PortFrameNumber ? b.Module._PortFrameNumber() : 0);

    /* beta leaves. */
    if (!bClosed && gameA >= B_LEAVES && gameB >= B_LEAVES) {
        bClosed = true;
        console.log(`[test] beta leaving at ~frame ${B_LEAVES}`);
        b.driver.close();
        b.dead = true;
    }

    /* gamma joins. */
    if (bClosed && !cJoinKicked && gameA >= C_JOINS_AT) {
        cJoinKicked = true;
        c.joinStarted = true;
        console.log(`[test] gamma joining at alpha frame ${gameA}`);
        c.driver = createRb({
            Module: c.Module,
            url: `ws://127.0.0.1:${relay.port}/rbtest?id=gamma`,
            log: (t) => c.logs.push(t),
        });
        c.driver.join(DEPTH, 2).then(() => {
            c.paused = false;
            if (c.resume) {
                const r = c.resume;
                c.resume = null;
                r();
            }
        }).catch((e) => {
            console.error('[test] gamma join failed:', e);
            process.exit(1);
        });
    }

    /* verdicts */
    const c1a = a.checkpoints[CHECK1], c1b = b.checkpoints[CHECK1];
    const c2a = a.checkpoints[CHECK2], c2c = c.checkpoints[CHECK2];
    if (c1a !== undefined && c1b !== undefined && !watch.v1) {
        watch.v1 = true;
        console.log(`[test] frame ${CHECK1}:\n  alpha=${c1a}\n  beta =${c1b} ` +
                    (c1a === c1b ? '(EQUAL)' : '(DESYNC)'));
        if (c1a !== c1b)
            fail('alpha and beta desynced');
    }
    if (c2a !== undefined && c2c !== undefined && !watch.v2) {
        watch.v2 = true;
        console.log(`[test] frame ${CHECK2}:\n  alpha=${c2a}\n  gamma=${c2c} ` +
                    (c2a === c2c ? '(EQUAL)' : '(DESYNC)'));
        if (c2a !== c2c)
            fail('gamma joined but does not agree with alpha');
    }
    if (watch.v1 && watch.v2) {
        clearInterval(watch);
        a.Module._PortRbReport();
        c.Module._PortRbReport();
        for (const line of a.logs.slice(-12).concat(c.logs.slice(-12)))
            if (line.includes('rollback') || line.includes('[rb-net]') || line.includes('[test]'))
                console.log('  ' + line);
        console.log('NETPLAY ROLLBACK TEST PASSED: two founders in sync, one ' +
                    'departed to the AI, one joined from the log and agrees');
        relay.close();
        process.exit(0);
    }
    if (gameA >= END || Date.now() - started > 240000)
        fail(`timeout: alpha at ${gameA}, checkpoints ` +
             JSON.stringify([a.checkpoints, b.checkpoints, c.checkpoints]));
}, 100);

function fail(why) {
    clearInterval(watch);
    console.error('[test] FAIL:', why);
    for (const inst of [a, b, c]) {
        console.error(`--- ${inst.name} (frame ${inst.frames}, game ` +
            `${inst.Module._PortFrameNumber ? inst.Module._PortFrameNumber() : '?'}) ---`);
        console.error(inst.logs.slice(-20).map((l) => `  ${l}`).join('\n'));
    }
    console.error('NETPLAY ROLLBACK TEST FAILED');
    relay.close();
    process.exit(1);
}
