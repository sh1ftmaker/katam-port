/* A Kirby's colour from a player card: the simulation side.
 *
 * --- what the game has ---------------------------------------------------------
 *
 * Fourteen Kirby colours, the spray paints of the collection room, as the
 * fourteen variants of one palette animation (anim 0x15, variant = colour):
 * entry 1 the outline, 2..8 the body from highlight to deepest shadow (3 is
 * the fill, the colour a player would name), 9..11 the feet, 12..15 the eyes,
 * mouth and shine.  A Kirby's colour is an index, kirby->color, set when the
 * Kirby is made (sub_0803EA90, from gUnk_0203AD1C: the collection room's
 * paint for player 1, nothing for the others, who take their slot number),
 * and every reload of its palette -- a room, an ability, a respawn, the pause
 * maps -- runs that animation's palette command into OBJ palette line `slot`
 * (sub_0803E558 and friends in kirby.c).  One ability has its own fourteen:
 * the UFO (anim 0x1A0, variant = colour), whose saucer is tinted per colour.
 *
 * --- a paint ---------------------------------------------------------------------
 *
 * Per slot, gPortSim->game.paint[slot] (port/simblock.h): none, a spray index,
 * or a colour 0xRRGGBB.  Set only by PortPaintSetSlot, at the latch of the
 * frame a recording line or a session event names.  It changes the Kirby
 * three ways, all inside the frame, all from simulation state:
 *
 *   kirby->color   the spray, or for a colour the nearest spray (below): set
 *                  when the Kirby is made (PortPaintColorInit) and when the
 *                  paint changes (PortPaintTick, from the Kirby's own task, so
 *                  never while play is paused or the Kirby is not in play);
 *                  everything the game indexes by colour keeps working.
 *   the palette    a colour's palette replaces the spray's in the palette
 *                  command itself (PortPaintPalette, sprite.c), for anim 0x15
 *                  and the UFO's anim when the variant is the one that Kirby
 *                  would load -- so every path that loads a Kirby palette, the
 *                  palette-effect source copy after it (damage flashes, fades),
 *                  and the ability code that keeps a hat's entries on top, all
 *                  see the derived palette without knowing.
 *   the reload     on a change, sub_0803E558 -- the game's own "load this
 *                  Kirby's palette for its ability" -- once, from the tick.
 *
 * The game's spray system is left alone: gUnk_0203AD1C and the save are never
 * written, and paint 0 gives back the colour the game would have chosen.
 * The attract demo and link play are never painted.
 *
 * --- a colour's palette --------------------------------------------------------
 *
 * The fourteen are hand-drawn ramps, not one ramp recoloured: yellow's shadows
 * lean orange, white's go grey, and each has feet of its own.  So a colour is
 * shaded from the spray nearest to it (the template; Cherry and Chalk are not
 * templates -- green feet on a pink body, a grey eye ramp), by moving every
 * entry that differs between the sprays the way the template's fill must move
 * to become the colour, in HSV with integer arithmetic only:
 *
 *   hue         shifted by (colour - fill); a grey template keeps each entry's
 *               own hue where it has one (Snow's pink feet) and takes the
 *               colour's where it has none (the body);
 *   saturation  and value: piecewise linear, mapping the fill's to the
 *               colour's and keeping 0 and 255 where they are, so the
 *               highlight stays the lightest entry and the outline-dark
 *               shadow the darkest.
 *
 * Entries all sprays share (outline, eyes, shine) are kept.  A colour equal to
 * a template's fill gives that template exactly.
 *
 * --- the parts: body and feet ---------------------------------------------------
 *
 * What the game's own palette lets a player colour apart.  Drawn with anim
 * 0x15's sixteen, a Kirby is: the body, 2..8 -- and the eyes, which are drawn
 * in the body's own darkest shades (6, 7, 8) with the outline (1) and the
 * shine (15); the feet, 9..11 -- and the cheeks: the blush is entry 9, the
 * feet's light shade, in every frame (the shine on the feet is 15 again);
 * and 12..14, which no frame of the Kirby draws and the ability hats lend.
 * So there are two parts and no more: a "face" colour of its own would need
 * an entry the game has not got.  A feet colour (paintFeet, BGR555) shades
 * 9..11 from the template's own feet, 9 to the colour, the way the body is
 * shaded from its fill; the HUD's row has one feet entry, 4, shaded the same.
 * The UFO's saucer has no feet and keeps the body's tint.  No feet colour:
 * the feet move with the body, byte for byte as before.  Inputs are the ROM in hand
 * and the paint; every device computes the same bytes, wasm and native alike.
 * Nothing here is cached: the work is a few hundred integer operations a load.
 *
 * Simulation file (tools/check_sim_state.py): no writable host state.
 */

