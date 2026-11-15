#ifndef GUARD_PORT_ONLINE_BROWSE_H
#define GUARD_PORT_ONLINE_BROWSE_H
/* The server browser: public games and passwords, on the ONLINE screens
 * (platform/online_ui.c, drawn by platform/menu_poc.c).
 *
 *   ONLINE > BROWSE GAMES   the public list (PORT_OUI_SUB_BROWSE): host name,
 *                           players, mode, a lock for a password.  A on a
 *                           game joins it through JOIN GAME's own states.
 *   HOST GAME rows 2, 3     GAME: PRIVATE (default: by code or link only,
 *                           never listed) / PUBLIC (listed for anyone), and
 *                           PASSWORD (typed with the pad, or a keyboard).
 *   PASSWORD                (PORT_OUI_SUB_PASS) opens by itself when a game
 *                           a player is joining asks for its password.
 *
 * The page (web/online_browse.js) fetches the list, publishes the host's
 * game and does the password's challenge and response; this file is the
 * screens' model and the host's two choices.  Front-end host memory only,
 * outside any session, never simulation state. */

#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

#define PORT_OBR_PASS_MAX   8       /* A-Z 0-9                               */
#define PORT_OBR_LIST_MAX   50      /* netplay/listing.mjs LIST_SHOWN        */
#define PORT_OBR_VIS        4       /* list rows on screen (the MP screen's
                                       sprite memory holds four: menu_poc.c) */

/* Browse status (PortObrListStatus, from the page). */
enum { PORT_OBR_LOADING = 0, PORT_OBR_OK, PORT_OBR_NOCONN, PORT_OBR_OLD, PORT_OBR_BUSY };
/* The host's listing (PortObrSetHostList, from the page). */
enum { PORT_OBR_H_PRIVATE = 0, PORT_OBR_H_PUBLISHING, PORT_OBR_H_LISTED, PORT_OBR_H_OLD,
       PORT_OBR_H_NOCONN, PORT_OBR_H_LIMIT };

/* Requests to the page (PortObrPoll). */
#define PORT_OBR_EV_PW_ANSWER 1     /* PASSWORD > JOIN: answer the challenge */

/* --- the screens' side (online_ui.c, menu_poc.c) --- */
const char *PortObrRowName(int sub, int row);
void PortObrRowValue(int sub, int row, int selected, char *buf, int size);
const char *PortObrRowDesc(int sub, int row);
int  PortObrRowUnavailable(int sub, int row);
void PortObrOpen(void);             /* BROWSE GAMES opened: the list anew   */
/* A key on BROWSE: PORT_OUI_K_*; on K_SELECT code[0..3] is the game to join. */
int  PortObrBrowseKey(int *row, unsigned key, char code[4]);
/* A key on PASSWORD; K_BACK = cancel the join. */
int  PortObrPassKey(int *row, unsigned key, int joinState);
/* A key on HOST GAME's rows 2, 3 (PORT_OUI_K_NONE: the screen's own). */
int  PortObrHostKey(int *row, unsigned key);
void PortObrPassOpened(void);       /* the PASSWORD screen just came up     */
int  PortObrRows(void);             /* BROWSE: rows on screen (1..4)        */
int  PortObrFollowRow(int sub, int row);  /* the cursor the screen should have */

/* --- the page's side (exported) --- */
void PortObrListSet(const char *packed);  /* "CODE\tNAME\tN\tFLAGS\n"...    */
void PortObrListStatus(int status);
int  PortObrBrowsing(void);
void PortObrSetHostList(int state);
int  PortObrHostPublic(void);
const char *PortObrHostPassJs(void);
const char *PortObrPassJs(void);    /* what the joiner typed                */
const char *PortObrSelectedJs(void);  /* tests: the selected game's code    */
int  PortObrPoll(void);
int  PortObrTyping(void);           /* a password row is being typed        */
int  PortObrWantsText(void);        /* the cursor is on a password row      */
void PortObrTypeChar(int c);        /* a keyboard's letter; 8 = backspace   */
void PortObrTypingEnd(void);
void PortObrSetTextJs(const char *s);    /* a phone's prompt                */

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_ONLINE_BROWSE_H */
