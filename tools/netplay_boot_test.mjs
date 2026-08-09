// The page's Path B flow, end to end, headless: boot-to-world and
// inhabiting an existing Kirby.
//
//   node tools/netplay_boot_test.mjs build/katam-node.js <rom.gba>
//
// Drives web/rb_boot.js + web/rb_net.js exactly as the page does -- no
// scripted F0 rendezvous in the test itself; the boot script, the
// activation events and the join choreography all come from rb_boot.js.
//
//   the HOST begins on an empty room: its own boot replays with the
//   picture off (the "boot to world" fast-forward), it seats itself as
//   Kirby 0 and plays.
//
//   the JOINER begins minutes of frames later: replays the same boot plus
//   the host's whole session history in one catch-up, closes the live gap,
//   asks for a seat, and takes over an AI Kirby in place.
//
//   then the JOINER'S SOCKET DIES mid-play (act two).  The host must
//   freeze at the rollback window -- never simulate past what the absent
//   peer confirmed -- the driver must reconnect and resynchronise, and the
//   two must be bit-equal again afterwards.  A reconnect is a pause, not a
//   desync.
//
// PASS: the joiner is seated in slot 1 on both instances' maps, the
// game's own desync quantity is bit-equal on both at the same frame, the
// host stalled while the joiner was dark, and both are bit-equal again
// after the recovery.

import { createRequire } from 'module';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

const [, , modulePath, romPath] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node tools/netplay_boot_test.mjs <katam-node.js> <rom.gba>');
    process.exit(2);
}

const { startRelay } = await import(path.join(here, '../netplay/dev-relay.mjs'));
const createKatam = require(path.resolve(modulePath));
const createRb = require(path.join(here, '../web/rb_net.js'));
const createWorld = require(path.join(here, '../web/rb_boot.js'));
const fs = require('fs');
const rom = fs.readFileSync(romPath);

// Delivery must beat the frame loop -- docs/NETPLAY.md §3a item 4.
global.requestAnimationFrame = (cb) => setTimeout(cb, 1);

const relay = await startRelay(0, { quiet: true });

function desyncQuantity(Module) {
    const dv = new DataView(Module.HEAPU8.buffer);
    const parts = [dv.getUint32(0x030068D8, true)];
    for (let i = 0; i < 4; i++) {
        parts.push(dv.getInt32(0x02020EE0 + 424 * i + 64, true));
        parts.push(dv.getInt32(0x02020EE0 + 424 * i + 68, true));
    }
    return parts.join(',');
}

let checkpointFrame = 0;            /* set once the joiner is seated         */
let checkpoint2Frame = 0;           /* set after the blip recovery           */

function makeInstance(name, keyScript) {
    const inst = { name, logs: [], Module: null, session: null, driver: null,
                   checkpoint: null, checkpoint2: null, seatedAt: 0 };
    const log = (t) => inst.logs.push(`[${name}] ${t}`);

    let resolveRom;
    const Module = {
        portRomReady: new Promise((res) => { resolveRom = res; }),
        portAudioRate: 0,
        print: (t) => inst.logs.push(t),
        printErr: (t) => inst.logs.push(t),
        portPresent() {
            const game = Module._PortFrameNumber();
            /* The page's input path, exactly: physical buttons in through
             * PortSetKeys, sampled back out by rb_boot's default getKeys
             * (PortCurrentKeys).  Overriding getKeys with the script here
             * would skip the one seam where the rollback engine's timeline
             * keys can clobber the player's -- which it did, in every
             * browser, while this test kept passing. */
            Module._PortSetKeys(keyScript(game));
            inst.session.tick();
            if (inst.session.state.phase === 'playing' && !inst.seatedAt)
                inst.seatedAt = game;
            if (checkpointFrame && game === checkpointFrame)
                inst.checkpoint = desyncQuantity(Module);
            if (checkpoint2Frame && game === checkpoint2Frame)
                inst.checkpoint2 = desyncQuantity(Module);
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            const driver = createRb({
                Module,
                url: `ws://127.0.0.1:${relay.port}/boottest?id=${name}`,
                log,
            });
            Module.portNetIdle = () => driver.idle();
            inst.driver = driver;
            inst.session = createWorld({
                Module, driver, log,
                onStatus: (t) => { console.log(`[${name}] ${t}`); },
            });
            /* The page holds the game at frame 0 by chaining portRomReady;
             * here begin() runs before the ROM promise resolves, which is
             * the same guarantee. */
            inst.session.begin().then(() => resolveRom())
                .catch((e) => { console.error(`[${name}] begin failed:`, e); process.exit(1); });
        },
    };
    inst.Module = Module;
    createKatam(Module);
    return inst;
}

