/* Tap to move: the phone's autopilot for this device's player 1.
 * platform/ui/tap_nav.c has the design; docs/design/TAP-TO-MOVE.md the why.
 *
 * Host side, like the party HUD: it reads the game (the Kirby, the room's
 * collision map, the objects) and writes nothing the simulation owns.  What
 * it produces is a GBA key mask, OR-ed into this device's player 1 keys in
 * platform/main.c exactly where the host's own buttons go -- so the input log
 * records keys as always, replays need nothing new, and an online session
 * sees this device's input and nothing else.  The taps themselves are never
 * recorded. */
#ifndef GUARD_PORT_UI_TAP_NAV_H
#define GUARD_PORT_UI_TAP_NAV_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- the frame loop (platform/main.c) ------------------------------------ */

/* Once per live frame, just before the key latch: steps the autopilot and
 * returns its keys for the next frame (0 when off).  hostKeys: what the
 * person is holding on a real pad or keyboard -- any press takes the
 * autopilot's hands off (a manual press always wins). */
unsigned short PortTapFrame(unsigned short hostKeys);
/* The keys PortTapFrame last returned (0 when off): main.c ORs them into
 * player 1's host keys. */
unsigned short PortTapKeys(void);

/* --- switching it on -------------------------------------------------------
 * On when the setting says TAP (PLAYERS > TOUCH PADS, PS_TOUCH_TAP), or when
 * the host forces it (the page's ?tap=1, KATAM_TAP=1 natively). */
void PortTapForce(int on);
int  PortTapEnabled(void);
/* Enabled and the game is in play (its HUD is up, not replaying): the host
 * sends pointers only then. */
int  PortTapActive(void);

/* --- pointers --------------------------------------------------------------
 * kind PORT_UI_DOWN / MOVE / UP / CANCEL (ui.h), framebuffer pixels (the GL
 * canvas, top-left origin), id the finger.  Mapped to the world through the
 * view the last presented picture was drawn with. */
void PortTapPointer(int kind, float x, float y, int id);
/* The same gesture in world pixels: the headless tests' hook (never
 * recorded; the keys it leads to are). */
void PortTapWorld(int kind, int wx, int wy, int id);
/* The native tests' devices-script commands (tap X Y, tapcell CX CY,
 * down/move/up X Y, tapkirby, swipekirby, tapenemy): 1 if `cmd` was one. */
int  PortTapScript(const char *cmd);

/* --- presentation (present.c, host_web.c's 2D canvas) ---------------------- */

/* How world pixels map onto the picture: view pixel = world - (camX, camY)
 * - (viewX, viewY), the picture viewW x viewH of them; world = where that
 * picture lands in the framebuffer {x, y, w, h}.  ok = 0: no world picture. */
void PortTapSetView(int ok, int camX, int camY, int viewX, int viewY, int viewW, int viewH);
void PortTapSetWorldRect(const int world[4], float dpr);
/* The markers (target, reticle, path) over the presented picture, under
 * the party HUD's cards.  GL overlays only (no 2D-canvas markers). */
void PortTapHudDraw(int fbW, int fbH, const int world[4], int smooth);

#ifdef __cplusplus
}
#endif

#endif
