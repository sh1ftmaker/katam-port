/* The front end's ONLINE screens: platform/port/online_ui.h says what.
 *
 * Everything here is host memory read only by the front end's MULTIPLAYER
 * screen while no online session is on (each entry point that the game's
 * frames reach checks gPortLnOn first), so a session's rewinds, replays and
 * images never see it. */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "port/lmpnet.h"
#include "port/online_ui.h"
#include "port/online_browse.h"     /* server browser */

#ifdef __cplusplus
extern "C" {
#endif

/* netplay/codes.mjs: room codes are 4 of these (no vowels, no digits). */
static const char kAlpha[] = "BCDFGHJKMNPQRSTVWXZ";
#define NALPHA ((int)sizeof(kAlpha) - 1)

static int sSub;
static int sState, sNote;
static char sCode[5] = "----";
static int sJoin[4], sJoinPos;
static int sStartPending;
/* HOST GAME's START row, Left / Right: play TOGETHER (one screen's rules:
 * everyone in one room, a shared camera) or FREE ROAM (rooms mode,
 * platform/localmp.c: each device on its own player, meeting through doors
 * and the cell phone).  Read by web/online.js when the game starts. */
static int sRoomsMode;

int PortOuiRoomsMode(void) { return sRoomsMode; }
static int sPlayers = 1, sConnecting;
static u32 sGen = 1;
static int sEv[8], sEvHead, sEvTail;

static void Push(int ev)
{
    int next = (sEvTail + 1) & 7;

    if (next == sEvHead)
        return;             /* the page is not polling: drop, never block */
    sEv[sEvTail] = ev;
    sEvTail = next;
}

/* --- the page's side ------------------------------------------------------- */

void PortOuiSet(int state, int c0, int c1, int c2, int c3)
{
    if (state < 0 || state >= PORT_OUI_STATES)
        state = PORT_OUI_IDLE;
    sState = state;
    if (c0 > 0) {
        sCode[0] = (char)c0; sCode[1] = (char)c1; sCode[2] = (char)c2; sCode[3] = (char)c3;
        sCode[4] = '\0';
    }
    /* server browser: a locked game's password screen opens by itself */
    if ((state == PORT_OUI_J_PASSWORD || state == PORT_OUI_J_PWBAD)
        && (sSub == PORT_OUI_SUB_JOIN || sSub == PORT_OUI_SUB_PASS)) {
        sSub = PORT_OUI_SUB_PASS;
        PortObrPassOpened();
    }
    sGen++;
}

/* server browser */
void PortOuiChanged(void) { sGen++; }

void PortOuiSetPlayers(int players, int connecting)
{
    sPlayers = players < 1 ? 1 : players > 4 ? 4 : players;
    sConnecting = connecting < 0 ? 0 : connecting;
    sGen++;
}

void PortOuiNote(int note)
{
    sNote = note;
    sGen++;
}

int PortOuiPoll(void)
{
    int ev;

    if (sEvHead == sEvTail)
        return 0;
    ev = sEv[sEvHead];
    sEvHead = (sEvHead + 1) & 7;
    return ev;
}

int PortOuiJoinCode(int i)
{
    return i >= 0 && i < 4 ? kAlpha[sJoin[i] % NALPHA] : 0;
}

int PortOuiScreen(void)
{
    return sSub * 256 + sState;
}

/* --- the screens' side ----------------------------------------------------- */

int PortOuiSub(void) { return gPortLnOn ? PORT_OUI_SUB_MP : sSub; }

void PortOuiReset(void)
{
    if (sSub == PORT_OUI_SUB_HOST)
        Push(PORT_OUI_EV_HOST_CLOSE);
    if (sSub == PORT_OUI_SUB_PASS)              /* server browser */
        Push(PORT_OUI_EV_JOIN_CANCEL);
    sSub = PORT_OUI_SUB_MP;
    sGen++;
}

u32 PortOuiGen(void) { return sGen; }

const char *PortOuiHeader(int sub)
{
    switch (sub) {
    case PORT_OUI_SUB_ONLINE: return "ONLINE";
    case PORT_OUI_SUB_HOST:   return "HOST GAME";
    case PORT_OUI_SUB_JOIN:   return "JOIN GAME";
    default:                  return "MULTIPLAYER";
    }
}

const char *PortOuiRowName(int sub, int row)
{
    /* server browser */
    if (sub == PORT_OUI_SUB_ONLINE && row == 2)
        return "BROWSE GAMES";
    if (sub >= PORT_OUI_SUB_BROWSE || (sub == PORT_OUI_SUB_HOST && row >= 2))
        return PortObrRowName(sub, row);
    switch (sub) {
    case PORT_OUI_SUB_ONLINE: return row == 0 ? "HOST GAME" : "JOIN GAME";
    case PORT_OUI_SUB_HOST:   return row == 0 ? "CODE" : "START";
    case PORT_OUI_SUB_JOIN:   return row == 0 ? "CODE" : "JOIN";
    default:                  return "";
    }
}

static int JoinBusy(void)
{
    return sState == PORT_OUI_J_CONNECTING || sState == PORT_OUI_J_WAITING
        || sState == PORT_OUI_J_STARTING;
}

static int HostHasRoom(void)
{
    return sState == PORT_OUI_H_WAITING || sState == PORT_OUI_H_JOINING || sState == PORT_OUI_H_READY;
}

void PortOuiRowValue(int sub, int row, int selected, char *buf, int size)
{
    const char *v = "";

    buf[0] = '\0';
    /* server browser */
    if (sub >= PORT_OUI_SUB_BROWSE || (sub == PORT_OUI_SUB_HOST && row >= 2)) {
        PortObrRowValue(sub, row, selected, buf, size);
        return;
    }
    if (sub == PORT_OUI_SUB_HOST) {
        if (row == 0) {
            v = sState == PORT_OUI_H_CREATING ? "...." : HostHasRoom() ? sCode : "----";
        } else {
            switch (sState) {
            case PORT_OUI_H_WAITING:
            case PORT_OUI_H_JOINING:
            case PORT_OUI_H_READY:
                /* selected: the mode, Left / Right change it */
                if (selected) {
                    snprintf(buf, size, "<%s>", sRoomsMode ? "FREE ROAM" : "TOGETHER");
                    return;
                }
                if (sPlayers <= 1 && !sConnecting) { v = "ALONE"; break; }
                snprintf(buf, size, "PLAYERS %d/4%s", sPlayers, sConnecting ? " +" : "");
                return;
            case PORT_OUI_H_NOSERVER: v = "OFFLINE"; break;
            default:                  v = ""; break;
            }
        }
        snprintf(buf, size, "%s", v);
        return;
    }
    if (sub == PORT_OUI_SUB_JOIN) {
        if (row == 0) {
            /* the letters so far, the one being picked between arrows
             * (Left / Right change it), dashes for the rest */
            int i, o = 0;

            for (i = 0; i < 4 && o < size - 4; i++) {
                if (i == sJoinPos && selected && !JoinBusy()) {
                    buf[o++] = '<'; buf[o++] = kAlpha[sJoin[i]]; buf[o++] = '>';
                } else {
                    buf[o++] = i <= sJoinPos || JoinBusy() ? kAlpha[sJoin[i]] : '-';
                }
            }
            buf[o] = '\0';
            return;
        }
        switch (sState) {
        case PORT_OUI_J_CONNECTING: v = "CONNECTING"; break;
        case PORT_OUI_J_WAITING:    v = "WAITING"; break;
        case PORT_OUI_J_STARTING:   v = "JOINING"; break;
        case PORT_OUI_J_NOGAME:     v = "NO SUCH GAME"; break;
        case PORT_OUI_J_FULL:       v = "GAME FULL"; break;
        case PORT_OUI_J_BUILD:      v = "OTHER BUILD"; break;
        case PORT_OUI_J_ROM:        v = "OTHER ROM"; break;
        case PORT_OUI_J_FAILED:     v = "FAILED"; break;
        case PORT_OUI_J_HOSTLEFT:   v = "HOST LEFT"; break;
        default:                    v = ""; break;
        }
        snprintf(buf, size, "%s", v);
        return;
    }
    snprintf(buf, size, "%s", v);
}

/* Description lines: letters the game's description pictures have where
 * possible (no J, Q, Z, digits -- platform/menu_poc.c kDescCuts); a line
 * with others is drawn in the fallback font. */
const char *PortOuiRowDesc(int sub, int row)
{
    /* server browser */
    if (sub == PORT_OUI_SUB_ONLINE && row == 2)
        return "FIND A PUBLIC GAME TO JOIN";
    if (sub >= PORT_OUI_SUB_BROWSE || (sub == PORT_OUI_SUB_HOST && row >= 2))
        return PortObrRowDesc(sub, row);
    if (sub == PORT_OUI_SUB_ONLINE)
        return row == 0 ? "MAKE A CODE FOR A FRIEND" : "ENTER A FRIENDS CODE";
    if (sub == PORT_OUI_SUB_HOST) {
        if (sNote == PORT_OUI_NOTE_COPIED) return "INVITE LINK COPIED";
        if (sNote == PORT_OUI_NOTE_NOCOPY) return "TELL YOUR FRIEND THE CODE";
        switch (sState) {
        case PORT_OUI_H_CREATING: return "MAKING A GAME CODE";
        case PORT_OUI_H_NOSERVER: return "NO GAME SERVER - TRY AGAIN";
        case PORT_OUI_H_JOINING:  return "A PLAYER IS CONNECTING";
        case PORT_OUI_H_READY:
            if (row == 0)
                return sPlayers >= 4 ? "FOUR PLAYERS - THE GAME IS FULL"
                     : sPlayers == 3 ? "THREE PLAYERS ARE HERE" : "TWO PLAYERS ARE HERE";
            return sRoomsMode ? "EACH PLAYS ON - MEET BY PHONE" : "PRESS A TO PLAY TOGETHER";
        default:
            if (row == 1 && HostHasRoom())
                return sRoomsMode ? "EACH PLAYS ON - MEET BY PHONE" : "ONE ROOM FOR EVERYONE";
            return row == 0 ? "A COPIES AN INVITE LINK" : "A FRIEND CAN COME IN LATER";
        }
    }
    if (sub == PORT_OUI_SUB_JOIN) {
        switch (sState) {
        case PORT_OUI_J_CONNECTING: return "CONNECTING TO THE HOST";
        case PORT_OUI_J_WAITING:    return "WAITING FOR THE HOST";
        case PORT_OUI_J_STARTING:   return "GETTING THE GAME";
        case PORT_OUI_J_NOGAME:     return "CHECK THE CODE";
        case PORT_OUI_J_FULL:       return "THAT GAME HAS FOUR PLAYERS";
        case PORT_OUI_J_BUILD:      return "REFRESH THE PAGE";
        case PORT_OUI_J_ROM:        return "YOU BOTH NEED THE SAME ROM";
        case PORT_OUI_J_FAILED:     return "NO CONNECTION - TRY AGAIN";
        case PORT_OUI_J_HOSTLEFT:   return "THE HOST CLOSED THE GAME";
        default:
            return row == 0 ? "LEFT RIGHT PICK - A NEXT" : "PRESS A TO PLAY";
        }
    }
    return "";
}

int PortOuiRowUnavailable(int sub, int row)
{
    if (gPortLnOn)
        return 1;
    if (sub >= PORT_OUI_SUB_BROWSE || (sub == PORT_OUI_SUB_HOST && row >= 2))
        return PortObrRowUnavailable(sub, row);     /* server browser */
    if (sub == PORT_OUI_SUB_HOST && row == 1)
        return !HostHasRoom();
    if (sub == PORT_OUI_SUB_JOIN && row == 1)
        return JoinBusy();
    return 0;
}

int PortOuiBusy(void)
{
    return !gPortLnOn && sSub != PORT_OUI_SUB_MP;
}

static void Go(int sub)
{
    sSub = sub;
    sNote = 0;
    sGen++;
}

static void JoinStart(int *row)
{
    sJoinPos = 3;
    *row = 1;
    sState = PORT_OUI_J_CONNECTING;
    sGen++;
    Push(PORT_OUI_EV_JOIN);
}

/* server browser: JOIN GAME with this code, as if typed and joined */
void PortOuiJoinWith(const char code[4])
{
    int i, k;

    for (i = 0; i < 4; i++) {
        for (k = 0; k < NALPHA && kAlpha[k] != code[i]; k++)
            ;
        sJoin[i] = k < NALPHA ? k : 0;
    }
    Go(PORT_OUI_SUB_JOIN);
}

int PortOuiKey(int *row, unsigned key)
{
    int act;            /* server browser */
    char code[4];

    if (gPortLnOn)
        return PORT_OUI_K_NONE;
    switch (sSub) {
    case PORT_OUI_SUB_MP:
        if (key == 1 && *row == 1) {
            Go(PORT_OUI_SUB_ONLINE);
            *row = 0;
            return PORT_OUI_K_SELECT;
        }
        return PORT_OUI_K_NONE;

    case PORT_OUI_SUB_ONLINE:
        if (key == 2) {
            Go(PORT_OUI_SUB_MP);
            *row = 1;
            return PORT_OUI_K_BACK;
        }
        if (key != 1)
            return PORT_OUI_K_NONE;
        if (*row == 2) {                    /* server browser */
            Go(PORT_OUI_SUB_BROWSE);
            PortObrOpen();
        } else if (*row == 0) {
            Go(PORT_OUI_SUB_HOST);
            sState = PORT_OUI_H_CREATING;
            Push(PORT_OUI_EV_HOST_OPEN);
        } else {
            Go(PORT_OUI_SUB_JOIN);
            sState = PORT_OUI_IDLE;
            sJoinPos = 0;
        }
        *row = 0;
        return PORT_OUI_K_SELECT;

    case PORT_OUI_SUB_HOST:
        /* server browser: GAME (PRIVATE / PUBLIC) and PASSWORD */
        if (*row >= 2 && (act = PortObrHostKey(row, key)) != PORT_OUI_K_NONE)
            return act;
        if (key == 2) {
            Push(PORT_OUI_EV_HOST_CLOSE);
            sState = PORT_OUI_IDLE;
            Go(PORT_OUI_SUB_ONLINE);
            *row = 0;
            return PORT_OUI_K_BACK;
        }
        if ((key == 5 || key == 6) && *row == 1) {
            if (!HostHasRoom())
                return PORT_OUI_K_DENY;
            sRoomsMode = !sRoomsMode;
            sGen++;
            return PORT_OUI_K_CURSOR;
        }
        if (key != 1 || *row >= 2)          /* server browser: rows 2, 3 */
            return PORT_OUI_K_NONE;
        if (*row == 0) {
            if (!HostHasRoom())
                return PORT_OUI_K_DENY;
            Push(PORT_OUI_EV_COPY_LINK);
            return PORT_OUI_K_SELECT;
        }
        if (!HostHasRoom())
            return PORT_OUI_K_DENY;
        sStartPending = 1;
        return PORT_OUI_K_START;

    case PORT_OUI_SUB_JOIN:
        if (JoinBusy()) {
            if (key == 2) {
                Push(PORT_OUI_EV_JOIN_CANCEL);
                sState = PORT_OUI_IDLE;
                sJoinPos = 3;
                *row = 0;
                sGen++;
                return PORT_OUI_K_BACK;
            }
            return PORT_OUI_K_QUIET;        /* nothing else while connecting */
        }
        if (*row == 0) {
            switch (key) {
            case 5: case 6:
                sJoin[sJoinPos] = (sJoin[sJoinPos] + (key == 6 ? 1 : NALPHA - 1)) % NALPHA;
                sState = PORT_OUI_IDLE;
                sGen++;
                return PORT_OUI_K_CURSOR;
            case 1:
                if (sJoinPos < 3) {
                    sJoinPos++;
                    sGen++;
                    return PORT_OUI_K_CURSOR;
                }
                JoinStart(row);
                return PORT_OUI_K_SELECT;
            case 2:
                if (sJoinPos > 0) {
                    sJoinPos--;
                    sGen++;
                    return PORT_OUI_K_CURSOR;
                }
                sState = PORT_OUI_IDLE;
                Go(PORT_OUI_SUB_ONLINE);
                *row = 1;
                return PORT_OUI_K_BACK;
            default:
                return PORT_OUI_K_NONE;
            }
        }
        if (key == 1) {
            JoinStart(row);
            return PORT_OUI_K_SELECT;
        }
        if (key == 2) {
            sState = PORT_OUI_IDLE;
            Go(PORT_OUI_SUB_ONLINE);
            *row = 1;
            return PORT_OUI_K_BACK;
        }
        return key == 5 || key == 6 ? PORT_OUI_K_QUIET : PORT_OUI_K_NONE;

    /* server browser (platform/online_browse.c) */
    case PORT_OUI_SUB_BROWSE:
        act = PortObrBrowseKey(row, key, code);
        if (act == PORT_OUI_K_BACK) {
            Go(PORT_OUI_SUB_ONLINE);
            *row = 2;
        } else if (act == PORT_OUI_K_SELECT) {
            PortOuiJoinWith(code);          /* JOIN GAME's states from here */
            JoinStart(row);
        }
        return act;
    case PORT_OUI_SUB_PASS:
        act = PortObrPassKey(row, key, sState);
        if (act == PORT_OUI_K_BACK) {       /* cancel: back to JOIN GAME */
            if (JoinBusy() || sState == PORT_OUI_J_PASSWORD || sState == PORT_OUI_J_PWBAD)
                Push(PORT_OUI_EV_JOIN_CANCEL);
            sState = PORT_OUI_IDLE;
            Go(PORT_OUI_SUB_JOIN);
            sJoinPos = 3;
            *row = 0;
        }
        return act;
    }
    return PORT_OUI_K_NONE;
}

void PortOuiFadeDone(void)
{
    if (sStartPending) {
        sStartPending = 0;
        Push(PORT_OUI_EV_HOST_START);
    }
}

#ifdef __cplusplus
}
#endif
