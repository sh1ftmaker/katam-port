/* Software PPU.
 *
 * The game talks to the display the way it always did: it writes tiles into
 * VRAM, entries into OAM, colours into palette RAM, and control values into
 * REG_DISPCNT / REG_BGxCNT / REG_BLDCNT.  Nothing in the game changed.  This
 * file is the other half of that conversation -- it reads the same memory the
 * hardware would have read and produces the 240x160 image.
 *
 * It renders one scanline at a time, because that is the only way the game's
 * per-scanline effects work: main.c arms DMA channel 0 to feed a register a
 * fresh value every HBlank, and installs an HBlank handler.  Both are driven
 * from the scanline loop below.
 *
 * Implemented: modes 0-2 (text and affine backgrounds), regular and affine
 * sprites in both 1D and 2D mapping, all four priorities, the two windows plus
 * the object window, alpha blending and brightness fade, mosaic on backgrounds,
 * and forced blank.  Modes 3-5 are the bitmap modes, which this game does not
 * use; they report themselves through PortUnimplemented if they ever appear.
 */

#include <string.h>

#include "port/port.h"
#include "port/dma.h"
#include "gba/gba.h"
#include "main.h"

#define SCREEN_W PORT_SCREEN_W
#define SCREEN_H PORT_SCREEN_H

#define NUM_BG 4
#define LAYER_OBJ 4
#define NUM_LAYERS 5

u32 gPortFramebuffer[SCREEN_W * SCREEN_H];

/* Per-layer scanline output.  `colour` holds BGR555; `opaque` is 0 or 1. */
static u16 sLayerColour[NUM_LAYERS][SCREEN_W];
static u8 sLayerOpaque[NUM_LAYERS][SCREEN_W];
static u8 sLayerPriority[NUM_LAYERS];
static u8 sObjSemiTransparent[SCREEN_W];
static u8 sObjPriority[SCREEN_W];
static u8 sObjWindow[SCREEN_W];

/* Affine background reference points latch at the start of the frame and
 * advance per scanline, so mid-frame writes to BGxX/BGxY do not take effect
 * until the next frame -- games depend on that. */
static s32 sAffineX[2], sAffineY[2];

#define IO16(off) (*(vu16 *)(GBA_IO_BASE + (off)))

static const u16 *Palette(void) { return (const u16 *)GBA_PLTT_BASE; }
static const u8 *Vram(void) { return (const u8 *)GBA_VRAM_BASE; }
static const u16 *Oam(void) { return (const u16 *)GBA_OAM_BASE; }

/* --- tile fetch ---------------------------------------------------------- */

static u16 FetchTilePixel(u32 charBase, u32 tile, u32 x, u32 y, int is256,
                          u32 palBank, u16 *outIndex)
{
    const u8 *vram = Vram();
    u32 offset;
    u8 index;

    /* Every caller passes an uninitialised local and then branches on it, so
     * the out-of-range paths below have to write it before returning -- leaving
     * it alone turned a dropped pixel into an indeterminate one, opaque with
     * colour 0 whenever the stack happened to hold non-zero. */
    *outIndex = 0;

    if (is256) {
        offset = charBase + tile * 64 + y * 8 + x;
        if (offset >= GBA_VRAM_SIZE)
            return 0;
        index = vram[offset];
        *outIndex = index;
        return index;
    }

    offset = charBase + tile * 32 + y * 4 + (x >> 1);
    if (offset >= GBA_VRAM_SIZE)
        return 0;
    index = vram[offset];
    index = (x & 1) ? (index >> 4) : (index & 0xF);
    *outIndex = index;
    return index ? (palBank * 16 + index) : 0;
}

/* --- text backgrounds ---------------------------------------------------- */

