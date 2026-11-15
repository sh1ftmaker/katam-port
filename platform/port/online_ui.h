#ifndef GUARD_PORT_ONLINE_UI_H
#define GUARD_PORT_ONLINE_UI_H
/* The front end's ONLINE screens (Start Game > MULTIPLAYER > ONLINE >
 * HOST GAME / JOIN GAME): what they show and what their keys do.
 * platform/menu_poc.c draws them with the MULTIPLAYER screen's machinery;
 * the page (web/online.js) does the networking, polls the requests made
 * here (PortOuiPoll) and reports back (PortOuiSet, PortOuiNote).
 *
 * Front-end state only, and only outside a session: once an online session
 * is on (gPortLnOn) none of it is read -- the MULTIPLAYER screen, if the game
 * ever reaches it again, shows fixed text and does nothing -- so nothing here
 * is simulation state (it is not in any image, keyframe or hash). */

#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

/* Sub-screens of the MULTIPLAYER screen. */
#define PORT_OUI_SUB_MP     0   /* LOCAL / ONLINE (the screen as it was)   */
#define PORT_OUI_SUB_ONLINE 1   /* HOST GAME / JOIN GAME                   */
#define PORT_OUI_SUB_HOST   2   /* CODE / START                            */
#define PORT_OUI_SUB_JOIN   3   /* CODE / JOIN                             */
/* server browser (platform/online_browse.c) */
#define PORT_OUI_SUB_BROWSE 4   /* the public games                        */
#define PORT_OUI_SUB_PASS   5   /* PASSWORD / JOIN: a locked game's        */

/* States, set by the page (PortOuiSet). */
enum {
    PORT_OUI_IDLE = 0,
    PORT_OUI_H_CREATING, PORT_OUI_H_WAITING, PORT_OUI_H_JOINING, PORT_OUI_H_READY,
    PORT_OUI_H_NOSERVER,
    PORT_OUI_J_CONNECTING, PORT_OUI_J_WAITING, PORT_OUI_J_STARTING,
    PORT_OUI_J_NOGAME, PORT_OUI_J_FULL, PORT_OUI_J_BUILD, PORT_OUI_J_ROM,
    PORT_OUI_J_FAILED, PORT_OUI_J_HOSTLEFT,
    /* server browser: a locked game asks (web/online_browse.js) */
    PORT_OUI_J_PASSWORD, PORT_OUI_J_PWBAD, PORT_OUI_J_PWLOCK,
    PORT_OUI_J_REMOVED,     /* 18: the host removed this device (web/online.js UI) */
    PORT_OUI_STATES
};

/* Notes (transient, PortOuiNote). */
#define PORT_OUI_NOTE_NONE    0
#define PORT_OUI_NOTE_COPIED  1     /* invite link copied                 */
#define PORT_OUI_NOTE_NOCOPY  2     /* the browser would not copy it      */

/* Requests to the page (PortOuiPoll). */
#define PORT_OUI_EV_HOST_OPEN   1   /* create a room                       */
#define PORT_OUI_EV_HOST_CLOSE  2   /* leave the HOST screen: close it     */
#define PORT_OUI_EV_HOST_START  3   /* the game is starting: session on    */
#define PORT_OUI_EV_COPY_LINK   4
#define PORT_OUI_EV_JOIN        5   /* PortOuiJoinCode has the code        */
#define PORT_OUI_EV_JOIN_CANCEL 6

/* What a key did (PortMenuPocMpKey, platform/menu_poc.c). */
#define PORT_OUI_K_NONE    0        /* not handled: the screen's own keys  */
#define PORT_OUI_K_QUIET   1
#define PORT_OUI_K_CURSOR  2
#define PORT_OUI_K_SELECT  3
#define PORT_OUI_K_BACK    4
#define PORT_OUI_K_DENY    5
#define PORT_OUI_K_START   6        /* start the game, two players, online */

/* --- the page's side (exported to JS) --- */
void PortOuiSet(int state, int c0, int c1, int c2, int c3);
void PortOuiNote(int note);
/* HOST GAME: people in the game so far (1..4: the host and each guest's
 * players), and of those how many are still connecting.  The START row
 * shows "PLAYERS n/4". */
void PortOuiSetPlayers(int players, int connecting);
int  PortOuiPoll(void);
int  PortOuiJoinCode(int i);
int  PortOuiScreen(void);           /* sub * 256 + state, for tests        */

/* --- the screens' side --- */
int  PortOuiSub(void);
void PortOuiReset(void);            /* the MULTIPLAYER screen opened       */
u32  PortOuiGen(void);              /* changes when anything shown changes */
const char *PortOuiHeader(int sub);
const char *PortOuiRowName(int sub, int row);
void PortOuiRowValue(int sub, int row, int selected, char *buf, int size);
const char *PortOuiRowDesc(int sub, int row);
int  PortOuiRowUnavailable(int sub, int row);
/* One key (sub_0813B484's 1..6) on an online sub-screen; *row is the
 * cursor.  PORT_OUI_K_*; PortOuiSub() may have changed. */
int  PortOuiKey(int *row, unsigned key);
/* The fade out after PORT_OUI_K_START finished (the game proper starts). */
void PortOuiFadeDone(void);
/* Is the screen busy with the network (the idle timeout must not fire)? */
int  PortOuiBusy(void);

/* HOST GAME's mode: 0 = TOGETHER, 1 = FREE ROAM (rooms mode, the default). */
int PortOuiRoomsMode(void);

/* server browser (platform/online_browse.c): something it shows changed;
 * join the game with this code through JOIN GAME's states. */
void PortOuiChanged(void);
void PortOuiJoinWith(const char code[4]);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_ONLINE_UI_H */
