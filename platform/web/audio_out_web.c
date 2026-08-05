/* Audio transport, browser side: a hand-written AudioWorklet fed by
 * postMessage, one block per VBlank.
 *
 * The C half -- s16 to float, block staging, the queue-depth feedback the
 * mixer clocks itself from -- is in platform/audio_out.c and is shared with
 * the native build.  This file is only the transport.
 *
 * Why not -sAUDIO_WORKLET
 * ----------------------
 * emscripten's own audio-worklet support (`-sAUDIO_WORKLET`) needs WASM_WORKERS
 * and wasm shared memory, and shared memory needs cross-origin isolation
 * (COOP/COEP).  The port is served from GitHub Pages, which sends neither and
 * offers no way to add them, so SharedArrayBuffer is unavailable there and the
 * flag simply cannot be used.  It would also mean auditing the entire
 * single-threaded port for races on the GBA memory map, which is a large price
 * for audio.
 *
 * So the worklet is hand-written and fed by postMessage instead.  An
 * AudioWorkletNode does *not* require SharedArrayBuffer -- that is only needed
 * if the worklet wants to read wasm linear memory directly.  Without it you
 * copy: a frame of 48 kHz stereo is ~3.2 KB, so at 60 Hz this is ~190 KB/s of
 * memcpy, which is nothing next to the software PPU.
 *
 * Why a worklet at all, rather than SDL2's ScriptProcessorNode
 * -----------------------------------------------------------
 * `process()` runs on the browser's dedicated audio render thread.  The main
 * thread here renders a full GBA scanline PPU in software and then blocks on
 * requestAnimationFrame under Asyncify -- exactly the workload that makes a
 * main-thread ScriptProcessorNode callback miss its deadline and crackle.  The
 * ScriptProcessorNode path is kept as a fallback for engines with no
 * AudioWorklet, where crackling beats silence.
 */

#include <emscripten.h>

#include "port/port.h"
#include "port/backend.h"

/* ------------------------------------------------------------------------- *
 * Everything lives on Module.portAudio so the page (and the headless harness)
 * can see it, and so a second call to PortAudioOpen is a no-op rather than a
 * second AudioContext.  Nothing here touches web/shell.html: the unlock
 * listeners are installed from this file, so audio works whether or not the
 * shell knows about it.
 * ------------------------------------------------------------------------- */