#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "sprite.h"
#include "constants/kirby.h"
#include "port/port.h"
#include "port/simblock.h"
#include "port/kirby_paint.h"
#include "palette_effects.h"
#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void sub_0803E558(u8);
extern const struct AnimInfo gUnk_08350A3C[];

#define BODY_ANIM   0x15        /* the Kirby colour animation, variant = colour */
#define FILL        3           /* its fill entry                               */
#define FEET0       9           /* its feet: 9 (also the cheeks) .. 11          */
#define FEET1       11
#define HUD_FEET    4           /* the HUD row's feet entry                     */
#define UFO_ABILITY KIRBY_ABILITY_UFO

#define P (gPortSim->game)

/* --- reading the game's palettes ---------------------------------------------- */

/* Words per animation command, by ~cmdId (sprite.h AnimCmd_* sizes / 4). */
static const u8 kCmdWords[13] = { 0, 3, 3, 2, 1, 2, 3, 2, 3, 2, 4, 2, 2 };

/* The palette an animation variant's script loads first (its OBJ palette
 * command), all 16 entries of that palette line; NULL if it has none. */
static const u16 *VariantPalette(u16 anim, u16 variant)
{
    const s32 *w;
    int n;

    if (gSpriteTables == NULL)
        return NULL;
    w = gSpriteTables->anims[anim][variant].words;
    for (n = 0; n < 16; n++) {
        s32 cmd = w[0];

        if (cmd >= 0 || -cmd >= 13 || kCmdWords[-cmd] == 0 || cmd == -3 || cmd == -4)
            return NULL;    /* a frame, a jump or the end before any palette */
        if (cmd == -2)
            return gSpriteTables->palette + 0x10 * w[1];
        w += kCmdWords[-cmd];
    }
    return NULL;
}

static int IsTemplate(int k)
{
    return k != 12 && k != 13;      /* not Cherry, not Chalk */
}

/* --- colour arithmetic (integers only) ------------------------------------------- */

static int C8(int c5) { return (c5 << 3) | (c5 >> 2); }
static int C5(int c8) { return (c8 * 31 + 127) / 255; }
static int Clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* HSV: h 0..1535 (six sectors of 256), s and v 0..255. */
static void ToHsv(int r, int g, int b, int *h, int *s, int *v)
{
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn, hh;

    *v = mx;
    *s = mx ? (d * 255 + mx / 2) / mx : 0;
    if (d == 0) {
        *h = 0;
        return;
    }
    if (mx == r)
        hh = (g - b) * 256 / d;
    else if (mx == g)
        hh = 512 + (b - r) * 256 / d;
    else
        hh = 1024 + (r - g) * 256 / d;
    *h = (hh % 1536 + 1536) % 1536;
}

