// run_node.js -- one device of the determinism audit, under node.
//
//   node tools/netaudit/run_node.js CONFIG.json
//
// Replays a recording through build/katam-node.js and writes, for every frame,
// the raw whole-image hashes platform/audit.c takes at the top of the frame
// (the rollback snapshot point): image ewram iwram io pltt vram oam sim, then
// (not part of the image) sram iocanon dmahost ppuhost.  Raw dumps of the
// whole image at chosen frames go to DUMP_DIR/F.kaud.
//
// The config names ONE device: everything that could differ between two
// phones or browsers running the same wasm build, so tools/netaudit/matrix.py
// can flip one thing at a time.  Keys (all optional except module/rom/replay):
//
//   module, rom, replay, out, frames, dumps:[F...], dumpDir, log
//   menus        true: a `location` exists (as in any browser), so the in-game
//                menus (PortMenuPoc) are on; "0": ?menus=0; false: no location
//                (node's default: menus off)
//   audio        "off" | {rate, quantum, live, jitter, seed}
//                  off      no device (pcmSamplesPerVBlank 1)
//                  rate     the device rate the context reports
//                  quantum  the device drains in chunks of this many frames
//                  live     the context reports "running" (the browser's
//                           AudioTopUp path in platform/web/host_web.c runs)
//                  jitter   0..1: random extra/short drain per vsync
//   pace         {dts:[ms...]} the virtual clock's step per frame, cycled;
//                [16.74] is a 59.73 Hz device, [33.5, 0] a 30 Hz one that
//                runs two frames per vsync.  Drives performance.now/Date.now
//                and the audio drain.
//   renderOff    "all" | {every:N, phase:P} | {burst:N, period:M}
//                frames on which PortSetRenderEnabled(0), as replayed or
//                caught-up frames would run
//   p2Input      0 | 1   PortLmpSetP2Input (touch-only device: 0)
//   storage      { key: value } localStorage contents at start ("katam.settings")
//   sram         path of a save file to restore at boot (portSramRestore)
//   clock        {offsetMs, rate}  the wall clock's origin and rate
//   random       seed for Math.random (null: node's own)
//   mallocShift  bytes to malloc (and keep) before the game starts, so every
//                later malloc returns a different address
//   heapJunk     fill free heap below the ROM with this byte before boot
//   viewPin      "native" | "session" (PortSimViewPinNative / UseSession at 0)
//   viewMode     [F, mode, padX, padY, cull, pin, tile]  PortSetViewMode at F
//   sessionViews [[F, W, H], ...]  PortInputLogScheduleView (with "session")
//   join         {frame, image, mask}  load a KAUD dump's parts at that frame
//   keysAt       [[F, mask], ...] PortSetKeys calls (host keys; replay ignores)
'use strict';
const fs = require('fs');
const path = require('path');

const logLines = [];
const cfg = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
// The port's own diagnostics go straight to console.log/error (PortConsole):
// keep them in the meta file rather than on the terminal.
const realErr = console.error.bind(console);
console.log = (...a) => logLines.push(a.join(' '));
console.error = (...a) => logLines.push(a.join(' '));
const out = cfg.out;
const dumpDir = cfg.dumpDir || path.dirname(out);
let frames = 0;
let target = cfg.frames || 0;

