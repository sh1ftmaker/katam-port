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

/* Scale the last drawn frame into the current framebuffer 0 of `w` x `h`
 * pixels: the largest integer scale that fits, nearest-neighbour, centred,
 * black around it; below 1x, the largest fractional fit. */
void GlrPresent(int w, int h);

/* The last drawn frame at view resolution, top row first, ABGR8888 words
 * (the PPU framebuffer's format).  Returns 0 on failure. */
int GlrReadback(u32 *out, int maxPixels, int *w, int *h);

const char *GlrDescribe(void);

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
/* GlrPresent's fit of a vw x vh picture into w x h: {x, y, w, h}, top-left
 * origin.  Integer scale when it is at least 1x. */
void GlrFitRect(int w, int h, int vw, int vh, int dst[4]);
/* Where the last GlrPresent put the picture: {x, y, w, h}, top-left origin. */
void GlrLastPresentRect(int dst[4]);

#ifdef __cplusplus
}
#endif

#endif