static void RenderTextBg(int bg, int line)
{
    u16 cnt = IO16(REG_OFFSET_BG0CNT + bg * 2);
    u32 charBase = ((cnt >> 2) & 3) * 0x4000;
    u32 screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    int is256 = (cnt & BGCNT_256COLOR) != 0;
    u32 size = (cnt >> 14) & 3;
    u32 widthTiles = (size & 1) ? 64 : 32;
    u32 heightTiles = (size & 2) ? 64 : 32;
    u16 hofs = IO16(REG_OFFSET_BG0HOFS + bg * 4);
    u16 vofs = IO16(REG_OFFSET_BG0VOFS + bg * 4);
    const u16 *pal = Palette();
    const u8 *vram = Vram();
    u32 sy, x;

    if (cnt & BGCNT_MOSAIC) {
        u16 mos = IO16(REG_OFFSET_MOSAIC);
        u32 mv = ((mos >> 4) & 0xF) + 1;
        line = (line / mv) * mv;
    }

    sy = ((u32)(line + vofs)) & (heightTiles * 8 - 1);

    for (x = 0; x < SCREEN_W; x++) {
        u32 sx = ((u32)(x + hofs)) & (widthTiles * 8 - 1);
        u32 tileX = sx >> 3, tileY = sy >> 3;
        u32 mapOffset = screenBase;
        u16 entry, tile, index, colour;
        u32 px, py;

        /* 512-wide and 512-tall maps are stored as 32x32 blocks. */
        if (tileX >= 32) {
            mapOffset += 0x800;
            tileX -= 32;
        }
        if (tileY >= 32) {
            mapOffset += (widthTiles > 32) ? 0x1000 : 0x800;
            tileY -= 32;
        }
        mapOffset += (tileY * 32 + tileX) * 2;
        if (mapOffset + 1 >= GBA_VRAM_SIZE)
            continue;

        entry = vram[mapOffset] | (vram[mapOffset + 1] << 8);
        tile = entry & 0x3FF;
        px = sx & 7;
        py = sy & 7;
        if (entry & 0x400) px = 7 - px;
        if (entry & 0x800) py = 7 - py;

        colour = FetchTilePixel(charBase, tile, px, py, is256,
                                (entry >> 12) & 0xF, &index);
        if (index) {
            sLayerColour[bg][x] = pal[colour & 0xFF];
            sLayerOpaque[bg][x] = 1;
        }
    }
}

/* --- affine backgrounds -------------------------------------------------- */

static void RenderAffineBg(int bg, int line)
{
    int slot = bg - 2;                       /* only BG2 and BG3 can be affine */
    u16 cnt = IO16(REG_OFFSET_BG0CNT + bg * 2);
    u32 charBase = ((cnt >> 2) & 3) * 0x4000;
    u32 screenBase = ((cnt >> 8) & 0x1F) * 0x800;
    u32 sizeTiles = 16 << ((cnt >> 14) & 3);  /* 16, 32, 64 or 128 tiles */
    u32 sizePixels = sizeTiles * 8;
    int wrap = (cnt & BGCNT_WRAP) != 0;
    s16 pa = (s16)IO16(REG_OFFSET_BG2PA + slot * 0x10);
    s16 pc = (s16)IO16(REG_OFFSET_BG2PC + slot * 0x10);
    const u16 *pal = Palette();
    const u8 *vram = Vram();
    s32 cx = sAffineX[slot], cy = sAffineY[slot];
    u32 x;

    (void)line;
    for (x = 0; x < SCREEN_W; x++) {
        s32 tx = (cx + pa * (s32)x) >> 8;
        s32 ty = (cy + pc * (s32)x) >> 8;
        u32 mapOffset, tile;
        u16 colour, index;

        if (wrap) {
            tx &= (s32)(sizePixels - 1);
            ty &= (s32)(sizePixels - 1);
        } else if (tx < 0 || ty < 0 || (u32)tx >= sizePixels || (u32)ty >= sizePixels) {
            continue;
        }

        mapOffset = screenBase + ((u32)ty >> 3) * sizeTiles + ((u32)tx >> 3);
        if (mapOffset >= GBA_VRAM_SIZE)
            continue;
        tile = vram[mapOffset];        /* affine maps are 8-bit tile indices */

        /* Affine backgrounds are always 256-colour. */
        colour = FetchTilePixel(charBase, tile, tx & 7, ty & 7, 1, 0, &index);
        if (index) {
            sLayerColour[bg][x] = pal[colour & 0xFF];
            sLayerOpaque[bg][x] = 1;
        }
    }
}

/* --- sprites ------------------------------------------------------------- */

