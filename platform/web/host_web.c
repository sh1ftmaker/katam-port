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
 * The headless harness shims rAF with setTimeout, whose callback carries no
 * timestamp -- that is the unpaced path, deliberately: tests outrun the
 * clock, and always have. */
EM_ASYNC_JS(void, PortAwaitAnimationFrame, (void), {
    var PERIOD = 1000 / 59.7275;
    var due = Module.portFrameDue || 0;
    var now;
    for (;;) {
        now = await new Promise(function (resolve) { requestAnimationFrame(resolve); });
        if (typeof now !== 'number')
            return;
        if (now >= due - 2)
            break;
    }
    Module.portFrameDue = (now > due + PERIOD) ? now + PERIOD : due + PERIOD;
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
