/* Sunlight for Rainbow Route (proof of concept, branch `lighting`).
 *
 * Off unless asked for: KATAM_LIGHTING=1 (native/node) or the web page's
 * /lighting/ path or ?lighting=1 (host_web.c sets gPortLighting).  With it
 * off nothing here runs and the render list is what it always was.
 *
 * Warm point lights hang in the room's sky (one per 640 px of room width,
 * placed from the collision grid, fixed per room).  Per room, from the
 * room's own collision grid (gUnk_02024ED0[slot], one byte per 16x16 cell,
 * an index into gCollisionAttributes), a map of one RG texel per 8x8 world
 * pixels is worked out by marching five rays from each texel to points
 * across each light's disc: R = light blocked (shadow, fanning out radially
 * with a penumbra that widens with distance), G = light arriving (falling
 * off with distance), then a small blur.  It is recomputed only when the
 * room changes or a byte of its grid does (a block broken), so a frame
 * costs a hash of at most 1950 bytes.  The GL composite samples it
 * bilinearly in world coordinates (stable while the camera moves) and
 * darkens/cools the shadow, warms the light and adds each light's halo, on
 * every world layer and world sprite; never on the HUD.  Outside the world
 * framing, or outside Rainbow Route, the frame carries no light map and the
 * composite is the old one.  The PPU (software) path is not touched.
 *
 * Runs at present time, outside the simulation, and only reads game memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "port/port.h"
#include "gba/gba.h"
#include "global.h"
#include "main.h"
#include "data.h"
#include "render/render_list.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define NowMs() emscripten_get_now()
#else
#include <time.h>
static double NowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

int gPortLighting = -1;          /* -1: not decided yet (reads the env)        */
double gPortLightBuildMs;        /* the last light-map build, for the report  */

/* Rainbow Route's rooms: the AREA_RAINBOW_ROUTE entries of the game's own
 * area-map table (game/src/pause_area_map.c, sAreaMapRoomInfos, static there,
 * so copied rather than linked -- 51 ids, no ROM bytes). */
static const u16 kRainbowRooms[] = {
    0x7e, 0x22d, 0xbe, 0xc2, 0x321, 0xb0, 0xc0, 0x24e, 0xb1, 0x217, 0x21a,
    0x218, 0x219, 0x21e, 0x21f, 0x8d, 0x8f, 0x22c, 0x22b, 0x22a, 0x6c, 0x8c,
    0x67, 0x68, 0x6b, 0x6a, 0x65, 0x6e, 0x1f7, 0x1fb, 0x89, 0x88, 0x7f,
    0x1f8, 0xae, 0x77, 0x76, 0xad, 0xaf, 0xac, 0xab, 0x75, 0x73, 0x8b,
    0x216, 0x23a, 0x23b, 0x214, 0x8a, 0xb2, 0x8e,
};

static int IsRainbowRoute(u16 room)
{
    unsigned i;
    for (i = 0; i < sizeof(kRainbowRooms) / sizeof(kRainbowRooms[0]); i++)
        if (kRainbowRooms[i] == room)
            return 1;
    return 0;
}

#define LIGHT_CELL 8                         /* world px per light texel    */
#define LIGHT_MAX_W (2176 / LIGHT_CELL + 2)
#define LIGHT_MAX_H (1648 / LIGHT_CELL + 2)
#define GRID_MAX 1950

/* Strength of the whole effect: 0 none, 1 the default (moderate), 2 strong.
 * KATAM_LIGHTING_STRENGTH overrides it (native/node). */
#define LIGHT_STRENGTH 1.0f
#define LIGHT_RANGE 560.0f                   /* px a light reaches          */
#define LIGHT_SPACING 640                    /* px between a room's lights  */
#define LIGHT_RADIUS 20.0f                   /* a light's disc: penumbra    */
#define LIGHT_RAYS 5
#define LIGHT_STEP 6.0f                      /* march step, px              */

static u8 sMask[LIGHT_MAX_W * LIGHT_MAX_H * 2];   /* RG: shadow, light     */
static float sTmp[LIGHT_MAX_W * LIGHT_MAX_H * 2];
static u8 sOcc[GRID_MAX];                    /* 0 air, 1 solid, 2 thin top  */
static s32 sMaskW, sMaskH;
static u32 sGen;
static u16 sRoom = 0xFFFF;
static u32 sGridHash;
static int sHave;
static float sStrength = -1.0f;
static float sLights[RL_MAX_LIGHTS][2];
static int sNumLights;

static int Occ(s32 x, s32 y, s32 cw, s32 ch)
{
    s32 cx, cy;
    u8 o;

    if (x < 0 || y < 0)
        return 0;
    cx = x >> 4;
    cy = y >> 4;
    if (cx >= cw || cy >= ch)
        return 0;
    o = sOcc[cy * cw + cx];
    return o == 1 || (o == 2 && (y & 15) < 4);
}

