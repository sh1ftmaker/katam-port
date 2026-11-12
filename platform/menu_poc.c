/* Front-end menu proof of concept: platform/port/menu_poc.h says what and why.
 *
 * Off unless switched on (PortMenuPoc).  The game-side half is in
 * game/src/code_08138D64.c (search for PortMenuPoc / PortPoc); this file owns
 * the pictures -- the extra Game Select entry and the settings screen -- and
 * the settings screen's model.
 *
 * --- the pictures ------------------------------------------------------------
 *
 * The game's option labels are ROM sprite frames, one picture per option per
 * language (gUnk_08385CD4), so anything new has no art.  This draws its own at
 * run time: banners and bold text, rendered from the 5x7 font below into 4bpp
 * OBJ tiles that VramMalloc hands out of the game's own sprite heap, and put
 * on screen as plain OAM entries from the game's own OAM pool (sub_08156D84),
 * so they are part of the GBA picture -- the PPU, the GPU render list, fades,
 * screenshots and recordings all see them like any other sprite.  Colours go
 * into OBJ palettes 9 (dimmed rows) and 10 (the selected row), which no
 * front-end screen uses (the labels use 2-8, the walking Kirbys 11-15).
 *
 * Where the tiles' address is kept: in the menu task's own struct, in the two
 * Sprite slots the Erase screen uses (unk3A8, unk3D0).  That screen allocates
 * them only after Game Select has freed its labels (sub_0813A9DC, which calls
 * PortMenuPocFree), the settings screen only after the same, so no two of
 * them hold tiles at once, and the task's destructor frees them either way.
 * Keeping the address in GBA memory rather than here keeps a snapshot of the
 * menu complete.
 *
 * --- the settings screen -------------------------------------------------------
 *
 * Six rows over gPortSettings (platform/ui/settings.h), the ones a first-time
 * player has a reason to touch.  A change is clamped, applied with
 * PortSettingsChanged() and saved by it -- exactly what the ImGui menu
 * (ui_menu.cpp) does -- so it takes effect at once and lands in localStorage
 * on the web, settings.ini natively (scripted native runs save nothing, as
 * with the ImGui menu).  The values are host presentation settings, not game
 * state; the screen only reads them to draw its rows.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "global.h"
#include "main.h"
#include "sprite.h"
#include "malloc_vram.h"
#include "code_08138D64.h"
#include "port/port.h"
#include "port/menu_poc.h"
#include "ui/settings.h"
#include "ui/ui.h"          /* PortSettingsChanged */

#define PAL_DIM         9           /* OBJ palette slots                    */
#define PAL_LIT         10
#define LABEL_W         176         /* Game Select's fifth banner, pixels   */
#define LABEL_OBJS      6           /* 32x16 objects: 192 px                */
#define LABEL_TILES     (LABEL_OBJS * 8)
#define DESC_OBJS       8           /* 32x8 objects: 256 px                 */
#define DESC_TILES      (DESC_OBJS * 4)
#define BANNER_MAP      ((u16 *)0x0600FD40)     /* BG0 rows 21-22: y 144..159 */
#define DESC_Y          147

/* The settings screen. */
#define ROWS            6
#define ROW_X           32
#define ROW_Y0          36
#define ROW_PITCH       18
#define ROW_W           192
#define ROW_OBJS        6
#define ROW_TILES       (ROW_OBJS * 8)
#define HEAD_W          96
#define HEAD_OBJS       3
#define HEAD_TILES      (HEAD_OBJS * 8)
#define SET_TILES       (ROWS * ROW_TILES + HEAD_TILES + DESC_TILES)

/* Colour indices, the same in both palettes except the banner's. */
enum { C_CLEAR, C_EDGE, C_STRIPE_A, C_STRIPE_B, C_SHADE, C_TEXT, C_TEXT_EDGE,
       C_DESC, C_DESC_SHADOW, C_TAB, C_TAB_EDGE, C_TAB_TEXT };

