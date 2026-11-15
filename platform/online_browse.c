/* The server browser's screens: platform/port/online_browse.h says what.
 *
 * Front-end host memory, read only by the ONLINE screens while no online
 * session is on (online_ui.c and menu_poc.c check gPortLnOn before they
 * reach here), so no session's rewinds, replays or images ever see it.  The
 * page (web/online_browse.js) does the network: it fills the list, says how
 * the host's listing is doing, reads the host's choices and the passwords
 * typed here.
 *
 * Text is drawn in the screens' fallback font where the game's own letters
 * do not cover it (platform/menu_poc.c), so it keeps to that font's
 * characters: A-Z 0-9 space : . - ! / % + < > and '*', the padlock. */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "port/lmpnet.h"
#include "port/simblock.h"     /* gPortSim: the description's turns */
#include "port/online_ui.h"
#include "port/online_browse.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define OBR_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define OBR_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- the list ---------------------------------------------------------------- */

struct ObrGame {
    char code[5];
    char name[9];
    u8 n, roam, locked, full;
};

static struct ObrGame sGames[PORT_OBR_LIST_MAX];
static int sCount, sStatus = PORT_OBR_LOADING;
static int sSel, sTop;              /* the selected game; the first on screen */
static char sSelCode[5];            /* ... by code, kept across refreshes     */

/* --- the passwords --------------------------------------------------------------- */

static char sHostPass[PORT_OBR_PASS_MAX + 1];
static char sJoinPass[PORT_OBR_PASS_MAX + 1];
static int sHostPublic;             /* HOST GAME's GAME row: 0 PRIVATE, 1 PUBLIC */
static int sHostList;               /* PORT_OBR_H_*, from the page             */

/* The typing state, as the player card's NAME row has it: A on the row
 * starts it; Left/Right pick a letter, A the next one, B the one before,
 * Start (Enter) ends it, and so does leaving the row. */
enum { T_NONE, T_HOST, T_JOIN };
static int sTyping, sTypePos;
static int sNoteSub = -1, sNoteRow = -1;    /* the screen's cursor, each frame */
static int sPassEnter;

/* The pad's letters: space (end of the password), A-Z, 0-9. */
static const char kPassChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
#define NPASS ((int)sizeof(kPassChars) - 1)

static int sEv[4], sEvHead, sEvTail;

static void Push(int ev)
{
    int next = (sEvTail + 1) & 3;

    if (next == sEvHead)
        return;
    sEv[sEvTail] = ev;
    sEvTail = next;
}

static char *TypeBuf(void)
{
    return sTyping == T_HOST ? sHostPass : sTyping == T_JOIN ? sJoinPass : NULL;
}

static void TypeStart(int which)
{
    char *b;

    sTyping = which;
    b = TypeBuf();
    sTypePos = (int)strlen(b);
    if (sTypePos >= PORT_OBR_PASS_MAX)
        sTypePos = PORT_OBR_PASS_MAX - 1;
    PortOuiChanged();
}

static void TypeStop(void)
{
    if (!sTyping)
        return;
    sTyping = T_NONE;
    PortOuiChanged();
}

static void Trim(char *b)
{
    int len = (int)strlen(b);

    while (len > 0 && b[len - 1] == ' ')
        b[--len] = '\0';
}

static void PickStep(int dir)
{
    char *b = TypeBuf();
    int len = (int)strlen(b), i, c;

    while (len < sTypePos) {
        b[len++] = ' ';
        b[len] = '\0';
    }
    c = sTypePos < len ? b[sTypePos] : ' ';
    for (i = 0; i < NPASS && kPassChars[i] != c; i++)
        ;
    if (i >= NPASS)
        i = 0;
    i = (i + dir + NPASS) % NPASS;
    b[sTypePos] = kPassChars[i];
    if (sTypePos >= len)
        b[sTypePos + 1] = '\0';
    Trim(b);
    PortOuiChanged();
}

/* The letters so far, the one being picked between arrows; `empty` when
 * there are none and the row is not being typed. */
