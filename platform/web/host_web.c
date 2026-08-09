/* The browser half of platform/port/backend.h.
 *
 * Everything in here was in platform/main.c until the native build needed the
 * same four hooks pointing somewhere else.  The bodies are unchanged.
 */

#include <emscripten.h>

#include "port/port.h"
#include "port/backend.h"

/* Nothing to do: the page has the module configured before main() runs, and
 * the wasm memory map is reserved by the linker (-sGLOBAL_BASE), not at run
 * time.  See docs/ARCHITECTURE.md. */
void PortHostInit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

/* Everything the port says goes to two places: the browser console, and the
 * page's own log.
 *
 * emscripten_console_log calls console.log directly, which is right for
 * devtools and useless for a bug report -- the crash panel builds its report
 * from the page log, so the port's own diagnostics were the one thing missing
 * from the report that exists to carry them.  Someone hitting a crash on a
 * phone has no console at all.
 *
 * Module.portDiag is optional: the headless harness does not define it. */
EM_JS(void, PortConsole, (const char *s, int isErr), {
    var text = UTF8ToString(s);
    if (isErr) console.error(text); else console.log(text);
    if (Module.portDiag) {
        try { Module.portDiag(text, isErr); } catch (e) { /* never break logging */ }
    }
});

/* One game frame per requestAnimationFrame is only right on a 60 Hz panel.
 * rAF fires at the monitor's refresh rate, so a 120 Hz display used to run
 * the game at double speed.  Ticks are waited out until the GBA's own frame
 * period has elapsed instead -- 59.7275 Hz, the same constant and the same
 * reasoning as the native host's PaceFrame: the game's clock is the one
 * that matters, and the audio clock fights anything else.
 *
 * The deadline accumulates in fractional milliseconds so the average rate
 * is exact on any refresh rate.  On a 60 Hz panel that means one repeated
 * frame every few seconds -- the 0.46% by which 60 Hz outruns the GBA,
 * paid in the open instead of by the audio queue; and two netplay peers on
 * different monitors advance at the same rate instead of one perpetually
 * predicting the other.  After a stall (hidden tab, suspend) the deadline
 * re-anchors to the clock rather than sprinting through the gap.
 *
 * The other direction matters just as much: browsers also deliver rAF
 * SLOWER than the panel -- Chrome's Energy Saver and macOS Low Power Mode
 * throttle it to 30 Hz outright -- and waiting out a tick that is already
 * overdue turned that into a half-speed game, which reads as "super laggy"
 * rather than as the frame-rate cut it is.  So lateness up to a small debt
 * ceiling is repaid, not forgiven: a frame whose deadline has already
 * passed runs immediately, no vsync wait, at most three in a row so the
 * event loop (sockets, input) always breathes.  At 30 Hz rAF that is two
 * game frames per paint -- correct speed, half the drawn frames.  Only a
 * gap past the ceiling (a real stall: tab hidden, machine asleep)
 * re-anchors and lets the lost time go.
 *
 * A hidden tab gets no rAF at all -- browsers stop it dead -- which used to
 * stop the game dead too.  Alone that was fine; in netplay it starves the
 * other player: cover one of two windows and the visible one drowns in
 * communication errors.  So a hidden tab paces itself off the timer clock
 * at the same 59.7275 Hz.  (Browsers exempt audibly-playing tabs from
 * timer throttling, and the game plays audio; a *muted* hidden tab still
 * winds down to ~1 Hz, which nothing here can help.)  The 250 ms guard
 * only bridges the transition: a wait that began visible would otherwise
 * sleep until the tab is looked at again.
 *
 * Headless (no `document`) means the node harness: one plain rAF await per
 * frame, unpaced -- tests outrun the clock, and always have -- and none of
 * the timers above, because the rollback tests pause an instance by parking
 * its rAF registrations, and a guard timer firing from outside any
 * instance's execution window re-registers on the wrong ledger. */
