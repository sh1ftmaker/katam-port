/* The LIGHTING section of a settings list screen, as a model.  See
 * light_opts.h. */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "port/port.h"
#include "ui/light_opts.h"
#include "ui/settings.h"
#include "ui/ui.h"
#include "render/render_list.h"
#include "render/gl_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LO_ON, LO_ENGINE, LO_STYLE, LO_SOURCE, LO_STRENGTH, LO_SHADOW, LO_SOFT,
    LO_RELIEF_ON, LO_RELIEF, LO_SHINE, LO_HEIGHT, LO_RELIEF_BG,
    LO_SUN_ANIM, LO_SUN_SPEED, LO_SUN_ANGLE,
    LO_CHAR, LO_CHAR_LEN, LO_CHAR_DEPTH,
    LO_WARM, LO_COOL, LO_GLOW, LO_INDOOR, LO_SOLID, LO_DEPTH, LO_BOUNCE,
    LO_RESET, LO_COUNT
};

static const struct { const char *name, *desc; } kRows[LO_COUNT] = {
    [LO_ON]        = { "LIGHTING",       "LIGHT AND SHADOWS IN EVERY LEVEL" },
    [LO_ENGINE]    = { "ENGINE",         "CASCADES IS SOFTER AND SLOWER" },
    [LO_STYLE]     = { "STYLE",          "STRENGTH SHADOWS AND COLOUR TOGETHER" },
    [LO_SOURCE]    = { "LIGHT SOURCE",   "AUTO PICKS SUN OR LAMPS BY ROOM" },
    [LO_STRENGTH]  = { "STRENGTH",       "HOW STRONG THE WHOLE EFFECT IS" },
    [LO_SHADOW]    = { "SHADOWS",        "HOW DARK FULL SHADE IS" },
    [LO_SOFT]      = { "SOFTNESS",       "SOFT OR SHARP SHADOW EDGES" },
    [LO_RELIEF_ON] = { "RELIEF",         "SHADING FROM THE ART ITSELF" },
    [LO_RELIEF]    = { "RELIEF DEPTH",   "HOW DEEP THE RELIEF LOOKS" },
    [LO_SHINE]     = { "SHINE",          "HIGHLIGHTS ON THE RELIEF" },
    [LO_HEIGHT]    = { "LIGHT HEIGHT",   "LOW LIGHT MAKES LONGER RELIEF" },
    [LO_RELIEF_BG] = { "RELIEF ON BG",   "RELIEF ON BACKGROUNDS TOO" },
    [LO_SUN_ANIM]  = { "MOVING SUN",     "THE SUN CROSSES THE SKY" },
    [LO_SUN_SPEED] = { "DAY LENGTH",     "HOW LONG THE SUN TAKES TO CROSS" },
    [LO_SUN_ANGLE] = { "SUN ANGLE",      "WHERE THE SUN STANDS" },
    [LO_CHAR]      = { "CHAR SHADOWS",   "SHADOWS CAST BY KIRBY AND FOES" },
    [LO_CHAR_LEN]  = { "SHADOW LENGTH",  "HOW FAR CHARACTER SHADOWS REACH" },
    [LO_CHAR_DEPTH]= { "SHADOW DEPTH",   "CHARACTER SHADOWS INTO THE GROUND" },
    [LO_WARM]      = { "WARMTH",         "WARM LIFT IN LIT PLACES" },
    [LO_COOL]      = { "COOLNESS",       "BLUE IN THE SHADE" },
    [LO_GLOW]      = { "LAMP GLOW",      "HALO AROUND LAMPS" },
    [LO_INDOOR]    = { "INDOOR SHADE",   "SHADE AWAY FROM LAMPS INDOORS" },
    [LO_SOLID]     = { "DARK GROUND",    "DARKEN INSIDE SOLID GROUND" },
    [LO_DEPTH]     = { "GROUND DEPTH",   "HOW FAR LIGHT GOES INTO GROUND" },
    [LO_BOUNCE]    = { "BOUNCE LIGHT",   "LIGHT OFF WALLS AND FLOORS" },
    [LO_RESET]     = { "RESET LIGHTING", "EVERY LIGHTING OPTION TO DEFAULT" },
};

/* The sliders: field, range, step.  Every default (render_list.h
 * PORT_LIGHT_PARAMS_DEFAULT) is on a step. */
