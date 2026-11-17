/* Tap to move: tap (or hold) where Kirby should go and he goes there; tap
 * an enemy to have him deal with it; tap a door to have him take it.
 * docs/design/TAP-TO-MOVE.md has the gesture table and the reasoning.
 *
 * --- what it is ----------------------------------------------------------
 *
 * An autopilot that drives this device's player 1 by pressing the GBA's
 * buttons for them.  Each live frame, just before the key latch, it reads
 * the game -- the Kirby, its room's collision map, the room's objects -- and
 * decides which buttons a careful player would hold for the next frame.
 * platform/main.c ORs that mask into the host's own keys, the way a second
 * keyboard would.  So:
 *
 *   - the input log records the keys and only the keys: a recording made
 *     with tap controls replays bit for bit with no new latch;
 *   - an online session sees this device's input like any other;
 *   - nothing here writes game memory, and its own state is the host's
 *     (statics in this file, never hashed or snapshotted).
 *
 * The taps themselves are never recorded.  Replays, rollback catch-up and
 * the pause overlay's fork never run it.
 *
 * --- the parts -------------------------------------------------------------
 *
 *   gestures  pointer down/move/up in framebuffer pixels, mapped to world
 *             pixels through the view the last picture was drawn with;
 *             classified at the finger's landing: on Kirby (tap = attack,
 *             swipe down = swallow / drop the ability), on an enemy (mark
 *             it), on a door (take it), anywhere else (go there; held, the
 *             target follows the finger).
 *   grid      the room's live collision map as nav classes (solid, one-way
 *             platform, droppable, water, hazard, door, ladder, slope).
 *   flow      a cost-to-go field from the target over the grid (Dijkstra
 *             on reversed moves), so every cell knows its best next step:
 *             walking is cheap, air and climbing cost more, hazards and
 *             bottomless rows much more, a one-way platform cannot be
 *             entered from above unless it is a drop-through one.
 *   control   per frame: the next few steps of the flow from Kirby's cell
 *             become Left/Right, a jump held for the height the path
 *             climbs, flaps while puffed, Down on a drop-through floor, an
 *             exhale to land, Up at a door, B at a marked enemy.
 *   watchdog  progress is the cost-to-go at Kirby's cell; no progress for a
 *             while gets a hop, then a fly, then the target is given up.
 *   markers   the target, the reticle, a faint path: GL overlays over the
 *             presented picture (the party HUD's mechanism), never the
 *             GBA picture.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "object.h"
#include "task.h"
#include "big_small_switch.h"
#include "constants/kirby.h"
#include "constants/object_types.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/inputlog.h"
#include "port/rollback.h"
#include "port/netsim.h"
#include "port/lmpnet.h"
#include "port/wide.h"
#include "render/gl_renderer.h"
#include "ui/settings.h"
#include "ui/party_hud.h"
#include "ui/ui.h"
#include "ui/tap_nav.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* GBA keys, 1 = held */
#define K_A      0x001
#define K_B      0x002
#define K_SELECT 0x004
#define K_START  0x008
#define K_RIGHT  0x010
#define K_LEFT   0x020
#define K_UP     0x040
#define K_DOWN   0x080
#define K_R      0x100
#define K_L      0x200

/* struct ObjectBase.flags / unk62 as the CPU brain reads them
 * (docs/naming/cpu-brain.md, code_0800ECAC_2.c) */
#define KF_LEFT    0x01         /* facing left                               */
#define KF_AIR     0x20         /* off the ground                            */
#define KF_FLY     0x40         /* puffed up, flying                         */
#define KF_MOUTH   0x80         /* a mouthful                                */
#define KH_WALL    0x03         /* unk62: against a wall                     */
/* (KF_AIR is not set while puffed: on the ground or not is unk62's floor
 * bit, KH_FLOOR) */
#define KH_FLOOR   0x04         /*        on the floor                       */
#define KH_CEIL    0x08         /*        against a ceiling                  */

/* --- state ------------------------------------------------------------------ */

static int sForced = -1;        /* -1: not yet read from KATAM_TAP          */
static int sLog = -1;           /* KATAM_TAP_LOG: 1 events, 2 every frame   */
static u16 sKeys;               /* what the last frame returned             */
static u32 sTick;               /* frames stepped (host clock, for gestures) */

/* The target. */
enum { T_NONE, T_GO, T_DOOR, T_ENEMY, T_SWITCH, T_BLOCK, T_PUSH };
static struct {
    int kind;
    s32 x, y;                   /* world px: where Kirby's centre should end */
    int cx, cy;                 /* its cell                                   */
    int air;                    /* GO: hover there rather than stand          */
    u16 room;
    struct Object *obj;         /* ENEMY, SWITCH                              */
    u8 objType;
    int hit;                    /* SWITCH: 0 stand on it, 1 hit it from beside */
    int wasDown;                /* SWITCH: already down when tapped           */
    int wait;                   /* SWITCH: frames standing at it this try     */
    s32 mx, my;                 /* SWITCH, BLOCK, PUSH: where the marker goes */
    int bx, by;                 /* BLOCK: the breakable cell                  */
    int face;                   /* BLOCK, PUSH: it is that way (-1, 1)        */
    int chest;                  /* DOOR: a chest (Up opens it; no room change) */
    s32 startX;                 /* PUSH: the block's x when he began          */
    int arrived;                /* frames since arrival (0: on the way)       */
    int frames;                 /* frames since set                           */
    int best, stall;            /* watchdog: best cost-to-go, frames without  */
    int tries;                  /* ENEMY: attack/inhale bouts; DOOR: Up tries */
    int fail;                   /* frames left of the "gave up" marker        */
    int unreach;                /* GO/DOOR: he cannot get there; going near   */
    s32 failX, failY;
} sT;

/* One-shot button sequences (tap Kirby, swipe down): they pause steering. */
static struct {
    u16 keys; int frames; int gap;
    u16 next[3]; u8 nextFrames[3]; int nNext;   /* steps after this one, no gap */
} sAct;
static void Action(u16 keys, int frames);

/* The controller's own memory. */
static struct {
    int jumpHold;               /* frames of A left in the current jump      */
    int aGap;                   /* frames until A may be pressed again       */
    int bHold, bGap;            /* B pattern at an enemy                      */
    int wasA, wasB;
    int downHold;
    int faceFix;                /* frames of a turn-to-face press            */
    int upHold;                 /* door: frames of Up                         */
    int dashPhase;              /* 0 none, 1 first tap, 2 gap                */
    int dashT, dashCool;
    int swallowCool;
    int unstick;                /* watchdog's escalation                     */
    int lastDir;
} sC;

/* The pointers. */
enum { G_NONE, G_KIRBY, G_DRAG, G_MARK, G_DONE, G_INHALE };
static struct {
    int on, id, kind;
    s32 x0, y0, x, y;           /* world px: landing, now                     */
    u32 t0;
    int moved;
} sP;
static int sSecondId = -1;      /* a second finger: attack                   */
static u32 sSecondT0;           /* when it landed: held, with the first, = L */
#define TAP_HOLD_L 30           /* frames of two fingers before L is held    */

/* The view the last picture was drawn with. */
static struct {
    int ok;
    s32 camX, camY, viewX, viewY, viewW, viewH;
    int world[4];
    float dpr;
} sV = { 0, 0, 0, 0, 0, 240, 160, { 0, 0, 240, 160 }, 1.0f };

/* --- small helpers ------------------------------------------------------------ */

static int Log(void)
{
    if (sLog < 0) {
        const char *e = getenv("KATAM_TAP_LOG");
        sLog = e ? atoi(e) : 0;
    }
    return sLog;
}

static int Abs(int v) { return v < 0 ? -v : v; }
static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int Slot(void)
{
    return gLocalPlayerId < gNumKirbys ? gLocalPlayerId : 0;
}

static struct Kirby *Me(void) { return &gKirbys[Slot()]; }
static struct LevelInfo *Li(void) { return &gCurLevelInfo[Slot()]; }

/* CSS pixels per world pixel on the picture now (for finger-sized tests). */
static float CssPerWorld(void)
{
    float k;

    if (!sV.ok || sV.viewW <= 0 || sV.world[2] <= 0)
        return 2.0f;
    k = (float)sV.world[2] / (float)sV.viewW / (sV.dpr > 0.0f ? sV.dpr : 1.0f);
    return k > 0.25f ? k : 0.25f;
}

/* A finger-sized distance (css px) in world px, clamped to [lo, hi]. */
static int FingerWorld(float css, int lo, int hi)
{
    return Clamp((int)(css / CssPerWorld() + 0.5f), lo, hi);
}

/* --- switching on ---------------------------------------------------------------- */

void PortTapForce(int on)
{
    sForced = on ? 1 : 0;
}

int PortTapEnabled(void)
{
    if (sForced < 0) {
        const char *e = getenv("KATAM_TAP");
        sForced = (e && *e && *e != '0') ? 1 : 0;
    }
    return sForced == 1 || gPortSettings.touch == PS_TOUCH_TAP;
}

/* In play: the game's HUD is up (play, not pause/menus/cutscenes), a room
 * is loaded and the Kirby is alive in it. */
static int InPlay(void)
{
    struct LevelInfo *li;

    if (!PortPartyHudGameHudUp())
        return 0;
    li = Li();
    if (li->currentRoom == 0xFFFF || li->roomWidth < 16 || li->roomHeight < 16)
        return 0;
    return 1;
}

int PortTapActive(void)
{
    return PortTapEnabled() && !PortInputLogReplaying() && InPlay();
}

unsigned short PortTapKeys(void)
{
    return sKeys;
}

/* --- the grid ------------------------------------------------------------------- */

#define NAV_CELLS 1950          /* sizeof gUnk_02024ED0[0]                   */
#define NAV_INF   0xFFFF

enum {
    CL_SOLID = 0x01, CL_PLAT = 0x02, CL_DROP = 0x04, CL_WATER = 0x08,
    CL_HAZ = 0x10, CL_DOOR = 0x20, CL_LADDER = 0x40, CL_SLOPE = 0x80
};

static u8  sCls[NAV_CELLS];
static u16 sDist[NAV_CELLS];
static int sW, sH;
static u16 sGridRoom = 0xFFFF;
static u32 sGridSig;
static int sGoal = -1;          /* the cell the flow field leads to          */
static int sFlowAge;

static u8 Classify(u32 a, int mini)
{
    u8 c = 0;
    u32 slope = a >> 28;

    if ((a & 0x400) && !mini)               /* a Mini-only gap crushes      */
        return CL_SOLID | CL_HAZ;
    if (a & (0x200 | 0x1000))               /* solid, breakable              */
        c |= CL_SOLID;
    if (!(c & CL_SOLID)) {
        if (slope >= 1 && slope <= 6)       /* a floor slope: ground inside  */
            c |= CL_SLOPE;
        if (a & 4)
            c |= CL_PLAT | ((a & 8) ? CL_DROP : 0);
    }
    if (a & 2)
        c |= CL_WATER;
    if (a & 0x2000)
        c |= CL_HAZ;
    if (a & 0x4000)
        c |= CL_DOOR;
    if (a & 0x100)
        c |= CL_LADDER;
    return c;
}

static struct Object *NextObject(int *i);
static void Box(const struct Object *o, s32 *x, s32 *y, s32 *w, s32 *h);

/* Objects that are walls (the game registers them as solid each frame,
 * Macro_080A4728): sliding doors, star stone blocks, lava walls, flame
 * columns.  Their boxes go into the grid as solid ground. */
static int IsObstacle(const struct Object *o)
{
    switch (o->type) {
    case OBJ_VERTICAL_SLIDING_DOOR: case OBJ_LAVA_WALL: case OBJ_STAR_STONE_BLOCK: case OBJ_UNKNOWN_7A:
    case OBJ_FLAME_COLUMN: case OBJ_LARGE_STAR_STONE_BLOCK_7D: case OBJ_LARGE_STAR_STONE_BLOCK_7E:
    case OBJ_LARGE_STAR_STONE_BLOCK_88: case OBJ_LARGE_STAR_STONE_BLOCK_89:
        return !(o->base.flags & 0x1000);
    case OBJ_CANNON: case OBJ_8_DIR_CANNON:
        return 2;                               /* not a wall: avoided (it   */
    default:                                    /* catches him; A fires it)  */
        return 0;
    }
}

/* A block he pushes by walking into it (star_stone_block.c and the large
 * ones: every Kirby touching a side pushes, opposite sides cancel). */
static int IsPushable(const struct Object *o)
{
    switch (o->type) {
    case OBJ_STAR_STONE_BLOCK: case OBJ_LARGE_STAR_STONE_BLOCK_7D: case OBJ_LARGE_STAR_STONE_BLOCK_7E:
    case OBJ_LARGE_STAR_STONE_BLOCK_88: case OBJ_LARGE_STAR_STONE_BLOCK_89:
        return !(o->base.flags & 0x1000);
    default:
        return 0;
    }
}

