/* Front-end menu proof of concept (KATAM_MENU_POC=1; off by default).
 *
 * Shows that the game's own menus can be edited in place:
 *   - the One Player / Multiplayer pop-up is skipped (Start Game and the
 *     sub-games go straight to one player), so the link-cable entry is gone;
 *   - Game Select gets a fifth entry, SETTINGS, which opens the port's
 *     settings menu.  Its label is drawn by the port as GBA sprite tiles from
 *     a font in platform/menu_poc.c -- no ROM data is involved.
 *
 * With the variable unset every function here is a no-op and the game runs
 * exactly as before.  The switch is a host setting and is not recorded in
 * input recordings: replay a POC recording with the same environment.
 */
#ifndef GUARD_PORT_MENU_POC_H
#define GUARD_PORT_MENU_POC_H

#ifdef __cplusplus
extern "C" {
#endif

struct Unk_08138D64;

/* Nonzero when KATAM_MENU_POC is set to a positive number (read once). */
int PortMenuPoc(void);

/* Game Select's labels were just allocated (end of sub_081397FC): allocate
 * and draw the extra label's tiles. */
void PortMenuPocAlloc(struct Unk_08138D64 *a1);
/* Game Select's labels are being freed (sub_0813A9DC): free ours too. */
void PortMenuPocFree(struct Unk_08138D64 *a1);
/* Once a frame, after the menu task's state function: put the extra label
 * (and, while it is selected, its description) into OAM. */
void PortMenuPocDraw(struct Unk_08138D64 *a1);
/* The cursor moved onto the extra entry: clear the description banner's
 * text (BG0), which the game's own description art would otherwise fill. */
void PortMenuPocBlankBanner(void);
/* The extra entry was chosen. */
void PortMenuPocOpenSettings(void);

/* Game Select gains one entry: cursor 0..4, the extra one is index 4. */
#define PORT_MENU_POC_SETTINGS 4

#ifdef __cplusplus
}
#endif

#endif