#define RGB5(r, g, b) (u16)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10))
#define COMMON RGB5(255, 255, 255), RGB5(0, 57, 165), RGB5(0, 123, 255), \
               RGB5(0, 41, 123), RGB5(255, 255, 255)

/* Picked by eye from the game's own selected (orange) and unselected (grey)
 * option banners and its blue header tabs, so the new pieces read as its. */
static const u16 kLit[16] = {
    0, RGB5(107, 74, 0), RGB5(255, 198, 0), RGB5(255, 173, 49), RGB5(222, 140, 0),
    RGB5(0, 74, 206), RGB5(255, 255, 255), COMMON
};
static const u16 kDim[16] = {
    0, RGB5(49, 49, 49), RGB5(198, 198, 198), RGB5(165, 165, 165), RGB5(115, 115, 115),
    RGB5(90, 90, 90), RGB5(33, 33, 33), COMMON
};

/* A 5x7 font, written for this file: one byte per row, bit 4 leftmost. */
static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.,:!'<>()%/";
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
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
};

#define ADVANCE 7       /* bold glyphs are 6 wide, plus a column of space */

static const char kLabel[] = "SETTINGS";
static const char kDesc[] = "CHANGE HOW THE GAME LOOKS";

static int sOn = -1;
/* The menu struct the tiles belong to; host-side, only so a draw after the
 * task destroyed itself (the struct is freed memory then) is refused. */
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

/* A banner like the game's option rows: clipped corners, two-row stripes, a
 * darker bottom, a one-pixel edge; bannerW of the canvas's w, 16 high. */
static void DrawBanner(u8 *px, int w, int bannerW)
{
    int x, y;

    for (y = 0; y < 16; y++)
        for (x = 0; x < bannerW; x++) {
            int corner = (y == 0 || y == 15) ? 2 : (y == 1 || y == 14) ? 1 : 0;
            u8 c;

            if (x < corner || x >= bannerW - corner)
                continue;
            if (y == 0 || y == 15 || x == corner || x == bannerW - 1 - corner)
                c = C_EDGE;
            else if (y >= 12)
                c = C_SHADE;
            else
                c = ((y - 1) / 2) & 1 ? C_STRIPE_B : C_STRIPE_A;
            px[y * w + x] = c;
        }
}

static void DrawLabelTiles(u32 vram)
{
    u8 px[16][LABEL_OBJS * 32];
    int w = LABEL_OBJS * 32;

    memset(px, C_CLEAR, sizeof(px));
    DrawBanner(&px[0][0], w, LABEL_W);
    DrawText(&px[0][0], w, 16, (LABEL_W - TextWidth(kLabel)) / 2, 4, kLabel,
             C_TEXT, C_TEXT_EDGE, 0);
    PackObjects(&px[0][0], w, 16, 32, vram);
}

/* A centred line for the description banner (white, dark-blue shadow). */
static void DrawDescTiles(u32 vram, const char *s)
{
    u8 px[8][DESC_OBJS * 32];
    int w = DESC_OBJS * 32;

    memset(px, C_CLEAR, sizeof(px));
    DrawText(&px[0][0], w, 8, (DISPLAY_WIDTH - TextWidth(s)) / 2, 0, s,
             C_DESC, 0, C_DESC_SHADOW);
    PackObjects(&px[0][0], w, 8, 32, vram);
}

static void SetPalettes(void)
{
    int i;

    for (i = 1; i < 16; i++) {
        gObjPalette[PAL_DIM * 16 + i] = kDim[i];
        gObjPalette[PAL_LIT * 16 + i] = kLit[i];
    }
    gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
}

static int VramOk(u32 v)
{
    return v != (u32)(uintptr_t)ewram_end;
}

