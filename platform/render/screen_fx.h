/* DISPLAY > FILTER, the post-process on the presented picture
 * (platform/render/screen_fx.c, docs/features/display.md). */
#ifndef GUARD_PORT_SCREEN_FX_H
#define GUARD_PORT_SCREEN_FX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Draw texture `tex`'s rect src (texels, row src[1] the picture's top) into
 * the window's rect (dx, gy, dw, dh) in GL rows, with filter `mode`
 * (PS_FX_*; texelsPerPixel texels make one game pixel).  0: not drawn (off,
 * or the shader failed), the caller blits as before. */
int GlrFxDraw(int es, unsigned tex, const int src[4], int dx, int gy, int dw, int dh,
              int texelsPerPixel, int mode);

/* The filter the next GlrPresentRect uses (present.c sets it each frame). */
extern int gGlrScreenFx;

#ifdef __cplusplus
}
#endif

#endif
