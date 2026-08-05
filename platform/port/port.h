#ifndef GUARD_PORT_PORT_H
#define GUARD_PORT_PORT_H

#include "gba/types.h"
#include "gba/defines.h"

/* ---------------------------------------------------------------------------
 * The GBA memory map, reserved inside the wasm linear memory.
 *
 * These are not emulated addresses.  The port links with --global-base above
 * 0x0A000000, which leaves the whole low region untouched by the compiler, the
 * stack and the allocator -- so `*(vu16 *)0x0600E002 = 0xF1B0;` in the game's
 * HUD code is a plain, valid store, and TaskGetStructPtr's `EWRAM_START + off`
 * arithmetic lands exactly where the game's own allocator put things.
 *
 * That is the single decision the rest of the port rests on.  See
 * docs/ARCHITECTURE.md.
 * ------------------------------------------------------------------------- */

#define GBA_EWRAM_BASE 0x02000000u
#define GBA_EWRAM_SIZE 0x00040000u
#define GBA_IWRAM_BASE 0x03000000u
#define GBA_IWRAM_SIZE 0x00008000u
#define GBA_IO_BASE    0x04000000u
#define GBA_IO_SIZE    0x00000400u
#define GBA_PLTT_BASE  0x05000000u
#define GBA_PLTT_SIZE  0x00000400u
#define GBA_VRAM_BASE  0x06000000u
#define GBA_VRAM_SIZE  0x00018000u
#define GBA_OAM_BASE   0x07000000u
#define GBA_OAM_SIZE   0x00000400u
#define GBA_ROM_BASE   0x08000000u
#define GBA_ROM_MAX    0x02000000u
/* Relocated from the hardware's 0x0E000000 by tools/portify.py -- see the
 * note there.  It saves 80 MiB of wasm memory reservation, which is the
 * difference between running on a phone and not. */
#define GBA_SRAM_BASE  0x09000000u
#define GBA_SRAM_SIZE  0x00010000u

#define PORT_SCREEN_W 240
#define PORT_SCREEN_H 160

/* Set once the player has supplied a ROM. */
extern u32 gPortRomSize;

void PortMemInit(void);
/* Fills the generated ROM-data storage; see build/generated/rom_copies.c. */
void PortLoadRomDataCopies(void);
/* Rewrites the ARM code addresses held in ROM structs into real function
 * pointers; see build/generated/rom_fn_tables.c. */
void PortPatchRomFunctionPointers(void);
void PortRomLoaded(u32 size);

/* --- diagnostics ---------------------------------------------------------
 * Two things the port needs to be loud about, because both mean "this part of
 * the game was never decompiled":
 *   PortMissingFunction -- a function that exists only as ARM assembly
 *   PortUnimplemented   -- a hardware feature the platform layer skips
 * Each name is reported once, then counted. */
void PortMissingFunction(const char *name);
void PortUnimplemented(const char *what);
void PortLog(const char *fmt, ...);
/* Same, at console error level -- for things that are actually wrong, so
 * they survive an "Errors only" filter in the browser console. */
void PortError(const char *fmt, ...);

/* Report each distinct (tag, a, b, c) once.  For state machines that stop
 * advancing somewhere a scripted run cannot reach: one line per state passed
 * through, and a stuck machine shows a single line holding the numbers that
 * explain why.  See platform/main.c. */
void PortTrace(const char *tag, u32 a, u32 b, u32 c);

/* --- write watchpoint -----------------------------------------------------
 * "Which transfer clobbered this byte?" is the question every stale-or-wrong
 * VRAM bug reduces to, and neither a stack trace nor a memory dump answers it:
 * the damage is done by a bulk move that finished frames ago.  Set an address
 * and every block move that covers it reports itself and its caller.
 *
 * PortSetWatch(0) disables it, which is the default, so the check is one
 * compare against a global on paths that already move kilobytes. */
extern u32 gPortWatchAddr;
void PortSetWatch(u32 addr);
void PortWatchCheck(const char *who, uintptr_t dest, u32 bytes,
                    uintptr_t src);
#define PORT_WATCH(who, dest, bytes, src)                                   \
    do {                                                                    \
        if (gPortWatchAddr)                                                 \
            PortWatchCheck((who), (uintptr_t)(dest), (bytes),               \
                           (uintptr_t)(src));                               \
    } while (0)
void PortReportGaps(void);

/* BIOS Halt (`swi 3`).  Nothing here generates interrupts, so a halt that
 * waited would hang; the port treats it as a frame boundary. */
void PortHalt(void);

/* --- frame loop ---------------------------------------------------------- */
void PortPresentFrame(void);   /* render one frame and yield to the browser */
void PortVBlank(void);         /* run the game's VBlank handler */

/* VBlank is a time budget, not a moment.
 *
 * The game keeps working after VBlankIntrWait returns -- it flushes OAM and
 * palettes, then drains its background-transfer queue -- and every worker in
 * that queue checks whether the display is still in VBlank before doing one
 * more item:
 *
 *     while (queue not empty) {
 *         if (!(REG_DISPSTAT & DISPSTAT_VBLANK)) return FALSE;
 *         ... transfer one background ...
 *     }
 *
 * So the flag has to stay set for a while and then go out, or the queue never
 * drains and GameLoop stops running tasks entirely.  Since the port maps the
 * I/O registers as plain memory there is no way to hook the read, so the
 * budget is spent by the transfers themselves: each one consumes bytes, and
 * VBlank ends when the budget runs out.  Whatever is left in the queue is
 * picked up next frame, which is exactly what the hardware does. */
