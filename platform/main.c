/* Entry point and frame loop.
 *
 * On hardware, crt0.s sets up the stack, clears RAM, installs the interrupt
 * vector and jumps to AgbMain, which never returns -- GameLoop runs forever and
 * blocks in VBlankIntrWait once a frame.  A browser cannot be blocked, so the
 * port is built with Asyncify: VBlankIntrWait unwinds the wasm stack, hands
 * control back to the page, and resumes on the next animation frame.  The
 * game's own loop is left exactly as it is.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <emscripten.h>

#include "port/port.h"
#include "port/dma.h"
#include "gba/gba.h"
#include "main.h"

u32 gPortRomSize;
static u16 sKeysDown;          /* 1 = pressed, in GBA button-bit order */
static int sRomReady;
static u32 sFrameCount;

/* Roughly what a real VBlank fits: 68 scanlines of DMA to VRAM.  Large enough
 * that the mandatory OAM and palette flush never eats the whole window, small
 * enough that a backed-up queue takes several frames to drain, as on hardware. */
#define PORT_VBLANK_BUDGET_BYTES 24576
static s32 sVBlankBudget;

/* --- diagnostics --------------------------------------------------------- */

#define MAX_REPORTED 64
static const char *sReported[MAX_REPORTED];
static int sNumReported;
static int sMissingCalls;

void PortLog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    emscripten_console_log(buf);
}

static int ReportOnce(const char *name)
{
    int i;

    for (i = 0; i < sNumReported; i++)
        if (sReported[i] == name || strcmp(sReported[i], name) == 0)
            return 0;
    if (sNumReported < MAX_REPORTED)
        sReported[sNumReported++] = name;
    return 1;
}

void PortMissingFunction(const char *name)
{
    sMissingCalls++;
    if (ReportOnce(name))
        PortLog("[katam-port] called %s(), which has no C body yet "
                "(still ARM assembly in the decomp)", name);
}

void PortUnimplemented(const char *what)
{
    if (ReportOnce(what))
        PortLog("[katam-port] unimplemented: %s", what);
}

void PortReportGaps(void)
{
    PortLog("[katam-port] %d distinct gaps reported, %d calls into missing "
            "functions", sNumReported, sMissingCalls);
}

/* --- memory -------------------------------------------------------------- */

void PortMemInit(void)
{
    /* The GBA regions are plain linear memory here, so "reset" is a memset.
     * crt0.s would have done this before AgbMain. */
    memset((void *)GBA_EWRAM_BASE, 0, GBA_EWRAM_SIZE);
    memset((void *)GBA_IWRAM_BASE, 0, GBA_IWRAM_SIZE);
    memset((void *)GBA_IO_BASE, 0, GBA_IO_SIZE);
    memset((void *)GBA_PLTT_BASE, 0, GBA_PLTT_SIZE);
    memset((void *)GBA_VRAM_BASE, 0, GBA_VRAM_SIZE);
    memset((void *)GBA_OAM_BASE, 0, GBA_OAM_SIZE);
    memset((void *)GBA_SRAM_BASE, 0, GBA_SRAM_SIZE);

    /* No buttons held at power-on; KEYINPUT is active-low. */
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_KEYINPUT) = 0x03FF;
}

void PortRomLoaded(u32 size)
{
    gPortRomSize = size;
    sRomReady = 1;
}

/* --- input ---------------------------------------------------------------
 * The shell sends a mask of held buttons; KEYINPUT wants the complement. */

void PortSetKeys(u16 downMask)
{
    sKeysDown = downMask & 0x03FF;
}

static void UpdateKeyInput(void)
{
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_KEYINPUT) = (~sKeysDown) & 0x03FF;
}

/* --- interrupts ----------------------------------------------------------
 * gIntrTable is the game's own handler table, in the order crt0's dispatcher
 * used: SIO, VBlank, HBlank, VCount, timers, DMA, keypad, gamepak. */