/* Rebuild the classes from the Kirby's room's live map and its wall
 * objects.  Returns 1 when anything changed (a block broke, a door slid,
 * a new room). */
static int GridUpdate(void)
{
    struct LevelInfo *li = Li();
    const u8 *cells;
    int w = li->roomWidth >> 4, h = li->roomHeight >> 4, i, mini, n = 0;
    u32 sig = 2166136261u;
    s32 ob[8][4];
    int kind[8];
    struct Object *o;

    if (w <= 0 || h <= 0 || w * h > NAV_CELLS || li->unk65E >= 4) {
        sW = sH = 0;
        return 0;
    }
    cells = gUnk_02024ED0[li->unk65E];
    mini = Me()->ability == KIRBY_ABILITY_MINI;
    for (i = 0; i < w * h; i++)
        sig = (sig ^ cells[i]) * 16777619u;
    sig ^= (u32)w * 7919u + (u32)h * 104729u + (u32)mini;
    for (i = 0; (o = NextObject(&i)) != NULL && n < 8; ) {
        s32 x, y, bw, bh;

        if (!IsObstacle(o))
            continue;
        Box(o, &x, &y, &bw, &bh);
        kind[n] = IsObstacle(o);
        ob[n][0] = x >> 4;
        ob[n][1] = y >> 4;
        ob[n][2] = (x + bw - 1) >> 4;
        ob[n][3] = (y + bh - 1) >> 4;
        sig = (sig ^ (u32)(ob[n][0] | ob[n][1] << 8 | ob[n][2] << 16 | (u32)ob[n][3] << 24)) * 16777619u;
        n++;
    }
    if (sig == sGridSig && sGridRoom == li->currentRoom && sW == w && sH == h)
        return 0;
    sW = w;
    sH = h;
    sGridRoom = li->currentRoom;
    sGridSig = sig;
    for (i = 0; i < w * h; i++)
        sCls[i] = Classify(gCollisionAttributes[cells[i]], mini);
    for (i = 0; i < n; i++) {
        int x, y;

        for (y = ob[i][1] < 0 ? 0 : ob[i][1]; y <= ob[i][3] && y < h; y++)
            for (x = ob[i][0] < 0 ? 0 : ob[i][0]; x <= ob[i][2] && x < w; x++)
                sCls[y * w + x] = kind[i] == 1 ? CL_SOLID : (u8)(sCls[y * w + x] | CL_HAZ);
    }
    return 1;
}

static u8 Cl(int x, int y)
{
    if (x < 0 || y < 0 || x >= sW)
        return CL_SOLID;
    if (y >= sH)
        return 0;               /* below the room: nothing (a pit)        */
    return sCls[y * sW + x];
}

static int Open(int x, int y) { return x >= 0 && y >= 0 && x < sW && y < sH && !(Cl(x, y) & CL_SOLID); }

/* A cell's live terrain attribute (the room's map as it is now). */
static u32 Attr(int x, int y)
{
    struct LevelInfo *li = Li();

    if (sW <= 0 || x < 0 || y < 0 || x >= sW || y >= sH || li->unk65E >= 4)
        return 0;
    return gCollisionAttributes[gUnk_02024ED0[li->unk65E][y * sW + x]];
}

/* A breakable block (TILE_BREAKABLE; its strength 0..4 in 0x00F00000:
 * 0 and 1 break to any hit, harder ones want an ability's attack). */
static int Breakable(int x, int y) { return (Attr(x, y) & 0x1000) != 0; }

/* Can a Kirby stand (or hold on) here? */
static int Supported(int x, int y)
{
    u8 c = Cl(x, y), b;

    if (c & (CL_SLOPE | CL_WATER | CL_LADDER))
        return 1;
    if (y + 1 >= sH)
        return 0;
    b = Cl(x, y + 1);
    return (b & (CL_SOLID | CL_PLAT | CL_SLOPE)) != 0;
}

/* The cost of one move from (x,y) to (nx,ny), neighbours; 0 = not allowed. */
static int MoveCost(int x, int y, int nx, int ny)
{
    u8 to;
    int c;

    if (!Open(nx, ny))
        return 0;
    to = Cl(nx, ny);
    if (ny > y) {                               /* down                      */
        if ((to & CL_PLAT) && !(to & CL_DROP) && !(Cl(x, y) & CL_PLAT))
            return 0;                           /* a one-way floor: from above, no */
        c = (to & CL_PLAT) ? 3 : 1;
    } else if (ny < y) {                        /* up: a jump or a flap      */
        c = (Cl(x, y) & (CL_WATER | CL_LADDER)) ? 3 : 4;
        if ((Cl(x, y) | to) & CL_SLOPE)
            c = 2;                              /* up a slope: a walk        */
    } else {
        c = Supported(nx, ny) ? 2 : 3;          /* walk, or through the air */
        if (to & CL_WATER)
            c = 3;
    }
    if (to & CL_HAZ)
        c += 60;
    if (ny + 1 < sH && (Cl(nx, ny + 1) & (CL_SOLID | CL_HAZ)) == (CL_SOLID | CL_HAZ))
        c += 30;                                /* standing on spikes        */
    if (ny == sH - 1 && !Supported(nx, ny))
        c += 40;                                /* the bottom of a pit       */
    return c;
}

/* A binary heap of (cost << 12 | cell). */
static u32 sHeap[NAV_CELLS * 4];
static int sHeapN;

static void HeapPush(u32 v)
{
    int i = sHeapN++;

    while (i > 0) {
        int p = (i - 1) >> 1;

        if (sHeap[p] <= v)
            break;
        sHeap[i] = sHeap[p];
        i = p;
    }
    sHeap[i] = v;
}

static u32 HeapPop(void)
{
    u32 top = sHeap[0], v = sHeap[--sHeapN];
    int i = 0;

    for (;;) {
        int c = 2 * i + 1;

        if (c >= sHeapN)
            break;
        if (c + 1 < sHeapN && sHeap[c + 1] < sHeap[c])
            c++;
        if (sHeap[c] >= v)
            break;
        sHeap[i] = sHeap[c];
        i = c;
    }
    sHeap[i] = v;
    return top;
}

static const s8 kDx[4] = { 1, -1, 0, 0 }, kDy[4] = { 0, 0, -1, 1 };

/* Cost-to-go from every cell to `goal`, over reversed moves. */
static void Flow(int goal)
{
    int i;

    for (i = 0; i < sW * sH; i++)
        sDist[i] = NAV_INF;
    sGoal = goal;
    sFlowAge = 0;
    if (goal < 0 || goal >= sW * sH)
        return;
    sHeapN = 0;
    sDist[goal] = 0;
    HeapPush((u32)goal);
    while (sHeapN > 0) {
        u32 e = HeapPop();
        int u = (int)(e & 0xFFF), du = (int)(e >> 12), ux = u % sW, uy = u / sW, k;

        if (du != sDist[u])
            continue;
        for (k = 0; k < 4; k++) {
            int vx = ux + kDx[k], vy = uy + kDy[k], v, c;

            if (vx < 0 || vy < 0 || vx >= sW || vy >= sH)
                continue;
            v = vy * sW + vx;
            c = MoveCost(vx, vy, ux, uy);       /* v -> u                    */
            if (!c || du + c >= sDist[v])
                continue;
            sDist[v] = (u16)(du + c);
            if (sHeapN < (int)(sizeof(sHeap) / sizeof(sHeap[0])))
                HeapPush(((u32)(du + c) << 12) | (u32)v);
        }
    }
}

/* Kirby's cell: his centre's, or the one above when that is solid (a slope,
 * a floor he is sunk into by a pixel). */
static int KirbyCell(s32 kx, s32 ky)
{
    int cx = Clamp(kx >> 4, 0, sW - 1), cy = Clamp(ky >> 4, 0, sH - 1);

    if (!Open(cx, cy) && cy > 0 && Open(cx, cy - 1))
        cy--;
    return cy * sW + cx;
}

/* The best step from cell s along the flow: -1 none. */
static int NextCell(int s)
{
    int x = s % sW, y = s / sW, k, best = -1, bc = NAV_INF;

    for (k = 0; k < 4; k++) {
        int nx = x + kDx[k], ny = y + kDy[k], c, n;

        if (nx < 0 || ny < 0 || nx >= sW || ny >= sH)
            continue;
        n = ny * sW + nx;
        c = MoveCost(x, y, nx, ny);
        if (!c || sDist[n] == NAV_INF || sDist[n] >= sDist[s])
            continue;
        if (c + sDist[n] < bc) {
            bc = c + sDist[n];
            best = n;
        }
    }
    return best;
}

/* The path from s, up to `max` cells after it (for the controller and the
 * markers).  Returns the count. */
#define NAV_PATH_MAX 64
static int sPath[NAV_PATH_MAX], sPathN;

static int Trace(int s, int max)
{
    int n = 0;

    while (n < max && s >= 0 && s != sGoal) {
        s = NextCell(s);
        if (s < 0)
            break;
        sPath[n++] = s;
    }
    return n;
}

/* The open cell nearest (x, y) that the flow from `from` can reach... is the
 * other way round: the reachable cell (forward breadth first from `from`)
 * nearest the point.  For a target Kirby cannot get to (a sealed chamber):
 * go as near as he can. */
static u8 sSeen[NAV_CELLS];
static u16 sQueue[NAV_CELLS];

static int NearestReachable(int from, s32 px, s32 py)
{
    int head = 0, tail = 0, best = from;
    s32 bd = 0x7FFFFFFF;

    memset(sSeen, 0, (size_t)(sW * sH));
    sSeen[from] = 1;
    sQueue[tail++] = (u16)from;
    while (head < tail) {
        int u = sQueue[head++], ux = u % sW, uy = u / sW, k;
        s32 dx = ux * 16 + 8 - px, dy = uy * 16 + 8 - py, d = dx * dx + dy * dy;

        if (d < bd && (Supported(ux, uy) || d < 24 * 24)) {
            bd = d;
            best = u;
        }
        for (k = 0; k < 4; k++) {
            int vx = ux + kDx[k], vy = uy + kDy[k], v;

            if (vx < 0 || vy < 0 || vx >= sW || vy >= sH)
                continue;
            v = vy * sW + vx;
            if (sSeen[v] || !MoveCost(ux, uy, vx, vy))
                continue;
            sSeen[v] = 1;
            sQueue[tail++] = (u16)v;
        }
    }
    return best;
}

/* --- reading the room ---------------------------------------------------------- */

static int IsEnemy(const struct Object *o)
{
    if (o->base.flags & 0x1000)
        return 0;
    if (o->type > OBJ_DARK_META_KNIGHT_W8)      /* enemies, mid-bosses, bosses */
        return 0;
    if (ObjType38To52(o) && o->unk80 <= 0)
        return 0;
    return 1;
}

static int IsItem(const struct Object *o)
{
    return !(o->base.flags & 0x1000)
        && (o->type == OBJ_SMALL_FOOD || o->type == OBJ_POP_SODA || o->type == OBJ_MEAT
            || o->type == OBJ_TOMATO || o->type == OBJ_BATTERY || o->type == OBJ_1UP
            || o->type == OBJ_INVINCIBLE_CANDY);
}

/* A puzzle switch (game/src/big_small_switch.c): the small floor button or
 * the big one (a person's hit only).  Kirby's body is itself an attack of
 * the type both take (unk68 0x82, kirby.c), so walking onto one presses
 * it.  0x40: the game made it inert (a big switch pressed on an earlier
 * visit); 0x1000: gone. */
static int IsSwitch(const struct Object *o)
{
    return (o->type == OBJ_SMALL_BUTTON || o->type == OBJ_BIG_BUTTON) && !(o->base.flags & 0x1040);
}

/* A treasure chest (chest.c): opened by a person pressing Up inside it. */
static int IsChest(const struct Object *o)
{
    return (o->type == OBJ_SMALL_CHEST || o->type == OBJ_BIG_CHEST) && !(o->base.flags & 0x1000);
}

/* A door that is an object (the hub's mirrors, an area's door, a boss
 * door): Up in front of it, as at a door tile. */
static int IsDoorObj(const struct Object *o)
{
    return (o->type == OBJ_DOOR || o->type == OBJ_DOOR_TO_HUB || o->type == OBJ_AREA_DOOR
            || o->type == OBJ_BOSS_CHALLENGE_DOOR) && !(o->base.flags & 0x1000);
}

/* Down now (sub_081197D4: 0x100 pressed this frame, 1 held). */
static int SwitchDown(const struct Object *o)
{
    return (((const struct Switch *)o)->unkB6 & 0x101) != 0;
}

