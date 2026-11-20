/* SETTINGS > ACCESSIBILITY: see port/access.h and docs/features/access.md.
 *
 * Everything here is presentation, or this device's own buttons before the
 * latch takes them (where they are recorded like any other press): nothing
 * is written to game memory, so simulation hashes do not move.
 *
 *   KIRBY MARKERS  a shape per Kirby slot (circle, square, triangle,
 *                  diamond, plus, cross, heart, ring) before its tag on
 *                  the party HUD's cards, the "1P" tag and the name tags
 *                  (platform/ui/party_hud.c), so colours are not the only cue.
 *   HIGH CONTRAST  those tags white on black, the cards' text white; on the
 *                  web the touch pads opaque with white labels.
 *   FLASHING       REDUCED: a brightness-up blend (white flashes and white
 *                  fades) may rise one step of 16 a frame (ppu.c, rl_build.c).
 *   SCREEN SHAKE   LESS / OFF: the room layer's camera shake halved or
 *                  removed by the enhanced renderer (rl_build.c); the
 *                  classic picture keeps the game's shake.
 *   HOLD B         TOGGLE: B held past HOLD_US and let go stays held until
 *                  the next B press (inhaling without holding a button);
 *                  taps are untouched.
 *   TOUCH SIZE     the web page's pads at 100..175 % (web/shell.html reads
 *                  "access=" from the saved settings).
 */
#include <stdio.h>
#include <string.h>

#include "port/access.h"
#include "port/menu_poc.h"
#include "port/inputlog.h"      /* PortInputLogReplaying */
#include "ui/settings.h"
#include "ui/ui.h"              /* PortSettingsChanged */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, PortAccessPrefersJs, (void), {
    try {
        if (typeof window === 'undefined' || !window.matchMedia) return 0;
        var r = 0;
        if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) r |= 1;
        if (window.matchMedia('(prefers-contrast: more)').matches) r |= 2;
        return r;
    } catch (e) { return 0; }
});
#endif

#ifdef __cplusplus
extern "C" {
#endif
u64 PortHostMicros(void);
u32 PortFrameNumber(void);     /* port/backend.h */
#ifdef __cplusplus
}
#endif

#define HOLD_US 350000u

static unsigned Word(void) { return gPortSettings.access; }
static int Shake(unsigned w) { return (int)(w >> PORT_A11Y_SHAKE_SHIFT) & 3; }
static int TouchSize(unsigned w) { return (int)(w >> PORT_A11Y_TOUCH_SHIFT) & 3; }

unsigned PortAccessDefaults(void)
{
    static int prefs = -1;

    if (prefs < 0) {
#ifdef __EMSCRIPTEN__
        prefs = PortAccessPrefersJs();
#else
        prefs = 0;
#endif
    }
    return ((prefs & 1) ? PORT_A11Y_FLASH | (2u << PORT_A11Y_SHAKE_SHIFT) : 0)
         | ((prefs & 2) ? PORT_A11Y_CONTRAST | PORT_A11Y_MARKERS : 0);
}

unsigned PortAccessClamp(unsigned w)
{
    w &= PORT_A11Y_MASK;
    if (Shake(w) == 3)
        w &= ~(3u << PORT_A11Y_SHAKE_SHIFT);
    return w;
}

/* --- the section's rows ----------------------------------------------------- */

enum { R_MARKERS, R_CONTRAST, R_FLASH, R_SHAKE, R_HOLDB, R_TOUCH };

static const char *const kName[] = {
    "KIRBY MARKERS", "HIGH CONTRAST", "FLASHING", "SCREEN SHAKE", "HOLD B", "TOUCH SIZE",
};

static const char *AccName(int row) { return row >= 0 && row < PORT_ACCESS_ROWS ? kName[row] : ""; }

static const char *AccDesc(int row)
{
    unsigned w = Word();

    switch (row) {
    case R_MARKERS:  return "A SHAPE FOR EACH KIRBY";
    case R_CONTRAST: return "BOLDER TAGS AND TEXT";
    case R_FLASH:    return (w & PORT_A11Y_FLASH) ? "WHITE FLASHES SOFTENED" : "AS THE GAME FLASHES";
    case R_SHAKE:    return "ENHANCED GRAPHICS ONLY";
    case R_HOLDB:    return (w & PORT_A11Y_HOLDB) ? "HOLD THEN LET GO - B STAYS" : "B AS THE GAME HAS IT";
    case R_TOUCH:    return "SIZE OF THE SCREEN BUTTONS";
    default:         return "";
    }
}

