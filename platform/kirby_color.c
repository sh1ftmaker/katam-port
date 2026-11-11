/* Kirby's colour: the player's own choice, in single player and in co-op.
 *
 * The game has fourteen Kirby colours -- the spray paints of the collection
 * room -- as fourteen variants of one palette animation (anim 0x15: entry 1
 * the outline, 2..8 the body from light to dark, 9..11 the feet, 12..15 the
 * colours an ability hat lends), and a Kirby's colour is an index,
 * kirby->color, that tables all over the game use: the HUD's palette, the
 * UFO's, the link-up screen's sprites.  This file lets the player wear any of
 * the fourteen whether or not their file has found that paint, or a colour
 * of their own: any body colour and any feet colour.
 *
 * --- a colour of their own ----------------------------------------------------
 *
 * The fourteen are hand-drawn ramps, not one ramp recoloured: a yellow Kirby's
 * shadows lean orange, a white one's go grey.  So a custom palette is built
 * from a model of how the game's own artists shaded a body colour, fitted to
 * the fourteen when the ROM is loaded.  In OKLab lightness/chroma/hue, every
 * body and feet entry i is
 *
 *     L_i = a_i + b_i * L_base     C_i = c_i * C_base     h_i = h_base + d_i
 *
 * with base the entry a picked colour stands for (KC_BODY_BASE, KC_FEET_BASE)
 * and a, b fitted by least squares over the fourteen, c the mean chroma ratio
 * and d the chroma-weighted mean hue offset.  Given only each paint's two base
 * colours, the model rebuilds the other entries of every one of the fourteen
 * to a mean OKLab distance of about 0.035 (leave-one-out; a plain hue shift of
 * the pink ramp manages 0.047).  The HUD's Kirby colours (BG palette 15) are
 * fitted the same way against the same bases.  Fitted, not tabulated: the
 * coefficients come from the ROM in hand and none of its data is in this file.
 *
 * A custom-coloured Kirby's index stays the nearest of the fourteen, so every
 * table indexed by colour stays in range and shows the nearest paint; only
 * the Kirby's own palette (the loads of anim 0x15 in kirby.c,
 * PortKirbyColorFixup) and the HUD's (hud.c, PortKirbyHudFixup) are replaced.
 *
 * --- whose colour wins ----------------------------------------------------------
 *
 * The setting, when it is not "the game's"; else the paint picked in the
 * collection room; else, in co-op, the seat's colour (player 2 yellow, as in
 * link play); else pink.  The choice lives in gUnk_0203AD1C[local], the
 * game's own per-Kirby colour that every Kirby rebuild reads, and in
 * kirby->color for the live Kirby.  Nothing is written to the save: the
 * game's choice (gUnk_0203ADE0) is left alone and given back when the
 * setting returns to "the game's".
 *
 * Refused while rollback, the lockstep session or seats are active: those
 * hash the game's memory and every peer must hold the same bytes, and a colour
 * is one player's setting.  Off the adventure (link play, sub-games, the
 * attract demo) the game's colours stand.  tests/sim_state_allowlist.txt.
 */

#include <math.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "main.h"
#include "kirby.h"
#include "level.h"
#include "sprite.h"
#include "palette.h"
#include "palette_effects.h"
#include "port/port.h"
#include "port/rollback.h"
#include "port/netsim.h"
#include "port/seats.h"
#include "port/kirby_color.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define KC_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define KC_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern void sub_0803E558(u8);                     /* kirby.c: reload a Kirby's palette */
extern void PortHudReloadKirbyPalette(void);      /* hud.c */
extern const u16 *PortHudPalette(u8 color);       /* hud.c: sHudPalettes[color] */

