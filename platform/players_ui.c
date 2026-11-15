/* The PLAYERS page: platform/port/players_ui.h says what.
 *
 * Host memory only.  What it shows is a snapshot of the host's devices --
 * the page's (web/input_devices.js, pushed through PortPui*) or, natively,
 * platform/input_devices.c's, read here -- and what it does is ask the host
 * to move devices (a queue the page polls; natively the core's own calls).
 * The page is never reachable in an online session (menu_poc.c dims the
 * row), so none of this is in an image, a keyframe or a hash. */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "port/localmp.h"
#include "port/lmpnet.h"
#include "port/online_ui.h"
#include "port/players_ui.h"
#include "port/simblock.h"
#ifndef __EMSCRIPTEN__
#include "port/input_devices.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KIND_KEYS_A 1
#define KIND_KEYS_B 2
#define KIND_TOUCH  3
#define KIND_PAD    4
#define DEV_FOLLOW  (-1)
#define DEV_PARKED  (-2)
#define DEV_OFF     (-3)

struct PuiDev {
    int id, kind, num, player, flags;
    char name[PORT_PUI_NAME];
};
struct PuiSnap {
    int nDev;
    struct PuiDev dev[PORT_PUI_MAX_DEV];
    int plFlags[4], plMain[4];
};

static struct PuiSnap sSnap, sNext;
static char sNameBuf[PORT_PUI_NAME];
static int sActive;
static int sChoice[4] = { -1, -1, -1, -1 };   /* candidate index, -1 = none */
static u32 sGen = 1;
static int sOps[16], sOpHead, sOpTail;
static int (*sCardHook)(int slot);
static char sDesc[64];

/* --- the host's side ------------------------------------------------------------ */

void PortPuiBegin(void)
{
    memset(&sNext, 0, sizeof sNext);
}

char *PortPuiNameBuf(void)
{
    return sNameBuf;
}

void PortPuiAddDevice(int id, int kind, int num, int player, int flags)
{
    struct PuiDev *d;

    if (sNext.nDev >= PORT_PUI_MAX_DEV)
        return;
    d = &sNext.dev[sNext.nDev++];
    d->id = id;
    d->kind = kind;
    d->num = num;
    d->player = player;
    d->flags = flags;
    sNameBuf[PORT_PUI_NAME - 1] = '\0';
    snprintf(d->name, sizeof d->name, "%s", sNameBuf);
    sNameBuf[0] = '\0';
}

void PortPuiSetPlayer(int p, int flags, int mainDev)
{
    if (p < 0 || p > 3)
        return;
    sNext.plFlags[p] = flags;
    sNext.plMain[p] = mainDev;
}

void PortPuiEnd(void)
{
    if (memcmp(&sNext, &sSnap, sizeof sSnap) != 0) {
        sSnap = sNext;
        sGen++;
    }
}

int PortPuiPoll(void)
{
    int op;

    if (sOpHead == sOpTail)
        return 0;
    op = sOps[sOpHead];
    sOpHead = (sOpHead + 1) & 15;
    return op;
}

int PortPuiActive(void)
{
    return sActive;
}

void PortPuiSetCardHook(int (*open)(int slot))
{
    sCardHook = open;
}

#ifndef __EMSCRIPTEN__
/* Natively the device core is in this program: read it, and act on it. */
static void NativeRefresh(void)
{
    struct PortDevInfo dev[PORT_DEV_MAX];
    struct PortDevPlayerInfo pl[4];
    int n = PortDevList(dev, PORT_DEV_MAX), i, p, pads = 0, p1 = PortDevPlayer1Device();
    int multi = 0;

    PortDevListPlayers(pl);
    for (p = 1; p < 4; p++)
        if (pl[p].human)
            multi = 1;
    PortPuiBegin();
    for (i = 0; i < n; i++) {
        int flags = dev[i].present ? PORT_PUI_DEV_PRESENT : 0;

        if (dev[i].kind == KIND_PAD)
            pads++;
        if (dev[i].present && (dev[i].player == DEV_PARKED
                               || (multi && dev[i].kind == KIND_PAD && dev[i].player == DEV_FOLLOW
                                   && dev[i].id != p1)))
            flags |= PORT_PUI_DEV_WAITING;
        snprintf(sNameBuf, sizeof sNameBuf, "%s", dev[i].name);
        PortPuiAddDevice(dev[i].id, dev[i].kind, dev[i].kind == KIND_PAD ? pads : 0, dev[i].player, flags);
    }
    for (p = 0; p < 4; p++) {
        int mainDev = pl[p].numDevices ? pl[p].devices[0] : 0;

        if (p == 0 && p1)
            for (i = 0; i < pl[0].numDevices; i++)
                if (pl[0].devices[i] == p1)
                    mainDev = p1;
        PortPuiSetPlayer(p, (pl[p].human ? PORT_PUI_PL_HUMAN : 0) | (pl[p].pending ? PORT_PUI_PL_PENDING : 0)
                            | (pl[p].graceLeft ? PORT_PUI_PL_GRACE : 0), mainDev);
    }
    PortPuiEnd();
}

