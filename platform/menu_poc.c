/* Front-end menu proof of concept: platform/port/menu_poc.h says what and why.
 *
 * Off unless switched on (PortMenuPoc).  The game-side half is in
 * game/src/code_08138D64.c (search for PortMenuPoc / PortPoc); this file owns
 * the pictures -- the extra Game Select entry and the settings screen -- and
 * the settings screen's model.
 *
 * --- the pictures ------------------------------------------------------------
 *
 * Made to look like the game's own by being made *of* the game's own: at run
 * time, from the ROM the player loaded, never stored anywhere.
 *
 *   - Banners: a Game Select option label (the ERASE A FILE frame, in the
 *     game's language) decoded from the sprite tables, its lettering cleared
 *     by colour, so the shape, bullet, stripes and bevel are the game's.  The
 *     settings rows use the same banner, widened by repeating a column of the
 *     plain middle.
 *   - The Game Select row's "SETTINGS": letters cut from the other labels
 *     (S, E, T, G from START GAME, I from ERASE A FILE, N from COLLECTION
 *     ROOM).  The settings screen's header: the SUB-GAMES header (its S icon)
 *     with letters cut from the other headers.  The description lines:
 *     letters cut from the game's description pictures.
 *   - Where the game has no letter to cut (the settings rows need digits, Z,
 *     X, P, H, ...), or the language is not English, or a source does not
 *     look the way this file expects: a font written here on the same 2x2
 *     cell grid and in the same colour indices as the game's label lettering.
 *
 * All of it is 4bpp colour *indices*, so it is shown with the game's own
 * palette slots: 2 (selected) and 3 (dimmed) for banners, 0 for the header,
 * and for the description lines a copy, made every frame, of the BG palette
 * line the game's description banner uses (the menu cycles it).  Tiles come
 * from VramMalloc, OAM entries from the game's own pool (sub_08156D84), so
 * it is part of the GBA picture -- both renderers, fades, screenshots and
 * recordings see it like any other sprite.
 *
 * Which pixels to cut is coordinates in this file: where a letter sits in a
 * label, never what it looks like.  Each cut is checked against the decoded
 * picture first (the letter's columns hold its colour, the columns either
 * side do not); a failed check uses the fallback font for that text.
 *
 * Where the tiles' address is kept: in the menu task's own struct, in the
 * Sprite slot the Erase screen uses (unk3A8).  That screen allocates it only
 * after Game Select has freed its labels (sub_0813A9DC, which calls
 * PortMenuPocFree), the settings screen only after the same, so no two of
 * them hold tiles at once, and the task's destructor frees it either way.
 *
 * --- the settings screen -------------------------------------------------------
 *
 * Shown in two places from the same functions (PortPocScreen*): the front
 * end's settings screen, and the pause menu's settings page
 * (game/src/pause_settings.c), which supplies its own sprite memory and
 * palette lines and draws the same rows, header, description and cursor.
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
#include "data.h"
#include "gba/syscall.h"
#include "constants/languages.h"
#include "code_08138D64.h"
#include "port/port.h"
#include "port/menu_poc.h"
#include "port/localmp.h"
#include "port/simblock.h"
#include "ui/settings.h"
#include "ui/ui.h"          /* PortSettingsChanged */

/* Palette slots: the game's own, plus one for the description line. */
#define PAL_HEADER      0           /* the screen header (unkC's palId)     */
#define PAL_LIT         2           /* a selected option label              */
#define PAL_DIM         3           /* a dimmed one                         */
#define PAL_DESC        9           /* copy of the description's BG line    */

#define BANNER_MAP      ((u16 *)0x0600FD40)     /* BG0 rows 21-22: y 144..159 */
#define DESC_Y          144

/* Colour indices of the game's pictures (checked when decoded). */
#define LBL_FILL        1           /* label lettering                      */
#define LBL_EDGE        7           /* its outline                          */
#define HDR_BG          1           /* header tab                           */
#define HDR_TEXT        7           /* header lettering                     */
#define DSC_TEXT        7           /* description lettering                */

/* Geometry of the decoded pictures (English and every other language: the
 * art is the same shape, only the lettering differs). */
#define LBL_W           176
#define LBL_H           24
#define LBL_TEXT_X0     52          /* lettering lives in [X0, X1)          */
#define LBL_TEXT_X1     163
#define LBL_BG_X        50          /* a plain column left of it            */
#define LBL_TEXT_LEFT   55          /* where the first letter's ink starts  */
#define LBL_ROW0        4           /* outline rows 4 and 15, ink 5..14     */
#define LBL_ROW1        15
#define HDR_W           152
#define HDR_H           24
#define HDR_TEXT_X0     47
#define HDR_TEXT_X1     134
#define HDR_TEXT_LEFT   49
#define HDR_ROW0        7
#define HDR_ROW1        16

/* Pictures in VRAM: 32-px columns, each a 32x16 object over a 32x8 one. */
#define COL_TILES       12
#define PIC_H           24

/* Game Select's extra row: label + description line. */
#define GS_LABEL_COLS   6           /* 176 px                               */
#define GS_DESC_COLS    8           /* 240 px                               */
#define GS_TILES        ((GS_LABEL_COLS + GS_DESC_COLS) * COL_TILES)

/* The settings screen. */
#define ROWS            6
#define ROW_W           208         /* the banner, widened by 32            */
#define ROW_WIDEN_AT    100         /* the column repeated to widen it      */
#define ROW_COLS        7
#define ROW_FRAME_X     16          /* left of the banner's frame           */
#define ROW_Y0          30          /* top of the first row's frame; low enough that the cursor (its glow frame is a 32-px orb) clears the header tab about as the game's does */
#define ROW_PITCH       18
#define ROW_VALUE_X1    192         /* values end here                      */
#define HEAD_COLS       5
#define SET_TILES       ((ROWS * ROW_COLS + HEAD_COLS + GS_DESC_COLS) * COL_TILES)
typedef char SetTilesMatchHeader[SET_TILES == PORT_POC_SCREEN_TILES ? 1 : -1];
typedef char PauseObjFits[PORT_POC_PAUSE_CURSOR_VRAM + 0x10 * 32
                          <= PORT_POC_PAUSE_OBJ_VRAM + PORT_POC_PAUSE_OBJ_BYTES ? 1 : -1];

static int sOn = -1;
/* KATAM_MENU_POC=2: as 1, but never cut letters from the game's pictures --
 * what a language other than English gets (for testing the fallback). */
