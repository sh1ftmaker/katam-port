// Milestone A: two real instances of the game, one relay, the game's own
// multi-cart lobby end to end.  No synthetic peer anywhere -- the far end of
// each cable is the other copy of the game, which is the thing
// docs/MULTIPLAYER.md §12 said had never existed.
//
//   node tools/netplay_test.mjs build/katam-node.js <rom.gba> [max frames]
//
// Two module instances run in one node process -- separate wasm memories, so
// the reserved-GBA-map problem does not apply -- each driven through
// title -> FILE 1 -> START GAME -> MULTIPLAYER by the same button script the
// harness runs from docs/MULTIPLAYER.md §6, talking through netplay/'s dev
// relay over real WebSockets on the loopback interface.
//
// PASS is the game's own verdict, read from its own state on both sides:
// gUnk_03002558 nonzero -- the flag GameLoop checks to run a link session --
// with gMultiBootStruct.unk01 > 1 and unk02 == 3, the lobby's completed
// handshake.  See §12 for what those mean.

import { createRequire } from 'module';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

const [, , modulePath, romPath, maxArg] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/netplay_test.mjs <katam-node.js> <rom.gba> [max frames]');
    process.exit(2);
}
const MAX_FRAMES = parseInt(maxArg || '2500', 10);

const { startRelay } = await import(path.join(here, '../netplay/dev-relay.mjs'));
const createKatam = require(path.resolve(modulePath));
const createNet = require(path.join(here, '../web/mp_net.js'));
const fs = require('fs');
const rom = fs.readFileSync(romPath);

// setTimeout(1), NOT the harness's setImmediate shim.  setImmediate makes a
// game frame the same event-loop priority as WebSocket delivery, and frames
// win the race -- so the child saw the parent's words only every other frame
// (0 words, then 32), which starves MultiSio's 8-frame validation windows
// and puts the sub-lobby state machine into a teardown loop.  A real browser
// frame takes 16 ms while loopback delivery takes ~1; a 1 ms frame keeps
// that ordering while still running 16x realtime.
global.requestAnimationFrame = (cb) => setTimeout(cb, 1);

// The game's own state, at the addresses linker.ld assigns.
const A_LOBBY_PHASE = 0x0300050C;   /* gUnk_0300050C: 1 = cartridge peers    */
const A_MB_STRUCT = 0x03000490;     /* gMultiBootStruct: unk01, unk02 at +1,+2 */
const A_SESSION = 0x03002558;       /* gUnk_03002558: link session running   */

// title -> FILE SELECT -> FILE 1 -> GAME SELECT -> START GAME -> MULTIPLAYER,
// the sequence from docs/MULTIPLAYER.md §6, plus a periodic confirm tap once
// the lobby could be settled (its own 90-frame wait plus slack) -- the parent
// needs one press of A to start the session and the child ignores it.
const PRESSES = [[480, 1], [500, 0], [560, 1], [580, 0], [720, 1], [740, 0],
                 [810, 0x80], [825, 0], [850, 1], [870, 0]];
function scriptMask(f) {
    /* Latching, exactly like the harness the sequence was tuned on: a value
     * holds until the next entry replaces it, and the mash window's *last*
     * value (pressed, at frame 439) stays held after the window closes --
     * docs/MULTIPLAYER.md §12 records that quirk, and the button timings
     * only mean what they meant under it.  A naive release at 440 adds an
     * extra A edge at 480, and one extra press picks single player. */
    let mask = 0;
    if (f >= 300 && f < 440)
        mask = ((f - 300) % 8) < 4 ? 1 : 0;             /* mash A            */
    else if (f >= 440)
        mask = 1;                                        /* last mash, latched */
    for (const [at, m] of PRESSES)
        if (f >= at)
            mask = m;
    if (f >= 1020)
        mask = f < 1200 && ((f - 1020) % 50) < 12 ? 1 : 0;  /* confirm taps  */
    return mask;
}

const relay = await startRelay(0, { quiet: false });

