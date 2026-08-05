/* Save persistence, browser side.
 *
 * The library itself -- ReadSram, WriteSram, VerifySram, WriteSramEx -- is in
 * platform/sram.c and is shared.  These are the two hooks it calls, and the
 * storage behind them is IndexedDB: the same database that already remembers
 * the player's ROM.  See web/shell.html.
 *
 * Neither hook exists outside a browser page.  The headless harness has no
 * Module functions to call, both bodies fall straight through, and the port
 * behaves as it always did -- save memory that lives and dies with the run. */

#include <emscripten.h>

#include "port/port.h"
#include "port/backend.h"

/* Hands the page an address to fill.  Returns 1 if it put a stored save there,
 * 0 if it had none for this ROM -- or if there is no page at all. */
EM_JS(int, PortSramLoad, (u8 *dest, u32 size), {
    if (!Module.portSramRestore)
        return 0;
    return Module.portSramRestore(dest, size) ? 1 : 0;
});

EM_JS(void, PortSramMarkDirty, (void), {
    if (Module.portSramDirty)
        Module.portSramDirty();
});
