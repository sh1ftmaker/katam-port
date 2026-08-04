// Headless smoke test: boot the port under node, run frames, and report what
// the game actually did.
//
// This is the only way to answer "does it get past GameInit" without a
// browser.  It stands in for the two things the page provides -- the ROM and
// requestAnimationFrame -- then watches the framebuffer.
//
//   node tools/headless_test.js <katam-node.js> <rom.gba> [frames]
//
// Writes the last frame to build/frame.ppm and prints a summary: how many
// distinct colours appeared, which display mode the game selected, and every
// diagnostic the port logged (missing functions, unimplemented hardware).

const fs = require('fs');
const path = require('path');

const [, , modulePath, romPath, framesArg] = process.argv;
if (!modulePath || !romPath) {
    console.error('usage: node headless_test.js <katam-node.js> <rom.gba> [frames]');
    process.exit(2);
}
const TARGET_FRAMES = parseInt(framesArg || '180', 10);
const SAVE_AT = (process.env.SAVE_AT || '').split(',').filter(Boolean).map(Number);
const PRESSES = (process.env.PRESS_AT || '').split(',').filter(Boolean)
    .map((s) => s.split(':').map(Number));

function savePpm(data, w, h, path) {
    const rgb = Buffer.alloc(w * h * 3);
    for (let i = 0, j = 0; i < data.length; i += 4, j += 3) {
        rgb[j] = data[i]; rgb[j + 1] = data[i + 1]; rgb[j + 2] = data[i + 2];
    }
    fs.mkdirSync('build', { recursive: true });
    fs.writeFileSync(path, Buffer.concat([Buffer.from(`P6\n${w} ${h}\n255\n`), rgb]));
}

// Refuse to run a module older than the objects it was built from.  Twice now
// a failed link has left the previous binary in place and I have diagnosed a
// build I did not make, reporting a fix that was really a stale artefact.
(function checkFresh() {
    const wasm = modulePath.replace(/\.js$/, '.wasm');
    if (!fs.existsSync(wasm)) return;
    const built = fs.statSync(wasm).mtimeMs;
    let newest = 0, newestName = '';
    const walk = (dir) => {
        let entries = [];
        try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (e) { return; }
        for (const e of entries) {
            const full = path.join(dir, e.name);
            if (e.isDirectory()) walk(full);
            else if (e.name.endsWith('.o')) {
                const m = fs.statSync(full).mtimeMs;
                if (m > newest) { newest = m; newestName = full; }
            }
        }
    };
    walk('build/obj');
    if (newest > built + 1000) {
        console.error('STALE: %s was built before %s.', wasm, newestName);
        console.error('       Rebuild it; the run would have tested the wrong binary.');
        process.exit(3);
    }
})();

const rom = fs.readFileSync(romPath);
const logs = [];
let frames = 0;
let lastFrame = null;
let firstNonBlankFrame = -1;

// The port drives its own pacing with requestAnimationFrame; node has none.
global.requestAnimationFrame = (cb) => setTimeout(cb, 0);

let resolveRom;
const Module = {
    portRomReady: new Promise((res) => { resolveRom = res; }),

    print: (t) => logs.push(t),
    printErr: (t) => logs.push(t),

    portPresent(ptr, w, h) {
        // Copy out of the heap: the view is a Uint8Array, and it can be
        // detached under us between frames.
        const data = Buffer.from(Module.HEAPU8.subarray(ptr, ptr + w * h * 4));
        lastFrame = { data, w, h };

        if (firstNonBlankFrame < 0 && !isBlank(data)) firstNonBlankFrame = frames;
        // A trace of what the display hardware was asked to do, so a blank
        // screen can be told apart from a screen that was never set up.
        // Keep a few frames along the way, so the boot sequence can be seen
        // rather than just its final state.
        // Scripted input, so the controls can be exercised without a browser:
        // PRESS_AT="1300:8,1310:0" holds Start at frame 1300, releases at 1310.
        for (const [at, mask] of PRESSES)
            if (at === frames) Module._PortSetKeys(mask);
        if (SAVE_AT.includes(frames)) savePpm(data, w, h, 'build/frame-' + frames + '.ppm');
        if (frames % 60 === 0) {
            const io = (off) => Module.HEAPU8[0x04000000 + off]
                              | (Module.HEAPU8[0x04000001 + off] << 8);
            const colours = new Set();
            for (let i = 0; i < data.length; i += 4) colours.add(data.readUInt32LE(i));
            const pal = new Set();
            for (let i = 0; i < 0x400; i += 2)
                pal.add(Module.HEAPU8[0x05000000 + i] | (Module.HEAPU8[0x05000001 + i] << 8));
            let vram = 0;
            for (let i = 0; i < 0x18000; i++) if (Module.HEAPU8[0x06000000 + i]) vram++;
            // The game's own state, at the addresses linker.ld gives them --
            // which the port reproduces exactly, so they can just be read.
            const u32 = (a) => Module.HEAPU8[a] | (Module.HEAPU8[a+1] << 8)
                             | (Module.HEAPU8[a+2] << 16) | (Module.HEAPU8[a+3] << 24);
            console.log('frame %d: DISPCNT=%s colours=%d palette=%d vram-nonzero=%d '
                        + 'mainFlags=%s numTasks=%d curTask=%s gate=%s frameCount=%d',
                        frames, io(0).toString(16).padStart(4, '0'),
                        colours.size, pal.size, vram,
                        (u32(0x03002440) >>> 0).toString(16),
                        u32(0x03002e7c),
                        (u32(0x030035d0) >>> 0).toString(16),
                        (u32(0x030035d4) >>> 0).toString(16),
                        u32(0x03002e64));
        }
        if (++frames >= TARGET_FRAMES) finish();
    },

    onRuntimeInitialized() {
        // Runs before main(), which is what lets the ROM be in place by the
        // time the game asks for it.
        // The ROM has to be at its real GBA address before the game starts:
        // every pointer it follows into ROM is an absolute 0x08xxxxxx value.
        const base = 0x08000000;
        if (Module.HEAPU8.length < base + rom.length) {
            console.error('FAIL: heap is %d bytes, ROM needs to sit at 0x%s',
                          Module.HEAPU8.length, (base + rom.length).toString(16));
            process.exit(1);
        }
        Module.HEAPU8.set(rom, base);
        Module._PortRomLoaded(rom.length);
        resolveRom();
    },
};

