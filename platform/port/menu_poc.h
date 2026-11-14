/* Front-end menu proof of concept (KATAM_MENU_POC=1 natively; on the web a
 * page under .../menus/ or with ?menus=1; off by default).
 *
 * Shows that the game's own menus can be edited in place:
 *   - Start Game's own One Player / Multiplayer pop-up is kept, but
 *     MULTIPLAYER no longer leads to the link-cable lobby: it opens a
 *     MULTIPLAYER screen (LOCAL: two players on this screen,
 *     platform/localmp.c; ONLINE: shown, not available yet).  The sub-games'
 *     copy of the pop-up is still skipped (they go straight to one player);
 *   - Game Select gets a fifth entry, SETTINGS (the five rows are re-spaced
 *     so they clear the walking Kirbys), which opens a settings screen drawn
 *     in the game picture and driven by the pad: a few player-facing fields
 *     of gPortSettings, applied and saved the way the port's own settings
 *     menu applies and saves them.  Labels, rows and text are GBA sprite
 *     tiles drawn by the port from a font in platform/menu_poc.c -- no ROM
 *     data is involved.
 *
 *   - the pause menu gains a fourth page after the world map, the same
 *     settings screen (game/src/pause_settings.c; Select cycles to it from
 *     the help page where there is no map).
 *
 * With the switch off every function here is a no-op and the game runs
 * exactly as before.  The switch is a host setting and is not recorded in
 * input recordings: replay a POC recording with the same environment.
 */
#ifndef GUARD_PORT_MENU_POC_H
#define GUARD_PORT_MENU_POC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

struct Unk_08138D64;

/* Nonzero when the proof of concept is switched on (read once). */
int PortMenuPoc(void);

/* --- Game Select's fifth entry -------------------------------------------- */

/* Game Select's labels were just allocated (end of sub_081397FC): allocate
 * and draw the extra label's tiles. */
void PortMenuPocAlloc(struct Unk_08138D64 *a1);
/* Game Select's labels are being freed (sub_0813A9DC): free ours too (and the
 * settings screen's, which use the same slots). */
void PortMenuPocFree(struct Unk_08138D64 *a1);
/* Once a frame, after the menu task's state function: put the extra label
 * (and, while it is selected, its description) into OAM. */
void PortMenuPocDraw(struct Unk_08138D64 *a1);
/* The cursor moved onto the extra entry: clear the description banner's
 * text (BG0), which the game's own description art would otherwise fill. */
void PortMenuPocBlankBanner(void);

/* Game Select gains one entry: cursor 0..4, the extra one is index 4. */
#define PORT_MENU_POC_SETTINGS 4
/* Game Select's row pitch with five rows (the game's is 0x15 for four). */
#define PORT_MENU_POC_ROW_PITCH 17

/* The Start Game pop-up (restored with local multiplayer): while its
 * MULTIPLAYER row is lit, our own description line instead of the game's
 * MULTI-PAK one (the banner is blanked by the caller); 0 = none. */
void PortMenuPocPopupDesc(int multiplayer);

/* --- the MULTIPLAYER screen (LOCAL / ONLINE) ---------------------------------
 * The settings screen's functions draw it (PortMenuPocSettingsDraw, ...Input,
 * ...CursorY/X, ...Close); this opens it instead of the settings. */
int  PortMenuPocMpOpen(struct Unk_08138D64 *a1);
/* Build the settings (multiplayer = 0) or MULTIPLAYER screen if it is not
 * up yet; 0 while the sprite memory is not free yet (ask again next frame;
 * `frames` asked so far, for a log line if it never comes). */
int  PortMenuPocScreenReady(struct Unk_08138D64 *a1, int multiplayer, int frames);
/* Does A on this row of the screen up now do anything? */
int PortMenuPocRowAvailable(int row);
/* a1->unk1 while the MULTIPLAYER screen is up. */
#define PORT_MENU_POC_MP_SCREEN 0x11
#define PORT_MENU_POC_MP_LOCAL  0
#define PORT_MENU_POC_MP_ONLINE 1

/* --- the settings screen ---------------------------------------------------- */

/* a1->unk1 while the settings screen is up (the game uses 1..10). */
#define PORT_MENU_POC_SCREEN 0x10

/* Allocate and draw the screen (row cursor a1->unk3A6 = 0); 0 if there is
 * no sprite memory for it (yet). */
int  PortMenuPocSettingsOpen(struct Unk_08138D64 *a1);
/* Put it into OAM, everything shifted right by slide pixels (0 = in place). */
void PortMenuPocSettingsDraw(struct Unk_08138D64 *a1, int slide);
/* One pad event (sub_0813B484's 0..6, B excluded): 0 nothing happened,
 * 1 the cursor moved, 2 a value changed (applied and saved). */
int PortMenuPocSettingsInput(struct Unk_08138D64 *a1, unsigned key);
/* The star cursor's place for the current row. */
int PortMenuPocSettingsCursorY(struct Unk_08138D64 *a1);
int PortMenuPocSettingsCursorX(void);
void PortMenuPocSettingsClose(struct Unk_08138D64 *a1);

/* --- the screen, shared by the front end and the pause page ------------------
 *
 * The same six rows, header and description line from one set of functions;
 * a caller supplies PORT_POC_SCREEN_TILES of OBJ VRAM, keeps the row cursor
 * in its own memory, and names the label (ERASE A FILE) and header
 * (SUB-GAMES) sprite variants of the game's language to build them from.
 * The description line's colours are copied each frame from the BG palette
 * line of the banner at 0x0600FD40 (BG0, screen block 31, rows 21-22). */
#define PORT_POC_SCREEN_TILES 660
void PortPocScreenBuild(u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row);
/* Into OAM; everything shifted right by slide pixels (0 = in place). */
void PortPocScreenDraw(u32 tiles, int row, int slide, u8 layer);
/* key as sub_0813B484 (1 A, 3 up, 4 down, 5 left, 6 right): 0 nothing,
 * 1 the cursor moved, 2 a value changed (applied and saved). */
int PortPocScreenInput(u32 tiles, u8 *row, unsigned key);
int PortPocScreenCursorY(int row);
int PortPocScreenCursorX(void);

/* --- the pause menu's settings page (game/src/pause_settings.c) -------------- */

/* Its sprite memory: the screen's tiles, then the star cursor's 16; the
 * whole range is copied away when the page opens and put back when it
 * closes, so neither gameplay's sprite tiles nor another page's are lost. */
#define PORT_POC_PAUSE_OBJ_VRAM    0x06010000u
#define PORT_POC_PAUSE_CURSOR_VRAM (PORT_POC_PAUSE_OBJ_VRAM + PORT_POC_SCREEN_TILES * 32)
#define PORT_POC_PAUSE_OBJ_BYTES   0x5600u
void PortPocPauseObjSave(void);
void PortPocPauseObjRestore(void);

#ifdef __cplusplus
}
#endif

#endif
