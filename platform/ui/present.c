/* The shared presentation path: one GL window for both renderers, the view
 * rectangle from the settings, and the menu drawn over it.
 *
 * Presentation only.  The renderer switch toggles gPortRlEnabled (the render
 * list hooks, which only read game memory) and gPortPpuCompose (whether the
 * emulated PPU composes pixels; its scan, HBlank DMA and interrupts run
 * either way), exactly the two states `--renderer gpu` and `--renderer ppu`
 * select at start-up; docs/RENDERER.md has the proof both leave keys/sim/vid/
 * sram alone.  The view size is a render-list input that never reaches the
 * game.  Nothing here writes game memory.
 */

#include <math.h>
#include <string.h>

#include "port/port.h"
#include "render/render_list.h"
#include "render/gl_renderer.h"
#include "ui/settings.h"
#include "ui/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int gPortPpuCompose;     /* platform/ppu.c */

static int sReady, sGpuAvail, sUsingGpu = -1, sUi;
static int sDst[4], sSrc[4];
static int sWorldViewW = 240, sWorldViewH = 160;

/* Dynamic zoom (docs/VIEW.md): ease in while the camera rests, out fast when
 * it moves.  Here it only changes how much of the world the render list is
 * asked to show. */
static float sDynZoom = 1.0f;
static s32 sPrevCamX, sPrevCamY;
static int sHaveCam;
static float sCamSpeed;

/* Rates, for the menu and the FPS overlay. */
static double sRateT0;
static int sRateSims, sRateDraws;

static void ApplyRenderer(void)
{
    int want = sGpuAvail && gPortSettings.renderer == PS_RENDER_GPU;

    if (want == sUsingGpu)
        return;
    sUsingGpu = want;
    gPortRlEnabled = want;
    gPortPpuCompose = !want;
    PortLog("[katam-port] renderer: %s", want ? "gpu (render list)" : "ppu");
}

void PortSettingsApplyEarly(void)
{
    PortSettingsClamp(&gPortSettings);
    gPortRlHudStrip = gPortSettings.hud == PS_HUD_STRIP;
    PortHostSetVolume(gPortSettings.mute ? 0.0f : gPortSettings.volume / 100.0f);
}

void PortSettingsChanged(void)
{
    char buf[512];

    PortSettingsApplyEarly();
    PortHostSetVsync(gPortSettings.vsync);
    if (sReady)
        ApplyRenderer();
    PortSettingsFormat(&gPortSettings, buf, sizeof(buf));
    PortHostSaveSettings(buf);
}

int PortShowInit(int es, int gpuAvailable)
{
    sGpuAvail = gpuAvailable;
    sReady = 1;
    ApplyRenderer();
    sUi = PortUiInit(es);
    if (!sUi)
        PortError("[katam-port] settings menu did not start");
    gPortUiStatus.gpuAvailable = gpuAvailable;
    gPortUiStatus.gpuDesc = GlrDescribe();
    return 1;
}

int PortShowReady(void)
{
    return sReady;
}

int PortShowUsingGpu(void)
{
    return sUsingGpu == 1;
}

static void UpdateDynZoom(void)
{
    s32 kx, ky, cx, cy, dx, dy;
    float speed, target;

    if (!gPortSettings.dynZoom) {
        sDynZoom = 1.0f;
        sHaveCam = 0;
        return;
    }
    if (!PortRlFocus(&kx, &ky, &cx, &cy)) {
        sHaveCam = 0;
        return;
    }
    if (!sHaveCam) {
        sPrevCamX = cx; sPrevCamY = cy; sHaveCam = 1;
    }
    dx = cx - sPrevCamX; if (dx < 0) dx = -dx;
    dy = cy - sPrevCamY; if (dy < 0) dy = -dy;
    sPrevCamX = cx; sPrevCamY = cy;
    if ((dx > dy ? dx : dy) > 0x180 * 8)
        return;                 /* a room change teleports the camera */
    /* Camera speed in 8.8 px/frame, smoothed; a pixel a frame is "moving". */
    speed = (float)(dx > dy ? dx : dy);
    sCamSpeed += (speed - sCamSpeed) * 0.125f;
    if (sCamSpeed > 256.0f) sCamSpeed = 256.0f;
    target = 1.5f - 0.5f * sCamSpeed / 256.0f;
    if (target < sDynZoom) {
        sDynZoom -= 0.0625f;            /* out in about a fifth of a second */
        if (sDynZoom < target) sDynZoom = target;
    } else if (target > sDynZoom) {
        sDynZoom += 0.0078f;            /* in over about a second and a half */
        if (sDynZoom > target) sDynZoom = target;
    }
}

