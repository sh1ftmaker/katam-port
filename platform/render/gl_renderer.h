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

#ifdef __cplusplus
}
#endif

#endif