void PortMenuPocAlloc(struct Unk_08138D64 *a1)
{
    if (!PortMenuPoc())
        return;
    PortMenuPocFree(a1);
    a1->unk3A8.tilesVram = VramMalloc(LABEL_TILES);
    a1->unk3D0.tilesVram = VramMalloc(DESC_TILES);
    if (!VramOk(a1->unk3A8.tilesVram) || !VramOk(a1->unk3D0.tilesVram)) {
        PortLog("[menu-poc] no sprite memory for the SETTINGS entry");
        sOwner = a1;
        PortMenuPocFree(a1);
        return;
    }
    DrawLabelTiles(a1->unk3A8.tilesVram);
    DrawDescTiles(a1->unk3D0.tilesVram, kDesc);
    sOwner = a1;
}

void PortMenuPocFree(struct Unk_08138D64 *a1)
{
    if (!PortMenuPoc())
        return;
    if (sOwner == a1) {
        if (a1->unk3A8.tilesVram && VramOk(a1->unk3A8.tilesVram))
            VramFree(a1->unk3A8.tilesVram);
        if (a1->unk3D0.tilesVram && VramOk(a1->unk3D0.tilesVram))
            VramFree(a1->unk3D0.tilesVram);
        a1->unk3A8.tilesVram = 0;
        a1->unk3D0.tilesVram = 0;
    }
    sOwner = NULL;
}

