/* DMA channels.
 *
 * The game uses DMA for three different things and the port has to tell them
 * apart:
 *
 *   immediate transfers  -- most of them; VRAM/OAM/palette uploads.  Run now.
 *   VBlank transfers     -- the OAM and palette flush each frame.
 *   HBlank transfers     -- per-scanline effects.  main.c arms channel 0 with
 *                           DMA_START_HBLANK | DMA_REPEAT | DMA_DEST_RELOAD to
 *                           feed a new value to a register every scanline.
 *                           The PPU drives these from its scanline loop.
 *
 * Real DMA also stalls the CPU, and games lean on that for timing.  Nothing
 * here is cycle-accurate, so immediate transfers simply complete before the
 * store returns -- which is why DmaWait is compiled to nothing.
 */

#include "port/port.h"
#include "port/dma.h"
#include "gba/io_reg.h"

struct DmaChannel {
    const u8 *src;
    u8 *dest;
    const u8 *srcReload;
    u8 *destReload;
    u32 count;
    u16 flags;
    u8 armed;
};

static struct DmaChannel sChannels[4];

#define DMA_TIMING(f)   ((f) & 0x3000)
#define TIMING_NOW      0x0000
#define TIMING_VBLANK   0x1000
#define TIMING_HBLANK   0x2000
#define TIMING_SPECIAL  0x3000

#define DEST_CTRL(f)    (((f) >> 5) & 3)
#define SRC_CTRL(f)     (((f) >> 7) & 3)
#define CTRL_INC        0
#define CTRL_DEC        1
#define CTRL_FIXED      2
#define CTRL_INC_RELOAD 3

/* Is [addr, addr+len) inside a region the console actually decodes?
 *
 * The port's whole memory map lives at its true GBA addresses inside one wasm
 * linear memory, so a DMA with a stale pointer does not fault at the pointer:
 * it faults inside the copy loop, tens of frames after whatever queued it, and
 * wasm reports "out of bounds memory access" with no address.  The VBlank
 * queue in main.c makes this worse -- an entry is enqueued in one frame and
 * drained in the next, so the stack at the fault names the drain, never the
 * code that supplied the pointer.
 *
 * Checking the endpoints here catches it at the transfer, where the channel
 * still knows its source, destination, count and flags.
 */
static int RangeOk(uintptr_t addr, u32 len)
{
    /* The regions the GBA decodes, in map order.  ROM is sized to the real
     * cartridge address space rather than the loaded image: the game reads
     * past the end of its own data in a few places and the hardware simply
     * returns open bus. */
    static const struct { uintptr_t base; u32 size; } kRegions[] = {
        { 0x00000000u,    0x00004000u    },          /* BIOS */
        { GBA_EWRAM_BASE, GBA_EWRAM_SIZE }, { GBA_IWRAM_BASE, GBA_IWRAM_SIZE },
        { GBA_IO_BASE,    GBA_IO_SIZE    }, { GBA_PLTT_BASE,  GBA_PLTT_SIZE  },
        { GBA_VRAM_BASE,  GBA_VRAM_SIZE  }, { GBA_OAM_BASE,   GBA_OAM_SIZE   },
        { GBA_ROM_BASE,   GBA_ROM_MAX    }, { GBA_SRAM_BASE,  GBA_SRAM_SIZE  },
    };
    u32 i;

    if (addr + len < addr)                           /* wrapped */
        return 0;

    /* The port's own C data, above the reserved map and below the end of
     * linear memory.  DmaFill passes the address of a local holding the fill
     * value, so a perfectly ordinary transfer has a source up here -- reading
     * the bound from the module rather than hardcoding it keeps this honest if
     * INITIAL_MEMORY changes. */
    if (addr >= PORT_GLOBAL_BASE
     && addr + len <= (uintptr_t)__builtin_wasm_memory_size(0) * 65536u)
        return 1;

    for (i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); i++)
        if (addr >= kRegions[i].base
         && addr + len <= kRegions[i].base + kRegions[i].size)
            return 1;
    return 0;
}

static u32 sBadTransfers;

/* Both endpoints of a run, given the address-control mode: a decrementing
 * transfer ends below where it started, a fixed one never moves. */
static void SpanOf(uintptr_t start, u32 ctrl, u32 unit, u32 count,
                   uintptr_t *lo, u32 *len)
{
    if (ctrl == CTRL_DEC) {
        *len = count * unit;
        *lo = start - (count - 1) * unit;
    } else if (ctrl == CTRL_FIXED) {
        *len = unit;
        *lo = start;
    } else {
        *len = count * unit;
        *lo = start;
    }
}