static void AccValue(int row, char *buf, int size)
{
    static const char *const kShake[] = { "FULL", "LESS", "OFF" };
    static const char *const kTouch[] = { "100%", "125%", "150%", "175%" };
    unsigned w = Word();

    switch (row) {
    case R_MARKERS:  snprintf(buf, (size_t)size, "%s", (w & PORT_A11Y_MARKERS) ? "ON" : "OFF"); break;
    case R_CONTRAST: snprintf(buf, (size_t)size, "%s", (w & PORT_A11Y_CONTRAST) ? "ON" : "OFF"); break;
    case R_FLASH:    snprintf(buf, (size_t)size, "%s", (w & PORT_A11Y_FLASH) ? "REDUCED" : "FULL"); break;
    case R_SHAKE:    snprintf(buf, (size_t)size, "%s", kShake[Shake(w) % 3]); break;
    case R_HOLDB:    snprintf(buf, (size_t)size, "%s", (w & PORT_A11Y_HOLDB) ? "TOGGLE" : "HOLD"); break;
    case R_TOUCH:    snprintf(buf, (size_t)size, "%s", kTouch[TouchSize(w)]); break;
    default:         if (size > 0) buf[0] = '\0'; break;
    }
}

static unsigned Cycle(unsigned w, int shift, int n, int dir)
{
    int v = (int)(w >> shift) & 3;

    v = (v + (dir < 0 ? -1 : 1) + n) % n;
    return (w & ~(3u << shift)) | ((unsigned)v << shift);
}

static void AccStep(int row, int dir)
{
    unsigned w = Word();

    if (PortInputLogReplaying())
        return;
    switch (row) {
    case R_MARKERS:  w ^= PORT_A11Y_MARKERS; break;
    case R_CONTRAST: w ^= PORT_A11Y_CONTRAST; break;
    case R_FLASH:    w ^= PORT_A11Y_FLASH; break;
    case R_SHAKE:    w = Cycle(w, PORT_A11Y_SHAKE_SHIFT, 3, dir); break;
    case R_HOLDB:    w ^= PORT_A11Y_HOLDB; break;
    case R_TOUCH:    w = Cycle(w, PORT_A11Y_TOUCH_SHIFT, 4, dir); break;
    default:         return;
    }
    gPortSettings.access = PortAccessClamp(w);
    PortSettingsChanged();          /* saved; the page hears of it */
}

const struct PortPocRows gPocAccessRows = {
    "ACCESSIBILITY", AccName, AccDesc, AccValue, AccStep, NULL, NULL, NULL, NULL,
};

/* --- hold-to-toggle ------------------------------------------------------------ */

#define A11Y_PLAYERS 16
#define KEY_B_BIT 0x0002u

static struct { u16 prev; u8 latched, swallow; u64 downAt; } sHold[A11Y_PLAYERS];

u16 PortAccessKeys(int player, u16 mask)
{
    int p = player >= 0 && player < A11Y_PLAYERS ? player : 0;
    int b = (mask & KEY_B_BIT) != 0, was = (sHold[p].prev & KEY_B_BIT) != 0;

    if (!(Word() & PORT_A11Y_HOLDB)) {
        memset(&sHold[p], 0, sizeof(sHold[p]));
        sHold[p].prev = mask;
        return mask;
    }
    if (b && !was) {
        if (sHold[p].latched) {         /* this press ends the latch */
            sHold[p].latched = 0;
            sHold[p].swallow = 1;
        } else {
            sHold[p].downAt = PortHostMicros();
        }
    } else if (!b && was) {
        if (!sHold[p].swallow && PortHostMicros() - sHold[p].downAt >= HOLD_US)
            sHold[p].latched = 1;
        sHold[p].swallow = 0;
    }
    sHold[p].prev = mask;
    if (sHold[p].latched)
        mask |= KEY_B_BIT;
    if (sHold[p].swallow)
        mask &= (u16)~KEY_B_BIT;
    return mask;
}

/* --- reduced flashing ------------------------------------------------------------ */

static struct { u32 frame, prevMax, curMax; } sFlash[2];

u32 PortAccessBrightUp(int ch, u32 evy)
{
    u32 f, limit;

    if (!(Word() & PORT_A11Y_FLASH) || ch < 0 || ch > 1)
        return evy;
    f = PortFrameNumber();
    if (f != sFlash[ch].frame) {
        sFlash[ch].frame = f;
        sFlash[ch].prevMax = sFlash[ch].curMax;
        sFlash[ch].curMax = 0;
    }
    limit = sFlash[ch].prevMax + 1;
    if (evy > limit)
        evy = limit;
    if (evy > sFlash[ch].curMax)
        sFlash[ch].curMax = evy;
    return evy;
}

/* --- shake, markers, contrast ------------------------------------------------------ */

int PortAccessShakeKeep(void)
{
    switch (Shake(Word())) {
    case 1:  return 128;
    case 2:  return 0;
    default: return 256;
    }
}

char PortAccessMarker(int slot)
{
    if (!(Word() & PORT_A11Y_MARKERS) || slot < 0)
        return 0;
    return (char)(1 + slot % 8);       /* party_hud.c's font: '\1'..'\10' */
}

int PortAccessContrast(void) { return (Word() & PORT_A11Y_CONTRAST) != 0; }
