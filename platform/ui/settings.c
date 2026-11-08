/* Player settings: defaults, persistence format, layout.  See settings.h. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "ui/settings.h"
#include "render/render_list.h"

#ifdef __cplusplus
extern "C" {
#endif

PortSettings gPortSettings;

void PortSettingsDefaults(PortSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->renderer = PS_RENDER_GPU;
    s->zoom = 1.0f;
    s->dynZoom = 0;
    s->aspect = PS_ASPECT_AUTO;
    s->scaling = PS_SCALE_FIT;
    s->letterbox = 0x0C0E11;
    s->hud = PS_HUD_CORNERS;
    s->vsync = 1;
    s->showFps = 0;
    s->volume = 100;
    s->mute = 0;
    s->uiScale = 1.0f;
}

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float ClampF(float v, float lo, float hi)
{
    if (!(v == v)) return lo;           /* NaN */
    return v < lo ? lo : v > hi ? hi : v;
}

void PortSettingsClamp(PortSettings *s)
{
    s->renderer = ClampI(s->renderer, 0, 1);
    s->zoom = ClampF(s->zoom, 0.5f, 2.0f);
    s->dynZoom = !!s->dynZoom;
    s->aspect = ClampI(s->aspect, 0, PS_ASPECT_COUNT - 1);
    s->scaling = ClampI(s->scaling, 0, PS_SCALE_COUNT - 1);
    s->letterbox &= 0xFFFFFF;
    s->hud = ClampI(s->hud, 0, 1);
    s->vsync = !!s->vsync;
    s->showFps = !!s->showFps;
    s->volume = ClampI(s->volume, 0, 100);
    s->mute = !!s->mute;
    s->uiScale = ClampF(s->uiScale, 0.6f, 2.0f);
}