static int sNoCuts;
/* The menu struct the tiles belong to; host-side, only so a draw after the
 * task destroyed itself (the struct is freed memory then) is refused. */
static struct Unk_08138D64 *sOwner;

/* Decoded geometry and scratch (host memory; rebuilt from the ROM). */
static s16 sLblOffX, sLblOffY, sHdrOffX, sHdrOffY;
static u8 sRowBanner[PIC_H * ROW_COLS * 32];   /* cleared, widened banner */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* On the web: on in a browser unless the page has ?menus=0.  Off under node
 * (the regression harness), where there is no location. */
EM_JS(int, PortWebMenuPocParam, (void), {
    try {
        if (typeof location === 'undefined') return 0;
        var q = new URLSearchParams(location.search);
        return q.get('menus') === '0' ? 0 : 1;
    } catch (e) {}
    return 0;
});
#endif

int PortMenuPoc(void)
{
    if (sOn < 0) {
        const char *e = getenv("KATAM_MENU_POC");

        sOn = e != NULL && atoi(e) > 0;
        sNoCuts = e != NULL && atoi(e) == 2;
#ifdef __EMSCRIPTEN__
        if (!sOn)
            sOn = PortWebMenuPocParam();
#endif
        if (sOn)
            PortLog("[menu-poc] on: Game Select has a SETTINGS entry; "
                    "Start Game > MULTIPLAYER is LOCAL / ONLINE");
    }
    return sOn;
}

/* --- decoding the game's pictures ------------------------------------------ */

/* The first frame an animation variant shows (its script's first
 * ShowFrame) and the tiles its GetTiles put in the sprite's VRAM before it;
 * command sizes from the AnimCmd_* structs, sprite.h. */
static int AnimFirstFrame(u16 animId, u8 variant, u32 *frame, s32 *tile0, u32 *ntiles)
{
    static const u8 kCmdWords[13] = { 0, 3, 3, 2, 1, 2, 3, 2, 3, 2, 4, 2, 2 };
    const s32 *w = gSpriteTables->anims[animId][variant].words;
    int n;

    *tile0 = -1;
    *ntiles = 0;
    for (n = 0; n < 64; n++) {
        s32 cmd = w[0];

        if (cmd >= 0) {
            *frame = (u32)w[1];
            return *tile0 >= 0;
        }
        if (-cmd >= 13 || kCmdWords[-cmd] == 0 || cmd == -3)
            return 0;   /* unknown, or a jump before any frame */
        if (cmd == -1) {
            *tile0 = w[1];          /* negative: 8bpp, refused */
            *ntiles = (u32)w[2];
        }
        w += kCmdWords[-cmd];
    }
    return 0;
}

static const u8 kObjDim[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

/* An animation variant's first frame, 4bpp colour indices, into px (cw x ch,
 * cleared first) with (0, 0) the frame's top-left -- where DisplaySprite puts
 * it is (sprite x - *offX, sprite y - *offY).  Returns the width, 0 on
 * anything unexpected. */
static int DecodeSpriteFrame(u16 animId, u8 variant, u8 *px, int cw, int ch,
                             int *h, s16 *offX, s16 *offY)
{
    const struct SpriteAttributes_Sub *a;
    const u8 *tiles;
    const u16 *oam;
    u32 frame, n;
    s32 tile0;
    int s, fw, fh;

    memset(px, 0, (size_t)cw * ch);
    if (!AnimFirstFrame(animId, variant, &frame, &tile0, &n) || (frame >> 28) != 0)
        return 0;
    a = &gSpriteTables->attrs[animId].sub[frame];
    fw = a->width;
    fh = a->height;
    if (fw <= 0 || fh <= 0 || fw > cw || fh > ch)
        return 0;
    tiles = (const u8 *)(const void *)gSpriteTables->tiles4bpp + tile0 * 32;
    oam = gSpriteTables->oamData[animId];
    for (s = 0; s < a->numSubframes; s++) {
        const u16 *o = &oam[3 * ((a->bitfield & 0x3FFF) + s)];
        int shape = o[0] >> 14, size = o[1] >> 14;
        int ow, oh, ox = o[1] & 0x1FF, oy = o[0] & 0xFF, t0 = o[2] & 0x3FF;
        int hf = (o[1] >> 12) & 1, vf = (o[1] >> 13) & 1, x, y;

        if (shape == 3 || (o[0] & 0x2300))
            return 0;   /* prohibited shape, 8bpp or affine */
        ow = kObjDim[shape][size][0];
        oh = kObjDim[shape][size][1];
        for (y = 0; y < oh; y++)
            for (x = 0; x < ow; x++) {
                int sx = hf ? ow - 1 - x : x, sy = vf ? oh - 1 - y : y;
                u32 t = (u32)t0 + (sy / 8) * (ow / 8) + sx / 8;
                int dx = ox + x, dy = oy + y;
                u8 b;

                if (t >= n || dx < 0 || dy < 0 || dx >= cw || dy >= ch)
                    continue;
                b = tiles[t * 32 + (sy & 7) * 4 + (sx & 7) / 2];
                b = (sx & 1) ? b >> 4 : b & 15;
                if (b)
                    px[dy * cw + dx] = b;
            }
    }
    *h = fh;
    *offX = a->offsetX;
    *offY = a->offsetY;
    return fw;
}

/* A two-row BG picture (gUnk_082D7850[idx]: a description line) into px,
 * 240 x 16, as colour indices; the palette line of its first entry in *line. */
static u8 sBgTiles[0x8000];
static int DecodeDescPicture(u16 idx, u8 *px, int *line)
{
    const struct TiledBg_082D7850 *bg = gUnk_082D7850[idx];
    u32 size = ((const u32 *)(const void *)bg->tileset)[0] >> 8;
    int r, c, y, x;

    if (size > sizeof(sBgTiles) || bg->height < 2 || bg->width < 30)
        return 0;
    LZ77UnCompWram(bg->tileset, sBgTiles);
    *line = bg->tilemap[0] >> 12;
    for (r = 0; r < 2; r++)
        for (c = 0; c < 30; c++) {
            u16 e = bg->tilemap[r * 0x1E + c];
            int t = e & 0x3FF, hf = (e >> 10) & 1, vf = (e >> 11) & 1;

            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x++) {
                    int sx = hf ? 7 - x : x, sy = vf ? 7 - y : y;
                    u8 b = 0;

                    if ((u32)(t * 32 + 32) <= size) {
                        b = sBgTiles[t * 32 + sy * 4 + sx / 2];
                        b = (sx & 1) ? b >> 4 : b & 15;
                    }
                    px[(r * 8 + y) * 240 + c * 8 + x] = b;
                }
        }
    return 1;
}

