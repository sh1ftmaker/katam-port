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
 * SETTINGS is a list of sections (DISPLAY, SOUND, PLAYERS, ONLINE, GAME,
 * ADVANCED; MAIN MENU on the pause page) holding every option the old ImGui
 * menu had ("the settings screen: sections" below).  A setting is clamped,
 * applied with PortSettingsChanged() and saved by it -- exactly what the
 * ImGui menu (ui_menu.cpp) does; the page's own things (files, fullscreen,
 * the touch pads, the randomizer, the log) are Module.portUiAction calls,
 * the same the ImGui menu makes.  A section scrolls by the cursor (TopRow),
 * with a small triangle where there is more; B goes back one level; the
 * PLAYERS row opens the PLAYERS page in place (platform/players_ui.c).
 * Online, everything drawn is session state and an edit belongs to the
 * pausing player's device ("online: per-device settings" below).
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
#include "port/lmpnet.h"
#include "port/port.h"
#include "port/menu_poc.h"
#include "port/localmp.h"
#include "port/online_ui.h"
#include "port/online_browse.h"     /* server browser */
#include "port/players_ui.h"
#include "port/backend.h"     /* PortFrameNumber (tests) */
#include "port/player_card.h"
#include "port/kirby_paint.h"     /* the presets' grid */
#include "port/simblock.h"
#include "port/inputlog.h"   /* PortInputLogReplaying */
#include "ui/settings.h"
#include "ui/ui.h"          /* PortSettingsChanged */
#include "ui/light_opts.h"  /* the LIGHTING section's rows */

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

/* The settings screen: six rows on screen (and in sprite memory); a list
 * with more scrolls (the settings' seventh, PLAYERS). */
#define ROWS            6
#define SET_ROWS        9
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
static void CardPalettesKeep(int on);   /* the player card's lent palette lines */
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

/* For a host with no environment (node, the online rig): the same switch
 * KATAM_MENU_POC is.  A host setting both online peers must agree on. */
void PortMenuPocSet(int on)
{
    sOn = on > 0;
    sNoCuts = on == 2;
}

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

/* The same, for the player card's preview and debug sheet
 * (platform/player_card.c; port/menu_poc.h). */
int PortPocDecodeFrame(u16 animId, u8 variant, u8 *px, int cw, int ch, int *h)
{
    s16 ox, oy;

    return DecodeSpriteFrame(animId, variant, px, cw, ch, h, &ox, &oy);
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
    { 'v', { "   ", "   ", "###", " # ", "   " } },  /* more rows below     */
    { '^', { "   ", " # ", "###", "   ", "   " } },  /* more rows above     */
    { '*', { " # ", "# #", "###", "###", "###" } },  /* server browser: a padlock */
};

static const struct CellGlyph *CellFind(char c)
{
    int i;

    for (i = 0; i < (int)ARRAY_COUNT(kCell); i++)
        if (kCell[i].c == c)
            return &kCell[i];
    return NULL;
}

static int SmallText(u8 *px, int w, int h, int x, int y, const char *s, u8 fill, u8 edge);
static int sCellGap = 2, sCellSpace = 4;     /* CellText's spacing (BuildDesc condenses) */

/* Ink width of s in pixels (letters 2 px a cell, one empty column between,
 * spaces 4 px). */
