/* The GL backend for the render list.
 *
 * Three passes per frame, all at the view's own resolution (game pixels):
 *
 *   1. OBJ: every sprite as an instanced quad into an RGBA8UI target, back to
 *      front with plain overwrite, so the front-most opaque sprite pixel wins
 *      whatever its priority -- the GBA's rule, which the PPU reproduces too.
 *      The texel holds the BGR555 colour and priority/semi-transparent bits.
 *      OBJ-window sprites go to a second, R8UI target.
 *   2. Composite: one full-view triangle.  Per pixel it evaluates the four
 *      background layers (tilemap -> tile -> palette, all integer texel
 *      fetches from the VRAM and palette caches), reads the OBJ texel, and
 *      resolves priority, windows, alpha blending and brightness exactly as
 *      platform/ppu.c does, in 5-bit integer arithmetic, so a 240x160 frame
 *      can be compared pixel for pixel.
 *   3. Present: a nearest-neighbour blit of the composite into the window at
 *      the largest integer scale that fits.
 *
 * Texture cache: VRAM (96 KiB as a 256x384 R8UI texture) is uploaded by
 * dirty 256-byte rows against a shadow of what the GPU has; palette RAM and
 * the per-line register table are small and go up every frame; each world
 * layer's room tilemap lives in a 1024-wide R16UI atlas, uploaded when the
 * map changes identity or content.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "render/gl_api.h"
#include "render/gl_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __EMSCRIPTEN__
#define GLR_DEFINE(ret, name, args) PFN_glr_##name glr_##name;
GLR_FUNCS(GLR_DEFINE)
#undef GLR_DEFINE
#endif

#define G(name) GLR_FN(name)

#define VRAM_BYTES 0x18000
#define VRAM_TEX_W 256
#define VRAM_TEX_H (VRAM_BYTES / VRAM_TEX_W)
#define MAP_TEX_W 1024

static int sReady;
static int sEs;
static char sDesc[256];

static GLuint sProgObj, sProgComp;
static GLuint sVaoObj, sVaoEmpty, sInstBuf;
static GLuint sTexVram, sTexPal, sTexLines, sTexMap;
static GLuint sTexObj, sTexObjWin, sTexComp;
static GLuint sFboObj, sFboObjWin, sFboComp;
static int sFboW, sFboH;

static u8 sVramShadow[VRAM_BYTES];
static int sVramValid;

static u16 *sMapShadow;
static int sMapRows;            /* allocated texture rows                      */
static int sMapBase[4];         /* linear entry index of each layer's map     */
static u32 sMapGen[4];
static const u16 *sMapPtr[4];
static int sMapLen[4];
static int sMaxTex = 2048;

static s32 *sInst;
static int sInstCap;

static int sLastW = 240, sLastH = 160;

/* Uniform locations. */
static GLint uObjView, uObjWinPass;
static GLint uCView, uCContent, uCLayer, uCValid, uCMapGeom, uCMapScroll,
             uCNumHud, uCHudRect, uCHudD;

/* --- shaders ------------------------------------------------------------- */

static const char *kCommon =
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp usampler2D;\n"
    "precision highp isampler2D;\n"
    "uniform usampler2D uVram;\n"
    "uniform usampler2D uPal;\n"
    "int vb(int a) { return int(texelFetch(uVram, ivec2(a & 255, a >> 8), 0).r); }\n"
    "int pal(int i) { return int(texelFetch(uPal, ivec2(i, 0), 0).r); }\n";

static const char *kObjVs =
    "layout(location = 0) in ivec4 aA;\n"   /* x y boxW boxH, view coords */
    "layout(location = 1) in ivec4 aB;\n"   /* w h tile flags            */
    "layout(location = 2) in ivec4 aC;\n"   /* pa pb pc pd               */
    "uniform ivec2 uViewSize;\n"
    "flat out ivec4 vA;\n"
    "flat out ivec4 vB;\n"
    "flat out ivec4 vC;\n"
    "void main() {\n"
    "  int c = gl_VertexID;\n"
    "  ivec2 corner = ivec2(c & 1, c >> 1);\n"
    "  vec2 p = vec2(aA.xy + corner * aA.zw);\n"
    "  gl_Position = vec4(p / vec2(uViewSize) * 2.0 - 1.0, 0.0, 1.0);\n"
    "  vA = aA; vB = aB; vC = aC;\n"
    "}\n";

/* flags: 0-3 palette bank, 4 8bpp, 5 hflip, 6 vflip, 7 affine, 8-9
 * priority, 10 semi-transparent, 12 1D mapping. */