static void PassShown(const char *b, int typing, char *buf, int size, const char *empty)
{
    int i, o = 0, len = (int)strlen(b);

    for (i = 0; i < PORT_OBR_PASS_MAX && o < size - 4; i++) {
        char c = i < len ? b[i] : ' ';

        if (typing && i == sTypePos) {
            buf[o++] = '<';
            buf[o++] = c == ' ' ? '-' : c;
            buf[o++] = '>';
        } else if (i < len) {
            buf[o++] = c == ' ' ? '-' : c;
        }
    }
    buf[o] = '\0';
    if (o == 0)
        snprintf(buf, size, "%s", empty);
}

/* One key on a password row being typed: PORT_OUI_K_*, or K_NONE when the
 * key is not the typing state's. */
static int TypeKey(int *row, unsigned key, int nextRow)
{
    char *b = TypeBuf();

    if (key == 1 && (gPressedKeys & START_BUTTON) && !(gPressedKeys & A_BUTTON)) {
        TypeStop();                 /* Start: done */
        return PORT_OUI_K_SELECT;
    }
    switch (key) {
    case 5: case 6:
        PickStep(key == 6 ? 1 : -1);
        return PORT_OUI_K_CURSOR;
    case 1:
        if (sTypePos < PORT_OBR_PASS_MAX - 1 && sTypePos < (int)strlen(b)) {
            sTypePos++;
            PortOuiChanged();
            return PORT_OUI_K_CURSOR;
        }
        TypeStop();
        if (nextRow >= 0)
            *row = nextRow;
        return PORT_OUI_K_SELECT;
    case 2:
        if (sTypePos > 0)
            sTypePos--;
        else
            sTyping = T_NONE;       /* B at the first letter: done */
        PortOuiChanged();
        return PORT_OUI_K_CURSOR;
    default:
        return PORT_OUI_K_NONE;
    }
}

/* --- the list's model -------------------------------------------------------------- */

/* The fallback font's ink width (menu_poc.c CellWidth), near enough to
 * decide what fits on a row. */
static int TextW(const char *s)
{
    int w = 0, first = 1;

    for (; *s; s++) {
        int g;

        if (*s == ' ') { w += 4; continue; }
        g = *s == 'M' || *s == 'W' ? 10 : *s == 'N' ? 8
          : *s == 'I' || *s == ':' || *s == '.' || *s == '!' ? 2 : *s == '<' || *s == '>' ? 4 : 6;
        if (!first) w += 2;
        first = 0;
        w += g;
    }
    return w;
}

#define ROW_TEXT_W (192 - 55 - 6)   /* menu_poc.c: LBL_TEXT_LEFT .. ROW_VALUE_X1, a gap */

static void Clamp(void)
{
    if (sSel >= sCount) sSel = sCount - 1;
    if (sSel < 0) sSel = 0;
    if (sTop > sSel) sTop = sSel;
    if (sSel >= sTop + PORT_OBR_VIS) sTop = sSel - PORT_OBR_VIS + 1;
    if (sTop > sCount - PORT_OBR_VIS) sTop = sCount - PORT_OBR_VIS;
    if (sTop < 0) sTop = 0;
}

static const struct ObrGame *GameAt(int row)
{
    int i = sTop + row;

    return sCount > 0 && i >= 0 && i < sCount ? &sGames[i] : NULL;
}

static const char *StatusName(void)
{
    switch (sStatus) {
    case PORT_OBR_LOADING: return "LOADING";
    case PORT_OBR_NOCONN:  return "NO CONNECTION";
    case PORT_OBR_OLD:     return "NOT AVAILABLE";
    case PORT_OBR_BUSY:    return "BUSY";
    default:               return "NO GAMES";
    }
}

static const char *StatusDesc(void)
{
    switch (sStatus) {
    case PORT_OBR_LOADING: return "LOOKING FOR PUBLIC GAMES";
    case PORT_OBR_NOCONN:  return "CANNOT REACH THE GAME SERVER";
    case PORT_OBR_OLD:     return "SERVER NEEDS AN UPDATE";
    case PORT_OBR_BUSY:    return "TOO MANY TRIES - WAIT A BIT";
    default:               return "NONE YET - HOST ONE AS PUBLIC";
    }
}