// Host: walks right for 150 frames a little after the world goes live.
const host = makeInstance('host',
    (f) => (f >= 2350 && f < 2500) ? 0x10 : 0);

let joiner = null;
let phase = 'host-boot';
const blip = { hostAtCut: 0, cutAt: 0 };
const started = Date.now();

/* While the blip lasts, reconnect attempts land here: a socket that never
 * opens and reports closed 50 ms later, so the driver's backoff runs but
 * no data moves.  Swapped in for global.WebSocket for the dark window. */
const RealWebSocket = global.WebSocket;
function BlackholeSocket() {
    const self = this;
    self.readyState = 0;
    setTimeout(() => {
        self.readyState = 3;
        if (self.onclose) self.onclose();
    }, 50);
}
BlackholeSocket.prototype.close = function () {};
BlackholeSocket.prototype.send = function () {};

const watch = setInterval(() => {
    if (phase === 'host-boot' && host.session
        && host.session.state.phase === 'playing'
        && host.Module._PortFrameNumber() >= 2600) {
        phase = 'joining';
        console.log(`[test] host live at frame ${host.Module._PortFrameNumber()}; ` +
                    'starting the joiner');
        joiner = makeInstance('joiner', (f) =>
            (joiner && joiner.seatedAt && f >= joiner.seatedAt + 50
                && f < joiner.seatedAt + 150) ? 0x20 : 0);
    }

    if (phase === 'joining' && joiner && joiner.seatedAt) {
        phase = 'checking';
        /* Inputs are quiet again 150 frames after the seat; compare well
         * past that, at the same frame on both. */
        checkpointFrame = joiner.seatedAt + 400;
        console.log(`[test] joiner seated at frame ${joiner.seatedAt}; ` +
                    `checkpoint at ${checkpointFrame}`);
    }

    if (phase === 'checking' && host.checkpoint && joiner.checkpoint) {
        const eq = host.checkpoint === joiner.checkpoint;
        console.log(`[test] frame ${checkpointFrame}:`);
        console.log(`  host  =${host.checkpoint}`);
        console.log(`  joiner=${joiner.checkpoint} ${eq ? '(EQUAL)' : '(DESYNC)'}`);
        const seatH = host.Module._PortRbSlotPeer(1);
        const seatJ = joiner.Module._PortRbSlotPeer(1);
        console.log(`[test] slot 1 is peer ${seatH} on host, ${seatJ} on joiner`);
        /* Whose Kirby does each camera follow?  gUnk_0203AD3C is inside the
         * snapshots, so a joiner's rollbacks used to restore the pre-seat
         * zero and hand its camera to the host's Kirby for good. */
        const focusH = host.Module.HEAPU8[0x0203AD3C];
        const focusJ = joiner.Module.HEAPU8[0x0203AD3C];
        /* Which viewport actually drives each screen: the display task's
         * player id, captured from gLocalPlayerId at world creation and
         * re-pointed by the engine when a joiner seats.  gUnk_02023354 ->
         * struct Task -> struct offset (task.h's TaskGetStructPtr). */
        const displayed = (Module) => {
            const dv = new DataView(Module.HEAPU8.buffer);
            const task = dv.getUint32(0x02023354, true);
            if (!task) return -1;
            const off = dv.getUint16(task + 6, true);
            const flags = dv.getUint16(task + 0x12, true);
            return Module.HEAPU8[(flags & 0x10)
                ? 0x02000000 + (off << 2) : 0x03000000 + off];
        };
        const dispH = displayed(host.Module);
        const dispJ = displayed(joiner.Module);
        console.log(`[test] camera focus: host on Kirby ${focusH} ` +
                    `(viewport ${dispH}), joiner on Kirby ${focusJ} ` +
                    `(viewport ${dispJ})`);
        if (!(eq && seatH === 1 && seatJ === 1 && focusH === 0 && focusJ === 1
              && dispH === 0 && dispJ === 1)) {
            clearInterval(watch);
            console.error('BOOT-TO-WORLD TEST FAILED (act one)');
            relay.close();
            process.exit(1);
        }
        /* Act two: the blip.  Kill the joiner's socket without telling the
         * driver (st.closed stays false, so it reconnects on its own).
         * The host must freeze at the rollback window rather than predict
         * its way into a silent desync, and after the reconnect + resync
         * the two must agree bit for bit again. */
        console.log("[test] act one good -- cutting the joiner's socket");
        blip.hostAtCut = host.Module._PortFrameNumber();
        blip.cutAt = Date.now();
        global.WebSocket = BlackholeSocket;
        joiner.driver.socket.close();
        phase = 'blip-dark';
    }

    if (phase === 'blip-dark' && Date.now() - blip.cutAt > 2000) {
        const advance = host.Module._PortFrameNumber() - blip.hostAtCut;
        console.log(`[test] host advanced ${advance} frame(s) in 2 s of ` +
                    'peer darkness (window is 16)');
        global.WebSocket = RealWebSocket;
        if (advance > 60) {
            clearInterval(watch);
            console.error('BOOT-TO-WORLD TEST FAILED: the host did not stall ' +
                          'at the rollback window -- that is a silent desync');
            relay.close();
            process.exit(1);
        }
        phase = 'blip-recover';
    }

    if (phase === 'blip-recover'
        && host.Module._PortFrameNumber() > blip.hostAtCut + 240) {
        checkpoint2Frame = Math.max(host.Module._PortFrameNumber(),
                                    joiner.Module._PortFrameNumber()) + 200;
        console.log(`[test] recovered -- second checkpoint at ${checkpoint2Frame}`);
        phase = 'blip-check';
    }

    if (phase === 'blip-check' && host.checkpoint2 && joiner.checkpoint2) {
        clearInterval(watch);
        const eq2 = host.checkpoint2 === joiner.checkpoint2;
        console.log(`[test] frame ${checkpoint2Frame} (after the blip):`);
        console.log(`  host  =${host.checkpoint2}`);
        console.log(`  joiner=${joiner.checkpoint2} ${eq2 ? '(EQUAL)' : '(DESYNC)'}`);
        if (eq2) {
            console.log('BOOT-TO-WORLD TEST PASSED: the joiner synchronised, ' +
                        'took over a Kirby, survived a socket loss, and ' +
                        'agrees again');
            relay.close();
            process.exit(0);
        }
        console.error('BOOT-TO-WORLD TEST FAILED (post-blip desync)');
        relay.close();
        process.exit(1);
    }

    if (Date.now() - started > 240000) {
        clearInterval(watch);
        for (const inst of [host, joiner].filter(Boolean)) {
            console.error(`--- ${inst.name}: phase=${inst.session && inst.session.state.phase} ` +
                          `game=${inst.Module._PortFrameNumber && inst.Module._PortFrameNumber()}`);
            console.error(inst.logs.slice(-20).map((l) => `  ${l}`).join('\n'));
        }
        console.error('BOOT-TO-WORLD TEST FAILED (timeout)');
        relay.close();
        process.exit(1);
    }
}, 100);