/* One plain (non-affine, 4bpp) OAM entry from the game's pool. */
static void PutObj(u8 layer, s32 x, s32 y, u16 shape, u16 size, u32 vram, int pal)
{
    OamData *o;

    if (x <= -32 || x >= DISPLAY_WIDTH || y <= -16 || y >= DISPLAY_HEIGHT)
        return;
    o = sub_08156D84(layer);
    if (o == iwram_end)
        return;
    o->all.attr0 = (u16)((y & 0xFF) | (shape << 14));
    o->all.attr1 = (u16)((x & 0x1FF) | (size << 14));
    o->all.attr2 = (u16)(((vram - 0x6010000u) >> 5) | (pal << 12));
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
    SetPalettes();
    for (o = 0; o < LABEL_OBJS; o++)            /* 32x16: shape 1, size 2 */
        PutObj(layer, x + o * 32, y, 1, 2, a1->unk3A8.tilesVram + o * 8 * 32,
               selected ? PAL_LIT : PAL_DIM);
    if (selected && a1->unk3D0.tilesVram)
        for (o = 0; o < DESC_OBJS; o++)         /* 32x8: shape 1, size 1 */
            PutObj(layer, o * 32, DESC_Y, 1, 1, a1->unk3D0.tilesVram + o * 4 * 32, PAL_DIM);
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

/* --- the settings screen -------------------------------------------------- */

static const char *const kAspect[PS_ASPECT_COUNT] = {
    "AUTO", "3:2 GBA", "4:3", "16:10", "16:9", "21:9", "FILL",
};
static const float kZoom[] = { 0.5f, 0.67f, 0.8f, 1.0f, 1.25f, 1.5f, 2.0f };
static const char *const kZoomName[] = { "0.5X", "0.67X", "0.8X", "1X", "1.25X", "1.5X", "2X" };
#define NZOOM ((int)(sizeof(kZoom) / sizeof(kZoom[0])))
static const char *const kScaling[PS_SCALE_COUNT] = { "SHARP", "FIT", "SMOOTH" };

enum { R_GRAPHICS, R_SCREEN, R_ZOOM, R_SMOOTHCAM, R_PIXELS, R_VOLUME };

static const char *const kRowName[ROWS] = {
    "GRAPHICS", "SCREEN", "ZOOM", "SMOOTH CAMERA", "PIXELS", "VOLUME",
};
static const char *const kRowDesc[ROWS] = {
    "ENHANCED CAN GO WIDESCREEN",
    "PICTURE SHAPE (ENHANCED)",
    "HOW MUCH OF THE LEVEL SHOWS",
    "SMOOTHER SCROLLING (ENHANCED)",
    "HOW THE PICTURE IS SCALED",
    "MUSIC AND SOUND EFFECTS",
};

static int ZoomIndex(void)
{
    int i, best = 0;
    float bestD = 99.0f;

    for (i = 0; i < NZOOM; i++) {
        float d = gPortSettings.zoom - kZoom[i];

        if (d < 0) d = -d;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

static void RowValue(int row, char *buf, int size)
{
    const PortSettings *s = &gPortSettings;

    switch (row) {
    case R_GRAPHICS:  snprintf(buf, size, "%s", s->renderer == PS_RENDER_GPU ? "ENHANCED" : "CLASSIC"); break;
    case R_SCREEN:    snprintf(buf, size, "%s", kAspect[s->aspect]); break;
    case R_ZOOM:      snprintf(buf, size, "%s", kZoomName[ZoomIndex()]); break;
    case R_SMOOTHCAM: snprintf(buf, size, "%s", s->smoothCam ? "ON" : "OFF"); break;
    case R_PIXELS:    snprintf(buf, size, "%s", kScaling[s->scaling]); break;
    case R_VOLUME:
        if (s->mute) snprintf(buf, size, "MUTE");
        else         snprintf(buf, size, "%d%%", s->volume);
        break;
    default: buf[0] = '\0';
    }
}

/* dir -1 or +1; wraps for the lists, stops at the ends for the numbers. */
static void RowStep(int row, int dir)
{
    PortSettings *s = &gPortSettings;

    switch (row) {
    case R_GRAPHICS:  s->renderer = s->renderer == PS_RENDER_GPU ? PS_RENDER_PPU : PS_RENDER_GPU; break;
    case R_SCREEN:    s->aspect = (s->aspect + dir + PS_ASPECT_COUNT) % PS_ASPECT_COUNT; break;
    case R_ZOOM: {
        int z = ZoomIndex() + dir;

        s->zoom = kZoom[z < 0 ? 0 : z >= NZOOM ? NZOOM - 1 : z];
        break;
    }
    case R_SMOOTHCAM: s->smoothCam = !s->smoothCam; break;
    case R_PIXELS:    s->scaling = (s->scaling + dir + PS_SCALE_COUNT) % PS_SCALE_COUNT; break;
    case R_VOLUME:
        if (s->mute) {
            s->mute = 0;
        } else {
            s->volume = (s->volume / 10) * 10 + dir * 10;
            if (s->volume < 0) s->volume = 0;
            if (s->volume > 100) s->volume = 100;
        }
        break;
    }
    /* What the settings menu does after a change (ui_menu.cpp): clamp,
     * apply, persist. */
    PortSettingsClamp(s);
    PortSettingsChanged();
}

static u32 RowVram(struct Unk_08138D64 *a1, int row)
{
    return a1->unk3A8.tilesVram + row * ROW_TILES * 32;
}

static u32 HeadVram(struct Unk_08138D64 *a1)
{
    return a1->unk3A8.tilesVram + ROWS * ROW_TILES * 32;
}

static u32 SetDescVram(struct Unk_08138D64 *a1)
{
    return HeadVram(a1) + HEAD_TILES * 32;
}

static void DrawRowTiles(struct Unk_08138D64 *a1, int row)
{
    u8 px[16][ROW_OBJS * 32];
    int w = ROW_OBJS * 32;
    char val[24], shown[32];
    int selected = a1->unk3A6 == row;

    memset(px, C_CLEAR, sizeof(px));
    DrawBanner(&px[0][0], w, ROW_W);
    DrawText(&px[0][0], w, 16, 8, 4, kRowName[row], C_TEXT, C_TEXT_EDGE, 0);
    RowValue(row, val, sizeof(val));
    snprintf(shown, sizeof(shown), selected ? "<%s>" : "%s", val);
    DrawText(&px[0][0], w, 16, ROW_W - 8 - TextWidth(shown), 4, shown,
             C_TEXT, C_TEXT_EDGE, 0);
    PackObjects(&px[0][0], w, 16, 32, RowVram(a1, row));
}

static void DrawHeadTiles(struct Unk_08138D64 *a1)
{
    u8 px[16][HEAD_OBJS * 32];
    int w = HEAD_OBJS * 32, x, y;

    memset(px, C_CLEAR, sizeof(px));
    /* A tab like the game's screen headers: blue, a darker edge, the right
     * end cut on a slant. */
    for (y = 0; y < 16; y++)
        for (x = 0; x < HEAD_W - (15 - y) / 2; x++)
            px[y][x] = (y == 0 || y == 15 || x == HEAD_W - (15 - y) / 2 - 1) ? C_TAB_EDGE : C_TAB;
    DrawText(&px[0][0], w, 16, 8, 4, "SETTINGS", C_TAB_TEXT, 0, C_TAB_EDGE);
    PackObjects(&px[0][0], w, 16, 32, HeadVram(a1));
}

void PortMenuPocSettingsOpen(struct Unk_08138D64 *a1)
{
    int r;

    PortMenuPocFree(a1);
    a1->unk3A6 = 0;
    a1->unk3A8.tilesVram = VramMalloc(SET_TILES);
    sOwner = a1;
    if (!VramOk(a1->unk3A8.tilesVram)) {
        PortLog("[menu-poc] no sprite memory for the settings screen");
        a1->unk3A8.tilesVram = 0;
        return;
    }
    for (r = 0; r < ROWS; r++)
        DrawRowTiles(a1, r);
    DrawHeadTiles(a1);
    DrawDescTiles(SetDescVram(a1), kRowDesc[0]);
    PortMenuPocBlankBanner();
}

void PortMenuPocSettingsClose(struct Unk_08138D64 *a1)
{
    PortMenuPocFree(a1);
}

int PortMenuPocSettingsCursorY(struct Unk_08138D64 *a1)
{
    /* Game Select draws the star 21 px above a row's middle (its rows are
     * 20 high, the star 11 above their y: gUnk_0838650A); ours are 16. */
    return ROW_Y0 + ROW_PITCH * a1->unk3A6 + 8 - 21;
}

void PortMenuPocSettingsDraw(struct Unk_08138D64 *a1, int slide)
{
    int r, o;
    u8 layer = 0x10;    /* the layer Game Select's rows use */

    if (sOwner != a1 || !a1->unk3A8.tilesVram)
        return;
    SetPalettes();
    for (o = 0; o < HEAD_OBJS; o++)             /* the tab slides in from the left */
        PutObj(layer, o * 32 - slide * HEAD_W / 208, 14, 1, 2,
               HeadVram(a1) + o * 8 * 32, PAL_DIM);
    for (r = 0; r < ROWS; r++)
        for (o = 0; o < ROW_OBJS; o++)
            PutObj(layer, ROW_X + slide + o * 32, ROW_Y0 + r * ROW_PITCH, 1, 2,
                   RowVram(a1, r) + o * 8 * 32, a1->unk3A6 == r ? PAL_LIT : PAL_DIM);
    for (o = 0; o < DESC_OBJS; o++)
        PutObj(layer, o * 32, DESC_Y, 1, 1, SetDescVram(a1) + o * 4 * 32, PAL_DIM);
}

int PortMenuPocSettingsInput(struct Unk_08138D64 *a1, unsigned key)
{
    int was = a1->unk3A6;

    if (!a1->unk3A8.tilesVram)
        return 0;
    switch (key) {
    case 3:     /* up */
        a1->unk3A6 = was == 0 ? ROWS - 1 : was - 1;
        break;
    case 4:     /* down */
        a1->unk3A6 = was == ROWS - 1 ? 0 : was + 1;
        break;
    case 1:     /* A: next value */
    case 6:     /* right */
        RowStep(was, 1);
        DrawRowTiles(a1, was);
        return 2;
    case 5:     /* left */
        RowStep(was, -1);
        DrawRowTiles(a1, was);
        return 2;
    default:
        return 0;
    }
    DrawRowTiles(a1, was);
    DrawRowTiles(a1, a1->unk3A6);
    DrawDescTiles(SetDescVram(a1), kRowDesc[a1->unk3A6]);
    return 1;
}
