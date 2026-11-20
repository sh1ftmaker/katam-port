/* SETTINGS > ACCESSIBILITY (platform/access.c): the rows, hold-to-toggle B,
 * the flash limiter, the shake factor and the markers, against stubs.
 *   make -s access-test */
#include <stdio.h>
#include <string.h>

#include "port/access.h"
#include "port/menu_poc.h"
#include "ui/settings.h"

PortSettings gPortSettings;
static unsigned long long sNow;
static unsigned sFrame;
static int sSaved;
unsigned long long PortHostMicros(void) { return sNow; }
u32 PortFrameNumber(void) { return sFrame; }
int PortInputLogReplaying(void) { return 0; }
void PortSettingsChanged(void) { sSaved++; }

static int sFail, sChecks;
#define CHECK(c) do { sChecks++; if (!(c)) { sFail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static u16 Keys(u16 m, unsigned ms) { sNow += ms * 1000ull; return PortAccessKeys(0, m); }

int main(void)
{
    char v[32];
    int i;
    u32 e;

    /* defaults natively: everything off */
    gPortSettings.access = PortAccessDefaults();
    CHECK(gPortSettings.access == 0);
    CHECK(PortAccessShakeKeep() == 256 && !PortAccessMarker(0) && !PortAccessContrast());

    /* off: B passes through */
    CHECK(Keys(2, 0) == 2 && Keys(0, 500) == 0);

    /* the rows */
    CHECK(strcmp(gPocAccessRows.name(4), "HOLD B") == 0);
    gPocAccessRows.step(4, 1);
    gPocAccessRows.value(4, v, sizeof v);
    CHECK(strcmp(v, "TOGGLE") == 0 && sSaved == 1);
    gPocAccessRows.step(3, 1);
    gPocAccessRows.step(3, 1);
    gPocAccessRows.value(3, v, sizeof v);
    CHECK(strcmp(v, "OFF") == 0 && PortAccessShakeKeep() == 0);
    gPocAccessRows.step(3, 1);
    CHECK(PortAccessShakeKeep() == 256);
    gPocAccessRows.step(3, -1);
    CHECK(PortAccessShakeKeep() == 0);
    CHECK(PortAccessClamp(0xFFFFu) == 0xCFu);  /* shake 3 is not a value */

    /* hold-to-toggle: a tap is a tap */
    CHECK(Keys(2, 10) == 2);
    CHECK(Keys(0, 100) == 0);
    /* held past the threshold and let go: stays held */
    CHECK(Keys(2, 10) == 2);
    CHECK(Keys(0, 400) == 2);
    CHECK(Keys(1, 50) == 3);            /* other buttons still work */
    CHECK(Keys(0, 50) == 2);
    /* the next press ends it, and is not a press itself */
    CHECK(Keys(2, 50) == 0);
    CHECK(Keys(0, 50) == 0);
    CHECK(Keys(2, 50) == 2);            /* then B is B again */
    CHECK(Keys(0, 50) == 0);

    /* reduced flashing: one step a frame up, at once down */
    CHECK(PortAccessBrightUp(0, 16) == 16);     /* off */
    gPocAccessRows.step(2, 1);
    for (i = 0; i < 20; i++) {
        sFrame++;
        e = PortAccessBrightUp(0, 16);
        PortAccessBrightUp(0, 16);              /* a second line */
        CHECK(e == (u32)(i + 1 > 16 ? 16 : i + 1));
    }
    sFrame++;
    CHECK(PortAccessBrightUp(0, 0) == 0);
    sFrame++;
    CHECK(PortAccessBrightUp(0, 16) == 1);      /* a one-frame flash */
    CHECK(PortAccessBrightUp(1, 16) == 1);      /* the other renderer's own */

    /* markers: a shape per slot, eight of them */
    gPocAccessRows.step(0, 1);
    CHECK(PortAccessMarker(0) == 1 && PortAccessMarker(7) == 8 && PortAccessMarker(8) == 1);
#ifdef __EMSCRIPTEN__
    gPocAccessRows.step(5, 1);
#endif
    printf("access-test: %d checks, %d failed\n", sChecks, sFail);
    return sFail != 0;
}
