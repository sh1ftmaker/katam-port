/* The GL backend: draws an RlFrame with OpenGL ES 3.0 / WebGL2 / GL 3.3 core.
 *
 * Replaceable.  It knows nothing about the game -- only the render list.
 */
#ifndef GUARD_PORT_GL_RENDERER_H
#define GUARD_PORT_GL_RENDERER_H

#include "render/render_list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `getProc` resolves a GL entry point (SDL_GL_GetProcAddress); ignored on
 * emscripten.  `es` says which shading language the context speaks: 1 for
 * GLSL ES 3.00 (GLES 3 / WebGL2), 0 for GLSL 3.30 (desktop core).  Returns 0
 * and logs why on failure. */
int GlrInit(void *(*getProc)(const char *), int es);

/* Draw the frame into the renderer's view-sized target.  Cheap to call again
 * with the same frame (a re-present at display rate). */
void GlrDraw(const RlFrame *frame);
/* The same at k target pixels per game pixel (reduced to fit the largest
 * texture), honouring the frame's 1/256-px positions (see gl_renderer.c).
 * The source GlrPresentRect/GlrReadback then see is viewW*k x viewH*k. */
void GlrDrawScaled(const RlFrame *frame, int k);

/* Scale the last drawn frame into the current framebuffer 0 of `w` x `h`
 * pixels: the largest integer scale that fits, nearest-neighbour, centred,
 * black around it; below 1x, the largest fractional fit. */
void GlrPresent(int w, int h);

/* The last drawn frame at view resolution, top row first, ABGR8888 words
 * (the PPU framebuffer's format).  Returns 0 on failure. */
int GlrReadback(u32 *out, int maxPixels, int *w, int *h);

const char *GlrDescribe(void);
/* Why the last GlrInit failed: the first shader or link error, one line
 * ("" none). */
const char *GlrInitError(void);

/* The host-driven present (platform/ui/present.c).  UploadPicture makes a
 * plain RGBA picture (the PPU framebuffer) the source instead of the
 * composite; GlrDraw makes the composite the source again.  PresentRect
 * clears framebuffer 0 (fbW x fbH) to `bg` and scales src = {x, y, w, h}
 * (top-left origin, source pixels) into dst = {x, y, w, h} (top-left origin,
 * framebuffer pixels): nearest, or sharp bilinear with `smooth`. */
void GlrUploadPicture(const u32 *pixels, int w, int h);
int GlrSourceSize(int *w, int *h);
void GlrPresentRect(int fbW, int fbH, const int src[4], const int dst[4],
                    int smooth, const float bg[3]);

/* The HUD as its own layer (docs/RENDERER.md, "HUD layer").  With
 * GlrSetHudLayer(1) before GlrDraw, a world frame's HUD (the layer in
 * RL_SPACE_HUD and the HUD sprites) is left out of the composite and drawn
 * into a HUD picture of its own, at HUD resolution, region by region;
 * GlrPresentHud then lays the regions over the world picture at `world` =
 * {x, y, w, h} (top-left origin, framebuffer pixels), `scale` framebuffer
 * pixels per HUD pixel, each pinned to its corner of that rectangle.  With 0
 * (the default) the HUD stays in the composite, in view pixels -- exactly
 * the GBA's picture at 240x160.  GlrHudActive says whether the last GlrDraw
 * drew a separate HUD (0 for frames without one, and after UploadPicture). */
void GlrSetHudLayer(int on);
int GlrHudActive(void);
void GlrPresentHud(int fbW, int fbH, const int world[4], double scale, int smooth);
/* A small host-drawn picture over the presented frame (the party HUD):
 * `pixels` is w x h premultiplied RGBA (R in the low byte), uploaded into
 * overlay `slot` (0..3) when `dirty` or new, and drawn with its top-left at
 * framebuffer pixel (x, y), `scale` framebuffer pixels per pixel -- nearest,
 * or sharp bilinear with `smooth` at a fractional scale. */
void GlrDrawOverlay(int slot, const u32 *pixels, int w, int h, int dirty,
                    int fbW, int fbH, double x, double y, double scale, int smooth);
/* GlrPresent's fit of a vw x vh picture into w x h: {x, y, w, h}, top-left
 * origin.  Integer scale when it is at least 1x. */
void GlrFitRect(int w, int h, int vw, int vh, int dst[4]);
/* Where the last GlrPresent put the picture: {x, y, w, h}, top-left origin. */
void GlrLastPresentRect(int dst[4]);

/* What the lighting (render/light.c) can do on this GPU: 0 no GPU renderer;
 * GLR_LIGHT_FAILED its shaders did not compile (logged; lighting stays off);
 * else GLR_LIGHT_OK, with RELIEF / CHARSH / RC for the parts that compiled
 * (before the first lit frame they are not tried yet: OK, RC only). */
#define GLR_LIGHT_NONE    0
#define GLR_LIGHT_OK      1
#define GLR_LIGHT_RELIEF  2
#define GLR_LIGHT_CHARSH  4
#define GLR_LIGHT_RC      8
#define GLR_LIGHT_FAILED  16
int GlrLightState(void);

#ifdef __cplusplus
}
#endif

#endif
