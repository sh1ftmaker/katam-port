/* SETTINGS > PLAYERS > CONTROLS: input remapping, the menu's half.
 *
 * The bindings are the web page's (web/input_remap.js: per-device profiles
 * in localStorage, press to bind, rumble); this section only shows its rows
 * and passes Left / Right / A on, through the host's UI queries and actions
 * ("remap.v.N", "remap.d.N", "remap.gen"; "remap.step" with row | dir << 8).
 * The row numbers are input_remap.js's ROW.  Nothing here touches the game:
 * the masks the bindings make reach it as before.
 *
 * A section kept by another file (port/menu_poc.h "Adding a section"): no
 * online rows, so during an online game the rows are dimmed (the menu is
 * drawn into game memory, and a device's bindings are its own).  Natively
 * the host answers no queries: the rows say "WEB PAGE ONLY".
 *
 * Also the rumble probe the page polls (PortRumbleProbe): a read of the
 * Kirbys' health and ability, nothing written.  docs/features/remap.md. */
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "kirby.h"
#include "port/port.h"
#include "port/menu_poc.h"
#include "port/inputlog.h"
#include "port/lmpnet.h"
#include "ui/ui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum { CR_DEVICE, CR_ACT0, CR_RUMBLE = CR_ACT0 + 12, CR_RESET, CR_ROWS };

static const char *const kName[CR_ROWS] = {
    "CONTROLLER", "A", "B", "L", "R", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT",
    "FULLSCREEN", "MUTE", "RUMBLE", "RESET CONTROLLER",
};

int PortControlsRowCount(void) { return CR_ROWS; }

static int Query(const char *key, char *buf, int size)
{
    buf[0] = '\0';
    PortHostUiQuery(key, buf, size);
    return buf[0] != '\0';
}

/* The menu's font: A-Z 0-9 space - . : / + ! (anything else dropped). */
static void Clean(char *s, int max)
{
    int i, o = 0;

    for (i = 0; s[i] && o < max; i++) {
        char c = s[i];

        if (c >= 'a' && c <= 'z')
            c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '.'
            || c == ':' || c == '/' || c == '+' || c == '!')
            s[o++] = c;
    }
    s[o] = '\0';
}

static int On(void)
{
    char b[4];

    return Query("remap.on", b, (int)sizeof b) && b[0] == '1';
}

static const char *CtlName(int row) { return row >= 0 && row < CR_ROWS ? kName[row] : ""; }

static const char *CtlDesc(int row)
{
    static char d[40];
    char key[16];

    if (!On())
        return "WEB PAGE ONLY";
    snprintf(key, sizeof key, "remap.d.%d", row);
    Query(key, d, (int)sizeof d);
    Clean(d, 30);
    return d;
}

static void CtlValue(int row, char *buf, int size)
{
    char key[16];

    buf[0] = '\0';
    if (!On())
        return;
    snprintf(key, sizeof key, "remap.v.%d", row);
    Query(key, buf, size);
    Clean(buf, size < 17 ? size - 1 : 16);
}

static void CtlStep(int row, int dir)
{
    if (PortInputLogReplaying())
        return;                     /* a replay never reaches the page */
    PortHostUiAction("remap.step", (row & 255) | (dir > 0 ? 1 << 8 : 0));
}

static int CtlAvailable(int row) { (void)row; return On(); }

/* (extern first: the native build compiles this as C++, where a const
 * object is otherwise this file's own) */
extern const struct PortPocRows gPocControlsRows;
const struct PortPocRows gPocControlsRows = {
    "CONTROLS", CtlName, CtlDesc, CtlValue, CtlStep, CtlAvailable, NULL, NULL, NULL,
};

/* The menu redraws when this changes (a bind landed, a pad came or went). */
u32 PortControlsGen(void)
{
    char b[16];
    u32 g = 0;
    int i;

    if (!Query("remap.gen", b, (int)sizeof b))
        return 0;
    for (i = 0; b[i]; i++)
        g = g * 10u + (u32)(b[i] - '0');
    return g;
}

/* For the page's rumble (web/input_remap.js watch): player p's Kirby's
 * health | ability << 8, or -1 (none).  Locally player p is Kirby p; online
 * only this device's own player (0), its Kirby gLocalPlayerId. */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortRumbleProbe(int p)
{
    int s = p;
    const struct Kirby *k;

    if (gPortLnOn) {
        if (p != 0)
            return -1;
        s = gLocalPlayerId;
    }
    if (s < 0 || s >= gNumKirbys)
        return -1;
    k = &gKirbys[s];
    return (k->hp > 0 ? (k->hp & 255) : 0) | ((k->ability & 255) << 8);
}
