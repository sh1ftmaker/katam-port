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

static void RunTransfer(struct DmaChannel *ch)
{
    u32 unit = (ch->flags & DMA_32BIT) ? 4 : 2;
    u32 destCtrl = DEST_CTRL(ch->flags);
    u32 srcCtrl = SRC_CTRL(ch->flags);
    const u8 *src = ch->src;
    u8 *dest = ch->dest;
    u32 i;

    if (ch->count == 0)
        return;

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