// --- a seeded PRNG for jitter/random ------------------------------------------
function mulberry(a) {
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
if (cfg.random !== undefined && cfg.random !== null) Math.random = mulberry(cfg.random);

// --- virtual clock --------------------------------------------------------------
const clock = cfg.clock || {};
const dts = (cfg.pace && cfg.pace.dts) || [1000 / 59.7275];
let vt = 0;                         // virtual ms since start
const realNow = performance.now.bind(performance);
const t0 = realNow();
const nowMs = () => (clock.offsetMs || 0) + vt * (clock.rate || 1);
performance.now = nowMs;
const realDateNow = Date.now;
const date0 = realDateNow();
Date.now = () => Math.floor(date0 + nowMs());

// --- browser-ish globals ---------------------------------------------------------
if (cfg.menus === true) global.location = { search: '' };
else if (cfg.menus === '0') global.location = { search: '?menus=0' };
const store = Object.assign({}, cfg.storage || {});
const storageWrites = [];
global.localStorage = {
    getItem: (k) => (k in store ? store[k] : null),
    setItem: (k, v) => { store[k] = String(v); storageWrites.push([frames, k, String(v)]); },
    removeItem: (k) => { delete store[k]; },
};

// --- the audio device model --------------------------------------------------
const audio = cfg.audio === undefined ? 'off' : cfg.audio;
const audioStats = { pushes: 0, frames: 0, underruns: 0, maxDepth: 0, topUps: 0 };
let device = null;
if (audio !== 'off') {
    const rnd = mulberry(audio.seed || 1);
    device = {
        rate: audio.rate || 48000,
        depth: 0, underruns: 0, headless: true, ready: true, pending: [],
        lastT: 0, acc: 0,
        ctx: audio.live ? { state: 'running' } : null,
        rep: audio.live ? { d: 0 } : null,
        drain() {
            const t = nowMs();
            // The device plays whole callbacks of `quantum` frames as its
            // clock (the virtual clock) passes them; jitter moves when.
            this.acc += (t - this.lastT) * this.rate / 1000;
            this.lastT = t;
            const q = audio.quantum || 128;
            let slack = audio.jitter ? (rnd() - 0.5) * 2 * audio.jitter * q : 0;
            let n = Math.max(0, Math.floor((this.acc + slack) / q) * q);
            if (n > this.acc + q) n = Math.floor(this.acc / q) * q;
            this.acc -= n;
            if (n > this.depth) { if (this.depth > 0 || audioStats.pushes > 4) this.underruns++; this.depth = 0; }
            else this.depth -= n;
        },
        queued() { this.drain(); return this.depth; },
        push(f32) {
            this.drain();
            this.depth += f32.length >> 1;
            audioStats.pushes++;
            audioStats.frames += f32.length >> 1;
            if (this.depth > audioStats.maxDepth) audioStats.maxDepth = this.depth;
            audioStats.underruns = this.underruns;
        },
    };
    device.lastT = nowMs();
}

// --- render-off schedule -----------------------------------------------------------
function renderOffAt(f) {
    const r = cfg.renderOff;
    if (!r) return false;
    if (r === 'all') return true;
    if (r.every) return (f % r.every) === (r.phase || 0);
    if (r.burst) return (f % r.period) < r.burst;
    return false;
}

const rom = fs.readFileSync(cfg.rom);
const hashLines = [];
const H = 12;
let hbuf = 0;
const viewModeAt = cfg.viewMode ? cfg.viewMode[0] : -1;
const keysAt = new Map((cfg.keysAt || []).map(([f, m]) => [f, m]));
const sramWrites = [];
let lastFrameSeen = -1;

function finish(code) {
    fs.writeFileSync(out, hashLines.join('\n') + '\n');
    const meta = {
        frames, audioStats, storageWrites, sramWrites, store,
        wallMs: realNow() - t0,
        log: logLines.filter((l) => /localmp|menu-poc|audio|renderer|refused|FAIL|error|save/i.test(l)).slice(0, 400),
    };
    fs.writeFileSync(out.replace(/\.txt$/, '') + '.meta.json', JSON.stringify(meta, null, 1));
    process.exit(code || 0);
}

let step = 0;
global.requestAnimationFrame = (cb) => {
    vt += dts[step % dts.length];
    step++;
    setImmediate(() => cb(nowMs()));
};

const Module = {
    portRomReady: null,
    portAudioRate: audio === 'off' ? 0 : (audio.rate || 48000),
    print: (t) => logLines.push(t),
    printErr: (t) => logLines.push(t),
    portDiag: (t) => logLines.push(t),
    portSramRestore: cfg.sram ? (dest, size) => {
        const b = fs.readFileSync(cfg.sram);
        Module.HEAPU8.set(b.subarray(0, Math.min(size, b.length)), dest);
        return true;
    } : undefined,
    portSramDirty: () => { if (sramWrites.length < 200) sramWrites.push(frames); },
    portPresent() {
        const f = frames;
        if (keysAt.has(f)) Module._PortSetKeys(keysAt.get(f));
        if (f === viewModeAt) {
            const v = cfg.viewMode;
            Module._PortSetViewMode(v[1], v[2], v[3], v[4], v[5], v[6]);
        }
        // The frame that runs next: render on or off.
        Module._PortSetRenderEnabled(renderOffAt(f + 1) ? 0 : 1);
        if (++frames > target) finish(0);
    },
};
if (device) Module.portAudio = device;
let resolveRom;
Module.portRomReady = new Promise((r) => { resolveRom = r; });
Module.onRuntimeInitialized = function () {
    const heap = Module.HEAPU8;
    heap.set(rom, 0x08000000);
    if (cfg.heapJunk !== undefined) {
        // Fill a big free block with junk, then give it back: later mallocs
        // that reuse it see this byte instead of zero.
        const n = 8 << 20, p = Module._malloc(n);
        if (p) { heap.fill(cfg.heapJunk & 255, p, p + n); Module._free(p); }
    }
    if (cfg.mallocShift) Module._malloc(cfg.mallocShift);
    const text = fs.readFileSync(cfg.replay);
    const p = Module._malloc(text.length + 1);
    heap.set(text, p);
    const n = Module._PortInputLogLoad(p, text.length);
    Module._free(p);
    if (n < 0) { realErr('FAIL: cannot replay ' + cfg.replay); process.exit(1); }
    if (!target) target = n;
    if (cfg.p2Input !== undefined) Module._PortLmpSetP2Input(cfg.p2Input);
    if (cfg.viewPin === 'native') Module._PortSimViewPinNative();
    if (cfg.viewPin === 'session') {
        Module._PortSimViewUseSession();
        for (const [f, w, h] of cfg.sessionViews || []) Module._PortInputLogScheduleView(f, w, h);
    }
    if (cfg.join) {
        // join: {frame, image: KAUD path, mask}: this device receives only
        // the host's image at the top of `frame`.
        const b = fs.readFileSync(cfg.join.image), jp = Module._malloc(b.length);
        heap.set(b, jp);
        Module._PortAuditLoadAt(cfg.join.frame, jp, b.length, cfg.join.mask);
        Module._free(jp);
    }
    Module._PortAuditEnable(1);
    for (const f of cfg.dumps || []) Module._PortAuditDumpAt(f);
    hbuf = Module._malloc(4 * 16);
    Module._PortSetFrameStateLog(1);
    Module.portFrameState = (frame) => {
        const fr = Module._PortAuditHashes(hbuf);
        const w = new Uint32Array(Module.HEAPU8.buffer, hbuf, H);
        let line = String(fr);
        for (let i = 0; i < H; i++) line += ' ' + (w[i] >>> 0).toString(16).toUpperCase().padStart(8, '0');
        hashLines.push(line);
        lastFrameSeen = fr;
        const dl = Module._PortAuditDumpLen();
        if (dl) {
            const dp = Module._PortAuditDumpPtr();
            fs.mkdirSync(dumpDir, { recursive: true });
            fs.writeFileSync(path.join(dumpDir, Module._PortAuditDumpFrame() + '.kaud'),
                             Buffer.from(Module.HEAPU8.subarray(dp, dp + dl)));
        }
    };
    Module._PortRomLoaded(rom.length);
    resolveRom();
};

process.on('uncaughtException', (e) => { logLines.push('FAIL: ' + (e && e.stack || e)); finish(2); });
global.Module = Module;
const factory = require(path.resolve(cfg.module));
if (typeof factory === 'function') factory(Module);