EM_ASYNC_JS(void, PortAwaitAnimationFrame, (void), {
    if (typeof document === 'undefined') {
        await new Promise(function (resolve) { requestAnimationFrame(resolve); });
        return;
    }
    var PERIOD = 1000 / 59.7275;
    var due = Module.portFrameDue || 0;
    var st = Module.portPace ||
        (Module.portPace = { t0: 0, sims: 0, waits: 0, catchups: 0,
                             skips: 0, cpuMax: 0, lastExit: 0 });
    /* Every five seconds, one summary -- and only when something is off
     * (heavy catch-up use, or a game running slow), so a healthy session
     * logs nothing.  This is the line a lag report needs: it separates "the
     * browser starves us of vsyncs" from "the frames themselves are slow". */
    var report = Module.portPaceLine || (Module.portPaceLine = function (s, t) {
        if (!s.t0) { s.t0 = t; return; }
        var secs = (t - s.t0) / 1000;
        if (secs < 5) return;
        var sims = s.sims / secs;
        if (s.catchups / secs > 5 || sims < 55)
            console.log('[katam-port] pace: ' + sims.toFixed(1) + ' frames/s, '
                + (s.waits / secs).toFixed(1) + ' vsync waits/s, '
                + s.catchups + ' catch-up frames, worst frame cpu '
                + s.cpuMax.toFixed(1) + ' ms'
                + (sims > 55 && s.waits / secs < 50
                   ? ' -- the browser is throttling animation (power saver?); '
                     + 'speed kept by extra frames between paints'
                   : ''));
        s.t0 = t; s.sims = 0; s.waits = 0; s.catchups = 0; s.cpuMax = 0;
    });
    var now = performance.now();
    if (st.lastExit && now - st.lastExit > st.cpuMax)
        st.cpuMax = now - st.lastExit;
    st.sims++;

    /* The catch-up path: a full frame of real debt, not a stall's worth,
     * and the last two frames were not themselves catch-ups.  The threshold
     * matters: at `due - 2` this fired during ordinary vsync pacing
     * whenever a frame's own work ran long enough, and at a 59.94 Hz-family
     * refresh that meant a burst of skip/double oscillation each time the
     * GBA clock's phase drifted across a vsync boundary -- the painted rate
     * halved for a second or two every beat period (~5 s) while the game's
     * speed stayed perfect, so nothing logged.  A throttled browser
     * accumulates a frame of debt every frame and clears the bar
     * immediately; vsync pacing never does. */
    if (due > 0 && st.skips < 3 && now >= due + PERIOD && now <= due + 8 * PERIOD) {
        st.skips++;
        st.catchups++;
        Module.portFrameDue = due + PERIOD;
        report(st, now);
        st.lastExit = performance.now();
        return;
    }
    st.skips = 0;
    for (;;) {
        if (document.hidden) {
            await new Promise(function (resolve) { setTimeout(resolve, 4); });
            now = performance.now();
        } else {
            now = await new Promise(function (resolve) {
                var guard = setTimeout(function () { resolve(-1); }, 250);
                requestAnimationFrame(function (t) {
                    clearTimeout(guard);
                    resolve(t);
                });
            });
            if (now < 0)
                continue;
            st.waits++;
        }
        if (now >= due - 2)
            break;
    }
    /* Lateness within the debt ceiling carries forward -- the catch-up path
     * above repays it next frame.  Only a real stall lets the time go. */
    Module.portFrameDue = (now > due + 8 * PERIOD) ? now + PERIOD : due + PERIOD;
    report(st, now);
    st.lastExit = performance.now();
});

/* setTimeout(0), not requestAnimationFrame: a hidden tab throttles rAF to
 * nothing, and a catch-up that happens to run in a background tab should not
 * take a thousand times longer there.  The clamp on nested zero timeouts (~1
 * ms after the first few) is why PortPresentFrame calls this rarely rather
 * than every caught-up frame. */
EM_ASYNC_JS(void, PortAwaitYield, (void), {
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
});

EM_JS(void, PortBlitFramebuffer, (const u32 *pixels, int w, int h), {
    if (Module.portPresent)
        Module.portPresent(pixels, w, h);
});

/* The stall-loop pump (port/backend.h).  The page wires portNetIdle to the
 * netplay driver's receive-queue flush; a page without netplay never
 * defines it and the loop just yields. */
EM_JS(void, PortNetIdle, (void), {
    if (Module.portNetIdle) {
        try { Module.portNetIdle(); } catch (e) { /* never break the loop */ }
    }
});

EM_ASYNC_JS(void, PortAwaitRom, (void), {
    await Module.portRomReady;
});

/* The port's own C data, above the reserved map and below the end of linear
 * memory.  DmaFill passes the address of a local holding the fill value, so a
 * perfectly ordinary transfer has a source up here -- reading the bound from
 * the module rather than hardcoding it keeps this honest if INITIAL_MEMORY
 * changes. */
int PortHostRangeOk(uintptr_t addr, u32 len)
{
    return addr >= PORT_GLOBAL_BASE
        && addr + len <= (uintptr_t)__builtin_wasm_memory_size(0) * 65536u;
}
