// One wasm instance of the game, stepped one presented frame at a time.
//
// The node build awaits requestAnimationFrame once per presented frame
// (platform/web/host_web.c, headless branch).  A shared rAF shim routes each
// registration to the instance that made it; step() hands the instance its
// callback and resolves at its next registration, so exactly one presented
// frame (plus any replayed frames the engine runs before it) happens per
// step, on a clock the caller owns.  Two instances never run at once.
import { createRequire } from 'module';
import path from 'path';

const require = createRequire(import.meta.url);
let current = null;
globalThis.requestAnimationFrame = (cb) => {
    const inst = current;
    if (!inst) { setTimeout(() => cb(0), 0); return 0; }
    inst._raf = cb;
    if (inst._onRaf) { const r = inst._onRaf; inst._onRaf = null; r(); }
    return 1;
};

export const KEY = { A: 1, B: 2, SELECT: 4, START: 8, RIGHT: 16, LEFT: 32, UP: 64, DOWN: 128, R: 256, L: 512 };

function cstr(M, p) {
    let s = '';
    for (let i = p; M.HEAPU8[i]; i++) s += String.fromCharCode(M.HEAPU8[i]);
    return s;
}

// An audio device model on the caller's clock (ms): the browser transport's
// interface (platform/web/audio_out_web.c) -- a queue the device drains in
// callbacks of `quantum` frames at `rate`, with optional jitter; `live` makes
// the context report "running" (the page's AudioTopUp path); `drift` the
// audio clock's error against the caller's (0.0005 = 500 ppm).  null: no
// device at all (pcmSamplesPerVBlank 1 off a session).
export function audioDevice(spec, clock) {
    if (!spec) return null;
    let seed = spec.seed || 1;
    const rnd = () => { seed |= 0; seed = (seed + 0x6D2B79F5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
    const q = spec.quantum || 128;
    const dev = {
        rate: spec.rate || 48000, depth: 0, underruns: 0, headless: true, ready: true, pending: [],
        lastT: null, acc: 0, playing: false,
        ctx: spec.live ? { state: 'running' } : null, rep: spec.live ? { d: 0 } : null,
        st: { pushes: 0, frames: 0, underruns: 0, maxDepth: 0, dryFrames: 0, underrunsAfter10s: 0, dryFramesAfter10s: 0, t0: null },
        drain() {
            const t = clock();
            if (this.lastT === null) this.lastT = t;
            this.acc += Math.max(0, t - this.lastT) * this.rate * (1 + (spec.drift || 0)) / 1000;
            this.lastT = t;
            const slack = spec.jitter ? (rnd() - 0.5) * 2 * spec.jitter * q : 0;
            let n = Math.max(0, Math.floor((this.acc + slack) / q) * q);
            if (n > this.acc + q) n = Math.floor(this.acc / q) * q;
            this.acc -= n;
            if (n > this.depth) {
                if (this.st.pushes > 8) {
                    this.underruns++; this.st.underruns++; this.st.dryFrames += n - this.depth;
                    if (t - this.st.t0 > 10000) { this.st.underrunsAfter10s++; this.st.dryFramesAfter10s += n - this.depth; }
                }
                this.depth = 0;
            } else this.depth -= n;
        },
        queued() { this.drain(); return this.depth; },
        push(f32) {
            this.drain();
            if (this.st.t0 === null) this.st.t0 = clock();
            this.depth += f32.length >> 1;
            this.st.pushes++;
            this.st.frames += f32.length >> 1;
            if (this.depth > this.st.maxDepth) this.st.maxDepth = this.depth;
            if (spec.onPush) spec.onPush(f32);      // measuring: a capture of what it plays
        },
    };
    return dev;
}

export async function createInstance({ modulePath, rom, name, render = false, menus = 0, quiet = true, onLog, audio = null, clock = null, modulePre = null }) {
    const createKatam = require(path.resolve(modulePath));
    const inst = { name, logs: [], _raf: null, _onRaf: null, frames: 0 };
    const device = audioDevice(audio, clock || (() => 0));
    inst.audio = device;
    const log = (t) => {
        inst.logs.push(t);
        if (inst.logs.length > 20000) inst.logs.splice(0, 5000);
        if (onLog) onLog(name, t);
        if (!quiet) console.log(`[${name}] ${t}`);
    };
    let romReady;
    const Module = {
        portRomReady: new Promise((r) => { romReady = r; }),
        portAudioRate: device ? device.rate : 0,
        print: log, printErr: log, portDiag: log,
        portPresent() { inst.frames++; },
        onRuntimeInitialized() {
            Module.HEAPU8.set(rom, 0x08000000);
            Module._PortRomLoaded(rom.length);
            if (!render) Module._PortSetRenderEnabled(0);
            if (menus) Module._PortMenuPocSet(menus);
            romReady();
        },
    };
    if (device) Module.portAudio = device;
    if (modulePre) modulePre(Module);       // a page's own hooks, before the game runs
    inst.M = Module;
    current = inst;
    const first = new Promise((r) => { inst._onRaf = r; });
    await createKatam(Module);
    await first;
    inst.tel = new Telemetry(Module);
    return Object.assign(inst, api(inst));
}

function api(inst) {
    const M = inst.M;
    const scratch = M._malloc(512);
    const s32 = (i) => M.HEAP32[(scratch >> 2) + i];
    const u32 = (i) => M.HEAPU32[(scratch >> 2) + i];
    return {
        async step() {
            current = inst;
            const cb = inst._raf;
            inst._raf = null;
            const p = new Promise((r) => { inst._onRaf = r; });
            cb(0);
            await p;
        },
        frame: () => M._PortFrameNumber(),
        kirby(i) { M._PortLnKirby(i, scratch); return { x: s32(0), y: s32(1), room: s32(2), hp: s32(3), anim: s32(4), flags: u32(5) }; },
        observe() {
            M._PortLnObserve(scratch);
            const o = []; for (let i = 0; i < 16; i++) o.push(s32(i));
            return { interlude: o[0], pauser: o[1], menuId: o[2], numHumans: o[3], lmpHumans: o[4],
                     battery: [o[5], o[6]], lives: [o[7], o[8]], ability: [o[9], o[10]],
                     groupRoom: o[11], room: o[12], localPlayer: o[13], rng: o[14] >>> 0, modeFlags: o[15] >>> 0,
                     ovl: this.overlay() };
        },
        // this device's pause overlay (platform/lmpnet.c "the pause overlay"):
        // state 0 closed / 1 opening / 2 open, its seat and slot, the fork's
        // pauser, pause page (menuId), SETTINGS row and model, fork frame;
        // counts of opens, closes, refusals, fork frames
        overlay() {
            if (!M._PortLnOverlayObserve) return null;
            M._PortLnOverlayObserve(scratch);
            const o = []; for (let i = 0; i < 14; i++) o.push(s32(i));
            return { state: o[0], seat: o[1], slot: o[2], pauser: o[3], menuId: o[4], row: o[5], model: o[6], forkFrame: o[7],
                     opens: o[8], closes: o[9], refused: o[10], frames: o[11], helpLine: o[12], shownWord: o[13] >>> 0 };
        },
        findEnemy() {
            if (!M._PortLnFindEnemy(scratch)) return null;
            return { addr: u32(0), type: s32(1), x: s32(2), y: s32(3), ability: s32(4) };
        },
        hashAt(f) {
            const r = M._PortLnHashAt(f, scratch);
            if (!r) return null;
            const rg = [];
            for (let i = 0; i < 11; i++) rg.push(u32(2 + 2 * i) + ':' + u32(3 + 2 * i));
            return { lo: u32(0), hi: u32(1), final: r === 2, rg };
        },
        sealedGet(f) {
            if (!M._PortLnSealedGet(f, scratch)) return null;
            const rec = { k0: u32(0), k1: u32(1), nev: u32(2), ontime: u32(3), hlo: u32(4), hhi: u32(5), hashed: u32(6), k2: u32(7), k3: u32(8), ev: [] };
            for (let i = 0; i < rec.nev; i++) {
                M._PortLnSealedEventGet(f, i, scratch);
                rec.ev.push([s32(0), s32(1), s32(2), s32(3), s32(4)]);
            }
            return rec;
        },
        corrections(from = 0) {
            const n = M._PortLnCorrectionCount(), out = [];
            for (let i = from; i < n; i++) {
                M._PortLnCorrection(i, scratch);
                const o = []; for (let k = 0; k < 16; k++) o.push(s32(k));
                out.push(o);
            }
            return out;
        },
        image() {
            const r = M._PortLnImageReady();
            if (!r) return null;
            const p = M._PortLnImagePtr(), n = M._PortLnImageBytes();
            return { frame: r - 1, bytes: M.HEAPU8.slice(p, p + n) };
        },
        loadImage(bytes) {
            const p = M._PortLnImageInBuffer();
            M.HEAPU8.set(bytes, p);
            return M._PortLnImageLoad(bytes.length);
        },
        dump() {
            const r = M._PortLnDumpReady();
            if (!r) return null;
            const p = M._PortLnDumpPtr(), n = M._PortLnImageBytes();
            return { frame: r - 1, bytes: M.HEAPU8.slice(p, p + n) };
        },
        // platform/audio.c + m4a_mixer.c, sessions: [mixed renders, state-only
        // renders, us mixing, us state-only, active channels summed] and
        // [pushes, frames out, drops, pads, muted, frames in, us resampling]
        audioStats() {
            if (!M._PortAudioStatsJs) return null;
            const a = M._PortAudioStatsJs(0) >> 2, b = M._PortAudioStatsJs(1) >> 2;
            // platform/audio_ear.c: what the guest's speakers heard (null without it)
            let ear = null;
            if (M._PortAudioEarNamesJs) {
                const names = cstr(M, M._PortAudioEarNamesJs()).split(',').filter(Boolean), e = M._PortAudioStatsJs(2) >> 2;
                ear = {}; names.forEach((n, i) => { ear[n] = M.HEAPU32[e + i]; });
            }
            return { mixer: Array.from(M.HEAPU32.subarray(a, a + 5)), out: Array.from(M.HEAPU32.subarray(b, b + 7)), ear,
                     device: inst.audio ? Object.assign({ rate: inst.audio.rate }, inst.audio.st) : null };
        },
        peek8: (a) => M.HEAPU8[a],
        peek32: (a) => M.HEAPU32[a >> 2],
        poke8: (a, v) => { M.HEAPU8[a] = v; },
        poke32: (a, v) => { M.HEAPU32[a >> 2] = v >>> 0; },
        cstr: (p) => cstr(M, p),
    };
}

export class Telemetry {
    constructor(M) {
        this.M = M;
        this.names = cstr(M, M._PortLnTelemetryNames()).split(',').filter(Boolean);
        this.corrNames = cstr(M, M._PortLnCorrectionNames()).split(',');
        this.regions = cstr(M, M._PortLnRegionNames()).split(',');
    }
    read() {
        const p = this.M._PortLnTelemetry() >> 2, o = {};
        this.names.forEach((n, i) => { o[n] = this.M.HEAPU32[p + i]; });
        return o;
    }
}