static const char *kObjFs =
    "flat in ivec4 vA;\n"
    "flat in ivec4 vB;\n"
    "flat in ivec4 vC;\n"
    "uniform int uWinPass;\n"
    "out uvec4 oObj;\n"
    "void main() {\n"
    "  ivec2 q = ivec2(gl_FragCoord.xy) - vA.xy;\n"
    "  int px = q.x, row = q.y;\n"
    "  int w = vB.x, h = vB.y, tile = vB.z, fl = vB.w;\n"
    "  bool is256 = (fl & 16) != 0;\n"
    "  int texX, texY;\n"
    "  if ((fl & 128) != 0) {\n"
    "    int dx = px - vA.z / 2, dy = row - vA.w / 2;\n"
    "    texX = ((vC.x * dx + vC.y * dy) >> 8) + w / 2;\n"
    "    texY = ((vC.z * dx + vC.w * dy) >> 8) + h / 2;\n"
    "  } else {\n"
    "    texX = (fl & 32) != 0 ? w - 1 - px : px;\n"
    "    texY = (fl & 64) != 0 ? h - 1 - row : row;\n"
    "  }\n"
    "  if (texX < 0 || texY < 0 || texX >= w || texY >= h) discard;\n"
    "  int m = is256 ? 2 : 1;\n"
    "  int ti = (fl & 4096) != 0 ? tile + (texY / 8) * (w / 8) * m + (texX / 8) * m\n"
    "                            : tile + (texY / 8) * 32 + (texX / 8) * m;\n"
    "  int col;\n"
    "  if (is256) {\n"
    "    int off = 0x10000 + ((ti & ~1) >> 1) * 64 + (texY & 7) * 8 + (texX & 7);\n"
    "    if (off >= 0x18000) discard;\n"
    "    int idx = vb(off);\n"
    "    if (idx == 0) discard;\n"
    "    col = pal(256 + idx);\n"
    "  } else {\n"
    "    int off = 0x10000 + ti * 32 + (texY & 7) * 4 + ((texX & 7) >> 1);\n"
    "    if (off >= 0x18000) discard;\n"
    "    int idx = vb(off);\n"
    "    idx = (texX & 1) != 0 ? (idx >> 4) : (idx & 15);\n"
    "    if (idx == 0) discard;\n"
    "    col = pal(256 + (((fl & 15) * 16 + idx) & 255));\n"
    "  }\n"
    "  if (uWinPass != 0) { oObj = uvec4(1u); return; }\n"
    "  oObj = uvec4(uint(col & 255), uint(col >> 8),\n"
    "               uint(((fl >> 8) & 3) | (((fl >> 10) & 1) << 2) | 8), 0u);\n"
    "}\n";