/* Where the lights hang: one per LIGHT_SPACING px of room width, each in
 * the most open spot (most air in a 9x9-cell window, higher preferred) of
 * the upper half of the room near its column -- the sky of an outdoor room.
 * Anchored to the room, so shadows do not swim. */
static void PlaceLights(s32 cw, s32 ch)
{
    int i, n = (cw * 16 + LIGHT_SPACING - 1) / LIGHT_SPACING;

    if (n < 1)
        n = 1;
    if (n > RL_MAX_LIGHTS)
        n = RL_MAX_LIGHTS;
    for (i = 0; i < n; i++) {
        s32 c0 = n == 1 ? cw / 3 : (s32)(((2 * i + 1) * cw) / (2 * n));
        s32 cx, cy, bx = c0, by = ch / 2, ymax = ch / 2 > 2 ? ch / 2 : 2;
        float best = -1e9f;

        for (cy = 1; cy <= ymax && cy < ch; cy++)
            for (cx = c0 - 5; cx <= c0 + 5; cx++) {
                int air = 0, x, y;
                float score;
                if (cx < 0 || cx >= cw || sOcc[cy * cw + cx] != 0)
                    continue;
                for (y = cy - 4; y <= cy + 4; y++)
                    for (x = cx - 4; x <= cx + 4; x++)
                        if (x >= 0 && y >= 0 && x < cw && y < ch && sOcc[y * cw + x] == 0)
                            air++;
                score = air - 0.6f * cy - 0.3f * (cx > c0 ? cx - c0 : c0 - cx);
                if (score > best) {
                    best = score;
                    bx = cx;
                    by = cy;
                }
            }
        sLights[i][0] = bx * 16 + 8;
        sLights[i][1] = by * 16 + 8;
    }
    sNumLights = n;
}

/* 0..1: how much of light (lx,ly)'s disc the point sees. */
static float Visible(float px0, float py0, float lx0, float ly0, float d0,
                     s32 cw, s32 ch)
{
    float nx = -(ly0 - py0) / d0, ny = (lx0 - px0) / d0, lit = 0.0f;
    int r;

    /* Rays to points across the light's disc, perpendicular to the line to
     * it: the penumbra widens with distance from the occluder, as an area
     * light's does. */
    for (r = 0; r < LIGHT_RAYS; r++) {
        float o = LIGHT_RADIUS * ((float)r / (LIGHT_RAYS - 1) * 2.0f - 1.0f);
        float lx = lx0 + nx * o, ly = ly0 + ny * o;
        float dx = lx - px0, dy = ly - py0, d = sqrtf(dx * dx + dy * dy);
        int n = (int)(d / LIGHT_STEP), s, start = 0, blocked = 0;
        float sx, sy;

        if (d < 1.0f) {
            lit += 1.0f;
            continue;
        }
        sx = dx / d * LIGHT_STEP;
        sy = dy / d * LIGHT_STEP;
        /* Inside the ground: start where the ray leaves it (within two
         * tiles), so the face of a lit block is lit inside. */
        while (start < n && start < 6
               && Occ((s32)(px0 + sx * start), (s32)(py0 + sy * start), cw, ch))
            start++;
        for (s = start + 1; s < n; s++)
            if (Occ((s32)(px0 + sx * s), (s32)(py0 + sy * s), cw, ch)) {
                blocked = 1;
                break;
            }
        lit += blocked ? 0.0f : 1.0f;
    }
    return lit / LIGHT_RAYS;
}

static void Build(const u8 *grid, s32 cw, s32 ch)
{
    s32 mw = (cw * 16) / LIGHT_CELL, mh = (ch * 16) / LIGHT_CELL;
    s32 tx, ty, i;

    for (i = 0; i < cw * ch; i++) {
        u32 a = gCollisionAttributes[grid[i]];
        if (a & 0x200)
            sOcc[i] = 1;
        else if ((a >> 28) != 0 && !(a & 1))
            sOcc[i] = 1;                     /* a slope: count it solid     */
        else if (a & 0x4)
            sOcc[i] = 2;                     /* jump-through platform       */
        else
            sOcc[i] = 0;
    }
    PlaceLights(cw, ch);

    /* Per texel: shadow = how much light is blocked (fading out towards
     * the edge of each light's reach), light = how much arrives, falling
     * off with distance.  Daylight stays neutral away from both. */
    for (ty = 0; ty < mh; ty++) {
        for (tx = 0; tx < mw; tx++) {
            float px0 = tx * LIGHT_CELL + LIGHT_CELL * 0.5f;
            float py0 = ty * LIGHT_CELL + LIGHT_CELL * 0.5f;
            float sh = 0.0f, li = 0.0f;
            int l;

            for (l = 0; l < sNumLights; l++) {
                float dx = sLights[l][0] - px0, dy = sLights[l][1] - py0;
                float d = sqrtf(dx * dx + dy * dy), v, fade, att;
                if (d >= LIGHT_RANGE)
                    continue;
                v = d < 1.0f ? 1.0f : Visible(px0, py0, sLights[l][0], sLights[l][1], d, cw, ch);
                fade = (LIGHT_RANGE - d) / 160.0f;
                if (fade > 1.0f)
                    fade = 1.0f;
                att = 1.0f - d / LIGHT_RANGE;
                sh += (1.0f - v) * fade;
                li += v * att * att;
            }
            sTmp[(ty * mw + tx) * 2] = sh > 1.0f ? 1.0f : sh;
            sTmp[(ty * mw + tx) * 2 + 1] = li > 1.0f ? 1.0f : li;
        }
    }

    /* 3x3 box blur, once. */
    for (ty = 0; ty < mh; ty++) {
        for (tx = 0; tx < mw; tx++) {
            float sum0 = 0.0f, sum1 = 0.0f;
            int n = 0, dx, dy;
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    s32 x = tx + dx, y = ty + dy;
                    if (x < 0 || y < 0 || x >= mw || y >= mh)
                        continue;
                    sum0 += sTmp[(y * mw + x) * 2];
                    sum1 += sTmp[(y * mw + x) * 2 + 1];
                    n++;
                }
            sMask[(ty * mw + tx) * 2] = (u8)(sum0 / (float)n * 255.0f + 0.5f);
            sMask[(ty * mw + tx) * 2 + 1] = (u8)(sum1 / (float)n * 255.0f + 0.5f);
        }
    }
    sMaskW = mw;
    sMaskH = mh;
    sGen++;
}