/* A description that changes with the list: two buffers, so the screen,
 * which redraws when the pointer changes, sees each new text. */
static const char *Dyn(const char *text)
{
    static char buf[2][40];
    static int cur;

    if (strcmp(buf[cur], text) == 0)
        return buf[cur];
    cur ^= 1;
    snprintf(buf[cur], sizeof(buf[cur]), "%s", text);
    return buf[cur];
}

void PortObrOpen(void)
{
    sCount = 0;
    sStatus = PORT_OBR_LOADING;
    sSel = sTop = 0;
    sSelCode[0] = '\0';
    PortOuiChanged();
}

int PortObrRows(void)
{
    if (sCount <= 0)
        return 1;
    return sCount < PORT_OBR_VIS ? sCount : PORT_OBR_VIS;
}

int PortObrFollowRow(int sub, int row)
{
    if (sub == PORT_OUI_SUB_BROWSE) {
        Clamp();
        row = sCount > 0 ? sSel - sTop : 0;
    } else if (sub == PORT_OUI_SUB_PASS && sPassEnter) {
        sPassEnter = 0;
        row = 0;
    }
    /* leaving a password row ends its typing state */
    if (sTyping && (sub != sNoteSub || row != sNoteRow))
        TypeStop();
    sNoteSub = sub;
    sNoteRow = row;
    return row;
}

void PortObrPassOpened(void)
{
    sPassEnter = 1;
    sNoteSub = PORT_OUI_SUB_PASS;
    sNoteRow = 0;
    /* straight into typing, after what was typed before (a wrong try) */
    TypeStart(T_JOIN);
}

static const char *ModeWord(const struct ObrGame *g) { return g->roam ? "FREE ROAM" : "TOGETHER"; }

const char *PortObrRowName(int sub, int row)
{
    static char name[12];

    /* "PASS", not PASSWORD: an 8-letter value and its arrows must fit */
    if (sub == PORT_OUI_SUB_HOST)
        return row == 2 ? "GAME" : "PASS";
    if (sub == PORT_OUI_SUB_PASS)
        return row == 0 ? "PASS" : "JOIN";
    if (sub == PORT_OUI_SUB_BROWSE) {
        const struct ObrGame *g = GameAt(row);

        if (!g)
            return StatusName();
        snprintf(name, sizeof(name), "%s", g->name);
        return name;
    }
    return "";
}

static void BrowseValue(int row, char *buf, int size)
{
    const struct ObrGame *g = GameAt(row);
    char base[24];
    const char *more = "";

    buf[0] = '\0';
    if (!g)
        return;
    if (row == PORT_OBR_VIS - 1 && sTop + PORT_OBR_VIS < sCount)
        more = " v";
    else if (row == 0 && sTop > 0)
        more = " ^";
    if (g->full)
        snprintf(base, sizeof(base), "%sFULL", g->locked ? "* " : "");
    else
        snprintf(base, sizeof(base), "%s%d/4", g->locked ? "* " : "", g->n);
    /* the mode where it fits beside the name (the description has it too) */
    snprintf(buf, size, "%s %s%s", base, g->roam ? "ROAM" : "TOGETHER", more);
    if (TextW(g->name) + 8 + TextW(buf) > ROW_TEXT_W)
        snprintf(buf, size, "%s %s%s", base, g->roam ? "ROAM" : "", more);
    if (TextW(g->name) + 8 + TextW(buf) > ROW_TEXT_W)
        snprintf(buf, size, "%s%s", base, more);
}

static const char *JoinStateWord(int st)
{
    switch (st) {
    case PORT_OUI_J_CONNECTING: return "CHECKING";
    case PORT_OUI_J_WAITING:    return "WAITING";
    case PORT_OUI_J_STARTING:   return "JOINING";
    case PORT_OUI_J_NOGAME:     return "NO SUCH GAME";
    case PORT_OUI_J_FULL:       return "GAME FULL";
    case PORT_OUI_J_BUILD:      return "OTHER BUILD";
    case PORT_OUI_J_ROM:        return "OTHER ROM";
    case PORT_OUI_J_FAILED:     return "FAILED";
    case PORT_OUI_J_HOSTLEFT:   return "HOST LEFT";
    case PORT_OUI_J_PWBAD:      return "WRONG";
    case PORT_OUI_J_PWLOCK:     return "REFUSED";
    default:                    return "";
    }
}