/* An object's hitbox in world px: x y w h. */
static void Box(const struct Object *o, s32 *x, s32 *y, s32 *w, s32 *h)
{
    const struct ObjectBase *b = &o->base;

    *w = b->unk3A * 2;
    *h = b->unk3B * 2;
    if (*w <= 0 || *h <= 0) {
        *w = *h = 16;
        *x = (b->x >> 8) - 8;
        *y = (b->y >> 8) - 8;
        return;
    }
    *x = (b->x >> 8) + ((b->flags & 1) ? -b->unk38 - b->unk3A * 2 : b->unk38);
    *y = (b->y >> 8) + b->unk39;
}

/* The room's objects.  Not the game's per-room collision list: that is
 * built during the frame and cleared at its end, which is when this runs.
 * The task table instead (platform/lmpnet.c PortLnFindEnemy does the same):
 * every live task whose struct is an object (header kind 1) in Kirby's room.
 * *i is the cursor (start at 0); NULL at the end. */
static struct Object *NextObject(int *i)
{
    u16 room = Li()->currentRoom;

    for (; *i < MAX_TASK_NUM; (*i)++) {
        const struct Task *t = &gTasks[*i];
        struct Object *o;

        if (!t->main || !t->structOffset)
            continue;
        o = (struct Object *)((t->flags & TASK_USE_EWRAM) ? (uintptr_t)EWRAM_START + ((uintptr_t)t->structOffset << 2)
                                                          : (uintptr_t)IWRAM_START + t->structOffset);
        if (o->base.header.kind != 1 || o->base.roomId != room)
            continue;
        (*i)++;
        return o;
    }
    return NULL;
}

/* Still there: a live object of that type (not the slot reused). */
static int ObjLive(const struct Object *want, u8 type)
{
    struct Object *ob;
    int i = 0;

    while ((ob = NextObject(&i)) != NULL)
        if (ob == want)
            return ob->type == type && !(ob->base.flags & 0x1000);
    return 0;
}

/* Still there: a live object, the same kind, alive. */
static int ObjAlive(const struct Object *want, u8 type)
{
    struct Object *ob;
    int i = 0;

    while ((ob = NextObject(&i)) != NULL)
        if (ob == want)
            return ob->type == type && IsEnemy(ob);
    return 0;
}

/* The object (enemy or item) under a world point, within `tol` px of its
 * box; `items` 1 for items, 0 for enemies. */
static struct Object *ObjAt(s32 px, s32 py, int tol, int items)
{
    struct Object *best = NULL, *ob;
    s32 bd = 0x7FFFFFFF;
    int i = 0;

    while ((ob = NextObject(&i)) != NULL) {
        s32 x, y, w, h, dx, dy, d;

        if (items ? !IsItem(ob) : !IsEnemy(ob))
            continue;
        Box(ob, &x, &y, &w, &h);
        dx = px < x ? x - px : px > x + w ? px - (x + w) : 0;
        dy = py < y ? y - py : py > y + h ? py - (y + h) : 0;
        if (dx > tol || dy > tol)
            continue;
        d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = ob;
        }
    }
    return best;
}

/* The object (IS says which) under a world point, within `tol` px of its
 * bounds (its own rect, base + unk3C..unk3F, as the game tests switches
 * and chests). */
static struct Object *ThingAt(s32 px, s32 py, int tol, int (*is)(const struct Object *))
{
    struct Object *best = NULL, *ob;
    s32 bd = 0x7FFFFFFF;
    int i = 0;

    while ((ob = NextObject(&i)) != NULL) {
        s32 x0, y0, x1, y1, dx, dy, d;

        if (!is(ob))
            continue;
        x0 = (ob->base.x >> 8) + ob->base.unk3C;
        y0 = (ob->base.y >> 8) + ob->base.unk3D;
        x1 = (ob->base.x >> 8) + ob->base.unk3E;
        y1 = (ob->base.y >> 8) + ob->base.unk3F;
        dx = px < x0 ? x0 - px : px > x1 ? px - x1 : 0;
        dy = py < y0 ? y0 - py : py > y1 ? py - y1 : 0;
        if (dx > tol || dy > tol)
            continue;
        d = dx * dx + dy * dy;
        if (d < bd) {
            bd = d;
            best = ob;
        }
    }
    return best;
}

/* The pushable block under a world point, within `tol` px of its box. */
static struct Object *PushAt(s32 px, s32 py, int tol)
{
    struct Object *ob;
    int i = 0;

    while ((ob = NextObject(&i)) != NULL) {
        s32 x, y, w, h;

        if (!IsPushable(ob))
            continue;
        Box(ob, &x, &y, &w, &h);
        if (px >= x - tol && px < x + w + tol && py >= y - tol && py < y + h + tol)
            return ob;
    }
    return NULL;
}

/* The breakable cell under a world point, or one within `tol` px of it:
 * 1 and the cell. */