typedef struct Slider {
    int row;
    size_t off;
    float lo, hi, step;
} Slider;
#define F(field) offsetof(PortSettings, field)
static const Slider kSliders[] = {
    { LO_STRENGTH,   F(lightStrength),  0.0f,  3.0f,  0.25f },
    { LO_SHADOW,     F(lightShadow),    0.0f,  0.8f,  0.05f },
    { LO_SOFT,       F(lightSoft),      0.0f,  1.0f,  0.05f },
    { LO_RELIEF,     F(lightRelief),    0.0f,  2.0f,  0.1f },
    { LO_SHINE,      F(lightShine),     0.0f,  1.0f,  0.05f },
    { LO_HEIGHT,     F(lightHeight),    0.2f,  2.0f,  0.1f },
    { LO_SUN_ANGLE,  F(lightAngle),   -75.0f, 75.0f,  5.0f },
    { LO_CHAR,       F(lightSprShadow), 0.0f,  1.0f,  0.1f },
    { LO_CHAR_LEN,   F(lightCharReach),16.0f,160.0f,  8.0f },
    { LO_CHAR_DEPTH, F(lightCharDepth), 0.0f, 48.0f,  3.0f },
    { LO_WARM,       F(lightWarm),      0.0f,  1.0f,  0.05f },
    { LO_COOL,       F(lightCool),      0.0f,  1.0f,  0.05f },
    { LO_GLOW,       F(lightGlow),      0.0f,  2.0f,  0.1f },
    { LO_INDOOR,     F(lightIndoor),    0.0f,  1.0f,  0.04f },
    { LO_DEPTH,      F(lightDepth),     0.0f, 48.0f,  3.0f },
    { LO_BOUNCE,     F(lightBounce),    0.0f,  1.0f,  0.05f },
};
#undef F
#define NSLIDERS ((int)(sizeof(kSliders) / sizeof(kSliders[0])))

/* The sun's speed in degrees a second (0.5..30), as the time the sun takes
 * from sunrise to sunset (150 degrees). */
static const float kSunSpeed[] = { 0.5f, 1.0f, 1.25f, 2.5f, 5.0f, 10.0f, 15.0f, 30.0f };
#define NSPEEDS ((int)(sizeof(kSunSpeed) / sizeof(kSunSpeed[0])))

/* STYLE: strength, shadow darkness, warmth, coolness together.  NORMAL is
 * the defaults; a set that matches none of them reads CUSTOM. */
typedef struct Style {
    const char *name;
    float strength, shadow, warm, cool;
} Style;
static const Style kStyles[] = {
    { "SUBTLE", 1.0f, 0.20f, 0.20f, 0.30f },
    { "NORMAL", 2.0f, 0.30f, 0.35f, 0.50f },
    { "STRONG", 3.0f, 0.45f, 0.50f, 0.70f },
};
#define NSTYLES ((int)(sizeof(kStyles) / sizeof(kStyles[0])))

static float *Fld(PortSettings *s, size_t off) { return (float *)((char *)s + off); }
static float FldC(const PortSettings *s, size_t off) { return *(const float *)((const char *)s + off); }

static const Slider *SliderOf(int row)
{
    int i;
    for (i = 0; i < NSLIDERS; i++)
        if (kSliders[i].row == row)
            return &kSliders[i];
    return NULL;
}

static int Near(float a, float b) { return fabsf(a - b) < 1e-3f; }

static int StyleOf(const PortSettings *s)
{
    int i;
    for (i = 0; i < NSTYLES; i++)
        if (Near(s->lightStrength, kStyles[i].strength) && Near(s->lightShadow, kStyles[i].shadow)
            && Near(s->lightWarm, kStyles[i].warm) && Near(s->lightCool, kStyles[i].cool))
            return i;
    return -1;
}

static int SpeedIndex(float v)
{
    int i, best = 0;
    for (i = 1; i < NSPEEDS; i++)
        if (fabsf(kSunSpeed[i] - v) < fabsf(kSunSpeed[best] - v))
            best = i;
    return best;
}

int LightOptHost(void)
{
    int g = GlrLightState(), h = 0;

    if (PortShowUsingGpu())
        h |= LIGHT_OPT_HOST_GPU;
    if (g && !(g & GLR_LIGHT_FAILED))
        h |= LIGHT_OPT_HOST_OK;
    if (g & GLR_LIGHT_RC)
        h |= LIGHT_OPT_HOST_RC;
    return h;
}

int LightOptCount(void)
{
    return LO_COUNT;
}

const char *LightOptName(int row)
{
    return row >= 0 && row < LO_COUNT ? kRows[row].name : "";
}

int LightOptIsAction(int row)
{
    return row == LO_RESET;
}

