/* Front-end menu proof of concept: platform/port/menu_poc.h says what and why.
 *
 * Off unless KATAM_MENU_POC is set.  The game-side half is in
 * game/src/code_08138D64.c (search for PortMenuPoc); this file owns the extra
 * Game Select entry's picture.
 *
 * --- the label ---------------------------------------------------------------
 *
 * The game's option labels are ROM sprite frames, one picture per option per
 * language (gUnk_08385CD4), so a new option has no art.  This draws one at
 * run time: a banner and bold text, rendered from the 5x7 font below into 4bpp
 * OBJ tiles that VramMalloc hands out of the game's own sprite heap, and put
 * on screen as plain OAM entries from the game's own OAM pool (sub_08156D84),
 * so it is part of the GBA picture -- the PPU, the GPU render list, fades,
 * screenshots and recordings all see it like any other sprite.  The colours
 * are written into OBJ palette 9, which no front-end screen uses (the labels
 * use 2-8, the walking Kirbys 11-15).
 *
 * Where the tiles' address is kept: in the menu task's own struct, in the two
 * Sprite slots the Erase screen uses (unk3A8, unk3D0).  That screen allocates
 * them only after Game Select has freed its labels (sub_0813A9DC, which calls
 * PortMenuPocFree), so the two never hold tiles at once, and the task's
 * destructor frees them either way.  Keeping the address in GBA memory rather
 * than here keeps a snapshot of the menu complete.
 */

#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "main.h"
#include "sprite.h"
#include "malloc_vram.h"
#include "code_08138D64.h"
#include "port/port.h"
#include "port/menu_poc.h"

/* The settings menu (platform/ui/ui_menu.cpp).  Weak: not every program that
 * links the game links the menu. */
#ifdef __cplusplus
extern "C" {
#endif
extern void PortUiSetOpen(int open) __attribute__((weak));
extern int PortUiIsOpen(void) __attribute__((weak));
#ifdef __cplusplus
}
#endif

#define POC_PAL         9           /* OBJ palette slot                     */
#define LABEL_W         176         /* banner, pixels                       */
#define LABEL_OBJS      6           /* 32x16 objects: 192 px                */
#define LABEL_TILES     (LABEL_OBJS * 8)
#define DESC_OBJS       8           /* 32x8 objects: 256 px                 */
#define DESC_TILES      (DESC_OBJS * 4)
#define BANNER_MAP      ((u16 *)0x0600FD40)     /* BG0 rows 21-22: y 144..159 */

/* Colour indices in POC_PAL. */
enum { C_CLEAR, C_EDGE, C_STRIPE_A, C_STRIPE_B, C_SHADE, C_TEXT, C_TEXT_EDGE,
       C_DESC, C_DESC_SHADOW };

#define RGB5(r, g, b) (u16)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10))

/* Picked by eye from the game's own selected (orange) and unselected (grey)
 * option banners, so the new entry reads as one of them. */
static const u16 kSelected[16] = {
    0, RGB5(107, 74, 0), RGB5(255, 198, 0), RGB5(255, 173, 49), RGB5(222, 140, 0),
    RGB5(0, 74, 206), RGB5(255, 255, 255), RGB5(255, 255, 255), RGB5(0, 57, 165),
};
static const u16 kUnselected[16] = {
    0, RGB5(49, 49, 49), RGB5(198, 198, 198), RGB5(165, 165, 165), RGB5(115, 115, 115),
    RGB5(90, 90, 90), RGB5(33, 33, 33), RGB5(255, 255, 255), RGB5(0, 57, 165),
};

/* A 5x7 font, written for this file: one byte per row, bit 4 leftmost. */
static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.,:!'";
static const u8 kGlyphs[][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x00,0x00,0x00,0x0E,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, {0x04,0x04,0x08,0x00,0x00,0x00,0x00},
};

#define ADVANCE 7       /* bold glyphs are 6 wide, plus a column of space */

static const char kLabel[] = "SETTINGS";
static const char kDesc[] = "OPEN THE PORT'S SETTINGS MENU";

static int sOn = -1;
/* The menu struct the tiles below belong to; host-side, only so a draw after
 * the task destroyed itself (the struct is freed memory then) is refused. */
static struct Unk_08138D64 *sOwner;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* On the web: a page served from .../menus/ or with ?menus=1. */
EM_JS(int, PortWebMenuPocParam, (void), {
    try {
        var q = new URLSearchParams(location.search);
        if (q.get('menus') === '1') return 1;
        var p = location.pathname, i = p.lastIndexOf('/');
        p = p.substring(0, i);
        if (p.substring(p.lastIndexOf('/') + 1) === 'menus') return 1;
    } catch (e) {}
    return 0;
});
#endif