static const char *kCompVs =
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexID & 1) * 4) - 1.0, float((gl_VertexID & 2) * 2) - 1.0);\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *kCompFs =
    "uniform isampler2D uLines;\n"
    "uniform usampler2D uObj;\n"
    "uniform usampler2D uObjWin;\n"
    "uniform usampler2D uMap;\n"
    "uniform ivec4 uView;\n"          /* viewX viewY viewW viewH (screen) */
    "uniform ivec4 uContent;\n"       /* l t r b (screen)                */
    "uniform ivec4 uLayer[4];\n"      /* space, streamed, shiftX, shiftY */
    "uniform ivec4 uValid[4];\n"      /* l r t b                         */
    "uniform ivec4 uMapGeom[4];\n"    /* w h base -                      */
    "uniform ivec4 uMapScroll[4];\n"  /* scrollX scrollY offX offY       */
    "uniform int uNumHud;\n"
    "uniform ivec4 uHudRect[8];\n"
    "uniform ivec2 uHudD[8];\n"
    "out vec4 oColor;\n"
    "int L(int line, int w) {\n"
    "  ivec4 t = texelFetch(uLines, ivec2(w >> 2, line), 0);\n"
    "  int c = w & 3;\n"
    "  return c == 0 ? t.x : c == 1 ? t.y : c == 2 ? t.z : t.w;\n"
    "}\n"
    "int imod(int a, int b) { int r = a - (a / b) * b; return r < 0 ? r + b : r; }\n"
    "int mapEntry(int bg, int idx) {\n"
    "  int a = uMapGeom[bg].z + idx;\n"
    "  return int(texelFetch(uMap, ivec2(a & 1023, a >> 10), 0).r);\n"
    "}\n"
    "int sampleText(int bg, int x, int y, int rl) {\n"
    "  int cnt = L(rl, 27 + bg);\n"
    "  int charBase = ((cnt >> 2) & 3) * 0x4000;\n"
    "  int screenBase = ((cnt >> 8) & 31) * 0x800;\n"
    "  bool is256 = (cnt & 0x80) != 0;\n"
    "  int size = (cnt >> 14) & 3;\n"
    "  int wT = (size & 1) != 0 ? 64 : 32;\n"
    "  int hT = (size & 2) != 0 ? 64 : 32;\n"
    "  int hofs = L(rl, bg * 2), vofs = L(rl, bg * 2 + 1);\n"
    "  if ((cnt & 0x40) != 0) {\n"
    "    int mv = ((L(rl, 26) >> 4) & 15) + 1;\n"
    "    y = y - imod(y, mv);\n"
    "  }\n"
    "  int sy = (y + vofs) & (hT * 8 - 1);\n"
    "  int sx = (x + hofs) & (wT * 8 - 1);\n"
    "  int entry;\n"
    "  bool outside = false;\n"
    "  int st = uLayer[bg].y;\n"
    "  if (st != 0)\n"
    "    outside = x < uValid[bg].x || x >= uValid[bg].y || y < uValid[bg].z || y >= uValid[bg].w;\n"
    "  if (outside) {\n"
    "    if (st != 1) return -1;\n"
    "    int r = uMapScroll[bg].w + ((uMapScroll[bg].y + y) >> 3);\n"
    "    int c = uMapScroll[bg].z + ((uMapScroll[bg].x + x) >> 3);\n"
    "    if (r < 0 || r >= uMapGeom[bg].y || c < 0 || c >= uMapGeom[bg].x) return -1;\n"
    "    entry = mapEntry(bg, r * uMapGeom[bg].x + c);\n"
    "  } else {\n"
    "    int tx = sx >> 3, ty = sy >> 3;\n"
    "    int mo = screenBase;\n"
    "    if (tx >= 32) { mo += 0x800; tx -= 32; }\n"
    "    if (ty >= 32) { mo += wT > 32 ? 0x1000 : 0x800; ty -= 32; }\n"
    "    mo += (ty * 32 + tx) * 2;\n"
    "    if (mo + 1 >= 0x18000) return -1;\n"
    "    entry = vb(mo) | (vb(mo + 1) << 8);\n"
    "  }\n"
    "  int tile = entry & 0x3FF;\n"
    "  int px = sx & 7, py = sy & 7;\n"
    "  if ((entry & 0x400) != 0) px = 7 - px;\n"
    "  if ((entry & 0x800) != 0) py = 7 - py;\n"
    "  if (is256) {\n"
    "    int off = charBase + tile * 64 + py * 8 + px;\n"
    "    if (off >= 0x18000) return -1;\n"
    "    int idx = vb(off);\n"
    "    return idx == 0 ? -1 : pal(idx);\n"
    "  }\n"
    "  int off = charBase + tile * 32 + py * 4 + (px >> 1);\n"
    "  if (off >= 0x18000) return -1;\n"
    "  int idx = vb(off);\n"
    "  idx = (px & 1) != 0 ? (idx >> 4) : (idx & 15);\n"
    "  if (idx == 0) return -1;\n"
    "  return pal((((entry >> 12) & 15) * 16 + idx) & 255);\n"
    "}\n"
    "int sampleAffine(int bg, int x, int y, int rl) {\n"
    "  int cnt = L(rl, 27 + bg);\n"
    "  int b = bg == 2 ? 8 : 12;\n"
    "  int cx = L(rl, b), cy = L(rl, b + 1), pa = L(rl, b + 2), pc = L(rl, b + 3);\n"
    "  cx += L(rl, 32 + (bg - 2) * 2) * (y - rl);\n"
    "  cy += L(rl, 33 + (bg - 2) * 2) * (y - rl);\n"
    "  int charBase = ((cnt >> 2) & 3) * 0x4000;\n"
    "  int screenBase = ((cnt >> 8) & 31) * 0x800;\n"
    "  int sizeTiles = 16 << ((cnt >> 14) & 3);\n"
    "  int sizePx = sizeTiles * 8;\n"
    "  int tx = (cx + pa * x) >> 8, ty = (cy + pc * x) >> 8;\n"
    "  if ((cnt & 0x2000) != 0) { tx &= sizePx - 1; ty &= sizePx - 1; }\n"
    "  else if (tx < 0 || ty < 0 || tx >= sizePx || ty >= sizePx) return -1;\n"
    "  int mo = screenBase + (ty >> 3) * sizeTiles + (tx >> 3);\n"
    "  if (mo >= 0x18000) return -1;\n"
    "  int tile = vb(mo);\n"
    "  int off = charBase + tile * 64 + (ty & 7) * 8 + (tx & 7);\n"
    "  if (off >= 0x18000) return -1;\n"
    "  int idx = vb(off);\n"
    "  return idx == 0 ? -1 : pal(idx);\n"
    "}\n"
    "int windowControl(int sx, int sy, int rl, int dispcnt, bool objwin) {\n"
    "  if ((dispcnt & 0xE000) == 0) return 0x3F;\n"
    "  int winin = L(rl, 24), winout = L(rl, 25);\n"
    "  for (int w = 0; w < 2; w++) {\n"
    "    if ((dispcnt & (0x2000 << w)) == 0) continue;\n"
    "    int h = L(rl, 20 + w), v = L(rl, 22 + w);\n"
    "    int left = h >> 8, right = h & 255, top = v >> 8, bottom = v & 255;\n"
    "    if (right > 240 || right < left) right = 240;\n"
    "    if (bottom > 160 || bottom < top) bottom = 160;\n"
    "    if (left <= 0) left = uView.x;\n"
    "    if (right >= 240) right = uView.x + uView.z;\n"
    "    if (top <= 0) top = uView.y;\n"
    "    if (bottom >= 160) bottom = uView.y + uView.w;\n"
    "    if (sx >= left && sx < right && sy >= top && sy < bottom)\n"
    "      return (winin >> (w * 8)) & 0x3F;\n"
    "  }\n"
    "  if ((dispcnt & 0x8000) != 0 && objwin) return (winout >> 8) & 0x3F;\n"
    "  return winout & 0x3F;\n"
    "}\n"
    "int blendAlpha(int a, int b, int eva, int evb) {\n"
    "  int r = min(((a & 31) * eva + (b & 31) * evb) >> 4, 31);\n"
    "  int g = min((((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4, 31);\n"
    "  int bl = min((((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4, 31);\n"
    "  return r | (g << 5) | (bl << 10);\n"
    "}\n"
    "int bright(int c, int evy, bool up) {\n"
    "  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;\n"
    "  if (up) { r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4; }\n"
    "  else { r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4; }\n"
    "  return r | (g << 5) | (b << 10);\n"
    "}\n"
    "vec4 toRgba(int c) {\n"
    "  int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;\n"
    "  r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);\n"
    "  return vec4(float(r), float(g), float(b), 255.0) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "  ivec2 v = ivec2(gl_FragCoord.xy);\n"
    "  int sx = v.x + uView.x, sy = v.y + uView.y;\n"
    "  if (sx < uContent.x || sy < uContent.y || sx >= uContent.z || sy >= uContent.w) {\n"
    "    oColor = vec4(0.0, 0.0, 0.0, 1.0); return;\n"
    "  }\n"
    "  int rl = clamp(sy, 0, 159);\n"
    "  int dispcnt = L(rl, 16);\n"
    "  if ((dispcnt & 0x80) != 0) { oColor = vec4(1.0); return; }\n"
    "  int mode = dispcnt & 7;\n"
    "  int lc[4]; int lp[4];\n"
    "  for (int bg = 0; bg < 4; bg++) {\n"
    "    lc[bg] = -1;\n"
    "    lp[bg] = L(rl, 27 + bg) & 3;\n"
    "    if ((dispcnt & (0x100 << bg)) == 0) continue;\n"
    "    int x = sx + uLayer[bg].z, y = sy + uLayer[bg].w;\n"
    "    if (uLayer[bg].x == 2) {\n"
    "      bool found = false;\n"
    "      for (int r = 0; r < 8; r++) {\n"
    "        if (r >= uNumHud) break;\n"
    "        int hx = v.x - uHudD[r].x, hy = v.y - uHudD[r].y;\n"
    "        if (hx >= uHudRect[r].x && hy >= uHudRect[r].y && hx < uHudRect[r].z && hy < uHudRect[r].w) {\n"
    "          x = hx; y = hy; found = true; break;\n"
    "        }\n"
    "      }\n"
    "      if (!found) continue;\n"
    "    }\n"
    "    if (mode == 0 || (mode == 1 && bg < 2)) lc[bg] = sampleText(bg, x, y, rl);\n"
    "    else if ((mode == 1 && bg == 2) || (mode == 2 && bg >= 2)) lc[bg] = sampleAffine(bg, x, y, rl);\n"
    "  }\n"
    "  bool objOn = (dispcnt & 0x1000) != 0;\n"
    "  uvec4 o = texelFetch(uObj, v, 0);\n"
    "  bool objOpq = objOn && (o.b & 8u) != 0u;\n"
    "  int objPri = objOpq ? int(o.b & 3u) : 3;\n"
    "  bool semi = (o.b & 4u) != 0u;\n"
    "  int objCol = int(o.r) | (int(o.g) << 8);\n"
    "  bool objwin = objOn && texelFetch(uObjWin, v, 0).r != 0u;\n"
    "  int ctrl = windowControl(sx, sy, rl, dispcnt, objwin);\n"
    "  bool allowBlend = (ctrl & 0x20) != 0;\n"
    "  int bd = pal(0);\n"
    "  int top = -1, second = -1, topC = bd, secC = bd;\n"
    "  bool topBlend = true;\n"
    "  for (int p = 0; p < 4; p++) {\n"
    "    if (second >= 0) break;\n"
    "    for (int layer = 0; layer < 5; layer++) {\n"
    "      int idx = layer == 0 ? 4 : layer - 1;\n"
    "      int lpri = idx == 4 ? objPri : lp[idx];\n"
    "      bool opq = idx == 4 ? objOpq : lc[idx] >= 0;\n"
    "      if (lpri != p || !opq) continue;\n"
    "      if (((ctrl >> idx) & 1) == 0) continue;\n"
    "      int c = idx == 4 ? objCol : lc[idx];\n"
    "      if (top < 0) { top = idx; topC = c; topBlend = allowBlend; }\n"
    "      else { second = idx; secC = c; break; }\n"
    "    }\n"
    "  }\n"
    "  int result = top < 0 ? bd : topC;\n"
    "  if (topBlend && top >= 0) {\n"
    "    int bldcnt = L(rl, 17), alpha = L(rl, 18), bldy = L(rl, 19);\n"
    "    int effect = (bldcnt >> 6) & 3;\n"
    "    int eva = min(alpha & 31, 16), evb = min((alpha >> 8) & 31, 16), evy = min(bldy & 31, 16);\n"
    "    int secBit = second < 0 ? 5 : second;\n"
    "    bool t1 = ((bldcnt >> top) & 1) != 0;\n"
    "    bool t2 = ((bldcnt >> (8 + secBit)) & 1) != 0;\n"
    "    if (top == 4 && semi && t2) result = blendAlpha(topC, secC, eva, evb);\n"
    "    else if (effect == 1 && t1 && t2) result = blendAlpha(topC, secC, eva, evb);\n"
    "    else if (effect == 2 && t1) result = bright(topC, evy, true);\n"
    "    else if (effect == 3 && t1) result = bright(topC, evy, false);\n"
    "  }\n"
    "  oColor = toRgba(result);\n"
    "}\n";

