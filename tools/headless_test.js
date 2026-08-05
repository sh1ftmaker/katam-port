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

// Getting through a menu means pressing a button over and over, which PRESS_AT
// cannot express without hundreds of entries.  MASH="600:1:6" starts at frame
// 600 and taps A (mask 1) with a six-frame period -- three down, three up.
// The release matters: the game edge-triggers menu confirmation, so a held
// button registers once and then does nothing.
const MASH = (process.env.MASH || '').split(':').filter(Boolean).map(Number);

// HOLD=0x10 keeps Right down the whole time while MASH taps A on top of it.
// Toggling both together makes Kirby stop dead every other burst, which is no
// way to cross a level.
const HOLD = parseInt(process.env.HOLD || '0', 0);

// --- link cable ------------------------------------------------------------
// MP=loopback[:players] plugs the in-process transport into the serial port at
// frame MP_AT, so the run has a before and an after and a link that comes up
// mid-run can be told apart from one that was there all along.
//
// MP_ID=n puts the game in slot n instead of slot 0, which swaps which end
// clocks the cable.  MP_SELF=1 additionally has the platform layer call the
// game's own MultiSioMain once a frame -- what GameLoop does while a link
// session is running, which a headless run cannot get to by itself.  See
// docs/MULTIPLAYER.md.
const MP = (process.env.MP || '').split(':');
const MP_PLAYERS = parseInt(MP[1] || '2', 10);
const MP_AT = parseInt(process.env.MP_AT || '60', 10);
const MP_ID = parseInt(process.env.MP_ID || '0', 10);

function mashMask(frame) {
    if (MASH.length < 3) return HOLD || null;
    const [start, mask, period, end] = MASH;
    // A fourth field stops the mashing again.  Needed for anything past the
    // first screen of a menu: mashing A picks the default option every time,
    // so reaching a second option means stopping and then steering, and
    // PRESS_AT takes over from here.
    if (frame < start || (end !== undefined && frame >= end))
        return HOLD || null;
    return HOLD | (((frame - start) % period) < (period >> 1) ? mask : 0);
}

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
    // Both object trees: release objects live in build/obj, the DWARF ones
    // DEBUG_INFO=1 produces in build/obj-g.
    walk('build/obj');
    walk('build/obj-g');
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

// --- audio -----------------------------------------------------------------
// There is no audio device here, so "is it making a sound" has to be answered
// from the samples themselves. Setting Module.portAudioRate before the module
// starts puts platform/audio_out.c into its headless mode: it skips the
// AudioContext entirely and hands every block to Module.portAudioSink instead.
//
// AUDIO=0 turns the capture off. AUDIO_RATE= picks the pretend device rate,
// which the mixer configures itself for exactly as it would in a browser.
// TONE=440 replaces the mixer's output with a square wave, which is how the
// transport gets tested without the sound engine being involved at all.
// WAV=build/audio.wav writes what was captured, so it can be listened to.
const AUDIO_RATE = process.env.AUDIO === '0' ? 0
                 : parseInt(process.env.AUDIO_RATE || '48000', 10);
const TONE = parseInt(process.env.TONE || '0', 10);
const WAV = process.env.WAV || '';
const audio = { blocks: 0, samples: 0, sumSq: 0, peak: 0, silentBlocks: 0,
                chunks: [] };

function audioSink(f32) {
    audio.blocks++;
    audio.samples += f32.length >> 1;
    let blockPeak = 0;
    for (let i = 0; i < f32.length; i++) {
        const v = f32[i];
        audio.sumSq += v * v;
        const a = Math.abs(v);
        if (a > blockPeak) blockPeak = a;
    }
    if (blockPeak > audio.peak) audio.peak = blockPeak;
    if (blockPeak === 0) audio.silentBlocks++;
    if (WAV) audio.chunks.push(Buffer.from(f32.buffer.slice(0)));
}