static int DevHasOther(int player, int except)
{
    int i;

    for (i = 0; i < sSnap.nDev; i++)
        if (sSnap.dev[i].player == player && sSnap.dev[i].id != except)
            return 1;
    return 0;
}

static void Do(int op, int player, int dev)
{
    int i, from = -9;

    for (i = 0; i < sSnap.nDev; i++)
        if (sSnap.dev[i].id == dev)
            from = sSnap.dev[i].player;
    switch (op) {
    case PORT_PUI_OP_ASSIGN:
        PortDevAssign(dev, player);
        if (from >= 1 && from != player && !DevHasOther(from, dev))
            PortDevDropOut(from);
        break;
    case PORT_PUI_OP_DROP:
        PortDevDropOut(player);
        break;
    case PORT_PUI_OP_ANY:
        for (i = 0; i < sSnap.nDev; i++)
            if (sSnap.dev[i].player == 0)
                PortDevAssign(sSnap.dev[i].id, DEV_FOLLOW);
        break;
    }
    NativeRefresh();
}
#else
static void Do(int op, int player, int dev)
{
    int next = (sOpTail + 1) & 15;

    if (next == sOpHead)
        return;             /* the page is not polling: drop, never block */
    sOps[sOpTail] = op | (player << 8) | (dev << 16);
    sOpTail = next;
}
#endif

/* --- what the rows say --------------------------------------------------------------- */

static const struct PuiDev *Dev(int id)
{
    int i;

    for (i = 0; i < sSnap.nDev; i++)
        if (sSnap.dev[i].id == id)
            return &sSnap.dev[i];
    return NULL;
}

static const char *Label(const struct PuiDev *d, char *buf, int size)
{
    if (d == NULL)
        snprintf(buf, size, "CPU");
    else if (d->kind == KIND_KEYS_A)
        snprintf(buf, size, "KEYBOARD");
    else if (d->kind == KIND_KEYS_B)
        snprintf(buf, size, "KEYS 2");
    else if (d->kind == KIND_TOUCH)
        snprintf(buf, size, "TOUCH");
    else
        snprintf(buf, size, "PAD %d", d->num);
    return buf;
}

/* A device's long name in what the fallback font can draw. */
static void LongName(const struct PuiDev *d, char *buf, int size)
{
    const char *s;
    int o = 0;

    if (d == NULL) { snprintf(buf, size, "THE CPU PLAYS"); return; }
    switch (d->kind) {
    case KIND_KEYS_A: snprintf(buf, size, "KEYBOARD: WASD J K ENTER"); return;
    case KIND_KEYS_B: snprintf(buf, size, "KEYS 2: ARROWS + NUMPAD"); return;
    case KIND_TOUCH:  snprintf(buf, size, "TOUCH BUTTONS"); return;
    default: break;
    }
    o = snprintf(buf, size, "PAD %d: ", d->num);
    /* what fits the description line in the fallback font: about 26 */
    for (s = d->name; *s && o < size - 1 && o < 26; s++) {
        char c = *s;

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.'
            || c == '+' || c == '/')
            buf[o++] = c;
        else if (o > 0 && buf[o - 1] != ' ')
            buf[o++] = ' ';
    }
    /* cut short: at the last whole word */
    if (*s) {
        int w = o;

        while (w > 0 && buf[w - 1] != ' ' && buf[w - 1] != ':')
            w--;
        if (w > 8)
            o = w;
    }
    while (o > 0 && buf[o - 1] == ' ')
        o--;
    buf[o] = '\0';
}

/* Row p's choices: 0 = CPU (P2-P4) or ANY (P1), then every device present
 * and not switched off, in the host's order. */