static GLuint Compile(GLenum type, const char *body, const char *what)
{
    const char *srcs[3];
    GLuint sh = G(glCreateShader)(type);
    GLint ok = 0;

    srcs[0] = sEs ? "#version 300 es\n" : "#version 330 core\n";
    srcs[1] = kCommon;
    srcs[2] = body;
    G(glShaderSource)(sh, 3, srcs, NULL);
    G(glCompileShader)(sh);
    G(glGetShaderiv)(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        G(glGetShaderInfoLog)(sh, sizeof(log), NULL, log);
        PortError("[katam-port] gpu: %s shader: %s", what, log);
        G(glDeleteShader)(sh);
        return 0;
    }
    return sh;
}

static GLuint Link(const char *vs, const char *fs, const char *what)
{
    GLuint v = Compile(GL_VERTEX_SHADER, vs, what);
    GLuint f = Compile(GL_FRAGMENT_SHADER, fs, what);
    GLuint p;
    GLint ok = 0;

    if (!v || !f)
        return 0;
    p = G(glCreateProgram)();
    G(glAttachShader)(p, v);
    G(glAttachShader)(p, f);
    G(glLinkProgram)(p);
    G(glGetProgramiv)(p, GL_LINK_STATUS, &ok);
    G(glDeleteShader)(v);
    G(glDeleteShader)(f);
    if (!ok) {
        char log[4096];
        G(glGetProgramInfoLog)(p, sizeof(log), NULL, log);
        PortError("[katam-port] gpu: %s link: %s", what, log);
        return 0;
    }
    return p;
}

