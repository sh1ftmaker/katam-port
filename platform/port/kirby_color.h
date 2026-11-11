/* Kirby's colour: the player's own choice, in single player and in co-op.
 * platform/kirby_color.c has the whole story; this is the interface.
 *
 * A colour is one of
 *   PORT_KIRBY_COLOR_GAME (-1)   whatever the game would show: the spray
 *                                paint picked in the collection room, else
 *                                (co-op) the seat's colour, else pink
 *   0 .. 13                      one of the game's fourteen, found or not
 *   PORT_KIRBY_COLOR_CUSTOM (14) any body colour and any feet colour
 *
 * Colours cross this interface in two forms: 0xRRGGBB (settings, menus,
 * the page) and BGR555 (the GBA's, and the co-op pose). */
#ifndef GUARD_PORT_KIRBY_COLOR_H
#define GUARD_PORT_KIRBY_COLOR_H

/* Plain C types: the menu (C++, without the game's include path) uses this
 * header too.  They are the same types as u8/u16/u32/s32. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_KIRBY_COLORS       14
#define PORT_KIRBY_COLOR_GAME   (-1)
#define PORT_KIRBY_COLOR_CUSTOM 14
/* Kirby slots with a palette of their own: the game's four.  More players
 * than that (co-op beyond four) are drawn outside gKirbys and build their
 * palettes with PortKirbyColorBuild. */
#define PORT_KIRBY_COLOR_SLOTS  4
/* The co-op pose's custom-colour word: body BGR555 | this bit. */
#define PORT_KIRBY_COLOR_POSE_CUSTOM 0x8000

/* --- the local player's choice --------------------------------------------- */

/* The setting (gPortSettings.kirbyColor/kirbyBody/kirbyFeet).  Applied by
 * PortKirbyColorFrame from the next gameplay frame. */
void PortKirbyColorChoose(int choice, uint32_t bodyRgb, uint32_t feetRgb);
/* Co-op: the colour a seat gives a player who chose nothing, -1 for none. */
void PortKirbyColorSetSeat(int color);
/* Once a frame, before the game's tasks (platform/main.c). */
void PortKirbyColorFrame(void);
/* What the local Kirby wears now: returns the game's colour index (the
 * nearest of the fourteen for a custom colour) and, for a custom colour,
 * the two BGR555 values (else 0). */
int PortKirbyColorLocal(uint16_t *body555, uint16_t *feet555);

/* --- any Kirby slot ---------------------------------------------------------- */

/* A slot's custom palette: on = 0 gives the slot back to the game's
 * fourteen.  Returns 1 when that changed what the slot looks like; the
 * caller reloads the palette (sub_0803E558).  Co-op puppets use this. */
int PortKirbyColorSetSlot(int slot, int on, uint16_t body555, uint16_t feet555);
/* The nearest of the fourteen to a custom colour. */
int PortKirbyColorNearest(uint16_t body555, uint16_t feet555);

/* --- for menus and the page -------------------------------------------------- */

/* 1 once the game's palettes are read and the model is built (the ROM is
 * loaded and has them). */
int PortKirbyColorAvailable(void);
/* One of the fourteen's body (which = 0) or feet (1) colour, 0xRRGGBB; -1
 * when the palettes are not available. */
int32_t PortKirbyColorSwatch(int color, int which);
/* A full 16-entry Kirby palette (BGR555) for any choice; GAME is pink.
 * The same palette PortKirbyColorSetSlot puts on a Kirby. */
int PortKirbyColorBuild(int choice, uint32_t bodyRgb, uint32_t feetRgb, uint16_t out[16]);
/* The standing Kirby as palette indices, for a preview: writes up to
 * maxW x maxH bytes (row-major, *w per row), 0 = transparent.  Returns 1 on
 * success. */
int PortKirbyColorPreview(uint8_t *out, int maxW, int maxH, int *w, int *h);

/* --- game hooks (game/src/kirby.c, game/src/hud.c) -------------------------- */

/* After the game loaded a Kirby's colour palette (anim 0x15) into OBJ
 * palette `slot`: a custom palette replaces the body and feet. */
void PortKirbyColorFixup(uint8_t slot);
/* After the HUD loaded its Kirby palette (BG palette 15). */
void PortKirbyHudFixup(void);
/* Kirby init: the colour index a new Kirby gets (`game` is the game's). */
uint8_t PortKirbyColorInit(uint8_t slot, uint8_t game);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_KIRBY_COLOR_H */