function isBlank(px) {
    const first = px.readUInt32LE(0);
    for (let i = 4; i < px.length; i += 4)
        if (px.readUInt32LE(i) !== first) return false;
    return true;
}

function finish() {
    const { data, w, h } = lastFrame;
    const colours = new Set();
    for (let i = 0; i < data.length; i += 4) colours.add(data.readUInt32LE(i));

    // PPM: no encoder needed, and any image viewer opens it.
    const header = Buffer.from(`P6\n${w} ${h}\n255\n`);
    const rgb = Buffer.alloc(w * h * 3);
    for (let i = 0, j = 0; i < data.length; i += 4, j += 3) {
        rgb[j] = data[i]; rgb[j + 1] = data[i + 1]; rgb[j + 2] = data[i + 2];
    }
    fs.mkdirSync('build', { recursive: true });
    fs.writeFileSync('build/frame.ppm', Buffer.concat([header, rgb]));

    const dispcnt = Module.HEAPU8[0x04000000] | (Module.HEAPU8[0x04000001] << 8);

    console.log('--- katam-port headless smoke test ---');
    console.log('frames rendered      : %d', frames);
    console.log('first non-blank frame: %s',
                firstNonBlankFrame < 0 ? 'none -- screen never changed'
                                       : firstNonBlankFrame);
    console.log('distinct colours     : %d (last frame)', colours.size);
    console.log('REG_DISPCNT          : 0x%s  (mode %d, BG %s, OBJ %s)',
                dispcnt.toString(16).padStart(4, '0'), dispcnt & 7,
                ((dispcnt >> 8) & 15).toString(2).padStart(4, '0'),
                (dispcnt & 0x1000) ? 'on' : 'off');
    console.log('last frame written to: build/frame.ppm');
    console.log('\nport diagnostics (%d):', logs.length);
    for (const l of logs) console.log('  ' + l);
    process.exit(0);
}

// Post-mortem. A trap unwinds out through the Asyncify resume, so it arrives
// here rather than at any call this script made. Dumping the task table at
// that moment is usually more informative than the stack: the tasks carry the
// function pointers the game is about to call, and a value in ROM range is
// proof that something ARM-shaped leaked into one.
process.on('uncaughtException', function (err) {
    console.error('\nTRAP: ' + (err && err.message));
    // The stack is the point; printing the dump without it was a own-goal.
    if (err && err.stack) console.error(err.stack.split('\n').slice(0, 12).join('\n'));
    try {
        const H = Module.HEAPU8;
        const u16 = (a) => H[a] | (H[a + 1] << 8);
        const u32 = (a) => (H[a] | (H[a+1] << 8) | (H[a+2] << 16) | (H[a+3] << 24)) >>> 0;
        console.error('gNumTasks = %d   gCurTask = 0x%s',
                      u32(0x03002E7C) | 0, u32(0x030035D0).toString(16));
        if (u32(0x09800000) === 0x44544F52) {
            console.error('last destructor call before the trap:');
            console.error('  task=0x%s dtor=0x%s main=0x%s flags=0x%s prio=%d '
                          + 'structOffset=0x%s parent=%d  (%d calls)',
                u32(0x09800004).toString(16), u32(0x09800008).toString(16),
                u32(0x0980000c).toString(16),
                (u32(0x09800010) & 0xffff).toString(16), u32(0x09800010) >>> 16,
                (u32(0x09800014) & 0xffff).toString(16), u32(0x09800014) >>> 16,
                u32(0x09800018));
        }
        console.error('task table at 0x030019F0 (index: main dtor priority flags):');
        let shown = 0;
        for (let i = 0; i < 0x80 && shown < 24; i++) {
            const base = 0x030019F0 + i * 0x14;
            const main = u32(base + 8), dtor = u32(base + 12);
            const prio = u16(base + 16), flags = u16(base + 18);
            if (!main && !dtor && !flags) continue;
            const romish = (v) => (v >>> 24) === 0x08 ? '  <-- ROM ADDRESS' : '';
            console.error('  %d: main=0x%s dtor=0x%s prio=%d flags=0x%s%s%s',
                i, main.toString(16), dtor.toString(16), prio, flags.toString(16),
                romish(main), romish(dtor));
            shown++;
        }
    } catch (e) { console.error('(could not read the heap: ' + e.message + ')'); }
    process.exit(1);
});

// A hard stop, in case the game never reaches a frame at all.
setTimeout(() => {
    console.error('TIMEOUT: only %d frames in 60s', frames);
    if (logs.length) console.error(logs.join('\n'));
    process.exit(1);
}, 60000);

// The module is built with MODULARIZE, so it takes our object as the base of
// its own Module rather than us hoping a global gets picked up.
const createKatam = require(path.resolve(modulePath));
createKatam(Module).catch((e) => {
    console.error('module failed to start:', e);
    process.exit(1);
});
