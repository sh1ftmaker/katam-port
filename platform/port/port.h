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

/* --- video --------------------------------------------------------------- */
extern u32 gPortFramebuffer[PORT_SCREEN_W * PORT_SCREEN_H];
void PortRenderFrame(void);

/* --- interrupt dispatch --------------------------------------------------
 * The game installs handlers in gIntrTable and expects the BIOS dispatcher to
 * call them.  There is no dispatcher here, so the platform layer calls them
 * directly at the points where hardware would have. */
void PortDispatchInterrupt(u32 flag);

#endif /* GUARD_PORT_PORT_H */