/* Why a row is unavailable, or NULL. */
static const char *Why(const PortSettings *s, int row, int host)
{
    int rc = s->lightEngine == LIGHT_ENGINE_RC && (host & LIGHT_OPT_HOST_RC);

    if (row < 0 || row >= LO_COUNT)
        return "";
    if (!(host & LIGHT_OPT_HOST_GPU))
        return "NEEDS THE ENHANCED GRAPHICS";
    if (!(host & LIGHT_OPT_HOST_OK))
        return "THIS DEVICE CANNOT SHOW IT";
    if (row == LO_ON)
        return NULL;
    if (!s->lightOn)
        return "TURN LIGHTING ON FIRST";
    switch (row) {
    case LO_ENGINE:
        return (host & LIGHT_OPT_HOST_RC) ? NULL : "CASCADES CANNOT RUN HERE";
    case LO_RELIEF: case LO_SHINE: case LO_HEIGHT: case LO_RELIEF_BG:
        return s->lightReliefOn ? NULL : "TURN RELIEF ON FIRST";
    case LO_SUN_ANIM:
        return s->lightMode == LIGHT_REQ_POINT ? "LAMPS ONLY HAVE NO SUN" : NULL;
    case LO_SUN_SPEED:
        if (s->lightMode == LIGHT_REQ_POINT)
            return "LAMPS ONLY HAVE NO SUN";
        return s->lightSunAnim ? NULL : "TURN MOVING SUN ON FIRST";
    case LO_SUN_ANGLE:
        if (s->lightSunAnim)
            return "THE MOVING SUN SETS IT";
        return s->lightMode == LIGHT_REQ_SUN ? NULL : "SET LIGHT SOURCE TO SUN";
    case LO_CHAR_LEN:
        return s->lightSprShadow > 0.0f ? NULL : "TURN CHAR SHADOWS ON FIRST";
    case LO_CHAR_DEPTH:
        if (rc)
            return "NOT USED BY CASCADES";
        return s->lightSprShadow > 0.0f ? NULL : "TURN CHAR SHADOWS ON FIRST";
    case LO_GLOW:
        return s->lightMode == LIGHT_REQ_SUN ? "SUN ONLY HAS NO LAMPS" : NULL;
    case LO_DEPTH:
        return s->lightSolidShade ? NULL : "TURN DARK GROUND ON FIRST";
    case LO_BOUNCE:
        return rc ? NULL : "ONLY WITH THE CASCADES ENGINE";
    default:
        return NULL;
    }
}

int LightOptAvailable(const PortSettings *s, int row, int host)
{
    return Why(s, row, host) == NULL;
}

const char *LightOptDesc(const PortSettings *s, int row, int host)
{
    const char *w;

    if (row < 0 || row >= LO_COUNT)
        return "";
    w = Why(s, row, host);
    return w ? w : kRows[row].desc;
}

static const char *OnOff(int v) { return v ? "ON" : "OFF"; }

void LightOptValue(const PortSettings *s, int row, char *buf, int size)
{
    const Slider *sl = SliderOf(row);

    if (size <= 0)
        return;
    buf[0] = '\0';
    if (sl && row != LO_SUN_ANGLE && row != LO_CHAR_LEN && row != LO_CHAR_DEPTH
        && row != LO_DEPTH && row != LO_HEIGHT) {
        int pct = (int)floorf(FldC(s, sl->off) * 100.0f + 0.5f);
        if (row == LO_CHAR && pct == 0)
            snprintf(buf, (size_t)size, "OFF");
        else
            snprintf(buf, (size_t)size, "%d%%", pct);
        return;
    }
    switch (row) {
    case LO_ON:        snprintf(buf, (size_t)size, "%s", OnOff(s->lightOn)); break;
    case LO_ENGINE:    snprintf(buf, (size_t)size, "%s", s->lightEngine == LIGHT_ENGINE_RC ? "CASCADES" : "CLASSIC"); break;
    case LO_STYLE: {
        int i = StyleOf(s);
        snprintf(buf, (size_t)size, "%s", i < 0 ? "CUSTOM" : kStyles[i].name);
        break;
    }
    case LO_SOURCE:
        snprintf(buf, (size_t)size, "%s", s->lightMode == LIGHT_REQ_SUN ? "SUN"
                                          : s->lightMode == LIGHT_REQ_POINT ? "LAMPS" : "AUTO");
        break;
    case LO_RELIEF_ON: snprintf(buf, (size_t)size, "%s", OnOff(s->lightReliefOn)); break;
    case LO_RELIEF_BG: snprintf(buf, (size_t)size, "%s", OnOff(s->lightReliefBg)); break;
    case LO_SUN_ANIM:  snprintf(buf, (size_t)size, "%s", OnOff(s->lightSunAnim)); break;
    case LO_SOLID:     snprintf(buf, (size_t)size, "%s", OnOff(s->lightSolidShade)); break;
    case LO_HEIGHT:
        snprintf(buf, (size_t)size, "%d%%", (int)floorf(s->lightHeight * 100.0f + 0.5f));
        break;
    case LO_SUN_SPEED: {
        int secs = (int)floorf(150.0f / kSunSpeed[SpeedIndex(s->lightSunSpeed)] + 0.5f);
        if (secs >= 120 && secs % 60 == 0)
            snprintf(buf, (size_t)size, "%d MIN", secs / 60);
        else
            snprintf(buf, (size_t)size, "%d SEC", secs);
        break;
    }
    case LO_SUN_ANGLE: {
        int d = (int)floorf(s->lightAngle + 0.5f);
        if (d == 0)
            snprintf(buf, (size_t)size, "OVERHEAD");
        else
            snprintf(buf, (size_t)size, "%d %s", d < 0 ? -d : d, d > 0 ? "LEFT" : "RIGHT");
        break;
    }
    case LO_CHAR_LEN:   snprintf(buf, (size_t)size, "%d PX", (int)floorf(s->lightCharReach + 0.5f)); break;
    case LO_CHAR_DEPTH: snprintf(buf, (size_t)size, "%d PX", (int)floorf(s->lightCharDepth + 0.5f)); break;
    case LO_DEPTH:      snprintf(buf, (size_t)size, "%d PX", (int)floorf(s->lightDepth + 0.5f)); break;
    default: break;
    }
}