/* --- letters cut from the game's pictures (English) ------------------------- */

/* The sources: Game Select labels (anim 0x357), screen headers (0x350) and
 * description pictures (gUnk_08385C14's English column). */
enum { SRC_START, SRC_COLLECTION, SRC_ERASE,                /* labels       */
       SRC_FILE_SELECT, SRC_SUB_GAMES, SRC_TRANSMITTING,    /* headers      */
       SRC_CHOOSE, SRC_ADVENTURE, SRC_EXTRA, SRC_VIEW,      /* descriptions */
       NUM_SRC };
static const u16 kSrcId[NUM_SRC] = {
    0, 2, 3,                    /* label variants of 0x357                  */
    3, 5, 6,                    /* header variants of 0x350                 */
    0xEC, 0xF7, 0xF8, 0xF9,     /* CHOOSE A FILE TO PLAY, ADVENTURE WITH 4
                                 * KIRBYS!, PLAY THE EXTRA SUB-GAMES, VIEW
                                 * COLLECTED ITEMS                          */
};
#define EN_LABEL_ANIM   0x357
#define EN_HEADER_ANIM  0x350

/* A letter's ink columns [x0, x1] in a source. */
struct Cut { char c; u8 src; u8 x0, x1; };

static const struct Cut kLabelCuts[] = {
    { 'S', SRC_START, 55, 63 }, { 'T', SRC_START, 65, 73 },
    { 'G', SRC_START, 109, 119 }, { 'E', SRC_START, 147, 154 },
    { 'I', SRC_ERASE, 137, 139 }, { 'N', SRC_COLLECTION, 119, 126 },
};
static const struct Cut kHeaderCuts[] = {
    { 'S', SRC_SUB_GAMES, 49, 54 }, { 'G', SRC_SUB_GAMES, 83, 90 },
    { 'E', SRC_SUB_GAMES, 114, 120 }, { 'I', SRC_FILE_SELECT, 57, 58 },
    { 'T', SRC_FILE_SELECT, 125, 132 }, { 'N', SRC_TRANSMITTING, 112, 118 },
};
static const struct Cut kDescCuts[] = {
    { 'A', SRC_CHOOSE, 95, 105 },     { 'B', SRC_ADVENTURE, 191, 198 },
    { 'C', SRC_VIEW, 79, 86 },        { 'D', SRC_ADVENTURE, 30, 37 },
    { 'E', SRC_CHOOSE, 81, 87 },      { 'F', SRC_CHOOSE, 114, 120 },
    { 'G', SRC_EXTRA, 176, 183 },     { 'H', SRC_CHOOSE, 43, 50 },
    { 'I', SRC_CHOOSE, 122, 123 },    { 'K', SRC_ADVENTURE, 169, 176 },
    { 'L', SRC_CHOOSE, 125, 131 },    { 'M', SRC_EXTRA, 197, 205 },
    { 'N', SRC_ADVENTURE, 57, 64 },   { 'O', SRC_VIEW, 88, 95 },
    { 'P', SRC_CHOOSE, 172, 179 },    { 'R', SRC_ADVENTURE, 84, 92 },
    { 'S', SRC_CHOOSE, 72, 79 },      { 'T', SRC_ADVENTURE, 66, 73 },
    { 'U', SRC_ADVENTURE, 75, 82 },   { 'V', SRC_ADVENTURE, 39, 47 },
    { 'W', SRC_ADVENTURE, 109, 122 }, { 'X', SRC_EXTRA, 96, 104 },
    { 'Y', SRC_ADVENTURE, 200, 207 }, { '!', SRC_ADVENTURE, 221, 222 },
    { '-', SRC_EXTRA, 170, 174 },
};

/* Decoded sources (host scratch, valid while sCutsOk). */
static u8 sSrc[NUM_SRC][PIC_H * 240];
static int sSrcW[NUM_SRC];
static int sLabelCutsOk, sHeaderCutsOk, sDescCutsOk;
static int sDescLine;           /* BG palette line of the description banner */

/* Does column x of src hold colour c anywhere in rows [y0, y1]? */
static int ColHas(int s, int x, int y0, int y1, u8 c)
{
    int y;

    if (x < 0 || x >= sSrcW[s])
        return 0;
    for (y = y0; y <= y1; y++)
        if (sSrc[s][y * sSrcW[s] + x] == c)
            return 1;
    return 0;
}

/* Every cut's ink is where the table says and nothing touches it. */
static int CutsCheck(const struct Cut *cuts, int n, int y0, int y1, u8 ink)
{
    int i;

    for (i = 0; i < n; i++) {
        const struct Cut *k = &cuts[i];

        if (sSrcW[k->src] == 0
            || !ColHas(k->src, k->x0, y0, y1, ink) || !ColHas(k->src, k->x1, y0, y1, ink)
            || ColHas(k->src, k->x0 - 1, y0, y1, ink) || ColHas(k->src, k->x1 + 1, y0, y1, ink))
            return 0;
    }
    return 1;
}

static void LoadSources(void)
{
    int s, h, line;
    s16 ox, oy;

    sLabelCutsOk = sHeaderCutsOk = sDescCutsOk = 0;
    memset(sSrcW, 0, sizeof(sSrcW));
    if (gLanguage != LANGUAGE_ENGLISH || sNoCuts)
        return;
    for (s = 0; s < NUM_SRC; s++) {
        if (s <= SRC_ERASE)
            sSrcW[s] = DecodeSpriteFrame(EN_LABEL_ANIM, (u8)kSrcId[s], sSrc[s], 240, PIC_H, &h, &ox, &oy);
        else if (s <= SRC_TRANSMITTING)
            sSrcW[s] = DecodeSpriteFrame(EN_HEADER_ANIM, (u8)kSrcId[s], sSrc[s], 240, PIC_H, &h, &ox, &oy);
        else if (DecodeDescPicture(kSrcId[s], sSrc[s], &line))
            sSrcW[s] = 240;
        if (sSrcW[s] != 0)
            sSrcW[s] = 240;     /* every canvas is decoded at stride 240 */
    }
    sLabelCutsOk = CutsCheck(kLabelCuts, ARRAY_COUNT(kLabelCuts), LBL_ROW0 + 1, LBL_ROW1 - 1, LBL_FILL);
    sHeaderCutsOk = CutsCheck(kHeaderCuts, ARRAY_COUNT(kHeaderCuts), HDR_ROW0, HDR_ROW1, HDR_TEXT);
    sDescCutsOk = CutsCheck(kDescCuts, ARRAY_COUNT(kDescCuts), 0, 15, DSC_TEXT);
    if (!sLabelCutsOk || !sHeaderCutsOk || !sDescCutsOk)
        PortLog("[menu-poc] lettering from the game's pictures: labels %s, header %s, "
                "descriptions %s (otherwise the fallback font)",
                sLabelCutsOk ? "ok" : "NO", sHeaderCutsOk ? "ok" : "NO",
                sDescCutsOk ? "ok" : "NO");
}