static GLuint MakeTex(GLenum ifmt, GLenum fmt, GLenum type, int w, int h)
{
    GLuint t;

    G(glGenTextures)(1, &t);
    G(glBindTexture)(GL_TEXTURE_2D, t);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G(glTexImage2D)(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, NULL);
    return t;
}

static void SetSamplers(GLuint prog)
{
    static const char *const kNames[] = { "uVram", "uPal", "uLines", "uObj",
                                          "uObjWin", "uMap" };
    int i;

    G(glUseProgram)(prog);
    for (i = 0; i < 6; i++) {
        GLint loc = G(glGetUniformLocation)(prog, kNames[i]);
        if (loc >= 0)
            G(glUniform1i)(loc, i);
    }
}

int GlrInit(void *(*getProc)(const char *), int es)
{
    sEs = es;
#ifndef __EMSCRIPTEN__
#define GLR_LOAD(ret, name, args) \
    glr_##name = (PFN_glr_##name)getProc(#name); \
    if (glr_##name == NULL) { \
        PortError("[katam-port] gpu: no %s in this GL", #name); \
        return 0; \
    }
    GLR_FUNCS(GLR_LOAD)
#undef GLR_LOAD
#else
    (void)getProc;
#endif

    snprintf(sDesc, sizeof(sDesc), "%s / %s / %s",
             (const char *)G(glGetString)(GL_VERSION),
             (const char *)G(glGetString)(GL_RENDERER),
             (const char *)G(glGetString)(GL_SHADING_LANGUAGE_VERSION));
    G(glGetIntegerv)(GL_MAX_TEXTURE_SIZE, &sMaxTex);

    sProgObj = Link(kObjVs, kObjFs, "sprite");
    sProgComp = Link(kCompVs, kCompFs, "composite");
    if (!sProgObj || !sProgComp)
        return 0;
    SetSamplers(sProgObj);
    uObjView = G(glGetUniformLocation)(sProgObj, "uViewSize");
    uObjWinPass = G(glGetUniformLocation)(sProgObj, "uWinPass");
    SetSamplers(sProgComp);
    uCView = G(glGetUniformLocation)(sProgComp, "uView");
    uCContent = G(glGetUniformLocation)(sProgComp, "uContent");
    uCLayer = G(glGetUniformLocation)(sProgComp, "uLayer");
    uCValid = G(glGetUniformLocation)(sProgComp, "uValid");
    uCMapGeom = G(glGetUniformLocation)(sProgComp, "uMapGeom");
    uCMapScroll = G(glGetUniformLocation)(sProgComp, "uMapScroll");
    uCNumHud = G(glGetUniformLocation)(sProgComp, "uNumHud");
    uCHudRect = G(glGetUniformLocation)(sProgComp, "uHudRect");
    uCHudD = G(glGetUniformLocation)(sProgComp, "uHudD");

    G(glPixelStorei)(GL_UNPACK_ALIGNMENT, 1);
    G(glPixelStorei)(GL_PACK_ALIGNMENT, 1);
    sTexVram = MakeTex(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, VRAM_TEX_W, VRAM_TEX_H);
    sTexPal = MakeTex(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 512, 1);
    sTexLines = MakeTex(GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, RL_LINE_WORDS / 4, RL_LINES);
    sMapRows = 1;
    sTexMap = MakeTex(GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, MAP_TEX_W, sMapRows);

    G(glGenVertexArrays)(1, &sVaoEmpty);
    G(glGenVertexArrays)(1, &sVaoObj);
    G(glGenBuffers)(1, &sInstBuf);
    G(glBindVertexArray)(sVaoObj);
    G(glBindBuffer)(GL_ARRAY_BUFFER, sInstBuf);
    {
        int a;
        for (a = 0; a < 3; a++) {
            G(glEnableVertexAttribArray)((GLuint)a);
            G(glVertexAttribIPointer)((GLuint)a, 4, GL_INT, 48, (const void *)(uintptr_t)(a * 16));
            G(glVertexAttribDivisor)((GLuint)a, 1);
        }
    }
    G(glBindVertexArray)(0);

    sReady = 1;
    PortLog("[katam-port] gpu renderer: %s", sDesc);
    return 1;
}

const char *GlrDescribe(void)
{
    return sDesc;
}

static void EnsureTargets(int w, int h)
{
    if (w == sFboW && h == sFboH)
        return;
    if (sFboObj) {
        G(glDeleteFramebuffers)(1, &sFboObj);
        G(glDeleteFramebuffers)(1, &sFboObjWin);
        G(glDeleteFramebuffers)(1, &sFboComp);
        G(glDeleteTextures)(1, &sTexObj);
        G(glDeleteTextures)(1, &sTexObjWin);
        G(glDeleteTextures)(1, &sTexComp);
    }
    sTexObj = MakeTex(GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, w, h);
    sTexObjWin = MakeTex(GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, w, h);
    sTexComp = MakeTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h);
    G(glGenFramebuffers)(1, &sFboObj);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, sFboObj);
    G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sTexObj, 0);
    G(glGenFramebuffers)(1, &sFboObjWin);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, sFboObjWin);
    G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sTexObjWin, 0);
    G(glGenFramebuffers)(1, &sFboComp);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, sFboComp);
    G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sTexComp, 0);
    if (G(glCheckFramebufferStatus)(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        PortError("[katam-port] gpu: composite target incomplete");
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    sFboW = w;
    sFboH = h;
}

static void UploadVram(const u8 *vram)
{
    int row = 0;

    G(glBindTexture)(GL_TEXTURE_2D, sTexVram);
    if (!sVramValid) {
        memcpy(sVramShadow, vram, VRAM_BYTES);
        G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, VRAM_TEX_W, VRAM_TEX_H,
                           GL_RED_INTEGER, GL_UNSIGNED_BYTE, sVramShadow);
        sVramValid = 1;
        return;
    }
    while (row < VRAM_TEX_H) {
        int start;

        if (memcmp(sVramShadow + row * VRAM_TEX_W, vram + row * VRAM_TEX_W, VRAM_TEX_W) == 0) {
            row++;
            continue;
        }
        start = row;
        while (row < VRAM_TEX_H
               && memcmp(sVramShadow + row * VRAM_TEX_W, vram + row * VRAM_TEX_W, VRAM_TEX_W) != 0)
            row++;
        memcpy(sVramShadow + start * VRAM_TEX_W, vram + start * VRAM_TEX_W,
               (size_t)(row - start) * VRAM_TEX_W);
        G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, start, VRAM_TEX_W, row - start,
                           GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                           sVramShadow + start * VRAM_TEX_W);
    }
}

