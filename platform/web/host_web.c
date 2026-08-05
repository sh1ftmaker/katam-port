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

EM_ASYNC_JS(void, PortAwaitAnimationFrame, (void), {
    await new Promise(function (resolve) { requestAnimationFrame(resolve); });
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