static const struct Cut *FindCut(const struct Cut *cuts, int n, char c)
{
    int i;

    for (i = 0; i < n; i++)
        if (cuts[i].c == c)
            return &cuts[i];
    return NULL;
}

/* Can every letter of s be cut? */
static int CanCut(const struct Cut *cuts, int n, const char *s)
{
    for (; *s; s++)
        if (*s != ' ' && FindCut(cuts, n, *s) == NULL)
            return 0;
    return 1;
}

/* Paste s's letters into px (stride w) starting at ink column x, copying
 * the source pixels of colours a or b (b may be 0) in rows [y0, y1] and
 * (pad) columns either side; gap empty columns between letters, space wide
 * spaces.  Returns the x after the last letter; with px NULL only measures. */
static int PasteCut(u8 *px, int w, int x, const struct Cut *cuts, int n,
                    const char *s, int y0, int y1, int pad, u8 a, u8 b,
                    int gap, int space)
{
    int first = 1;

    for (; *s; s++) {
        const struct Cut *k;
        int sx, y;

        if (*s == ' ') {
            x += space;
            continue;
        }
        k = FindCut(cuts, n, *s);
        if (k == NULL)
            continue;
        if (!first)
            x += gap;
        first = 0;
        if (px != NULL)
            for (sx = k->x0 - pad; sx <= k->x1 + pad; sx++)
                for (y = y0; y <= y1; y++) {
                    u8 v = sSrc[k->src][y * 240 + sx];
                    int dx = x + sx - k->x0;

                    if ((v == a || (b && v == b)) && dx >= 0 && dx < w)
                        px[y * w + dx] = v;
                }
        x += k->x1 - k->x0 + 1;
    }
    return x;
}

/* --- the fallback font -------------------------------------------------------
 *
 * Written for this file, on the grid the game's label lettering is built on:
 * cells of 2x2 pixels, five rows of them (10 px of ink, as the game's), one
 * pixel of outline all round.  Each glyph is five rows of up to five cells,
 * '#' ink; its width is its first row's length. */
struct CellGlyph { char c; const char *rows[5]; };
static const struct CellGlyph kCell[] = {
    { 'A', { "###", "# #", "###", "# #", "# #" } },
    { 'B', { "## ", "# #", "## ", "# #", "## " } },
    { 'C', { "###", "#  ", "#  ", "#  ", "###" } },
    { 'D', { "## ", "# #", "# #", "# #", "## " } },
    { 'E', { "###", "#  ", "## ", "#  ", "###" } },
    { 'F', { "###", "#  ", "## ", "#  ", "#  " } },
    { 'G', { "###", "#  ", "# #", "# #", "###" } },
    { 'H', { "# #", "# #", "###", "# #", "# #" } },
    { 'I', { "#", "#", "#", "#", "#" } },
    { 'J', { "  #", "  #", "  #", "# #", "###" } },
    { 'K', { "# #", "# #", "## ", "# #", "# #" } },
    { 'L', { "#  ", "#  ", "#  ", "#  ", "###" } },
    { 'M', { "#   #", "## ##", "# # #", "#   #", "#   #" } },
    { 'N', { "#  #", "## #", "# ##", "#  #", "#  #" } },
    { 'O', { "###", "# #", "# #", "# #", "###" } },
    { 'P', { "###", "# #", "###", "#  ", "#  " } },
    { 'Q', { "###", "# #", "# #", "###", "  #" } },
    { 'R', { "###", "# #", "## ", "# #", "# #" } },
    { 'S', { "###", "#  ", "###", "  #", "###" } },
    { 'T', { "###", " # ", " # ", " # ", " # " } },
    { 'U', { "# #", "# #", "# #", "# #", "###" } },
    { 'V', { "# #", "# #", "# #", "# #", " # " } },
    { 'W', { "#   #", "#   #", "# # #", "## ##", "#   #" } },
    { 'X', { "# #", "# #", " # ", "# #", "# #" } },
    { 'Y', { "# #", "# #", "###", " # ", " # " } },
    { 'Z', { "###", "  #", " # ", "#  ", "###" } },
    { '0', { "###", "# #", "# #", "# #", "###" } },
    { '1', { "## ", " # ", " # ", " # ", "###" } },
    { '2', { "###", "  #", "###", "#  ", "###" } },
    { '3', { "###", "  #", " ##", "  #", "###" } },
    { '4', { "# #", "# #", "###", "  #", "  #" } },
    { '5', { "###", "#  ", "###", "  #", "###" } },
    { '6', { "###", "#  ", "###", "# #", "###" } },
    { '7', { "###", "  #", "  #", "  #", "  #" } },
    { '8', { "###", "# #", "###", "# #", "###" } },
    { '9', { "###", "# #", "###", "  #", "###" } },
    { ':', { " ", "#", " ", "#", " " } },
    { '.', { " ", " ", " ", " ", "#" } },
    { '-', { "   ", "   ", "###", "   ", "   " } },
    { '!', { "#", "#", "#", " ", "#" } },
    { '/', { "  #", "  #", " # ", "#  ", "#  " } },
    { '%', { "# #", "  #", " # ", "#  ", "# #" } },
    { '+', { "   ", " # ", "###", " # ", "   " } },
    { '<', { "  ", " #", "##", " #", "  " } },   /* triangles, like the */
    { '>', { "  ", "# ", "##", "# ", "  " } },   /* labels' bullet      */
};

static const struct CellGlyph *CellFind(char c)
{
    int i;

    for (i = 0; i < (int)ARRAY_COUNT(kCell); i++)
        if (kCell[i].c == c)
            return &kCell[i];
    return NULL;
}

/* Ink width of s in pixels (letters 2 px a cell, one empty column between,
 * spaces 4 px). */
