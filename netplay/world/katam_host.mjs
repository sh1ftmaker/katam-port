// The game inside a Worker: instantiate build/katam-server.mjs against a
// precompiled WebAssembly.Module, boot to the world, split into seats, and
// hand every seat's frame to a callback.  No sockets here; spike.mjs and the
// world server drive it.
//
// workerd is neither a page nor node: no requestAnimationFrame, no location,
// no document.  The glue only needs location for a URL it never uses (it is
// given the module directly), and the port's frame pacing uses rAF -- which
// here is the host's tick.

import createKatam from '../../build/katam-server.mjs';
import { armBoot, F0, AHEAD } from './boot.mjs';
import { createEncoder, rgbaTo555, scrollRegs } from '../framecodec.mjs';

export const SEATS = 4;

/* opts: { wasm: WebAssembly.Module, rom: Uint8Array, seats?, deflate,
 *         onFrame(seat, frame, packet, mode), log? }
 * Resolves to a handle once the world is up and the seats exist. */
export async function startWorld(opts) {
    const seats = opts.seats || SEATS;
    const log = opts.log || (() => {});
    if (typeof globalThis.location === 'undefined')
        globalThis.location = { href: 'https://worker.invalid/' };

    /* The frame loop's vsync: the port awaits one rAF per round.  Until
     * started, rounds run back to back (boot); afterwards the host's tick
     * releases them. */
    let pending = null;
    let paced = false;
    globalThis.requestAnimationFrame = (cb) => {
        if (paced) pending = cb; else queueMicrotask(cb);
        return 0;
    };

    const encoders = [];
    for (let s = 0; s < seats; s++) encoders.push(createEncoder(opts.deflate));
    let confirm = null;
    let ready; const worldUp = new Promise((r) => { ready = r; });
    let rounds = 0, presentMs = 0;

    const Module = {
        portRomReady: Promise.resolve(),
        portAudioRate: 0,
        print: (t) => log(t), printErr: (t) => log(t),
        instantiateWasm(imports, done) {
            const inst = new WebAssembly.Instance(opts.wasm, imports);
            done(inst);
            return inst.exports;
        },
        onRuntimeInitialized() {
            Module.HEAPU8.set(opts.rom, 0x08000000);
            Module._PortRomLoaded(opts.rom.length);
            confirm = armBoot(Module, seats);
        },
        portPresent(ptr, w, h) {
            const frame = Module._PortFrameNumber();
            if (!Module._PortSeatActive()) {
                confirm(frame + AHEAD);
                if (frame >= F0 + 1) {
                    Module._PortRbShutdown();
                    if (!Module._PortSeatInit(seats)) throw new Error('PortSeatInit failed');
                    paced = true;
                    ready();
                }
                return;
            }
            const t = performance.now();
            const seat = Module._PortSeatCurrent();
            const px = rgbaTo555(Module.HEAPU8.subarray(ptr, ptr + w * h * 4));
            const r = encoders[seat].encode(px, scrollRegs(Module.HEAPU8, 0x04000000));
            presentMs += performance.now() - t;
            opts.onFrame(seat, frame, r.packet, r.mode);
            if (Module._PortSeatIsLast()) rounds++;
        },
    };
    const t0 = performance.now();
    createKatam(Module);
    await worldUp;
    const bootMs = performance.now() - t0;

    return {
        Module, bootMs,
        /* Release one round (every seat's frame).  Returns false if the
         * previous round has not finished yet. */
        tick() {
            const cb = pending;
            if (!cb) return false;
            pending = null;
            cb();
            return true;
        },
        setInput(slot, keys) { Module._PortSeatSetInput(slot, keys); },
        setDriven(slot, driven) { Module._PortSeatSetDriven(slot, driven ? 1 : 0); },
        keyframe(seat) { encoders[seat].reset(); },
        stats() { return { rounds, presentMs }; },
    };
}
