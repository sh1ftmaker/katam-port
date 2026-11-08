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

#ifdef __cplusplus
}
#endif

#endif