static int CellWidth(const char *s)
{
    int w = 0, first = 1;

    for (; *s; s++) {
        const struct CellGlyph *g = CellFind(*s);

        if (*s == ' ') { w += 4; continue; }
        if (g == NULL) continue;
        if (!first) w += 2;     /* a column of outline, shared, + one more */
        first = 0;
        w += 2 * (int)strlen(g->rows[0]);
    }
    return w;
}

/* Draw s with its ink starting at (x, y), ink colour fill and, if edge, a
 * one-pixel outline in edge. */
static void CellText(u8 *px, int w, int h, int x, int y, const char *s, u8 fill, u8 edge)
{
    int pass, first;
    const char *p;

    for (pass = edge ? 0 : 1; pass < 2; pass++) {
        int cx = x;

        first = 1;
        for (p = s; *p; p++) {
            const struct CellGlyph *g = CellFind(*p);
            int gw, r, c, dx, dy;

            if (*p == ' ') { cx += 4; continue; }
            if (g == NULL) continue;
            if (!first) cx += 2;
            first = 0;
            gw = (int)strlen(g->rows[0]);
            for (r = 0; r < 5; r++)
                for (c = 0; c < gw && g->rows[r][c]; c++) {
                    if (g->rows[r][c] != '#')
                        continue;
                    for (dy = pass ? 0 : -1; dy <= (pass ? 1 : 2); dy++)
                        for (dx = pass ? 0 : -1; dx <= (pass ? 1 : 2); dx++) {
                            int qx = cx + 2 * c + dx, qy = y + 2 * r + dy;

                            if (qx >= 0 && qx < w && qy >= 0 && qy < h)
                                px[qy * w + qx] = pass ? fill : edge;
                        }
                }
            cx += 2 * gw;
        }
    }
}

/* --- building pictures ---------------------------------------------------------- */

/* The ERASE A FILE banner (the game's language: anim and variant of the live
 * label) with its lettering cleared, into px (stride w, PIC_H rows).
 * Returns 0 if the picture is not the shape this file expects. */
static int ClearedBanner(u16 animId, u8 variant, u8 *px, int w)
{
    int fw, h, x, y;

    fw = DecodeSpriteFrame(animId, variant, px, w, PIC_H, &h, &sLblOffX, &sLblOffY);
    if (fw != LBL_W || h != LBL_H)
        return 0;
    for (y = 0; y < PIC_H; y++) {
        u8 bg = px[y * w + LBL_BG_X];

        if (bg != 2 && bg != 3)
            continue;           /* the bevel rows: no lettering there */
        for (x = LBL_TEXT_X0; x < LBL_TEXT_X1; x++) {
            u8 *p = &px[y * w + x];

            if (*p == LBL_FILL || *p == LBL_EDGE)
                *p = bg;
        }
    }
    return 1;
}

/* Pack a PIC_H-row canvas of cols 32-px columns into VRAM: per column a 32x16
 * object's 8 tiles, then a 32x8 object's 4 (1D mapping, row-major). */
