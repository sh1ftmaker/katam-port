/* Front-end menu proof of concept (KATAM_MENU_POC=1 natively; on the web a
 * page under .../menus/ or with ?menus=1; off by default).
 *
 * Shows that the game's own menus can be edited in place:
 *   - the One Player / Multiplayer pop-up is skipped (Start Game and the
 *     sub-games go straight to one player), so the link-cable entry is gone;
 *   - Game Select gets a fifth entry, SETTINGS (the five rows are re-spaced
 *     so they clear the walking Kirbys), which opens a settings screen drawn
 *     in the game picture and driven by the pad: a few player-facing fields
 *     of gPortSettings, applied and saved the way the port's own settings
 *     menu applies and saves them.  Labels, rows and text are GBA sprite
 *     tiles drawn by the port from a font in platform/menu_poc.c -- no ROM
 *     data is involved.
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

/* --- the settings screen ---------------------------------------------------- */

/* a1->unk1 while the settings screen is up (the game uses 1..10). */
#define PORT_MENU_POC_SCREEN 0x10

/* Allocate and draw the screen (row cursor a1->unk3A6 = 0). */
void PortMenuPocSettingsOpen(struct Unk_08138D64 *a1);
/* Put it into OAM, everything shifted right by slide pixels (0 = in place). */
void PortMenuPocSettingsDraw(struct Unk_08138D64 *a1, int slide);
/* One pad event (sub_0813B484's 0..6, B excluded): 0 nothing happened,
 * 1 the cursor moved, 2 a value changed (applied and saved). */
int PortMenuPocSettingsInput(struct Unk_08138D64 *a1, unsigned key);
/* The star cursor's place for the current row. */
int PortMenuPocSettingsCursorY(struct Unk_08138D64 *a1);
int PortMenuPocSettingsCursorX(void);
void PortMenuPocSettingsClose(struct Unk_08138D64 *a1);

#ifdef __cplusplus
}
#endif

#endif