// 16-bit stereo PCM WAV, written by hand -- no encoder, and every player opens
// it. Only reached when WAV= is set, because a few thousand frames of 48 kHz
// stereo is a large file to write by accident.
function writeWav(path, rate) {
    const total = audio.chunks.reduce((n, c) => n + (c.length >> 2), 0);
    const pcm = Buffer.alloc(total * 2);
    let o = 0;
    for (const c of audio.chunks) {
        const f = new Float32Array(c.buffer, c.byteOffset, c.length >> 2);
        for (let i = 0; i < f.length; i++) {
            let v = Math.round(f[i] * 32767);
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            pcm.writeInt16LE(v, o); o += 2;
        }
    }
    const hdr = Buffer.alloc(44);
    hdr.write('RIFF', 0); hdr.writeUInt32LE(36 + pcm.length, 4);
    hdr.write('WAVE', 8); hdr.write('fmt ', 12);
    hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20);
    hdr.writeUInt16LE(2, 22); hdr.writeUInt32LE(rate, 24);
    hdr.writeUInt32LE(rate * 4, 28); hdr.writeUInt16LE(4, 32);
    hdr.writeUInt16LE(16, 34); hdr.write('data', 36);
    hdr.writeUInt32LE(pcm.length, 40);
    fs.mkdirSync(path.replace(/\/[^/]*$/, '') || '.', { recursive: true });
    fs.writeFileSync(path, Buffer.concat([hdr, pcm]));
}

// The port drives its own pacing with requestAnimationFrame; node has none.
global.requestAnimationFrame = (cb) => setTimeout(cb, 0);