static int BlockNear(s32 px, s32 py, int tol, int *ocx, int *ocy)
{
    static const s8 off[5][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    int i;

    for (i = 0; i < 5; i++) {
        int cx = (px + off[i][0] * tol) >> 4, cy = (py + off[i][1] * tol) >> 4;

        if (Breakable(cx, cy)) {
            *ocx = cx;
            *ocy = cy;
            return 1;
        }
    }
    return 0;
}

/* --- targets -------------------------------------------------------------------- */

static void Clear(const char *why)
{
    if (sT.kind != T_NONE && Log())
        PortLog("[tap] f%u clear (%s) kind %d frames %d", (unsigned)PortFrameNumber(), why, sT.kind, sT.frames);
    sT.kind = T_NONE;
    sT.obj = NULL;
    sGoal = -1;
    memset(&sC, 0, sizeof(sC));
}

static void Fail(const char *why)
{
    sT.fail = 45;
    sT.failX = sT.x;
    sT.failY = sT.y;
    Clear(why);
}

static void NewTarget(int kind)
{
    int keepFail = sT.fail;
    s32 fx = sT.failX, fy = sT.failY;

    memset(&sT, 0, sizeof(sT));
    sT.kind = kind;
    sT.room = Li()->currentRoom;
    sT.best = NAV_INF;
    sT.fail = keepFail;
    sT.failX = fx;
    sT.failY = fy;
    memset(&sC, 0, sizeof(sC));
    sGoal = -1;
}

/* How far above the ground (world pixels) a tap still means "walk there":
 * about Kirby's height.  Higher, he flies there and hovers.  (Was up to
 * two cells above the standing cell, about 48 px: too eager to land.) */
#define TAP_SNAP_PX 16

/* A finger held this long (frames) on Kirby without moving: inhale. */
#define TAP_HOLD 14

/* A point to go to: snapped to the ground below it when it is at most
 * TAP_SNAP_PX above it (a tap at Kirby's height), onto the top of the block
 * when it is in solid ground, else an air point to hover at. */
static void SetGo(s32 px, s32 py)
{
    int cx, cy, i, moved = 0;

    GridUpdate();
    if (sW <= 0)
        return;
    px = Clamp(px, 0, sW * 16 - 1);
    py = Clamp(py, 0, sH * 16 - 1);
    cx = px >> 4;
    cy = py >> 4;
    if (!Open(cx, cy)) {
        moved = 1;
        for (i = 1; i <= 3 && !Open(cx, cy - i); i++)
            ;
        if (i <= 3)
            cy -= i;
        else {
            int s = NearestReachable(cy * sW + cx, px, py);
            cx = s % sW;
            cy = s / sW;
        }
    }
    if (sT.kind != T_GO)
        NewTarget(T_GO);
    sT.air = 1;
    for (i = 0; i <= 1 && cy + i < sH && Open(cx, cy + i); i++)
        if (Supported(cx, cy + i)) {
            if (moved || (cy + i + 1) * 16 - py <= TAP_SNAP_PX) {
                cy += i;
                sT.air = 0;
            }
            break;
        }
    sT.cx = cx;
    sT.cy = cy;
    sT.x = Clamp(Clamp(px, cx * 16 + 3, cx * 16 + 12), 16, sW * 16 - 16);
    sT.y = sT.air ? cy * 16 + 8 : cy * 16 + 8;
    sT.arrived = 0;
    sT.stall = 0;
    sT.best = NAV_INF;
}

static void SetDoor(int cx, int cy)
{
    NewTarget(T_DOOR);
    sT.cx = cx;
    sT.cy = cy;
    sT.x = cx * 16 + 8;
    sT.y = cy * 16 + 8;
}

static void SetEnemy(struct Object *o)
{
    NewTarget(T_ENEMY);
    sT.obj = o;
    sT.objType = o->type;
    sT.x = o->base.x >> 8;
    sT.y = o->base.y >> 8;
}

/* Where Kirby stands for a switch: on it, or (hit) beside it on his side
 * (the other side when his side is a wall), facing it. */
static void SwitchSpot(void)
{
    struct Object *o = sT.obj;
    s32 sx = o->base.x >> 8, sy = o->base.y >> 8, kx = Me()->base.x >> 8, x = sx;
    int cy = (sy - 4) >> 4, side;

    if (sT.hit) {
        side = o->base.unk3E + 12;              /* its half width, and Kirby's */
        x = kx < sx ? sx - side : sx + side;
        if (!Open(Clamp(x, 0, sW * 16 - 1) >> 4, cy))
            x = kx < sx ? sx + side : sx - side;
    }
    x = Clamp(x, 16, sW * 16 - 16);
    while (cy > 0 && !Open(x >> 4, cy))
        cy--;
    sT.x = x;
    sT.cx = x >> 4;
    sT.cy = cy;
    sT.y = cy * 16 + 8;
    sT.arrived = 0;
    sT.wait = 0;
    sT.best = NAV_INF;
    sGoal = -1;
}

static void SetSwitch(struct Object *o)
{
    GridUpdate();
    NewTarget(T_SWITCH);
    sT.obj = o;
    sT.objType = o->type;
    sT.hit = 0;                                 /* touching it is a hit      */
    sT.wasDown = SwitchDown(o);
    sT.mx = o->base.x >> 8;
    sT.my = (o->base.y >> 8) + o->base.unk3D;
    SwitchSpot();
}

/* A breakable block: stand right beside it (his side first), on ground,
 * on its row or the row above or below in the same column (which is then
 * the block he hits); else two cells off; else hover beside it.  Facing it. */
static void SetBlock(int bx, int by)
{
    static const s8 rows[3] = { 0, 1, -1 };
    struct Kirby *k = Me();
    int side0 = (k->base.x >> 12) < bx ? -1 : 1, d, r, pass, x = -1, row = by;

    NewTarget(T_BLOCK);
    for (d = 1; d <= 2 && x < 0; d++)
        for (r = 0; r < 3 && x < 0; r++)
            for (pass = 0; pass < 2 && x < 0; pass++) {
                int side = pass ? -side0 : side0, cy = by + rows[r], cx = bx + side * d, i;

                if (!Breakable(bx, cy) || !Supported(cx, cy))
                    continue;
                for (i = 1; i <= d && Open(bx + side * i, cy); i++)
                    ;
                if (i > d) {
                    x = cx;
                    row = cy;
                }
            }
    sT.air = x < 0;
    if (x < 0)                                  /* hover beside it           */
        x = Open(bx + side0, by) ? bx + side0 : Open(bx - side0, by) ? bx - side0 : bx + side0;
    sT.bx = bx;
    sT.by = row;
    sT.mx = bx * 16 + 8;
    sT.my = row * 16 + 8;
    sT.cx = x;
    sT.cy = row;
    sT.x = x * 16 + 8;
    sT.y = row * 16 + 8;
    sT.face = bx < x ? -1 : 1;
}

/* A heavy star block (it wants a push of 2: two Kirbys walking into it,
 * or one strong inhale -- B held 48 frames, code_0806F780.c sub_080720F8,
 * 55 px of reach): stand on its row on ground, his side first, up to 3
 * cells off, and face it. */
static void SetPush(struct Object *o)
{
    s32 x, y, w, h;
    int bx, by, side, pass, d, cx = -1;

    GridUpdate();
    NewTarget(T_PUSH);
    sT.obj = o;
    sT.objType = o->type;
    Box(o, &x, &y, &w, &h);
    sT.mx = x + w / 2;
    sT.my = y + h / 2;
    sT.startX = o->base.x >> 8;
    bx = sT.mx >> 4;
    by = (y + h - 1) >> 4;                      /* its bottom row            */
    side = (Me()->base.x >> 8) < sT.mx ? -1 : 1;
    for (pass = 0; pass < 2 && cx < 0; pass++, side = -side)
        for (d = (w + 15) / 32 + 1; d <= 3; d++) {
            int c = bx + side * d;

            if (!Open(c, by))
                break;
            if (Supported(c, by)) {
                cx = c;
                break;
            }
        }
    sT.air = cx < 0;                            /* no ground: hover there    */
    if (cx < 0)
        cx = Clamp(bx + ((Me()->base.x >> 8) < sT.mx ? -1 : 1), 0, sW - 1);
    sT.cx = cx;
    sT.cy = by;
    sT.x = cx * 16 + 8;
    sT.y = by * 16 + 8;
    sT.face = bx < cx ? -1 : 1;
}

/* At the heavy block, facing it: a strong inhale (B 110 frames), a walk
 * into it (60 frames), a strong inhale again. */
static u16 MoveBlock(struct Kirby *k)
{
    int left = sT.face < 0, fl = k->base.flags;

    if (fl & KF_FLY) {                          /* puffed: let it out first  */
        if (!sC.bGap) {
            sC.bGap = 20;
            Action(K_B, 2);
        }
        return 0;
    }
    if (((fl & KF_LEFT) != 0) != left)
        return left ? K_LEFT : K_RIGHT;
    if (!(k->base.unk62 & KH_FLOOR) || ++sT.wait < 10)
        return 0;
    sT.wait = 0;
    if (Log())
        PortLog("[tap] f%u heavy block: try %d", (unsigned)PortFrameNumber(), sT.tries);
    switch (sT.tries++) {
    case 0:
    case 2:
        Action(K_B, 110);
        break;
    case 1:
        Action(left ? K_LEFT : K_RIGHT, 60);
        break;
    default:
        Fail("block: it did not move");
        break;
    }
    return 0;
}

/* A door cell near a world point (the tap can be on the door's upper half,
 * a cell above the one Kirby stands in to use it): 1 and the cell. */
static int DoorNear(s32 px, s32 py, int tol, int *ocx, int *ocy)
{
    int cx = px >> 4, cy = py >> 4, x, y, found = 0;
    s32 bd = 0x7FFFFFFF;

    for (y = cy - 1; y <= cy + 2; y++)
        for (x = cx - 1; x <= cx + 1; x++) {
            s32 dx, dy, d;

            if (x < 0 || y < 0 || x >= sW || y >= sH || !(Cl(x, y) & CL_DOOR))
                continue;
            /* the door's picture: its cell and the one above */
            dx = Abs(px - (x * 16 + 8));
            dy = py < y * 16 - 16 ? y * 16 - 16 - py : py > y * 16 + 16 ? py - (y * 16 + 16) : 0;
            if (dx > 8 + tol || dy > tol)
                continue;
            d = dx * dx + dy * dy;
            if (d < bd) {
                bd = d;
                *ocx = x;
                *ocy = y;
                found = 1;
            }
        }
    return found;
}

/* --- gestures ------------------------------------------------------------------ */

static void Action(u16 keys, int frames)
{
    sAct.keys = keys;
    sAct.frames = frames;
    sAct.gap = 2;               /* a release first, so the press is an edge */
    sAct.nNext = 0;
}

/* One more step after the current action, straight on (no release). */
static void ActionThen(u16 keys, int frames)
{
    if (sAct.nNext < 3) {
        sAct.next[sAct.nNext] = keys;
        sAct.nextFrames[sAct.nNext] = (u8)frames;
        sAct.nNext++;
    }
}

/* The slide, a ground kick: face DIR (-1 left, 1 right, 0 as he is), crouch
 * (Down), then A with Down still held.  Down+A from nothing is a jump. */
static void Slide(int dir)
{
    if (dir) {
        Action(dir < 0 ? K_LEFT : K_RIGHT, 2);
        ActionThen(K_DOWN, 6);
    } else
        Action(K_DOWN, 6);
    ActionThen(K_DOWN | K_A, 6);
}

static void TapKirby(void)
{
    if (Log())
        PortLog("[tap] f%u kirby: attack", (unsigned)PortFrameNumber());
    Clear("tap kirby");
    Action(K_B, 6);
}

static void SwipeKirby(void)
{
    struct Kirby *k = Me();
    u16 keys = K_DOWN;

    if (!(k->base.flags & KF_MOUTH) && k->ability != KIRBY_ABILITY_NORMAL)
        keys = K_SELECT;                        /* drop the ability          */
    if (Log())
        PortLog("[tap] f%u kirby: swipe down (%s)", (unsigned)PortFrameNumber(),
                keys == K_SELECT ? "drop ability" : "swallow");
    Clear("swipe kirby");
    Action(keys, 6);
}

/* A swipe from Kirby, diagonally down: the slide, that way. */
static void KickKirby(int dir)
{
    if (Log())
        PortLog("[tap] f%u kirby: swipe diagonal (slide %s)", (unsigned)PortFrameNumber(), dir < 0 ? "left" : "right");
    Clear("swipe kick");
    Slide(dir);
}

static int NearKirby(s32 px, s32 py)
{
    struct Kirby *k = Me();
    s32 dx = px - (k->base.x >> 8), dy = py - ((k->base.y >> 8) - 2);
    int r = FingerWorld(16.0f, 10, 20);

    return dx * dx + dy * dy <= r * r;
}

/* The finger lands: decide what it is about. */
static void GestureDown(s32 px, s32 py)
{
    struct Object *o;
    int tol = FingerWorld(14.0f, 4, 16), dcx, dcy;

    GridUpdate();
    sP.kind = G_DRAG;
    if (NearKirby(px, py)) {
        sP.kind = G_KIRBY;
        return;
    }
    if ((o = ObjAt(px, py, tol, 0)) != NULL) {
        SetEnemy(o);
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u mark enemy type 0x%02X at %d,%d", (unsigned)PortFrameNumber(), o->type,
                    (int)(o->base.x >> 8), (int)(o->base.y >> 8));
        return;
    }
    if (!DoorNear(px, py, tol, &dcx, &dcy) && (o = ThingAt(px, py, tol + 8, IsDoorObj)) != NULL) {
        /* a door object over no door tile: its standing cell, then Up */
        int cx = Clamp(o->base.x >> 12, 0, sW - 1), cy = Clamp(((o->base.y >> 8) - 4) >> 4, 0, sH - 1), i;

        for (i = 0; i < 2 && !Supported(cx, cy) && Open(cx, cy + 1); i++)
            cy++;
        SetDoor(cx, cy);
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u door object type 0x%02X at %d,%d -> cell %d,%d", (unsigned)PortFrameNumber(), o->type,
                    (int)(o->base.x >> 8), (int)(o->base.y >> 8), cx, cy);
        return;
    }
    if ((o = ThingAt(px, py, tol, IsChest)) != NULL) {
        /* a chest: walk into it and press Up, as at a door */
        int cx = Clamp(o->base.x >> 12, 0, sW - 1), cy = Clamp(((o->base.y >> 8) - 4) >> 4, 0, sH - 1), i;

        for (i = 0; i < 2 && !Supported(cx, cy) && Open(cx, cy + 1); i++)
            cy++;                               /* stand on the floor in it  */
        SetDoor(cx, cy);
        sT.chest = 1;
        sT.mx = o->base.x >> 8;
        sT.my = (o->base.y >> 8) + o->base.unk3D / 2;
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u chest type 0x%02X at %d,%d", (unsigned)PortFrameNumber(), o->type,
                    (int)(o->base.x >> 8), (int)(o->base.y >> 8));
        return;
    }
    if ((o = ThingAt(px, py, tol, IsSwitch)) != NULL) {
        SetSwitch(o);
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u switch type 0x%02X at %d,%d%s -> stand %d,%d", (unsigned)PortFrameNumber(), o->type,
                    (int)(o->base.x >> 8), (int)(o->base.y >> 8), sT.wasDown ? " (down already)" : "", (int)sT.x,
                    (int)sT.y);
        return;
    }
    if ((o = PushAt(px, py, tol)) != NULL) {
        SetPush(o);
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u heavy block type 0x%02X at %d,%d -> from cell %d,%d%s", (unsigned)PortFrameNumber(),
                    o->type, (int)sT.mx, (int)sT.my, sT.cx, sT.cy, sT.air ? " (air)" : "");
        return;
    }
    if (Log() >= 3)
        PortLog("[tap] down %d,%d tol %d grid %dx%d room %u cl %02X", (int)px, (int)py, tol, sW, sH,
                (unsigned)sGridRoom, Cl(px >> 4, py >> 4));
    if (DoorNear(px, py, tol, &dcx, &dcy)) {
        SetDoor(dcx, dcy);
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u door at cell %d,%d", (unsigned)PortFrameNumber(), dcx, dcy);
        return;
    }
    if (BlockNear(px, py, tol / 2, &dcx, &dcy)) {
        SetBlock(dcx, dcy);
        sP.kind = G_MARK;
        if (Log())
            PortLog("[tap] f%u block at cell %d,%d (strength %u) -> from %d,%d%s", (unsigned)PortFrameNumber(), dcx,
                    dcy, (unsigned)((Attr(dcx, dcy) >> 20) & 0xF), sT.cx, sT.cy, sT.air ? " (air)" : "");
        return;
    }
    if ((o = ObjAt(px, py, tol, 1)) != NULL) {  /* food and the like: walk to it */
        px = o->base.x >> 8;
        py = o->base.y >> 8;
    }
    SetGo(px, py);
    if (Log() && sT.kind == T_GO)
        PortLog("[tap] f%u go %d,%d -> cell %d,%d%s", (unsigned)PortFrameNumber(), (int)px, (int)py,
                sT.cx, sT.cy, sT.air ? " (air)" : "");
}

static void GestureMove(s32 px, s32 py)
{
    int far = FingerWorld(26.0f, 8, 28);
    s32 dx = px - sP.x0, dy = py - sP.y0;

    if (dx * dx + dy * dy > far * far)
        sP.moved = 1;
    switch (sP.kind) {
    case G_KIRBY:
    case G_INHALE:
        /* from Kirby: straight down swallows (or drops the ability),
         * straight up calls for help (R, the phone), diagonally (up or
         * down) slides that way, sideways is a drag (not while inhaling:
         * then it is only the finger wandering) */
        if (!sP.moved)
            break;
        if (dy > 0 && 2 * Abs(dx) < dy) {
            SwipeKirby();
            sP.kind = G_DONE;
        } else if (dy < 0 && 2 * Abs(dx) < -dy) {
            if (Log())
                PortLog("[tap] f%u kirby: swipe up (call for help, R)", (unsigned)PortFrameNumber());
            Clear("swipe up");
            Action(K_R, 6);
            sP.kind = G_DONE;
        } else if (3 * Abs(dy) >= Abs(dx)) {
            KickKirby(dx < 0 ? -1 : 1);
            sP.kind = G_DONE;
        } else if (sP.kind == G_KIRBY) {
            sP.kind = G_DRAG;               /* dragged off Kirby: go there   */
            SetGo(px, py);
        }
        break;
    case G_MARK:
        if (sP.moved) {
            sP.kind = G_DRAG;
            SetGo(px, py);
        }
        break;
    case G_DRAG:
        if (Abs(px - sT.x) > 2 || Abs(py - sT.y) > 6 || sT.kind != T_GO)
            SetGo(px, py);
        break;
    default:
        break;
    }
}

static void GestureUp(s32 px, s32 py)
{
    if (sP.kind == G_KIRBY && !sP.moved)
        TapKirby();
    else if (sP.kind == G_DRAG)
        SetGo(px, py);
}

/* Pointer events wait for the frame (the world is read in one place). */
#define EVQ 32
static struct { int kind, id; s32 x, y; } sEv[EVQ];
static int sEvN;

void PortTapWorld(int kind, int wx, int wy, int id)
{
    if (sEvN < EVQ) {
        sEv[sEvN].kind = kind;
        sEv[sEvN].id = id;
        sEv[sEvN].x = wx;
        sEv[sEvN].y = wy;
        sEvN++;
    }
}

void PortTapPointer(int kind, float x, float y, int id)
{
    float vx, vy;

    if (!sV.ok || sV.world[2] <= 0 || sV.world[3] <= 0) {
        if (kind == PORT_UI_UP || kind == PORT_UI_CANCEL)
            PortTapWorld(kind, sP.x, sP.y, id);
        return;
    }
    vx = (x - (float)sV.world[0]) * (float)sV.viewW / (float)sV.world[2];
    vy = (y - (float)sV.world[1]) * (float)sV.viewH / (float)sV.world[3];
    PortTapWorld(kind, (int)floorf(vx) + sV.camX + sV.viewX, (int)floorf(vy) + sV.camY + sV.viewY, id);
}

