#ifndef GUARD_PORT_KIRBY_PAINT_H
#define GUARD_PORT_KIRBY_PAINT_H
/* A Kirby's colour from a player card: simulation side.  platform/kirby_paint.c
 * has the design.
 *
 * Per Kirby slot, a paint: none (the game's own colour -- the collection
 * room's spray paint for player 1, the slot's colour for the others), one of
 * the game's fourteen spray colours, or any colour, from which the whole
 * shaded palette is derived in integer arithmetic.  The paint is simulation
 * state (port/simblock.h, gPortSim->game.paint) and changes only by a
 * frame-stamped input: `paint FRAME SLOT VALUE` in a recording
 * (platform/inputlog.c), PORT_LN_EV_PAINT in an online session
 * (platform/lmpnet.c).  Nobody painting, every hook is a test of one zero
 * byte, and the game is the game it was. */

#include "gba/types.h"
#include "port/simblock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A paint value (the u32 the events carry). */
#define PORT_PAINT_NONE         0u
#define PORT_PAINT_KIND_MASK    0xFF000000u
#define PORT_PAINT_SPRAY        0x01000000u     /* | spray index 0..13            */
#define PORT_PAINT_CUSTOM       0x02000000u     /* | 0xRRGGBB, 8 bits a channel   */
#define PORT_PAINT_COLORS       14              /* the game's spray colours       */
#define PortPaintSpray(i)       (PORT_PAINT_SPRAY | (u32)(i))
#define PortPaintCustom(rgb)    (PORT_PAINT_CUSTOM | ((u32)(rgb) & 0xFFFFFFu))

/* Is v a paint value an event may carry?  (Anything else is refused.) */
int  PortPaintValid(u32 v);

/* The feet (port/simblock.h paintFeet): a second colour for the feet's
 * three entries -- which the game also draws the cheeks with -- or 0, the
 * feet shaded with the body as ever.  Only with a body paint (not NONE). */
#define PORT_PAINT_FEET_ON      0x8000u         /* | BGR555                     */
#define PortPaintFeet555(c)     (PORT_PAINT_FEET_ON | ((u32)(c) & 0x7FFFu))
int  PortPaintFeetValid(u32 feet);

/* --- simulation input -------------------------------------------------- */
/* Slot s (0..3) wears paint v and feet `feet` from now on.  Applied at the
 * latch of the frame it is stamped with (recording line, session event);
 * never call it from the host directly -- schedule it
 * (PortInputLogSchedulePaint, or the session's
 * PortLnEvent(PORT_LN_EV_PAINT, s, v, feet, 0)). */
void PortPaintSetSlot(int slot, u32 v, u32 feet);
/* Read-only, for the host and the presentation layer. */
u32  PortPaintOfSlot(int slot);
u32  PortPaintFeetOfSlot(int slot);

/* --- game hooks ----------------------------------------------------------- */
/* kirby.c sub_0803EA90 (a Kirby is made): its colour index, given the one
 * the game chose. */
u8   PortPaintColorInitSlow(u8 slot, u8 game);
#define PortPaintColorInit(slot, chosen) \
    ((gPortSim->game.paintOn | gPortSim->game.paintDirty) ? PortPaintColorInitSlow((slot), (chosen)) : (chosen))
/* kirby.c sub_0803EE18 (the Kirby's own task, once a frame of play): a paint
 * that changed since its palette was last loaded is put on now. */
struct Kirby;
void PortPaintTickSlow(struct Kirby *k);
#define PortPaintTick(k) do { if (gPortSim->game.paintDirty) PortPaintTickSlow(k); } while (0)
/* sprite.c sub_08155400 (an animation's OBJ palette command): the palette to
 * copy -- `rom`, or the slot's derived palette written into `buf`. */
struct Sprite;
const u16 *PortPaintPaletteSlow(const struct Sprite *s, const u16 *rom, u16 *buf,
                                u32 num, u32 offset);
#define PortPaintPalette(s, rom, buf, num, off) \
    (gPortSim->game.paintOn ? PortPaintPaletteSlow((s), (rom), (buf), (num), (off)) : (rom))

/* hud.c (the bottom bar's palette, BG line 15): sHudPalettes[color], or for a
 * custom colour on the screen's Kirby that row shaded like the Kirby, into
 * buf.  PortPaintHudTick: once a frame from the bar's update, reloads the
 * palette after the screen's Kirby changed colour (paintDirty bit 4). */
#define PORT_PAINT_DIRTY_HUD    0x10
const u16 *PortPaintHudPaletteSlow(u8 color, u16 *buf);
#define PortPaintHudPalette(color, buf) \
    (gPortSim->game.paintOn ? PortPaintHudPaletteSlow((color), (buf)) : sHudPalettes[color])
void PortPaintHudTickSlow(void);
#define PortPaintHudTick() \
    do { if (gPortSim->game.paintDirty & PORT_PAINT_DIRTY_HUD) PortPaintHudTickSlow(); } while (0)

/* --- the derivation, for the card's preview and the presentation layer -----
 * (pure functions of the paint and the loaded ROM; no state). */
/* The Kirby's full body palette (anim 0x15, 16 BGR555 entries) for paint v
 * and feet `feet`; 0 if the ROM has no such palettes.  PORT_PAINT_NONE gives
 * `fallback`'s spray colour (0..13), feet ignored. */
int  PortPaintBodyPalette(u32 v, u32 feet, int fallback, u16 out[16]);
/* The HUD's palette row (sHudPalettes) for paint v and feet on a Kirby whose
 * colour index is `color` (used when v is none): a custom colour's row is
 * shaded like the Kirby.  For the party HUD's icons.  0 if out of range. */
int  PortPaintHudPaletteOf(u32 v, u32 feet, int color, u16 out[16]);
/* The game's colour index a paint stands for: the spray itself, or for a
 * custom colour the nearest spray it is shaded from (tables indexed by
 * colour -- the HUD's palette, the link-up screen -- show that one). */
int  PortPaintTemplate(u32 v);
/* The body fill (entry 3) of spray colour i as 0xRRGGBB, -1 past the table. */
s32  PortPaintSprayRgb(int i);

/* --- the presets: the grid the card's picker shows (32 colours) ------------
 * Ours, not the game's: a hue spread, pastels, darks and the greys, with the
 * game's fourteen spray fills read from the ROM where they fit.  Preset i as
 * a body paint (the spray itself for a spray, else PORT_PAINT_CUSTOM) and as
 * 0xRRGGBB; -1 / 0 past the table or without the ROM. */
#define PORT_PAINT_PRESETS      32
#define PORT_PAINT_PRESET_COLS  8
u32  PortPaintPresetBody(int i);
s32  PortPaintPresetRgb(int i);
const char *PortPaintPresetName(int i);
/* 0xRRGGBB -> a feet value (BGR555, rounded as the palettes are). */
u32  PortPaintFeetOfRgb(u32 rgb);
/* Unit checks of the derivation (old one-colour paints give the palettes
 * they always did, the feet move only the feet); failures, 0 = all good.
 * Needs the ROM loaded.  Logs what failed. */
int  PortPaintSelfTest(void);

#ifdef __cplusplus
}
#endif
#endif /* GUARD_PORT_KIRBY_PAINT_H */
