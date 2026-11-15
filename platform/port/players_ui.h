#ifndef GUARD_PORT_PLAYERS_UI_H
#define GUARD_PORT_PLAYERS_UI_H
/* The PLAYERS page: who plays which Kirby with which controller.
 *
 * Reached from the SETTINGS screen's PLAYERS row, on Game Select and on the
 * pause menu's SETTINGS page; platform/menu_poc.c draws it with the settings
 * screen's machinery, platform/players_ui.c says what it shows and what its
 * keys do.  Four rows, P1..P4: CPU, or the controller that player has
 * (KEYBOARD, KEYS 2, TOUCH, PAD n; the pad's own name on the description
 * line).  Left / Right pick another controller for that player (or CPU), A
 * does it: a controller moved from another player takes it from them (that
 * player leaves when it was their only one), CPU drops the player out, a
 * waiting controller on a CPU row joins.  B goes back to SETTINGS.
 *
 * The devices are the host's (web/input_devices.js on the web, through
 * PortPui* below; platform/input_devices.c natively), so the page is host
 * state, never simulation state: it cannot be opened in an online session
 * (the row is dimmed there), and only the joins and leaves it asks for reach
 * the game, as recorded `human` events. */

#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

#define PORT_PUI_ROWS     4
#define PORT_PUI_MAX_DEV  16
#define PORT_PUI_NAME     40

/* Device kinds and players, as platform/port/input_devices.h. */

/* --- the host's side (the page, exported to JS) -----------------------------
 * A snapshot, given whenever PortPuiActive() (every frame is fine: only a
 * change redraws): PortPuiBegin, a PortPuiAddDevice per device (its name
 * first written to PortPuiNameBuf, ASCII, NUL-terminated), PortPuiSetPlayer
 * for each of the four, PortPuiEnd. */
void  PortPuiBegin(void);
char *PortPuiNameBuf(void);
/* num: the pad's number (PAD n), 0 for others; player as the device core
 * (0..3, -1 FOLLOW, -2 PARKED, -3 OFF); flags PORT_PUI_DEV_*. */
void  PortPuiAddDevice(int id, int kind, int num, int player, int flags);
#define PORT_PUI_DEV_PRESENT 1
#define PORT_PUI_DEV_WAITING 2      /* its Start would join now                */
/* flags PORT_PUI_PL_*; mainDev: the device to show for that player (0 none). */
void  PortPuiSetPlayer(int p, int flags, int mainDev);
#define PORT_PUI_PL_HUMAN    1
#define PORT_PUI_PL_PENDING  2      /* a join or leave was asked for            */
#define PORT_PUI_PL_GRACE    4      /* its controller is unplugged (grace time) */
void  PortPuiEnd(void);
/* What the page asked for, oldest first; 0 = nothing.  PORT_PUI_OP_* in the
 * low byte, the player in bits 8..15, the device id from bit 16. */
int   PortPuiPoll(void);
#define PORT_PUI_OP_ASSIGN   1      /* device -> player (0..3)                 */
#define PORT_PUI_OP_DROP     2      /* player (1..3) -> CPU                    */
#define PORT_PUI_OP_ANY      3      /* player 1's claimed devices -> unclaimed */
/* Is the PLAYERS page up (keep the snapshot fresh)? */
int   PortPuiActive(void);

/* The player card (name, colour; another page): A on a row with nothing to
 * change calls this with the slot, if set.  Return 1 if it opened. */
void  PortPuiSetCardHook(int (*open)(int slot));

/* --- the screen's side (platform/menu_poc.c) --------------------------------- */
void  PortPuiOpen(void);            /* the page came up                      */
void  PortPuiClose(void);
u32   PortPuiGen(void);             /* changes when anything shown changes   */
const char *PortPuiRowName(int row);
void  PortPuiRowValue(int row, int selected, char *buf, int size);
const char *PortPuiRowDesc(int row);
int   PortPuiRowUnavailable(int row);
/* Left (5) / Right (6) / A (1) on row `row`: PORT_OUI_K_* (port/online_ui.h). */
int   PortPuiKey(int row, unsigned key);
/* The cursor left row `row` (a choice not confirmed is forgotten). */
void  PortPuiLeaveRow(int row);
/* The SETTINGS screen's PLAYERS row: "1 PLAYER", "3 PLAYERS" (simulation
 * state only: the same on every device of an online session). */
void  PortPuiSummary(char *buf, int size);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_PLAYERS_UI_H */