static void PackPic(const u8 *px, int cols, u32 vram)
{
    int w = cols * 32, c, part, ty, tx, y, x;
    u8 *dst = (u8 *)(uintptr_t)vram;

    for (c = 0; c < cols; c++)
        for (part = 0; part < 2; part++)
            for (ty = 0; ty < (part ? 1 : 2); ty++)
                for (tx = 0; tx < 4; tx++)
                    for (y = 0; y < 8; y++)
                        for (x = 0; x < 8; x += 2) {
                            int sx = c * 32 + tx * 8 + x, sy = part * 16 + ty * 8 + y;

                            *dst++ = (u8)(px[sy * w + sx] | (px[sy * w + sx + 1] << 4));
                        }
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

static void DrawPic(u8 layer, s32 x, s32 y, u32 vram, int cols, int rows, int pal)
{
    int c;

    for (c = 0; c < cols; c++) {
        u32 v = vram + c * COL_TILES * 32;

        PutObj(layer, x + c * 32, y, 1, 2, v, pal);                 /* 32x16 */
        if (rows > 16)
            PutObj(layer, x + c * 32, y + 16, 1, 1, v + 8 * 32, pal); /* 32x8 */
    }
}

/* A description line in the game's description lettering (or the fallback
 * font), centred in a 240 x 16 strip, into VRAM. */
static void BuildDesc(const char *s, u32 vram)
{
    static u8 px[PIC_H * GS_DESC_COLS * 32];
    int w = GS_DESC_COLS * 32;

    memset(px, 0, sizeof(px));
    if (sDescCutsOk && CanCut(kDescCuts, ARRAY_COUNT(kDescCuts), s)) {
        /* the game's spacing: one empty column between letters, seven
         * between words */
        int tw = PasteCut(NULL, w, 0, kDescCuts, ARRAY_COUNT(kDescCuts), s, 0, 15, 0,
                          DSC_TEXT, 0, 1, 6);

        if (tw > DISPLAY_WIDTH - 4)
            PortLog("[menu-poc] description too wide (%d px): %s", tw, s);
        PasteCut(px, w, (DISPLAY_WIDTH - tw) / 2, kDescCuts, ARRAY_COUNT(kDescCuts), s,
                 0, 15, 0, DSC_TEXT, 0, 1, 6);
    } else {
        CellText(px, w, PIC_H, (DISPLAY_WIDTH - CellWidth(s)) / 2, 3, s, DSC_TEXT, 0);
    }
    PackPic(px, GS_DESC_COLS, vram);
}

/* The description banner's colours, every frame: the menu cycles its BG
 * palette, and the line is a copy. */
static void SyncDescPalette(void)
{
    int i;

    sDescLine = BANNER_MAP[0] >> 12;
    for (i = 1; i < 16; i++)
        gObjPalette[PAL_DESC * 16 + i] = gBgPalette[sDescLine * 16 + i];
    gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
}

static int VramOk(u32 v)
{
    return v != (u32)(uintptr_t)ewram_end;
}

/* --- Game Select's fifth row ------------------------------------------------------ */

static const char kLabel[] = "SETTINGS";
static const char kDesc[] = "CHANGE PICTURE AND SOUND";
/* The Start Game pop-up's MULTIPLAYER line (the game's own says MULTI-PAK). */
static const char kPopupMpDesc[] = "LOCAL OR ONLINE PLAY";
static const char *sGsDescWant, *sGsDescShown;

static void BuildLabel(struct Unk_08138D64 *a1, u32 vram)
{
    static u8 px[PIC_H * GS_LABEL_COLS * 32];
    int w = GS_LABEL_COLS * 32;

    memset(px, 0, sizeof(px));
    if (!ClearedBanner(a1->unk2DC[3].animId, a1->unk2DC[3].variant, px, w)) {
        PortLog("[menu-poc] the ERASE A FILE label is not the expected shape; "
                "SETTINGS row left blank");
        memset(px, 0, sizeof(px));
    } else if (sLabelCutsOk && a1->unk2DC[3].animId == EN_LABEL_ANIM) {
        /* the game's spacing: one outline column between letters */
        PasteCut(px, w, LBL_TEXT_LEFT, kLabelCuts, ARRAY_COUNT(kLabelCuts), kLabel,
                 LBL_ROW0, LBL_ROW1, 1, LBL_FILL, LBL_EDGE, 1, 8);
    } else {
        CellText(px, w, PIC_H, LBL_TEXT_LEFT, LBL_ROW0 + 1, kLabel, LBL_FILL, LBL_EDGE);
    }
    PackPic(px, GS_LABEL_COLS, vram);
}

void PortMenuPocAlloc(struct Unk_08138D64 *a1)
{
    u32 v;

    if (!PortMenuPoc())
        return;
    PortMenuPocFree(a1);
    v = VramMalloc(GS_TILES);
    if (!VramOk(v)) {
        PortLog("[menu-poc] no sprite memory for the SETTINGS entry");
        return;
    }
    a1->unk3A8.tilesVram = v;
    sOwner = a1;
    LoadSources();
    BuildLabel(a1, v);
    BuildDesc(kDesc, v + GS_LABEL_COLS * COL_TILES * 32);
    sGsDescShown = kDesc;
    sGsDescWant = NULL;
}

void PortMenuPocPopupDesc(int multiplayer)
{
    sGsDescWant = multiplayer ? kPopupMpDesc : NULL;
}

void PortMenuPocFree(struct Unk_08138D64 *a1)
{
    if (!PortMenuPoc())
        return;
    if (sOwner == a1) {
        if (a1->unk3A8.tilesVram && VramOk(a1->unk3A8.tilesVram))
            VramFree(a1->unk3A8.tilesVram);
        a1->unk3A8.tilesVram = 0;
    }
    sOwner = NULL;
}

void PortMenuPocDraw(struct Unk_08138D64 *a1)
{
    struct Sprite *last;
    s32 x, y;
    u8 layer;
    int selected;

    if (!PortMenuPoc() || sOwner != a1 || a1->unk1 != 2 || !a1->unk3A8.tilesVram)
        return;
    /* A fifth label sprite in all but name: one row below the fourth,
     * whatever slide or spread the game is animating, drawn where
     * DisplaySprite would draw that frame. */
    last = &a1->unk2DC[3];
    x = last->x - sLblOffX;
    y = last->y + (last->y - a1->unk2DC[2].y) - sLblOffY;
    layer = (last->unk14 & 0x7C0) >> 6;
    selected = a1->unk3A4 == PORT_MENU_POC_SETTINGS;
    DrawPic(layer, x, y, a1->unk3A8.tilesVram, GS_LABEL_COLS, PIC_H,
            selected ? PAL_LIT : PAL_DIM);
    {
        const char *want = selected ? kDesc : sGsDescWant;

        if (want != NULL && want != sGsDescShown) {
            BuildDesc(want, a1->unk3A8.tilesVram + GS_LABEL_COLS * COL_TILES * 32);
            sGsDescShown = want;
        }
        selected = want != NULL;
    }
    if (selected) {
        SyncDescPalette();
        DrawPic(layer, 0, DESC_Y, a1->unk3A8.tilesVram + GS_LABEL_COLS * COL_TILES * 32,
                GS_DESC_COLS, 16, PAL_DESC);
    }
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
    "GRAPHICS", "SCREEN", "ZOOM", "CAMERA", "PIXELS", "VOLUME",
};
/* Only letters the game's description pictures have (no Z, Q, J, digits). */
static const char *const kRowDesc[ROWS] = {
    "ENHANCED CAN GO WIDESCREEN",
    "PICTURE SHAPE FOR ENHANCED",
    "HOW MUCH OF THE LEVEL SHOWS",
    "SMOOTHER SCROLLING",
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
    case R_SMOOTHCAM: snprintf(buf, size, "%s", s->smoothCam ? "SMOOTH" : "NORMAL"); break;
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

/* --- what a list screen shows ----------------------------------------------------
 *
 * The screen below draws a model: its rows' names and values, a header, a
 * description per row, and what Left/Right/A do.  Two models: the settings
 * (front end and pause page) and the front end's MULTIPLAYER choice. */
struct PocModel {
    int rows;
    const char *header;
    const char *(*name)(int row);
    void (*value)(int row, char *buf, int size);
    const char *(*desc)(int row);
    /* dir -1 / +1 (Left / Right, or A as +1): 1 if a value changed */
    int (*step)(int row, int dir);
    /* a row shown but not available: always dimmed */
    int (*unavailable)(int row);
};

static const char *SetName(int row) { return kRowName[row]; }
static const char *SetDesc(int row) { return kRowDesc[row]; }
static int SetStep(int row, int dir) { RowStep(row, dir); return 1; }
static int SetUnavailable(int row) { (void)row; return 0; }

static const struct PocModel kSettingsModel = {
    ROWS, "SETTINGS", SetName, RowValue, SetDesc, SetStep, SetUnavailable,
};

/* MULTIPLAYER: LOCAL (two players on this screen, platform/localmp.c) and
 * ONLINE (not built yet: shown, dimmed, does nothing). */
static const char *MpName(int row) { return row == 0 ? "LOCAL" : "ONLINE"; }
static void MpValue(int row, char *buf, int size)
{
    snprintf(buf, size, "%s", row == 0 ? "2 PLAYERS" : "SOON");
}
static const char *MpDesc(int row)
{
    if (row != 0)
        return "COMING LATER";
    if (!PortLmpP2InputAvailable())
        return "NEEDS KEYS OR A GAMEPAD";
    /* alternate, about every two seconds: what it is, and player 2's keys */
    return (gPortSim->frame.frameCount / 120) & 1 ? "P2: ARROW KEYS + NUMPAD"
                                                  : "TWO PLAYERS ON ONE SCREEN";
}
static int MpStep(int row, int dir) { (void)row; (void)dir; return 0; }
static int MpUnavailable(int row) { return row != 0 || !PortLmpP2InputAvailable(); }

static const struct PocModel kMpModel = {
    2, "MULTIPLAYER", MpName, MpValue, MpDesc, MpStep, MpUnavailable,
};

static const struct PocModel *sModel = &kSettingsModel;
static const char *sDescShown;      /* what the screen's description holds */

/* --- the screen itself, shared by the front end and the pause page ----------
 *
 * Everything below draws from `tiles` (SET_TILES of OBJ VRAM, wherever the
 * caller found them) and a row cursor the caller keeps in its own (game)
 * memory; the banner and header are decoded from the sprite variants the
 * caller names.  The two callers are the front end's settings screen (tiles
 * from VramMalloc, row in a1->unk3A6) and the pause menu's settings page
 * (game/src/pause_settings.c: fixed tiles, saved and put back around it). */

static u32 RowVram(u32 tiles, int row)
{
    return tiles + row * ROW_COLS * COL_TILES * 32;
}

/* The header and description follow the rows the screen up now has (the
 * MULTIPLAYER screen takes only the tiles its two rows need: a second visit
 * to it can find the VRAM heap too fragmented for the settings' 660). */
static u32 HeadVram(u32 tiles)
{
    return RowVram(tiles, sModel->rows);
}

static u32 SetDescVram(u32 tiles)
{
    return HeadVram(tiles) + HEAD_COLS * COL_TILES * 32;
}

/* The game's banner, widened from 176 to ROW_W by repeating one plain
 * column (the stripes run across, so a repeated column is invisible). */
static int BuildRowBanner(u16 lblAnim, u8 lblVariant)
{
    static u8 src[PIC_H * GS_LABEL_COLS * 32];
    int sw = GS_LABEL_COLS * 32, w = ROW_COLS * 32, extra = ROW_W - LBL_W, x, y;

    memset(sRowBanner, 0, sizeof(sRowBanner));
    if (!ClearedBanner(lblAnim, lblVariant, src, sw))
        return 0;
    for (y = 0; y < PIC_H; y++)
        for (x = 0; x < ROW_W; x++) {
            int sx = x < ROW_WIDEN_AT ? x : x < ROW_WIDEN_AT + extra ? ROW_WIDEN_AT : x - extra;

            sRowBanner[y * w + x] = src[y * sw + sx];
        }
    return 1;
}

static void DrawRowTiles(u32 tiles, int row, int selected)
{
    static u8 px[PIC_H * ROW_COLS * 32];
    int w = ROW_COLS * 32;
    char val[24], shown[32];

    memcpy(px, sRowBanner, sizeof(px));
    CellText(px, w, PIC_H, LBL_TEXT_LEFT, LBL_ROW0 + 1, sModel->name(row), LBL_FILL, LBL_EDGE);
    sModel->value(row, val, sizeof(val));
    /* the selected row's value between arrows, where they fit (and where
     * Left/Right change it) */
    snprintf(shown, sizeof(shown), "<%s>", val);
    if (!selected || sModel != &kSettingsModel
        || LBL_TEXT_LEFT + CellWidth(sModel->name(row)) + 8 + CellWidth(shown) > ROW_VALUE_X1)
        snprintf(shown, sizeof(shown), "%s", val);
    CellText(px, w, PIC_H, ROW_VALUE_X1 - CellWidth(shown), LBL_ROW0 + 1, shown,
             LBL_FILL, LBL_EDGE);
    PackPic(px, ROW_COLS, RowVram(tiles, row));
}

/* The SUB-GAMES header (its S icon suits SETTINGS) with "SETTINGS" in the
 * headers' own lettering. */
static void DrawHeadTiles(u32 tiles, u16 hdrAnim, u8 hdrVariant)
{
    const char *text = sModel->header;
    static u8 px[PIC_H * HEAD_COLS * 32];
    int w = HEAD_COLS * 32, fw, h, x, y;

    fw = DecodeSpriteFrame(hdrAnim, hdrVariant, px, w, PIC_H, &h, &sHdrOffX, &sHdrOffY);
    if (fw != HDR_W || h != HDR_H) {
        memset(px, 0, sizeof(px));
    } else {
        for (y = HDR_ROW0; y <= HDR_ROW1; y++)
            for (x = HDR_TEXT_X0; x < HDR_TEXT_X1; x++)
                if (px[y * w + x] == HDR_TEXT)
                    px[y * w + x] = HDR_BG;
        if (sHeaderCutsOk && hdrAnim == EN_HEADER_ANIM
            && CanCut(kHeaderCuts, ARRAY_COUNT(kHeaderCuts), text))
            PasteCut(px, w, HDR_TEXT_LEFT, kHeaderCuts, ARRAY_COUNT(kHeaderCuts), text,
                     HDR_ROW0, HDR_ROW1, 0, HDR_TEXT, 0, 2, 6);
        else
            CellText(px, w, PIC_H, HDR_TEXT_LEFT, HDR_ROW0, text, HDR_TEXT, 0);
    }
    PackPic(px, HEAD_COLS, HeadVram(tiles));
}

static void ModelBuild(const struct PocModel *m, u32 tiles, u16 lblAnim, u8 lblVariant,
                       u16 hdrAnim, u8 hdrVariant, int row)
{
    int r;

    sModel = m;
    LoadSources();
    if (!BuildRowBanner(lblAnim, lblVariant))
        PortLog("[menu-poc] the ERASE A FILE label is not the expected shape; rows without banners");
    for (r = 0; r < m->rows; r++)
        DrawRowTiles(tiles, r, r == row);
    DrawHeadTiles(tiles, hdrAnim, hdrVariant);
    sDescShown = m->desc(row);
    BuildDesc(sDescShown, SetDescVram(tiles));
}

void PortPocScreenBuild(u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row)
{
    ModelBuild(&kSettingsModel, tiles, lblAnim, lblVariant, hdrAnim, hdrVariant, row);
}

static s32 RowFrameY(int row)
{
    return ROW_Y0 + ROW_PITCH * row;
}

int PortPocScreenCursorY(int row)
{
    /* Game Select draws the star 11 px above a label sprite's y
     * (gUnk_0838650A), and a label's frame is drawn sLblOffY above that. */
    return RowFrameY(row) + sLblOffY - 11;
}

int PortPocScreenCursorX(void)
{
    /* ... and 16 px left of its x: our frames sit 16 px further left. */
    return ROW_FRAME_X + sLblOffX - 16;
}

void PortPocScreenDraw(u32 tiles, int row, int slide, u8 layer)
{
    int r;

    /* the header slides in from the left as the game's (unkC) do */
    DrawPic(layer, -sHdrOffX - slide * 0x9C / 0xD0, -sHdrOffY, HeadVram(tiles), HEAD_COLS,
            PIC_H, PAL_HEADER);
    for (r = 0; r < sModel->rows; r++)
        DrawPic(layer, ROW_FRAME_X + slide, RowFrameY(r), RowVram(tiles, r), ROW_COLS, PIC_H,
                row == r && !sModel->unavailable(r) ? PAL_LIT : PAL_DIM);
    /* a description that changes by itself (MULTIPLAYER's LOCAL row) */
    if (sModel->desc(row) != sDescShown) {
        sDescShown = sModel->desc(row);
        BuildDesc(sDescShown, SetDescVram(tiles));
    }
    SyncDescPalette();
    DrawPic(layer, 0, DESC_Y, SetDescVram(tiles), GS_DESC_COLS, 16, PAL_DESC);
}

int PortPocScreenInput(u32 tiles, u8 *row, unsigned key)
{
    int was = *row, n = sModel->rows;

    switch (key) {
    case 3:     /* up */
        *row = was == 0 ? n - 1 : was - 1;
        break;
    case 4:     /* down */
        *row = was == n - 1 ? 0 : was + 1;
        break;
    case 1:     /* A: next value */
    case 6:     /* right */
        if (!sModel->step(was, 1))
            return 0;
        DrawRowTiles(tiles, was, 1);
        return 2;
    case 5:     /* left */
        if (!sModel->step(was, -1))
            return 0;
        DrawRowTiles(tiles, was, 1);
        return 2;
    default:
        return 0;
    }
    DrawRowTiles(tiles, was, 0);
    DrawRowTiles(tiles, *row, 1);
    sDescShown = sModel->desc(*row);
    BuildDesc(sDescShown, SetDescVram(tiles));
    return 1;
}

/* --- the front end's settings screen ------------------------------------------ */

int PortMenuPocSettingsOpen(struct Unk_08138D64 *a1)
{
    u32 v;

    PortMenuPocFree(a1);
    a1->unk3A6 = 0;
    v = VramMalloc(SET_TILES);
    if (!VramOk(v))
        return 0;
    a1->unk3A8.tilesVram = v;
    sOwner = a1;
    /* the ERASE A FILE label's banner; unkC is Game Select's header and
     * Sub-Games' is the next variant in every language (gUnk_08385CD4 [1],
     * [2]) */
    PortPocScreenBuild(v, a1->unk2DC[3].animId, a1->unk2DC[3].variant,
                       a1->unkC.animId, a1->unkC.variant + 1, 0);
    PortMenuPocBlankBanner();
    return 1;
}

/* The screen's sprite memory, wanted as the screen comes in.  Not at once:
 * the walking Kirbys the screen takes off (sub_0802FBE0) only free theirs
 * when their task next runs, and on a second visit they sit where the
 * screen's block went the first time -- the settings screen came up blank
 * then ("no sprite memory").  So the slide-in asks every frame until it
 * fits; 1 once the screen is built. */
int PortMenuPocScreenReady(struct Unk_08138D64 *a1, int multiplayer, int frames)
{
    int ok;

    if (sOwner == a1 && a1->unk3A8.tilesVram)
        return 1;
    ok = multiplayer ? PortMenuPocMpOpen(a1) : PortMenuPocSettingsOpen(a1);
    if (!ok && frames == 30)
        PortLog("[menu-poc] no sprite memory for the %s screen", multiplayer ? "multiplayer" : "settings");
    return ok;
}

/* The MULTIPLAYER screen: the same screen with kMpModel, same memory. */
int PortMenuPocMpOpen(struct Unk_08138D64 *a1)
{
    u32 v;

    PortMenuPocFree(a1);
    a1->unk3A6 = 0;
    v = VramMalloc((kMpModel.rows * ROW_COLS + HEAD_COLS + GS_DESC_COLS) * COL_TILES);
    if (!VramOk(v))
        return 0;
    a1->unk3A8.tilesVram = v;
    sOwner = a1;
    ModelBuild(&kMpModel, v, a1->unk2DC[3].animId, a1->unk2DC[3].variant,
               a1->unkC.animId, a1->unkC.variant + 1, 0);
    PortMenuPocBlankBanner();
    return 1;
}

/* Is row `row` of the screen up now available (A does something)? */
int PortMenuPocRowAvailable(int row)
{
    return !sModel->unavailable(row);
}

void PortMenuPocSettingsClose(struct Unk_08138D64 *a1)
{
    PortMenuPocFree(a1);
}

int PortMenuPocSettingsCursorY(struct Unk_08138D64 *a1)
{
    return PortPocScreenCursorY(a1->unk3A6);
}

int PortMenuPocSettingsCursorX(void)
{
    return PortPocScreenCursorX();
}

void PortMenuPocSettingsDraw(struct Unk_08138D64 *a1, int slide)
{
    if (sOwner != a1 || !a1->unk3A8.tilesVram)
        return;
    PortPocScreenDraw(a1->unk3A8.tilesVram, a1->unk3A6, slide, 0x10);  /* Game Select's rows' layer */
}

int PortMenuPocSettingsInput(struct Unk_08138D64 *a1, unsigned key)
{
    u8 row = a1->unk3A6;
    int ret;

    if (!a1->unk3A8.tilesVram)
        return 0;
    ret = PortPocScreenInput(a1->unk3A8.tilesVram, &row, key);
    a1->unk3A6 = row;
    return ret;
}

/* --- the pause page's sprite memory -------------------------------------------
 *
 * The pause page draws from fixed OBJ VRAM (the pause screens' heap is the
 * game's, mid-level), so it keeps a copy of what it covers and puts it back
 * as it goes: whatever the gameplay sprites or the page before left there
 * is there again afterwards, byte for byte.  Host memory, like the decoded
 * pictures above: it only ever holds bytes the game wrote, and only while
 * the page is up. */
static u8 sObjSave[PORT_POC_PAUSE_OBJ_BYTES];
static int sObjSaved;

void PortPocPauseObjSave(void)
{
    memcpy(sObjSave, (const void *)(uintptr_t)PORT_POC_PAUSE_OBJ_VRAM, sizeof(sObjSave));
    sObjSaved = 1;
}

void PortPocPauseObjRestore(void)
{
    if (!sObjSaved)
        return;
    memcpy((void *)(uintptr_t)PORT_POC_PAUSE_OBJ_VRAM, sObjSave, sizeof(sObjSave));
    sObjSaved = 0;
}