void PortObrRowValue(int sub, int row, int selected, char *buf, int size)
{
    buf[0] = '\0';
    if (sub == PORT_OUI_SUB_HOST) {
        if (row == 2)
            snprintf(buf, size, selected ? "<%s>" : "%s", sHostPublic ? "PUBLIC" : "PRIVATE");
        else
            PassShown(sHostPass, selected && sTyping == T_HOST, buf, size, "NONE");
        return;
    }
    if (sub == PORT_OUI_SUB_PASS) {
        if (row == 0)
            PassShown(sJoinPass, selected && sTyping == T_JOIN, buf, size, "----");
        else
            snprintf(buf, size, "%s", JoinStateWord(PortOuiScreen() & 255));
        return;
    }
    if (sub == PORT_OUI_SUB_BROWSE)
        BrowseValue(row, buf, size);
}

/* Description lines: at most about 30 characters (the line is 240 px). */
const char *PortObrRowDesc(int sub, int row)
{
    char t[48];

    if (sub == PORT_OUI_SUB_HOST) {
        if (row == 2) {
            if (!sHostPublic)
                return "ONLY PLAYERS WITH THE CODE";
            switch (sHostList) {
            case PORT_OBR_H_OLD:    return "SERVER UPDATE NEEDED TO LIST";
            case PORT_OBR_H_NOCONN: return "NOT LISTED - NO CONNECTION";
            case PORT_OBR_H_LIMIT:  return "NOT LISTED - TOO MANY GAMES";
            default:
                /* the privacy note, in turn with what PUBLIC is: players'
                 * browsers connect to each other directly */
                return (gPortSim->frame.frameCount / 150) & 1 ? "STRANGERS CAN SEE YOUR IP"
                     : sHostList == PORT_OBR_H_LISTED ? "LISTED - ANYONE CAN JOIN"
                                                       : "PUBLIC - ANYONE CAN JOIN";
            }
        }
        if (sTyping == T_HOST)
            return "TYPE OR PICK - START DONE";
        return sHostPass[0] ? "JOINERS MUST TYPE IT" : "A TO SET A PASSWORD";
    }
    if (sub == PORT_OUI_SUB_PASS) {
        int st = PortOuiScreen() & 255;

        if (row == 0 && sTyping == T_JOIN)
            return st == PORT_OUI_J_PWBAD ? "WRONG PASSWORD - TRY AGAIN" : "TYPE OR PICK - START DONE";
        switch (st) {
        case PORT_OUI_J_PASSWORD:   return row == 0 ? "A TO TYPE THE PASSWORD" : "PRESS A TO JOIN";
        case PORT_OUI_J_PWBAD:      return "WRONG PASSWORD - TRY AGAIN";
        case PORT_OUI_J_PWLOCK:     return "TOO MANY WRONG TRIES";
        case PORT_OUI_J_CONNECTING: return "CHECKING THE PASSWORD";
        case PORT_OUI_J_WAITING:    return "WAITING FOR THE HOST";
        case PORT_OUI_J_STARTING:   return "GETTING THE GAME";
        case PORT_OUI_J_FULL:       return "THAT GAME HAS FOUR PLAYERS";
        case PORT_OUI_J_HOSTLEFT:   return "THE HOST CLOSED THE GAME";
        default:                    return "NO CONNECTION - TRY AGAIN";
        }
    }
    if (sub == PORT_OUI_SUB_BROWSE) {
        const struct ObrGame *g = GameAt(row);

        if (!g)
            return StatusDesc();
        if (sStatus == PORT_OBR_NOCONN)
            return "LIST MAY BE OLD - NO CONNECTION";
        snprintf(t, sizeof(t), "%s - %s - %s", g->code, ModeWord(g),
                 g->full ? "FULL" : g->locked ? "PASSWORD" : "A TO JOIN");
        return Dyn(t);
    }
    return "";
}