int PortMenuPoc(void)
{
    if (sOn < 0) {
        const char *e = getenv("KATAM_MENU_POC");

        sOn = e != NULL && atoi(e) > 0;
#ifdef __EMSCRIPTEN__
        if (!sOn)
            sOn = PortWebMenuPocParam();
#endif
        if (sOn)
            PortLog("[menu-poc] on: no One Player/Multiplayer pop-up; "
                    "Game Select has a SETTINGS entry");
    }
    return sOn;
}

static const u8 *Glyph(char c)
{
    const char *p = strchr(kChars, c);

    return (c != ' ' && c != '\0' && p != NULL) ? kGlyphs[p - kChars] : NULL;
}

static int TextWidth(const char *s)
{
    return (int)strlen(s) * ADVANCE - 1;
}

/* Draw bold text into an 8-bit-per-pixel canvas; outline != 0 rings it with
 * that colour, shadow != 0 drops it one pixel down-right instead. */
static void DrawText(u8 *px, int w, int h, int x0, int y0, const char *s,
                     u8 fill, u8 outline, u8 shadow)
{
    int pass, i, x, y;

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; s[i] != '\0'; i++) {
            const u8 *g = Glyph(s[i]);

            if (g == NULL)
                continue;
            for (y = 0; y < 7; y++) {
                for (x = 0; x < 5; x++) {
                    int b, dx, dy;

                    if (!(g[y] & (0x10 >> x)))
                        continue;
                    for (b = 0; b < 2; b++) {           /* bold: two columns */
                        int px0 = x0 + i * ADVANCE + x + b, py0 = y0 + y;

                        if (pass == 1) {
                            if (px0 >= 0 && px0 < w && py0 >= 0 && py0 < h)
                                px[py0 * w + px0] = fill;
                            continue;
                        }
                        if (shadow) {
                            if (px0 + 1 < w && py0 + 1 < h)
                                px[(py0 + 1) * w + px0 + 1] = shadow;
                            continue;
                        }
                        if (!outline)
                            continue;
                        for (dy = -1; dy <= 1; dy++)
                            for (dx = -1; dx <= 1; dx++) {
                                int qx = px0 + dx, qy = py0 + dy;

                                if (qx >= 0 && qx < w && qy >= 0 && qy < h)
                                    px[qy * w + qx] = outline;
                            }
                    }
                }
            }
        }
    }
}

/* Pack a canvas (w a multiple of objW) into 4bpp tiles in VRAM, object by
 * object, each object's tiles row-major -- what 1D OBJ mapping expects. */
static void PackObjects(const u8 *px, int w, int h, int objW, u32 vram)
{
    int objs = w / objW, o, ty, tx, y, x;
    u8 *dst = (u8 *)(uintptr_t)vram;

    for (o = 0; o < objs; o++)
        for (ty = 0; ty < h / 8; ty++)
            for (tx = 0; tx < objW / 8; tx++)
                for (y = 0; y < 8; y++)
                    for (x = 0; x < 8; x += 2) {
                        int sx = o * objW + tx * 8 + x, sy = ty * 8 + y;

                        *dst++ = (u8)(px[sy * w + sx] | (px[sy * w + sx + 1] << 4));
                    }
}

static void DrawLabelTiles(u32 vram)
{
    u8 px[16][LABEL_OBJS * 32];
    int x, y, w = LABEL_OBJS * 32;

    memset(px, C_CLEAR, sizeof(px));
    /* A banner with clipped corners, striped like the game's: two-row
     * stripes, a darker bottom, a one-pixel edge. */
    for (y = 0; y < 16; y++)
        for (x = 0; x < LABEL_W; x++) {
            int corner = (y == 0 || y == 15) ? 2 : (y == 1 || y == 14) ? 1 : 0;
            u8 c;

            if (x < corner || x >= LABEL_W - corner)
                continue;
            if (y == 0 || y == 15 || x == corner || x == LABEL_W - 1 - corner)
                c = C_EDGE;
            else if (y >= 12)
                c = C_SHADE;
            else
                c = ((y - 1) / 2) & 1 ? C_STRIPE_B : C_STRIPE_A;
            px[y][x] = c;
        }
    DrawText(&px[0][0], w, 16, (LABEL_W - TextWidth(kLabel)) / 2, 4, kLabel,
             C_TEXT, C_TEXT_EDGE, 0);
    PackObjects(&px[0][0], w, 16, 32, vram);
}