static void Events(void)
{
    int i;

    for (i = 0; i < sEvN; i++) {
        int kind = sEv[i].kind, id = sEv[i].id;
        s32 x = sEv[i].x, y = sEv[i].y;

        if (kind == PORT_UI_DOWN) {
            if (sP.on && id != sP.id) {         /* a second finger: attack   */
                sSecondId = id;                 /* (on release, if quick: a  */
                sSecondT0 = sTick;              /* hold is L, which must be  */
                continue;                       /* its own fresh press)      */
            }
            memset(&sP, 0, sizeof(sP));
            sP.on = 1;
            sP.id = id;
            sP.x0 = sP.x = x;
            sP.y0 = sP.y = y;
            sP.t0 = sTick;
            GestureDown(x, y);
        } else if (!sP.on || id != sP.id) {
            if (id == sSecondId && kind != PORT_UI_MOVE) {
                if (kind == PORT_UI_UP && sTick - sSecondT0 < TAP_HOLD_L)
                    Action(K_B, 6);
                sSecondId = -1;
            }
        } else if (kind == PORT_UI_MOVE) {
            sP.x = x;
            sP.y = y;
            GestureMove(x, y);
        } else {
            if (kind == PORT_UI_UP)
                GestureUp(x, y);
            sP.on = 0;
        }
    }
    sEvN = 0;
}

/* An enemy in Kirby's way: within `reach` px ahead in direction dir, about
 * his height.  Not the one he is after (sT.obj). */
static int EnemyAhead(s32 kx, s32 ky, int dir, int reach)
{
    struct Object *ob;
    int i = 0;

    if (!dir)
        return 0;
    while ((ob = NextObject(&i)) != NULL) {
        s32 x, y, w, h, gap;

        if (ob == sT.obj || !IsEnemy(ob))
            continue;
        Box(ob, &x, &y, &w, &h);
        gap = dir > 0 ? x - (kx + 8) : (kx - 8) - (x + w);
        if (gap < -6 || gap > reach || y > ky + 10 || y + h < ky - 12)
            continue;
        return 1;
    }
    return 0;
}

/* --- the controller ---------------------------------------------------------- */

/* The game's physics, measured (KATAM_TAP_LOG=2 in room 801): y grows down
 * and yspeed > 0 is up, 8.8.  A ground jump starts at yspeed 1024 and loses
 * 38 a frame while A is held (a release cuts it to ~138): 4 frames of A rise
 * 16 px, 10 frames 34, ~23 frames 54 (the most).  A press in the air puffs
 * Kirby up (flags 0x40); held, a puffed Kirby climbs at 336 (1.3 px a frame)
 * for as long as it is held, and released floats down at 288 at most.  B
 * while puffed exhales: he falls at 640.  Walking is 332 (1.3 px), flying
 * sideways 272.  He lands still puffed, and stays so until B. */
static int JumpFrames(int cells)
{
    return cells <= 1 ? 7 : cells == 2 ? 14 : 23;
}

/* Where the path goes from Kirby's cell. */
typedef struct {
    int n;                      /* steps traced                              */
    int rise;                   /* of the next five, how many climb          */
    int first;                  /* the first step: 0 across, -1 up, 1 down   */
    int gap;                    /* the next cell across has nothing under it */
    s32 aimX, aimY;             /* a point a few steps out (world px)        */
} Plan;

static void PlanFrom(int s, Plan *p)
{
    int sx = s % sW, sy = s / sW, i, x = sx, y = sy, k;

    memset(p, 0, sizeof(*p));
    p->n = sPathN = Trace(s, NAV_PATH_MAX);
    p->aimX = sT.x;
    p->aimY = sT.y;
    for (i = 0; i < p->n && i < 5; i++) {
        int c = sPath[i], cx = c % sW, cy = c / sW;

        if (cy < y)
            p->rise++;
        if (i == 0)
            p->first = cy < y ? -1 : cy > y ? 1 : 0;
        x = cx;
        y = cy;
    }
    (void)x;
    /* the aim: two steps out, or the target itself near the end */
    k = p->n - 1 < 2 ? p->n - 1 : 2;
    if (p->n > 3 && k >= 0) {
        p->aimX = (sPath[k] % sW) * 16 + 8;
        p->aimY = (sPath[k] / sW) * 16 + 8;
    }
    if (p->n > 0 && p->first == 0) {
        int c0x = sPath[0] % sW;

        if (!Supported(c0x, sy)) {
            int down = 0;

            for (i = 1; i < p->n && i < 4; i++)
                if (sPath[i] / sW > sy)
                    down = 1;
            p->gap = !down;
        }
    }
}

/* Below Kirby, down to the room's bottom, is there anything to land on? */
static int GroundBelow(int cx, int cy)
{
    int y;

    for (y = cy + 1; y < sH; y++)
        if (Cl(cx, y) & (CL_SOLID | CL_PLAT | CL_SLOPE | CL_WATER))
            return 1;
    return 0;
}

/* Steer one frame toward the target along the flow.  arriveR: how near in
 * x counts as there; hover: an air target (hold the height). */
static u16 Steer(struct Kirby *k, s32 kx, s32 ky, int arriveR, int hover)
{
    u32 fl = k->base.flags;
    int air = !(k->base.unk62 & KH_FLOOR), fly = (fl & KF_FLY) != 0, water = (k->base.unk58 & 2) != 0;
    int s = KirbyCell(kx, ky), scx = s % sW, scy = s / sW;
    int dx, dy, dir, wantUp, wantDown, end, hop, holdA = 0;
    u16 keys = 0;
    Plan p;

    PlanFrom(s, &p);
    end = p.n <= 2 || s == sGoal;
    if (end) {
        p.aimX = sT.x;
        p.aimY = sT.y;
    }
    dx = p.aimX - kx;
    dy = p.aimY - ky;
    /* across: toward the aim, the target's own x once near it */
    if (end || (Abs(sT.x - kx) <= 20 && Abs(sT.y - ky) <= 10))
        dx = sT.x - kx;
    dir = dx > arriveR ? 1 : dx < -arriveR ? -1 : 0;
    /* near the end on the ground: coast when the slide alone gets there
     * (walking Kirby slows by ~14/256 px a frame per frame: a walk slides
     * 15 px, a run 31) */
    if (dir && !air && Abs(sT.x - kx) < 64) {
        s32 vx = k->base.xspeed;

        if ((vx > 0) == (dir > 0) && vx != 0 && vx * vx / (28 * 256) >= Abs(dx) - 2)
            dir = 0;
    }
    /* the path goes up or down first past a corner (the cell across, or
     * the one diagonally, is solid): line up in his own column first, or
     * he presses into the corner and hangs there */
    if (p.first != 0 && dir && !(end && Abs(sT.x - kx) < 12)) {
        int nx = scx + dir;

        if (!Open(nx, scy) || !Open(nx, scy + p.first)) {
            s32 c = scx * 16 + 8;

            dir = c > kx + 2 ? 1 : c < kx - 2 ? -1 : 0;
        }
    }
    /* the path drops straight down but he is standing: he is wider than
     * his cell, and the floor beside it holds him up -- step off it, to
     * the far side of his own cell */
    if (!air && p.first > 0 && !water && (k->base.unk62 & KH_FLOOR)
        && !(Cl(scx, scy + 1) & (CL_SOLID | CL_PLAT | CL_SLOPE))) {
        int r = (Cl(scx + 1, scy + 1) & (CL_SOLID | CL_PLAT | CL_SLOPE)) != 0;
        int l = (Cl(scx - 1, scy + 1) & (CL_SOLID | CL_PLAT | CL_SLOPE)) != 0;
        s32 want = scx * 16 + 8 + (r && !l ? -5 : l && !r ? 5 : 0);

        dir = want > kx + 1 ? 1 : want < kx - 1 ? -1 : 0;
    }
    if (dir > 0)
        keys |= K_RIGHT;
    else if (dir < 0)
        keys |= K_LEFT;
    /* a long way along the ground: run (a double tap, as the CPU does) */
    if (sC.dashCool > 0)
        sC.dashCool--;
    if (sC.dashPhase == 0 && dir && !air && !fly && !water && !(fl & 0x10) && !sC.dashCool
        && Abs(sT.x - kx) > 80 && p.first == 0 && !p.rise && !p.gap) {
        sC.dashPhase = 1;
        sC.dashT = 1;
        sC.dashCool = 90;
    }
    if (sC.dashPhase == 1) {
        if (--sC.dashT <= 0) {
            sC.dashPhase = 2;
            sC.dashT = 4;
        }
    } else if (sC.dashPhase == 2) {
        keys &= (u16)~(K_LEFT | K_RIGHT);
        if (--sC.dashT <= 0)
            sC.dashPhase = 0;
    }

    /* up and down: puffed, he holds the row's centre or a little above it */
    wantUp = p.first < 0 || dy < (fly ? -3 : -6) || (hover && end && ky > sT.y + 2)
          || (air && !GroundBelow(scx, scy) && p.first >= 0 && !(end && dy > 8));
    /* across, in the air, into a row with solid ground above or below the
     * next cell: his body is a row tall, so he has to be inside the row to
     * pass -- keep his centre on the row's (a hair below a ceiling lip, a
     * hair above a floor lip) */
    if (air && dir && p.first == 0 && !water) {
        int nx = scx + dir, above = !Open(nx, scy - 1), below = !Open(nx, scy + 1);
        s32 c = scy * 16 + 8;

        if (above && ky < c)
            wantUp = 0;
        else if (below && ky > c - 1)
            wantUp = 1;
    }
    /* an enemy in the way: over it (a hop, or a little more height) */
    hop = !water && dir && !(end && Abs(sT.x - kx) < 20) && EnemyAhead(kx, ky, dir, air ? 20 : 28);
    if (hop && air)
        wantUp = 1;
    wantDown = !wantUp && (p.first > 0 || dy > 12);

    /* a mouthful cannot fly: when the way climbs more than a jump (or a
     * pit opens under him), swallow it (Down) -- he keeps whatever it gives */
    if (sC.swallowCool > 0)
        sC.swallowCool--;
    if ((fl & KF_MOUTH) && !water && sAct.frames == 0 && !sC.swallowCool
        && ((!air && (p.rise >= 3 || dy < -44)) || (air && wantUp && !GroundBelow(scx, scy)))) {
        if (Log())
            PortLog("[tap] f%u swallow: the way needs flying", (unsigned)PortFrameNumber());
        sC.swallowCool = 90;
        Action(K_DOWN, 4);
        return keys;
    }
    if (water) {
        /* swimming: Up/Down steer, A strokes up */
        if (wantUp) {
            keys |= K_UP;
            holdA = (sTick & 7) < 3;
        } else if (wantDown)
            keys |= K_DOWN;
    } else if (!air) {
        if (fly) {
            /* landed puffed: climb on, or let the air out to walk */
            if (wantUp)
                holdA = 1;
            else if (!sC.bGap && !p.rise) {
                sC.bGap = 20;
                Action(K_B, 2);
            }
        } else if (p.first < 0 && dir && !p.gap
                   && ((Cl(scx, scy) | Cl(scx + dir, scy) | Cl(scx + dir, scy + 1)) & CL_SLOPE)
                   && p.rise <= 1) {
            ;                                   /* up a slope: just walk     */
        } else if (sC.jumpHold == 0 && sC.aGap == 0
                   && (wantUp || p.gap || hop || ((k->base.unk62 & KH_WALL) && dir && p.rise))) {
            int h = p.rise ? p.rise : wantUp ? (-dy + 15) / 16 : hop ? 2 : 1;

            sC.jumpHold = JumpFrames(h > 0 ? h : 1);
        } else if (p.first > 0 && (Cl(scx, scy + 1) & CL_DROP))
            keys |= K_DOWN;                     /* through the floor         */
    } else if (wantDown && !fly && (Cl(scx, scy + 1) & CL_WATER)) {
        keys |= K_DOWN;                         /* bobbing on water: dive    */
    } else if (sC.jumpHold == 0) {
        /* in the air: puff up and climb while the path climbs, float down
         * when it does not; let the air out for a long way down */
        if (wantUp && sC.aGap == 0 && (fly || k->base.yspeed < 0x180))
            holdA = 1;
        else if (fly && !hover && !sC.bGap && sAct.frames == 0 && dy > 40 && Abs(dx) < 12) {
            sC.bGap = 30;
            Action(K_B, 2);
        }
    }
    /* the jump's A: held for its frames, then let go a moment (an edge) */
    if (sC.jumpHold > 0) {
        holdA = 1;
        if (--sC.jumpHold == 0)
            sC.aGap = 2;
    }
    if (holdA)
        keys |= K_A;
    if (Log() >= 3)
        PortLog("[tap] f%u steer s %d,%d n %d first %d rise %d gap %d aim %d,%d up %d dn %d",
                (unsigned)PortFrameNumber(), scx, scy, p.n, p.first, p.rise, p.gap, (int)p.aimX, (int)p.aimY,
                wantUp, wantDown);
    return keys;
}