static int Toggle(int *v)
{
    *v = !*v;
    return 1;
}

int LightOptStep(PortSettings *s, int row, int dir)
{
    const Slider *sl = SliderOf(row);

    dir = dir < 0 ? -1 : 1;
    if (row == LO_SUN_ANGLE)
        dir = -dir;                  /* the angle is degrees left: Right moves it right */
    if (sl) {
        float *v = Fld(s, sl->off), was = *v;
        float n = floorf((*v - sl->lo) / sl->step + 0.5f) + (float)dir;
        float hiN = floorf((sl->hi - sl->lo) / sl->step + 0.5f);
        if (n < 0.0f) n = 0.0f;
        if (n > hiN) n = hiN;
        *v = sl->lo + n * sl->step;
        if (*v > sl->hi) *v = sl->hi;
        return !Near(*v, was);
    }
    switch (row) {
    case LO_ON:        return Toggle(&s->lightOn);
    case LO_ENGINE:    s->lightEngine = s->lightEngine == LIGHT_ENGINE_RC ? LIGHT_ENGINE_CLASSIC : LIGHT_ENGINE_RC; return 1;
    case LO_RELIEF_ON: return Toggle(&s->lightReliefOn);
    case LO_RELIEF_BG: return Toggle(&s->lightReliefBg);
    case LO_SUN_ANIM:  return Toggle(&s->lightSunAnim);
    case LO_SOLID:     return Toggle(&s->lightSolidShade);
    case LO_SOURCE: {
        /* AUTO, SUN, LAMPS (LIGHT_REQ_* order) */
        s->lightMode = (s->lightMode + dir + 3) % 3;
        return 1;
    }
    case LO_STYLE: {
        int i = StyleOf(s);
        /* From CUSTOM, Right goes to the first, Left to the last. */
        i = i < 0 ? (dir > 0 ? 0 : NSTYLES - 1) : (i + dir + NSTYLES) % NSTYLES;
        s->lightStrength = kStyles[i].strength;
        s->lightShadow = kStyles[i].shadow;
        s->lightWarm = kStyles[i].warm;
        s->lightCool = kStyles[i].cool;
        return 1;
    }
    case LO_SUN_SPEED: {
        int i = SpeedIndex(s->lightSunSpeed);
        /* Right: a longer day (a slower sun), as the values read. */
        int j = i - dir;
        if (j < 0) j = 0;
        if (j >= NSPEEDS) j = NSPEEDS - 1;
        s->lightSunSpeed = kSunSpeed[j];
        return j != i || !Near(s->lightSunSpeed, kSunSpeed[i]);
    }
    case LO_RESET: {
        /* Every lighting option to its default but the switch and the
         * engine, as the developer menu's Reset (ui_menu.cpp). */
        PortSettings d = *s;
        LightDefaults(&d);
        d.lightOn = s->lightOn;
        d.lightEngine = s->lightEngine;
        if (memcmp(&d, s, sizeof(d)) == 0)
            return 0;
        *s = d;
        return 1;
    }
    default:
        return 0;
    }
}

int LightOptStepLive(int row, int dir)
{
    if (!LightOptAvailable(&gPortSettings, row, LightOptHost()))
        return 0;
    if (!LightOptStep(&gPortSettings, row, dir))
        return 0;
    PortSettingsClamp(&gPortSettings);
    PortSettingsChanged();
    return 1;
}

#ifdef __cplusplus
}
#endif
