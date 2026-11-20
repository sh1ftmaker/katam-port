/* DISPLAY > FILTER: an optional post-process on the presented picture
 * (docs/features/display.md).
 *
 * GlrPresentRect normally blits the composite (or the uploaded PPU picture)
 * to the window.  With a filter on it calls GlrFxDraw instead: one
 * full-picture triangle, four texel fetches a pixel -- cheap enough for a
 * phone -- with
 *   LCD    the GBA screen's colour response and a faint pixel grid,
 *   CRT    light scanlines, one per game row,
 *   SHARP  sharp bilinear (nearest to the whole multiple, then linear).
 * All three sample sharp-bilinear, so a fractional scale stays even.  The
 * grid and the scanlines follow game pixels (k texels each in the GPU
 * renderer's composite) and fade out when a game pixel is too few screen
 * pixels to carry them.
 *
 * Presentation only: it reads a texture the frame already drew and writes
 * the window.  Nothing here reaches the game, the render list or a hash.
 * Off (the default) never comes here.
 */

#include <stdio.h>

#include "port/port.h"
#include "render/gl_api.h"
#include "render/screen_fx.h"

#define G(name) GLR_FN(name)

static GLuint sProg, sVao;
static int sTried;
static GLint uTex, uSrc, uDst, uK, uMode;

static const char *kVs =
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID & 1) * 4) - 1.0, float((gl_VertexID & 2) * 2) - 1.0);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *kFs =
    "uniform sampler2D uTex;\n"
    "uniform vec4 uSrc;\n"      /* texels: x, y, w, h (row y = the picture's top) */
    "uniform vec4 uDst;\n"      /* window pixels, GL rows: x, y, w, h */
    "uniform float uK;\n"       /* texels per game pixel */
    "uniform int uMode;\n"
    "out vec4 oColor;\n"
    "vec3 at(ivec2 p) {\n"
    "  ivec2 lo = ivec2(uSrc.xy), hi = ivec2(uSrc.xy + uSrc.zw) - 1;\n"
    "  return texelFetch(uTex, clamp(p, lo, hi), 0).rgb;\n"
    "}\n"
    "void main() {\n"
    "  vec2 q = (gl_FragCoord.xy - uDst.xy) / uDst.zw;\n"
    "  q.y = 1.0 - q.y;\n"
    "  vec2 t = uSrc.xy + q * uSrc.zw;\n"
    "  vec2 scale = uDst.zw / uSrc.zw;\n"           /* window pixels a texel */
    "  vec2 tc = t - 0.5;\n"
    "  vec2 i = floor(tc);\n"
    "  vec2 f = clamp((tc - i - 0.5) * max(scale, vec2(1.0)) + 0.5, 0.0, 1.0);\n"
    "  ivec2 b = ivec2(i);\n"
    "  vec3 c = mix(mix(at(b), at(b + ivec2(1, 0)), f.x),\n"
    "               mix(at(b + ivec2(0, 1)), at(b + ivec2(1, 1)), f.x), f.y);\n"
    "  vec2 g = (t - uSrc.xy) / uK;\n"              /* game pixels */
    "  vec2 fg = fract(g);\n"
    "  vec2 px = scale * uK;\n"                     /* window pixels a game pixel */
    "  if (uMode == 1) {\n"
    "    vec3 l = pow(c, vec3(2.2));\n"
    "    l = mat3(0.82, 0.125, 0.195, 0.24, 0.665, 0.075, -0.06, 0.21, 0.73) * l;\n"
    "    c = pow(clamp(l, 0.0, 1.0), vec3(1.0 / 2.2));\n"
    "    float s = clamp((min(px.x, px.y) - 2.0) / 3.0, 0.0, 1.0);\n"
    "    vec2 e = clamp(min(fg, 1.0 - fg) * px, 0.0, 1.0);\n"
    "    c *= mix(1.0, (0.74 + 0.26 * e.x * e.y) * 1.08, s);\n"
    "  } else if (uMode == 2) {\n"
    "    float s = clamp((px.y - 1.5) / 2.5, 0.0, 1.0);\n"
    "    float d = abs(fg.y - 0.5) * 2.0;\n"
    "    c *= mix(1.0, (1.0 - 0.32 * d * d) * 1.1, s);\n"
    "  }\n"
    "  oColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint Shader(GLenum type, int es, const char *body)
{
    const char *srcs[3];
    GLuint sh = G(glCreateShader)(type);
    GLint ok = 0;

    srcs[0] = es ? "#version 300 es\n" : "#version 330 core\n";
    srcs[1] = es ? "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n" : "";
    srcs[2] = body;
    G(glShaderSource)(sh, 3, srcs, NULL);
    G(glCompileShader)(sh);
    G(glGetShaderiv)(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];

        G(glGetShaderInfoLog)(sh, sizeof(log), NULL, log);
        PortError("[katam-port] screen filter shader: %s", log);
        G(glDeleteShader)(sh);
        return 0;
    }
    return sh;
}

static int Ensure(int es)
{
    GLuint v, f;
    GLint ok = 0;

    if (sTried)
        return sProg != 0;
    sTried = 1;
    v = Shader(GL_VERTEX_SHADER, es, kVs);
    f = Shader(GL_FRAGMENT_SHADER, es, kFs);
    if (!v || !f)
        return 0;
    sProg = G(glCreateProgram)();
    G(glAttachShader)(sProg, v);
    G(glAttachShader)(sProg, f);
    G(glLinkProgram)(sProg);
    G(glDeleteShader)(v);
    G(glDeleteShader)(f);
    G(glGetProgramiv)(sProg, GL_LINK_STATUS, &ok);
    if (!ok) {
        PortError("[katam-port] screen filter: link failed; filters off");
        G(glDeleteProgram)(sProg);
        sProg = 0;
        return 0;
    }
    uTex = G(glGetUniformLocation)(sProg, "uTex");
    uSrc = G(glGetUniformLocation)(sProg, "uSrc");
    uDst = G(glGetUniformLocation)(sProg, "uDst");
    uK = G(glGetUniformLocation)(sProg, "uK");
    uMode = G(glGetUniformLocation)(sProg, "uMode");
    G(glGenVertexArrays)(1, &sVao);
    return 1;
}

int GlrFxDraw(int es, unsigned tex, const int src[4], int dx, int gy, int dw, int dh,
              int texelsPerPixel, int mode)
{
    if (mode <= 0 || !tex || !Ensure(es))
        return 0;
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    G(glDisable)(GL_BLEND);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glViewport)(dx, gy, dw, dh);
    G(glUseProgram)(sProg);
    G(glActiveTexture)(GL_TEXTURE0);
    G(glBindTexture)(GL_TEXTURE_2D, (GLuint)tex);
    G(glUniform1i)(uTex, 0);
    G(glUniform4f)(uSrc, (float)src[0], (float)src[1], (float)src[2], (float)src[3]);
    G(glUniform4f)(uDst, (float)dx, (float)gy, (float)dw, (float)dh);
    G(glUniform1f)(uK, (float)(texelsPerPixel > 0 ? texelsPerPixel : 1));
    G(glUniform1i)(uMode, mode == 3 ? 0 : mode);   /* SHARP: the sampling alone */
    G(glBindVertexArray)(sVao);
    G(glDrawArrays)(GL_TRIANGLES, 0, 3);
    G(glBindVertexArray)(0);
    G(glUseProgram)(0);
    return 1;
}
