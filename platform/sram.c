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
 * at 0x0E000000, like the rest of the GBA map.
 *
 * The contents are not yet persisted between sessions -- see docs/STATUS.md.
 */

#include <string.h>

#include "port/port.h"
#include "gba/gba.h"
#include "agb_sram.h"

const char gAgbSramLibVer[] = "NINTENDOSRAM_V113";

void ReadSram(const u8 *src, u8 *dest, u32 size)
{
    memcpy(dest, src, size);
}

void WriteSram(const u8 *src, u8 *dest, u32 size)
{
    memcpy(dest, src, size);
}

u32 VerifySram(const u8 *src, u8 *target, u32 size)
{
    u32 i;

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