int PortObrRowUnavailable(int sub, int row)
{
    if (sub == PORT_OUI_SUB_BROWSE) {
        const struct ObrGame *g = GameAt(row);

        return !g || g->full;
    }
    if (sub == PORT_OUI_SUB_PASS && row == 1) {
        int st = PortOuiScreen() & 255;

        return st != PORT_OUI_J_PASSWORD && st != PORT_OUI_J_PWBAD;
    }
    return 0;
}

/* --- keys ------------------------------------------------------------------------ */

int PortObrBrowseKey(int *row, unsigned key, char code[4])
{
    const struct ObrGame *g;

    switch (key) {
    case 2:
        return PORT_OUI_K_BACK;
    case 3: case 4:
        if (sCount <= 1)
            return PORT_OUI_K_QUIET;
        sSel = (sSel + (key == 4 ? 1 : sCount - 1)) % sCount;
        Clamp();
        memcpy(sSelCode, sGames[sSel].code, 5);
        *row = sSel - sTop;
        PortOuiChanged();
        return PORT_OUI_K_CURSOR;
    case 1:
        g = GameAt(*row);
        if (!g || g->full)
            return PORT_OUI_K_DENY;
        memcpy(code, g->code, 4);
        sJoinPass[0] = '\0';        /* a new game: its own password */
        return PORT_OUI_K_SELECT;
    default:
        return key == 5 || key == 6 ? PORT_OUI_K_QUIET : PORT_OUI_K_NONE;
    }
}

int PortObrPassKey(int *row, unsigned key, int joinState)
{
    int ask = joinState == PORT_OUI_J_PASSWORD || joinState == PORT_OUI_J_PWBAD;

    if (*row == 0 && sTyping == T_JOIN) {
        int act = TypeKey(row, key, 1);

        if (act != PORT_OUI_K_NONE)
            return act;
    }
    if (key == 2) {
        TypeStop();
        return PORT_OUI_K_BACK;
    }
    if (key != 1)
        return key == 5 || key == 6 ? PORT_OUI_K_QUIET : PORT_OUI_K_NONE;
    if (*row == 0) {
        if (!ask)
            return PORT_OUI_K_DENY;
        TypeStart(T_JOIN);
        return PORT_OUI_K_SELECT;
    }
    if (!ask || !sJoinPass[0])
        return PORT_OUI_K_DENY;
    Push(PORT_OBR_EV_PW_ANSWER);
    return PORT_OUI_K_SELECT;
}

int PortObrHostKey(int *row, unsigned key)
{
    if (*row == 2) {
        if (key == 5 || key == 6 || key == 1) {
            sHostPublic = !sHostPublic;
            sHostList = PORT_OBR_H_PRIVATE;
            PortOuiChanged();
            return PORT_OUI_K_CURSOR;
        }
        return PORT_OUI_K_NONE;
    }
    if (*row != 3)
        return PORT_OUI_K_NONE;
    if (sTyping == T_HOST) {
        int act = TypeKey(row, key, -1);

        if (act != PORT_OUI_K_NONE)
            return act;
    }
    if (key == 1) {
        TypeStart(T_HOST);
        return PORT_OUI_K_SELECT;
    }
    return key == 5 || key == 6 ? PORT_OUI_K_QUIET : PORT_OUI_K_NONE;
}

/* --- the page's side ----------------------------------------------------------------- */