static const struct { u32 flag; int index; } sIntrIndex[] = {
    { INTR_FLAG_SERIAL, 0 },
    { INTR_FLAG_VBLANK, 1 },
    { INTR_FLAG_HBLANK, 2 },
    { INTR_FLAG_VCOUNT, 3 },
    { INTR_FLAG_TIMER0, 4 },
    { INTR_FLAG_TIMER1, 5 },
    { INTR_FLAG_TIMER2, 6 },
    { INTR_FLAG_TIMER3, 7 },
};

void PortDispatchInterrupt(u32 flag)
{
    u32 i;

    /* Honour the game's own masking, exactly as the BIOS dispatcher would. */
    if (!(*(vu16 *)(GBA_IO_BASE + REG_OFFSET_IME) & 1))
        return;
    if (!(*(vu16 *)(GBA_IO_BASE + REG_OFFSET_IE) & flag))
        return;

    for (i = 0; i < sizeof(sIntrIndex) / sizeof(sIntrIndex[0]); i++) {
        if (sIntrIndex[i].flag == flag) {
            IntrFunc handler = gIntrTable[sIntrIndex[i].index];
            if (handler)
                handler();
            return;
        }
    }
}

/* --- frame boundary ------------------------------------------------------ */

EM_ASYNC_JS(void, PortAwaitAnimationFrame, (void), {
    await new Promise(function (resolve) { requestAnimationFrame(resolve); });
});

EM_JS(void, PortBlitFramebuffer, (const u32 *pixels, int w, int h), {
    if (Module.portPresent)
        Module.portPresent(pixels, w, h);
});

void PortPresentFrame(void)
{
    vu16 *dispstat = (vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPSTAT);

    /* Draw the visible frame from whatever the game last wrote. */
    PortRenderFrame();

    /* Enter VBlank: run the flush DMAs, then the game's VBlank handler. */
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_VCOUNT) = 160;
    *dispstat |= DISPSTAT_VBLANK;
    PortDmaVBlank();
    PortDispatchInterrupt(INTR_FLAG_VBLANK);

    PortBlitFramebuffer(gPortFramebuffer, PORT_SCREEN_W, PORT_SCREEN_H);
    PortAwaitAnimationFrame();

    UpdateKeyInput();
    sFrameCount++;

    /* Return with VBlank still in progress.  GameLoop does its screen flush
     * and queue draining now, and PortVBlankConsume ends VBlank once those
     * transfers have used up the window -- which is also what releases the
     * `while (REG_DISPSTAT & DISPSTAT_VBLANK);` spin at the end of the loop. */
    sVBlankBudget = PORT_VBLANK_BUDGET_BYTES;
}

void PortVBlankConsume(u32 bytes)
{
    vu16 *dispstat = (vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPSTAT);

    if (!(*dispstat & DISPSTAT_VBLANK))
        return;
    sVBlankBudget -= (s32)bytes;
    if (sVBlankBudget <= 0)
        PortVBlankEnd();
}

void PortVBlankEnd(void)
{
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_DISPSTAT) &= ~DISPSTAT_VBLANK;
    *(vu16 *)(GBA_IO_BASE + REG_OFFSET_VCOUNT) = 0;
    sVBlankBudget = 0;
}

/* --- startup ------------------------------------------------------------- */

EM_ASYNC_JS(void, PortAwaitRom, (void), {
    await Module.portRomReady;
});

int main(void)
{
    PortMemInit();

    /* The page loads the player's own ROM into 0x08000000 before we start;
     * every pointer the game follows into ROM depends on it being there. */
    PortAwaitRom();
    if (!sRomReady || gPortRomSize == 0) {
        PortLog("[katam-port] no ROM supplied; refusing to start");
        return 1;
    }
    PortLog("[katam-port] ROM loaded (%u bytes), starting AgbMain", gPortRomSize);
    PortLoadRomDataCopies();
    PortPatchRomFunctionPointers();

    UpdateKeyInput();
    AgbMain();      /* never returns */
    return 0;
}
