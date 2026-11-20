/* SETTINGS > ACCESSIBILITY (platform/access.c, docs/features/access.md).
 *
 * Presentation and this device's own input only: nothing here is written to
 * game memory or hashed.  The options live in one word, gPortSettings.access
 * ("access=0x..." in the saved settings; absent = this device's first-run
 * defaults, which follow prefers-reduced-motion and prefers-contrast on the
 * web). */
#ifndef GUARD_PORT_ACCESS_H
#define GUARD_PORT_ACCESS_H

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_A11Y_MARKERS   0x01u   /* a shape per Kirby on tags and cards   */
#define PORT_A11Y_CONTRAST  0x02u   /* high-contrast HUD tags, text, pads    */
#define PORT_A11Y_FLASH     0x04u   /* reduce flashing (brightness-up ramps) */
#define PORT_A11Y_HOLDB     0x08u   /* B: hold, let go, it stays held        */
#define PORT_A11Y_SHAKE_SHIFT 4     /* 2 bits: 0 FULL, 1 LESS, 2 OFF         */
#define PORT_A11Y_TOUCH_SHIFT 6     /* 2 bits: pads 100 125 150 175 %        */
#define PORT_A11Y_MASK      0xFFu

/* The defaults for this device (the page's prefers-* queries; 0 natively). */
unsigned PortAccessDefaults(void);
unsigned PortAccessClamp(unsigned w);

/* The menu's section (platform/menu_poc.c kExtRows). */
#ifdef __EMSCRIPTEN__
#define PORT_ACCESS_ROWS 6
#else
#define PORT_ACCESS_ROWS 5          /* no touch pads natively */
#endif
struct PortPocRows;
extern const struct PortPocRows gPocAccessRows;

/* Hold-to-toggle for B: this device's player's raw keys in, the keys to
 * latch out.  Called where the host's keys are handed over (PortSetKeys,
 * PortLmpSetHostKeys); a pass-through while the option is off. */
u16 PortAccessKeys(int player, u16 mask);

/* Reduced flashing: a brightness-up (BLDCNT effect 2) coefficient for this
 * scanline as shown -- it may rise only one step a frame, so a flash of a
 * frame or two barely shows while a slow fade still reaches white.  ch:
 * 0 the software renderer, 1 the render list. */
u32 PortAccessBrightUp(int ch, u32 evy);

/* Screen shake kept, of 256 (256 when the option is off). */
int PortAccessShakeKeep(void);

/* The party HUD: the shape glyph for a Kirby slot (a character its font
 * draws), 0 when markers are off; and whether high contrast is on. */
char PortAccessMarker(int slot);
int  PortAccessContrast(void);

#ifdef __cplusplus
}
#endif

#endif
