/* SETTINGS > GAME > SAVE STATES: the rows of platform/states.c's section,
 * as a struct PortPocRows (port/menu_poc.h "Adding a section").
 *
 * SLOT picks the slot SAVE STATE and LOAD STATE use (the keyboard's
 * F1-F9 / Shift+F1-F9 name theirs directly); both run once the SETTINGS
 * screen has closed.  REWIND turns the rewind recording on and off, FAST
 * and SLOW are the speeds the hold keys (Tab, Shift+Tab) give.  The page
 * keeps these in the browser (web/states.js).  None of it online. */

#include <stdio.h>
#include <string.h>

#include "port/port.h"
#include "port/menu_poc.h"
#include "port/states.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { ROW_SLOT, ROW_SAVE, ROW_LOAD, ROW_REWIND, ROW_FAST, ROW_SLOW, ST_ROWS };

static const char *const kName[ST_ROWS] = {
    "SLOT", "SAVE STATE", "LOAD STATE", "REWIND", "FAST SPEED", "SLOW SPEED",
};
static const char *const kDesc[ST_ROWS] = {
    "WHICH STATE SAVE AND LOAD USE",
    "SAVED AS THE MENU CLOSES",
    "LOADED AS THE MENU CLOSES",
    "HOLD BACK QUOTE TO STEP BACK",
    "HOLD TAB TO SPEED UP",
    "HOLD SHIFT AND TAB TO SLOW",
};

static const char *StName(int row) { return row >= 0 && row < ST_ROWS ? kName[row] : ""; }
static const char *StDesc(int row) { return row >= 0 && row < ST_ROWS ? kDesc[row] : ""; }

static void StValue(int row, char *buf, int size)
{
    static const char *const kSlow[] = { "", "", "HALF", "THIRD", "QUARTER" };

    buf[0] = '\0';
    switch (row) {
    case ROW_SLOT:   snprintf(buf, size, "%d", PortStSlot()); break;
    case ROW_LOAD:
        if (!(PortStHave() & (1u << (PortStSlot() - 1))))
            snprintf(buf, size, "EMPTY");
        break;
    case ROW_REWIND: snprintf(buf, size, "%s", PortStRewindOn() ? "ON" : "OFF"); break;
    case ROW_FAST:   snprintf(buf, size, "%dX", PortStFast()); break;
    case ROW_SLOW:   snprintf(buf, size, "%s", kSlow[PortStSlow()]); break;
    default: break;
    }
}

static void StStep(int row, int dir)
{
    switch (row) {
    case ROW_SLOT:   PortStStepPref(0, dir); break;
    case ROW_SAVE:   PortStRequestSave(PortStSlot()); break;
    case ROW_LOAD:   PortStRequestLoad(PortStSlot()); break;
    case ROW_REWIND: PortStStepPref(1, dir); break;
    case ROW_FAST:   PortStStepPref(2, dir); break;
    case ROW_SLOW:   PortStStepPref(3, dir); break;
    default: break;
    }
}

static int StAvailable(int row)
{
    (void)row;
    return PortStAllowed();
}

/* extern first: a C++ (lp64) const is otherwise file-local */
extern const struct PortPocRows gPocStateRows;
const struct PortPocRows gPocStateRows = {
    "SAVE STATES", StName, StDesc, StValue, StStep, StAvailable, NULL, NULL, NULL,
};

#ifdef __cplusplus
}
#endif
