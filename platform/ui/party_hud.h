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

/* The game's HUD is up: play, not the pause screens, menus, cutscenes or
 * the title (the tap controls are live only then, platform/ui/tap_nav.c). */
int PortPartyHudGameHudUp(void);

/* For the tests and the menu: how many cards the last sample showed, and
 * whether the "1P" tag on the game's own bar was up. */
int PortPartyHudCards(void);
/* ... how many of them are open (the whole card), and the name tags up. */
int PortPartyHudFullCards(void);
int PortPartyHudNameTags(void);

/* --- the connection readout ------------------------------------------------
 * Each human player's link to the host beside their name in the in-game
 * player list (the pause SETTINGS > ONLINE rows, platform/menu_poc.c): a dot
 * (green good, yellow ok, red poor, grey not known / old) and the ping round
 * trip in ms, "R" before it for a link over the room relay.  The host's own
 * rows show HOST (the list's own value) and nothing more, the CPU's nothing.
 * The table comes from the page (web/ln_session.js "the connection
 * readout"): the host measures every guest's link and sends the table to
 * every guest.  Per-device presentation: drawn over the presented picture,
 * never written to game memory, recorded or hashed. */
#define PORT_NETQ_DEVICES   16
#define PORT_NETQ_PATH_UNKNOWN 0
#define PORT_NETQ_PATH_DIRECT  1
#define PORT_NETQ_PATH_RELAY   2
/* grades: good under both, ok under both of the next, else poor */
#define PORT_NETQ_GOOD_MS     80
#define PORT_NETQ_OK_MS       180
#define PORT_NETQ_GOOD_LATE   5
#define PORT_NETQ_OK_LATE     15
enum { PORT_NETQ_GRADE_UNKNOWN, PORT_NETQ_GRADE_GOOD, PORT_NETQ_GRADE_OK, PORT_NETQ_GRADE_POOR };
/* The page's calls: forget the table; one guest device's row (rttMs,
 * latePct -1: not known; stale: the table is old). */
void PortNetqClear(void);
void PortNetqSet(int dev, int rttMs, int path, int latePct, int stale);
int  PortNetqGrade(int rttMs, int latePct);
/* The list (menu_poc.c), each frame its ONLINE rows are drawn on this
 * device's pause overlay (the fork: never the shared game): the rows on
 * screen that show a player -- the row's frame top (GBA y) and the device
 * that plays it (0 the host, -1 the CPU).  colX: the readout column's left
 * edge (GBA x).  Read at the next sample; not drawn otherwise. */
#define PORT_NETQ_ROWS 8
void PortNetqListBegin(int colX);
void PortNetqListRow(int frameY, int dev);
/* present.c: over the 240x160 picture (`picture` = its x y w h in
 * framebuffer pixels; srcW x srcH the picture's own size: drawn only when
 * that is the GBA's 240x160, a menu's). */
void PortNetqDraw(int fbW, int fbH, const int picture[4], int srcW, int srcH, int smooth);

#ifdef __cplusplus
}
#endif

#endif