/* Lay out the streamed layers' maps in the atlas; upload what changed. */
static void UploadMaps(const RlFrame *f)
{
    int bg, need = 0, relayout = 0;
    int base[4];

    for (bg = 0; bg < 4; bg++) {
        const RlTileLayer *L = &f->layers[bg];
        int len = (L->streamed && L->map) ? L->mapW * L->mapH : 0;

        base[bg] = need;
        need += (len + MAP_TEX_W - 1) / MAP_TEX_W * MAP_TEX_W;
        if (len != sMapLen[bg] || L->map != sMapPtr[bg] || (len && L->mapGen != sMapGen[bg]))
            relayout = 1;
    }
    if (need / MAP_TEX_W > sMaxTex)
        need = 0;                       /* too big: layers fall back below */
    G(glBindTexture)(GL_TEXTURE_2D, sTexMap);
    if (need / MAP_TEX_W > sMapRows || sMapShadow == NULL) {
        if (need / MAP_TEX_W > sMapRows)
            sMapRows = need / MAP_TEX_W;
        G(glTexImage2D)(GL_TEXTURE_2D, 0, GL_R16UI, MAP_TEX_W, sMapRows, 0,
                        GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
        sMapShadow = (u16 *)realloc(sMapShadow, (size_t)sMapRows * MAP_TEX_W * 2);
        relayout = 1;
    }
    for (bg = 0; bg < 4; bg++) {
        const RlTileLayer *L = &f->layers[bg];
        int len = (L->streamed && L->map && need) ? L->mapW * L->mapH : 0;
        int rows;

        sMapBase[bg] = base[bg];
        if (len == 0) {
            sMapLen[bg] = 0;
            sMapPtr[bg] = NULL;
            continue;
        }
        if (!relayout && memcmp(sMapShadow + base[bg], L->map, (size_t)len * 2) == 0)
            continue;
        memcpy(sMapShadow + base[bg], L->map, (size_t)len * 2);
        rows = (len + MAP_TEX_W - 1) / MAP_TEX_W;
        G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, base[bg] / MAP_TEX_W, MAP_TEX_W, rows,
                           GL_RED_INTEGER, GL_UNSIGNED_SHORT, sMapShadow + base[bg]);
        sMapLen[bg] = len;
        sMapPtr[bg] = L->map;
        sMapGen[bg] = L->mapGen;
    }
}