static void RunTransfer(struct DmaChannel *ch)
{
    u32 unit = (ch->flags & DMA_32BIT) ? 4 : 2;
    u32 destCtrl = DEST_CTRL(ch->flags);
    u32 srcCtrl = SRC_CTRL(ch->flags);
    /* The hardware ignores the low address bits for the transfer width -- a
     * 32-bit DMA from 0x...2 reads from 0x...0.  The game relies on that: it
     * passes addresses that are not unit-aligned and gets aligned transfers
     * back.  Without this mask the port reads a window shifted by one or two
     * bytes, which corrupts whatever it copies rather than failing. */
    const u8 *src = (const u8 *)((uintptr_t)ch->src & ~(uintptr_t)(unit - 1));
    u8 *dest = (u8 *)((uintptr_t)ch->dest & ~(uintptr_t)(unit - 1));
    u32 i;

    if (ch->count == 0)
        return;

    {
        uintptr_t srcLo, destLo;
        u32 srcLen, destLen;

        SpanOf((uintptr_t)src, srcCtrl, unit, ch->count, &srcLo, &srcLen);
        SpanOf((uintptr_t)dest, destCtrl, unit, ch->count, &destLo, &destLen);

        if (!RangeOk(srcLo, srcLen) || !RangeOk(destLo, destLen)) {
            sBadTransfers++;
            if (sBadTransfers <= 20)
                PortLog("[katam-port] DMA leaves the map: src=0x%08X%s "
                        "dest=0x%08X%s count=%u unit=%u flags=0x%04X",
                        (unsigned)srcLo, RangeOk(srcLo, srcLen) ? "" : " <-- bad",
                        (unsigned)destLo, RangeOk(destLo, destLen) ? "" : " <-- bad",
                        (unsigned)ch->count, (unsigned)unit,
                        (unsigned)ch->flags);
            else if (sBadTransfers == 21)
                PortLog("[katam-port] further bad DMA transfers suppressed");
            /* Skip it rather than trap.  On hardware an undecoded address
             * reads open bus and writes nowhere -- the transfer is garbage
             * either way, but the game keeps running, and one session then
             * reports every bad transfer instead of only the first. */
            return;
        }
    }

    PORT_WATCH("DMA", dest, ch->count * unit, src);

    for (i = 0; i < ch->count; i++) {
        if (unit == 4)
            *(u32 *)dest = *(const u32 *)src;
        else
            *(u16 *)dest = *(const u16 *)src;

        if (srcCtrl == CTRL_INC || srcCtrl == CTRL_INC_RELOAD)
            src += unit;
        else if (srcCtrl == CTRL_DEC)
            src -= unit;

        if (destCtrl == CTRL_INC || destCtrl == CTRL_INC_RELOAD)
            dest += unit;
        else if (destCtrl == CTRL_DEC)
            dest -= unit;
    }

    PortVBlankConsume(ch->count * unit);

    /* A repeating channel restarts from its latched addresses; DEST_RELOAD
     * additionally rewinds the destination.  A non-repeating one is done. */
    if (ch->flags & DMA_REPEAT) {
        ch->src = (srcCtrl == CTRL_FIXED) ? ch->srcReload : src;
        ch->dest = (destCtrl == CTRL_INC_RELOAD) ? ch->destReload : dest;
    } else {
        ch->armed = 0;
    }
}

void PortDmaSet(int channel, const void *src, void *dest, u32 control)
{
    struct DmaChannel *ch;
    u16 flags = control >> 16;
    u32 count = control & 0xFFFF;

    if (channel < 0 || channel > 3)
        return;
    ch = &sChannels[channel];

    ch->src = ch->srcReload = (const u8 *)src;
    ch->dest = ch->destReload = (u8 *)dest;
    /* A count of 0 means the maximum: 0x4000 for channels 0-2, 0x10000 for 3. */
    ch->count = count ? count : (channel == 3 ? 0x10000 : 0x4000);
    ch->flags = flags;

    /* Mirror into the IO registers so code that reads them back sees what it
     * wrote.  The game's DmaWait spins on the enable bit, but portify compiles
     * DmaWait away, so the bit is cleared immediately for immediate transfers. */
    ((vu32 *)(REG_ADDR_DMA0 + channel * 12))[0] = (u32)src;
    ((vu32 *)(REG_ADDR_DMA0 + channel * 12))[1] = (u32)dest;

    if (!(flags & DMA_ENABLE)) {
        ch->armed = 0;
        ((vu32 *)(REG_ADDR_DMA0 + channel * 12))[2] = control & ~(DMA_ENABLE << 16);
        return;
    }

    ch->armed = 1;
    if (DMA_TIMING(flags) == TIMING_NOW) {
        RunTransfer(ch);
        ((vu32 *)(REG_ADDR_DMA0 + channel * 12))[2] = control & ~(DMA_ENABLE << 16);
    } else {
        ((vu32 *)(REG_ADDR_DMA0 + channel * 12))[2] = control;
    }
}

void PortDmaStop(int channel)
{
    if (channel < 0 || channel > 3)
        return;
    sChannels[channel].armed = 0;
    sChannels[channel].flags = 0;
    ((vu32 *)(REG_ADDR_DMA0 + channel * 12))[2] = 0;
}

static void RunTiming(u16 timing)
{
    int i;
    for (i = 0; i < 4; i++) {
        struct DmaChannel *ch = &sChannels[i];
        if (ch->armed && DMA_TIMING(ch->flags) == timing)
            RunTransfer(ch);
    }
}

void PortDmaHBlank(int line)
{
    (void)line;
    RunTiming(TIMING_HBLANK);
}

void PortDmaVBlank(void)
{
    RunTiming(TIMING_VBLANK);
}