static int Candidates(int p, int *ids, int max)
{
    int i, n = 0;

    (void)p;
    ids[n++] = 0;
    for (i = 0; i < sSnap.nDev && n < max; i++)
        if ((sSnap.dev[i].flags & PORT_PUI_DEV_PRESENT) && sSnap.dev[i].player != DEV_OFF)
            ids[n++] = sSnap.dev[i].id;
    return n;
}

/* Row p's current choice index (-1: a person without a controller here). */
static int Current(int p, const int *ids, int n)
{
    int i, claimed = 0;

    if (p >= 1 && !(sSnap.plFlags[p] & PORT_PUI_PL_HUMAN))
        return 0;
    if (p == 0) {
        for (i = 0; i < sSnap.nDev; i++)
            if (sSnap.dev[i].player == 0 && (sSnap.dev[i].flags & PORT_PUI_DEV_PRESENT))
                claimed = sSnap.dev[i].id;
        if (!claimed)
            return 0;
    }
    for (i = 1; i < n; i++)
        if (ids[i] == sSnap.plMain[p])
            return i;
    for (i = 1; i < n; i++)
        if (ids[i] == claimed)
            return i;
    return -1;
}

static int Phase(void)
{
    return (int)((gPortSim->frame.frameCount / 150) & 1);
}

void PortPuiOpen(void)
{
    int p;

    sActive = 1;
    for (p = 0; p < 4; p++)
        sChoice[p] = -1;
    sGen++;
#ifndef __EMSCRIPTEN__
    NativeRefresh();
#endif
}

void PortPuiClose(void)
{
    sActive = 0;
}

u32 PortPuiGen(void)
{
#ifndef __EMSCRIPTEN__
    if (sActive)
        NativeRefresh();
#endif
    return sGen * 2u + (u32)Phase();
}

const char *PortPuiRowName(int row)
{
    static const char *const kName[4] = { "P1", "P2", "P3", "P4" };

    return row >= 0 && row < 4 ? kName[row] : "";
}

/* Controllers present on player p. */
static int Count(int p)
{
    int i, n = 0;

    for (i = 0; i < sSnap.nDev; i++)
        if (sSnap.dev[i].player == p && (sSnap.dev[i].flags & PORT_PUI_DEV_PRESENT))
            n++;
    return n;
}

void PortPuiRowValue(int row, int selected, char *buf, int size)
{
    int ids[PORT_PUI_MAX_DEV + 1], n, cur, f;
    char lab[24];

    buf[0] = '\0';
    if (row < 0 || row > 3)
        return;
    n = Candidates(row, ids, PORT_PUI_MAX_DEV + 1);
    cur = Current(row, ids, n);
    f = sSnap.plFlags[row];
    if (selected && sChoice[row] >= 0 && sChoice[row] < n) {
        const int id = ids[sChoice[row]];

        if (id == 0)
            snprintf(lab, sizeof lab, "%s", row == 0 ? "ANY" : "CPU");
        else
            Label(Dev(id), lab, sizeof lab);
        snprintf(buf, size, "<%s>", lab);
        return;
    }
    if (row >= 1 && (f & PORT_PUI_PL_PENDING) && !(f & PORT_PUI_PL_HUMAN)) {
        snprintf(buf, size, "JOINING");
        return;
    }
    if (row >= 1 && (f & PORT_PUI_PL_GRACE)) {
        snprintf(buf, size, "UNPLUGGED");
        return;
    }
    if (row >= 1 && !(f & PORT_PUI_PL_HUMAN)) {
        snprintf(buf, size, "CPU");
        return;
    }
    if (cur == -1 && sSnap.plMain[row] == 0) {
        snprintf(buf, size, row == 0 ? "ANY" : "NO PAD");
        return;
    }
    Label(Dev(sSnap.plMain[row] ? sSnap.plMain[row] : (cur > 0 ? ids[cur] : 0)), buf, size);
    if (row == 0 && cur <= 0 && sSnap.plMain[0] == 0)
        snprintf(buf, size, "ANY");
    else if (row >= 1 && Count(row) > 1 && (int)strlen(buf) < size - 2)
        strcat(buf, "+");       /* and another controller */
}

static int AnyWaiting(void)
{
    int i;

    for (i = 0; i < sSnap.nDev; i++)
        if (sSnap.dev[i].flags & PORT_PUI_DEV_WAITING)
            return 1;
    return 0;
}