static int CellWidth(const char *s)
{
    int w = 0, first = 1;

    for (; *s; s++) {
        const struct CellGlyph *g = CellFind(*s);

        if (*s == ' ') { w += sCellSpace; continue; }
        if (g == NULL) continue;
        if (!first) w += sCellGap;     /* a column of outline, shared, + one more */
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

            if (*p == ' ') { cx += sCellSpace; continue; }
            if (g == NULL) continue;
            if (!first) cx += sCellGap;
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
         * between words -- or, for a line too long for that (LIGHTING's
         * run to 40 letters), three between words */
        int space = 6, tw = PasteCut(NULL, w, 0, kDescCuts, ARRAY_COUNT(kDescCuts), s, 0, 15, 0,
                                     DSC_TEXT, 0, 1, space);

        if (tw > DISPLAY_WIDTH - 4) {
            space = 3;
            tw = PasteCut(NULL, w, 0, kDescCuts, ARRAY_COUNT(kDescCuts), s, 0, 15, 0, DSC_TEXT, 0, 1, space);
        }
        if (tw <= DISPLAY_WIDTH - 2) {
            PasteCut(px, w, (DISPLAY_WIDTH - tw) / 2, kDescCuts, ARRAY_COUNT(kDescCuts), s,
                     0, 15, 0, DSC_TEXT, 0, 1, space);
            PackPic(px, GS_DESC_COLS, vram);
            return;
        }
    }
    if (CellWidth(s) <= DISPLAY_WIDTH - 4) {
        CellText(px, w, PIC_H, (DISPLAY_WIDTH - CellWidth(s)) / 2, 3, s, DSC_TEXT, 0);
    } else {
        /* too long for the line: letters closer together, and failing that
         * the half-size font */
        sCellGap = 1; sCellSpace = 3;
        if (CellWidth(s) <= DISPLAY_WIDTH - 2)
            CellText(px, w, PIC_H, (DISPLAY_WIDTH - CellWidth(s)) / 2, 3, s, DSC_TEXT, 0);
        else
            SmallText(px, w, PIC_H, (DISPLAY_WIDTH - SmallText(NULL, w, PIC_H, 0, 5, s, DSC_TEXT, 0)) / 2, 5, s,
                      DSC_TEXT, 0);
        sCellGap = 2; sCellSpace = 4;
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
    CardPalettesKeep(0);
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

/* --- the settings screen: sections ------------------------------------------------
 *
 * SETTINGS is a short list of sections (DISPLAY, SOUND, PLAYERS, ONLINE, GAME,
 * ADVANCED, and on the pause page MAIN MENU); A opens one, B comes back.  A
 * section is a list of items: settings (Left / Right / A step them, the
 * value between arrows), page toggles and actions the web page carries out
 * (Module.portUiAction, web/shell.html: the same calls the ImGui menu makes),
 * sub-pages (PLAYERS), read-only lines, and things that ask first (a NO /
 * YES screen, kConfirmModel).  Every item of the old ImGui menu is here or
 * listed in docs as dropped.
 *
 * Online, everything drawn is session state: see "online: per-device
 * settings" below. */

static const char *const kAspect[PS_ASPECT_COUNT] = {
    "AUTO", "3:2 GBA", "4:3", "16:10", "16:9", "21:9", "FILL",
};
static const float kZoom[] = { 0.5f, 0.67f, 0.8f, 1.0f, 1.25f, 1.5f, 2.0f };
static const char *const kZoomName[] = { "0.5X", "0.67X", "0.8X", "1X", "1.25X", "1.5X", "2X" };
#define NZOOM ((int)(sizeof(kZoom) / sizeof(kZoom[0])))
static const char *const kScaling[PS_SCALE_COUNT] = { "SHARP", "FIT", "SMOOTH" };
static const char *const kPartyName[] = { "OFF", "PLAYERS", "ALL" };
/* the ImGui menu's four border colours (ui_menu.cpp), and anything else */
static const unsigned kBorder[4] = { 0x000000, 0x0C0E11, 0x2A2E36, 0x3A2A55 };
static const char *const kBorderName[5] = { "BLACK", "DARK", "GREY", "PURPLE", "OTHER" };

/* The ONLINE list's Kirby slots and the pause page's per-slot session copies
 * (settings, names): PORT_LN_SLOTS -- four, the wide build one per Kirby
 * slot -- and a mask with a bit for each. */
#define POC_SLOTS PORT_LN_SLOTS
#if POC_SLOTS > 8
typedef u16 PocSlotMask;
#else
typedef u8 PocSlotMask;
#endif
#define IS_SLOT_ITEM(it) ((it) >= IT_SLOT0 && (it) <= IT_SLOT_LAST)

enum {
    IT_NONE,
    /* the top level */
    IT_SEC_DISPLAY, IT_SEC_SOUND, IT_SEC_PLAYERS, IT_SEC_ONLINE, IT_SEC_GAME, IT_SEC_ADVANCED,
    IT_SEC_LIGHTING, IT_SEC_EXPERIMENTAL,
    IT_MAINMENU,
    /* DISPLAY */
    IT_GRAPHICS, IT_SCREEN, IT_ZOOM, IT_DYNZOOM, IT_CAMERA, IT_INTERP, IT_PIXELS, IT_BORDERS,
    IT_HUD, IT_HUDSIZE, IT_FPS, IT_FULLSCREEN, IT_VSYNC,
    /* SOUND */
    IT_VOLUME, IT_MUTE,
    /* PLAYERS */
    IT_PLAYERS, IT_PARTY, IT_TAGS, IT_TOUCH,
    /* ONLINE: the code, the mode, then a row per Kirby slot (four; the
     * wide build's POC_SLOTS; rows ordered by device, RowSlot) */
    IT_CODE, IT_MODE, IT_SLOT0, IT_SLOT_LAST = IT_SLOT0 + POC_SLOTS - 1,
    /* GAME */
    IT_EXPORT, IT_IMPORT, IT_DELSAVE, IT_ROMLOAD, IT_ROMFORGET, IT_RANDO, IT_SEED, IT_REROLL,
    IT_SIZES, IT_ABIL, IT_RESET,
    /* ADVANCED */
    IT_DIAG, IT_COPYLOG, IT_DEFAULTS, IT_BUILD, IT_RENDERER, IT_WEBGL, IT_GPU, IT_SETSTORE,
    /* EXPERIMENTAL (and LIGHTING, a section in it) */
    IT_KIRBYS, IT_ADD4, IT_REMOVE4,
    /* not a row: the NO / YES screen's REMOVE (a player row's A) */
    IT_KICK,
    /* not a row: the NO / YES screen before the page reloads with the other
     * module (CPU KIRBYS on the default module, the web page) */
    IT_KIRBYS_GO,
    /* PLAYERS: the player card (its row is the section's last) */
    IT_CARD,
    IT_COUNT
};

/* What an item is. */
enum {
    K_SECTION,      /* A opens a section                                    */
    K_SETTING,      /* a gPortSettings field: Left / Right / A step it      */
    K_TOGGLE,       /* the page's on / off (or list): Left / Right / A      */
    K_ACTION,       /* A does it                                            */
    K_CONFIRM,      /* A asks first (NO / YES), then does it                */
    K_OPEN,         /* A opens a page (PLAYERS)                             */
    K_INFO,         /* shown only                                           */
    K_PLAYER,       /* a Kirby in the online game: the host's A removes it  */
    K_CARD,         /* A opens a player card (platform/player_card.c)       */
};

struct ItemDef {
    const char *name;
    u8 kind;
    const char *desc;       /* what it is (when it is available)            */
};

/* Descriptions: the game's own description letters where it has them (no
 * J, Q, Z, digits); anything else is drawn in the fallback font. */
static const struct ItemDef kItem[IT_COUNT] = {
    [IT_NONE]        = { "", K_INFO, "" },
    [IT_SEC_DISPLAY] = { "DISPLAY", K_SECTION, "PICTURE SHAPE SIZE AND HUD" },
    [IT_SEC_SOUND]   = { "SOUND", K_SECTION, "MUSIC AND SOUND EFFECTS" },
    [IT_SEC_PLAYERS] = { "PLAYERS", K_SECTION, "WHO PLAYS AND THEIR CARDS" },
    [IT_SEC_ONLINE]  = { "ONLINE", K_SECTION, "THE ONLINE GAME" },
    [IT_SEC_GAME]    = { "GAME", K_SECTION, "SAVE ROM AND RANDOMIZER" },
    [IT_SEC_ADVANCED]= { "ADVANCED", K_SECTION, "LOG BUILD AND DEFAULTS" },
    [IT_SEC_LIGHTING]= { "LIGHTING", K_SECTION, "LIGHT AND SHADOWS" },
    [IT_SEC_EXPERIMENTAL] = { "EXPERIMENTAL", K_SECTION, "TRY-OUTS THAT MAY GLITCH" },
    [IT_MAINMENU]    = { "MAIN MENU", K_CONFIRM, "BACK TO THE TITLE SCREEN" },
    [IT_GRAPHICS]    = { "GRAPHICS", K_SETTING, "ENHANCED CAN GO WIDESCREEN" },
    [IT_SCREEN]      = { "SCREEN", K_SETTING, "PICTURE SHAPE FOR ENHANCED" },
    [IT_ZOOM]        = { "ZOOM", K_SETTING, "HOW MUCH OF THE LEVEL SHOWS" },
    [IT_DYNZOOM]     = { "AUTO ZOOM", K_SETTING, "EASES IN WHEN KIRBY RESTS" },
    [IT_CAMERA]      = { "CAMERA", K_SETTING, "SMOOTHER SCROLLING" },
    [IT_INTERP]      = { "IN-BETWEEN", K_SETTING, "SMOOTHER ON FAST SCREENS" },
    [IT_PIXELS]      = { "PIXELS", K_SETTING, "HOW THE PICTURE IS SCALED" },
    [IT_BORDERS]     = { "BORDERS", K_SETTING, "COLOR AROUND THE PICTURE" },
    [IT_HUD]         = { "HUD", K_SETTING, "WHERE THE HUD SITS" },
    [IT_HUDSIZE]     = { "HUD SIZE", K_SETTING, "AUTO KEEPS THE GAMES SIZE" },
    [IT_FPS]         = { "SHOW FPS", K_SETTING, "FRAMES PER SECOND COUNTER" },
    [IT_FULLSCREEN]  = { "FULLSCREEN", K_TOGGLE, "FILL THE WHOLE SCREEN" },
    [IT_VSYNC]       = { "VSYNC", K_SETTING, "MATCH THE DISPLAY REFRESH" },
    [IT_VOLUME]      = { "VOLUME", K_SETTING, "MUSIC AND SOUND EFFECTS" },
    [IT_MUTE]        = { "MUTE", K_SETTING, "SILENCE EVERYTHING" },
    [IT_PLAYERS]     = { "PLAYERS", K_OPEN, "CHOOSE WHO USES WHICH PAD" },
    [IT_PARTY]       = { "PARTY HUD", K_SETTING, "CARDS FOR THE OTHER PLAYERS" },
    [IT_TAGS]        = { "NAME TAGS", K_SETTING, "NAMES OVER THE PLAYERS" },
    [IT_TOUCH]       = { "TOUCH PADS", K_TOGGLE, "BUTTONS ON THE SCREEN" },
    [IT_CODE]        = { "CODE", K_INFO, "TELL A FRIEND THIS CODE" },
    [IT_MODE]        = { "MODE", K_INFO, "CHOSEN BY THE HOST" },
    [IT_SLOT0]       = { "", K_PLAYER, "" },
    [IT_SLOT0 + 1]   = { "", K_PLAYER, "" },
    [IT_SLOT0 + 2]   = { "", K_PLAYER, "" },
    [IT_SLOT0 + 3]   = { "", K_PLAYER, "" },
#if POC_SLOTS > 4
    [IT_SLOT0 + 4]    = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 5
    [IT_SLOT0 + 5]    = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 6
    [IT_SLOT0 + 6]    = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 7
    [IT_SLOT0 + 7]    = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 8
    [IT_SLOT0 + 8]    = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 9
    [IT_SLOT0 + 9]    = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 10
    [IT_SLOT0 + 10]   = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 11
    [IT_SLOT0 + 11]   = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 12
    [IT_SLOT0 + 12]   = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 13
    [IT_SLOT0 + 13]   = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 14
    [IT_SLOT0 + 14]   = { "", K_PLAYER, "" },
#endif
#if POC_SLOTS > 15
    [IT_SLOT0 + 15]   = { "", K_PLAYER, "" },
#endif
    [IT_EXPORT]      = { "EXPORT SAVE", K_ACTION, "A DOWNLOADS A COPY - BACKUP" },
    [IT_IMPORT]      = { "IMPORT SAVE", K_ACTION, "LOAD A SAV FILE" },
    [IT_DELSAVE]     = { "DELETE SAVE", K_CONFIRM, "ERASE THE STORED SAVE" },
    [IT_ROMLOAD]     = { "CHANGE ROM", K_ACTION, "PICK ANOTHER ROM FILE" },
    [IT_ROMFORGET]   = { "FORGET ROM", K_CONFIRM, "ERASE THE STORED ROM" },
    [IT_RANDO]       = { "RANDOMIZER", K_TOGGLE, "SHUFFLE THE ENEMIES" },
    [IT_SEED]        = { "SEED", K_ACTION, "A TYPES A SEED NUMBER" },
    [IT_REROLL]      = { "NEW SEED", K_ACTION, "A PICKS A NEW RANDOM SEED" },
    [IT_SIZES]       = { "MATCH SIZES", K_TOGGLE, "KEEP ENEMY SIZES ALIKE" },
    [IT_ABIL]        = { "KEEP ABILITY", K_TOGGLE, "ABILITIES STAY IN PLACE" },
    [IT_RESET]       = { "RESET GAME", K_CONFIRM, "RESTART - YOUR SAVE IS KEPT" },
    [IT_DIAG]        = { "DIAGNOSTICS", K_TOGGLE, "NUMBERS OVER THE PICTURE" },
    [IT_COPYLOG]     = { "COPY LOG", K_ACTION, "FOR A BUG REPORT" },
    [IT_DEFAULTS]    = { "DEFAULTS", K_CONFIRM, "SETTINGS BACK TO DEFAULT" },
    [IT_BUILD]       = { "BUILD", K_INFO, "THE VERSION YOU ARE PLAYING" },
    [IT_RENDERER]    = { "RENDERER", K_INFO, "" },
    [IT_WEBGL]       = { "WEBGL2", K_INFO, "" },
    [IT_GPU]         = { "GPU", K_INFO, "" },
    [IT_SETSTORE]    = { "STORED", K_INFO, "" },
    /* EXPERIMENTAL: the descriptions are KirbysDesc's */
    [IT_KIRBYS]      = { "CPU KIRBYS", K_SETTING, "" },
    [IT_ADD4]        = { "ADD 4", K_ACTION, "" },
    [IT_REMOVE4]     = { "REMOVE 4", K_ACTION, "" },
    [IT_KICK]        = { "REMOVE", K_CONFIRM, "" },
    [IT_KIRBYS_GO]   = { "CPU KIRBYS", K_CONFIRM, "" },
    [IT_CARD]        = { "PLAYER CARD", K_CARD, "YOUR NAME AND KIRBY COLOR" },
};

/* The sections' items.  Per build: the page's actions on the web, VSYNC
 * natively. */
static const u8 kTopItems[] = {
    IT_SEC_DISPLAY, IT_SEC_SOUND, IT_SEC_PLAYERS, IT_SEC_ONLINE, IT_SEC_GAME, IT_SEC_ADVANCED,
    IT_SEC_EXPERIMENTAL,
    IT_MAINMENU,
};
static const u8 kDisplayItems[] = {
    IT_GRAPHICS, IT_SCREEN, IT_ZOOM, IT_DYNZOOM, IT_CAMERA, IT_INTERP, IT_PIXELS, IT_BORDERS,
    IT_HUD, IT_HUDSIZE, IT_FPS,
#ifdef __EMSCRIPTEN__
    IT_FULLSCREEN,
#else
    IT_VSYNC,
#endif
};
static const u8 kSoundItems[] = { IT_VOLUME, IT_MUTE };
static const u8 kPlayersItems[] = {
    IT_PLAYERS, IT_PARTY, IT_TAGS,
#ifdef __EMSCRIPTEN__
    IT_TOUCH,
#endif
    IT_CARD,
};
static const u8 kOnlineItems[] = {
    IT_CODE, IT_MODE, IT_SLOT0, IT_SLOT0 + 1, IT_SLOT0 + 2, IT_SLOT0 + 3,
    /* the wide build: one row more per Kirby slot (the list scrolls) */
#if POC_SLOTS > 4
    IT_SLOT0 + 4,
#endif
#if POC_SLOTS > 5
    IT_SLOT0 + 5,
#endif
#if POC_SLOTS > 6
    IT_SLOT0 + 6,
#endif
#if POC_SLOTS > 7
    IT_SLOT0 + 7,
#endif
#if POC_SLOTS > 8
    IT_SLOT0 + 8,
#endif
#if POC_SLOTS > 9
    IT_SLOT0 + 9,
#endif
#if POC_SLOTS > 10
    IT_SLOT0 + 10,
#endif
#if POC_SLOTS > 11
    IT_SLOT0 + 11,
#endif
#if POC_SLOTS > 12
    IT_SLOT0 + 12,
#endif
#if POC_SLOTS > 13
    IT_SLOT0 + 13,
#endif
#if POC_SLOTS > 14
    IT_SLOT0 + 14,
#endif
#if POC_SLOTS > 15
    IT_SLOT0 + 15,
#endif
};
static const u8 kGameItems[] = {
    IT_EXPORT, IT_IMPORT, IT_DELSAVE, IT_ROMLOAD, IT_ROMFORGET, IT_RANDO, IT_SEED, IT_REROLL,
    IT_SIZES, IT_ABIL, IT_RESET,
};
static const u8 kAdvancedItems[] = {
    IT_DIAG, IT_COPYLOG, IT_DEFAULTS, IT_BUILD, IT_RENDERER, IT_WEBGL, IT_GPU, IT_SETSTORE,
};
/* EXPERIMENTAL: LIGHTING (its own section), and the CPU Kirbys */
static const u8 kExperimentalItems[] = { IT_SEC_LIGHTING, IT_KIRBYS, IT_ADD4, IT_REMOVE4 };
#define EXP_ROW_LIGHTING 0
static const u8 kConfirmItems[] = { IT_NONE, IT_NONE };     /* NO, YES */

/* Sections kept by other files (port/menu_poc.h "Adding a section"): their
 * rows' definitions, before the NULL.  The index here is the one in
 * EXT_SECTION and in an edit's item number. */
extern const struct PortPocRows gPocLightRows;     /* LIGHTING, below */
static const struct PortPocRows *const kExtRows[] = { &gPocLightRows, NULL };
#define NEXTROWS ((int)ARRAY_COUNT(kExtRows) - 1)

static int ZoomIndexOf(const PortSettings *s)
{
    int i, best = 0;
    float bestD = 99.0f;

    for (i = 0; i < NZOOM; i++) {
        float d = s->zoom - kZoom[i];

        if (d < 0) d = -d;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

static int BorderIndexOf(const PortSettings *s)
{
    int i;

    for (i = 0; i < 4; i++)
        if ((s->letterbox & 0xFFFFFFu) == kBorder[i])
            return i;
    return 4;
}

/* The display queries (platform/ui/settings.h, present.c):
 *   PortDisplayOptionsAvailable: 0 while the page draws on its 2D canvas,
 *     where the picture options do nothing.
 *   PortDisplayStatus: five lines ("BUILD: ...", "RENDERER: ...", "WEBGL2:
 *     ...", "GPU: ...", "SETTINGS: ..."), '\n' between them. */
static int PocDisplayFallback(void)
{
    return !PortDisplayOptionsAvailable();
}

/* Line `n` of PortDisplayStatus (0 BUILD, 1 RENDERER, 2 WEBGL2, 3 GPU, 4
 * SETTINGS) after its "NAME: ", drawable, at most max characters. */
static void StatusLine(int n, char *out, int max)
{
    const char *t = PortDisplayStatus(), *e;
    int i, len;

    out[0] = '\0';
    for (i = 0; i < n && t; i++) {
        t = strchr(t, '\n');
        if (t) t++;
    }
    if (t == NULL || !*t)
        return;
    e = strchr(t, '\n');
    len = e ? (int)(e - t) : (int)strlen(t);
    {
        const char *c = memchr(t, ':', (size_t)len);

        if (c != NULL) {
            len -= (int)(c + 1 - t);
            t = c + 1;
            while (len > 0 && *t == ' ') { t++; len--; }
        }
    }
    if (len > 63) len = 63;
    memcpy(out, t, (size_t)len);
    out[len] = '\0';
    for (i = 0; out[i]; i++)
        if (out[i] == '|') out[i] = '/';
    if ((int)strlen(out) > max) out[max] = '\0';
}

/* --- online: per-device settings ----------------------------------------------------
 *
 * The page is drawn into game memory, so in a session it may show only what
 * every device holds alike, and an edit must rewind with the frame that made
 * it.  So:
 *
 *   - Each player slot has a copy of its device's settings in session state
 *     (sSlotSet below, carried with this file's host state in every keyframe
 *     and image and hashed).  A device publishes its own settings whenever
 *     they change (web/ln_session.js `pset`; the host's own directly), and the
 *     host seals them as PORT_LN_EV_PSET events for every slot that device
 *     plays (platform/lmpnet.c), the way player cards' colours travel.  The
 *     page shows the copy of the slot whose player paused.
 *   - An edit (a setting step, a toggle, an action) is written into the
 *     session's edit request (PORT_SIM_LNSET: counter, slot, item, step) and
 *     nothing else happens in the frame.  When the frame is final
 *     (platform/lmpnet.c FinalSettings: every frame on the host, verified
 *     frames on a guest), the device that plays that slot carries the edit
 *     out on itself -- once, whatever was rewound and replayed -- and its
 *     changed settings come back to every device as a sealed copy.
 *
 * Nothing in gPortSettings is ever read for drawing in a session, and
 * nothing a device does to itself reaches the simulation but through the
 * host's sealed events.  Off, none of this runs. */
static u32 sSlotSet[POC_SLOTS];     /* packed (PocPack), valid if sSlotSetOn */
static PocSlotMask sSlotSetOn;      /* bit per slot                          */
static u16 sSlotExt[POC_SLOTS];     /* per slot: the other files' online rows */
/* Each slot's player's name as the host sealed it (PORT_LN_EV_NAME): names
 * are relayed to the guests as presentation, so the list in game memory
 * draws these, which every device applies at the same frame. */
static char sSlotName[POC_SLOTS][PORT_CARD_NAME_MAX + 1];
static u8 sConfirmArg;              /* REMOVE: the slot asked about; CPU
                                     * KIRBYS: the Kirbys in all asked for    */
static u8 sConfirmRow;              /* CPU KIRBYS: the row that asked         */

/* Settings to a word (and back) for the copies: the fields the sections
 * show, nothing native-only. */
static u32 PocPack(const PortSettings *s)
{
    u32 v = 0;
    int vol = s->volume < 0 ? 0 : s->volume > 100 ? 100 : s->volume;

    v |= (u32)(s->renderer & 1);
    v |= (u32)(s->aspect & 7) << 1;
    v |= (u32)(ZoomIndexOf(s) & 7) << 4;
    v |= (u32)(s->smoothCam != 0) << 7;
    v |= (u32)(s->scaling & 3) << 8;
    v |= (u32)vol << 10;
    v |= (u32)(s->mute != 0) << 17;
    v |= (u32)(s->partyHud & 3) << 18;
    v |= (u32)(s->nameTags != 0) << 20;
    v |= (u32)(s->dynZoom != 0) << 21;
    v |= (u32)(s->interp != 0) << 22;
    v |= (u32)(s->hud & 1) << 23;
    v |= (u32)(s->hudScale & 15) << 24;
    v |= (u32)(s->showFps != 0) << 28;
    v |= (u32)(BorderIndexOf(s) & 7) << 29;
    return v;
}

static void PocUnpack(u32 v, PortSettings *s)
{
    int b = (int)(v >> 29) & 7;

    PortSettingsDefaults(s);
    s->renderer = (int)(v & 1);
    s->aspect = (int)(v >> 1) & 7;
    if (s->aspect >= PS_ASPECT_COUNT) s->aspect = 0;
    s->zoom = kZoom[((v >> 4) & 7) < (u32)NZOOM ? (v >> 4) & 7 : NZOOM - 1];
    s->smoothCam = (int)(v >> 7) & 1;
    s->scaling = (int)(v >> 8) & 3;
    if (s->scaling >= PS_SCALE_COUNT) s->scaling = 0;
    s->volume = (int)(v >> 10) & 127;
    s->mute = (int)(v >> 17) & 1;
    s->partyHud = (int)(v >> 18) & 3;
    if (s->partyHud > PS_PARTY_HELPERS) s->partyHud = PS_PARTY_HELPERS;
    s->nameTags = (int)(v >> 20) & 1;
    s->dynZoom = (int)(v >> 21) & 1;
    s->interp = (int)(v >> 22) & 1;
    s->hud = (int)(v >> 23) & 1;
    s->hudScale = (int)(v >> 24) & 15;
    s->showFps = (int)(v >> 28) & 1;
    s->letterbox = b < 4 ? kBorder[b] : 0x123456u;      /* OTHER */
}

/* This device's settings as a session copy (web/ln_session.js publishes it). */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
u32 PortPocSettingsWord(void)
{
    return PocPack(&gPortSettings);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortPocMenusOn(void)
{
    return PortMenuPoc();
}

/* The online rows of the sections other files keep: four 4-bit values. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
u32 PortPocSettingsExtWord(void)
{
    u32 v = 0;
    int i, r;

    for (i = 0; i < NEXTROWS; i++) {
        const struct PortPocRows *x = kExtRows[i];

        for (r = 0; x->onlineSlot && x->onlineGet && r < 16; r++) {
            int o = x->onlineSlot(r);

            if (o >= 0 && o < 4)
                v |= (u32)(x->onlineGet(o) & 15) << (4 * o);
        }
    }
    return v;
}

/* A sealed PORT_LN_EV_PSET (platform/lmpnet.c ApplyEvents). */
void PortPocSlotSettingsSet(int slot, u32 word, u32 ext)
{
    if (slot < 0 || slot >= POC_SLOTS)
        return;
    sSlotExt[slot] = (u16)ext;
    sSlotSet[slot] = word;
    sSlotSetOn |= (PocSlotMask)(1u << slot);
}

/* A sealed PORT_LN_EV_NAME, and what the host compares before sealing.
 * The letters came from another device: made drawable here (the same on
 * every device -- PortCardCleanName is a pure function of them). */
void PortPocSlotNameSet(int slot, u32 lo, u32 hi)
{
    int i;

    if (slot < 0 || slot >= POC_SLOTS)
        return;
    for (i = 0; i < 8 && i < PORT_CARD_NAME_MAX; i++) {
        u32 w = i < 4 ? lo : hi;
        char c = (char)((w >> (8 * (i & 3))) & 0xFF);

        sSlotName[slot][i] = (c >= ' ' && c < 127) ? c : '\0';
        if (!c)
            break;
    }
    sSlotName[slot][i < PORT_CARD_NAME_MAX ? i : PORT_CARD_NAME_MAX] = '\0';
    PortCardCleanName(sSlotName[slot]);
}

const char *PortPocSlotName(int slot)
{
    return slot >= 0 && slot < POC_SLOTS ? sSlotName[slot] : "";
}

void PortPocNameWords(const char *name, u32 *lo, u32 *hi)
{
    int i;

    *lo = *hi = 0;
    for (i = 0; i < 8 && name[i]; i++)
        *(i < 4 ? lo : hi) |= (u32)(u8)name[i] << (8 * (i & 3));
}

int PortPocSlotNameSame(int slot, const char *name)
{
    return slot >= 0 && slot < POC_SLOTS && strncmp(sSlotName[slot], name, PORT_CARD_NAME_MAX) == 0;
}

/* The host, before sealing: what slot `slot`'s copy holds (-1: none). */
int PortPocSlotSettingsHave(int slot, u32 *word, u32 *ext)
{
    if (slot < 0 || slot >= POC_SLOTS || !(sSlotSetOn & (1u << slot)))
        return 0;
    *word = sSlotSet[slot];
    *ext = sSlotExt[slot];
    return 1;
}

/* The host, as its session starts: the edit request and the room code. */
void PortPocSessionSettingsInit(const char code[4])
{
    u8 *c = PORT_SIM_LNSET;

    memset(c, 0, PORT_SIM_LNSET_BYTES);
    c[0] = 2;
    if (code != NULL && code[0]) {
        c[5] = 1;
        memcpy(&c[6], code, 4);
    }
}

/* The session's room code (session state: the host's, with every image), or
 * NULL outside a session. */
static const char *SessionCode(void)
{
    static char code[5];
    const u8 *c = PORT_SIM_LNSET;

    if (!gPortLnOn || c[0] != 2 || !c[5])
        return NULL;
    memcpy(code, &c[6], 4);
    code[4] = '\0';
    return code;
}

/* Is the page drawn from the session's copies, and does an edit go through
 * the session's edit request?  In a session -- except on this device's own
 * pause overlay (platform/lmpnet.c "the pause overlay"), which runs on a
 * private copy of the game no other device sees: there the page is this
 * device's, as outside a session, and an edit is carried out on it at the
 * next live frame.  What a session forbids (the rows dimmed NOT DURING AN
 * ONLINE GAME) stays forbidden on the overlay too: those tests stay
 * gPortLnOn. */
static int PocShared(void)
{
    return gPortLnOn && !PortLnOverlayRunning();
}

/* The player whose pause menu this is (the simulation's: gUnk_0203AD50). */
static int Pauser(void)
{
    return gUnk_0203AD50 < POC_SLOTS ? gUnk_0203AD50 : 0;
}

/* The settings the page shows: this device's own; in a session the copy of
 * the pausing player's device (NULL while it has none yet). */
static const PortSettings *Shown(void)
{
    static PortSettings copy;

    if (!PocShared())
        return &gPortSettings;
    if (!(sSlotSetOn & (1u << Pauser())))
        return NULL;
    PocUnpack(sSlotSet[Pauser()], &copy);
    return &copy;
}

/* --- what the page asks of the web page (web/shell.html portUiAction) ------------ */

static int HostAct(const char *name, int arg)
{
    if (PortInputLogReplaying())
        return 0;               /* a replay never reaches the page */
    return PortHostUiAction(name, arg);
}

static int HostOn(const char *key)
{
    char b[8];

    b[0] = '\0';
    PortHostUiQuery(key, b, (int)sizeof b);
    return b[0] == '1';
}

/* The randomizer as the page has it: pools, sizes, abilities, seed. */
static void Rando(unsigned v[4])
{
    char b[64];

    v[0] = 0; v[1] = 1; v[2] = 1; v[3] = 0;
    b[0] = '\0';
    PortHostUiQuery("rando", b, (int)sizeof b);
    if (b[0])
        sscanf(b, "%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3]);
}

static int Web(void)
{
#ifdef __EMSCRIPTEN__
    return 1;
#else
    return 0;
#endif
}

/* --- items: values, availability, steps -------------------------------------------- */

static void OnOff(char *buf, int size, int on) { snprintf(buf, size, "%s", on ? "ON" : "OFF"); }

/* PLAYERS > TOUCH PADS: gPortSettings.touch (PS_TOUCH_*), this device's
 * own; the page shows or hides its pads from the saved text (shell.html). */
static const char *const kTouchName[PS_TOUCH_COUNT] = { "AUTO", "ON", "OFF" };
static const char *TouchDesc(void)
{
    if (gPortLnOn || gPortSettings.touch != PS_TOUCH_AUTO)
        return kItem[IT_TOUCH].desc;
    return HostOn("touch") ? "AUTO - SHOWN ON THIS DEVICE" : "AUTO - ON PHONES AND TABLETS";
}

static void Drawable(char *s, int max);

/* --- the ONLINE list: who plays each Kirby ----------------------------------------
 * Session state only: the slot table (PortLnSlotOwner, sealed, in every image
 * and keyframe), the people (PortLmpSlotPerson) and the sealed names. */

/* The device that plays slot s: 0 the host, 1..3 a guest, -1 the CPU. */
static int SlotDevice(int s)
{
    int o = PortLnSlotOwner(s);

    if (s == 0)
        return 0;                   /* slot 0 is always the host's own */
    if (o < 0 || !PortLmpSlotPerson(s))
        return -1;
    return o >> 2;
}

/* Is the pausing player on the host's device?  (The host's own Kirbys:
 * END GAME, REMOVE.) */
static int PauserIsHost(void)
{
    return SlotDevice(Pauser()) == 0;
}

/* Has slot s a Kirby in the world?  Always, with four; the wide build's
 * count changes in play (platform/wide.c), and its list has a row for every
 * slot the module has.  A person's slot counts as soon as the join is held
 * (its Kirby follows). */
static int SlotExists(int s)
{
#if KATAM_WIDE
    return s < gNumKirbys || SlotDevice(s) >= 0;
#else
    (void)s;
    return 1;
#endif
}

/* The slot shown on row k of the list: by device (the host's first, then
 * each guest's), so Kirbys that share a device sit together; the CPU last,
 * and last of all (the wide build) the slots with no Kirby now. */
static int RowSlot(int k)
{
    int order[POC_SLOTS], n = 0, d, s;

    for (d = 0; d < PORT_LN_MAX_DEV; d++)
        for (s = 0; s < POC_SLOTS; s++)
            if (SlotDevice(s) == d)
                order[n++] = s;
    for (s = 0; s < POC_SLOTS; s++)
        if (SlotDevice(s) < 0 && SlotExists(s))
            order[n++] = s;
    for (s = 0; s < POC_SLOTS; s++)
        if (SlotDevice(s) < 0 && !SlotExists(s))
            order[n++] = s;
    return order[(unsigned)k % POC_SLOTS];
}

static void SlotWho(int s, char *buf, int size)
{
    int d = SlotDevice(s);

    if (d < 0) snprintf(buf, size, "CPU");
    else if (d == 0) snprintf(buf, size, "HOST");
    else snprintf(buf, size, "GUEST %d", d);
}

/* A list row's label: the Kirby's tag and its player's name, shortened
 * until it fits beside the device. */
static const char *SlotLabel(int k)
{
    static char lbl[POC_SLOTS][24];
    char who[12], name[PORT_CARD_NAME_MAX + 1];
    int s = RowSlot(k), n;

    k = (int)((unsigned)k % POC_SLOTS);
    if (!gPortLnOn) {
        snprintf(lbl[k], sizeof lbl[0], "%dP", k + 1);
        return lbl[k];
    }
    SlotWho(s, who, sizeof who);
    snprintf(name, sizeof name, "%s", SlotDevice(s) < 0 ? "" : sSlotName[s]);
    Drawable(name, PORT_CARD_NAME_MAX);
    for (n = (int)strlen(name); ; n--) {
        name[n] = '\0';
        snprintf(lbl[k], sizeof lbl[0], "%dP%s%s", s + 1, n ? " " : "", name);
        if (n == 0 || LBL_TEXT_LEFT + CellWidth(lbl[k]) + 8 + CellWidth(who) <= ROW_VALUE_X1)
            break;
    }
    return lbl[k];
}

/* Drawable text: the fallback font's letters, upper case. */
static void Drawable(char *s, int max)
{
    int i, o = 0;

    for (i = 0; s[i] && o < max; i++) {
        char c = s[i];

        if (c >= 'a' && c <= 'z')
            c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.'
            || c == ':' || c == '/' || c == '%' || c == '+' || c == '!')
            s[o++] = c;
        else if (c == '_' || c == ',' || c == '(' || c == ')')
            s[o++] = ' ';
    }
    s[o] = '\0';
}

/* --- EXPERIMENTAL > CPU KIRBYS ---------------------------------------------------
 * gPortSettings.kirbys is the Kirbys in all (4..16); the row shows the CPU
 * helpers (3..15), Left / Right one at a time, ADD 4 / REMOVE 4 four.  In
 * the wide build (port/wide.h: the web page's second module, katam16) a
 * change applies at once, in the running game (platform/wide.c "The live
 * count").  The default build has four Kirby slots: on the web a count
 * above 4 asks first (NO / YES), then the page reloads with the wide module
 * (web/shell.html "kirbys.restart"); natively it needs a wide build.  Never
 * during an online game (the host's EV_KIRBYS changes it there); in a local
 * multiplayer game on the wide build live as alone (the count never goes
 * below a person: platform/localmp.c), on the default build not at all (it
 * would restart the game). */
static int KirbysMax(void)
{
    return KATAM_WIDE ? PortWideMax() : 16;
}

/* Tests (the page's browser test): Kirbys in play now, and the most this
 * module has.  Read only. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortPocKirbysNow(void)
{
    return gNumKirbys;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortPocKirbysMax(void)
{
    return KATAM_KIRBYS;
}

/* More than four Kirbys are drawn by the ENHANCED (GPU) renderer only: the
 * CLASSIC one shows what the console could, four Kirbys' sprite memory, so
 * Kirbys 5.. would come out as another Kirby's pose and colours (port/wide.h
 * KATAM_KIRBY_TILES).  So while more than four are in the world the picture
 * is ENHANCED whatever GRAPHICS says (platform/ui/present.c, per device) and
 * the GRAPHICS row is dimmed with the reason; where there is no GPU path at
 * all (the 2D fallback) the count may come down but not go above four.
 * Device state: never asked in a session (KirbysWhyNot). */
static int KirbysCanShow(void)
{
    return PortShowGpuAvailable();
}

/* Is the picture held at ENHANCED now (more than four Kirbys in the world,
 * or -- outside a session -- this device's CPU KIRBYS setting above four)?
 * The Kirbys in the world are simulation state, the same on every device. */
static int GpuHeld(void)
{
    return PortWideNeedsGpu() || (KATAM_WIDE && !gPortLnOn && gPortSettings.kirbys > 4);
}

/* Why GRAPHICS may not be changed now, or NULL.  `gpu`: its setting is
 * ENHANCED. */
static const char *GraphicsWhyNot(int gpu)
{
    if (!GpuHeld())
        return NULL;
    return gpu ? "CLASSIC SHOWS ONLY 4 KIRBYS" : "ENHANCED WHILE OVER 4 KIRBYS";
}

/* The total item `it` (step dir) asks for. */
static int KirbysNext(int it, int dir)
{
    int n = gPortSettings.kirbys, max = KirbysMax();

    if (!KirbysCanShow() && max > (n < 4 ? 4 : n))
        max = n < 4 ? 4 : n;            /* CLASSIC: down, never up */

    if (it == IT_ADD4) n += 4;
    else if (it == IT_REMOVE4) n -= 4;
    else n += dir;
    return n < 4 ? 4 : n > max ? max : n;
}

/* Does a total of n need the page to reload with the other module? */
static int KirbysRestart(int n)
{
    return !KATAM_WIDE && n > 4;
}

static const char *KirbysWhyNot(int it)
{
    if (gPortLnOn)
        return "NOT DURING AN ONLINE GAME";
    if (!KATAM_WIDE && !Web())
        return "NEEDS THE 16 KIRBY BUILD";
    if (!KATAM_WIDE && (PortLmpInPlay() || PortLmpSetting() > 1))
        return "NOT IN A 2 PLAYER GAME";
    if (!KirbysCanShow() && (it == IT_ADD4 || (it == IT_KIRBYS && gPortSettings.kirbys <= 4)))
        return "NOT WITH THE 2D FALLBACK";
    if (it == IT_ADD4 && gPortSettings.kirbys >= KirbysMax())
        return KirbysMax() < 16 ? "THE MOST THIS BUILD HAS" : "16 IS THE MOST";
    if (it == IT_REMOVE4 && gPortSettings.kirbys <= 4)
        return "3 IS THE FEWEST";
    return NULL;
}

static const char *KirbysDesc(int it)
{
    static char line[2][32];
    static int flip;
    int n = gPortSettings.kirbys;
    char *l = line[flip ^= 1];

    switch (it) {
    case IT_KIRBYS:
        if (!KATAM_WIDE)
            snprintf(l, 32, "%d IN ALL - MORE RESTARTS", n);
        else if (n > 4 && !KirbysCanShow())
            snprintf(l, 32, "2D SHOWS ONLY 4 - LEFT LOWERS");
        else if (n > 4)
            snprintf(l, 32, "%d IN ALL - EXPERIMENTAL", n);
        else
            snprintf(l, 32, "4 IN ALL - RIGHT ADDS MORE");
        return l;
    case IT_ADD4:
        return KirbysRestart(KirbysNext(it, 1)) ? "4 MORE - THE GAME RESTARTS" : "4 MORE CPU KIRBYS";
    case IT_REMOVE4:
        return "4 FEWER CPU KIRBYS";
    default:
        return "";
    }
}

static int CardOfPauser(void);

static void ItemValue(int it, char *buf, int size)
{
    const PortSettings *s = Shown();

    buf[0] = '\0';
    if (kItem[it].kind == K_SETTING && s == NULL) {
        snprintf(buf, size, "-");       /* that device has not told us yet */
        return;
    }
    switch (it) {
    case IT_SEC_ONLINE: {
        const char *code = SessionCode();

        snprintf(buf, size, "%s", code ? code : "");
        break;
    }
    case IT_GRAPHICS:  /* (held at ENHANCED with more than four Kirbys) */
        snprintf(buf, size, "%s", s->renderer == PS_RENDER_GPU || GpuHeld() ? "ENHANCED" : "CLASSIC");
        break;
    case IT_SCREEN:    snprintf(buf, size, "%s", kAspect[s->aspect]); break;
    case IT_ZOOM:      snprintf(buf, size, "%s", kZoomName[ZoomIndexOf(s)]); break;
    case IT_DYNZOOM:   OnOff(buf, size, s->dynZoom); break;
    case IT_CAMERA:    snprintf(buf, size, "%s", s->smoothCam ? "SMOOTH" : "NORMAL"); break;
    case IT_INTERP:    OnOff(buf, size, s->interp); break;
    case IT_PIXELS:    snprintf(buf, size, "%s", kScaling[s->scaling]); break;
    case IT_BORDERS:   snprintf(buf, size, "%s", kBorderName[BorderIndexOf(s)]); break;
    case IT_HUD:       snprintf(buf, size, "%s", s->hud == PS_HUD_STRIP ? "STRIP" : "CORNERS"); break;
    case IT_HUDSIZE:
        if (s->hudScale <= 0) snprintf(buf, size, "AUTO");
        else snprintf(buf, size, "%dX", s->hudScale);
        break;
    case IT_FPS:       OnOff(buf, size, s->showFps); break;
    case IT_VSYNC:     OnOff(buf, size, s->vsync); break;
    case IT_VOLUME:    snprintf(buf, size, "%d%%", s->volume); break;
    case IT_MUTE:      OnOff(buf, size, s->mute); break;
    case IT_PARTY:     snprintf(buf, size, "%s", kPartyName[s->partyHud <= PS_PARTY_HELPERS ? s->partyHud : 0]); break;
    case IT_TAGS:      OnOff(buf, size, s->nameTags); break;
    case IT_PLAYERS:   PortPuiSummary(buf, size); break;
    case IT_CARD:      /* (device state: never on a shared page) */
        if (CardOfPauser() >= 0) snprintf(buf, size, "%s", PortCardShownName(CardOfPauser()));
        break;
    case IT_KIRBYS:    if (!gPortLnOn) snprintf(buf, size, "%d", gPortSettings.kirbys - 1); break;
    /* the page's own state: per device, so never shown in a session */
    case IT_FULLSCREEN: if (!gPortLnOn) OnOff(buf, size, HostOn("fullscreen")); break;
    case IT_TOUCH:      if (!gPortLnOn) snprintf(buf, size, "%s", kTouchName[gPortSettings.touch]); break;
    case IT_DIAG:       if (!gPortLnOn) OnOff(buf, size, HostOn("diag")); break;
    case IT_RANDO: case IT_SEED: case IT_SIZES: case IT_ABIL: {
        unsigned v[4];

        if (gPortLnOn || !Web())
            break;
        Rando(v);
        if (it == IT_RANDO) snprintf(buf, size, "%s", v[0] == 0 ? "OFF" : v[0] == 1 ? "SHUFFLE" : "BOSSES TOO");
        else if (it == IT_SEED) snprintf(buf, size, "%u", v[3]);
        else OnOff(buf, size, it == IT_SIZES ? v[1] != 0 : v[2] != 0);
        break;
    }
    /* ONLINE: session state only */
    case IT_CODE: {
        const char *code = SessionCode();

        snprintf(buf, size, "%s", code ? code : "----");
        break;
    }
    case IT_MODE:
        snprintf(buf, size, "%s", !gPortLnOn ? "-" : gPortSim->view.lmpRooms ? "FREE ROAM" : "TOGETHER");
        break;
    case IT_SLOT0 ... IT_SLOT_LAST:
        if (gPortLnOn && SlotExists(RowSlot(it - IT_SLOT0))) SlotWho(RowSlot(it - IT_SLOT0), buf, size);
        else snprintf(buf, size, "-");
        break;
    case IT_BUILD:
        snprintf(buf, size, "%s", PortUiBuildLabel());
        Drawable(buf, 14);
        break;
    case IT_RENDERER: case IT_WEBGL: case IT_GPU: case IT_SETSTORE: {
        char line[64];
        int n;

        if (gPortLnOn)
            break;                  /* this device's: not in a session */
        StatusLine(it - IT_RENDERER + 1, line, 63);
        Drawable(line, 63);
        /* as much as fits beside the name; the description has more */
        for (n = (int)strlen(line); n > 0; n--) {
            line[n] = '\0';
            if (LBL_TEXT_LEFT + CellWidth(kItem[it].name) + 8 + CellWidth(line) <= ROW_VALUE_X1)
                break;
        }
        snprintf(buf, size, "%s", line);
        break;
    }
    default:
        break;
    }
}

/* Why an item cannot be used now, or NULL if it can.  In a session only
 * session state decides. */
static const char *ItemWhyNot(int it)
{
    /* on the 2D canvas the picture options do nothing (the GL path's) */
    int display = it == IT_GRAPHICS || it == IT_SCREEN || it == IT_ZOOM || it == IT_DYNZOOM
               || it == IT_CAMERA || it == IT_INTERP || it == IT_PIXELS || it == IT_FPS;
    int game = it >= IT_EXPORT && it <= IT_RESET;

    if (display && PocDisplayFallback())
        return "NOT WITH THE 2D FALLBACK";
    if (it == IT_GRAPHICS) {
        /* more than four Kirbys: held at ENHANCED (KirbysCanShow); in a
         * session the pausing player's setting */
        const PortSettings *sh = Shown();
        const char *why = GraphicsWhyNot(sh != NULL && sh->renderer == PS_RENDER_GPU);

        if (why != NULL)
            return why;
    }
    if (it == IT_KIRBYS || it == IT_ADD4 || it == IT_REMOVE4)
        return KirbysWhyNot(it);
    if (it == IT_CARD && CardOfPauser() < 0)
        return gPortLnOn ? "NOT DURING AN ONLINE GAME" : "NO CARD FOR THIS PLAYER";
    /* the CLASSIC renderer draws the GBA's 240 x 160 only: the view's
     * shape, zoom and camera are the ENHANCED renderer's (GRAPHICS stays
     * live to switch back).  In a session: the pausing player's setting. */
    if (it == IT_SCREEN || it == IT_ZOOM || it == IT_DYNZOOM || it == IT_CAMERA || it == IT_INTERP) {
        const PortSettings *sh = Shown();
        int classic = gPortLnOn ? (sh != NULL && sh->renderer == PS_RENDER_PPU && !GpuHeld())
                                : !PortShowUsingGpu();

        if (classic)
            return "NEEDS ENHANCED GRAPHICS";
    }
    if (game && gPortLnOn)
        return "NOT DURING AN ONLINE GAME";
    if ((game || it == IT_FULLSCREEN || it == IT_TOUCH || it == IT_DIAG || it == IT_COPYLOG) && !Web())
        return "ON THE WEB PAGE ONLY";
    if (it >= IT_RENDERER && it <= IT_SETSTORE && gPortLnOn)
        return "NOT DURING AN ONLINE GAME";
    if ((it == IT_CODE || it == IT_MODE || IS_SLOT_ITEM(it)) && !gPortLnOn)
        return "NOT IN AN ONLINE GAME";
    if (IS_SLOT_ITEM(it) && !SlotExists(RowSlot(it - IT_SLOT0)))
        return "NO KIRBY IN THIS SLOT NOW";
    if (gPortLnOn)
        return NULL;
    /* the page's state, outside a session */
    if (it == IT_DELSAVE && !HostOn("save.stored"))
        return "NO SAVE IS STORED";
    if (it == IT_ROMFORGET && !HostOn("rom.stored"))
        return "NO ROM IS STORED";
    if (it == IT_SEED || it == IT_REROLL || it == IT_SIZES || it == IT_ABIL) {
        unsigned v[4];

        Rando(v);
        if (v[0] == 0)
            return "TURN THE RANDOMIZER ON";
    }
    return NULL;
}

static const char *ItemDesc(int it)
{
    const char *why = ItemWhyNot(it);
    static char line[40];

    if (why != NULL)
        return why;
    switch (it) {
    case IT_PLAYERS:
        return gPortLnOn ? "WHO PLAYS EACH KIRBY" : kItem[it].desc;
    case IT_MAINMENU:
        if (gPortLnOn)
            return PauserIsHost() ? "END THE GAME FOR EVERYONE" : "LEAVE - THE CPU TAKES OVER";
        return kItem[it].desc;
    case IT_SEC_ONLINE:
        return gPortLnOn ? "CODE MODE AND PLAYERS" : "NOT IN AN ONLINE GAME";
    case IT_KIRBYS: case IT_ADD4: case IT_REMOVE4:
        return KirbysDesc(it);
    case IT_TOUCH:
        return TouchDesc();
    case IT_CARD:
        return gPortLnOn ? "EVERYONE SEES IT AS YOU GO" : kItem[it].desc;
    case IT_MODE:
        return gPortSim->view.lmpRooms ? "EACH PLAYS ON - MEET BY PHONE" : "ONE ROOM FOR EVERYONE";
    case IT_SLOT0 ... IT_SLOT_LAST: {
        int d = SlotDevice(RowSlot(it - IT_SLOT0));

        if (d < 0) return "THE CPU PLAYS THIS KIRBY";
        if (d == 0) return "THE HOST PLAYS THIS KIRBY";
        return PauserIsHost() ? "A REMOVES THIS PLAYER" : "ONLY THE HOST CAN REMOVE";
    }
    case IT_RENDERER: case IT_WEBGL: case IT_GPU: case IT_SETSTORE:
        /* the whole line (as much as the description holds) */
        StatusLine(it - IT_RENDERER + 1, line, 28);
        Drawable(line, 28);
        return line[0] ? line : it == IT_RENDERER ? "HOW THE PICTURE IS DRAWN"
             : it == IT_WEBGL ? "THE BROWSERS 3D GRAPHICS" : it == IT_GPU ? "THE GRAPHICS CHIP"
             : "HOW YOUR SETTINGS ARE KEPT";
    default:
        return kItem[it].desc;
    }
}

/* dir -1 or +1; wraps for the lists, stops at the ends for the numbers. */
static void RowStepOn(PortSettings *s, int it, int dir)
{
    switch (it) {
    case IT_GRAPHICS:  s->renderer = s->renderer == PS_RENDER_GPU ? PS_RENDER_PPU : PS_RENDER_GPU; break;
    case IT_SCREEN:    s->aspect = (s->aspect + dir + PS_ASPECT_COUNT) % PS_ASPECT_COUNT; break;
    case IT_ZOOM: {
        int z = ZoomIndexOf(s) + dir;

        s->zoom = kZoom[z < 0 ? 0 : z >= NZOOM ? NZOOM - 1 : z];
        break;
    }
    case IT_DYNZOOM:   s->dynZoom = !s->dynZoom; break;
    case IT_CAMERA:    s->smoothCam = !s->smoothCam; break;
    case IT_INTERP:    s->interp = !s->interp; break;
    case IT_PIXELS:    s->scaling = (s->scaling + dir + PS_SCALE_COUNT) % PS_SCALE_COUNT; break;
    case IT_BORDERS: {
        int b = BorderIndexOf(s);

        b = b >= 4 ? (dir > 0 ? 0 : 3) : (b + dir + 4) % 4;
        s->letterbox = kBorder[b];
        break;
    }
    case IT_HUD:       s->hud = s->hud == PS_HUD_STRIP ? PS_HUD_CORNERS : PS_HUD_STRIP; break;
    case IT_HUDSIZE:   s->hudScale = (s->hudScale + dir + PS_HUD_SCALE_MAX + 1) % (PS_HUD_SCALE_MAX + 1); break;
    case IT_FPS:       s->showFps = !s->showFps; break;
    case IT_VSYNC:     s->vsync = !s->vsync; break;
    case IT_PARTY:     s->partyHud = (s->partyHud + dir + PS_PARTY_HELPERS + 1) % (PS_PARTY_HELPERS + 1); break;
    case IT_TAGS:      s->nameTags = !s->nameTags; break;
    case IT_MUTE:      s->mute = !s->mute; break;
    case IT_VOLUME:
        if (s->mute) {
            s->mute = 0;
        } else {
            s->volume = (s->volume / 10) * 10 + dir * 10;
            if (s->volume < 0) s->volume = 0;
            if (s->volume > 100) s->volume = 100;
        }
        break;
    }
}

/* An item carried out on THIS device: at once outside a session, from a
 * final frame inside one (PortPocSessionEditApply). */
static void ItemApply(int it, int dir)
{
    if (it == IT_KIRBYS || it == IT_ADD4 || it == IT_REMOVE4 || it == IT_KIRBYS_GO) {
        int n = it == IT_KIRBYS_GO ? dir : KirbysNext(it, dir);

        if (gPortLnOn || PortInputLogReplaying())
            return;
        gPortSettings.kirbys = n;
        PortSettingsClamp(&gPortSettings);
        PortSettingsChanged();              /* saved: the page reads it at load */
        if (KirbysRestart(gPortSettings.kirbys))
            HostAct("kirbys.restart", gPortSettings.kirbys);
        else
            PortWideRequestKirbys(gPortSettings.kirbys);    /* live (wide build) */
        return;
    }
    if (it >= 0x80) {                   /* a section another file keeps */
        int x = (it >> 4) & 7;

        if (x < NEXTROWS && kExtRows[x]->step)
            kExtRows[x]->step(it & 15, dir);
        return;
    }
    switch (kItem[it].kind) {
    case K_SETTING:
        RowStepOn(&gPortSettings, it, dir);
        /* What the settings menu does after a change (ui_menu.cpp): clamp,
         * apply, persist. */
        PortSettingsClamp(&gPortSettings);
        PortSettingsChanged();
        return;
    default:
        break;
    }
    switch (it) {
    case IT_FULLSCREEN: HostAct("fullscreen", 0); break;
    case IT_TOUCH:                      /* AUTO / ON / OFF, saved */
        if (PortInputLogReplaying())
            break;
        gPortSettings.touch = (gPortSettings.touch + (dir < 0 ? -1 : 1) + PS_TOUCH_COUNT) % PS_TOUCH_COUNT;
        PortSettingsClamp(&gPortSettings);
        PortSettingsChanged();          /* the page hears of it as it is saved */
        break;
    case IT_DIAG:       HostAct("diag", 0); break;
    case IT_COPYLOG:    HostAct("log.copy", 0); break;
    case IT_EXPORT:     HostAct("save.export", 0); break;
    case IT_IMPORT:     HostAct("save.import", 0); break;
    case IT_DELSAVE:    HostAct("save.delete", 0); break;
    case IT_ROMLOAD:    HostAct("rom.load", 0); break;
    case IT_ROMFORGET:  HostAct("rom.forget", 0); break;
    case IT_RESET:      HostAct("reset", 0); break;
    case IT_SEED:       HostAct("rando.seed", 0); break;
    case IT_REROLL:     HostAct("rando.reroll", 0); break;
    case IT_SIZES:      HostAct("rando.size", 0); break;
    case IT_ABIL:       HostAct("rando.ability", 0); break;
    case IT_RANDO: {
        static const int kPools[3] = { 0, 1, 7 };
        unsigned v[4];
        int i;

        Rando(v);
        i = v[0] == 0 ? 0 : v[0] == 1 ? 1 : 2;
        HostAct("rando.pools", kPools[(i + dir + 3) % 3]);
        break;
    }
    case IT_DEFAULTS:
        PortSettingsDefaults(&gPortSettings);
        PortSettingsChanged();
        if (KATAM_WIDE)
            PortWideRequestKirbys(gPortSettings.kirbys);    /* CPU KIRBYS 3, live */
        break;
    case IT_KICK:                       /* the host's device: remove a guest's
                                         * (dir: the slot, web/online.js kick) */
        if (gPortLnOn && PortLnRole() == PORT_LN_HOST && dir >= 1 && dir < POC_SLOTS)
            HostAct("online.kick", dir);
        break;
    case IT_MAINMENU:
        if (gPortLnOn)
            HostAct("online.leave", 0);     /* guest: leave; host: end it */
        else if (!PortInputLogReplaying())
            PortLnGoHomeLocal();            /* back to the title at the next frame */
        break;
    }
}

/* platform/lmpnet.c, a final frame's edit by a player of this device. */
void PortPocSessionEditApply(int item, int dir)
{
    if (item <= IT_NONE || (item >= IT_COUNT && item < 0x80) || item > 0xFF)
        return;
    ItemApply(item, dir);
}

/* An edit on the page: carried out here and now, or in a session written
 * into the session's edit request for the pausing player's device. */
static void ItemDo(int it, int dir)
{
    if (gPortLnOn && !PocShared()) {
        PortLnOverlayEdit(it, dir);     /* this device's own pause overlay */
        return;
    }
    if (gPortLnOn) {
        u8 *c = PORT_SIM_LNSET;

        if (c[0] != 2)
            return;
        c[1]++;
        c[2] = (u8)Pauser();
        c[3] = (u8)it;
        c[4] = (u8)(s8)dir;
        return;
    }
    ItemApply(it, dir);
}

/* --- what a list screen shows ----------------------------------------------------
 *
 * The screen below draws a model: its rows' names and values, a header, a
 * description per row, and what Left/Right/A do.  The settings' sections
 * and NO / YES screen, the front end's MULTIPLAYER choice and ONLINE
 * screens, the PLAYERS page and the player card. */
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
    /* an ONLINE sub-screen (platform/online_ui.c draws its text): its
     * PORT_OUI_SUB_*, 0 for the others */
    int oui;
    /* rows and description redrawn when this changes (NULL: on input only) */
    u32 (*gen)(void);
    /* the PLAYERS page (platform/players_ui.c): values carry their own
     * arrows, Left/Right/A are its own */
    int pui;
    /* a player card screen (platform/player_card.c): its PORT_CARD_SUB_* */
    int card;
    /* a settings section: its items (rows); NULL for the others */
    const u8 *items;
    /* where B goes (and the row it lands on); NULL: the caller's own B */
    const struct PocModel *parent;
    int parentRow;
    /* a section whose rows another file keeps (port/menu_poc.h
     * struct PortPocRows), with its index in kExtRows */
    const struct PortPocRows *ext;
    int extIndex;
};

static const char *SecName(int row);
static void SecValue(int row, char *buf, int size);
static const char *SecDesc(int row);
static int SecStep(int row, int dir);
static int SecUnavailable(int row);
static u32 SecGen(void);
/* (MAIN MENU is kTopItems' last: the front end shows the rest) */
static const struct PocModel kTopModel = {
    (int)ARRAY_COUNT(kTopItems) - 1, "SETTINGS", SecName, SecValue, SecDesc, SecStep, SecUnavailable, 0, SecGen, 0, 0, kTopItems, NULL, 0, NULL, 0,
};
static const struct PocModel kTopPauseModel = {
    (int)ARRAY_COUNT(kTopItems), "SETTINGS", SecName, SecValue, SecDesc, SecStep, SecUnavailable, 0, SecGen, 0, 0, kTopItems, NULL, 0, NULL, 0,
};
/* The model up now (never NULL: a device that took an image without the
 * host state still draws something). */
static const struct PocModel *sModel = &kTopModel;
static u8 sConfirmItem;             /* the item a NO / YES screen asks about */

static int ModelItem(int row)
{
    return sModel->items != NULL && row >= 0 && row < sModel->rows ? sModel->items[row] : IT_NONE;
}

/* --- sections kept by another file (port/menu_poc.h struct PortPocRows) --------
 * Its rows' edits travel like the others': item EXT_ITEM(section, row) in
 * the session's edit request.  In a session only the rows it names as online
 * rows work, their values drawn from the slots' copies (a 4-bit value each,
 * four such rows in all, in the second word of PORT_LN_EV_PSET); the rest
 * are dimmed. */
#define EXT_ITEM(x, r)   (0x80 | ((x) << 4) | ((r) & 15))

static int ExtOnline(const struct PortPocRows *x, int row)
{
    return x->onlineSlot ? x->onlineSlot(row) : -1;
}

static int ExtUnavailable(int row)
{
    const struct PortPocRows *x = sModel->ext;

    if (gPortLnOn && (ExtOnline(x, row) < 0 || (PocShared() && !(sSlotSetOn & (1u << Pauser())))))
        return 1;
    return x->available ? !x->available(row) : 0;
}

static const char *ExtDesc(int row)
{
    const struct PortPocRows *x = sModel->ext;

    if (gPortLnOn && ExtOnline(x, row) < 0)
        return "NOT DURING AN ONLINE GAME";
    return x->desc(row);
}

static void ExtValue(int row, char *buf, int size)
{
    const struct PortPocRows *x = sModel->ext;
    int o = ExtOnline(x, row);

    buf[0] = '\0';
    if (!PocShared()) {
        x->value(row, buf, size);
        return;
    }
    if (o >= 0 && o < 4 && x->onlineText && (sSlotSetOn & (1u << Pauser())))
        x->onlineText(o, (sSlotExt[Pauser()] >> (4 * o)) & 15, buf, size);
}

static const char *ExtName(int row) { return sModel->ext->name(row); }

/* --- LIGHTING (platform/ui/light_opts.h) -------------------------------------
 * Offline: this device's settings and GPU, every row.  In a session the
 * section is drawn from the pausing player's device's copy -- its four
 * online rows (LIGHTING, ENGINE, STYLE, LIGHT SOURCE: rows 0-3, online slots
 * 0-3) carried as 4-bit values with the settings copy -- and as if every
 * device could light (LIGHT_OPT_HOST_ALL), so nothing drawn depends on this
 * device's GPU; the other rows are dimmed.  An edit is that device's own
 * LightOptStepLive, from its final frame. */
#define LIGHT_ONLINE_ROWS 4

static const PortSettings *LightSettings(int *host)
{
    static PortSettings copy;
    const PortSettings *sh;
    u32 x;
    int st;

    if (!PocShared()) {
        *host = LightOptHost();
        return &gPortSettings;
    }
    *host = LIGHT_OPT_HOST_ALL;
    sh = Shown();
    if (sh != NULL) copy = *sh; else PortSettingsDefaults(&copy);
    x = sSlotExt[Pauser()];
    copy.lightOn = (int)(x & 1);
    copy.lightEngine = (int)((x >> 4) & 1);
    copy.lightMode = (int)((x >> 12) & 3);
    /* STYLE: the defaults are NORMAL; a step either side is SUBTLE or
     * STRONG; anything else draws as CUSTOM */
    st = (int)((x >> 8) & 15);
    if (st == 0 || st == 2)
        LightOptStep(&copy, 2, st == 0 ? -1 : 1);
    else if (st >= 3)
        copy.lightStrength = 1.37f;
    return &copy;
}

static const char *LightName(int row) { return LightOptName(row); }
static const char *LightDesc(int row)
{
    int host;
    const PortSettings *s = LightSettings(&host);

    return LightOptDesc(s, row, host);
}
static void LightValue(int row, char *buf, int size)
{
    int host;

    LightOptValue(LightSettings(&host), row, buf, size);
}
static void LightStep(int row, int dir) { LightOptStepLive(row, dir); }
static int LightAvailable(int row)
{
    int host;
    const PortSettings *s = LightSettings(&host);

    return LightOptAvailable(s, row, host);
}
static int LightOnlineSlot(int row) { return row < LIGHT_ONLINE_ROWS ? row : -1; }
static int LightOnlineGet(int slot)
{
    char v[16];
    const PortSettings *s = &gPortSettings;

    switch (slot) {
    case 0: return s->lightOn & 1;
    case 1: return s->lightEngine & 1;
    case 2:
        LightOptValue(s, 2, v, sizeof v);
        return strcmp(v, "SUBTLE") == 0 ? 0 : strcmp(v, "NORMAL") == 0 ? 1 : strcmp(v, "STRONG") == 0 ? 2 : 3;
    default: return s->lightMode & 3;
    }
}
static void LightOnlineText(int slot, int v, char *buf, int size)
{
    (void)v;
    LightValue(slot, buf, size);    /* the copy holds v (LightSettings) */
}

const struct PortPocRows gPocLightRows = {
    "LIGHTING", LightName, LightDesc, LightValue, LightStep, LightAvailable,
    LightOnlineSlot, LightOnlineGet, LightOnlineText,
};

static const char *SecName(int row)
{
    int it = ModelItem(row);

    if (IS_SLOT_ITEM(it))
        return SlotLabel(it - IT_SLOT0);
    return kItem[it].name;
}
static void SecValue(int row, char *buf, int size) { ItemValue(ModelItem(row), buf, size); }
static const char *SecDesc(int row) { return ItemDesc(ModelItem(row)); }
static int SecUnavailable(int row) { return ItemWhyNot(ModelItem(row)) != NULL; }
static int SecStep(int row, int dir) { (void)row; (void)dir; return 0; }    /* PortPocScreenInput */

/* Rows follow, while the screen is up, what changes without a key: the
 * people playing and, in a session, the copies and the code (simulation
 * state); outside a session the page's own state (fullscreen, the touch
 * pads, the randomizer, stored files), which is this device's only. */
static u32 SecGen(void)
{
    u32 g = 0;
    int s;

    for (s = 0; s < POC_SLOTS; s++)
        g = g * 2u + (u32)(PortLmpSlotPerson(s) != 0);
#if KATAM_WIDE
    g = g * 31u + (u32)gNumKirbys;      /* the list's rows with a Kirby */
#endif
    if (gPortLnOn) {
        const u8 *c = PORT_SIM_LNSET;

        /* the overlay draws this device's own (an edit from it lands a
         * frame later: PortLnOverlayEdit) */
        if (!PocShared())
            g = g * 31u + PocPack(&gPortSettings) + PortPocSettingsExtWord() * 7u;
        for (s = 0; s < POC_SLOTS; s++)
            g = g * 31u + ((sSlotSetOn >> s) & 1u ? sSlotSet[s] + sSlotExt[s] * 7u : 0x5A5Au)
                + (u32)(PortLnSlotOwner(s) + 1);
        for (s = 5; s < 10; s++)
            g = g * 31u + c[s];
        for (s = 0; s < (int)sizeof sSlotName; s++)
            g = g * 31u + (u8)((const char *)sSlotName)[s];
        g = g * 31u + gPortSim->view.lmpRooms;
        return g;
    }
    if (Web() && (sModel->items == kDisplayItems || sModel->items == kPlayersItems
                  || sModel->items == kGameItems || sModel->items == kAdvancedItems)) {
        char b[64];

        g = g * 31u + (u32)HostOn("fullscreen") * 2u + (u32)HostOn("touch") * 4u + (u32)HostOn("diag") * 8u
            + (u32)HostOn("save.stored") * 16u + (u32)HostOn("rom.stored") * 32u;
        b[0] = '\0';
        PortHostUiQuery("rando", b, (int)sizeof b);
        for (s = 0; b[s]; s++)
            g = g * 31u + (u8)b[s];
    }
    return g;
}

#define SECTION(n, hdr, list, par, prow) \
    static const struct PocModel n = { (int)ARRAY_COUNT(list), hdr, SecName, SecValue, SecDesc, SecStep, \
                                       SecUnavailable, 0, SecGen, 0, 0, list, par, prow, NULL, 0 }
/* a section kept by another file: its rows (a constant count), the place of
 * its row on the top level, its index in kExtRows */
#define EXT_SECTION(n, rowsDef, nrows, par, prow, idx) \
    static const struct PocModel n = { nrows, NULL, ExtName, ExtValue, ExtDesc, SecStep, \
                                       ExtUnavailable, 0, SecGen, 0, 0, NULL, par, prow, rowsDef, idx }

/* the top level: the front end's six, the pause page's seven (MAIN MENU) */
/* (a section's parent row is its place on the top level; the parent itself
 * is whichever top level is up: SectionParent) */
SECTION(kSecDisplay, "DISPLAY", kDisplayItems, &kTopModel, 0);
SECTION(kSecSound, "SOUND", kSoundItems, &kTopModel, 1);
SECTION(kSecPlayers, "PLAYERS", kPlayersItems, &kTopModel, 2);
SECTION(kSecOnline, "ONLINE", kOnlineItems, &kTopModel, 3);
SECTION(kSecGame, "GAME", kGameItems, &kTopModel, 4);
SECTION(kSecAdvanced, "ADVANCED", kAdvancedItems, &kTopModel, 5);
SECTION(kSecExperimental, "EXPERIMENTAL", kExperimentalItems, &kTopModel, 6);
/* LIGHTING is a section inside EXPERIMENTAL */
#define LIGHT_ROWS 26               /* LightOptCount(): checked as it opens */
EXT_SECTION(kSecLighting, &gPocLightRows, LIGHT_ROWS, &kSecExperimental, EXP_ROW_LIGHTING, 0);

/* NO / YES */
static const char *ConfirmName(int row) { return row == 0 ? "NO" : "YES"; }
static void ConfirmValue(int row, char *buf, int size) { (void)row; buf[0] = '\0'; (void)size; }
static const char *ConfirmDesc(int row)
{
    static char q[40];

    if (sConfirmItem == IT_KICK) {
        const char *n = sSlotName[sConfirmArg % POC_SLOTS];
        char who[12];

        SlotWho(sConfirmArg % POC_SLOTS, who, sizeof who);
        snprintf(q, sizeof q, row == 0 ? "REMOVE %s?" : "%s CANNOT COME BACK", n[0] ? n : who);
        Drawable(q, 30);
        return q;
    }
    if (sConfirmItem == IT_KIRBYS_GO) {
        if (row == 0)
            snprintf(q, sizeof q, "RESTART WITH %d KIRBYS?", sConfirmArg);
        else
            snprintf(q, sizeof q, "FROM LAST SAVE - 1 PLAYER");
        return q;
    }
    if (row == 0)
        return "B OR NO GOES BACK";
    switch (sConfirmItem) {
    case IT_MAINMENU:
        if (!gPortLnOn)
            return "TO THE TITLE - SAVE KEPT";
        return PauserIsHost() ? "ENDS THE GAME FOR EVERYONE" : "THE CPU TAKES YOUR KIRBY";
    case IT_DELSAVE:   return "THE STORED SAVE IS ERASED";
    case IT_ROMFORGET: return "PICK THE ROM AGAIN LATER";
    case IT_RESET:     return "RESTARTS - THE SAVE IS KEPT";
    case IT_DEFAULTS:  return "ALL SETTINGS TO DEFAULT";
    default:           return "";
    }
}
static int ConfirmUnavailable(int row) { (void)row; return 0; }
static const struct PocModel kConfirmModel = {
    2, "", ConfirmName, ConfirmValue, ConfirmDesc, SecStep, ConfirmUnavailable, 0, NULL, 0, 0,
    kConfirmItems, &kTopModel, 0, NULL, 0,
};

static const char *ConfirmHeader(void)
{
    switch (sConfirmItem) {
    case IT_MAINMENU:
        if (!gPortLnOn)
            return "MAIN MENU";
        return PauserIsHost() ? "END GAME" : "LEAVE GAME";
    default:
        return kItem[sConfirmItem].name;
    }
}

/* Each top-level section row and its screen (port/menu_poc.h "Adding a
 * section"). */
static const struct { u8 item; const struct PocModel *m; } kSectionOf[] = {
    { IT_SEC_DISPLAY, &kSecDisplay }, { IT_SEC_LIGHTING, &kSecLighting },
    { IT_SEC_SOUND, &kSecSound }, { IT_SEC_PLAYERS, &kSecPlayers },
    { IT_SEC_ONLINE, &kSecOnline }, { IT_SEC_GAME, &kSecGame }, { IT_SEC_ADVANCED, &kSecAdvanced },
    { IT_SEC_EXPERIMENTAL, &kSecExperimental },
};
static const struct PocModel *const kSections[] = {
    &kSecDisplay, &kSecSound, &kSecPlayers, &kSecOnline, &kSecGame, &kSecAdvanced, &kSecLighting,
    &kSecExperimental,
};

static const struct PocModel *SectionModel(int it)
{
    int i;

    for (i = 0; i < (int)ARRAY_COUNT(kSectionOf); i++)
        if (kSectionOf[i].item == it)
            return kSectionOf[i].m;
    return NULL;
}

/* The section holding item `it`, and its row there (the top level's rows
 * for the top-level items). */
static const struct PocModel *ItemHome(int it, int *row)
{
    int i, r;

    for (i = 0; i < (int)ARRAY_COUNT(kSections); i++)
        for (r = 0; kSections[i]->items && r < kSections[i]->rows; r++)
            if (kSections[i]->items[r] == it) {
                *row = r;
                return kSections[i];
            }
    for (r = 0; r < (int)ARRAY_COUNT(kTopItems); r++)
        if (kTopItems[r] == it)
            *row = r;
    return NULL;            /* the top level */
}

/* The top level up now or last: the pause page's when it is the pause page. */
static int sPauseScreen;

/* SETTINGS > PLAYERS > PLAYER CARD: whose card it opens.  The front end's
 * SETTINGS: card 0 (its PLAYER row picks P1..P4, as MULTIPLAYER's does).
 * The pause page: the pausing player's own -- locally slot s wears card s
 * (each seat edits its own, as on the PLAYERS page); on this device's pause
 * overlay online, the card of the seat that paused (a device's seat s wears
 * card s).  -1: none (a shared pause in a session, a Kirby 5.. locally). */
static int CardOfPauser(void)
{
    if (gPortLnOn)
        return PocShared() ? -1 : PortLnOverlaySeat();
    if (!sPauseScreen)
        return 0;
    return Pauser() < PORT_CARD_PLAYERS ? Pauser() : -1;
}

static const struct PocModel *TopModel(void)
{
    return sPauseScreen ? &kTopPauseModel : &kTopModel;
}

static int IsTop(const struct PocModel *m)
{
    return m == &kTopModel || m == &kTopPauseModel;
}

/* PLAYERS (platform/players_ui.c): P1..P4 and their controllers. */
static void PuiValue(int row, char *buf, int size) { PortPuiRowValue(row, 0, buf, size); }
static int PuiStep(int row, int dir) { (void)row; (void)dir; return 0; }
static const struct PocModel kPlayersModel = {
    PORT_PUI_ROWS, "PLAYERS", PortPuiRowName, PuiValue, PortPuiRowDesc, PuiStep, PortPuiRowUnavailable,
    0, PortPuiGen, 1, 0, NULL, &kSecPlayers, 0,
};

/* PLAYERS during an online session: who plays each Kirby, read only.  It is
 * drawn into game memory, so only what is the same on every device at the
 * same frame: the session's slot table (PortLnSlotOwner -- applied with the
 * sealed frames, in every keyframe and image, hashed), never a name (names
 * are relayed, not sealed) and never "you".  The host is HOST, guests are
 * GUEST 1..3 by the number the host gave them; a second person on one
 * device has a +. */
static const char *LnPlName(int row)
{
    static const char *const n[16] = { "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8",
                                       "P9", "P10", "P11", "P12", "P13", "P14", "P15", "P16" };
    return n[(unsigned)row % POC_SLOTS];
}
static void LnPlValue(int row, char *buf, int size)
{
    int o = PortLnSlotOwner(row);

    if (row == 0 && o < 0)
        o = 0;                      /* slot 0 is always the host's own */
    if (o < 0)
        snprintf(buf, size, "CPU");
    else if (o >> 2 == 0)
        snprintf(buf, size, "HOST%s", (o & 3) ? " +" : "");
    else
        snprintf(buf, size, "GUEST %d%s", o >> 2, (o & 3) ? " +" : "");
}
static const char *LnPlDesc(int row) { (void)row; return "WHO PLAYS EACH KIRBY"; }
static int LnPlStep(int row, int dir) { (void)row; (void)dir; return 0; }
static int LnPlUnavailable(int row) { (void)row; return 0; }
static u32 LnPlGen(void)
{
    u32 g = 0;
    int s;

    for (s = 0; s < POC_SLOTS; s++)
        g = g * 64u + (u32)(PortLnSlotOwner(s) + 1);
    return g;
}
static const struct PocModel kLnPlayersModel = {
    POC_SLOTS, "PLAYERS", LnPlName, LnPlValue, LnPlDesc, LnPlStep, LnPlUnavailable, 0, LnPlGen, 0, 0, NULL,
    &kSecPlayers, 0,
};

/* MULTIPLAYER: LOCAL (two players on this screen, platform/localmp.c) and
 * ONLINE (two devices, platform/online_ui.c and web/online.js).  In an
 * online session, should the game ever come back here, nothing that depends
 * on the device: both rows dimmed, fixed text. */
static const char *MpName(int row) { return row == 0 ? "LOCAL" : row == 1 ? "ONLINE" : "PLAYER CARD"; }
static void MpValue(int row, char *buf, int size)
{
    if (row == PORT_MENU_POC_MP_CARD)   /* online: nothing device-dependent */
        snprintf(buf, size, "%s", gPortLnOn ? "" : PortCardShownName(0));
    else
        snprintf(buf, size, "%s", row == 0 ? "2 PLAYERS" : "INTERNET");
}
static const char *MpDesc(int row)
{
    if (gPortLnOn)
        return "IN AN ONLINE GAME";
    /* the wide module (the web page's default): online is its 16-player
     * games (platform/online_ui.c PortOuiSeats: 16) */
    if (KATAM_WIDE && row == PORT_MENU_POC_MP_ONLINE)
        return "PLAY WITH UP TO SIXTEEN ONLINE";
    if (row == PORT_MENU_POC_MP_CARD)
        return "YOUR NAME AND KIRBY COLOR";
    if (row != 0)
        return "PLAY WITH A FRIEND ONLINE";
    if (!PortLmpP2InputAvailable())
        return "NEEDS KEYS OR A GAMEPAD";
    /* alternate, about every two seconds: what it is, and player 2's keys */
    return (gPortSim->frame.frameCount / 120) & 1 ? "P2: A PAD OR ARROWS + NUMPAD"
                                                  : "TWO PLAYERS ON ONE SCREEN";
}
static int MpStep(int row, int dir) { (void)row; (void)dir; return 0; }
static int MpUnavailable(int row)
{
    if (gPortLnOn)
        return 1;
    return row == 0 && !PortLmpP2InputAvailable();
}

static const struct PocModel kMpModel = {
    3, "MULTIPLAYER", MpName, MpValue, MpDesc, MpStep, MpUnavailable,
};

/* ONLINE > HOST GAME / JOIN GAME, and the two screens behind them: the
 * MULTIPLAYER screen's two rows and tiles, text from platform/online_ui.c. */
static const char *OuiName(int row) { return PortOuiRowName(sModel->oui, row); }
static void OuiValue(int row, char *buf, int size) { PortOuiRowValue(sModel->oui, row, 0, buf, size); }
static const char *OuiDesc(int row) { return PortOuiRowDesc(sModel->oui, row); }
static int OuiUnavailable(int row) { return PortOuiRowUnavailable(sModel->oui, row); }

static const struct PocModel kOnlineModel = {
    4 /* server browser: + BROWSE GAMES; + the other module (HOST 16 PLAYERS) */, "ONLINE", OuiName, OuiValue, OuiDesc, MpStep, OuiUnavailable, PORT_OUI_SUB_ONLINE,
};
static const struct PocModel kHostModel = {
    4 /* server browser: + GAME, PASS */, "HOST GAME", OuiName, OuiValue, OuiDesc, MpStep, OuiUnavailable, PORT_OUI_SUB_HOST,
};
static const struct PocModel kJoinModel = {
    2, "JOIN GAME", OuiName, OuiValue, OuiDesc, MpStep, OuiUnavailable, PORT_OUI_SUB_JOIN,
};
/* server browser (platform/online_browse.c): the public list, as many rows
 * as it has games up to PORT_OBR_VIS (OuiFollow keeps `rows` in step), and
 * a locked game's PASSWORD screen */
static struct PocModel kBrowseModel = {
    1, "OPEN GAMES", OuiName, OuiValue, OuiDesc, MpStep, OuiUnavailable, PORT_OUI_SUB_BROWSE,
};
static const struct PocModel kPassModel = {
    2, "PASSWORD", OuiName, OuiValue, OuiDesc, MpStep, OuiUnavailable, PORT_OUI_SUB_PASS,
};

/* PLAYER CARD and its colour picker (platform/player_card.c), on the same
 * tiles, with the Kirby preview and the picker's bars beside them. */
static const char *CardName(int row) { return PortCardRowName(sModel->card, row); }
static void CardValue(int row, char *buf, int size) { PortCardRowValue(sModel->card, row, 0, buf, size); }
static const char *CardDesc(int row) { return PortCardRowDesc(sModel->card, row); }
static int CardUnavailable(int row) { (void)row; return 0; }

static const struct PocModel kCardModel = {
    PORT_CARD_ROWS, "PLAYER CARD", CardName, CardValue, CardDesc, MpStep, CardUnavailable, 0,
    NULL, 0, PORT_CARD_SUB_CARD,
};
static const struct PocModel kPickModel = {
    PORT_CARD_PICK_ROWS, "COLOR", CardName, CardValue, CardDesc, MpStep, CardUnavailable, 0,
    NULL, 0, PORT_CARD_SUB_PICK,
};

static const char *sDescShown;      /* what the screen's description holds */
static u32 sOuiGenShown;            /* PortOuiGen() the rows were drawn at */
static u32 sCardGenShown;           /* PortCardGen() the same              */

/* The models, numbered for the host state an online image carries. */
static const struct PocModel *const kModels[] = {
    &kTopModel, &kMpModel, &kOnlineModel, &kHostModel, &kJoinModel, &kPlayersModel,
    &kCardModel, &kPickModel, &kLnPlayersModel,
    /* 9.. the pause page's top level, the sections, NO / YES, PLAYING */
    &kTopPauseModel, &kSecDisplay, &kSecSound, &kSecPlayers, &kSecOnline, &kSecGame, &kSecAdvanced,
    &kConfirmModel,
    /* 17, 18: the server browser's */
    &kBrowseModel, &kPassModel,     /* server browser */
    &kSecLighting,                  /* 19 */
    &kSecExperimental,              /* 20 */
};

/* The art the screen up now was built from, for a sub-screen built in place
 * (PLAYERS from SETTINGS and back). */
static u16 sArtLbl, sArtHdr;
static u8 sArtLblVar, sArtHdrVar;

/* --- the screen itself, shared by the front end and the pause page ----------
 *
 * Everything below draws from `tiles` (SET_TILES of OBJ VRAM, wherever the
 * caller found them) and a row cursor the caller keeps in its own (game)
 * memory; the banner and header are decoded from the sprite variants the
 * caller names.  The two callers are the front end's settings screen (tiles
 * from VramMalloc, row in a1->unk3A6) and the pause menu's settings page
 * (game/src/pause_settings.c: fixed tiles, saved and put back around it). */

/* Rows on screen (and in sprite memory) for the model up now. */
static int VisRows(void)
{
    return sModel->rows < ROWS ? sModel->rows : ROWS;
}

/* The first row on screen while the cursor is on `row`: a function of the
 * cursor alone (the list scrolls just enough to show it), so neither caller
 * keeps any more state for it. */
static int TopRow(int row)
{
    int top = row - (ROWS - 1);

    if (top < 0 || sModel->rows <= ROWS)
        return 0;
    return top > sModel->rows - ROWS ? sModel->rows - ROWS : top;
}

/* Sprite memory of the row shown in screen slot `slot`. */
static u32 RowVram(u32 tiles, int slot)
{
    return tiles + slot * ROW_COLS * COL_TILES * 32;
}

/* The header and description follow the rows the screen up now has (the
 * MULTIPLAYER screen takes only the tiles its two rows need: a second visit
 * to it can find the VRAM heap too fragmented for the settings' 660). */
static u32 HeadVram(u32 tiles)
{
    return RowVram(tiles, VisRows());
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

/* Draw row `row` into its screen slot, if it is on screen with the cursor
 * on `cursor`. */
static void DrawRowTiles(u32 tiles, int row, int cursor)
{
    static u8 px[PIC_H * ROW_COLS * 32];
    int w = ROW_COLS * 32, top = TopRow(cursor), selected = row == cursor;
    char val[24], shown[32];

    if (row < top || row >= top + VisRows())
        return;
    memcpy(px, sRowBanner, sizeof(px));
    CellText(px, w, PIC_H, LBL_TEXT_LEFT, LBL_ROW0 + 1, sModel->name(row), LBL_FILL, LBL_EDGE);
    if (sModel->oui)
        PortOuiRowValue(sModel->oui, row, selected, val, sizeof(val));
    else if (sModel->pui)
        PortPuiRowValue(row, selected, val, sizeof(val));
    else if (sModel->card)
        PortCardRowValue(sModel->card, row, selected, val, sizeof(val));
    else
        sModel->value(row, val, sizeof(val));
    /* the selected row's value between arrows, where they fit (and where
     * Left/Right change it) */
    snprintf(shown, sizeof(shown), "<%s>", val);
    if (!selected || (sModel->ext == NULL && (sModel->items == NULL
                                               || (kItem[ModelItem(row)].kind != K_SETTING
                                                   && kItem[ModelItem(row)].kind != K_TOGGLE)))
        || sModel->unavailable(row) || !val[0]
        || LBL_TEXT_LEFT + CellWidth(sModel->name(row)) + 8 + CellWidth(shown) > ROW_VALUE_X1)
        snprintf(shown, sizeof(shown), "%s", val);
    CellText(px, w, PIC_H, ROW_VALUE_X1 - CellWidth(shown), LBL_ROW0 + 1, shown,
             LBL_FILL, LBL_EDGE);
    /* more rows below the last one on screen, or above the first: a small
     * triangle just past the banner's tip, in the margin (inside the banner
     * it read as part of the value: "AUTO^") */
    if (row == top + VisRows() - 1 && top + VisRows() < sModel->rows)
        CellText(px, w, PIC_H, ROW_W + 3, LBL_ROW0 + 1, "v", LBL_FILL, LBL_EDGE);
    else if (row == top && top > 0)
        CellText(px, w, PIC_H, ROW_W + 3, LBL_ROW0 + 1, "^", LBL_FILL, LBL_EDGE);
    PackPic(px, ROW_COLS, RowVram(tiles, row - top));
}

static void DrawRows(u32 tiles, int cursor)
{
    int r;

    for (r = 0; r < sModel->rows; r++)
        DrawRowTiles(tiles, r, cursor);
}

/* The SUB-GAMES header (its S icon suits SETTINGS) with "SETTINGS" in the
 * headers' own lettering. */
static void DrawHeadTiles(u32 tiles, u16 hdrAnim, u8 hdrVariant)
{
    const char *text = sModel == &kConfirmModel ? ConfirmHeader()
                     : sModel->ext != NULL ? sModel->ext->header : sModel->header;
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

/* --- the player card's extras: a Kirby in the card's colour, and the
 * picker's grid --------------------------------------------------------------
 *
 * In the MULTIPLAYER screen's block, after the most its three-row screens
 * use.  The Kirby is the standing frame (anim 0, variant 0) decoded from
 * the sprite tables like the banners are, drawn at twice its size; its
 * palette line is the card's derived palette (platform/kirby_paint.c), the
 * same bytes the game will load, rewritten every frame.  The PICK screen
 * puts the Kirby to the right and the presets' grid (8 x 4) to the left:
 * each cell an 8 x 8 object of one solid tile, the cell of the row's part
 * framed (white while the grid is open and the d-pad moves it, grey when
 * not), and a swatch of each part's colour at its row's end.  Four OBJ
 * palette lines the front end does not use here, kept and put back around
 * the card screens: 10 and 11 the first 30 presets, 12 the last two, the
 * swatches and the frames' colours, 13 the Kirby. */
#define MP_TILES         ((PORT_CARD_ROWS * ROW_COLS + HEAD_COLS + GS_DESC_COLS) * COL_TILES)
#define CARD_PREV_TILES  64             /* 64 x 64                           */
#define CARD_SOLID_TILES 15             /* tile k-1: every pixel colour k    */
#define CARD_FRAME_TILES 4              /* 16 x 16: a frame, two of them     */
#define CARD_EXTRA_TILES (CARD_PREV_TILES + CARD_SOLID_TILES + 2 * CARD_FRAME_TILES)
/* server browser: HOST GAME and the public list have four rows; the fourth
 * row's tiles fit in the card's extras, which those screens do not draw */
typedef char ObrRowsFit[(PORT_OBR_VIS * ROW_COLS + HEAD_COLS + GS_DESC_COLS) * COL_TILES
                        <= MP_TILES + CARD_EXTRA_TILES ? 1 : -1];
#define PAL_BAR0         10             /* 10, 11, 12: the grid, swatches    */
#define PAL_PREVIEW      13
#define CARD_PAL_LINES   4
#define CARD_PREV_X      (DISPLAY_WIDTH / 2 - 32)
#define CARD_PREV_Y      88
#define PICK_PREV_X      (DISPLAY_WIDTH / 2 + 26)
#define PICK_PREV_Y      70
#define GRID_X           (DISPLAY_WIDTH / 2 - 96)
#define GRID_Y           78
#define GRID_PITCH       11
#define SWATCH_X         (ROW_FRAME_X + ROW_VALUE_X1 + 4)
#define C_SWATCH0        3              /* line 12: 3 the body's, 4 the feet's */
#define C_FRAME_ON       14
#define C_FRAME_OFF      15

static s32 RowFrameY(int row);

static u16 sCardPalSave[CARD_PAL_LINES * 16];
static int sCardPalSaved;

static u32 CardExtraVram(u32 tiles)
{
    return tiles + MP_TILES * 32;
}

static void CardPalettesKeep(int on)
{
    if (on && !sCardPalSaved) {
        memcpy(sCardPalSave, &gObjPalette[PAL_BAR0 * 16], sizeof(sCardPalSave));
        sCardPalSaved = 1;
    } else if (!on && sCardPalSaved) {
        memcpy(&gObjPalette[PAL_BAR0 * 16], sCardPalSave, sizeof(sCardPalSave));
        sCardPalSaved = 0;
        gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
    }
}

/* Pack a w x h canvas (w, h multiples of 8) as one object's 1D tiles. */
static void PackObj(const u8 *px, int w, int h, u32 vram)
{
    u8 *dst = (u8 *)(uintptr_t)vram;
    int ty, tx, y, x;

    for (ty = 0; ty < h / 8; ty++)
        for (tx = 0; tx < w / 8; tx++)
            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x += 2) {
                    int sx = tx * 8 + x, sy = ty * 8 + y;

                    *dst++ = (u8)(px[sy * w + sx] | (px[sy * w + sx + 1] << 4));
                }
}

static void CardPreviewPack(u32 tiles)
{
    static u8 src[32 * 32];
    static u8 big[64 * 64];
    int fw, fh = 0, x, y, ox, oy;

    memset(big, 0, sizeof(big));
    fw = PortPocDecodeFrame(0, 0, src, 32, 32, &fh);
    if (fw > 0) {
        ox = (64 - 2 * fw) / 2;
        oy = 64 - 2 * fh;
        for (y = 0; y < 2 * fh; y++)
            for (x = 0; x < 2 * fw; x++)
                if (ox + x >= 0 && ox + x < 64 && oy + y >= 0)
                    big[(oy + y) * 64 + ox + x] = src[(y / 2) * 32 + x / 2];
    }
    PackObj(big, 64, 64, CardExtraVram(tiles));
}

static u32 CardSolidVram(u32 tiles, int k)      /* k = 1..15 */
{
    return CardExtraVram(tiles) + (CARD_PREV_TILES + k - 1) * 32;
}

static u32 CardFrameVram(u32 tiles, int on)
{
    return CardExtraVram(tiles) + (CARD_PREV_TILES + CARD_SOLID_TILES + (on ? 0 : CARD_FRAME_TILES)) * 32;
}

/* The grid's tiles: fifteen solid ones and the two frames (12 x 12 squares
 * in a 16 x 16 object, so a frame drawn 2 px up and left of a cell rings it
 * in the gap between cells). */
static void CardGridPack(u32 tiles)
{
    static u8 px[16 * 16];
    int k, on, i;

    for (k = 1; k <= CARD_SOLID_TILES; k++) {
        memset(px, k, 64);
        PackObj(px, 8, 8, CardSolidVram(tiles, k));
    }
    for (on = 0; on < 2; on++) {
        u8 c = (u8)(on ? C_FRAME_ON : C_FRAME_OFF);

        memset(px, 0, sizeof(px));
        for (i = 0; i < 12; i++)
            px[i] = px[11 * 16 + i] = px[i * 16] = px[i * 16 + 11] = c;
        PackObj(px, 16, 16, CardFrameVram(tiles, on));
    }
}

/* Every frame: the palettes (cheap), then the objects -- before the rows,
 * so the swatches are drawn over the rows' banners, and the frame before
 * the cells, so it is over them. */
static void CardExtrasDraw(u32 tiles, int slide, u8 layer, int row)
{
    u16 pal[16];
    int i, pick = sModel->card == PORT_CARD_SUB_PICK;

    if (pick) {
        u16 cols[PORT_PAINT_PRESETS];
        int open = 0, cell = PortCardGrid(row, cols, &open), r;

        for (i = 0; i < PORT_PAINT_PRESETS; i++)
            gObjPalette[(PAL_BAR0 + i / 15) * 16 + 1 + i % 15] = cols[i];
        for (r = 0; r < PORT_CARD_PICK_ROWS; r++)
            gObjPalette[(PAL_BAR0 + 2) * 16 + C_SWATCH0 + r] = PortCardPartColor(r);
        gObjPalette[(PAL_BAR0 + 2) * 16 + C_FRAME_ON] = 0x7FFF;
        gObjPalette[(PAL_BAR0 + 2) * 16 + C_FRAME_OFF] = 0x4E73;
        if (cell >= 0)
            PutObj(layer, GRID_X + (cell % PORT_PAINT_PRESET_COLS) * GRID_PITCH - 2 + slide,
                   GRID_Y + (cell / PORT_PAINT_PRESET_COLS) * GRID_PITCH - 2, 0, 1,
                   CardFrameVram(tiles, open), PAL_BAR0 + 2);
        for (i = 0; i < PORT_PAINT_PRESETS; i++)
            PutObj(layer, GRID_X + (i % PORT_PAINT_PRESET_COLS) * GRID_PITCH + slide,
                   GRID_Y + (i / PORT_PAINT_PRESET_COLS) * GRID_PITCH, 0, 0,
                   CardSolidVram(tiles, 1 + i % 15), PAL_BAR0 + i / 15);
        for (r = 0; r < PORT_CARD_PICK_ROWS; r++)
            PutObj(layer, SWATCH_X + slide, RowFrameY(r) + 5, 0, 0, CardSolidVram(tiles, C_SWATCH0 + r),
                   PAL_BAR0 + 2);
    }
    if (PortCardPreviewPalette(pal)) {
        for (i = 1; i < 16; i++)
            gObjPalette[PAL_PREVIEW * 16 + i] = pal[i];
        PutObj(layer, (pick ? PICK_PREV_X : CARD_PREV_X) + slide, pick ? PICK_PREV_Y : CARD_PREV_Y, 0, 3,
               CardExtraVram(tiles), PAL_PREVIEW);
    }
    gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
}

static void ModelBuild(const struct PocModel *m, u32 tiles, u16 lblAnim, u8 lblVariant,
                       u16 hdrAnim, u8 hdrVariant, int row)
{
    int r;

    /* the card screens' palette lines are lent, not taken */
    CardPalettesKeep(m->card != 0);
    if (m->card) {
        CardPreviewPack(tiles);
        CardGridPack(tiles);
        sCardGenShown = PortCardGen();
        PortCardNoteRow(row);
    }
    sModel = m;
    sArtLbl = lblAnim; sArtLblVar = lblVariant; sArtHdr = hdrAnim; sArtHdrVar = hdrVariant;
    sOuiGenShown = m->gen ? m->gen() : PortOuiGen();
    LoadSources();
    if (!BuildRowBanner(lblAnim, lblVariant))
        PortLog("[menu-poc] the ERASE A FILE label is not the expected shape; rows without banners");
    DrawRows(tiles, row);
    DrawHeadTiles(tiles, hdrAnim, hdrVariant);
    sDescShown = m->desc(row);
    BuildDesc(sDescShown, SetDescVram(tiles));
}

static void ScreenBuild(int pause, u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row)
{
    PortPuiClose();
    if (sModel != NULL && sModel->card && PortCardSub() != PORT_CARD_SUB_NONE)
        PortPlayerCardClose();      /* a card left open from PLAYERS */
    sPauseScreen = pause;
    if (LightOptCount() != LIGHT_ROWS)
        PortLog("[menu-poc] LIGHTING has %d rows, the section %d: update LIGHT_ROWS", LightOptCount(), LIGHT_ROWS);
    ModelBuild(TopModel(), tiles, lblAnim, lblVariant, hdrAnim, hdrVariant, row);
}

void PortPocScreenBuild(u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row)
{
    ScreenBuild(0, tiles, lblAnim, lblVariant, hdrAnim, hdrVariant, row);
}

void PortPocPauseScreenBuild(u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row)
{
    ScreenBuild(1, tiles, lblAnim, lblVariant, hdrAnim, hdrVariant, row);
}

/* Below SETTINGS' top level (a section, NO / YES, PLAYERS, a card): B is
 * the screen's own (back one level), not the caller's. */
int PortPocScreenSub(void)
{
    return sModel != NULL && !IsTop(sModel);
}

static s32 RowFrameY(int row)
{
    return ROW_Y0 + ROW_PITCH * row;
}

int PortPocScreenCursorY(int row)
{
    /* Game Select draws the star 11 px above a label sprite's y
     * (gUnk_0838650A), and a label's frame is drawn sLblOffY above that. */
    return RowFrameY(row - TopRow(row)) + sLblOffY - 11;
}

int PortPocScreenCursorX(void)
{
    /* ... and 16 px left of its x: our frames sit 16 px further left. */
    return ROW_FRAME_X + sLblOffX - 16;
}

/* Tests (the page's browser test): the frame the screen was last drawn on,
 * and which model it shows (index in kModels).  Host memory, never read by
 * the game. */
static u32 sLastDrawFrame;

u32 PortPocScreenLastDraw(void)
{
    return sLastDrawFrame;
}

int PortPocScreenModel(void)
{
    int i;

    for (i = 0; i < (int)ARRAY_COUNT(kModels); i++)
        if (sModel == kModels[i])
            return i;
    return -1;
}

void PortPocScreenDraw(u32 tiles, int row, int slide, u8 layer)
{
    int r;

    sLastDrawFrame = PortFrameNumber();
    /* the player card: its rows follow every edit (keys, letters the web
     * page types), and its Kirby and grid are drawn first (on top) */
    if (sModel->card && PortCardGen() != sCardGenShown) {
        sCardGenShown = PortCardGen();
        DrawRows(tiles, row);
    }
    if (sModel->card)
        CardExtrasDraw(tiles, slide, layer, row);

    /* the header slides in from the left as the game's (unkC) do */
    DrawPic(layer, -sHdrOffX - slide * 0x9C / 0xD0, -sHdrOffY, HeadVram(tiles), HEAD_COLS,
            PIC_H, PAL_HEADER);
    for (r = 0; r < VisRows(); r++)
        DrawPic(layer, ROW_FRAME_X + slide, RowFrameY(r), RowVram(tiles, r), ROW_COLS, PIC_H,
                row == TopRow(row) + r && !sModel->unavailable(row) ? PAL_LIT : PAL_DIM);
    /* the ONLINE screens' rows follow the network (platform/online_ui.c);
     * never in a session (PortOuiGen is not simulation state, and these
     * screens are not reachable then) */
    if (sModel->oui && !gPortLnOn && PortOuiGen() != sOuiGenShown) {
        sOuiGenShown = PortOuiGen();
        DrawRows(tiles, row);
    }
    /* the PLAYERS page follows the devices (host state, never online), the
     * settings' PLAYERS row the number of people (simulation state) */
    if (sModel->gen && sModel->gen() != sOuiGenShown) {
        sOuiGenShown = sModel->gen();
        DrawRows(tiles, row);
        if (sModel->pui || sModel->items) {
            sDescShown = sModel->desc(row);
            BuildDesc(sDescShown, SetDescVram(tiles));
        }
    }
    /* a description that changes by itself (MULTIPLAYER's LOCAL row) */
    if (sModel->desc(row) != sDescShown) {
        sDescShown = sModel->desc(row);
        BuildDesc(sDescShown, SetDescVram(tiles));
    }
    SyncDescPalette();
    DrawPic(layer, 0, DESC_Y, SetDescVram(tiles), GS_DESC_COLS, 16, PAL_DESC);
}

static int CardFromPlayers(int slot);
/* Where B on a card goes: the PLAYERS page (0), or SETTINGS > PLAYERS'
 * PLAYER CARD row (1). */
static int sCardFromSection;

/* SETTINGS -> PLAYERS and back, in place, with the same tiles (and PLAYERS
 * -> a player's card and back). */
static void SwapModel(u32 tiles, const struct PocModel *m, int row)
{
    if (m == &kPlayersModel) {
        PortPuiSetCardHook(CardFromPlayers);
        PortPuiOpen();
    } else
        PortPuiClose();
    ModelBuild(m, tiles, sArtLbl, sArtLblVar, sArtHdr, sArtHdrVar, row);
}

/* The PLAYERS page's A on a player: that player's card, in place (local
 * play: slot p is card p).  Never online -- the page cannot open then, and
 * a card is device state that would be drawn into game memory. */
static int CardFromPlayers(int slot)
{
    if (gPortLnOn || slot < 0 || slot >= PORT_CARD_PLAYERS)
        return 0;
    PortPlayerCardOpen(slot, 1);
    sCardFromSection = 0;
    return 1;
}

/* The PLAYERS section's row of item `it`. */
static int PlayersRowOf(int it)
{
    int r;

    for (r = 0; r < (int)ARRAY_COUNT(kPlayersItems); r++)
        if (kPlayersItems[r] == it)
            return r;
    return 0;
}

/* A key on a card screen opened from PLAYERS (the shared screen: Game
 * Select's SETTINGS and the pause page). */
static int CardScreenInput(u32 tiles, u8 *row, unsigned key)
{
    int r = *row, act = PortCardKey(&r, key);

    switch (act) {
    case PORT_CARD_K_NONE:
    case PORT_CARD_K_QUIET:
        return key == 2 ? PORT_POC_IN_REFUSED : PORT_POC_IN_NONE;
    case PORT_CARD_K_CLOSE:     /* back to PLAYERS, on that player's row */
        if (sCardFromSection) { /* ... or to the section's PLAYER CARD row */
            sCardFromSection = 0;
            *row = (u8)PlayersRowOf(IT_CARD);
            SwapModel(tiles, &kSecPlayers, *row);
            return PORT_POC_IN_SCREEN;
        }
        *row = (u8)PortCardPlayer();
        SwapModel(tiles, &kPlayersModel, *row);
        return PORT_POC_IN_SCREEN;
    default:
        break;
    }
    *row = (u8)r;
    PortCardNoteRow(r);
    if ((PortCardSub() == PORT_CARD_SUB_PICK) != (sModel == &kPickModel)) {
        SwapModel(tiles, PortCardSub() == PORT_CARD_SUB_PICK ? &kPickModel : &kCardModel, r);
        return PORT_POC_IN_SCREEN;
    }
    sCardGenShown = PortCardGen();
    DrawRows(tiles, r);
    sDescShown = sModel->desc(r);
    BuildDesc(sDescShown, SetDescVram(tiles));
    return act == PORT_CARD_K_CURSOR ? PORT_POC_IN_CURSOR : PORT_POC_IN_VALUE;
}

/* B on a screen below SETTINGS' top level: back one level, onto the row
 * that opened it. */
static int ScreenBack(u32 tiles, u8 *row)
{
    const struct PocModel *to = sModel->parent;
    int r = sModel->parentRow;

    if (sModel == &kConfirmModel && sConfirmItem == IT_KIRBYS_GO) {
        to = &kSecExperimental;
        r = sConfirmRow;
    } else if (sModel == &kConfirmModel) {
        to = ItemHome(sConfirmItem, &r);
        if (sConfirmItem == IT_KICK) {
            to = &kSecOnline;
            for (r = 0; r < POC_SLOTS && RowSlot(r) != sConfirmArg; r++)
                ;
            r = 2 + (r % POC_SLOTS);
        }
    }
    if (to == NULL || IsTop(to))
        to = TopModel();
    *row = (u8)r;
    SwapModel(tiles, to, *row);
    return PORT_POC_IN_SCREEN;
}


/* A, Left or Right on a settings item. */
static int ItemKey(u32 tiles, u8 *row, unsigned key)
{
    int was = *row, it = ModelItem(was), dir = key == 5 ? -1 : 1;

    if (ItemWhyNot(it) != NULL)
        return key == 1 ? PORT_POC_IN_REFUSED : PORT_POC_IN_NONE;
    if (it == IT_KIRBYS || it == IT_ADD4 || it == IT_REMOVE4) {
        int n;

        if (it != IT_KIRBYS && key != 1)
            return PORT_POC_IN_NONE;
        n = KirbysNext(it, dir);
        if (n == gPortSettings.kirbys)
            return key == 1 ? PORT_POC_IN_REFUSED : PORT_POC_IN_NONE;
        if (KirbysRestart(n)) {             /* the page reloads: ask first */
            sConfirmItem = IT_KIRBYS_GO;
            sConfirmArg = (u8)n;
            sConfirmRow = (u8)was;
            *row = 0;                       /* on NO */
            SwapModel(tiles, &kConfirmModel, 0);
            return PORT_POC_IN_SCREEN;
        }
        ItemDo(it, dir);
        DrawRows(tiles, was);
        sDescShown = sModel->desc(was);
        BuildDesc(sDescShown, SetDescVram(tiles));
        return PORT_POC_IN_VALUE;
    }
    switch (kItem[it].kind) {
    case K_SECTION:
        if (key != 1)
            return PORT_POC_IN_NONE;
        *row = 0;
        SwapModel(tiles, SectionModel(it) ? SectionModel(it) : TopModel(), 0);
        return PORT_POC_IN_SCREEN;
    case K_OPEN:
        if (key != 1)
            return PORT_POC_IN_NONE;
        *row = 0;
        /* in an online session: who plays each Kirby (the same on every
         * device); otherwise the controllers */
        SwapModel(tiles, gPortLnOn ? &kLnPlayersModel : &kPlayersModel, 0);
        return PORT_POC_IN_SCREEN;
    case K_CARD: {
        /* this player's own card, in place (on the pause page and online the
         * PLAYER row is fixed: a player edits only their own) */
        int c = CardOfPauser();

        if (key != 1)
            return PORT_POC_IN_NONE;
        if (c < 0)
            return PORT_POC_IN_REFUSED;
        PortPlayerCardOpen(c, sPauseScreen || gPortLnOn);
        sCardFromSection = 1;
        *row = 1;               /* NAME */
        SwapModel(tiles, &kCardModel, *row);
        return PORT_POC_IN_SCREEN;
    }
    case K_PLAYER: {
        int slot = RowSlot(it - IT_SLOT0);

        /* the host's players remove a guest's device (never the host's own:
         * that is END GAME); everyone else only sees the list */
        if (key != 1)
            return PORT_POC_IN_NONE;
        if (!PauserIsHost() || SlotDevice(slot) < 1)
            return PORT_POC_IN_REFUSED;
        sConfirmItem = IT_KICK;
        sConfirmArg = (u8)slot;
        *row = 0;
        SwapModel(tiles, &kConfirmModel, 0);
        return PORT_POC_IN_SCREEN;
    }
    case K_CONFIRM:
        if (key != 1)
            return PORT_POC_IN_NONE;
        sConfirmItem = (u8)it;
        *row = 0;                   /* on NO */
        SwapModel(tiles, &kConfirmModel, 0);
        return PORT_POC_IN_SCREEN;
    case K_ACTION:
        if (key != 1)
            return PORT_POC_IN_NONE;
        ItemDo(it, 1);
        DrawRowTiles(tiles, was, was);
        return PORT_POC_IN_DONE;
    case K_SETTING:
    case K_TOGGLE:
        ItemDo(it, dir);
        PortLnNoteSetting(was);     /* a setting changed (online: R9) */
        DrawRows(tiles, was);
        sDescShown = sModel->desc(was);
        BuildDesc(sDescShown, SetDescVram(tiles));
        return PORT_POC_IN_VALUE;
    default:
        return key == 1 ? PORT_POC_IN_REFUSED : PORT_POC_IN_NONE;
    }
}

int PortPocScreenInput(u32 tiles, u8 *row, unsigned key)
{
    int was = *row, n = sModel->rows;

    if (sModel->card)
        return CardScreenInput(tiles, row, key);
    if (key == 2 && !IsTop(sModel) && (sModel->parent != NULL || sModel == &kConfirmModel))
        return ScreenBack(tiles, row);          /* B: back one level */
    if (sModel == &kConfirmModel && key == 1) {
        int item = sConfirmItem;

        if (was == 1)
            ItemDo(item, item == IT_KICK || item == IT_KIRBYS_GO ? sConfirmArg : 1);   /* YES */
        ScreenBack(tiles, row);
        return was == 1 ? PORT_POC_IN_DONE : PORT_POC_IN_SCREEN;
    }
    if (sModel->pui) {
        int act = key == 1 || key == 5 || key == 6 ? PortPuiKey(was, key) : PORT_OUI_K_NONE;

        if (PortCardSub() != PORT_CARD_SUB_NONE) {     /* A opened the card */
            *row = 1;           /* NAME: whose card it is is fixed */
            SwapModel(tiles, &kCardModel, *row);
            return PORT_POC_IN_SCREEN;
        }
        if (act != PORT_OUI_K_NONE) {
            sOuiGenShown = sModel->gen();
            DrawRowTiles(tiles, was, was);
            sDescShown = sModel->desc(was);
            BuildDesc(sDescShown, SetDescVram(tiles));
            return act == PORT_OUI_K_SELECT ? 4 : act == PORT_OUI_K_DENY ? 5 : 2;
        }
        if (key == 3 || key == 4)
            PortPuiLeaveRow(was);
    } else if (sModel->items != NULL && sModel != &kConfirmModel && (key == 1 || key == 5 || key == 6)) {
        return ItemKey(tiles, row, key);
    } else if (sModel->ext != NULL && (key == 1 || key == 5 || key == 6)) {
        if (sModel->unavailable(was))
            return key == 1 ? PORT_POC_IN_REFUSED : PORT_POC_IN_NONE;
        ItemDo(EXT_ITEM(sModel->extIndex, was), key == 5 ? -1 : 1);
        PortLnNoteSetting(was);
        DrawRows(tiles, was);
        sDescShown = sModel->desc(was);
        BuildDesc(sDescShown, SetDescVram(tiles));
        return PORT_POC_IN_VALUE;
    }

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
        DrawRowTiles(tiles, was, was);
        return 2;
    case 5:     /* left */
        if (!sModel->step(was, -1))
            return 0;
        DrawRowTiles(tiles, was, was);
        return 2;
    default:
        return 0;
    }
    if (TopRow(was) != TopRow(*row)) {
        DrawRows(tiles, *row);
    } else {
        DrawRowTiles(tiles, was, *row);
        DrawRowTiles(tiles, *row, *row);
    }
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
    v = VramMalloc(MP_TILES + CARD_EXTRA_TILES);
    if (!VramOk(v))
        return 0;
    a1->unk3A8.tilesVram = v;
    sOwner = a1;
    PortOuiReset();
    if (PortCardSub() != PORT_CARD_SUB_NONE)
        PortPlayerCardClose();
    {
        char code[4];

        /* an invite link: the visitor's card first, on JOIN */
        if (PortOuiInviteCard(code)) {
            PortPlayerCardOpenInvite(code);
            ModelBuild(&kCardModel, v, a1->unk2DC[3].animId, a1->unk2DC[3].variant,
                       a1->unkC.animId, a1->unkC.variant + 1, 0);
            PortMenuPocBlankBanner();
            return 1;
        }
    }
    ModelBuild(&kMpModel, v, a1->unk2DC[3].animId, a1->unk2DC[3].variant,
               a1->unkC.animId, a1->unkC.variant + 1, 0);
    PortMenuPocBlankBanner();
    return 1;
}

/* A key on the MULTIPLAYER screen, for the ONLINE screens behind it
 * (platform/online_ui.c): PORT_OUI_K_NONE leaves it to the screen's own
 * handling.  A change of sub-screen rebuilds the screen in place, with the
 * same two rows' tiles. */
/* Which of the MULTIPLAYER screen's models is up: a card screen, an ONLINE
 * screen, or the MULTIPLAYER choice itself. */
static const struct PocModel *MpScreenModel(void)
{
    static const struct PocModel *const kSub[] = { &kMpModel, &kOnlineModel, &kHostModel, &kJoinModel };

    if (PortCardSub() == PORT_CARD_SUB_CARD)
        return &kCardModel;
    if (PortCardSub() == PORT_CARD_SUB_PICK)
        return &kPickModel;
    if (PortOuiSub() == PORT_OUI_SUB_BROWSE)    /* server browser */
        return &kBrowseModel;
    if (PortOuiSub() == PORT_OUI_SUB_PASS)
        return &kPassModel;
    return kSub[PortOuiSub() & 3];
}

/* server browser: the ONLINE screens follow the page between keys too -- a
 * locked game's PASSWORD screen opens by itself, and the public list grows,
 * shrinks and keeps its cursor on the game it was on as it refreshes. */
static void OuiFollow(struct Unk_08138D64 *a1)
{
    const struct PocModel *m;
    int row, rows;

    if (!sModel->oui || gPortLnOn || PortCardSub() != PORT_CARD_SUB_NONE)
        return;
    m = MpScreenModel();
    row = PortObrFollowRow(PortOuiSub(), a1->unk3A6);
    rows = PortObrRows();
    if (m == sModel && row == a1->unk3A6 && (m != &kBrowseModel || rows == kBrowseModel.rows))
        return;
    a1->unk3A6 = (u8)row;
    if (m != sModel || (m == &kBrowseModel && rows != kBrowseModel.rows)) {
        kBrowseModel.rows = rows;
        ModelBuild(m, a1->unk3A8.tilesVram, a1->unk2DC[3].animId, a1->unk2DC[3].variant,
                   a1->unkC.animId, a1->unkC.variant + 1, row);
    } else {
        sOuiGenShown = PortOuiGen();
        DrawRows(a1->unk3A8.tilesVram, row);
        sDescShown = sModel->desc(row);
        BuildDesc(sDescShown, SetDescVram(a1->unk3A8.tilesVram));
    }
    a1->unk28C.y = PortMenuPocSettingsCursorY(a1);
}

int PortMenuPocMpKey(struct Unk_08138D64 *a1, unsigned key)
{
    int row = a1->unk3A6, act, r;

    if (!PortMenuPoc() || sOwner != a1 || !a1->unk3A8.tilesVram || gPortLnOn)
        return PORT_OUI_K_NONE;
    if (PortCardSub() != PORT_CARD_SUB_NONE) {
        /* PLAYER CARD: every key is the card's (platform/player_card.c) */
        switch (PortCardKey(&row, key)) {
        case PORT_CARD_K_CURSOR: act = PORT_OUI_K_CURSOR; break;
        case PORT_CARD_K_SELECT: act = PORT_OUI_K_SELECT; break;
        case PORT_CARD_K_BACK:   act = PORT_OUI_K_BACK; break;
        case PORT_CARD_K_CLOSE:
            act = PORT_OUI_K_BACK;
            row = PORT_MENU_POC_MP_CARD;
            if (PortOuiSub() == PORT_OUI_SUB_ONLINE)
                PortOuiInviteCancel(&row);      /* an invite's: ONLINE */
            break;
        case PORT_CARD_K_JOIN:   /* an invite's: JOIN GAME joins its code */
            act = PORT_OUI_K_SELECT;
            PortOuiInviteGo(&row);
            break;
        default:                 act = PORT_OUI_K_QUIET; break;
        }
    } else if (PortOuiSub() == PORT_OUI_SUB_MP && key == 1 && row == PORT_MENU_POC_MP_CARD) {
        PortPlayerCardOpen(0, 0);
        row = 1;                /* on NAME: the PLAYER row is for P2..P4 */
        act = PORT_OUI_K_SELECT;
    } else {
        act = PortOuiKey(&row, key);
    }
    if (act == PORT_OUI_K_NONE)
        return act;
    a1->unk3A6 = (u8)row;
    PortCardNoteRow(row);
    if (MpScreenModel() != sModel) {
        ModelBuild(MpScreenModel(), a1->unk3A8.tilesVram, a1->unk2DC[3].animId, a1->unk2DC[3].variant,
                   a1->unkC.animId, a1->unkC.variant + 1, row);
    } else {
        sOuiGenShown = PortOuiGen();
        (void)r;
        sCardGenShown = PortCardGen();
        DrawRows(a1->unk3A8.tilesVram, row);
        sDescShown = sModel->desc(row);
        BuildDesc(sDescShown, SetDescVram(a1->unk3A8.tilesVram));
    }
    return act;
}

int PortMenuPocMpBusy(void)
{
    return PortOuiBusy() || PortCardSub() != PORT_CARD_SUB_NONE;
}

void PortMenuPocMpStarted(void)
{
    PortOuiFadeDone();
}

/* Is row `row` of the screen up now available (A does something)?  Read
 * only (also the page's browser tests). */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortMenuPocRowAvailable(int row)
{
    return !sModel->unavailable(row);
}

void PortMenuPocSettingsClose(struct Unk_08138D64 *a1)
{
    PortPuiClose();
    if (sModel->card && PortCardSub() != PORT_CARD_SUB_NONE)
        PortPlayerCardClose();
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
    OuiFollow(a1);                  /* server browser */
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
    PortPuiClose();
    if (sModel != NULL && sModel->card) {   /* the page closed on a player's card */
        if (PortCardSub() != PORT_CARD_SUB_NONE)
            PortPlayerCardClose();
        CardPalettesKeep(0);
        sModel = &kTopPauseModel;
    }
    if (!sObjSaved)
        return;
    memcpy((void *)(uintptr_t)PORT_POC_PAUSE_OBJ_VRAM, sObjSave, sizeof(sObjSave));
    sObjSaved = 0;
}

/* --- the pause help page's line about the online game ----------------------------
 *
 * In a session the first pause page (the game's help page, pause_help.c)
 * says which game this is: its code, the mode and how many play, in a small
 * line along the top of the paper, above the page's frame.  Session state
 * only (SessionCode, the rooms flag, the people), so every device draws the
 * same bytes; the tiles are fixed OBJ VRAM the help page does not use,
 * kept and put back around the page like the settings page's.  Off a
 * session, or with the menus off, nothing. */
#define HELP_VRAM   (PORT_POC_PAUSE_OBJ_VRAM + 0x4000u)
#define HELP_COLS   (PORT_POC_HELP_BYTES / (4 * 32))     /* 32 x 8 objects */
#define PAL_HELP    13
static u8 sHelpSave[PORT_POC_HELP_BYTES];
static int sHelpSaved;

/* s in a font of 1-pixel cells (the fallback font's glyphs at half size)
 * with a 1-pixel outline; returns the width. */
static int SmallText(u8 *px, int w, int h, int x, int y, const char *s, u8 fill, u8 edge)
{
    int pass, x0 = x;

    for (pass = 0; pass < 2; pass++) {
        const char *p;

        x = x0;
        for (p = s; *p; p++) {
            const struct CellGlyph *g = CellFind(*p);
            int gw, r, c, dx, dy;

            if (*p == ' ') { x += 3; continue; }
            if (g == NULL) continue;
            gw = (int)strlen(g->rows[0]);
            for (r = 0; r < 5; r++)
                for (c = 0; c < gw && g->rows[r][c]; c++) {
                    if (g->rows[r][c] != '#')
                        continue;
                    for (dy = pass ? 0 : -1; dy <= (pass ? 0 : 1); dy++)
                        for (dx = pass ? 0 : -1; dx <= (pass ? 0 : 1); dx++) {
                            int qx = x + c + dx, qy = y + r + dy;

                            if (px != NULL && qx >= 0 && qx < w && qy >= 0 && qy < h)
                                px[qy * w + qx] = pass ? fill : edge;
                        }
                }
            x += gw + 2;
        }
    }
    return x - x0;
}

static int HelpLineText(char *buf, int size)
{
    const char *code = SessionCode();
    int s, n = 0;

    if (!PortMenuPoc() || code == NULL)
        return 0;
    for (s = 0; s < POC_SLOTS; s++)
        n += PortLmpSlotPerson(s) != 0;
    snprintf(buf, size, "GAME %s - %s - %d PLAYER%s", code,
             gPortSim->view.lmpRooms ? "FREE ROAM" : "TOGETHER", n, n == 1 ? "" : "S");
    return 1;
}

void PortPocHelpLineOpen(void)
{
    char t[48];

    if (sHelpSaved || !HelpLineText(t, sizeof t))
        return;
    memcpy(sHelpSave, (const void *)(uintptr_t)HELP_VRAM, sizeof sHelpSave);
    sHelpSaved = 1;
    /* dark blue on a white edge, over the paper */
    gObjPalette[PAL_HELP * 16 + 1] = (u16)((20 << 10) | (6 << 5) | 4);
    gObjPalette[PAL_HELP * 16 + 2] = 0x7FFF;
    gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
}

void PortPocHelpLineDraw(void)
{
    static u8 px[8 * HELP_COLS * 32];
    char t[48];
    int w = HELP_COLS * 32, tw, c;

    if (!sHelpSaved || !HelpLineText(t, sizeof t))
        return;
    memset(px, 0, sizeof px);
    tw = SmallText(NULL, w, 8, 0, 1, t, 1, 2);
    SmallText(px, w, 8, w - tw, 1, t, 1, 2);
    PackObj(px, w, 8, HELP_VRAM);       /* a w x 8 strip: one tile row */
    for (c = 0; c < HELP_COLS; c++)
        PutObj(0x10, DISPLAY_WIDTH - 2 - w + c * 32, 0, 1, 1, HELP_VRAM + c * 4 * 32, PAL_HELP);
}

/* Tests: is the help page's line up; the settings word the page shows (the
 * pausing player's device's, in a session; 0 if none yet). */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortPocHelpLineShown(void) { return sHelpSaved; }

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
u32 PortPocShownWord(void)
{
    const PortSettings *s = Shown();

    return s ? PocPack(s) : 0;
}

void PortPocHelpLineClose(void)
{
    if (!sHelpSaved)
        return;
    memcpy((void *)(uintptr_t)HELP_VRAM, sHelpSave, sizeof sHelpSave);
    sHelpSaved = 0;
}

/* --- online: this file's host state, as simulation state ------------------------
 *
 * The pause page keeps simulation-reaching state in host memory: the OBJ tiles
 * it saved (put back into VRAM when it closes), whether it saved them, the
 * model, the description shown, the row banner and its geometry.  An online
 * session (platform/lmpnet.c) rewinds and replays frames, and hands a joining
 * device the host's memory, so it carries these with every keyframe and image
 * and hashes them.  The decoded pictures (LoadSources) are a pure function of
 * the ROM and are rebuilt instead of copied.  Same build only (pointers). */
struct PocHostState {
    u32 objSaved;
    u32 model;                      /* index in kModels                   */
    u64 descShown, owner, gsWant, gsShown;
    s16 lblOffX, lblOffY, hdrOffX, hdrOffY;
    s32 descLine;
    u32 genShown;                   /* sOuiGenShown                       */
    /* (POC_SLOTS: four, so the default build's layout is the old one; the
     * wide build's is its own -- the state travels between devices of one
     * build only) */
    u32 slotSet[POC_SLOTS];         /* online: each slot's settings copy  */
    u16 slotExt[POC_SLOTS];
    PocSlotMask slotSetOn;
    u8  confirmItem, pauseScreen, helpSaved, confirmArg, confirmRow;
    char slotName[POC_SLOTS][PORT_CARD_NAME_MAX + 1];
    u8  helpSave[PORT_POC_HELP_BYTES];  /* under the pause help page's line */
    u8  objSave[PORT_POC_PAUSE_OBJ_BYTES];
    u8  rowBanner[sizeof(sRowBanner)];
};

u32 PortPocHostStateSize(void)
{
    return (u32)((sizeof(struct PocHostState) + 3u) & ~3u);
}

void PortPocHostStateSave(u8 *dest)
{
    struct PocHostState *h = (struct PocHostState *)dest;

    memset(dest, 0, PortPocHostStateSize());
    h->objSaved = (u32)sObjSaved;
    {
        u32 i;

        for (i = 0; i < ARRAY_COUNT(kModels); i++)
            if (sModel == kModels[i])
                h->model = i;
    }
    h->descShown = (u64)(uintptr_t)sDescShown;
    h->owner = (u64)(uintptr_t)sOwner;
    h->gsWant = (u64)(uintptr_t)sGsDescWant;
    h->gsShown = (u64)(uintptr_t)sGsDescShown;
    h->lblOffX = sLblOffX; h->lblOffY = sLblOffY; h->hdrOffX = sHdrOffX; h->hdrOffY = sHdrOffY;
    h->descLine = sDescLine;
    h->genShown = sOuiGenShown;
    memcpy(h->slotSet, sSlotSet, sizeof sSlotSet);
    memcpy(h->slotExt, sSlotExt, sizeof sSlotExt);
    h->slotSetOn = sSlotSetOn;
    h->confirmItem = sConfirmItem;
    h->confirmArg = sConfirmArg;
    h->confirmRow = sConfirmRow;
    memcpy(h->slotName, sSlotName, sizeof sSlotName);
    h->pauseScreen = (u8)sPauseScreen;
    h->helpSaved = (u8)sHelpSaved;
    memcpy(h->helpSave, sHelpSave, sizeof sHelpSave);
    memcpy(h->objSave, sObjSave, sizeof(sObjSave));
    memcpy(h->rowBanner, sRowBanner, sizeof(sRowBanner));
}

static void HostStateLoad(const u8 *src, int copies);

void PortPocHostStateLoad(const u8 *src)
{
    HostStateLoad(src, 1);
}

void PortPocHostStateLoadOwn(const u8 *src)
{
    HostStateLoad(src, 0);
}

static void HostStateLoad(const u8 *src, int copies)
{
    const struct PocHostState *h = (const struct PocHostState *)src;

    sObjSaved = (int)h->objSaved;
    sModel = h->model < ARRAY_COUNT(kModels) ? kModels[h->model] : &kTopModel;
    sDescShown = (const char *)(uintptr_t)h->descShown;
    sOwner = (struct Unk_08138D64 *)(uintptr_t)h->owner;
    sGsDescWant = (const char *)(uintptr_t)h->gsWant;
    sGsDescShown = (const char *)(uintptr_t)h->gsShown;
    sLblOffX = h->lblOffX; sLblOffY = h->lblOffY; sHdrOffX = h->hdrOffX; sHdrOffY = h->hdrOffY;
    sDescLine = h->descLine;
    sOuiGenShown = h->genShown;
    if (copies) {
        memcpy(sSlotSet, h->slotSet, sizeof sSlotSet);
        memcpy(sSlotExt, h->slotExt, sizeof sSlotExt);
        sSlotSetOn = h->slotSetOn;
        memcpy(sSlotName, h->slotName, sizeof sSlotName);
    }
    sConfirmItem = h->confirmItem;
    sConfirmArg = h->confirmArg;
    sConfirmRow = h->confirmRow;
    sPauseScreen = h->pauseScreen;
    sHelpSaved = h->helpSaved;
    memcpy(sHelpSave, h->helpSave, sizeof sHelpSave);
    memcpy(sObjSave, h->objSave, sizeof(sObjSave));
    memcpy(sRowBanner, h->rowBanner, sizeof(sRowBanner));
}

/* A device that took an image without ever building a screen: the decoded
 * pictures, from the ROM, as a screen build would have made them. */
void PortPocHostStateRebuild(void)
{
    if (PortMenuPoc())
        LoadSources();
}

/* --- an online notice over the play picture (platform/online_notice.c) ---------
 *
 * "WAITING FOR HOST 0:30": the settings rows' banner (the ERASE A FILE
 * label in the game's language, its lettering cleared), widened to the text
 * by repeating a plain column, the text in the label lettering's colour
 * indices (the fallback font: the notice has digits and a colon), into px
 * (stride w, PIC_H rows); and in pal the label's own colours from the ROM
 * (its animation's palette command).  Host memory only: neither the game's
 * video nor this file's decoded sources nor anything a session saves or
 * hashes is touched (DecodeSpriteFrame with offsets of our own, not
 * ClearedBanner's).  Returns the width used; 0 if the picture is not the
 * shape this file expects or it does not fit w. */
#include "pause_settings.h"         /* PortPocPauseMenuArt */
int PortPocNoticeCompose(const char *s, u8 *px, int w, int h, u16 pal[16]);

int PortPocNoticeCompose(const char *s, u8 *px, int w, int h, u16 pal[16])
{
    static u8 src[PIC_H * 240];
    static const u8 kCmdWords[13] = { 0, 3, 3, 2, 1, 2, 3, 2, 3, 2, 4, 2, 2 };
    u16 anim, hdrAnim;
    u8 variant, hdrVariant;
    const s32 *cmd;
    s16 ox, oy;
    int fh, fw, tw, extra, width, x, y, n, found = 0;

    if (h < PIC_H)
        return 0;
    PortPocPauseMenuArt(&anim, &variant, &hdrAnim, &hdrVariant);
    fw = DecodeSpriteFrame(anim, variant, src, 240, PIC_H, &fh, &ox, &oy);
    if (fw != LBL_W || fh != LBL_H)
        return 0;
    for (y = 0; y < PIC_H; y++) {
        u8 bg = src[y * 240 + LBL_BG_X];

        if (bg != 2 && bg != 3)
            continue;
        for (x = LBL_TEXT_X0; x < LBL_TEXT_X1; x++)
            if (src[y * 240 + x] == LBL_FILL || src[y * 240 + x] == LBL_EDGE)
                src[y * 240 + x] = bg;
    }
    tw = CellWidth(s);
    extra = LBL_TEXT_LEFT + tw + 4 - LBL_TEXT_X1;
    if (extra < 0)
        extra = 0;
    width = LBL_W + extra;
    if (width > w)
        return 0;
    memset(px, 0, (size_t)w * PIC_H);
    for (y = 0; y < PIC_H; y++)
        for (x = 0; x < width; x++) {
            int sx = x < ROW_WIDEN_AT ? x : x < ROW_WIDEN_AT + extra ? ROW_WIDEN_AT : x - extra;

            px[y * w + x] = src[y * 240 + sx];
        }
    CellText(px, w, PIC_H, LBL_TEXT_LEFT, LBL_ROW0 + 1, s, LBL_FILL, LBL_EDGE);
    /* the colours: the label's palette command (AnimCmd_GetPalette) */
    memset(pal, 0, 16 * sizeof(u16));
    cmd = gSpriteTables->anims[anim][variant].words;
    for (n = 0; n < 64 && !found; n++) {
        s32 c = cmd[0];

        if (c >= 0 || -c >= 13 || kCmdWords[-c] == 0 || c == -3)
            break;
        if (c == -2) {
            u32 cnt = (u32)cmd[2] & 0xFFFF, off = (u32)cmd[2] >> 16, i;
            const u16 *p = gSpriteTables->palette + 0x10 * cmd[1];

            for (i = 0; i < cnt && off + i < 16; i++)
                pal[off + i] = p[i];
            found = 1;
        }
        cmd += kCmdWords[-c];
    }
    return found ? width : 0;
}