static const u8 kObjW[4][4] = { {8,16,32,64}, {16,32,32,64}, {8,8,16,32}, {8,8,8,8} };
static const u8 kObjH[4][4] = { {8,16,32,64}, {8,8,16,32}, {16,32,32,64}, {8,8,8,8} };

/* Fill the instance buffer back to front; returns the count. */
static int BuildInstances(const RlFrame *f, u32 from, u32 count)
{
    int n = 0;
    u32 i;

    if ((int)count > sInstCap) {
        sInstCap = (int)count + 256;
        sInst = (s32 *)realloc(sInst, (size_t)sInstCap * 12 * sizeof(s32));
    }
    for (i = count; i-- > 0;) {
        const RlSprite *s = &f->sprites[from + i];
        u32 shape = (s->attr0 >> 14) & 3, size = (s->attr1 >> 14) & 3;
        s32 w = kObjW[shape][size], h = kObjH[shape][size];
        int affine = (s->attr0 & 0x100) != 0;
        int dbl = affine && (s->attr0 & 0x200);
        s32 bw = w << dbl, bh = h << dbl, x, y;
        s32 *o = sInst + n * 12;
        int flags;

        if (s->space == RL_SPACE_HUD) {
            x = s->x + f->hud[s->anchor].dx;
            y = s->y + f->hud[s->anchor].dy;
        } else {
            x = s->x - f->viewX;
            y = s->y - f->viewY;
        }
        if (x >= f->viewW || y >= f->viewH || x + bw <= 0 || y + bh <= 0)
            continue;
        flags = (s->attr2 >> 12) & 15;
        if (s->attr0 & 0x2000) flags |= 16;
        if (!affine && (s->attr1 & 0x1000)) flags |= 32;
        if (!affine && (s->attr1 & 0x2000)) flags |= 64;
        if (affine) flags |= 128;
        flags |= ((s->attr2 >> 10) & 3) << 8;
        if (((s->attr0 >> 10) & 3) == 1) flags |= 1024;
        if (f->objOneD) flags |= 4096;
        o[0] = x; o[1] = y; o[2] = bw; o[3] = bh;
        o[4] = w; o[5] = h; o[6] = s->attr2 & 0x3FF; o[7] = flags;
        o[8] = s->pa; o[9] = s->pb; o[10] = s->pc; o[11] = s->pd;
        n++;
    }
    return n;
}

static void DrawSprites(const RlFrame *f, GLuint fbo, u32 from, u32 count, int winPass)
{
    static const GLuint kZero[4] = { 0, 0, 0, 0 };
    int n;

    G(glBindFramebuffer)(GL_FRAMEBUFFER, fbo);
    G(glViewport)(0, 0, f->viewW, f->viewH);
    G(glClearBufferuiv)(GL_COLOR, 0, kZero);
    n = BuildInstances(f, from, count);
    if (n == 0)
        return;
    G(glUseProgram)(sProgObj);
    G(glUniform2i)(uObjView, f->viewW, f->viewH);
    G(glUniform1i)(uObjWinPass, winPass);
    G(glBindVertexArray)(sVaoObj);
    G(glBindBuffer)(GL_ARRAY_BUFFER, sInstBuf);
    G(glBufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)n * 48, sInst, GL_STREAM_DRAW);
    G(glDrawArraysInstanced)(GL_TRIANGLE_STRIP, 0, 4, n);
    G(glBindVertexArray)(0);
}