static const u8 sObjWidth[4][4] = {
    { 8, 16, 32, 64 },   /* square */
    { 16, 32, 32, 64 },  /* wide */
    { 8, 8, 16, 32 },    /* tall */
    { 8, 8, 8, 8 },      /* prohibited */
};
static const u8 sObjHeight[4][4] = {
    { 8, 16, 32, 64 },
    { 8, 8, 16, 32 },
    { 16, 32, 32, 64 },
    { 8, 8, 8, 8 },
};

static void RenderSprites(int line)
{
    u16 dispcnt = IO16(REG_OFFSET_DISPCNT);
    int oneD = (dispcnt & DISPCNT_OBJ_1D_MAP) != 0;
    const u16 *oam = Oam();
    const u16 *pal = Palette() + 0x100;      /* object palette */
    int i;

    for (i = 127; i >= 0; i--) {
        u16 a0 = oam[i * 4 + 0];
        u16 a1 = oam[i * 4 + 1];
        u16 a2 = oam[i * 4 + 2];
        int affine = (a0 & 0x100) != 0;
        int disabled = !affine && (a0 & 0x200);
        u32 mode = (a0 >> 10) & 3;
        int is256 = (a0 & 0x2000) != 0;
        u32 shape = (a0 >> 14) & 3;
        u32 sizeBits = (a1 >> 14) & 3;
        u32 w = sObjWidth[shape][sizeBits];
        u32 h = sObjHeight[shape][sizeBits];
        u32 boxW = w, boxH = h;
        s32 y = a0 & 0xFF;
        s32 x = a1 & 0x1FF;
        u32 tile = a2 & 0x3FF;
        u32 palBank = (a2 >> 12) & 0xF;
        u32 priority = (a2 >> 10) & 3;
        s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        s32 row, px;

        if (disabled || mode == 3)
            continue;

        if (affine && (a0 & 0x200)) {       /* double-size affine sprite */
            boxW = w * 2;
            boxH = h * 2;
        }

        if (x >= 240) x -= 512;
        if (y >= 160) y -= 256;

        row = line - y;
        if (row < 0 || row >= (s32)boxH)
            continue;

        if (affine) {
            u32 slot = (a1 >> 9) & 0x1F;
            pa = (s16)oam[slot * 16 + 3];
            pb = (s16)oam[slot * 16 + 7];
            pc = (s16)oam[slot * 16 + 11];
            pd = (s16)oam[slot * 16 + 15];
        }

        for (px = 0; px < (s32)boxW; px++) {
            s32 sx = x + px;
            s32 texX, texY;
            u32 tileIndex;
            u16 index;
            u16 colour;

            if (sx < 0 || sx >= SCREEN_W)
                continue;

            if (affine) {
                s32 dx = px - (s32)boxW / 2;
                s32 dy = row - (s32)boxH / 2;
                texX = ((pa * dx + pb * dy) >> 8) + (s32)w / 2;
                texY = ((pc * dx + pd * dy) >> 8) + (s32)h / 2;
            } else {
                texX = (a1 & 0x1000) ? (s32)w - 1 - px : px;     /* h-flip */
                texY = (a1 & 0x2000) ? (s32)h - 1 - row : row;   /* v-flip */
            }

            if (texX < 0 || texY < 0 || (u32)texX >= w || (u32)texY >= h)
                continue;

            /* 1D mapping lays tiles out consecutively; 2D uses a 32-tile-wide
             * sheet.  In 256-colour mode each tile takes two slots. */
            if (oneD) {
                tileIndex = tile + ((u32)texY / 8) * (w / 8) * (is256 ? 2 : 1)
                                 + ((u32)texX / 8) * (is256 ? 2 : 1);
            } else {
                tileIndex = tile + ((u32)texY / 8) * 32 + ((u32)texX / 8) * (is256 ? 2 : 1);
            }

            /* OBJ tile numbers always index 32-byte slots, whatever the depth
             * -- unlike a background, where a 256-colour map entry counts in
             * 64-byte units.  FetchTilePixel works in the background's units,
             * so an 8bpp sprite has to be handed half the slot number, and the
             * low bit of the base is ignored exactly as hardware ignores it. */
            colour = FetchTilePixel(0x10000, is256 ? (tileIndex & ~1u) >> 1
                                                   : tileIndex,
                                    texX & 7, texY & 7,
                                    is256, palBank, &index);
            if (!index)
                continue;

            if (mode == 2) {           /* object window: shape only, no pixels */
                sObjWindow[sx] = 1;
                continue;
            }

            /* Lower OAM index wins; the loop runs backwards so later writes
             * are higher-priority sprites. */
            sLayerColour[LAYER_OBJ][sx] = pal[colour & 0xFF];
            sLayerOpaque[LAYER_OBJ][sx] = 1;
            sObjPriority[sx] = priority;
            sObjSemiTransparent[sx] = (mode == 1);
        }
    }
}