EM_JS(void, PortAudioOpen, (int ringFrames), {
    if (typeof Module.portAudio !== 'undefined')
        return;

    /* Headless: the harness sets Module.portAudioRate before the module runs,
     * which turns the transport into a capture buffer with no device at all.
     * This is the only way to answer "is sound coming out" without a browser,
     * and it is why tools/headless_test.js can report RMS. */
    var AC = (typeof AudioContext !== 'undefined') ? AudioContext
           : (typeof webkitAudioContext !== 'undefined') ? webkitAudioContext
           : null;
    if (AC === null) {
        Module.portAudio = {
            rate: Module.portAudioRate || 0,
            depth: 0,
            headless: true,
            push: function (f32) {
                /* Model a device draining at exactly 60 Hz.  Without this the
                 * queue always reads empty, the producer widens every block to
                 * its 3% limit, and the harness reports 3% more audio than
                 * game time -- which looks like a bug in the mixer and is
                 * really the control loop with no feedback attached.  With it,
                 * "seconds of audio == seconds of game" is a real check on the
                 * clocking. */
                this.depth += f32.length >> 1;
                this.depth -= Math.round(this.rate / 60);
                if (this.depth < 0) this.depth = 0;
                if (Module.portAudioSink) Module.portAudioSink(f32);
            },
        };
        return;
    }

    var pa = {
        ctx: null, node: null, ready: false, headless: false,
        rate: 0, depth: 0, pushed: 0, underruns: 0,
        pending: [],            /* blocks produced before the node exists */
        push: null,
    };
    Module.portAudio = pa;

    try {
        pa.ctx = new AC({ latencyHint: 'interactive' });
    } catch (e) {
        try { pa.ctx = new AC(); } catch (e2) { pa.rate = 0; return; }
    }
    /* Available even while the context is suspended, which is what lets the
     * mixer be configured for the real device rate before the first user
     * gesture.  Do not assume 48000: iOS picks its own and ignores a request. */
    pa.rate = pa.ctx.sampleRate;

    /* The autoplay unlock.  The lesson from every other emscripten port of a
     * console game is that a one-shot unlock is not enough: on iOS the context
     * can refuse the first resume() and the listener has already been removed.
     * So this stays attached and keeps trying until the state sticks. */
    var unlock = function () {
        if (pa.ctx && pa.ctx.state !== 'running') {
            var p = pa.ctx.resume();
            if (p && p.catch) p.catch(function () {});
        }
    };
    ['pointerdown', 'touchstart', 'touchend', 'keydown', 'click'].forEach(
        function (ev) {
            if (typeof document !== 'undefined')
                document.addEventListener(ev, unlock, { passive: true });
        });
    unlock();

    var ringCap = Math.max(2048, Math.ceil(pa.rate / 60) * ringFrames);

    /* The worklet processor, as a function that is stringified into a blob.
     * Writing it as real code rather than a quoted string is not cosmetic:
     * this text goes through the C preprocessor on its way here, and a string
     * literal spanning lines is where that goes wrong.
     *
     * GitHub Pages sends no CSP (checked), so a blob: worklet module loads.
     * Keeping it inline also means the published site stays three files --
     * index.html, katam.js, katam.wasm -- and scripts/publish-pages.sh needs
     * no change. */
    var mkProcessor = function () {
        class KatamSink extends AudioWorkletProcessor {
            constructor(options) {
                super();
                var cap = options.processorOptions.cap;
                this.buf = new Float32Array(cap * 2);
                this.cap = cap;
                this.rd = 0;
                this.wr = 0;
                this.have = 0;
                this.underruns = 0;
                this.tick = 0;
                this.port.onmessage = (e) => this.take(e.data);
            }
            take(block) {
                var n = block.length >> 1;
                /* Overflow means the producer is ahead of the device.  Drop
                 * the incoming block rather than the queued one: what is
                 * already in the ring is what plays next, and cutting into it
                 * is audible while dropping the newest is not. */
                if (n > this.cap - this.have) n = this.cap - this.have;
                for (var i = 0; i < n; i++) {
                    var w = ((this.wr + i) % this.cap) * 2;
                    this.buf[w] = block[i * 2];
                    this.buf[w + 1] = block[i * 2 + 1];
                }
                this.wr = (this.wr + n) % this.cap;
                this.have += n;
            }
            process(inputs, outputs) {
                var out = outputs[0];
                var L = out[0], R = out.length > 1 ? out[1] : out[0];
                var n = L.length;
                var take = n < this.have ? n : this.have;
                for (var i = 0; i < take; i++) {
                    var r = ((this.rd + i) % this.cap) * 2;
                    L[i] = this.buf[r];
                    R[i] = this.buf[r + 1];
                }
                for (var j = take; j < n; j++) { L[j] = 0; R[j] = 0; }
                this.rd = (this.rd + take) % this.cap;
                this.have -= take;
                if (take < n) this.underruns++;
                /* Report the depth back a few times a second.  The producer
                 * only needs it to decide whether this frame should be a
                 * couple of samples longer or shorter, so a stale-by-50ms
                 * number is fine and 375 messages a second is not. */
                if ((++this.tick & 31) === 0)
                    this.port.postMessage({ d: this.have, u: this.underruns });
                return true;
            }
        }
        registerProcessor('katam-sink', KatamSink);
    };

    var startWorklet = function () {
        var src = '(' + mkProcessor.toString() + ')();';
        var url = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
        return pa.ctx.audioWorklet.addModule(url).then(function () {
            URL.revokeObjectURL(url);
            pa.node = new AudioWorkletNode(pa.ctx, 'katam-sink', {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [2],
                processorOptions: { cap: ringCap },
            });
            pa.node.port.onmessage = function (e) {
                pa.depth = e.data.d;
                pa.underruns = e.data.u;
                pa.pushed = 0;
            };
            pa.node.connect(pa.ctx.destination);
            pa.push = function (f32) {
                pa.node.port.postMessage(f32, [f32.buffer]);
                pa.pushed += f32.length >> 1;
            };
            pa.ready = true;
            /* Anything the game produced while addModule was in flight. */
            for (var i = 0; i < pa.pending.length; i++) pa.push(pa.pending[i]);
            pa.pending = [];
        });
    };

    /* The fallback.  ScriptProcessorNode is deprecated and runs its callback on
     * the main thread, which is the thread this port is busy on -- but an
     * engine without AudioWorklet has no better option, and crackle beats
     * silence.  The ring lives here instead of in the worklet. */
    var startScriptProcessor = function () {
        var ring = new Float32Array(ringCap * 2);
        var rd = 0, wr = 0, have = 0;
        var sp = pa.ctx.createScriptProcessor(1024, 0, 2);
        sp.onaudioprocess = function (e) {
            var L = e.outputBuffer.getChannelData(0);
            var R = e.outputBuffer.getChannelData(1);
            var n = L.length;
            var take = n < have ? n : have;
            for (var i = 0; i < take; i++) {
                var r = ((rd + i) % ringCap) * 2;
                L[i] = ring[r]; R[i] = ring[r + 1];
            }
            for (var j = take; j < n; j++) { L[j] = 0; R[j] = 0; }
            rd = (rd + take) % ringCap;
            have -= take;
            if (take < n) pa.underruns++;
            pa.depth = have;
            pa.pushed = 0;
        };
        sp.connect(pa.ctx.destination);
        pa.push = function (f32) {
            var n = f32.length >> 1;
            if (n > ringCap - have) n = ringCap - have;
            for (var i = 0; i < n; i++) {
                var w = ((wr + i) % ringCap) * 2;
                ring[w] = f32[i * 2]; ring[w + 1] = f32[i * 2 + 1];
            }
            wr = (wr + n) % ringCap;
            have += n;
            pa.pushed += n;
        };
        pa.ready = true;
        for (var i = 0; i < pa.pending.length; i++) pa.push(pa.pending[i]);
        pa.pending = [];
    };

    if (pa.ctx.audioWorklet && typeof AudioWorkletNode !== 'undefined') {
        startWorklet().catch(function (err) {
            if (Module.printErr)
                Module.printErr('[katam-port] AudioWorklet failed (' + err
                                + '), falling back to ScriptProcessorNode');
            try { startScriptProcessor(); } catch (e) { pa.rate = 0; }
        });
    } else {
        try { startScriptProcessor(); } catch (e) { pa.rate = 0; }
    }
});

