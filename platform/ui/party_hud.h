/* The party HUD: every Kirby that matters at a glance -- each human player,
 * and the CPU helpers while they are with the group.  platform/ui/party_hud.c
 * has the design.  Presentation only: it reads the displayed frame's game
 * state and draws over the presented picture; nothing here is written to
 * game memory, recorded, or hashed. */
#ifndef GUARD_PORT_UI_PARTY_HUD_H
#define GUARD_PORT_UI_PARTY_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Once per simulated frame, at the blit (present.c): read the party from the
 * frame just shown. */
void PortPartyHudSample(void);
/* Before each sample: how world pixels map onto the picture -- view pixel =
 * world - (camX, camY) - (viewX, viewY), the picture being viewW x viewH of
 * them (the GPU renderer's frame; the game camera and 240x160 for the
 * Classic one).  ok = 0: no world picture (no name tags). */
void PortPartyHudSetView(int ok, int camX, int camY, int viewX, int viewY, int viewW, int viewH);
/* Draw it over the presented frame.  `world` = {x, y, w, h} is the picture
 * the game's HUD is pinned to (framebuffer pixels, top-left origin), `scale`
 * framebuffer pixels per HUD pixel, `strip` the HUD's "original strip"
 * anchoring (centred, 240 wide) instead of the corners. */
void PortPartyHudDraw(int fbW, int fbH, const int world[4], double scale, int strip,
                      int smooth, double dpr);
/* The 2D canvas (the web page without WebGL2, host_web.c): sample the frame
 * just simulated (1: something to draw), then blend the cards, the hint and
 * the name tags into a 240x160 RGBA copy of the picture -- never the
 * framebuffer itself. */
int PortPartyHudSoftSample(void);
void PortPartyHudDrawSoft(unsigned int *pixels, int w, int h);
/* Screen areas the party HUD keeps out of: the page's touch pads and menu
 * button, the native menu button.  Framebuffer pixels, top-left origin;
 * slot 0..PORT_PARTY_KEEPOUTS-1, w or h <= 0 clears the slot. */
#define PORT_PARTY_KEEPOUTS 12
void PortPartyHudKeepOut(int slot, int x, int y, int w, int h);

/* For the tests and the menu: how many cards the last sample showed, and
 * whether the "1P" tag on the game's own bar was up. */
int PortPartyHudCards(void);
/* ... how many of them are open (the whole card), and the name tags up. */
int PortPartyHudFullCards(void);
int PortPartyHudNameTags(void);

#ifdef __cplusplus
}
#endif

#endif