/* How near an ability's B reaches, roughly (px in front of Kirby). */
static int AbilityRange(u8 a)
{
    switch (a) {
    case KIRBY_ABILITY_CUTTER: case KIRBY_ABILITY_LASER: case KIRBY_ABILITY_UFO:
    case KIRBY_ABILITY_CUPID: case KIRBY_ABILITY_MISSILE: case KIRBY_ABILITY_CRASH:
        return 72;                              /* shots                     */
    case KIRBY_ABILITY_FIRE: case KIRBY_ABILITY_ICE: case KIRBY_ABILITY_BEAM:
    case KIRBY_ABILITY_BOMB: case KIRBY_ABILITY_COOK: case KIRBY_ABILITY_MAGIC:
        return 40;                              /* breath, whip, a throw     */
    default:
        return 26;                              /* swords, hammers, fists    */
    }
}

/* The engagement with a marked enemy: stand off at range on its row, face
 * it, then inhale (no ability), spit (a mouthful) or attack (an ability). */
static u16 Engage(struct Kirby *k, s32 kx, s32 ky)
{
    struct Object *o = sT.obj;
    s32 ex = o->base.x >> 8, ey = o->base.y >> 8, adx = Abs(ex - kx), ady = Abs(ey - ky);
    u32 fl = k->base.flags;
    int mouth = (fl & KF_MOUTH) != 0, normal = k->ability == KIRBY_ABILITY_NORMAL;
    int range = mouth ? 72 : normal ? 40 : AbilityRange(k->ability), near = mouth ? 24 : normal ? 12 : 10;
    int side = kx < ex ? -1 : 1;                /* the side Kirby stands on */
    int faceLeft = ex < kx, facing = ((fl & KF_LEFT) != 0) == faceLeft;
    u16 keys = 0;

    if (adx <= range && ady <= 14) {
        /* in range: puffed first lets the air out (which is itself a shot) */
        if ((fl & KF_FLY) && !sAct.frames && !sC.bGap) {
            sC.bGap = 10;
            Action(K_B, 2);
            return 0;
        }
        if (!facing) {
            sC.bHold = 0;
            return faceLeft ? K_LEFT : K_RIGHT;
        }
        if (normal && !mouth) {
            /* inhale: B held until he has a mouthful, or 60 frames */
            if (sC.bGap > 0)
                return 0;
            if (++sC.bHold < 60)
                return K_B;
            sC.bHold = 0;
            sC.bGap = 12;
            sT.tries++;
            return 0;
        }
        /* spit, or the ability's attack: B pulses */
        if (sC.bGap > 0)
            return 0;
        if (++sC.bHold <= 8)
            return K_B;
        sC.bHold = 0;
        sC.bGap = mouth ? 20 : 8;
        sT.tries++;
        return 0;
    }
    sC.bHold = 0;
    /* walk to the stand-off point: on its row, at range on the near side */
    {
        s32 off = Clamp(range - 12, near, 64), gx = ex + side * off, gy = ey;
        int cx = Clamp(gx >> 4, 0, sW - 1), cy = Clamp(gy >> 4, 0, sH - 1), goal;

        if (!Open(cx, cy)) {
            gx = ex - side * off;               /* the far side              */
            cx = Clamp(gx >> 4, 0, sW - 1);
            if (!Open(cx, cy)) {
                gx = ex;
                cx = Clamp(ex >> 4, 0, sW - 1);
            }
        }
        goal = cy * sW + cx;
        if (goal != sGoal || sFlowAge > 20) {
            Flow(goal);
            if (sDist[KirbyCell(kx, ky)] == NAV_INF) {
                Flow(NearestReachable(KirbyCell(kx, ky), gx, gy));
                sGoal = goal;
            }
        }
        sT.x = gx;
        sT.y = gy;
        keys = Steer(k, kx, ky, 4, 0);
        sT.x = ex;
        sT.y = ey;
    }
    return keys;
}

/* One frame of the autopilot.  Returns the keys. */
/* At the switch and it is not down.  Walking onto it nearly always does it
 * (his body is a hit); if not, a hop onto it, then hits from beside it:
 * face it, B (the ability's attack, a star, an air puff), a slide (Down+A),
 * B, a slide.  Each try waits 20 frames on the ground first. */
static u16 PressSwitch(struct Kirby *k, s32 kx)
{
    int left = (sT.obj->base.x >> 8) < kx;

    if (k->base.flags & KF_FLY) {               /* puffed: let it out (a puff is a hit) */
        if (!sC.bGap) {
            sC.bGap = 20;
            Action(K_B, 2);
        }
        return 0;
    }
    if (!(k->base.unk62 & KH_FLOOR))
        return 0;
    if (sT.hit && ((k->base.flags & KF_LEFT) != 0) != left)
        return left ? K_LEFT : K_RIGHT;         /* turn to face it           */
    if (++sT.wait < 20)
        return 0;
    sT.wait = 0;
    if (Log())
        PortLog("[tap] f%u switch: %s try %d", (unsigned)PortFrameNumber(), sT.hit ? "hit" : "stand", sT.tries);
    if (!sT.hit) {
        if (sT.tries++ == 0) {
            Action(K_A, 4);                     /* a hop, landing on it      */
            return 0;
        }
        sT.hit = 1;                             /* step off, hit it          */
        sT.tries = 0;
        SwitchSpot();
        return 0;
    }
    switch (sT.tries++) {
    case 0:
    case 2:
        Action(K_B, 8);
        break;
    case 1:
    case 3:
        Slide(left ? -1 : 1);
        break;
    default:
        Fail("switch: it did not go down");
        break;
    }
    return 0;
}

/* Beside a breakable block, facing it, and it is still there.  Each try
 * waits 12 frames: B (a star with a mouthful, the ability's attack, an air
 * puff when puffed; with nothing, a long inhale, which may pull the block
 * or an enemy in), then a star if something came in, else a slide; again. */
static u16 HitBlock(struct Kirby *k)
{
    int left = sT.face < 0, fl = k->base.flags;

    if (!(fl & KF_FLY) && ((fl & KF_LEFT) != 0) != left)
        return left ? K_LEFT : K_RIGHT;         /* turn to face it           */
    if (!(fl & KF_FLY) && !(k->base.unk62 & KH_FLOOR))
        return 0;
    if (++sT.wait < 12)
        return 0;
    sT.wait = 0;
    if (Log())
        PortLog("[tap] f%u block %d,%d: try %d (%s)", (unsigned)PortFrameNumber(), sT.bx, sT.by, sT.tries,
                (fl & KF_MOUTH) ? "mouthful" : (fl & KF_FLY) ? "puffed" : k->ability ? "ability" : "nothing");
    switch (sT.tries++) {
    case 0:
    case 2:
        if ((fl & (KF_MOUTH | KF_FLY)) || k->ability != KIRBY_ABILITY_NORMAL)
            Action(K_B, 6);
        else
            Action(K_B, 40);
        break;
    case 1:
    case 3:
        if ((fl & (KF_MOUTH | KF_FLY)) || !(k->base.unk62 & KH_FLOOR))
            Action(K_B, 6);
        else
            Slide(left ? -1 : 1);
        break;
    default:
        Fail("block: it did not break");
        break;
    }
    return 0;
}

static u16 Drive(void)
{
    struct Kirby *k = Me();
    struct LevelInfo *li = Li();
    s32 kx = k->base.x >> 8, ky = k->base.y >> 8;
    u16 keys = 0, room = li->currentRoom;
    int s;

    if (sT.fail > 0)
        sT.fail--;
    if (Log()) {
        static s8 lastHp = -1;

        if (lastHp >= 0 && k->hp < lastHp)
            PortLog("[tap] f%u hurt: hp %d -> %d at %d,%d (target kind %d)", (unsigned)PortFrameNumber(), lastHp,
                    k->hp, (int)kx, (int)ky, sT.kind);
        lastHp = k->hp;
    }
    /* a room change: a door target is done, anything else is moot */
    if (sT.kind != T_NONE && room != sT.room) {
        if (Log() && sT.kind == T_DOOR)
            PortLog("[tap] f%u door taken: room %u -> %u", (unsigned)PortFrameNumber(), (unsigned)sT.room,
                    (unsigned)room);
        Clear("new room");
    }
    if (GridUpdate() && sT.kind != T_NONE)
        sGoal = -1;                             /* the map changed: re-flow  */
    if (sW <= 0)
        return 0;
    if (sC.aGap > 0)
        sC.aGap--;
    if (sC.bGap > 0)
        sC.bGap--;
    /* a one-shot action has the buttons */
    if (sAct.frames > 0) {
        if (sAct.gap > 0) {
            sAct.gap--;
            return 0;
        }
        keys = sAct.keys;
        if (--sAct.frames == 0 && sAct.nNext > 0) {
            int j;

            sAct.keys = sAct.next[0];
            sAct.frames = sAct.nextFrames[0];
            for (j = 1; j < sAct.nNext; j++) {
                sAct.next[j - 1] = sAct.next[j];
                sAct.nextFrames[j - 1] = sAct.nextFrames[j];
            }
            sAct.nNext--;
        }
        return keys;
    }
    /* two fingers held: L, for as long as they stay ("hold L to start
     * over", the tutorial's last card) */
    if (sP.on && sSecondId >= 0 && sTick - sSecondT0 >= TAP_HOLD_L) {
        if (sTick - sSecondT0 == TAP_HOLD_L) {
            if (Log())
                PortLog("[tap] f%u two fingers held: L", (unsigned)PortFrameNumber());
            Clear("two fingers");
            sP.kind = G_DONE;                   /* lifting it is not a tap   */
        }
        return K_L;
    }
    /* a finger held still on Kirby: inhale (B) for as long as it stays */
    if (sP.on && sP.kind == G_KIRBY && sSecondId < 0 && sTick - sP.t0 >= TAP_HOLD) {
        s32 dx = sP.x - sP.x0, dy = sP.y - sP.y0;
        int slop = FingerWorld(9.0f, 3, 10);

        if (dx * dx + dy * dy <= slop * slop) {
            if (Log())
                PortLog("[tap] f%u kirby: hold (inhale)", (unsigned)PortFrameNumber());
            Clear("hold kirby");
            sP.kind = G_INHALE;
        }
    }
    if (sP.on && sP.kind == G_INHALE)
        return K_B;
    /* the kinds with a thing to act on: is it done? */
    if (sT.kind == T_BLOCK) {
        if (!Breakable(sT.bx, sT.by)) {
            if (Log())
                PortLog("[tap] f%u block %d,%d broken after %d frames", (unsigned)PortFrameNumber(), sT.bx, sT.by,
                        sT.frames);
            Clear("block broken");
            return 0;
        }
        if (sT.frames > 1200) {
            Fail("block: gave up");
            return 0;
        }
    } else if (sT.kind == T_PUSH) {
        s32 x, y, w, h, ox;

        if (!ObjLive(sT.obj, sT.objType)) {
            Clear("block gone");
            return 0;
        }
        Box(sT.obj, &x, &y, &w, &h);
        ox = sT.obj->base.x >> 8;
        if (Abs(ox - sT.startX) >= 8 || Abs(y + h / 2 - sT.my) >= 8) {
            if (Log())
                PortLog("[tap] f%u moved the block %d px (now %d,%d) after %d frames", (unsigned)PortFrameNumber(),
                        (int)(ox - sT.startX), (int)(x + w / 2), (int)(y + h / 2), sT.frames);
            Clear("pushed");
            return 0;
        }
        if (sT.frames > 900) {
            Fail("push: gave up");
            return 0;
        }
    }
    if (sT.kind == T_NONE)
        return 0;
    sT.frames++;
    sFlowAge++;

    switch (sT.kind) {
    case T_ENEMY:
        if (!ObjAlive(sT.obj, sT.objType)) {
            if (Log())
                PortLog("[tap] f%u enemy gone (%s) after %d frames", (unsigned)PortFrameNumber(),
                        (k->base.flags & KF_MOUTH) ? "inhaled" : "defeated", sT.frames);
            Clear("enemy gone");
            return 0;
        }
        if (sT.frames > 900 || sT.tries > 6) {
            Fail("enemy: gave up");
            return 0;
        }
        keys = Engage(k, kx, ky);
        break;
    case T_SWITCH:
        if (!ObjLive(sT.obj, sT.objType)) {
            Clear("switch gone");
            return 0;
        }
        if (!SwitchDown(sT.obj))
            sT.wasDown = 0;
        else if (!sT.wasDown && (sT.hit || sT.arrived)) {   /* stood on: stopped on it */
            if (Log())
                PortLog("[tap] f%u switch down (type 0x%02X, %s) after %d frames", (unsigned)PortFrameNumber(),
                        sT.objType, sT.hit ? "hit" : "stood on", sT.frames);
            Clear("switch down");
            return 0;
        }
        if (sT.frames > 1200) {
            Fail("switch: gave up");
            return 0;
        }
        /* fall through: walk there */
    case T_BLOCK:
    case T_PUSH:
    case T_DOOR:
    case T_GO: {
        int hover = (sT.kind == T_GO || sT.kind == T_BLOCK || sT.kind == T_PUSH) && sT.air;
        int goal = sT.cy * sW + sT.cx;
        int dx, dy, there;

        if (goal != sGoal || sFlowAge > 30) {
            Flow(goal);
            s = KirbyCell(kx, ky);
            if (sDist[s] == NAV_INF) {
                int alt = NearestReachable(s, sT.x, sT.y);

                if (Log() && sFlowAge == 0 && goal != sGoal)
                    PortLog("[tap] f%u target cell %d,%d not reachable: nearest %d,%d",
                            (unsigned)PortFrameNumber(), sT.cx, sT.cy, alt % sW, alt / sW);
                Flow(alt);
                sGoal = goal;                   /* (no re-flow next frame)   */
                sT.unreach = 1;
            } else
                sT.unreach = 0;
        }
        /* as near as he can get, and that is here: say so (the red ring) */
        if (sT.unreach && sDist[KirbyCell(kx, ky)] <= 2 && (k->base.unk62 & KH_FLOOR)) {
            Fail("cannot get there");
            return 0;
        }
        /* arrived? */
        dx = sT.x - kx;
        dy = sT.y - ky;
        s = KirbyCell(kx, ky);
        there = sT.kind == T_DOOR ? (s == goal || (Abs(dx) <= 6 && Abs(dy) <= 10))
              : hover ? (Abs(dx) <= 8 && Abs(dy) <= 10)
              : (Abs(dx) <= (sT.kind == T_SWITCH && !sT.hit ? 3 : 6) && Abs(dy) <= 12
                 && (k->base.unk62 & KH_FLOOR) && Abs(k->base.xspeed) < 0x80);
        if (there) {
            if (!sT.arrived && Log())
                PortLog("[tap] f%u arrived room %u at %d,%d (target %d,%d) after %d frames",
                        (unsigned)PortFrameNumber(), (unsigned)room, (int)kx, (int)ky,
                        (int)sT.x, (int)sT.y, sT.frames);
            sT.arrived++;
            if (sT.kind == T_DOOR) {
                if (sT.chest && (k->animationIndex == 96 || k->animationIndex == 55)) {
                    if (Log())                  /* sub_0804B6FC: the opening  */
                        PortLog("[tap] f%u chest opened", (unsigned)PortFrameNumber());
                    Clear("chest opened");
                    return 0;
                }
                /* on the ground, not puffed, then Up */
                if (k->base.flags & KF_FLY) {
                    if (!sC.bGap) {
                        sC.bGap = 20;
                        Action(K_B, 2);
                    }
                    return 0;
                }
                if (!(k->base.unk62 & KH_FLOOR))
                    return 0;
                if (++sC.upHold <= 10)
                    return K_UP;
                if (sC.upHold > 40) {
                    sC.upHold = 0;
                    if (++sT.tries > 4)
                        Fail(sT.chest ? "chest: Up did nothing" : "door: Up did nothing");
                }
                return 0;
            }
            if (sT.kind == T_SWITCH)
                return PressSwitch(k, kx);
            if (sT.kind == T_BLOCK)
                return HitBlock(k);
            if (sT.kind == T_PUSH)
                return MoveBlock(k);
            if (hover) {
                keys = Steer(k, kx, ky, 3, 1);
                break;
            }
            /* standing on it: done, unless a finger still holds it */
            if (k->base.flags & KF_FLY) {
                if (!sC.bGap) {
                    sC.bGap = 20;
                    Action(K_B, 2);
                }
                return 0;
            }
            if (!(sP.on && sP.kind == G_DRAG))
                Clear("arrived");
            return 0;
        }
        if (sT.arrived && !hover && sT.kind != T_DOOR)
            sT.arrived = 0;
        keys = Steer(k, kx, ky, sT.kind == T_DOOR || sT.kind == T_SWITCH ? 3 : 4, hover);
        break;
    }
    default:
        return 0;
    }

    /* the watchdog: progress is the cost-to-go at Kirby's cell */
    s = KirbyCell(kx, ky);
    if (sGoal >= 0 && sDist[s] != NAV_INF && (int)sDist[s] < sT.best) {
        sT.best = sDist[s];
        sT.stall = 0;
        sC.unstick = 0;
    } else if (!sT.arrived) {
        sT.stall++;
        if (sT.stall == 90 || sT.stall == 180) {
            /* a hop, then a long one: most stalls are a lip or a slope */
            sC.unstick++;
            if (!(k->base.flags & KF_FLY) && sC.jumpHold == 0)
                sC.jumpHold = sT.stall == 90 ? 10 : 23;   /* (A also fires a cannon he sits in) */
            if (Log())
                PortLog("[tap] f%u stalled at %d,%d: unstick %d", (unsigned)PortFrameNumber(), (int)kx, (int)ky,
                        sC.unstick);
        }
        if (sT.stall > 330 && sT.kind != T_ENEMY)
            Fail("stuck");
    }
    return keys;
}