#define KC_ANIM       0x15   /* the Kirby colour animation; variant = colour   */
#define KC_BODY0      2      /* entries 2..8: the body, light to dark          */
#define KC_BODYN      7
#define KC_FEET0      9      /* entries 9..11: the feet                        */
#define KC_FEETN      3
#define KC_BODY_BASE  3      /* the entry a body colour stands for: the fill   */
#define KC_FEET_BASE  9      /* ... and a feet colour: the most-drawn shade    */
#define KC_HUD_FEET   4      /* the HUD's feet entry; its body is kHudBody     */

#define KC_PREVIEW_ANIM 0    /* Kirby, standing: anim 0 variant 0, frame 0     */

static const u8 kHudBody[] = { 1, 2, 3, 5, 6 };

/* --- colour spaces ------------------------------------------------------------ */

typedef struct { float L, C, h; } Lch;

static float ToLinear(float x)
{
    return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
}

static float ToSrgb(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}

static Lch LchFromRgbF(float r, float g, float b)
{
    float l, m, s, L, A, B;
    Lch o;

    r = ToLinear(r); g = ToLinear(g); b = ToLinear(b);
    l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    A = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    B = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    o.L = L;
    o.C = sqrtf(A * A + B * B);
    o.h = atan2f(B, A);
    return o;
}

static Lch Lch555(u16 c)
{
    return LchFromRgbF((c & 31) / 31.0f, ((c >> 5) & 31) / 31.0f, ((c >> 10) & 31) / 31.0f);
}

static u16 To555(Lch c)
{
    float A = c.C * cosf(c.h), B = c.C * sinf(c.h), l, m, s, r, g, b;
    int ri, gi, bi;

    if (c.L < 0.0f) c.L = 0.0f;
    if (c.L > 1.0f) c.L = 1.0f;
    l = c.L + 0.3963377774f * A + 0.2158037573f * B;
    m = c.L - 0.1055613458f * A - 0.0638541728f * B;
    s = c.L - 0.0894841775f * A - 1.2914855480f * B;
    l = l * l * l; m = m * m * m; s = s * s * s;
    r = ToSrgb( 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s);
    g = ToSrgb(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s);
    b = ToSrgb(-0.0041960863f * l - 0.7034186147f * m + 1.7076956211f * s);
    ri = (int)(r * 31.0f + 0.5f);
    gi = (int)(g * 31.0f + 0.5f);
    bi = (int)(b * 31.0f + 0.5f);
    return (u16)(ri | (gi << 5) | (bi << 10));
}

static u16 Rgb555(u32 rgb)
{
    u32 r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;

    return (u16)(((r * 31 + 127) / 255) | (((g * 31 + 127) / 255) << 5)
                 | (((b * 31 + 127) / 255) << 10));
}