static void DrawDescTiles(u32 vram)
{
    u8 px[8][DESC_OBJS * 32];
    int w = DESC_OBJS * 32;

    memset(px, C_CLEAR, sizeof(px));
    DrawText(&px[0][0], w, 8, (DISPLAY_WIDTH - TextWidth(kDesc)) / 2, 0, kDesc,
             C_DESC, 0, C_DESC_SHADOW);
    PackObjects(&px[0][0], w, 8, 32, vram);
}

static void SetPalette(int selected)
{
    const u16 *src = selected ? kSelected : kUnselected;
    int i;

    for (i = 1; i < 16; i++)
        gObjPalette[POC_PAL * 16 + i] = src[i];
    gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
}

void PortMenuPocAlloc(struct Unk_08138D64 *a1)
{
    if (!PortMenuPoc())
        return;
    PortMenuPocFree(a1);
    a1->unk3A8.tilesVram = VramMalloc(LABEL_TILES);
    a1->unk3D0.tilesVram = VramMalloc(DESC_TILES);
    DrawLabelTiles(a1->unk3A8.tilesVram);
    DrawDescTiles(a1->unk3D0.tilesVram);
    sOwner = a1;
}

void PortMenuPocFree(struct Unk_08138D64 *a1)
{
    if (!PortMenuPoc())
        return;
    if (sOwner == a1) {
        if (a1->unk3A8.tilesVram)
            VramFree(a1->unk3A8.tilesVram);
        if (a1->unk3D0.tilesVram)
            VramFree(a1->unk3D0.tilesVram);
        a1->unk3A8.tilesVram = 0;
        a1->unk3D0.tilesVram = 0;
    }
    sOwner = NULL;
}

/* One plain (non-affine, 4bpp) OAM entry from the game's pool. */
static void PutObj(u8 layer, s32 x, s32 y, u16 shape, u16 size, u32 vram)
{
    OamData *o;

    if (x <= -32 || x >= DISPLAY_WIDTH || y <= -16 || y >= DISPLAY_HEIGHT)
        return;
    o = sub_08156D84(layer);
    if (o == iwram_end)
        return;
    o->all.attr0 = (u16)((y & 0xFF) | (shape << 14));
    o->all.attr1 = (u16)((x & 0x1FF) | (size << 14));
    o->all.attr2 = (u16)(((vram - 0x6010000u) >> 5) | (POC_PAL << 12));
}

void PortMenuPocDraw(struct Unk_08138D64 *a1)
{
    struct Sprite *last;
    s32 x, y, dy;
    u8 layer;
    int o, selected;

    if (!PortMenuPoc() || sOwner != a1 || a1->unk1 != 2 || !a1->unk3A8.tilesVram)
        return;
    /* Ride along with the fourth entry, one row further down: whatever slide
     * or spread the game is animating, the new row follows it. */
    last = &a1->unk2DC[3];
    dy = last->y - a1->unk2DC[2].y;
    x = last->x;
    y = last->y + dy + 2;           /* our banner is 16 px, theirs 20: centre it */
    layer = (last->unk14 & 0x7C0) >> 6;
    selected = a1->unk3A4 == PORT_MENU_POC_SETTINGS;
    SetPalette(selected);
    for (o = 0; o < LABEL_OBJS; o++)            /* 32x16: shape 1, size 2 */
        PutObj(layer, x + o * 32, y, 1, 2, a1->unk3A8.tilesVram + o * 8 * 32);
    if (selected && a1->unk3D0.tilesVram)
        for (o = 0; o < DESC_OBJS; o++)         /* 32x8: shape 1, size 1 */
            PutObj(layer, o * 32, 147, 1, 1, a1->unk3D0.tilesVram + o * 4 * 32);
}

void PortMenuPocBlankBanner(void)
{
    int row, col;

    /* Each row's leftmost entry is plain banner; repeat it across. */
    for (row = 0; row < 2; row++) {
        u16 *r = BANNER_MAP + row * 32;

        for (col = 1; col < 30; col++)
            r[col] = r[0];
    }
}

void PortMenuPocOpenSettings(void)
{
    if (PortUiSetOpen != NULL) {
        PortUiSetOpen(1);
        PortLog("[menu-poc] SETTINGS chosen: settings menu %s",
                PortUiIsOpen != NULL && PortUiIsOpen() ? "opened"
                : "requested (no menu in this run; --ui or an interactive window draws it)");
    } else {
        PortLog("[menu-poc] SETTINGS chosen, but this program has no settings menu");
    }
}
