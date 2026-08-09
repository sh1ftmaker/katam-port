// The link-session takeover, end to end: two real instances through the
// game's own lobby over the word relay, then -- the moment the game commits
// to the session -- the cable goes local and one MultiSio payload block per
// frame each way is the whole of the netplay (web/mp_net.js armTakeover,
// platform/mp_loopback.c payload mode).
//
//   node tools/netplay_link_test.mjs build/katam-node.js <rom.gba> [max frames]
//
// The old word-relay sessions died ~65 frames in, during the post-lobby
// pat2 negotiation, and kept dying under real latency after that.  This
// test therefore demands what Path A never managed: a THOUSAND frames of
// played session -- movement inputs crossing both ways -- with the game's
// own error code clean throughout.  That code is the real oracle: every
// input sample carries a 2-bit hash of gRngVal and all Kirby positions
// (sub_08030E44), so the game itself continuously compares world states,
// and a desync or a dropped cable raises gUnk_02038580 / clears the
// session flag.  Surviving the game's own referee for a thousand frames is
// the pass.

import { createRequire } from 'module';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

const [, , modulePath, romPath, maxArg] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/netplay_link_test.mjs <katam-node.js> <rom.gba> [max frames]');
    process.exit(2);
}
const MAX_FRAMES = parseInt(maxArg || '2600', 10);
const SUSTAIN_TO = 2200;            /* played session must reach this frame  */

const { startRelay } = await import(path.join(here, '../netplay/dev-relay.mjs'));
const createKatam = require(path.resolve(modulePath));
const createNet = require(path.join(here, '../web/mp_net.js'));
const fs = require('fs');
const rom = fs.readFileSync(romPath);

/* Delivery must beat the frame loop -- docs/NETPLAY.md §3a item 4. */
global.requestAnimationFrame = (cb) => setTimeout(cb, 1);

const A_SESSION = 0x03002558;       /* gUnk_03002558: link session running   */
const A_ERR = 0x02038580;           /* gUnk_02038580: the in-play error code */

/* title -> FILE 1 -> START GAME -> MULTIPLAYER, the proven script, then a
 * played session: the parent walks right, the child walks left, then both
 * idle.  Different masks on purpose -- the inputs have to cross the wire
 * for either world to be right. */
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                 [810, 0x80], [825, 0], [850, 1], [870, 0]];
function scriptMask(f, name) {
    let mask = 0;
    if (f >= 300 && f < 440)
        mask = ((f - 300) % 8) < 4 ? 1 : 0;
    else if (f >= 440)
        mask = 1;
    for (const [at, m] of PRESSES)
        if (f >= at)
            mask = m;
    if (f >= 1020)
        mask = f < 1200 && ((f - 1020) % 50) < 12 ? 1 : 0;  /* confirm taps  */
    if (f >= 1300 && f < 1600)
        mask = name === 'alpha' ? 0x10 : 0x20;              /* walk apart    */
    if (f >= 1600)
        mask = 0;
    return mask;
}

const relay = await startRelay(0, { quiet: true });

function makeInstance(name, attachAt) {
    const inst = { name, frames: 0, logs: [], net: null, Module: null,
                   sessionAt: 0, dead: false };
    const log = (t) => inst.logs.push(t);

    let resolveRom;
    const Module = {
        portRomReady: new Promise((res) => { resolveRom = res; }),
        portAudioRate: 0,
        print: log,
        printErr: log,
        portNetIdle() { if (inst.net) inst.net.idle(); },
        portPresent(ptr, w, h) {
            const f = ++inst.frames;
            inst.fb = { ptr, w, h };
            Module._PortSetKeys(scriptMask(f, name));
            if (f === attachAt) {
                inst.net = createNet({
                    Module,
                    url: `ws://127.0.0.1:${relay.port}/linktest?id=${name}`,
                    log,
                });
                if (!inst.net.attach(2))
                    throw new Error(`${name}: attach failed`);
                inst.net.armTakeover();
            }
            if (inst.net)
                inst.net.tick();
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            resolveRom();
        },
    };
    inst.Module = Module;
    createKatam(Module);
    return inst;
}

