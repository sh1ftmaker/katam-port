/* The wide build: every Kirby's own tiles and palettes (port/wide.h,
 * KATAM_KIRBY_TILES / KATAM_KIRBY_PAL).  Empty unless KATAM_MAX_KIRBYS is set.
 *
 * The console gives each of its four Kirbys two OBJ palette lines (i, i + 4)
 * and 2 KiB of OBJ VRAM (0x06010000 + (i << 11)) for its animation frames,
 * ability hat and ability effects.  Kirbys 4.. have no such memory on the
 * console, so the wide build makes it, in the wide area of the sim block
 * (WIDE_OFF_OBJX) -- in every snapshot, rollback, state hash and transfer
 * with no change to any of them:
 *
 *   tiles     2 KiB per Kirby.  A Kirby sprite's tilesVram is an address in
 *             here; the game's own tile queue (main.c sub_081525DC, the GBA
 *             DMA) copies its frames there at VBlank exactly as it copies
 *             Kirbys 0..3's into VRAM.
 *   palettes  two lines per Kirby, as palIds 16.. (16 * (i / 4) + i % 4, and
 *             + 4).  Three copies, as the console has three: the game's
 *             buffer (gObjPalette's twin: the animation palette command, the
 *             ability and flash code write it), the palette-effect source
 *             (gSourcePalettes.obj's: what fades and flashes restore from) and
 *             the "hardware" copy (OBJ_PLTT's: made at UpdateScreen when the
 *             game asks for an OBJ palette sync, which is what the renderer
 *             reads, one frame behind the buffer as on the console).
 *
 * What draws them: the GPU renderer.  OAM's 10-bit tile number and 4-bit
 * palette cannot reach past the console's memory, so DisplaySprite writes
 * OAM as for Kirby i & 3 (KATAM_OAM_TILES; palId & 15) -- the classic (PPU)
 * renderer shows that placeholder, a Kirby 4.. wearing Kirby i & 3's frame
 * -- and the render list's sprite hook (platform/render/rl_build.c), which
 * sees the struct Sprite itself, gives the GPU the real tile and line.
 *
 * Whole-palette operations (a fade restoring from the source, gRgbMap
 * transforms of everything, the palette effects' bank masks) reach these
 * lines through the functions below, each called beside the game's own
 * statement for the console's lines; a mask bit covers the wide-area lines of
 * its console slot (line & 15). */

#include "port/wide.h"

#if KATAM_WIDE

#include <string.h>

#include "global.h"
#include "main.h"
#include "palette.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XPAL(base) ((u16 *)(uintptr_t)(base))

u16 *PortWideObjPal(u32 off)
{
    if (off < 0x100u)
        return &gObjPalette[off];
    return XPAL(WIDE_OBJX_PAL) + (off - 0x100u);
}

u16 *PortWideObjSrcPal(u32 off)
{
    return XPAL(WIDE_OBJX_SRCPAL) + (off - 0x100u);
}

void PortWideObjPalRestore(void)
{
    if (WIDE_SZ_OBJX_PAL)
        memcpy(XPAL(WIDE_OBJX_PAL), XPAL(WIDE_OBJX_SRCPAL), WIDE_SZ_OBJX_PAL);
}

/* LoadObjPaletteWithTransformation(gObjPalette, 0, 0x100), for these lines. */
void PortWideObjPalTransformAll(void)
{
    u16 *p = XPAL(WIDE_OBJX_PAL);
    u32 i;

    for (i = 0; i < WIDE_OBJX_LINES * 16u; i++, p++)
        *p = gRgbMap[0][*p & 0x1F] | gRgbMap[1][(*p & 0x3E0) >> 5] | gRgbMap[2][(*p & 0x7C00) >> 10];
}

void PortWideObjPalSync(void)
{
    if (WIDE_SZ_OBJX_PAL)
        memcpy(XPAL(WIDE_OBJX_HWPAL), XPAL(WIDE_OBJX_PAL), WIDE_SZ_OBJX_PAL);
}

#ifdef __cplusplus
}
#endif

#endif /* KATAM_WIDE */