unsigned short PortTapFrame(unsigned short hostKeys)
{
    u16 keys = 0;

    sTick++;
    if (!PortTapEnabled() || PortInputLogReplaying()) {
        sKeys = 0;
        sEvN = 0;
        return 0;
    }
    if (!InPlay()) {
        /* pause, menus, cutscenes: hands off, targets kept (a door's
         * room change clears it when play resumes) */
        sEvN = 0;
        sP.on = 0;
        sKeys = 0;
        return 0;
    }
    /* a real button wins: the autopilot lets go (not Start or Select: the
     * pause button, and the page's own Select-held-to-leave) */
    if (hostKeys & 0x3FF & ~(K_START | K_SELECT)) {
        if (sT.kind != T_NONE || sAct.frames)
            Clear("manual input");
        sAct.frames = 0;
        sAct.nNext = 0;
        sEvN = 0;
    } else {
        Events();
        keys = Drive();
    }
    if (Log() >= 2 || (Log() && (sTick % 60) == 0 && sT.kind != T_NONE)) {
        struct Kirby *k = Me();

        PortLog("[tap] f%u k %d,%d v %d,%d fl %X h %X t%d %d,%d g%d d%d keys %03X", (unsigned)PortFrameNumber(),
                (int)(k->base.x >> 8), (int)(k->base.y >> 8), k->base.xspeed, k->base.yspeed,
                (unsigned)(k->base.flags & 0xFF), (unsigned)k->base.unk62, sT.kind, (int)sT.x, (int)sT.y, sGoal,
                sGoal >= 0 && sW > 0 ? (int)sDist[KirbyCell(k->base.x >> 8, k->base.y >> 8)] : -1, keys | hostKeys);
    }
    sKeys = keys;
    return keys;
}

/* --- the native test hook: devices-script commands ------------------------------
 * (platform/native/host_sdl.c RunStep, tests/tap_nav.sh)
 *   tap X Y            a tap at world pixel X Y (down and up)
 *   tapcell CX CY      a tap at the centre of cell CX CY
 *   down X Y / move X Y / up X Y   a held finger
 *   tapkirby           a tap on Kirby
 *   swipekirby         a swipe down from Kirby
 *   tapenemy           a tap on the nearest enemy
 *   tapswitch          a tap on the nearest switch
 *   tappush            a tap on the nearest pushable block
 *   downkirby / upkirby   a finger down on Kirby, and up (held between: inhale)
 *   kickkirby L|R      a swipe from Kirby diagonally down that way
 *   swipeupkirby       a swipe up from Kirby (R: call for help)
 *   down2 / up2        a second finger down near Kirby, and up (held: L)
 * Returns 1 if the command was one of these. */
int PortTapScript(const char *cmd)
{
    int a = 0, b = 0;
    struct Kirby *k = Me();
    s32 kx = k->base.x >> 8, ky = k->base.y >> 8;

    if (sscanf(cmd, "tapcell %d %d", &a, &b) == 2) {
        PortTapWorld(PORT_UI_DOWN, a * 16 + 8, b * 16 + 8, 1);
        PortTapWorld(PORT_UI_UP, a * 16 + 8, b * 16 + 8, 1);
    } else if (sscanf(cmd, "tap %d %d", &a, &b) == 2) {
        PortTapWorld(PORT_UI_DOWN, a, b, 1);
        PortTapWorld(PORT_UI_UP, a, b, 1);
    } else if (sscanf(cmd, "down %d %d", &a, &b) == 2) {
        PortTapWorld(PORT_UI_DOWN, a, b, 1);
    } else if (sscanf(cmd, "move %d %d", &a, &b) == 2) {
        PortTapWorld(PORT_UI_MOVE, a, b, 1);
    } else if (sscanf(cmd, "up %d %d", &a, &b) == 2) {
        PortTapWorld(PORT_UI_UP, a, b, 1);
    } else if (sscanf(cmd, "taprandom %d", &a) == 1) {
        /* a tap on a random cell he can stand in and reach, 3..25 cells
         * away (tools/tap_sweep.sh): the sweep's way of asking "can he get
         * there?" in rooms nobody wrote a script for */
        static u32 seed;
        int s0, i, n = 0, pick = -1;

        GridUpdate();
        if (sW <= 0)
            return 1;
        if (!seed || a > 0)
            seed = (u32)a * 2654435761u + 1;
        s0 = KirbyCell(kx, ky);
        NearestReachable(s0, kx, ky);           /* fills sSeen: what he can reach */
        for (i = 0; i < sW * sH; i++) {
            int x = i % sW, y = i / sW, d = Abs(x - s0 % sW) + Abs(y - s0 / sW);

            if (!sSeen[i] || !Supported(x, y) || (Cl(x, y) & (CL_HAZ | CL_WATER)) || d < 3 || d > 25 || y >= sH - 1)
                continue;
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % (u32)++n == 0)
                pick = i;
        }
        if (pick < 0) {
            PortLog("[tap] f%u taprandom: nothing in reach", (unsigned)PortFrameNumber());
            return 1;
        }
        PortLog("[tap] f%u taprandom room %u cell %d,%d from %d,%d", (unsigned)PortFrameNumber(),
                (unsigned)Li()->currentRoom, pick % sW, pick / sW, s0 % sW, s0 / sW);
        PortTapWorld(PORT_UI_DOWN, pick % sW * 16 + 8, pick / sW * 16 + 8, 1);
        PortTapWorld(PORT_UI_UP, pick % sW * 16 + 8, pick / sW * 16 + 8, 1);
    } else if (strcmp(cmd, "dumpgrid") == 0) {
        /* the nav classes, a row per line: # solid, - one-way, = drop-through,
         * ~ water, ^ hazard, D door, H ladder, / slope, . air, K Kirby */
        char line[160];
        int x, y;

        GridUpdate();
        PortLog("[tap] grid room %u %dx%d kirby %d,%d ability %d flags %X hp %d battery %d", (unsigned)Li()->currentRoom,
                sW, sH, (int)kx, (int)ky, k->ability, (unsigned)(k->base.flags & 0xFF), k->hp, k->battery);
        for (y = 0; y < sH; y++) {
            int n = snprintf(line, sizeof line, "[tap] %3d ", y);

            for (x = 0; x < sW && n < (int)sizeof(line) - 2; x++) {
                u8 c = Cl(x, y);
                char ch = Breakable(x, y) ? 'B' : (c & CL_HAZ) ? '^' : (c & CL_SOLID) ? '#' : (c & CL_DOOR) ? 'D' : (c & CL_DROP) ? '='
                        : (c & CL_PLAT) ? '-' : (c & CL_WATER) ? '~' : (c & CL_LADDER) ? 'H' : (c & CL_SLOPE) ? '/' : '.';

                if (x == kx >> 4 && y == ky >> 4)
                    ch = 'K';
                line[n++] = ch;
            }
            line[n] = '\0';
            PortLog("%s", line);
        }
        {
            struct Object *ob;
            int i = 0;

            while ((ob = NextObject(&i)) != NULL) {
                PortLog("[tap] %s type 0x%02X at %d,%d (cell %d,%d) hp %d ability %d rect %d,%d,%d,%d", IsEnemy(ob) ? "enemy" : IsItem(ob) ? "item" : "object",
                            ob->type, (int)(ob->base.x >> 8), (int)(ob->base.y >> 8), (int)(ob->base.x >> 12),
                            (int)(ob->base.y >> 12), ob->unk80, ob->kirbyAbility, ob->base.unk3C, ob->base.unk3D,
                            ob->base.unk3E, ob->base.unk3F);
            }
        }
    } else if (strcmp(cmd, "tapkirby") == 0) {
        PortTapWorld(PORT_UI_DOWN, kx, ky, 1);
        PortTapWorld(PORT_UI_UP, kx, ky, 1);
    } else if (strcmp(cmd, "swipekirby") == 0) {
        PortTapWorld(PORT_UI_DOWN, kx, ky, 1);
        PortTapWorld(PORT_UI_MOVE, kx, ky + 40, 1);
        PortTapWorld(PORT_UI_UP, kx, ky + 40, 1);
    } else if (strcmp(cmd, "downkirby") == 0) {
        PortTapWorld(PORT_UI_DOWN, kx, ky, 1);
    } else if (strcmp(cmd, "upkirby") == 0) {
        PortTapWorld(PORT_UI_UP, sP.on ? sP.x : kx, sP.on ? sP.y : ky, 1);
    } else if (strcmp(cmd, "swipeupkirby") == 0) {
        PortTapWorld(PORT_UI_DOWN, kx, ky, 1);
        PortTapWorld(PORT_UI_MOVE, kx, ky - 40, 1);
        PortTapWorld(PORT_UI_UP, kx, ky - 40, 1);
    } else if (strcmp(cmd, "down2") == 0) {
        PortTapWorld(PORT_UI_DOWN, kx + 40, ky, 2);
    } else if (strcmp(cmd, "up2") == 0) {
        PortTapWorld(PORT_UI_UP, kx + 40, ky, 2);
    } else if (strncmp(cmd, "kickkirby ", 10) == 0) {
        int dir = cmd[10] == 'L' ? -1 : 1;

        PortTapWorld(PORT_UI_DOWN, kx, ky, 1);
        PortTapWorld(PORT_UI_MOVE, kx + dir * 30, ky + 30, 1);
        PortTapWorld(PORT_UI_UP, kx + dir * 30, ky + 30, 1);
    } else if (strcmp(cmd, "tappush") == 0) {
        struct Object *best = NULL, *ob;
        s32 bd = 0x7FFFFFFF;
        int i = 0;

        while ((ob = NextObject(&i)) != NULL) {
            s32 dx = (ob->base.x >> 8) - kx, dy = (ob->base.y >> 8) - ky, d = dx * dx + dy * dy;

            if (IsPushable(ob) && d < bd) {
                bd = d;
                best = ob;
            }
        }
        if (best == NULL) {
            PortLog("[tap] f%u tappush: no block in the room", (unsigned)PortFrameNumber());
            return 1;
        }
        {
            s32 x, y, w, h;

            Box(best, &x, &y, &w, &h);
            PortTapWorld(PORT_UI_DOWN, x + w / 2, y + h / 2, 1);
            PortTapWorld(PORT_UI_UP, x + w / 2, y + h / 2, 1);
        }
    } else if (strcmp(cmd, "tapswitch") == 0) {
        /* a tap on the nearest switch in the room */
        struct Object *best = NULL, *ob;
        s32 bd = 0x7FFFFFFF;
        int i = 0;

        while ((ob = NextObject(&i)) != NULL) {
            s32 dx = (ob->base.x >> 8) - kx, dy = (ob->base.y >> 8) - ky, d = dx * dx + dy * dy;

            if (IsSwitch(ob) && d < bd) {
                bd = d;
                best = ob;
            }
        }
        if (best == NULL) {
            PortLog("[tap] f%u tapswitch: no switch in the room", (unsigned)PortFrameNumber());
            return 1;
        }
        PortTapWorld(PORT_UI_DOWN, best->base.x >> 8, (best->base.y >> 8) - 6, 1);
        PortTapWorld(PORT_UI_UP, best->base.x >> 8, (best->base.y >> 8) - 6, 1);
    } else if (strcmp(cmd, "tapenemy") == 0) {
        struct Object *best = NULL, *ob;
        s32 bd = 0x7FFFFFFF;
        int i = 0;

        while ((ob = NextObject(&i)) != NULL) {
            s32 dx = (ob->base.x >> 8) - kx, dy = (ob->base.y >> 8) - ky, d = dx * dx + dy * dy;

            if (IsEnemy(ob) && d < bd) {
                bd = d;
                best = ob;
            }
        }
        if (best == NULL) {
            PortLog("[tap] f%u tapenemy: no enemy in the room", (unsigned)PortFrameNumber());
            return 1;
        }
        PortTapWorld(PORT_UI_DOWN, best->base.x >> 8, best->base.y >> 8, 1);
        PortTapWorld(PORT_UI_UP, best->base.x >> 8, best->base.y >> 8, 1);
    } else {
        return 0;
    }
    return 1;
}