function makeInstance(name, attachAt) {
    const inst = {
        name, frames: 0, logs: [], net: null, Module: null,
        lobby: () => ({
            phase: inst.Module.HEAPU8[A_LOBBY_PHASE],
            peers: inst.Module.HEAPU8[A_MB_STRUCT + 1],
            state: inst.Module.HEAPU8[A_MB_STRUCT + 2],
            session: inst.Module.HEAPU8[A_SESSION],
            err: inst.Module.HEAPU8[0x02038580],    /* gUnk_02038580: the   */
        }),                                          /* in-play error code   */
        done: false,
    };
    const log = (t) => inst.logs.push(t);

    let resolveRom;
    const Module = {
        portRomReady: new Promise((res) => { resolveRom = res; }),
        portAudioRate: 0,
        print: log,
        printErr: log,
        portPresent(ptr, w, h) {
            const f = ++inst.frames;
            inst.fb = { ptr, w, h };
            Module._PortSetKeys(scriptMask(f));
            /* Log every change of the lobby's own state tuple, so a failure
             * says where the protocol died rather than only that it did. */
            if ((f % 5) === 0) {
                const s = inst.lobby();
                const h = Module.HEAPU8;
                const cnt = h[0x04000128] | (h[0x04000129] << 8);
                /* gMultiSioStatusFlags and the sub-session packet count --
                 * what sub_08031C64 is waiting on during the 60-frame
                 * window after the lobby. */
                const msio = (h[0x03002554] | (h[0x03002555] << 8) |
                             (h[0x03002556] << 16) | (h[0x03002557] << 24)) >>> 0;
                const subCnt = h[0x020382A0 + 0x28];
                /* the game's own sub-lobby over MultiSio: its state variable
                 * and what both sides' tag-2 packets carry in unkE */
                const sub04 = h[0x020382A0 + 4] | (h[0x020382A0 + 5] << 8);
                const sendE = h[0x030036B0 + 0xE];
                const recvE = [0, 1].map((u) => h[0x03002490 + 20 * u + 0xE]);
                const key = `phase=${s.phase} peers=${s.peers} state=${s.state} ` +
                            `session=${s.session} err=${s.err} siocnt=${cnt.toString(16)} ` +
                            `msio=${msio.toString(16)} sub=${subCnt} sub04=${sub04} ` +
                            `sendE=${sendE.toString(16)} recvE=${recvE.map((x) => x.toString(16))}`;
                if (key !== inst.lastState) {
                    inst.lastState = key;
                    const n = inst.net ? inst.net.state : null;
                    log(`f=${f} ${key}` + (n ? ` tx=${n.sent} rx=${n.received} st=${n.stalls} ph=${n.phantoms}` : ''));
                }
            }
            if (f === attachAt) {
                inst.net = createNet({
                    Module,
                    url: `ws://127.0.0.1:${relay.port}/lobbytest?id=${name}`,
                    log,
                });
                if (!inst.net.attach(2))
                    throw new Error(`${name}: attach failed`);
            }
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            if (process.env.PORT_MP_TRACE && Module._PortMpSetTrace)
                Module._PortMpSetTrace(1);
            resolveRom();
        },
    };
    inst.Module = Module;
    createKatam(Module);
    return inst;
}

// Staggered attach so the relay's slot assignment is deterministic: whoever
// connects first is slot 0 and clocks the cable.
const a = makeInstance('alpha', 200);
const b = makeInstance('beta', 215);

const started = Date.now();
const watch = setInterval(() => {
    for (const inst of [a, b]) {
        const s = inst.lobby();
        const humans = inst.Module.HEAPU8[0x0203AD30];
        if (s.session && humans === 2 && s.err === 0) {
            inst.sessionFrames = (inst.sessionFrames || 0) + 1;
            if (!inst.done && inst.sessionFrames === 1)
                console.log(`[test] ${inst.name}: session running with 2 humans ` +
                            `at frame ${inst.frames}`);
        }
        if (!inst.done && (inst.sessionFrames || 0) > 15) {   /* watcher ticks, ~50 frames each */
            inst.done = true;
            console.log(`[test] ${inst.name}: session SUSTAINED through frame ${inst.frames}`);
        }
    }
    if (a.done && b.done) {
        clearInterval(watch);
        const netA = a.net.state, netB = b.net.state;
        console.log(`[test] transport alpha: sent=${netA.sent} recv=${netA.received} stalls=${netA.stalls} phantoms=${netA.phantoms}`);
        console.log(`[test] transport beta : sent=${netB.sent} recv=${netB.received} stalls=${netB.stalls} phantoms=${netB.phantoms}`);
        /* Same world, same frame, two machines: compare the pictures. */
        if (a.fb && b.fb) {
            const pa = new Uint8Array(a.Module.HEAPU8.buffer, a.fb.ptr, a.fb.w * a.fb.h * 4);
            const pb = new Uint8Array(b.Module.HEAPU8.buffer, b.fb.ptr, b.fb.w * b.fb.h * 4);
            let diff = 0;
            for (let i = 0; i < pa.length; i += 4)
                if (pa[i] !== pb[i] || pa[i + 1] !== pb[i + 1] || pa[i + 2] !== pb[i + 2])
                    diff++;
            console.log(`[test] framebuffers at sample time: ${diff} of ${a.fb.w * a.fb.h} pixels differ`);
        }
        console.log('NETPLAY TEST PASSED: two real instances linked through the ' +
                    "game's own lobby and are running a two-player session");
        relay.close();
        process.exit(0);
    }
    if (a.frames >= MAX_FRAMES || b.frames >= MAX_FRAMES ||
        Date.now() - started > 300000) {
        clearInterval(watch);
        for (const inst of [a, b]) {
            const s = inst.lobby();
            console.error(`[test] ${inst.name}: frame ${inst.frames} phase=${s.phase} ` +
                          `peers=${s.peers} state=${s.state} session=${s.session}`);
            console.error(inst.logs.slice(-60).map((l) => `  [${inst.name}] ${l}`).join('\n'));
            if (inst.fb) {
                const { ptr, w, h } = inst.fb;
                const px = Buffer.from(inst.Module.HEAPU8.buffer, ptr, w * h * 4);
                const rgb = Buffer.alloc(w * h * 3);
                for (let i = 0, j = 0; i < px.length; i += 4, j += 3) {
                    rgb[j] = px[i]; rgb[j + 1] = px[i + 1]; rgb[j + 2] = px[i + 2];
                }
                fs.writeFileSync(`build/netplay_${inst.name}.ppm`,
                    Buffer.concat([Buffer.from(`P6\n${w} ${h}\n255\n`), rgb]));
                console.error(`[test] wrote build/netplay_${inst.name}.ppm`);
            }
        }
        console.error('NETPLAY LOBBY TEST FAILED');
        relay.close();
        process.exit(1);
    }
}, 50);
