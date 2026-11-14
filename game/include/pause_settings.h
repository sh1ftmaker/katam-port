#ifndef GUARD_PAUSE_SETTINGS_H
#define GUARD_PAUSE_SETTINGS_H

/* PORT: the pause menu's settings page, a fourth page after the world map
 * (KATAM_MENU_POC; port/menu_poc.h).  game/src/pause_settings.c. */

#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"

/* gPauseMenus[].menuId while it is up; next to enum PauseMenuId's 1, 2, 4. */
#define MENU_PORT_SETTINGS 8

void CreatePauseSettings(void);

/* game/src/code_08138D64.c: what the page borrows from the front end. */
void PortPocPauseMenuArt(u16 *lblAnim, u8 *lblVariant, u16 *hdrAnim, u8 *hdrVariant);
void PortPocPauseLoadMenuColours(u32 scratchVram);

#ifdef __cplusplus
}
#endif

#endif