const a = makeInstance('alpha', 200);
const b = makeInstance('beta', 215);

const started = Date.now();
const watch = setInterval(() => {
    for (const inst of [a, b]) {
        const session = inst.Module.HEAPU8[A_SESSION];
        const err = inst.Module.HEAPU8[A_ERR];

        if (session && !inst.sessionAt) {
            inst.sessionAt = inst.frames;
            console.log(`[test] ${inst.name}: session up at frame ${inst.frames}`);
        }
        /* Once the session existed, losing it -- or any error code -- is
         * the game's own referee calling a foul.  That is the failure the
         * takeover exists to prevent. */
        if (inst.sessionAt && inst.frames < SUSTAIN_TO
            && (!session || err !== 0) && !inst.dead) {
            inst.dead = true;
            console.error(`[test] ${inst.name}: session ${session ? 'errored'
                : 'DIED'} at frame ${inst.frames} (err=${err}) -- ` +
                `takeover=${inst.net.state.takeover}`);
        }
    }

    if (a.dead || b.dead) {
        clearInterval(watch);
        for (const inst of [a, b])
            console.error(inst.logs.slice(-25).map((l) => `  [${inst.name}] ${l}`).join('\n'));
        console.error('LINK TAKEOVER TEST FAILED');
        relay.close();
        process.exit(1);
    }

    if (a.frames >= SUSTAIN_TO && b.frames >= SUSTAIN_TO
        && a.sessionAt && b.sessionAt) {
        clearInterval(watch);
        const ok = a.Module.HEAPU8[A_SESSION] && b.Module.HEAPU8[A_SESSION]
            && a.Module.HEAPU8[A_ERR] === 0 && b.Module.HEAPU8[A_ERR] === 0
            && a.net.state.takeover === 'live' && b.net.state.takeover === 'live';
        console.log(`[test] alpha: session since ${a.sessionAt}, ` +
                    `takeover=${a.net.state.takeover}, ` +
                    `payloads in=${a.net.state.payloadsIn} out=${a.net.state.payloadSeq}`);
        console.log(`[test] beta : session since ${b.sessionAt}, ` +
                    `takeover=${b.net.state.takeover}, ` +
                    `payloads in=${b.net.state.payloadsIn} out=${b.net.state.payloadSeq}`);
        if (a.fb && b.fb) {
            const pa = new Uint8Array(a.Module.HEAPU8.buffer, a.fb.ptr, a.fb.w * a.fb.h * 4);
            const pb = new Uint8Array(b.Module.HEAPU8.buffer, b.fb.ptr, b.fb.w * b.fb.h * 4);
            let diff = 0;
            for (let i = 0; i < pa.length; i += 4)
                if (pa[i] !== pb[i] || pa[i + 1] !== pb[i + 1] || pa[i + 2] !== pb[i + 2])
                    diff++;
            console.log(`[test] framebuffers at sample time: ${diff} of ` +
                        `${a.fb.w * a.fb.h} pixels differ`);
        }
        if (ok) {
            console.log('LINK TAKEOVER TEST PASSED: the lobby ran over the ' +
                        'wire, the session ran over payloads, and the ' +
                        "game's own referee stayed silent for a thousand " +
                        'played frames');
            relay.close();
            process.exit(0);
        }
        console.error('LINK TAKEOVER TEST FAILED (final state)');
        relay.close();
        process.exit(1);
    }

    if (a.frames >= MAX_FRAMES || b.frames >= MAX_FRAMES ||
        Date.now() - started > 300000) {
        clearInterval(watch);
        for (const inst of [a, b]) {
            console.error(`[test] ${inst.name}: frame ${inst.frames} ` +
                `session=${inst.Module.HEAPU8[A_SESSION]} ` +
                `takeover=${inst.net ? inst.net.state.takeover : 'none'}`);
            console.error(inst.logs.slice(-25).map((l) => `  [${inst.name}] ${l}`).join('\n'));
        }
        console.error('LINK TAKEOVER TEST FAILED (timeout)');
        relay.close();
        process.exit(1);
    }
}, 50);
