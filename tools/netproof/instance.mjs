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

export async function createInstance({ modulePath, rom, name, render = false, menus = 0, quiet = true, onLog }) {
    const createKatam = require(path.resolve(modulePath));
    const inst = { name, logs: [], _raf: null, _onRaf: null, frames: 0 };
    const log = (t) => {
        inst.logs.push(t);
        if (inst.logs.length > 20000) inst.logs.splice(0, 5000);
        if (onLog) onLog(name, t);
        if (!quiet) console.log(`[${name}] ${t}`);
    };
    let romReady;
    const Module = {
        portRomReady: new Promise((r) => { romReady = r; }),
        portAudioRate: 0,
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
                     groupRoom: o[11], room: o[12], localPlayer: o[13], rng: o[14] >>> 0, modeFlags: o[15] >>> 0 };
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
            const rec = { k0: u32(0), k1: u32(1), nev: u32(2), ontime: u32(3), hlo: u32(4), hhi: u32(5), hashed: u32(6), ev: [] };
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