static void Attach(RlFrame *f, int roomBg);

/* With KATAM_LIGHTING_LOG: the mean CPU cost of a frame's attach (the grid
 * hash, plus any rebuild) every 600 frames. */
void PortLightAttach(RlFrame *f, int roomBg)
{
    static double sum;
    static int frames, logOn = -1;
    double t0;

    if (logOn < 0)
        logOn = getenv("KATAM_LIGHTING_LOG") != NULL;
    if (!logOn || gPortLighting == 0) {
        Attach(f, roomBg);
        return;
    }
    t0 = NowMs();
    Attach(f, roomBg);
    sum += NowMs() - t0;
    if (++frames == 600) {
        fprintf(stderr, "[katam-port] light: attach %.4f ms/frame mean over 600 frames\n",
                sum / frames);
        sum = 0;
        frames = 0;
    }
}

static void Attach(RlFrame *f, int roomBg)
{
    const struct LevelInfo *li;
    const u8 *grid;
    s32 cw, ch, n, i;
    u32 h = 2166136261u;

    f->light = NULL;
    if (gPortLighting < 0) {
        const char *e = getenv("KATAM_LIGHTING");
        gPortLighting = (e && e[0] == '1') ? 1 : 0;
    }
    if (!gPortLighting || !f->world || roomBg < 0 || roomBg > 3)
        return;
    if (sStrength < 0.0f) {
        const char *e = getenv("KATAM_LIGHTING_STRENGTH");
        sStrength = e ? (float)atof(e) : LIGHT_STRENGTH;
    }
    li = &gCurLevelInfo[gLocalPlayerId];
    if (li->currentRoom == 0xFFFF || !IsRainbowRoute(li->currentRoom) || li->unk65E > 3)
        return;
    cw = li->roomWidth / 16;
    ch = li->roomHeight / 16;
    n = cw * ch;
    if (cw <= 0 || ch <= 0 || n > GRID_MAX || cw * 16 / LIGHT_CELL > LIGHT_MAX_W
        || ch * 16 / LIGHT_CELL > LIGHT_MAX_H)
        return;
    grid = gUnk_02024ED0[li->unk65E];
    for (i = 0; i < n; i++)
        h = (h ^ grid[i]) * 16777619u;
    h ^= (u32)cw << 16;
    if (!sHave || li->currentRoom != sRoom || h != sGridHash) {
        double t0 = NowMs();
        Build(grid, cw, ch);
        gPortLightBuildMs = NowMs() - t0;
        if (getenv("KATAM_LIGHTING_LOG"))
            fprintf(stderr, "[katam-port] light: room %u %dx%d cells -> %dx%d map, %d lights, in %.2f ms; first at %.0f,%.0f\n",
                    (unsigned)li->currentRoom, (int)cw, (int)ch, (int)sMaskW,
                    (int)sMaskH, sNumLights, gPortLightBuildMs, sLights[0][0], sLights[0][1]);
        sRoom = li->currentRoom;
        sGridHash = h;
        sHave = 1;
    }
    f->light = sMask;
    f->lightW = sMaskW;
    f->lightH = sMaskH;
    f->lightGen = sGen;
    f->lightLayers = (u8)(1u << roomBg);
    f->lightK = sStrength;
    f->numLights = (u8)sNumLights;
    for (i = 0; i < sNumLights; i++) {
        f->lights[i][0] = sLights[i][0];
        f->lights[i][1] = sLights[i][1];
    }
}