static void FromHsv(int h, int s, int v, int *r, int *g, int *b)
{
    int sector, f, p, q, t;

    if (s <= 0) {
        *r = *g = *b = v;
        return;
    }
    h = (h % 1536 + 1536) % 1536;
    sector = h >> 8;
    f = h & 255;
    p = v * (255 - s) / 255;
    q = v * (255 * 256 - s * f) / (255 * 256);
    t = v * (255 * 256 - s * (256 - f)) / (255 * 256);
    switch (sector) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

/* Map x the way `from` maps to `to`, keeping 0 and 255 fixed. */
static int Remap(int x, int from, int to)
{
    if (x >= from)
        return from >= 255 ? to + (x - from) : Clamp255(to + (x - from) * (255 - to) / (255 - from));
    return from <= 0 ? x : Clamp255(x * to / from);
}

static void Rgb555(u16 c, int *r, int *g, int *b)
{
    *r = C8(c & 31);
    *g = C8((c >> 5) & 31);
    *b = C8((c >> 10) & 31);
}

static u16 To555(int r, int g, int b)
{
    return (u16)(C5(r) | (C5(g) << 5) | (C5(b) << 10));
}

#define GREY 16     /* below this saturation an entry has no hue to speak of */

/* Entry e of the template moved the way the template's fill must move to be
 * (cr, cg, cb). */
static u16 Shade(u16 e, u16 fill, int cr, int cg, int cb)
{
    int er, eg, eb, fr, fg, fb, eh, es, ev, fh, fs, fv, ch, cs, cv, h, s, v, r, g, b;

    Rgb555(e, &er, &eg, &eb);
    Rgb555(fill, &fr, &fg, &fb);
    ToHsv(er, eg, eb, &eh, &es, &ev);
    ToHsv(fr, fg, fb, &fh, &fs, &fv);
    ToHsv(cr, cg, cb, &ch, &cs, &cv);
    if (fs >= GREY)
        h = eh + (ch - fh);
    else
        h = es >= GREY ? eh : ch;
    s = Remap(es, fs, cs);
    v = Remap(ev, fv, cv);
    FromHsv(h, s, v, &r, &g, &b);
    return (u16)(e & 0x8000) | To555(r, g, b);
}

/* How far a template's fill is from a colour.  A colour with a hue (more
 * than a quarter saturated) is matched on hue first, among the templates
 * that have one -- a dark blue is a darker Sapphire, not a blue Carbon; a
 * greyish one by plain distance among the grey ones, Carbon and Snow (a
 * saturated ramp turned grey keeps colourful shadows).  -1: not a
 * candidate. */
#define HUED 64
static int Dist(int ch, int cs, int cv, int cr, int cg, int cb, u16 fill)
{
    int fr, fg, fb, fh, fs, fv, dh;

    Rgb555(fill, &fr, &fg, &fb);
    ToHsv(fr, fg, fb, &fh, &fs, &fv);
    if ((cs < HUED) != (fs < HUED))
        return -1;
    if (cs < HUED) {
        fr -= cr; fg -= cg; fb -= cb;
        return 3 * fr * fr + 4 * fg * fg + 2 * fb * fb;
    }
    dh = ch - fh;
    if (dh < 0) dh = -dh;
    if (dh > 768) dh = 1536 - dh;
    return 4 * dh * dh + (cs - fs) * (cs - fs) + (cv - fv) * (cv - fv);
}

/* The template for a colour: the spray whose fill is nearest.  -1 if the ROM
 * does not have the palettes this file expects. */
static int Nearest(u32 rgb)
{
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int k, best = -1, bestD = 0, h, sat, v;

    ToHsv(r, g, b, &h, &sat, &v);
    for (k = 0; k < PORT_PAINT_COLORS; k++) {
        const u16 *p = VariantPalette(BODY_ANIM, (u16)k);
        int d;

        if (p == NULL)
            return -1;
        if (!IsTemplate(k))
            continue;
        d = Dist(h, sat, v, r, g, b, p[FILL]);
        if (d >= 0 && (best < 0 || d < bestD)) {
            best = k;
            bestD = d;
        }
    }
    return best;
}

int PortPaintValid(u32 v)
{
    switch (v & PORT_PAINT_KIND_MASK) {
    case 0:                  return v == 0;
    case PORT_PAINT_SPRAY:   return (v & 0xFFFFFFu) < PORT_PAINT_COLORS;
    case PORT_PAINT_CUSTOM:  return 1;
    default:                 return 0;
    }
}

int PortPaintTemplate(u32 v)
{
    if ((v & PORT_PAINT_KIND_MASK) == PORT_PAINT_SPRAY)
        return (int)(v & 0xFFu) % PORT_PAINT_COLORS;
    if ((v & PORT_PAINT_KIND_MASK) == PORT_PAINT_CUSTOM)
        return Nearest(v & 0xFFFFFFu);
    return -1;
}

s32 PortPaintSprayRgb(int i)
{
    const u16 *p;
    int r, g, b;

    if (i < 0 || i >= PORT_PAINT_COLORS || (p = VariantPalette(BODY_ANIM, (u16)i)) == NULL)
        return -1;
    Rgb555(p[FILL], &r, &g, &b);
    return (s32)((r << 16) | (g << 8) | b);
}

/* A family of fourteen palettes (one per spray), template t's first num
 * entries shaded to rgb, into out: every entry that differs between the
 * templates moves the way the template's body fill must move.  0 if the ROM
 * disagrees. */
/* Which entries of a family are the feet: f0..f1, shaded from entry f0. */
struct FeetAt { u8 f0, f1; };
static const struct FeetAt kKirbyFeet = { FEET0, FEET1 };
static const struct FeetAt kHudFeet = { HUD_FEET, HUD_FEET };
static const struct FeetAt kNoFeet = { 16, 0 };

/* The same with the parts: the body kept as template t has it (`keep`, a
 * spray) or shaded to rgb, and, when `feet` (PORT_PAINT_FEET_ON | BGR555),
 * the feet entries shaded from the template's own feet to that colour. */
static int DeriveParts(const u16 *const fam[PORT_PAINT_COLORS], int t, int keep, u32 rgb, u32 feet,
                       struct FeetAt fa, u16 *out, u32 num)
{
    const u16 *body = VariantPalette(BODY_ANIM, (u16)t);
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int fr = 0, fg = 0, fb = 0;
    u32 i;
    int k;

    if (body == NULL || num > 16 || t < 0 || t >= PORT_PAINT_COLORS)
        return 0;
    if (feet)
        Rgb555((u16)feet, &fr, &fg, &fb);
    for (i = 0; i < num; i++) {
        u16 e = fam[t][i];
        int varies = 0;

        if (i != 0 && feet && i >= fa.f0 && i <= fa.f1) {
            out[i] = Shade(e, fam[t][fa.f0], fr, fg, fb);
            continue;
        }
        if (keep) {
            out[i] = e;
            continue;
        }
        for (k = 0; k < PORT_PAINT_COLORS && !varies; k++)
            varies = IsTemplate(k) && fam[k][i] != fam[0][i];
        /* entry 0 is transparent: never drawn, kept */
        out[i] = (i == 0 || !varies) ? e : Shade(e, body[FILL], r, g, b);
    }
    return 1;
}


/* A family's palette (anim, variants base..base+13) for template t, its first
 * num entries (what the palette command copies), shaded to rgb, into out.
 * 0 if the ROM disagrees. */
static int Derive(u16 anim, u16 base, int t, int keep, u32 rgb, u32 feet, struct FeetAt fa,
                  u16 *out, u32 num)
{
    const u16 *fam[PORT_PAINT_COLORS];
    int k;

    for (k = 0; k < PORT_PAINT_COLORS; k++)
        if ((fam[k] = VariantPalette(anim, (u16)(base + k))) == NULL)
            return 0;
    return DeriveParts(fam, t, keep, rgb, feet, fa, out, num);
}

/* --- the HUD's palette ---------------------------------------------------------
 *
 * The bottom bar (lives icon, health bar, the ability label's frame) is BG
 * palette line 15 from hud.c's sHudPalettes[colour], one row per spray: the
 * same family shape as the Kirby's, so a colour shades it the same way. */
extern const u16 sHudPalettes[18][16];      /* game/src/hud.c */

int PortPaintFeetValid(u32 feet)
{
    return feet == 0 || (feet & ~0xFFFFu) == 0 && (feet & PORT_PAINT_FEET_ON) != 0;
}

int PortPaintHudPaletteOf(u32 v, u32 feet, int color, u16 out[16])
{
    const u16 *fam[PORT_PAINT_COLORS];
    int k, t, spray = (v & PORT_PAINT_KIND_MASK) == PORT_PAINT_SPRAY;

    if (!PortPaintFeetValid(feet))
        feet = 0;
    if ((v & PORT_PAINT_KIND_MASK) != PORT_PAINT_CUSTOM && !(spray && feet)) {
        t = spray ? PortPaintTemplate(v) : color;
        if (t < 0 || t >= 18)
            return 0;
        memcpy(out, sHudPalettes[t], 16 * sizeof(u16));
        return 1;
    }
    t = PortPaintTemplate(v);
    for (k = 0; k < PORT_PAINT_COLORS; k++)
        fam[k] = sHudPalettes[k];
    return t >= 0 && DeriveParts(fam, t, spray, v & 0xFFFFFFu, feet, kHudFeet, out, 16);
}

int PortPaintBodyPalette(u32 v, u32 feet, int fallback, u16 out[16])
{
    const u16 *p;
    int t, spray = (v & PORT_PAINT_KIND_MASK) == PORT_PAINT_SPRAY;

    if (!PortPaintFeetValid(feet))
        feet = 0;
    if ((v & PORT_PAINT_KIND_MASK) == PORT_PAINT_CUSTOM || (spray && feet)) {
        t = PortPaintTemplate(v);
        return t >= 0 && Derive(BODY_ANIM, 0, t, spray, v & 0xFFFFFFu, feet, kKirbyFeet, out, 16);
    }
    t = spray ? PortPaintTemplate(v) : fallback;
    if (t < 0 || t >= PORT_PAINT_COLORS || (p = VariantPalette(BODY_ANIM, (u16)t)) == NULL)
        return 0;
    memcpy(out, p, 16 * sizeof(u16));
    return 1;
}

/* --- the simulation's side ---------------------------------------------------- */

/* Not the attract demo (0x10), not link play (2). */
static int Paintable(void)
{
    return !(gUnk_0203AD10 & 0x12);
}

void PortPaintSetSlot(int slot, u32 v, u32 feet)
{
    int s;

    if (slot < 0 || slot > 3 || !PortPaintValid(v) || !PortPaintFeetValid(feet))
        return;
    if (v == PORT_PAINT_NONE)
        feet = 0;           /* feet of their own go with a body paint */
    if (P.paint[slot] == v && P.paintFeet[slot] == feet)
        return;
    P.paint[slot] = v;
    P.paintFeet[slot] = (u16)feet;
    P.paintDirty |= (u8)(1u << slot);
    P.paintOn = 0;
    for (s = 0; s < 4; s++)
        if (P.paint[s])
            P.paintOn = 1;
    /* Nobody painting any more: paintDirty still has the Kirbys whose own
     * colour must come back (PortPaintTickSlow, or a new Kirby's init); then
     * the bytes are all zero again. */
    PortLog("[katam-port] paint: f%u slot %d = %08X feet %04X", (unsigned)gPortSim->frame.frameCount,
            slot, (unsigned)v, (unsigned)feet);
}

u32 PortPaintOfSlot(int slot)
{
    return slot >= 0 && slot < 4 ? P.paint[slot] : 0;
}

u32 PortPaintFeetOfSlot(int slot)
{
    return slot >= 0 && slot < 4 ? P.paintFeet[slot] : 0;
}

/* The game's own choice for slot s (sub_0803EA90). */
static u8 GameColor(u8 s)
{
    return gUnk_0203AD1C[s] != 0xFF ? gUnk_0203AD1C[s] : s;
}

u8 PortPaintColorInitSlow(u8 slot, u8 chosen)
{
    int t;

    if (slot < 4)
        P.paintDirty &= (u8)~(1u << slot);     /* the new Kirby loads it fresh */
    if (slot >= 4 || !Paintable() || (t = PortPaintTemplate(P.paint[slot])) < 0)
        return chosen;
    return (u8)t;
}

void PortPaintTickSlow(struct Kirby *k)
{
    u8 s = k->base.unk56;
    int t;

    if (s >= 4 || !((P.paintDirty >> s) & 1))
        return;
    P.paintDirty &= (u8)~(1u << s);
    if (!Paintable())
        return;
    t = PortPaintTemplate(P.paint[s]);
    k->color = t >= 0 ? (u8)t : GameColor(s);
    sub_0803E558(s);
    if (s == gLocalPlayerId)
        P.paintDirty |= PORT_PAINT_DIRTY_HUD;   /* the bar's palette too (hud.c) */
}

/* hud.c: the bar's palette for the screen's Kirby (gLocalPlayerId), whose
 * colour index is `color`: the row of sHudPalettes, or for a custom colour
 * that row shaded like the Kirby. */
const u16 *PortPaintHudPaletteSlow(u8 color, u16 *buf)
{
    u8 s = gLocalPlayerId;
    u32 v;

    if (s >= 4 || !Paintable() || color >= PORT_PAINT_COLORS)
        return sHudPalettes[color];
    v = P.paint[s];
    if (((v & PORT_PAINT_KIND_MASK) != PORT_PAINT_CUSTOM && !P.paintFeet[s]) || gKirbys[s].color != color)
        return sHudPalettes[color];
    if (!PortPaintHudPaletteOf(v, P.paintFeet[s], color, buf))
        return sHudPalettes[color];
    return buf;
}

/* hud.c UpdateGameplayHud, once a frame while the bar is up: the screen's
 * Kirby changed colour (PortPaintTickSlow), so the bar's palette is loaded
 * again now rather than at the next room. */
void PortPaintHudTickSlow(void)
{
    u16 buf[16];
    u8 color;

    P.paintDirty &= (u8)~PORT_PAINT_DIRTY_HUD;
    if (gLocalPlayerId >= 4 || !Paintable())
        return;
    color = gKirbys[gLocalPlayerId].color;
    if (color >= 18)
        return;
    LoadBgPaletteAndPaletteEffectSource(PortPaintHudPaletteSlow(color, buf), 0xF0, 16);
    gMainFlags |= MAIN_FLAG_BG_PALETTE_SYNC_ENABLE;
}

const u16 *PortPaintPaletteSlow(const struct Sprite *sp, const u16 *rom, u16 *buf, u32 num, u32 off)
{
    u8 s = sp->palId;
    u32 v, feet;
    int t, custom;

    (void)off;      /* the command copies from the line's start, to off */
    if (s >= 4 || !Paintable())
        return rom;
    v = P.paint[s];
    custom = (v & PORT_PAINT_KIND_MASK) == PORT_PAINT_CUSTOM;
    feet = P.paintFeet[s];
    if (!custom && !feet)
        return rom;
    t = gKirbys[s].color;
    if (sp->animId == BODY_ANIM && sp->variant == t) {
        if (Derive(BODY_ANIM, 0, t, !custom, v & 0xFFFFFFu, feet, kKirbyFeet, buf, num))
            return buf;
    } else if (custom && sp->animId == gUnk_08350A3C[UFO_ABILITY].animId
               && sp->variant == gUnk_08350A3C[UFO_ABILITY].variant + t) {
        if (Derive(sp->animId, gUnk_08350A3C[UFO_ABILITY].variant, t, 0, v & 0xFFFFFFu, 0, kNoFeet,
                   buf, num))
            return buf;
    }
    return rom;
}

/* --- the presets ------------------------------------------------------------------
 *
 * The picker's grid, 8 x 4, the same for every part.  Row by row: the game's
 * own vivid sprays as a hue wheel; soft colours (Pink and Cherry, then ours);
 * deep ones; then three bright extras and the neutrals from white to black.
 * A spray is named by its index and its colour read from the ROM in hand; the
 * rest are our own RGB.  Every spray is here, so a card on any spray has a
 * cell. */
static const struct { s8 spray; u32 rgb; char name[10]; } kPresets[PORT_PAINT_PRESETS] = {
    {  2, 0, "RED" },        { 10, 0, "ORANGE" },    {  1, 0, "YELLOW" },  {  3, 0, "GREEN" },
    {  9, 0, "EMERALD" },    {  6, 0, "OCEAN" },     {  7, 0, "SAPPHIRE" }, { 8, 0, "GRAPE" },
    {  0, 0, "PINK" },       { 12, 0, "CHERRY" },    { -1, 0xFF8A80, "CORAL" },
    { -1, 0xFFC896, "PEACH" }, { -1, 0xFFF0A0, "CREAM" }, { -1, 0xB4F0B4, "MINT" },
    { -1, 0xA0D4FF, "SKY" }, { -1, 0xD4B4FF, "LILAC" },
    { -1, 0xA01830, "CRIMSON" }, { 11, 0, "CHOCOLATE" }, { -1, 0x808020, "OLIVE" },
    { -1, 0x207838, "FOREST" }, { -1, 0x108888, "TEAL" }, { -1, 0x202880, "NAVY" },
    { -1, 0x4A2898, "INDIGO" }, { -1, 0x802070, "PLUM" },
    { -1, 0xFF40C0, "MAGENTA" }, { -1, 0xE8B020, "GOLD" }, { -1, 0x40E0D0, "AQUA" },
    {  4, 0, "SNOW" },       { 13, 0, "CHALK" },     {  5, 0, "CARBON" },
    { -1, 0x404048, "CHARCOAL" }, { -1, 0x202020, "BLACK" },
};

u32 PortPaintPresetBody(int i)
{
    if (i < 0 || i >= PORT_PAINT_PRESETS)
        return PORT_PAINT_NONE;
    if (kPresets[i].spray >= 0)
        return PortPaintSpray(kPresets[i].spray);
    return PortPaintCustom(kPresets[i].rgb);
}

s32 PortPaintPresetRgb(int i)
{
    if (i < 0 || i >= PORT_PAINT_PRESETS)
        return -1;
    if (kPresets[i].spray >= 0)
        return PortPaintSprayRgb(kPresets[i].spray);
    return (s32)kPresets[i].rgb;
}

const char *PortPaintPresetName(int i)
{
    return i >= 0 && i < PORT_PAINT_PRESETS ? kPresets[i].name : "";
}

u32 PortPaintFeetOfRgb(u32 rgb)
{
    return PortPaintFeet555(To555((int)(rgb >> 16) & 255, (int)(rgb >> 8) & 255, (int)rgb & 255));
}

/* --- unit checks -------------------------------------------------------------------- */

static int Near555(u16 a, u16 b)
{
    int k;

    for (k = 0; k < 15; k += 5) {
        int d = (int)((a >> k) & 31) - (int)((b >> k) & 31);

        if (d < -1 || d > 1)
            return 0;
    }
    return 1;
}

int PortPaintSelfTest(void)
{
    /* FNV-1a over what the one-colour derivation gave before the parts
     * (recorded from fd72fc2 with the English ROM): 4096 custom colours, the
     * fourteen sprays and none, each as the Kirby's palette and the HUD's. */
    const u32 kOldDigest = 0x56B59CB5u;
    u32 h = 2166136261u;
    u16 pal[16], ref[16], hud[16], hudRef[16];
    int i, k, c, fail = 0, checks = 0;

    if (VariantPalette(BODY_ANIM, 0) == NULL) {
        PortLog("[paint-test] no ROM palettes");
        return 1;
    }
    memset(pal, 0, sizeof pal);
    for (i = 0; i < 4096 + 14 + 1; i++) {
        u32 v = i < 4096 ? PortPaintCustom(((u32)((i >> 8) & 15) * 17 << 16) | ((u32)((i >> 4) & 15) * 17 << 8)
                                           | (u32)(i & 15) * 17)
              : i < 4110 ? PortPaintSpray(i - 4096) : 0;

        for (c = 0; c < 2; c++) {
            int ok = c == 0 ? PortPaintBodyPalette(v, 0, 2, pal) : PortPaintHudPaletteOf(v, 0, 3, pal);

            h = (h ^ (u32)ok) * 16777619u;
            for (k = 0; k < 16; k++)
                h = (h ^ pal[k]) * 16777619u;
        }
        h = (h ^ (u32)PortPaintTemplate(v)) * 16777619u;
    }
    checks++;
    if (h != kOldDigest) {
        PortLog("[paint-test] FAIL one-colour palettes changed: digest %08X, was %08X", (unsigned)h,
                (unsigned)kOldDigest);
        fail++;
    }

    /* Every preset: a valid body paint and a colour; the sprays are theirs. */
    for (i = 0; i < PORT_PAINT_PRESETS; i++) {
        checks++;
        if (!PortPaintValid(PortPaintPresetBody(i)) || PortPaintPresetBody(i) == PORT_PAINT_NONE
            || PortPaintPresetRgb(i) < 0 || !PortPaintPresetName(i)[0]
            || (kPresets[i].spray >= 0 && PortPaintPresetRgb(i) != PortPaintSprayRgb(kPresets[i].spray))) {
            PortLog("[paint-test] FAIL preset %d", i);
            fail++;
        }
    }
    for (k = 0; k < PORT_PAINT_COLORS; k++) {
        int found = 0;

        for (i = 0; i < PORT_PAINT_PRESETS; i++)
            found |= PortPaintPresetBody(i) == PortPaintSpray(k);
        checks++;
        if (!found) {
            PortLog("[paint-test] FAIL spray %d is not in the grid", k);
            fail++;
        }
    }

    /* The feet: every body preset with every feet preset.  Only 9..11 (the
     * Kirby) and 4 (the HUD) move; 9 and 4 are the feet colour itself; a
     * spray body keeps the spray's every other entry. */
    for (i = 0; i < PORT_PAINT_PRESETS; i++) {
        u32 v = PortPaintPresetBody(i);

        if (!PortPaintBodyPalette(v, 0, 0, ref) || !PortPaintHudPaletteOf(v, 0, 0, hudRef)) {
            PortLog("[paint-test] FAIL preset %d: no palette", i);
            fail++;
            continue;
        }
        for (c = 0; c < PORT_PAINT_PRESETS; c++) {
            u32 feet = PortPaintFeetOfRgb((u32)PortPaintPresetRgb(c));
            int bad = 0;

            checks++;
            if (!PortPaintBodyPalette(v, feet, 0, pal) || !PortPaintHudPaletteOf(v, feet, 0, hud)) {
                bad = 1;
            } else {
                for (k = 0; k < 16; k++) {
                    if ((k >= FEET0 && k <= FEET1) != 0) {
                        if (k == FEET0 && !Near555(pal[k], (u16)(feet & 0x7FFF)))
                            bad |= 2;
                    } else if (pal[k] != ref[k]) {
                        bad |= 4;
                    }
                    if (k == HUD_FEET) {
                        if (!Near555((u16)(hud[k] & 0x7FFF), (u16)(feet & 0x7FFF)))
                            bad |= 8;
                    } else if (hud[k] != hudRef[k]) {
                        bad |= 16;
                    }
                }
            }
            if (bad) {
                PortLog("[paint-test] FAIL body %s feet %s: %d (9 = %04X want %04X)", kPresets[i].name,
                        kPresets[c].name, bad, (unsigned)pal[FEET0], (unsigned)(feet & 0x7FFF));
                fail++;
            }
        }
    }

    /* Values an event may carry. */
    checks++;
    if (!PortPaintFeetValid(0) || !PortPaintFeetValid(0x8000) || !PortPaintFeetValid(0xFFFF)
        || PortPaintFeetValid(0x7FFF) || PortPaintFeetValid(0x18000)) {
        PortLog("[paint-test] FAIL feet values");
        fail++;
    }
    /* None + feet: the feet are ignored (no body paint, no parts). */
    checks++;
    if (!PortPaintBodyPalette(PORT_PAINT_NONE, 0x801F, 5, pal) || !PortPaintBodyPalette(PortPaintSpray(5), 0, 0, ref)
        || memcmp(pal, ref, sizeof pal) != 0) {
        PortLog("[paint-test] FAIL none + feet");
        fail++;
    }
    PortLog("[paint-test] %d checks, %d failed", checks, fail);
    return fail;
}

#ifdef __cplusplus
}
#endif