const char *PortPuiRowDesc(int row)
{
    int ids[PORT_PUI_MAX_DEV + 1], n, f;
    char a[40], b[24];

    if (row < 0 || row > 3)
        return "";
    if (gPortLnOn)
        return "IN AN ONLINE GAME";
    n = Candidates(row, ids, PORT_PUI_MAX_DEV + 1);
    f = sSnap.plFlags[row];
    if (sChoice[row] >= 0 && sChoice[row] < n) {
        const int id = ids[sChoice[row]];
        const struct PuiDev *d = Dev(id);

        if (id == 0) {
            snprintf(sDesc, sizeof sDesc, row == 0 ? "A: ANY FREE CONTROLLER" : "A: P%d LEAVES - CPU PLAYS",
                     row + 1);
        } else if (d->player >= 0 && d->player != row) {
            snprintf(sDesc, sizeof sDesc, "A: TAKE %s FROM P%d", Label(d, b, sizeof b), d->player + 1);
        } else if (row >= 1 && !(f & PORT_PUI_PL_HUMAN)) {
            snprintf(sDesc, sizeof sDesc, "A: P%d JOINS WITH %s", row + 1, Label(d, b, sizeof b));
        } else {
            snprintf(sDesc, sizeof sDesc, "A: P%d PLAYS WITH %s", row + 1, Label(d, b, sizeof b));
        }
        return sDesc;
    }
    if (row >= 1 && !(f & PORT_PUI_PL_HUMAN)) {
        if (Phase())
            return "LEFT RIGHT: PICK A CONTROLLER";
        return AnyWaiting() ? "PRESS START ON A PAD TO JOIN" : "PLUG IN A PAD AND PRESS START";
    }
    if (row >= 1 && (f & PORT_PUI_PL_GRACE))
        return "PLUG THE CONTROLLER BACK IN";
    if (Phase())
        return row == 0 ? (sCardHook ? "A: PLAYER CARD" : "LEFT RIGHT: CHANGE CONTROLLER")
                        : (sCardHook ? "A: PLAYER CARD - HOLD SELECT: LEAVE" : "HOLD SELECT ON IT TO LEAVE");
    if (row == 0 && sSnap.plMain[0] == 0)
        return "EVERY FREE CONTROLLER";
    LongName(Dev(sSnap.plMain[row]), a, sizeof a);
    snprintf(sDesc, sizeof sDesc, "%s", a);
    return sDesc;
}

int PortPuiRowUnavailable(int row)
{
    (void)row;
    return gPortLnOn != 0;
}

void PortPuiLeaveRow(int row)
{
    if (row >= 0 && row < 4 && sChoice[row] >= 0) {
        sChoice[row] = -1;
        sGen++;
    }
}

int PortPuiKey(int row, unsigned key)
{
    int ids[PORT_PUI_MAX_DEV + 1], n, cur, c;

    if (row < 0 || row > 3 || gPortLnOn)
        return PORT_OUI_K_NONE;
    n = Candidates(row, ids, PORT_PUI_MAX_DEV + 1);
    cur = Current(row, ids, n);
    switch (key) {
    case 5: case 6:
        c = sChoice[row] >= 0 ? sChoice[row] : cur >= 0 ? cur : 0;
        c = (c + (key == 6 ? 1 : n - 1)) % n;
        sChoice[row] = c == cur ? -1 : c;
        sGen++;
        return PORT_OUI_K_CURSOR;
    case 1:
        c = sChoice[row];
        if (c < 0 || c >= n) {
            if (sCardHook && sCardHook(row))
                return PORT_OUI_K_SELECT;
            return PORT_OUI_K_DENY;
        }
        sChoice[row] = -1;
        sGen++;
        if (ids[c] == 0) {
            if (row == 0)
                Do(PORT_PUI_OP_ANY, 0, 0);
            else
                Do(PORT_PUI_OP_DROP, row, 0);
        } else {
            Do(PORT_PUI_OP_ASSIGN, row, ids[c]);
        }
        return PORT_OUI_K_SELECT;
    default:
        return PORT_OUI_K_NONE;
    }
}

void PortPuiSummary(char *buf, int size)
{
    int s, n = 0;

    /* shown on the SETTINGS screens, which are drawn into game memory: the
     * simulation's count, the same on every device in a session */
    for (s = 0; s < 4; s++)
        if (PortLmpSlotPerson(s))
            n++;
    snprintf(buf, size, n == 1 ? "1 PLAYER" : "%d PLAYERS", n);
}

#ifdef __cplusplus
}
#endif