EM_JS(int, PortAudioRate, (void), {
    return (Module.portAudio && Module.portAudio.rate) | 0;
});

EM_JS(int, PortAudioQueued, (void), {
    var pa = Module.portAudio;
    if (!pa) return 0;
    return (pa.depth + (pa.pushed | 0)) | 0;
});

EM_JS(int, PortAudioUnderruns, (void), {
    return (Module.portAudio && Module.portAudio.underruns) | 0;
});

/* The one place samples cross out of wasm.
 *
 * A fresh Float32Array is allocated per block so it can be transferred to the
 * worklet rather than copied again; the allocation is 60 a second and the GC
 * does not notice.  Reading through HEAPU8.buffer rather than a cached HEAPF32
 * is deliberate: this port pins its memory (-sALLOW_MEMORY_GROWTH=0), but a
 * cached typed-array view is exactly the thing that breaks silently if that
 * ever changes. */
EM_JS(void, PortAudioSubmit, (const float *samples, int frames), {
    var pa = Module.portAudio;
    if (!pa) return;
    var n = frames * 2;
    var f32 = new Float32Array(n);
    f32.set(new Float32Array(HEAPU8.buffer, samples, n));
    if (pa.headless) { pa.push(f32); return; }
    if (pa.ready) pa.push(f32);
    else if (pa.pending.length < 8) pa.pending.push(f32);
});
