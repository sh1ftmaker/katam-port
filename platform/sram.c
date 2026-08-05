/* Save memory.
 *
 * src/agb_sram.c cannot be ported as written.  Cartridge SRAM has to be
 * accessed by code running out of RAM, so Nintendo's library copies the machine
 * code of its own inner loop into a stack buffer and calls it:
 *
 *     s = (void *)((uintptr_t)ReadSram_Core & ~1);
 *     d = readSramFast_Work;
 *     size_ = ((uintptr_t)ReadSram - (uintptr_t)ReadSram_Core) / 2;
 *     while (size_ != 0) { *d++ = *s++; --size_; }
 *     ((void (*)(const u8 *, u8 *, u32))readSramFast_Work + 1)(src, dest, size);
 *
 * In WebAssembly code is not addressable as data and a function pointer is a
 * table index, not an address, so that call goes straight out of bounds -- it
 * was the first thing to crash the port after boot.
 *
 * There is no waitstate constraint here, so this file is what the library was
 * always doing underneath: copy and compare bytes.  SRAM itself is real memory
 * inside the reserved GBA map, like every other region.
 *
 * ---------------------------------------------------------------------------
 * Keeping the save
 *
 * A cartridge holds its save with the console switched off; neither a tab nor
 * a process does.  The host owns the storage -- IndexedDB in the browser, a
 * plain 64 KiB .sav in the platform config directory natively -- and this file
 * is the two hooks it needs.  See platform/web/sram_web.c and
 * platform/native/save_file.c.
 *
 *   restore   The stored bytes are copied in the first time the *game* touches
 *             save memory, rather than at page load.  That ordering is not a
 *             detail: main() calls PortMemInit(), which memsets the whole GBA
 *             map -- this region included -- and it does so after the page has
 *             mapped the ROM and resolved the promise main() is parked on.
 *             Anything the shell wrote into save memory beforehand would be
 *             zeroed a moment later, and the failure would be invisible: a
 *             blank save reads as "no file yet", not as an error.  Restoring
 *             from inside a call the game itself made cannot lose that race,
 *             because the game does not run until PortMemInit has returned.
 *
 *   dirty     Every write that actually changes a byte tells the host, which
 *             debounces and writes the region out.  Hooking the write rather
 *             than polling memory is what makes a missed save impossible:
 *             src/save.c is the only file in the game that names this region
 *             at all, and it reaches it exclusively through WriteSramEx ->
 *             WriteSram, so every byte that ever lands in save memory passes
 *             through the function below.  The page re-checksums on a slow
 *             timer anyway, as a backstop against some future writer that does
 *             not come through here.
 *
 * A host may have no storage at all: the headless harness has no Module
 * functions to call, both web bodies fall straight through, and the port
 * behaves as it always did -- save memory that lives and dies with the run.
 */

#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "gba/gba.h"
#include "agb_sram.h"

const char gAgbSramLibVer[] = "NINTENDOSRAM_V113";

static int sRestored;

/* The whole region is asked for at once, not just the range being read.  The
 * game reads a checksum out of one part of save memory and then the section
 * that checksum covers out of another, so filling only what was asked for
 * would hand it a save that fails its own verification. */
static void SramRestoreOnce(void)
{
    if (sRestored)
        return;
    /* Set before the call, not after: the host is free to log, and anything
     * that ran back into the game here would restore a second time on top of
     * whatever had already been written. */
    sRestored = 1;
    if (PortSramLoad((u8 *)GBA_SRAM_BASE, GBA_SRAM_SIZE))
        PortLog("[katam-port] save memory restored from host storage");
}

/* The library's contract lets a caller name any two addresses, and the game
 * does read save data straight into its own EWRAM buffers.  Only a write whose
 * destination is really inside the region is worth persisting. */
static int InSram(const void *p, u32 size)
{
    uintptr_t addr = (uintptr_t)p;

    return addr >= GBA_SRAM_BASE
        && size <= GBA_SRAM_SIZE
        && addr - GBA_SRAM_BASE <= GBA_SRAM_SIZE - size;
}

/* --- the library ---------------------------------------------------------- */

void ReadSram(const u8 *src, u8 *dest, u32 size)
{
    SramRestoreOnce();
    memcpy(dest, src, size);
}

void WriteSram(const u8 *src, u8 *dest, u32 size)
{
    SramRestoreOnce();

    /* Compare before copying.  WriteSramEx writes, verifies and retries, and
     * the game re-writes sections it has just verified as unchanged; reporting
     * those as changes would have the host push an identical 64K into storage
     * several times over for one in-game save.  At these sizes -- the largest
     * buffer in gWorldProps is a few hundred bytes -- the compare costs
     * nothing next to the storage write it avoids. */
    if (size == 0 || memcmp(dest, src, size) == 0)
        return;

    memcpy(dest, src, size);
    if (InSram(dest, size))
        PortSramMarkDirty();
}

u32 VerifySram(const u8 *src, u8 *target, u32 size)
{
    u32 i;

    SramRestoreOnce();

    /* Returns the address of the first mismatching byte, 0 when identical --
     * the same contract as the original. */
    for (i = 0; i < size; i++)
        if (target[i] != src[i])
            return (u32)(target + i);
    return 0;
}

u32 WriteSramEx(const u8 *src, u8 *dest, u32 size)
{
    int retry;

    for (retry = 0; retry < SRAM_RETRY_MAX; retry++) {
        u32 bad;

        WriteSram(src, dest, size);
        bad = VerifySram(src, dest, size);
        if (bad == 0)
            return 0;
    }
    return (u32)dest;
}
