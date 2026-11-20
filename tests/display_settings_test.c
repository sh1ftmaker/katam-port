/* DISPLAY polish (docs/features/display.md): the STRETCH layout and the
 * FILTER setting's save / load / clamp.  `make display-test`; well under a
 * second, no ROM. */
#include <stdio.h>
#include <string.h>

#include "port/port.h"
#include "ui/settings.h"

/* settings.c's only outside calls */
int PortCardFormatSettings(char *buf, int size) { (void)size; buf[0] = '\0'; return 0; }
int PortCardParseSetting(const char *key, const char *val) { (void)key; (void)val; return 0; }
int PortSimViewCovers(s32 w, s32 h) { (void)w; (void)h; return 0; }

static int sFails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); sFails++; } } while (0)

int main(void)
{
    PortSettings s, t;
    PortLayout lay;
    char buf[4096];
    int d[4];

    PortSettingsDefaults(&s);
    CHECK(s.filter == PS_FX_OFF);                   /* off by default */

    /* FIT keeps the shape and centres; STRETCH fills the window */
    s.scaling = PS_SCALE_FIT;
    PortLayoutFit(&s, 1000, 700, 240, 160, d);
    CHECK(d[2] == 1000 && d[3] == 667 && d[1] == 16);
    s.scaling = PS_SCALE_INTEGER;
    PortLayoutFit(&s, 1000, 700, 240, 160, d);
    CHECK(d[2] == 960 && d[3] == 640 && d[0] == 20 && d[1] == 30);
    s.scaling = PS_SCALE_STRETCH;
    PortLayoutFit(&s, 1000, 700, 240, 160, d);
    CHECK(d[0] == 0 && d[1] == 0 && d[2] == 1000 && d[3] == 700);
    PortLayoutCompute(&s, 1000, 700, 1, 1.0f, 240, 160, &lay);
    CHECK(lay.dst[2] == 1000 && lay.dst[3] == 700 && lay.smooth);
    CHECK(PS_SCALE_SOFT(PS_SCALE_STRETCH) && PS_SCALE_SOFT(PS_SCALE_SMOOTH)
          && !PS_SCALE_SOFT(PS_SCALE_FIT) && !PS_SCALE_SOFT(PS_SCALE_INTEGER));

    /* the filter and the scaling survive a save and a load */
    s.filter = PS_FX_CRT;
    PortSettingsFormat(&s, buf, sizeof(buf));
    CHECK(strstr(buf, "filter=2\n") != NULL);
    PortSettingsDefaults(&t);
    PortSettingsParse(&t, buf);
    CHECK(t.filter == PS_FX_CRT && t.scaling == PS_SCALE_STRETCH);

    /* out of range is clamped */
    PortSettingsParse(&t, "schema=2\nfilter=9\n");
    CHECK(t.filter == PS_FX_COUNT - 1);
    PortSettingsParse(&t, "schema=2\nfilter=-3\n");
    CHECK(t.filter == PS_FX_OFF);

    printf("display-test: %s\n", sFails ? "FAILED" : "ok");
    return sFails != 0;
}