void PortVBlankConsume(u32 bytes);
void PortVBlankEnd(void);

/* --- input ---------------------------------------------------------------
 * KEYINPUT is active-low: a clear bit means pressed. */
void PortSetKeys(u16 downMask);

/* --- video ----------------------------------------------------------------
 *
 * The GBA draws exactly 240x160 and nothing about that is negotiable on
 * hardware.  Here it is: the PPU reads the game's memory and writes pixels,
 * and neither end insists on the size of the rectangle in between.  So the
 * renderer is parameterised by a *view rectangle* in GBA screen coordinates:
 *
 *     (gPortViewX, gPortViewY, gPortViewW, gPortViewH)
 *
 * The default is (0, 0, 240, 160), which is the hardware, exactly, and is
 * what ships.  Widening it renders columns the hardware never scanned;
 * shrinking it is a crop that the page then scales up.  See docs/VIEW.md for
 * what that costs, which is a great deal more than it sounds like.
 *
 * The framebuffer is one static allocation at the maximum size and is packed
 * to the *current* width, so a frame is always gPortViewW*gPortViewH
 * contiguous pixels starting at gPortFramebuffer -- the page's blit does not
 * have to know about a stride.
 */
#define PORT_MAX_VIEW_W 512
#define PORT_MAX_VIEW_H 352

extern s32 gPortViewX, gPortViewY, gPortViewW, gPortViewH;

/* Backgrounds that live in screen space rather than world space -- the HUD.
 * A bit per BG.  Widening the view moves everything else outwards; these get
 * pinned to the edges of the picture instead.  See PortMapScreenSpace. */
extern u32 gPortScreenSpaceBgs;
extern u32 gPortPinScreenSpace;    /* 0 = let the HUD float, 1 = pin it */

/* How far outside the screen each background still holds real data, in screen
 * coordinates.  See the note in ppu.c: the game streams exactly one screen
 * block of tilemap, so beyond it a layer repeats itself rather than
 * continuing, and drawing that repeat is a lie the wider view has to choose
 * whether to tell.  +-0x40000000 is "no limit". */
extern s32 gPortBgValidL[4], gPortBgValidR[4];
extern s32 gPortBgValidT[4], gPortBgValidB[4];

/* Where to read a map entry the game never copied into VRAM.  Filled from the
 * game's own Background records; see the note in ppu.c.  A null `map` means
 * this layer has no source to synthesise from and must stop at the window. */
typedef struct {
    const u16 *map;
    s32 widthTiles, heightTiles;
    s32 scrollX, scrollY;      /* in pixels, as the game holds them */
    s32 offX, offY;            /* Background.unk1E / unk20, in tiles */
} PortBgSource;
extern PortBgSource gPortBgSource[4];

extern u32 gPortFramebuffer[PORT_MAX_VIEW_W * PORT_MAX_VIEW_H];
void PortRenderFrame(void);
/* Clamped to the maximum, and to something the compositor can index. */
void PortSetView(s32 x, s32 y, s32 w, s32 h);
void PortSetScreenSpaceBgs(u32 mask, u32 pin);

/* --- the view controller (platform/view.c) --------------------------------
 * One description of what the player is looking at, from which the renderer's
 * rectangle and the game's four separate ideas of "on screen" are all
 * derived.  PORT_VIEW_NATIVE is the hardware and is the default. */
#define PORT_VIEW_NATIVE    0
#define PORT_VIEW_WIDE      1
#define PORT_VIEW_ZOOM      2
#define PORT_VIEW_WIDE_ZOOM 3

#define PORT_CULL_STOCK 0   /* the game's own bounds, untouched */
#define PORT_CULL_MATCH 1   /* the same bounds, moved out to match the view */
#define PORT_CULL_NONE  2   /* no despawn at all -- see docs/VIEW.md */

void PortViewUpdate(void);
/* tilemap: 0 draw the hardware wrap, 1 draw nothing past the streamed
 * window, 2 read the room's own tilemap for what is past it. */
void PortSetViewMode(u32 mode, s32 padX, s32 padY, u32 cull, u32 pinHud,
                     u32 tilemap);
extern u32 gPortClipToStreamed, gPortSynthesiseColumns;

/* The literals tools/portify.py lifts out of the game's own sources.  Each
 * holds the constant it replaced until platform/view.c says otherwise. */
extern s32 gPortOamMinX, gPortOamMaxX, gPortOamMinY, gPortOamMaxY;
extern s32 gPortCullHalfW, gPortCullHalfH;
extern s32 gPortSpawnPadX, gPortSpawnPadY;
extern s32 gPortCamPadX, gPortCamPadY;

/* --- interrupt dispatch --------------------------------------------------
 * The game installs handlers in gIntrTable and expects the BIOS dispatcher to
 * call them.  There is no dispatcher here, so the platform layer calls them
 * directly at the points where hardware would have. */
void PortDispatchInterrupt(u32 flag);

#endif /* GUARD_PORT_PORT_H */