let resolveRom;
const Module = {
    portRomReady: new Promise((res) => { resolveRom = res; }),

    portAudioRate: AUDIO_RATE,
    portAudioSink: AUDIO_RATE ? audioSink : undefined,

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
        const mash = mashMask(frames);
        if (mash !== null) Module._PortSetKeys(mash);
        // LAYERS=0b0001 renders BG0 alone, so "which layer holds the room"
        // stops being a guess.  Bits 0-3 are BG0-3, bit 4 is OBJ.
        // WATCH=0x06003FE0 names every block move that covers that address.
        if (process.env.WATCH && Module._PortSetWatch && frames === 0)
            Module._PortSetWatch(parseInt(process.env.WATCH, 0));
        if (TONE && Module._PortAudioTestTone && frames === 0)
            Module._PortAudioTestTone(TONE);
        // FORCE=0x04 draws BG2 even though the game disabled it.
        if ((process.env.LAYERS || process.env.FORCE) && Module._PortSetLayerMask)
            Module._PortSetLayerMask(parseInt(process.env.LAYERS || '0x1F', 0),
                                     parseInt(process.env.FORCE || '0', 0));
        if (MP[0] && frames === MP_AT) {
            if (MP[0] === 'loopback') {
                if (MP_ID) Module._PortMpLoopbackSelfId(MP_ID);
                Module._PortMpUseLoopback(MP_PLAYERS);
            } else if (MP[0] === 'js') {
                // The smallest transport that is not a no-op: an echo peer,
                // which puts this unit's own halfword into every other slot.
                // The game therefore receives a byte-for-byte copy of its own
                // packet from each peer, which is well framed and checksums,
                // so a link that comes up here can only mean the JavaScript
                // side of the seam works.
                Module.portMp = {
                    open: () => 1,
                    close: () => {},
                    poll: (ptr) => {
                        const h = Module.HEAPU8;
                        h[ptr] = 1; h[ptr + 1] = 0;
                        h[ptr + 2] = MP_PLAYERS; h[ptr + 3] = 0;
                    },
                    exchange: (word, ptr) => {
                        const h = new Uint16Array(Module.HEAPU8.buffer);
                        for (let i = 0; i < 4; i++)
                            h[(ptr >> 1) + i] = i < MP_PLAYERS ? word : 0xFFFF;
                        return 1;
                    },
                };
                Module._PortMpUseJs(MP_PLAYERS);
            }
            if (process.env.MP_SELF) Module._PortMpSelfTest(1);
        }
        if (SAVE_AT.includes(frames)) savePpm(data, w, h, 'build/frame-' + frames + '.ppm');
        // VRAM_AT=600 writes VRAM, both palettes and OAM to build/vram-600.bin,
        // so tile data can be looked at as pixels rather than guessed at from
        // a hex dump.  Layout: 0x18000 VRAM, 0x400 BG pal, 0x400 OBJ pal, OAM.
        if ((process.env.VRAM_AT || '').split(',').map(Number).includes(frames)) {
            const grab = (a, n) => Buffer.from(Module.HEAPU8.subarray(a, a + n));
            fs.writeFileSync('build/vram-' + frames + '.bin', Buffer.concat([
                grab(0x06000000, 0x18000), grab(0x05000000, 0x400), grab(0x07000000, 0x400)]));
        }
        if (frames % (parseInt(process.env.INTERVAL || '60', 10)) === 0) {
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
            // Per-layer state, because "the room does not draw" is answered by
            // the control registers long before it is answered by the pixels:
            // which char/screen base, which size, what priority, where scrolled.
            // The VBlank transfer queue: head, tail, and how many entries are
            // still waiting.  A queue that never empties means the port's
            // VBlank window is too small, which would leave uploads undone.
            // LIVE="0x0203AD30:1" prints addresses every diagnostic tick, as
            // opposed to DUMP= which only fires at a trap.  Same spec format.
            for (const spec of (process.env.LIVE || '').split(',').filter(Boolean)) {
                const [a, w, n, stride] = spec.split(':');
                const addr = parseInt(a, 0), width = parseInt(w || '4', 0);
                const count = parseInt(n || '1', 0);
                const step = parseInt(stride || String(width), 0);
                const rd = (p) => width === 1 ? Module.HEAPU8[p]
                    : width === 2 ? (Module.HEAPU8[p] | (Module.HEAPU8[p+1] << 8)) : u32(p);
                const out = [];
                for (let i = 0; i < count; i++) out.push('0x' + rd(addr + i * step).toString(16));
                console.log('    live %s: %s', a, out.join(' '));
            }
            if (process.env.QUEUE) {
                const head = Module.HEAPU8[0x03006078], tail = Module.HEAPU8[0x030039A4];
                let pending = (tail - head) & 0x3f, bytes = 0;
                for (let i = 0; i < pending; i++)
                    bytes += u32(0x03002EC0 + (((head + i) & 0x3f) * 12) + 8);
                console.log('    vblank queue: head=%d tail=%d pending=%d (%d bytes)',
                            head, tail, pending, bytes);
            }
            if (process.env.BG) {
                for (let bg = 0; bg < 4; bg++) {
                    const cnt = io(8 + bg * 2);
                    console.log('    BG%d cnt=%s pri=%d charBase=0x%s screenBase=0x%s '
                                + '%dbpp size=%d scroll=%d,%d %s',
                        bg, cnt.toString(16).padStart(4, '0'), cnt & 3,
                        (0x06000000 + ((cnt >> 2) & 3) * 0x4000).toString(16),
                        (0x06000000 + ((cnt >> 8) & 31) * 0x800).toString(16),
                        (cnt & 0x80) ? 8 : 4, (cnt >> 14) & 3,
                        io(0x10 + bg * 4), io(0x12 + bg * 4),
                        (io(0) & (0x100 << bg)) ? 'ON' : 'off');
                    // A map that is one repeated entry is a map that was never
                    // filled; a varied one means the failure is downstream.
                    const sb = 0x06000000 + ((cnt >> 8) & 31) * 0x800;
                    const tiles = new Set();
                    for (let i = 0; i < 0x800; i += 2)
                        tiles.add(Module.HEAPU8[sb + i] | (Module.HEAPU8[sb + i + 1] << 8));
                    const sample = [...tiles].slice(0, 6)
                        .map((t) => '0x' + t.toString(16)).join(',');
                    console.log('         map has %d distinct entries [%s%s]',
                                tiles.size, sample, tiles.size > 6 ? ',...' : '');
                }
            }
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
        // PORT_STATE_TRACE=1 makes the port emit one line per frame holding
        // the input and a hash of each region of the emulated console, for
        // diffing one build of the port against another.  It has to be turned
        // on through an export rather than the environment: this module is
        // linked -sENVIRONMENT=web, so emscripten's getenv cannot see
        // process.env.
        if (process.env.PORT_STATE_TRACE)
            Module._PortSetStateTrace(1);
        if (process.env.PORT_STATE_DETAIL)
            Module._PortSetStateDetailFrame(Number(process.env.PORT_STATE_DETAIL));
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

    // Before the summary, so the port's own report lands with the diagnostics.
    if (MP[0] && Module._PortMpReport) Module._PortMpReport();

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

    if (AUDIO_RATE) {
        // RMS in dBFS is the honest measure: a mixer that is running but
        // producing only the DC the envelope left behind reads as a tiny
        // non-zero peak, and "peak > 0" would call that a success.
        const rms = audio.samples
            ? Math.sqrt(audio.sumSq / (audio.samples * 2)) : 0;
        const db = rms > 0 ? (20 * Math.log10(rms)).toFixed(1) : '-inf';
        const expected = frames;           // one block per VBlank
        console.log('\naudio (%d Hz):', AUDIO_RATE);
        console.log('  blocks pushed      : %d  (%d frames rendered)',
                    audio.blocks, expected);
        console.log('  samples            : %d  (%ss of audio for %ss of game)',
                    audio.samples, (audio.samples / AUDIO_RATE).toFixed(2),
                    (frames / 60).toFixed(2));
        console.log('  RMS                : %s dBFS%s', db,
                    audio.blocks === 0 ? '   <-- nothing was pushed at all'
                    : rms === 0 ? '   <-- every sample was zero' : '');
        console.log('  peak               : %s', audio.peak.toFixed(4));
        console.log('  fully silent blocks: %d of %d',
                    audio.silentBlocks, audio.blocks);
        if (WAV) {
            writeWav(WAV, AUDIO_RATE);
            console.log('  written to         : %s', WAV);
        }
    }

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
        // Whatever the current investigation needs, without teaching this
        // script the game's struct layouts -- those change under us every
        // time the decompilation names another field.
        //   DUMP="0x0203AD3C:1,0x02020F40:2:4:0x1A8"
        //     address : width in bytes [: count [: stride]]
        for (const spec of (process.env.DUMP || '').split(',').filter(Boolean)) {
            const [a, w, n, stride] = spec.split(':');
            const addr = parseInt(a, 0), width = parseInt(w || '4', 0);
            const count = parseInt(n || '1', 0);
            const step = parseInt(stride || String(width), 0);
            const read = (p) => width === 1 ? H[p] : width === 2 ? u16(p) : u32(p);
            const out = [];
            for (let i = 0; i < count; i++)
                out.push('0x' + read(addr + i * step).toString(16));
            console.error('dump %s (%d x %d bytes): %s', a, count, width, out.join(' '));
        }
    } catch (e) { console.error('(could not read the heap: ' + e.message + ')'); }
    process.exit(1);
});

// A hard stop, in case the game never reaches a frame at all.  Scaled to the
// run: a fixed 60s silently truncated long menu-driving runs and made a game
// that was still going look like a game that had stopped.
const TIMEOUT_MS = Math.max(60000, TARGET_FRAMES * 40);
setTimeout(() => {
    console.error('TIMEOUT: only %d of %d frames in %ds',
                  frames, TARGET_FRAMES, TIMEOUT_MS / 1000);
    if (logs.length) console.error(logs.join('\n'));
    process.exit(1);
}, TIMEOUT_MS);

// The module is built with MODULARIZE, so it takes our object as the base of
// its own Module rather than us hoping a global gets picked up.
const createKatam = require(path.resolve(modulePath));
createKatam(Module).catch((e) => {
    console.error('module failed to start:', e);
    process.exit(1);
});
