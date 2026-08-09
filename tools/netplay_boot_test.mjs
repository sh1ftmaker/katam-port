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
// PASS: the joiner is seated in slot 1 on both instances' maps, and the
// game's own desync quantity is bit-equal on both at the same frame.

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

function makeInstance(name, keyScript) {
    const inst = { name, logs: [], Module: null, session: null,
                   checkpoint: null, seatedAt: 0 };
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
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            const driver = createRb({
                Module,
                url: `ws://127.0.0.1:${relay.port}/boottest?id=${name}`,
                log,
            });
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
const started = Date.now();

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
        clearInterval(watch);
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
        console.log(`[test] camera focus: host on Kirby ${focusH}, ` +
                    `joiner on Kirby ${focusJ}`);
        if (eq && seatH === 1 && seatJ === 1 && focusH === 0 && focusJ === 1) {
            console.log('BOOT-TO-WORLD TEST PASSED: the joiner synchronised to the ' +
                        "host's world and took over an existing Kirby");
            relay.close();
            process.exit(0);
        }
        console.error('BOOT-TO-WORLD TEST FAILED');
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