/* --- presentation -------------------------------------------------------------- */

void PortTapSetView(int ok, int camX, int camY, int viewX, int viewY, int viewW, int viewH)
{
    sV.ok = ok && viewW > 0 && viewH > 0;
    sV.camX = camX;
    sV.camY = camY;
    sV.viewX = viewX;
    sV.viewY = viewY;
    sV.viewW = viewW;
    sV.viewH = viewH;
}

void PortTapSetWorldRect(const int world[4], float dpr)
{
    memcpy(sV.world, world, sizeof(sV.world));
    sV.dpr = dpr > 0.0f ? dpr : 1.0f;
}

/* The pictures: premultiplied RGBA, R in the low byte, in world pixels
 * (drawn at the picture's own scale, so they sit in the game's pixel grid). */
#define MK_W 24
static u32 sMk[3][MK_W * MK_W];
static int sMkW[3], sMkH[3], sMkKey[3] = { -1, -1, -1 };

static u32 Rgba(int r, int g, int b, int a)
{
    return (u32)(r * a / 255) | ((u32)(g * a / 255) << 8) | ((u32)(b * a / 255) << 16) | ((u32)a << 24);
}

static void Ring(u32 *px, int w, float cx, float cy, float r0, float r1, u32 ink)
{
    int x, y;

    for (y = 0; y < w; y++)
        for (x = 0; x < w; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);

            if (d >= r0 && d <= r1)
                px[y * w + x] = ink;
        }
}

/* 0: the target -- a ring that breathes (yellow; an arrow over it for a
 * door; red when given up), 1: the reticle (red, four ticks), 2: a path dot. */
static void Picture(int which, int phase, int style)
{
    int key = which * 1000 + phase * 4 + style;
    u32 *px = sMk[which];
    u32 ink, edge = Rgba(0, 0, 0, 190);

    if (sMkKey[which] == key)
        return;
    sMkKey[which] = key;
    memset(px, 0, sizeof(sMk[which]));
    if (which == 0) {
        float r = 6.0f + (float)phase * 0.75f;
        int i;

        sMkW[0] = sMkH[0] = MK_W;
        ink = style == 2 ? Rgba(255, 80, 80, 255) : Rgba(255, 236, 96, 255);
        Ring(px, MK_W, 12, 12, r - 1.0f, r + 2.6f, edge);
        Ring(px, MK_W, 12, 12, r, r + 1.6f, ink);
        Ring(px, MK_W, 12, 12, 0, 2.6f, edge);
        Ring(px, MK_W, 12, 12, 0, 1.8f, ink);
        if (style == 1)                         /* a door: an arrow, up */
            for (i = 0; i < 4; i++) {
                int y = 2 + i, x0 = 12 - i, x1 = 11 + i, x;

                for (x = x0 - 1; x <= x1 + 1; x++)
                    if (px[y * MK_W + x] != ink)
                        px[y * MK_W + x] = edge;
                for (x = x0; x <= x1; x++)
                    px[y * MK_W + x] = ink;
            }
    } else if (which == 1) {
        int i;

        sMkW[1] = sMkH[1] = MK_W;
        ink = Rgba(255, 64, 88, 255);
        Ring(px, MK_W, 12, 12, 7.0f, 10.2f, edge);
        Ring(px, MK_W, 12, 12, 7.6f, 9.4f, ink);
        for (i = 0; i < 5; i++) {
            px[(1 + i) * MK_W + 11] = px[(1 + i) * MK_W + 12] = ink;
            px[(18 + i) * MK_W + 11] = px[(18 + i) * MK_W + 12] = ink;
            px[11 * MK_W + 1 + i] = px[12 * MK_W + 1 + i] = ink;
            px[11 * MK_W + 18 + i] = px[12 * MK_W + 18 + i] = ink;
        }
    } else {
        sMkW[2] = sMkH[2] = 4;
        ink = Rgba(255, 255, 255, 210);
        px[1] = px[2] = px[4] = px[7] = px[8] = px[11] = px[13] = px[14] = Rgba(0, 0, 0, 120);
        px[5] = px[6] = px[9] = px[10] = ink;
    }
}

void PortTapHudDraw(int fbW, int fbH, const int world[4], int smooth)
{
    double kx, ky, scale;
    int i, red = 0, uploaded[3] = { 0, 0, 0 };
    s32 tx, ty;

    PortTapSetWorldRect(world, sV.dpr);
    if (!PortTapEnabled() || !sV.ok || !InPlay() || world[2] <= 0 || world[3] <= 0)
        return;
    if (sT.kind == T_NONE && sT.fail <= 0)
        return;
    kx = (double)world[2] / sV.viewW;
    ky = (double)world[3] / sV.viewH;
    scale = kx;
#define FBX(wx) (world[0] + ((wx) - sV.camX - sV.viewX) * kx)
#define FBY(wy) (world[1] + ((wy) - sV.camY - sV.viewY) * ky)
    /* the path: a dot every other cell */
    if (sT.kind != T_NONE && sGoal >= 0 && sW > 0) {
        int prev = sMkKey[2];

        Picture(2, 0, 0);
        uploaded[2] = prev != sMkKey[2];
        for (i = 1; i < sPathN; i += 2) {
            int c = sPath[i];
            double x = FBX(c % sW * 16 + 8) - 2 * scale, y = FBY(c / sW * 16 + 8) - 2 * scale;

            if (x < world[0] || y < world[1] || x > world[0] + world[2] || y > world[1] + world[3])
                continue;
            GlrDrawOverlay(GLR_OVERLAY_TAP(2), sMk[2], sMkW[2], sMkH[2], !uploaded[2] ? 1 : 0, fbW, fbH, x, y,
                           scale, smooth);
            uploaded[2] = 1;
        }
    }
    if (sT.kind == T_ENEMY) {
        int prev = sMkKey[1];

        Picture(1, 0, 0);
        tx = sT.obj->base.x >> 8;
        ty = sT.obj->base.y >> 8;
        GlrDrawOverlay(GLR_OVERLAY_TAP(1), sMk[1], sMkW[1], sMkH[1], prev != sMkKey[1], fbW, fbH,
                       FBX(tx) - 12 * scale, FBY(ty) - 12 * scale, scale, smooth);
        return;
    }
    if (sT.kind == T_NONE) {
        red = 2;
        tx = sT.failX;
        ty = sT.failY;
    } else {
        red = sT.kind == T_DOOR;
        int onThing = sT.kind == T_SWITCH || sT.kind == T_BLOCK || sT.kind == T_PUSH || sT.chest;

        tx = onThing ? sT.mx : sT.x;
        ty = onThing ? sT.my : sT.kind == T_DOOR ? sT.cy * 16 + 4 : sT.air ? sT.y : sT.cy * 16 + 10;
    }
    {
        int prev = sMkKey[0], phase = (int)((sTick / 6) % 4);

        Picture(0, phase < 2 ? phase : 4 - phase, red);
        GlrDrawOverlay(GLR_OVERLAY_TAP(0), sMk[0], sMkW[0], sMkH[0], prev != sMkKey[0], fbW, fbH,
                       FBX(tx) - 12 * scale, FBY(ty) - 12 * scale, scale, smooth);
    }
#undef FBX
#undef FBY
}

/* --- the page's side ---------------------------------------------------------- */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE void PortTapPointerJs(int kind, float x, float y, int id) { PortTapPointer(kind, x, y, id); }
EMSCRIPTEN_KEEPALIVE int PortTapActiveJs(void) { return PortTapActive(); }
EMSCRIPTEN_KEEPALIVE void PortTapForceJs(int on) { PortTapForce(on); }
/* Tests: the target now (k 0 kind, 1 x, 2 y, 3 air, 4 arrived frames,
 * 5 Kirby x, 6 Kirby y, 7 room). */
EMSCRIPTEN_KEEPALIVE int PortTapStateJs(int k)
{
    struct Kirby *me = Me();

    switch (k) {
    case 0: return sT.kind;
    case 1: return sT.x;
    case 2: return sT.y;
    case 3: return sT.air;
    case 4: return sT.arrived;
    case 5: return me->base.x >> 8;
    case 6: return me->base.y >> 8;
    case 7: return Li()->currentRoom;
    default: return 0;
    }
}
/* Tests: world pixel -> framebuffer pixel of the last picture (k 0 x, 1 y). */
EMSCRIPTEN_KEEPALIVE int PortTapWorldToFbJs(int wx, int wy, int k)
{
    if (!sV.ok || sV.viewW <= 0 || sV.viewH <= 0)
        return -1;
    if (k == 0)
        return (int)(sV.world[0] + (wx - sV.camX - sV.viewX + 0.5) * sV.world[2] / sV.viewW);
    return (int)(sV.world[1] + (wy - sV.camY - sV.viewY + 0.5) * sV.world[3] / sV.viewH);
}
#endif

#ifdef __cplusplus
}
#endif