void GlrDraw(const RlFrame *f)
{
    GLint layer[16], valid[16], geom[16], scroll[16], hudRect[32], hudD[16];
    int bg, r;

    if (!sReady)
        return;
    EnsureTargets(f->viewW, f->viewH);
    G(glDisable)(GL_BLEND);
    G(glDisable)(GL_DEPTH_TEST);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glDisable)(GL_CULL_FACE);

    /* Texture cache. */
    G(glActiveTexture)(GL_TEXTURE0 + 0);
    UploadVram(f->vram);
    G(glActiveTexture)(GL_TEXTURE0 + 1);
    G(glBindTexture)(GL_TEXTURE_2D, sTexPal);
    G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, 512, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, f->palette);
    G(glActiveTexture)(GL_TEXTURE0 + 2);
    G(glBindTexture)(GL_TEXTURE_2D, sTexLines);
    G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, RL_LINE_WORDS / 4, RL_LINES, GL_RGBA_INTEGER, GL_INT, f->lines);
    G(glActiveTexture)(GL_TEXTURE0 + 5);
    UploadMaps(f);
    G(glActiveTexture)(GL_TEXTURE0 + 0);
    G(glBindTexture)(GL_TEXTURE_2D, sTexVram);
    G(glActiveTexture)(GL_TEXTURE0 + 1);
    G(glBindTexture)(GL_TEXTURE_2D, sTexPal);
    G(glActiveTexture)(GL_TEXTURE0 + 2);
    G(glBindTexture)(GL_TEXTURE_2D, sTexLines);
    /* The sprite targets must not be bound as textures while drawn into. */
    G(glActiveTexture)(GL_TEXTURE0 + 3);
    G(glBindTexture)(GL_TEXTURE_2D, 0);
    G(glActiveTexture)(GL_TEXTURE0 + 4);
    G(glBindTexture)(GL_TEXTURE_2D, 0);

    /* Pass 1: sprites. */
    DrawSprites(f, sFboObj, 0, f->numSprites, 0);
    DrawSprites(f, sFboObjWin, f->numSprites, f->numObjWin, 1);

    /* Pass 2: composite. */
    for (bg = 0; bg < 4; bg++) {
        const RlTileLayer *L = &f->layers[bg];
        int streamed = 0;

        if (L->streamed)
            streamed = (L->map && sMapLen[bg]) ? 1 : 2;
        layer[bg * 4 + 0] = L->space;
        layer[bg * 4 + 1] = streamed;
        layer[bg * 4 + 2] = L->shiftX;
        layer[bg * 4 + 3] = L->shiftY;
        valid[bg * 4 + 0] = L->validL;
        valid[bg * 4 + 1] = L->validR;
        valid[bg * 4 + 2] = L->validT;
        valid[bg * 4 + 3] = L->validB;
        geom[bg * 4 + 0] = L->mapW;
        geom[bg * 4 + 1] = L->mapH;
        geom[bg * 4 + 2] = sMapBase[bg];
        geom[bg * 4 + 3] = 0;
        scroll[bg * 4 + 0] = L->mapScrollX;
        scroll[bg * 4 + 1] = L->mapScrollY;
        scroll[bg * 4 + 2] = L->mapOffX;
        scroll[bg * 4 + 3] = L->mapOffY;
    }
    memset(hudRect, 0, sizeof(hudRect));
    memset(hudD, 0, sizeof(hudD));
    for (r = 0; r < f->numHud && r < 8; r++) {
        hudRect[r * 4 + 0] = f->hud[r].x0;
        hudRect[r * 4 + 1] = f->hud[r].y0;
        hudRect[r * 4 + 2] = f->hud[r].x1;
        hudRect[r * 4 + 3] = f->hud[r].y1;
        hudD[r * 2 + 0] = f->hud[r].dx;
        hudD[r * 2 + 1] = f->hud[r].dy;
    }
    G(glBindFramebuffer)(GL_FRAMEBUFFER, sFboComp);
    G(glViewport)(0, 0, f->viewW, f->viewH);
    G(glUseProgram)(sProgComp);
    G(glActiveTexture)(GL_TEXTURE0 + 3);
    G(glBindTexture)(GL_TEXTURE_2D, sTexObj);
    G(glActiveTexture)(GL_TEXTURE0 + 4);
    G(glBindTexture)(GL_TEXTURE_2D, sTexObjWin);
    G(glActiveTexture)(GL_TEXTURE0 + 5);
    G(glBindTexture)(GL_TEXTURE_2D, sTexMap);
    G(glUniform4i)(uCView, f->viewX, f->viewY, f->viewW, f->viewH);
    G(glUniform4i)(uCContent, f->contentL, f->contentT, f->contentR, f->contentB);
    G(glUniform4iv)(uCLayer, 4, layer);
    G(glUniform4iv)(uCValid, 4, valid);
    G(glUniform4iv)(uCMapGeom, 4, geom);
    G(glUniform4iv)(uCMapScroll, 4, scroll);
    G(glUniform1i)(uCNumHud, f->numHud);
    G(glUniform4iv)(uCHudRect, 8, hudRect);
    G(glUniform2iv)(uCHudD, 8, hudD);
    G(glBindVertexArray)(sVaoEmpty);
    G(glDrawArrays)(GL_TRIANGLES, 0, 3);
    G(glBindVertexArray)(0);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    sLastW = f->viewW;
    sLastH = f->viewH;
}

void GlrPresent(int w, int h)
{
    int s, dw, dh, x0, y0;

    if (!sReady || !sFboComp)
        return;
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    G(glViewport)(0, 0, w, h);
    G(glClearColor)(0.0f, 0.0f, 0.0f, 1.0f);
    G(glClear)(GL_COLOR_BUFFER_BIT);
    s = w / sLastW < h / sLastH ? w / sLastW : h / sLastH;
    if (s >= 1) {
        dw = sLastW * s;
        dh = sLastH * s;
    } else if ((long)w * sLastH < (long)h * sLastW) {
        dw = w;
        dh = (int)((long)sLastH * w / sLastW);
    } else {
        dh = h;
        dw = (int)((long)sLastW * h / sLastH);
    }
    x0 = (w - dw) / 2;
    y0 = (h - dh) / 2;
    G(glBindFramebuffer)(GL_READ_FRAMEBUFFER, sFboComp);
    G(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, 0);
    /* View row 0 is the top of the picture and texture row 0; the window's
     * row 0 is its bottom.  Flip in the blit. */
    G(glBlitFramebuffer)(0, 0, sLastW, sLastH, x0, y0 + dh, x0 + dw, y0,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
}

int GlrReadback(u32 *out, int maxPixels, int *w, int *h)
{
    if (!sReady || !sFboComp || sLastW * sLastH > maxPixels)
        return 0;
    G(glBindFramebuffer)(GL_FRAMEBUFFER, sFboComp);
    G(glReadPixels)(0, 0, sLastW, sLastH, GL_RGBA, GL_UNSIGNED_BYTE, out);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    *w = sLastW;
    *h = sLastH;
    return 1;
}

#ifdef __cplusplus
}
#endif