/* --- windows ------------------------------------------------------------- */

static int WindowAllows(int x, int line, int layer, int *allowBlend)
{
    u16 dispcnt = IO16(REG_OFFSET_DISPCNT);
    u16 winin = IO16(REG_OFFSET_WININ);
    u16 winout = IO16(REG_OFFSET_WINOUT);
    int anyWindow = (dispcnt & (DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON)) != 0;
    int w;

    *allowBlend = 1;
    if (!anyWindow)
        return 1;

    for (w = 0; w < 2; w++) {
        u16 h, v;
        int left, right, top, bottom, control;

        if (!(dispcnt & (DISPCNT_WIN0_ON << w)))
            continue;
        h = IO16(REG_OFFSET_WIN0H + w * 2);
        v = IO16(REG_OFFSET_WIN0V + w * 2);
        left = h >> 8; right = h & 0xFF;
        top = v >> 8; bottom = v & 0xFF;
        if (right > SCREEN_W || right < left) right = SCREEN_W;
        if (bottom > SCREEN_H || bottom < top) bottom = SCREEN_H;

        if (x >= left && x < right && line >= top && line < bottom) {
            control = (winin >> (w * 8)) & 0x3F;
            *allowBlend = (control & 0x20) != 0;
            return (control >> layer) & 1;
        }
    }

    if ((dispcnt & DISPCNT_OBJWIN_ON) && sObjWindow[x]) {
        int control = (winout >> 8) & 0x3F;
        *allowBlend = (control & 0x20) != 0;
        return (control >> layer) & 1;
    }

    *allowBlend = (winout & 0x20) != 0;
    return (winout >> layer) & 1;
}

/* --- blending ------------------------------------------------------------ */