/* "CODE\tNAME\tN\tFLAGS\n" per game; FLAGS bit 0 locked, 1 free roam, 2 full. */
OBR_EXPORT void PortObrListSet(const char *packed)
{
    const char *p = packed;
    int n = 0, i;

    while (p && *p && n < PORT_OBR_LIST_MAX) {
        struct ObrGame g;
        char f[4][16];
        int k = 0, o = 0, flags;

        memset(f, 0, sizeof(f));
        for (; *p && *p != '\n'; p++) {
            if (*p == '\t') { if (++k >= 4) break; o = 0; continue; }
            if (o < 15) f[k][o++] = *p;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (strlen(f[0]) != 4)
            continue;
        memset(&g, 0, sizeof(g));
        memcpy(g.code, f[0], 4);
        /* the server sanitised it; again, to this font's letters */
        for (i = 0, o = 0; f[1][i] && o < 8; i++) {
            char c = f[1][i];

            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' || c == '!')
                g.name[o++] = c;
        }
        if (!o)
            snprintf(g.name, sizeof(g.name), "PLAYER");
        g.n = (u8)(f[2][0] >= '1' && f[2][0] <= '4' ? f[2][0] - '0' : 1);
        flags = f[3][0] >= '0' && f[3][0] <= '9' ? f[3][0] - '0' : 0;
        g.locked = (u8)(flags & 1);
        g.roam = (u8)((flags >> 1) & 1);
        g.full = (u8)(((flags >> 2) & 1) || g.n >= 4);
        sGames[n++] = g;
    }
    sCount = n;
    /* the cursor stays on the game it was on, wherever that went */
    for (i = 0; sSelCode[0] && i < sCount; i++)
        if (strcmp(sGames[i].code, sSelCode) == 0) {
            int slot = sSel - sTop;

            sSel = i;
            sTop = i - slot;
            break;
        }
    Clamp();
    if (sCount > 0)
        memcpy(sSelCode, sGames[sSel].code, 5);
    PortOuiChanged();
}

OBR_EXPORT void PortObrListStatus(int status)
{
    if (status == sStatus)
        return;
    sStatus = status;
    PortOuiChanged();
}

OBR_EXPORT int PortObrBrowsing(void)
{
    return !gPortLnOn && PortOuiSub() == PORT_OUI_SUB_BROWSE;
}

OBR_EXPORT void PortObrSetHostList(int state)
{
    if (state == sHostList)
        return;
    sHostList = state;
    PortOuiChanged();
}

/* Tests: the selected game's code ("" with none). */
OBR_EXPORT const char *PortObrSelectedJs(void) { return sCount > 0 ? sSelCode : ""; }

OBR_EXPORT int PortObrHostPublic(void) { return sHostPublic; }
OBR_EXPORT const char *PortObrHostPassJs(void) { return sHostPass; }
OBR_EXPORT const char *PortObrPassJs(void) { return sJoinPass; }

OBR_EXPORT int PortObrPoll(void)
{
    int ev;

    if (sEvHead == sEvTail)
        return 0;
    ev = sEv[sEvHead];
    sEvHead = (sEvHead + 1) & 3;
    return ev;
}

OBR_EXPORT int PortObrTyping(void)
{
    return !gPortLnOn && sTyping != T_NONE;
}

OBR_EXPORT int PortObrWantsText(void)
{
    if (gPortLnOn)
        return 0;
    return (PortOuiSub() == PORT_OUI_SUB_HOST && sNoteSub == PORT_OUI_SUB_HOST && sNoteRow == 3)
        || (PortOuiSub() == PORT_OUI_SUB_PASS && sNoteSub == PORT_OUI_SUB_PASS && sNoteRow == 0);
}

OBR_EXPORT void PortObrTypingEnd(void)
{
    TypeStop();
}

OBR_EXPORT void PortObrTypeChar(int c)
{
    char *b = TypeBuf();
    int len;

    if (!b)
        return;
    len = (int)strlen(b);
    if (c == 8) {
        if (len > 0)
            b[len - 1] = '\0';
    } else {
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) || len >= PORT_OBR_PASS_MAX)
            return;
        b[len++] = (char)c;
        b[len] = '\0';
    }
    len = (int)strlen(b);
    sTypePos = len < PORT_OBR_PASS_MAX ? len : PORT_OBR_PASS_MAX - 1;
    PortOuiChanged();
}

/* A phone's prompt: the whole password at once, for the row the cursor is on. */
OBR_EXPORT void PortObrSetTextJs(const char *s)
{
    char *b;
    int i, o = 0;

    if (!PortObrWantsText() || s == NULL)
        return;
    b = PortOuiSub() == PORT_OUI_SUB_HOST ? sHostPass : sJoinPass;
    for (i = 0; s[i] && o < PORT_OBR_PASS_MAX; i++) {
        char c = s[i];

        if (c >= 'a' && c <= 'z')
            c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            b[o++] = c;
    }
    b[o] = '\0';
    sTyping = T_NONE;
    PortOuiChanged();
}

#ifdef __cplusplus
}
#endif