static void CountRates(double nowMs, int sim)
{
    if (sRateT0 == 0)
        sRateT0 = nowMs;
    sRateDraws++;
    sRateSims += sim;
    if (nowMs - sRateT0 >= 500.0) {
        double secs = (nowMs - sRateT0) / 1000.0;

        gPortUiStatus.simFps = (float)(sRateSims / secs);
        gPortUiStatus.drawFps = (float)(sRateDraws / secs);
        sRateT0 = nowMs;
        sRateSims = sRateDraws = 0;
    }
}

static void Draw(int fbW, int fbH, float dpr, double nowMs)
{
    float bg[3];
    int w, h;

    bg[0] = ((gPortSettings.letterbox >> 16) & 255) / 255.0f;
    bg[1] = ((gPortSettings.letterbox >> 8) & 255) / 255.0f;
    bg[2] = (gPortSettings.letterbox & 255) / 255.0f;
    if (GlrSourceSize(&w, &h)) {
        sSrc[0] = 0; sSrc[1] = 0; sSrc[2] = w; sSrc[3] = h;
        PortLayoutFit(&gPortSettings, fbW, fbH, w, h, sDst);
    } else {
        memset(sSrc, 0, sizeof(sSrc));
        memset(sDst, 0, sizeof(sDst));
    }
    GlrPresentRect(fbW, fbH, sSrc, sDst,
                   gPortSettings.scaling == PS_SCALE_SMOOTH, bg);
    if (sUi)
        PortUiDraw(fbW, fbH, dpr, nowMs);
}

void PortShowNewFrame(int fbW, int fbH, float dpr, double nowMs,
                      const unsigned int *ppu, int ppuW, int ppuH)
{
    const RlFrame *f = sUsingGpu == 1 ? PortRlCurrentFrame() : NULL;
    PortLayout lay;
    float zoom;

    if (!sReady)
        return;
    UpdateDynZoom();
    zoom = gPortSettings.zoom * sDynZoom;
    /* The world view for the next frame built (applies to gameplay frames;
     * menus are always 240x160). */
    PortLayoutCompute(&gPortSettings, fbW, fbH, 1, zoom, 240, 160, &lay);
    sWorldViewW = lay.viewW;
    sWorldViewH = lay.viewH;
    PortRlSetViewSize(lay.viewW, lay.viewH);

    if (f != NULL) {
        GlrDraw(f);
        gPortUiStatus.viewW = f->viewW;
        gPortUiStatus.viewH = f->viewH;
        gPortUiStatus.world = f->world;
    } else {
        GlrUploadPicture(ppu, ppuW, ppuH);
        gPortUiStatus.viewW = ppuW;
        gPortUiStatus.viewH = ppuH;
        gPortUiStatus.world = 0;
    }
    gPortUiStatus.gpu = sUsingGpu == 1;
    gPortUiStatus.zoomNow = zoom;
    CountRates(nowMs, 1);
    Draw(fbW, fbH, dpr, nowMs);
}

void PortShowAgain(int fbW, int fbH, float dpr, double nowMs)
{
    if (!sReady)
        return;
    CountRates(nowMs, 0);
    Draw(fbW, fbH, dpr, nowMs);
}

#ifdef __cplusplus
}
#endif