static u32 ToRgba(u16 c)
{
    u32 r = (c & 0x1F) << 3;
    u32 g = ((c >> 5) & 0x1F) << 3;
    u32 b = ((c >> 10) & 0x1F) << 3;

    /* Replicate the top bits into the low ones so full-scale 31 maps to 255. */
    r |= r >> 5; g |= g >> 5; b |= b >> 5;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static u16 BlendAlpha(u16 top, u16 bottom, u32 eva, u32 evb)
{
    u32 r = ((top & 0x1F) * eva + (bottom & 0x1F) * evb) >> 4;
    u32 g = (((top >> 5) & 0x1F) * eva + ((bottom >> 5) & 0x1F) * evb) >> 4;
    u32 b = (((top >> 10) & 0x1F) * eva + ((bottom >> 10) & 0x1F) * evb) >> 4;

    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return r | (g << 5) | (b << 10);
}

static u16 BlendBrightness(u16 c, u32 evy, int up)
{
    u32 r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;

    if (up) {
        r += ((31 - r) * evy) >> 4;
        g += ((31 - g) * evy) >> 4;
        b += ((31 - b) * evy) >> 4;
    } else {
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }
    return r | (g << 5) | (b << 10);
}

/* --- scanline composition ------------------------------------------------ */

static u16 EffectiveDispcnt(void);

static void ComposeScanline(int line)
{
    /* Must be the same view of the enable bits that RenderScanline used.
     * Reading the register directly here meant a layer forced on for debugging
     * was rendered and then dropped at composition, so the tool reported an
     * empty layer whatever was on it. */
    u16 dispcnt = EffectiveDispcnt();
    u16 bldcnt = IO16(REG_OFFSET_BLDCNT);
    u16 bldalpha = IO16(REG_OFFSET_BLDALPHA);
    u16 bldy = IO16(REG_OFFSET_BLDY);
    u32 effect = (bldcnt >> 6) & 3;
    u32 eva = bldalpha & 0x1F, evb = (bldalpha >> 8) & 0x1F;
    u32 evy = bldy & 0x1F;
    const u16 *pal = Palette();
    u32 *out = &gPortFramebuffer[line * SCREEN_W];
    int x;

    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;

    for (x = 0; x < SCREEN_W; x++) {
        u16 topColour = pal[0];
        u16 secondColour = pal[0];
        int topLayer = -1, secondLayer = -1;
        int priority, layer, allowBlend = 1, topAllowsBlend = 1;
        u16 result;

        /* Resolve visibility: lowest priority value wins, and within the same
         * priority the object layer beats a background, which beats the one
         * with the higher index. */
        for (priority = 0; priority < 4 && secondLayer < 0; priority++) {
            for (layer = 0; layer < NUM_LAYERS; layer++) {
                int idx = (layer == 0) ? LAYER_OBJ : layer - 1;

                {
                    int layerPriority = (idx == LAYER_OBJ) ? sObjPriority[x]
                                                           : sLayerPriority[idx];
                    if (layerPriority != priority || !sLayerOpaque[idx][x])
                        continue;
                }
                if (idx < NUM_BG && !(dispcnt & (DISPCNT_BG0_ON << idx)))
                    continue;
                if (idx == LAYER_OBJ && !(dispcnt & DISPCNT_OBJ_ON))
                    continue;
                if (!WindowAllows(x, line, idx == LAYER_OBJ ? 4 : idx, &allowBlend))
                    continue;

                if (topLayer < 0) {
                    topLayer = idx;
                    topColour = sLayerColour[idx][x];
                    topAllowsBlend = allowBlend;
                } else if (secondLayer < 0) {
                    secondLayer = idx;
                    secondColour = sLayerColour[idx][x];
                    break;
                }
            }
        }

        result = (topLayer < 0) ? pal[0] : topColour;

        if (topAllowsBlend && topLayer >= 0) {
            int topBit = (topLayer == LAYER_OBJ) ? 4 : topLayer;
            int secondBit = (secondLayer < 0) ? 5
                          : (secondLayer == LAYER_OBJ) ? 4 : secondLayer;
            int isTarget1 = (bldcnt >> topBit) & 1;
            int isTarget2 = (bldcnt >> (8 + secondBit)) & 1;

            /* A semi-transparent sprite blends regardless of BLDCNT's first
             * target, which is how the game fades individual objects. */
            if (topLayer == LAYER_OBJ && sObjSemiTransparent[x] && isTarget2)
                result = BlendAlpha(topColour, secondColour, eva, evb);
            else if (effect == 1 && isTarget1 && isTarget2)
                result = BlendAlpha(topColour, secondColour, eva, evb);
            else if (effect == 2 && isTarget1)
                result = BlendBrightness(topColour, evy, 1);
            else if (effect == 3 && isTarget1)
                result = BlendBrightness(topColour, evy, 0);
        }

        out[x] = ToRgba(result);
    }
}

/* --- frame --------------------------------------------------------------- */

static void LatchAffineReferencePoints(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        u32 lo = IO16(REG_OFFSET_BG2X_L + i * 0x10);
        u32 hi = IO16(REG_OFFSET_BG2X_H + i * 0x10);
        s32 x = (s32)((lo | (hi << 16)) << 4) >> 4;   /* 28-bit signed */

        lo = IO16(REG_OFFSET_BG2Y_L + i * 0x10);
        hi = IO16(REG_OFFSET_BG2Y_H + i * 0x10);
        sAffineX[i] = x;
        sAffineY[i] = (s32)((lo | (hi << 16)) << 4) >> 4;
    }
}

static void AdvanceAffineReferencePoints(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        sAffineX[i] += (s16)IO16(REG_OFFSET_BG2PB + i * 0x10);
        sAffineY[i] += (s16)IO16(REG_OFFSET_BG2PD + i * 0x10);
    }
}

/* Debug: force layers off without touching the game's own DISPCNT.
 *
 * "The room does not draw" is a question about which layer holds what, and the
 * only way to answer it is to look at one layer at a time.  Bit 0-3 are BG0-3,
 * bit 4 is OBJ; a zero bit blanks that layer.  Defaults to everything on, so
 * this costs one AND per scanline when unused. */