void PortSettingsParse(PortSettings *s, const char *text)
{
    const char *p = text;

    while (p != NULL && *p != '\0') {
        char key[32], val[64];
        const char *eol = strchr(p, '\n');
        const char *eq = strchr(p, '=');
        size_t kl, vl;

        if (eol == NULL) eol = p + strlen(p);
        if (eq != NULL && eq < eol) {
            kl = (size_t)(eq - p);
            vl = (size_t)(eol - eq - 1);
            if (kl < sizeof(key) && vl < sizeof(val)) {
                memcpy(key, p, kl); key[kl] = '\0';
                memcpy(val, eq + 1, vl); val[vl] = '\0';
                if (vl && val[vl - 1] == '\r') val[vl - 1] = '\0';
#define INT_KEY(name, field) else if (strcmp(key, name) == 0) s->field = (int)strtol(val, NULL, 0);
#define FLT_KEY(name, field) else if (strcmp(key, name) == 0) s->field = (float)strtod(val, NULL);
                if (0) {}
                INT_KEY("renderer", renderer)
                FLT_KEY("zoom", zoom)
                INT_KEY("dynzoom", dynZoom)
                INT_KEY("aspect", aspect)
                INT_KEY("scaling", scaling)
                else if (strcmp(key, "letterbox") == 0) s->letterbox = (unsigned)strtoul(val, NULL, 16);
                INT_KEY("hud", hud)
                INT_KEY("vsync", vsync)
                INT_KEY("fps", showFps)
                INT_KEY("volume", volume)
                INT_KEY("mute", mute)
                FLT_KEY("uiscale", uiScale)
#undef INT_KEY
#undef FLT_KEY
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    PortSettingsClamp(s);
}

int PortSettingsFormat(const PortSettings *s, char *buf, int size)
{
    int n = snprintf(buf, (size_t)size,
        "renderer=%d\nzoom=%.3f\ndynzoom=%d\naspect=%d\nscaling=%d\n"
        "letterbox=%06X\nhud=%d\nvsync=%d\nfps=%d\nvolume=%d\nmute=%d\n"
        "uiscale=%.2f\n",
        s->renderer, (double)s->zoom, s->dynZoom, s->aspect, s->scaling,
        s->letterbox, s->hud, s->vsync, s->showFps, s->volume, s->mute,
        (double)s->uiScale);
    return n < size ? n : size - 1;
}

float PortAspectValue(int aspect, float windowAspect)
{
    switch (aspect) {
    case PS_ASPECT_3_2:   return 3.0f / 2.0f;
    case PS_ASPECT_4_3:   return 4.0f / 3.0f;
    case PS_ASPECT_16_10: return 16.0f / 10.0f;
    case PS_ASPECT_16_9:  return 16.0f / 9.0f;
    case PS_ASPECT_21_9:  return 21.0f / 9.0f;
    case PS_ASPECT_FILL:  return windowAspect > 0.2f ? windowAspect : 1.5f;
    case PS_ASPECT_AUTO:
    default:
        /* The window's own shape, between the GBA's and ultrawide: a
         * landscape phone fills, a portrait one keeps the original. */
        return ClampF(windowAspect, 1.5f, 21.0f / 9.0f);
    }
}

const char *PortAspectName(int aspect)
{
    static const char *const k[PS_ASPECT_COUNT] = {
        "Auto (window, 3:2 to 21:9)", "Original 3:2 (240x160)", "4:3", "16:10",
        "16:9", "21:9", "Fill window",
    };
    return (aspect >= 0 && aspect < PS_ASPECT_COUNT) ? k[aspect] : "?";
}

static void Centre(int fbW, int fbH, int w, int h, int dst[4])
{
    dst[0] = (fbW - w) / 2;
    dst[1] = (fbH - h) / 2;
    dst[2] = w;
    dst[3] = h;
}

void PortLayoutFit(const PortSettings *s, int fbW, int fbH, int srcW, int srcH,
                   int dst[4])
{
    double k = fmin((double)fbW / srcW, (double)fbH / srcH);

    if (s->scaling == PS_SCALE_INTEGER && k >= 1.0)
        k = floor(k);
    Centre(fbW, fbH, (int)(srcW * k + 0.5), (int)(srcH * k + 0.5), dst);
}

void PortLayoutCompute(const PortSettings *s, int fbW, int fbH, int world,
                       float zoom, int srcW, int srcH, PortLayout *out)
{
    float winA, a, visH;
    double vw, vh, k;

    if (fbW < 1) fbW = 1;
    if (fbH < 1) fbH = 1;
    out->smooth = s->scaling == PS_SCALE_SMOOTH;
    if (!world) {
        out->viewW = srcW;
        out->viewH = srcH;
        PortLayoutFit(s, fbW, fbH, srcW, srcH, out->dst);
        return;
    }
    winA = (float)fbW / (float)fbH;
    a = PortAspectValue(s->aspect, winA);
    visH = 160.0f / ClampF(zoom, 0.25f, 4.0f);
    vh = visH;
    vw = visH * a;
    /* Inside what the render list can draw, keeping the shape. */
    if (vw < RL_MIN_VIEW_W) { vh *= RL_MIN_VIEW_W / vw; vw = RL_MIN_VIEW_W; }
    if (vh < RL_MIN_VIEW_H) { vw *= RL_MIN_VIEW_H / vh; vh = RL_MIN_VIEW_H; }
    if (vw > 1024) { vh *= 1024 / vw; vw = 1024; }
    if (vh > 512)  { vw *= 512 / vh;  vh = 512; }
    out->viewW = ((int)(vw + 0.5)) & ~1;
    out->viewH = ((int)(vh + 0.5)) & ~1;
    if (out->viewW < RL_MIN_VIEW_W) out->viewW = RL_MIN_VIEW_W;
    if (out->viewH < RL_MIN_VIEW_H) out->viewH = RL_MIN_VIEW_H;
    k = fmin((double)fbW / out->viewW, (double)fbH / out->viewH);
    if (s->scaling == PS_SCALE_INTEGER && k >= 1.0)
        k = floor(k);
    Centre(fbW, fbH, (int)(out->viewW * k + 0.5), (int)(out->viewH * k + 0.5),
           out->dst);
    (void)srcW; (void)srcH;
}

int PortLayoutPastSpawnMargin(int viewW, int viewH)
{
    /* The spawn margin follows the view (platform/view.c) unless a session
     * holds it at the original. */
    if (PortSimViewCovers(viewW, viewH))
        return 0;
    return viewW > 240 + 2 * PORT_SPAWN_MARGIN || viewH > 160 + 2 * PORT_SPAWN_MARGIN;
}

#ifdef __cplusplus
}
#endif