static u32 RgbOf555(u16 c)
{
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;

    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

static float LabDist(Lch x, Lch y)
{
    float ax = x.C * cosf(x.h), bx = x.C * sinf(x.h);
    float ay = y.C * cosf(y.h), by = y.C * sinf(y.h);
    float dl = x.L - y.L, da = ax - ay, db = bx - by;

    return sqrtf(dl * dl + da * da + db * db);
}

/* --- the game's palettes, and the model fitted to them --------------------------- */

typedef struct { float a, b, c, dh; u8 from; } Shade;   /* from: 0 kept, 1 body, 2 feet */

static struct {
    u8  state;          /* 0 not tried, 1 built, 2 this ROM lacks the palettes */
    u16 pal[PORT_KIRBY_COLORS][16];
    u16 hud[PORT_KIRBY_COLORS][16];
    Lch body[PORT_KIRBY_COLORS], feet[PORT_KIRBY_COLORS];
    Shade kirby[16], hudShade[16];
} sModel;

/* Sizes of the animation script commands, by ~cmdId (sprite.h). */
static const u8 kCmdSize[13] = { 0, 0xC, 0xC, 8, 4, 8, 0xC, 8, 0xC, 8, 0x10, 8, 8 };

/* The palette an animation variant loads: its first getPalette command. */
static const u16 *VariantPalette(u16 anim, u8 variant, int *num, int *offset)
{
    const union AnimCmd *base;
    union AnimCmd v;
    const s32 *w;
    int n;

    if (gSpriteTables == NULL)
        return NULL;
    base = gSpriteTables->anims[anim];
    v = base[variant];
    w = &v.words[0];
    for (n = 0; n < 16; n++) {
        s32 id = w[0];

        if (id >= 0 || -id >= (s32)sizeof(kCmdSize))
            return NULL;
        if (id == -2) {
            const struct AnimCmd_GetPalette *g = (const struct AnimCmd_GetPalette *)(const void *)w;

            *num = g->numColors;
            *offset = g->insertOffset;
            return &gSpriteTables->palette[0x10 * g->palId];
        }
        if (id == -3 || id == -4)
            return NULL;        /* a loop or the end before any palette */
        w += kCmdSize[-id] / 4;
    }
    return NULL;
}

/* Fit one entry against the base it follows, over the fourteen. */
static void Fit(Shade *s, const Lch *base, const Lch *val, int n)
{
    float mx = 0, my = 0, sxx = 0, sxy = 0, cr = 0, hs = 0, hc = 0;
    int i, ncr = 0;

    for (i = 0; i < n; i++) {
        mx += base[i].L;
        my += val[i].L;
    }
    mx /= n;
    my /= n;
    for (i = 0; i < n; i++) {
        sxx += (base[i].L - mx) * (base[i].L - mx);
        sxy += (base[i].L - mx) * (val[i].L - my);
        if (base[i].C > 0.03f) {
            cr += val[i].C / base[i].C;
            ncr++;
            hs += sinf(val[i].h - base[i].h) * val[i].C;
            hc += cosf(val[i].h - base[i].h) * val[i].C;
        }
    }
    s->b = sxx > 1e-6f ? sxy / sxx : 1.0f;
    s->a = my - s->b * mx;
    s->c = ncr ? cr / ncr : 1.0f;
    s->dh = (hs != 0 || hc != 0) ? atan2f(hs, hc) : 0.0f;
}

static int ModelReady(void)
{
    Lch base[PORT_KIRBY_COLORS], val[PORT_KIRBY_COLORS];
    int v, i, k;

    if (sModel.state)
        return sModel.state == 1;
    if (gSpriteTables == NULL)
        return 0;               /* too early: try again later */
    sModel.state = 2;
    for (v = 0; v < PORT_KIRBY_COLORS; v++) {
        int num, off;
        const u16 *p = VariantPalette(KC_ANIM, (u8)v, &num, &off);
        const u16 *h = PortHudPalette((u8)v);

        if (p == NULL || num != 16 || off != 0 || h == NULL)
            return 0;
        memcpy(sModel.pal[v], p, sizeof(sModel.pal[v]));
        memcpy(sModel.hud[v], h, sizeof(sModel.hud[v]));
        sModel.body[v] = Lch555(p[KC_BODY_BASE]);
        sModel.feet[v] = Lch555(p[KC_FEET_BASE]);
    }
    memset(sModel.kirby, 0, sizeof(sModel.kirby));
    memset(sModel.hudShade, 0, sizeof(sModel.hudShade));
    for (i = 0; i < 16; i++) {
        int body = i >= KC_BODY0 && i < KC_BODY0 + KC_BODYN;
        int feet = i >= KC_FEET0 && i < KC_FEET0 + KC_FEETN;

        if (!body && !feet)
            continue;
        for (v = 0; v < PORT_KIRBY_COLORS; v++) {
            base[v] = body ? sModel.body[v] : sModel.feet[v];
            val[v] = Lch555(sModel.pal[v][i]);
        }
        Fit(&sModel.kirby[i], base, val, PORT_KIRBY_COLORS);
        sModel.kirby[i].from = body ? 1 : 2;
    }
    for (k = 0; k <= (int)sizeof(kHudBody); k++) {
        int body = k < (int)sizeof(kHudBody);

        i = body ? kHudBody[k] : KC_HUD_FEET;
        for (v = 0; v < PORT_KIRBY_COLORS; v++) {
            base[v] = body ? sModel.body[v] : sModel.feet[v];
            val[v] = Lch555(sModel.hud[v][i]);
        }
        Fit(&sModel.hudShade[i], base, val, PORT_KIRBY_COLORS);
        sModel.hudShade[i].from = body ? 1 : 2;
    }
    sModel.state = 1;
    return 1;
}

static u16 Shaded(const Shade *s, Lch body, Lch feet)
{
    Lch b = s->from == 1 ? body : feet, o;

    o.L = s->a + s->b * b.L;
    o.C = s->c * b.C;
    o.h = b.h + s->dh;
    return To555(o);
}

static int Nearest(Lch body, Lch feet)
{
    float best = 1e9f;
    int v, at = 0;

    for (v = 0; v < PORT_KIRBY_COLORS; v++) {
        float d = LabDist(body, sModel.body[v]) + 0.1f * LabDist(feet, sModel.feet[v]);

        if (d < best) {
            best = d;
            at = v;
        }
    }
    return at;
}

/* The Kirby and HUD palettes for a custom colour: the nearest paint's, with
 * the body and feet entries built from the model.  Needs ModelReady(). */
static int BuildCustom(u16 body555, u16 feet555, u16 kirby[16], u16 hud[16])
{
    Lch body = Lch555(body555), feet = Lch555(feet555);
    int near = Nearest(body, feet), i;

    if (kirby) {
        memcpy(kirby, sModel.pal[near], 32);
        for (i = 0; i < 16; i++)
            if (sModel.kirby[i].from)
                kirby[i] = Shaded(&sModel.kirby[i], body, feet);
        /* The two picked colours land exactly where they stand for. */
        kirby[KC_BODY_BASE] = body555;
        kirby[KC_FEET_BASE] = feet555;
    }
    if (hud) {
        memcpy(hud, sModel.hud[near], 32);
        for (i = 0; i < 16; i++)
            if (sModel.hudShade[i].from)
                hud[i] = Shaded(&sModel.hudShade[i], body, feet);
    }
    return near;
}

/* --- per-slot custom palettes ------------------------------------------------------- */

static struct KcSlot {
    u8  on;
    u8  nearest;
    u16 body, feet;     /* BGR555 */
    u16 kirby[16];
    u16 hud[16];
} sSlot[PORT_KIRBY_COLOR_SLOTS];

KC_EXPORT int PortKirbyColorSetSlot(int slot, int on, u16 body555, u16 feet555)
{
    struct KcSlot *s;

    if (slot < 0 || slot >= PORT_KIRBY_COLOR_SLOTS)
        return 0;
    s = &sSlot[slot];
    body555 &= 0x7FFF;
    feet555 &= 0x7FFF;
    if (on && !ModelReady())
        on = 0;
    if (!on) {
        if (!s->on)
            return 0;
        s->on = 0;
        return 1;
    }
    if (s->on && s->body == body555 && s->feet == feet555)
        return 0;
    s->on = 1;
    s->body = body555;
    s->feet = feet555;
    s->nearest = (u8)BuildCustom(body555, feet555, s->kirby, s->hud);
    return 1;
}

KC_EXPORT int PortKirbyColorNearest(u16 body555, u16 feet555)
{
    if (!ModelReady())
        return 0;
    return Nearest(Lch555(body555 & 0x7FFF), Lch555(feet555 & 0x7FFF));
}

/* As the palette command itself writes (sprite.c, sub_08155400): through
 * the fade transformation while one is running, else straight into the
 * shadow palette with a sync. */
static void WriteObjColors(const u16 *src, u32 offset, u32 num)
{
    if (gMainFlags & MAIN_FLAG_OBJ_PALETTE_TRANSFORMATION_ENABLE) {
        LoadObjPaletteWithTransformation(src, (u8)offset, (u16)num);
    } else {
        memcpy(&gObjPalette[offset], src, num * sizeof(u16));
        gMainFlags |= MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE;
    }
}

void PortKirbyColorFixup(u8 slot)
{
    if (slot >= PORT_KIRBY_COLOR_SLOTS || !sSlot[slot].on)
        return;
    WriteObjColors(&sSlot[slot].kirby[KC_BODY0], 0x10u * slot + KC_BODY0, KC_BODYN + KC_FEETN);
}

void PortKirbyHudFixup(void)
{
    u8 me = gLocalPlayerId;

    if (me >= PORT_KIRBY_COLOR_SLOTS || !sSlot[me].on)
        return;
    LoadBgPaletteAndPaletteEffectSource(sSlot[me].hud, 0xF0, 16);
}

/* --- the local player's choice -------------------------------------------------------- */

static struct {
    s8  choice;         /* PortKirbyColorChoose: GAME, 0..13 or CUSTOM            */
    s8  seat;           /* PortKirbyColorSetSeat: -1 none                          */
    u8  applied;        /* gUnk_0203AD1C[local] holds our value, not the game's    */
    u8  pad;
    u16 body, feet;     /* the custom colour, BGR555                               */
} sKc = { PORT_KIRBY_COLOR_GAME, -1, 0, 0, 0, 0 };

KC_EXPORT void PortKirbyColorChoose(int choice, u32 bodyRgb, u32 feetRgb)
{
    if (choice < PORT_KIRBY_COLOR_GAME || choice > PORT_KIRBY_COLOR_CUSTOM)
        choice = PORT_KIRBY_COLOR_GAME;
    sKc.choice = (s8)choice;
    sKc.body = Rgb555(bodyRgb);
    sKc.feet = Rgb555(feetRgb);
}

void PortKirbyColorSetSeat(int color)
{
    sKc.seat = (s8)(color >= 0 && color < PORT_KIRBY_COLORS ? color : -1);
}

static int Blocked(void)
{
    return PortRbActive() || PortNetActive() || PortSeatActive();
}

/* The one-player adventure with its four Kirbys (platform/coop.c,
 * InAdventure): not link play (2), a sub-game (4) or the attract demo. */
static int OnePlayer(void)
{
    return gNumHumanPlayers == 1 && gLocalPlayerId < PORT_KIRBY_COLOR_SLOTS
        && !(gUnk_0203AD10 & (2 | 4 | 0x10));
}

/* What the local Kirby should wear: the value for gUnk_0203AD1C[local]
 * (0xFF = the game's default for the slot), whether that is ours rather than
 * the game's, and whether it is a custom colour. */
static u8 Decide(int *ours, int *custom)
{
    u8 game = gUnk_0203ADE0;

    *ours = 0;
    *custom = 0;
    if (sKc.choice == PORT_KIRBY_COLOR_CUSTOM && ModelReady()) {
        *ours = 1;
        *custom = 1;
        return (u8)Nearest(Lch555(sKc.body), Lch555(sKc.feet));
    }
    if (sKc.choice >= 0 && sKc.choice < PORT_KIRBY_COLORS) {
        *ours = 1;
        return (u8)sKc.choice;
    }
    if (game < PORT_KIRBY_COLORS)
        return game;
    if (sKc.seat >= 0) {
        *ours = 1;
        return (u8)sKc.seat;
    }
    return game;
}

u8 PortKirbyColorInit(u8 slot, u8 game)
{
    int ours, custom;
    u8 want;

    if (slot != gLocalPlayerId || Blocked() || !OnePlayer())
        return game;
    want = Decide(&ours, &custom);
    if (!ours)
        return game;
    if (custom)
        PortKirbyColorSetSlot(slot, 1, sKc.body, sKc.feet);
    return want;
}

void PortKirbyColorFrame(void)
{
    struct Kirby *me;
    u8 slot = gLocalPlayerId, want, show;
    int ours, custom, changed;

    if (Blocked() || !OnePlayer() || gUnk_02023354 == NULL || gNumKirbys != 4)
        return;
    want = Decide(&ours, &custom);
    changed = PortKirbyColorSetSlot(slot, custom, sKc.body, sKc.feet);
    if ((ours || sKc.applied) && gUnk_0203AD1C[slot] != want)
        gUnk_0203AD1C[slot] = want;
    sKc.applied = (u8)ours;
    me = &gKirbys[slot];
    show = want < PORT_KIRBY_COLORS ? want : slot;
    if (me->color != show || changed) {
        me->color = show;
        sub_0803E558(slot);
        PortHudReloadKirbyPalette();
    }
}

KC_EXPORT int PortKirbyColorLocal(u16 *body555, u16 *feet555)
{
    u8 slot = gLocalPlayerId;
    int on = slot < PORT_KIRBY_COLOR_SLOTS && sSlot[slot].on;

    if (body555) *body555 = on ? sSlot[slot].body : 0;
    if (feet555) *feet555 = on ? sSlot[slot].feet : 0;
    return slot < PORT_KIRBY_COLOR_SLOTS ? gKirbys[slot].color : 0;
}

/* --- menus and the page ------------------------------------------------------------------ */

KC_EXPORT int PortKirbyColorAvailable(void)
{
    return ModelReady();
}

KC_EXPORT s32 PortKirbyColorSwatch(int color, int which)
{
    if (!ModelReady() || color < 0 || color >= PORT_KIRBY_COLORS)
        return -1;
    return (s32)RgbOf555(sModel.pal[color][which ? KC_FEET_BASE : KC_BODY_BASE]);
}

KC_EXPORT int PortKirbyColorBuild(int choice, u32 bodyRgb, u32 feetRgb, u16 out[16])
{
    if (!ModelReady())
        return 0;
    if (choice == PORT_KIRBY_COLOR_CUSTOM)
        BuildCustom(Rgb555(bodyRgb), Rgb555(feetRgb), out, NULL);
    else
        memcpy(out, sModel.pal[choice >= 0 && choice < PORT_KIRBY_COLORS ? choice : 0], 32);
    return 1;
}

/* GBA OBJ sizes by shape << 2 | size (hardware, not game data). */
static const u8 kObjSize[12][2] = {
    { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
    { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 },
    { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 },
};

#define KC_PREVIEW_TILES 128

static u8 sPreviewTiles[KC_PREVIEW_TILES * 32];   /* the menu preview's scratch */

/* The 4bpp tile sheet as bytes (through a plain pointer: the 64-bit builds'
 * P32 converts to one pointer type at a time -- rl_build.c does the same). */
static const u8 *Tiles4(void)
{
    const void *v = gSpriteTables->tiles4bpp;

    return (const u8 *)v;
}

/* The standing Kirby, decoded from the sprite tables the way DisplaySprite
 * and rl_build.c's PortRlEmitSprite read them: the variant's script up to
 * its first frame (its tile uploads), then the frame's subsprites. */
KC_EXPORT int PortKirbyColorPreview(u8 *out, int maxW, int maxH, int *ow, int *oh)
{
    u8 *tiles = sPreviewTiles;
    const union AnimCmd *base;
    union AnimCmd v;
    union SpriteAttributes at;
    const struct SpriteAttributes_Sub *sub;
    const u16 *oam;
    const s32 *w;
    s32 frame = -1;
    int n, k, ntiles = 0, fw, fh, fhflip, fvflip;

    if (out == NULL || gSpriteTables == NULL)
        return 0;
    base = gSpriteTables->anims[KC_PREVIEW_ANIM];
    v = base[0];
    w = &v.words[0];
    for (n = 0; n < 32; n++) {
        s32 id = w[0];

        if (id >= 0) {
            frame = ((const struct AnimCmd_ShowFrame *)(const void *)w)->index;
            break;
        }
        if (-id >= (s32)sizeof(kCmdSize) || id == -3 || id == -4)
            return 0;
        if (id == -1) {
            const struct AnimCmd_GetTiles *g = (const struct AnimCmd_GetTiles *)(const void *)w;
            int cnt = g->numTilesToCopy;

            if (g->tileIndex < 0)
                return 0;       /* 8bpp: not Kirby's */
            if (cnt > KC_PREVIEW_TILES) cnt = KC_PREVIEW_TILES;
            memcpy(tiles, Tiles4() + 0x20 * g->tileIndex, (size_t)cnt * 32);
            ntiles = cnt;
        }
        w += kCmdSize[-id] / 4;
    }
    if (frame < 0)
        return 0;
    at = gSpriteTables->attrs[KC_PREVIEW_ANIM];
    if ((frame >> 28) == 1) {
        const struct SpriteAttributes_Full *full = &at.full[frame & 0x0FFFFFFF];
        const s32 *list;
        u32 cnt = full->unkC >> 24, i;

        list = gSpriteTables->unk18 + (full->unkC & 0xFFFFFF);
        if (list[0] < 0)
            return 0;
        if (cnt > KC_PREVIEW_TILES) cnt = KC_PREVIEW_TILES;
        for (i = 0; i < cnt; i++)
            memcpy(tiles + i * 32, Tiles4() + 0x20 * list[i], 32);
        ntiles = (int)cnt;
        sub = &full->sub;
    } else {
        sub = &at.sub[frame];
    }
    fw = sub->width;
    fh = sub->height;
    if (fw <= 0 || fh <= 0 || fw > maxW || fh > maxH)
        return 0;
    fhflip = (sub->bitfield >> 14) & 1;
    fvflip = (sub->bitfield >> 15) & 1;
    memset(out, 0, (size_t)fw * fh);
    oam = gSpriteTables->oamData[KC_PREVIEW_ANIM];
    for (k = 0; k < sub->numSubframes; k++) {
        const u16 *t = &oam[3 * ((sub->bitfield & 0x3FFF) + k)];
        u16 t0 = t[0], t1 = t[1], t2 = t[2];
        int shape = ((t0 & 0xC000) >> 12) | ((t1 & 0xC000) >> 14);
        int x1 = t1 & 0x1FF, y1 = t0 & 0xFF, sw, sh, hf, vf, tile, px, py;

        if (shape >= 12 || (t0 & 0x2000))
            continue;           /* not a 4bpp subsprite */
        if (x1 >= 256) x1 -= 512;
        if (y1 >= 128) y1 -= 256;
        sw = kObjSize[shape][0];
        sh = kObjSize[shape][1];
        hf = (t1 >> 12) & 1;
        vf = (t1 >> 13) & 1;
        if (fhflip) { hf ^= 1; x1 = fw - sw - x1; }
        if (fvflip) { vf ^= 1; y1 = fh - sh - y1; }
        tile = t2 & 0x3FF;
        for (py = 0; py < sh; py++) {
            for (px = 0; px < sw; px++) {
                int sx = hf ? sw - 1 - px : px, sy = vf ? sh - 1 - py : py;
                int ti = tile + (sy >> 3) * (sw >> 3) + (sx >> 3), c, dx = x1 + px, dy = y1 + py;

                if (ti >= ntiles || dx < 0 || dy < 0 || dx >= fw || dy >= fh)
                    continue;
                c = tiles[ti * 32 + (sy & 7) * 4 + ((sx & 7) >> 1)];
                c = (sx & 1) ? c >> 4 : c & 15;
                if (c)
                    out[dy * fw + dx] = (u8)c;
            }
        }
    }
    if (ow) *ow = fw;
    if (oh) *oh = fh;
    return 1;
}

#ifdef __cplusplus
}
#endif