u32 gPortLayerMask = 0x1F;
u32 gPortLayerForce = 0;

/* andMask blanks layers the game enabled; orMask draws layers it disabled --
 * needed because "what is on the layer the game turned off" is exactly the
 * question when a level does not appear. */
void PortSetLayerMask(u32 andMask, u32 orMask)
{
    gPortLayerMask = andMask;
    gPortLayerForce = orMask;
}

/* The game's DISPCNT with the debug mask applied.  Nothing the game reads back
 * changes -- the register itself is untouched. */
static u16 EffectiveDispcnt(void)
{
    u16 dispcnt = IO16(REG_OFFSET_DISPCNT);

    if (gPortLayerMask == 0x1F && gPortLayerForce == 0)
        return dispcnt;
    dispcnt &= ~((u16)((~gPortLayerMask & 0xF) * DISPCNT_BG0_ON));
    dispcnt |= (u16)((gPortLayerForce & 0xF) * DISPCNT_BG0_ON);
    if (!(gPortLayerMask & 0x10))
        dispcnt &= ~DISPCNT_OBJ_ON;
    if (gPortLayerForce & 0x10)
        dispcnt |= DISPCNT_OBJ_ON;
    return dispcnt;
}

static void RenderScanline(int line)
{
    u16 dispcnt = EffectiveDispcnt();
    u32 mode = dispcnt & 7;
    int bg;

    for (bg = 0; bg < NUM_LAYERS; bg++) {
        memset(sLayerOpaque[bg], 0, SCREEN_W);
        sLayerPriority[bg] = 3;
    }
    memset(sObjSemiTransparent, 0, SCREEN_W);
    memset(sObjPriority, 3, SCREEN_W);
    memset(sObjWindow, 0, SCREEN_W);

    if (dispcnt & DISPCNT_FORCED_BLANK) {
        u32 *out = &gPortFramebuffer[line * SCREEN_W];
        int x;
        for (x = 0; x < SCREEN_W; x++)
            out[x] = 0xFFFFFFFFu;
        return;
    }

    for (bg = 0; bg < NUM_BG; bg++)
        sLayerPriority[bg] = IO16(REG_OFFSET_BG0CNT + bg * 2) & 3;

    switch (mode) {
    case 0:
        for (bg = 0; bg < 4; bg++)
            if (dispcnt & (DISPCNT_BG0_ON << bg))
                RenderTextBg(bg, line);
        break;
    case 1:
        for (bg = 0; bg < 2; bg++)
            if (dispcnt & (DISPCNT_BG0_ON << bg))
                RenderTextBg(bg, line);
        if (dispcnt & DISPCNT_BG2_ON)
            RenderAffineBg(2, line);
        break;
    case 2:
        for (bg = 2; bg < 4; bg++)
            if (dispcnt & (DISPCNT_BG0_ON << bg))
                RenderAffineBg(bg, line);
        break;
    default:
        PortUnimplemented("bitmap display mode");
        break;
    }

    if (dispcnt & DISPCNT_OBJ_ON)
        RenderSprites(line);

    ComposeScanline(line);
}

void PortRenderFrame(void)
{
    int line;

    LatchAffineReferencePoints();

    for (line = 0; line < SCREEN_H; line++) {
        IO16(REG_OFFSET_VCOUNT) = line;
        IO16(REG_OFFSET_DISPSTAT) &= ~(DISPSTAT_VBLANK | DISPSTAT_HBLANK);

        RenderScanline(line);
        AdvanceAffineReferencePoints();

        /* HBlank: the per-scanline effects the game arms every frame. */
        IO16(REG_OFFSET_DISPSTAT) |= DISPSTAT_HBLANK;
        PortDmaHBlank(line);
        if (IO16(REG_OFFSET_DISPSTAT) & DISPSTAT_HBLANK_INTR)
            PortDispatchInterrupt(INTR_FLAG_HBLANK);
        if ((IO16(REG_OFFSET_DISPSTAT) & DISPSTAT_VCOUNT_INTR)
            && (IO16(REG_OFFSET_DISPSTAT) >> 8) == (u32)line)
            PortDispatchInterrupt(INTR_FLAG_VCOUNT);
    }
}
