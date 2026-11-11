/* Online co-op: the live state of a room the players share -- the blocks
 * broken in it, the buttons held down, the ability stars dropped.  The API
 * is in platform/port/coop.h; this is the engine half.
 *
 * --- blocks ------------------------------------------------------------------
 *
 * Star blocks and their kin are tiles: an attack, an inhale or a chain
 * reaction rewrites the room's tile map (sub_08001408 with the replacement
 * sub_080025AC finds for the tile, object_collision.c / code_0800E0E4.c).
 * Every break a game makes is reported (PortCoopTileBroken) as room and tile;
 * every other game in that room makes the same break if the tile is still
 * breakable there (attribute 0x1000, or 0x40 for a chain) -- the condition
 * the game itself breaks on, and the one that guarantees sub_080025AC has an
 * entry for the tile (it searches without an end otherwise).  A game also
 * keeps the breaks of the room it is in, and the page sends them to a player
 * who walks in (PortCoopTilesSnapshot): the room as the others have it, not
 * as it loads.  Tiles an object manages (crumbling rock, pushed stone blocks)
 * are left alone: the object would rewrite them again from a state that is
 * no longer there.
 *
 * --- buttons -------------------------------------------------------------------
 *
 * A button is held by a Kirby standing on it, and puppets stand on nothing
 * (they are out of the solid-object lists).  PortCoopPuppetOn lets a puppet
 * standing on a button hold it down in this game too (big_small_switch.c):
 * whatever the button opens opens here as well, while the player is on it.
 *
 * --- ability stars -------------------------------------------------------------
 *
 * The star a Kirby drops when it is hurt or lets its ability go is a real
 * object in the game that dropped it and was only a ghost everywhere else, so
 * nobody else could take it.  Now the dropping game reports it
 * (PortCoopStarDropped), every other game in the room makes a star of its own
 * at the same place, moving the same way, next to the puppet of whoever
 * dropped it; and whoever swallows one (PortCoopStarSwallowed) has the others
 * removed.  Ability stars are no longer captured as ghosts (coop_fx.c).
 *
 * Not simulation state: co-op is refused under rollback, the lockstep session
 * and seats; what arrives here is external input, like keys. */

#include <string.h>

#include "global.h"
#include "data.h"
#include "functions.h"
#include "task.h"
#include "kirby.h"
#include "object.h"
#include "code_0806F780.h"
#include "constants/kirby.h"
#include "constants/object_types.h"
#include "constants/songs.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/coop.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define COOP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define COOP_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

u8 gPortCoopRoomOn;

#define TILE_Q        64       /* breaks waiting for the page               */
#define TILE_SET      256      /* breaks kept for the room the player is in */
#define SNAP_WAIT     180      /* frames a snapshot waits for its room      */
#define STAR_MAX      16
#define STAR_Q        16

static u8  sShare;
static u8  sApplying;           /* breaking or spawning for another game     */
static struct { u16 room; u8 x, y, kind; } sTileQ[TILE_Q];
static int sTileQN;
static struct { u8 x, y; } sTileSet[TILE_SET];
static int sTileSetN;
static u16 sTileRoom = 0xFFFF;
static u8  sSnap[4 + 2 * TILE_SET];
static int sSnapLen;
static u32 sSnapAt;
static struct ObjectBase sTileBase;  /* the break effect's parent, already gone */

struct Star {
    u8  used, src, id;
    struct Object *obj;
    int task;
};
static struct Star sStar[STAR_MAX];
static u8  sStarId;
static u8  sStarQ[STAR_Q][12];
static int sStarQN;

/* tiles: broken here, sent, applied, refused (not breakable here), queued
 * snapshot breaks applied; buttons held by puppets; stars: dropped here,
 * made for others, swallowed (sent), removed (taken elsewhere); buttons hit
 * here (sent), held for another game's hit */
static u32 sStat[12];

static void Put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u32  Get16(const u8 *p)  { return (u32)p[0] | ((u32)p[1] << 8); }

static int Ready(void)
{
    return sShare && PortCoopReady();
}

static u16 LocalRoom(void)
{
    return gKirbys[gLocalPlayerId].base.roomId;
}

/* --- blocks ------------------------------------------------------------------ */

static void SetAdd(u32 x, u32 y)
{
    int i;

    for (i = 0; i < sTileSetN; i++)
        if (sTileSet[i].x == x && sTileSet[i].y == y)
            return;
    if (sTileSetN < TILE_SET) {
        sTileSet[sTileSetN].x = (u8)x;
        sTileSet[sTileSetN].y = (u8)y;
        sTileSetN++;
    }
}

void PortCoopTileBroken(u32 playerId, u32 x, u32 y, u32 kind)
{
    u32 room;

    if (!sShare || sApplying || playerId >= 4 || PORT_COOP_IS_PUPPET(playerId))
        return;
    room = gCurLevelInfo[playerId].currentRoom;
    if (room == 0xFFFF)
        return;
    if (room == LocalRoom())
        SetAdd(x, y);
    if (sTileQN < TILE_Q) {
        sTileQ[sTileQN].room = (u16)room;
        sTileQ[sTileQN].x = (u8)x;
        sTileQ[sTileQN].y = (u8)y;
        sTileQ[sTileQN].kind = (u8)kind;
        sTileQN++;
        sStat[0]++;
    }
}

/* Breaks (x, y) in the local room the way the game does, if it is still
 * breakable here.  kind: 0 with the dust and the sound, 1 the sound only,
 * 2 silently (a snapshot). */
static int Break(u32 x, u32 y, u32 kind)
{
    u8 p = gLocalPlayerId;
    const struct LevelInfo *li = &gCurLevelInfo[p];
    u32 attr;
    struct Task *cur;

    if (x >= (u32)(li->roomWidth >> 4) || y >= (u32)(li->roomHeight >> 4))
        return 0;
    attr = gCollisionAttributes[GetCollisionTile(p, (u16)x, (u16)y)];
    if (!(attr & (0x1000 | 0x40))) {
        sStat[3]++;
        return 0;
    }
    sApplying = 1;
    cur = gCurTask;
    gCurTask = NULL;                /* nothing made here is anyone's spawn */
    sub_08001408(p, sub_080025AC(p, (u8)x, (u8)y), NULL, NULL);
    memset(&sTileBase, 0, sizeof(sTileBase));
    sTileBase.header.kind = 2;
    sTileBase.flags = 0x1000;       /* the effect lets go of it at once */
    sTileBase.roomId = LocalRoom();
    sTileBase.unk56 = p;
    sTileBase.x = (s32)(x * 0x1000 + 0x800);
    sTileBase.y = (s32)(y * 0x1000 + 0x800);
    if (kind == 0) {
        struct EffectObject *e = CreateEffectObject(&sTileBase, 0, 0x28D, 0);

        e->x = sTileBase.x;
        e->y = sTileBase.y;
    }
    if (kind != 2)
        PlaySfx(&sTileBase, SE_BLOCK_BREAK);
    gCurTask = cur;
    sApplying = 0;
    SetAdd(x, y);
    return 1;
}

/* Breaks made elsewhere: n records of 5 bytes (u16 room, u8 x, u8 y, u8 kind). */
COOP_EXPORT int PortCoopTilesApply(const unsigned char *buf, int len)
{
    int i, done = 0;

    if (buf == NULL || !Ready())
        return 0;
    for (i = 0; i + 5 <= len; i += 5) {
        if (Get16(buf + i) != LocalRoom())
            continue;
        if (Break(buf[i + 2], buf[i + 3], buf[i + 4] > 2 ? 0 : buf[i + 4])) {
            done++;
            sStat[2]++;
        }
    }
    return done;
}

COOP_EXPORT int PortCoopTilesTake(unsigned char *out, int max)
{
    int i, n = sTileQN < max / 5 ? sTileQN : max / 5;

    if (out == NULL)
        return 0;
    for (i = 0; i < n; i++) {
        Put16(out + i * 5, sTileQ[i].room);
        out[i * 5 + 2] = sTileQ[i].x;
        out[i * 5 + 3] = sTileQ[i].y;
        out[i * 5 + 4] = sTileQ[i].kind;
    }
    memmove(sTileQ, sTileQ + n, (size_t)(sTileQN - n) * sizeof(sTileQ[0]));
    sTileQN -= n;
    sStat[1] += (u32)n;
    return n * 5;
}

/* The breaks of the room the local player is in: u16 room, u16 n, n x
 * (u8 x, u8 y).  Returns the length, 0 when there are none. */
COOP_EXPORT int PortCoopTilesSnapshot(unsigned char *out, int max)
{
    int i, n;

    if (out == NULL || !Ready() || sTileSetN == 0 || sTileRoom != LocalRoom() || max < 6)
        return 0;
    n = sTileSetN < (max - 4) / 2 ? sTileSetN : (max - 4) / 2;
    Put16(out, sTileRoom);
    Put16(out + 2, (u32)n);
    for (i = 0; i < n; i++) {
        out[4 + i * 2] = sTileSet[i].x;
        out[4 + i * 2 + 1] = sTileSet[i].y;
    }
    return 4 + n * 2;
}

/* Another player's breaks of a room: applied as soon as the local player is
 * in that room (a snapshot often arrives while the room is still loading). */
COOP_EXPORT void PortCoopTilesSetSnapshot(const unsigned char *buf, int len)
{
    if (buf == NULL || len < 4 || len > (int)sizeof(sSnap) || 4 + 2 * (int)Get16(buf + 2) > len)
        return;
    memcpy(sSnap, buf, (size_t)len);
    sSnapLen = len;
    sSnapAt = PortFrameNumber();
}

static void SnapFrame(void)
{
    u32 i, n;

    if (sSnapLen == 0)
        return;
    if (PortFrameNumber() - sSnapAt > SNAP_WAIT) {
        sSnapLen = 0;
        return;
    }
    if (Get16(sSnap) != LocalRoom() || sTileRoom != LocalRoom())
        return;
    n = Get16(sSnap + 2);
    for (i = 0; i < n; i++)
        if (Break(sSnap[4 + i * 2], sSnap[5 + i * 2], 2))
            sStat[4]++;
    sSnapLen = 0;
}

/* --- buttons ------------------------------------------------------------------- */

/* A puppet standing on (or in) the rectangle l,t..r,b of `room`, pixels. */
int PortCoopPuppetOn(u32 room, s32 l, s32 t, s32 r, s32 b)
{
    u32 i;

    if (!gPortCoopRoomOn || !gPortCoopPuppets)
        return 0;
    for (i = 1; i < PORT_COOP_MAX_SLOTS && i < gNumKirbys; i++) {
        const struct Kirby *k = &gKirbys[i];
        s32 kl, kt, kr, kb;

        if (!PORT_COOP_IS_PUPPET(i) || k->base.roomId != room || (k->base.flags & 0x400))
            continue;
        kl = (k->base.x >> 8) + k->base.unk38;
        kt = (k->base.y >> 8) + k->base.unk39;
        kr = (k->base.x >> 8) + k->base.unk3A;
        kb = (k->base.y >> 8) + k->base.unk3B;
        /* standing on it: the feet touch the top, so reach 3 px further down */
        if (kl <= r && kr >= l && kt <= b && kb + 3 >= t) {
            sStat[5]++;
            return 1;
        }
    }
    return 0;
}

/* A button can also be pressed by hitting it (an attack sets its 0x40000):
 * the hit is reported (PortCoopButtonHit, once per half second per button)
 * and every other game in the room holds the same button down for a few
 * frames (PortCoopButtonHeld) -- long enough for its own timer to take over,
 * as a hit there would have. */
#define BTN_MAX   8
#define BTN_HOLD  4
#define BTN_AGAIN 30
static struct { u16 room, idx; } sBtnQ[BTN_MAX];
static int sBtnQN;
static struct { u16 room, idx; u32 at; u8 used; } sBtnSent[BTN_MAX], sBtnHeld[BTN_MAX];

void PortCoopButtonHit(const struct Object *sw)
{
    int idx, i, free = -1;
    u32 now = PortFrameNumber();

    if (!sShare || sApplying || (idx = PortCoopObjectIndex(sw)) < 0)
        return;
    for (i = 0; i < BTN_MAX; i++) {
        if (sBtnSent[i].used && sBtnSent[i].room == sw->base.roomId && sBtnSent[i].idx == (u16)idx) {
            if (now - sBtnSent[i].at < BTN_AGAIN)
                return;
            free = i;
            break;
        }
        if (free < 0 && (!sBtnSent[i].used || now - sBtnSent[i].at >= BTN_AGAIN))
            free = i;
    }
    if (free < 0 || sBtnQN >= BTN_MAX)
        return;
    sBtnSent[free].used = 1;
    sBtnSent[free].room = sw->base.roomId;
    sBtnSent[free].idx = (u16)idx;
    sBtnSent[free].at = now;
    sBtnQ[sBtnQN].room = sw->base.roomId;
    sBtnQ[sBtnQN].idx = (u16)idx;
    sBtnQN++;
    sStat[10]++;
}

int PortCoopButtonHeld(const struct Object *sw)
{
    int idx, i;
    u32 now = PortFrameNumber();

    if (!sShare || (idx = PortCoopObjectIndex(sw)) < 0)
        return 0;
    for (i = 0; i < BTN_MAX; i++)
        if (sBtnHeld[i].used && sBtnHeld[i].room == sw->base.roomId && sBtnHeld[i].idx == (u16)idx) {
            if (now - sBtnHeld[i].at <= BTN_HOLD)
                return 1;
            sBtnHeld[i].used = 0;
        }
    return 0;
}

/* Buttons hit here: n records of 4 bytes (u16 room, u16 index). */
COOP_EXPORT int PortCoopButtonsTake(unsigned char *out, int max)
{
    int i, n = sBtnQN < max / 4 ? sBtnQN : max / 4;

    if (out == NULL)
        return 0;
    for (i = 0; i < n; i++) {
        Put16(out + i * 4, sBtnQ[i].room);
        Put16(out + i * 4 + 2, sBtnQ[i].idx);
    }
    sBtnQN = 0;
    return n * 4;
}

COOP_EXPORT void PortCoopButtonsApply(const unsigned char *buf, int len)
{
    int i, k, slot;

    if (buf == NULL || !Ready())
        return;
    for (i = 0; i + 4 <= len; i += 4) {
        if (Get16(buf + i) != LocalRoom())
            continue;
        slot = -1;
        for (k = 0; k < BTN_MAX; k++) {
            if (sBtnHeld[k].used && sBtnHeld[k].room == Get16(buf + i) && sBtnHeld[k].idx == Get16(buf + i + 2)) {
                slot = k;
                break;
            }
            if (slot < 0 && !sBtnHeld[k].used)
                slot = k;
        }
        if (slot < 0)
            continue;
        sBtnHeld[slot].used = 1;
        sBtnHeld[slot].room = (u16)Get16(buf + i);
        sBtnHeld[slot].idx = (u16)Get16(buf + i + 2);
        sBtnHeld[slot].at = PortFrameNumber();
        sStat[11]++;
    }
}

/* --- ability stars --------------------------------------------------------------- */

static int TaskOf(const void *p)
{
    int i;

    for (i = 0; i < MAX_TASK_NUM; i++) {
        struct Task *t = &gTasks[i];

        if (t->prev == 0 || t->structOffset == 0)
            continue;
        if (TaskGetStructPtr(t) == p)
            return i;
    }
    return -1;
}

static int IsStar(const struct Object *o)
{
    return o->base.header.kind == 1 && (o->type == OBJ_ABILITY_STAR_1 || o->type == OBJ_ABILITY_STAR_2);
}

/* The tracked star is still the object it was. */
static int StarAlive(const struct Star *s)
{
    struct Task *t;

    if (s->task < 0 || s->task >= MAX_TASK_NUM)
        return 0;
    t = &gTasks[s->task];
    return t->prev != 0 && t->structOffset != 0 && TaskGetStructPtr(t) == s->obj
        && IsStar(s->obj) && !(s->obj->base.flags & 0x1000);
}

static struct Star *StarFind(u32 src, u32 id)
{
    int i;

    for (i = 0; i < STAR_MAX; i++)
        if (sStar[i].used && sStar[i].src == src && sStar[i].id == id)
            return &sStar[i];
    return NULL;
}

static struct Star *StarAdd(u32 src, u32 id, struct Object *obj)
{
    int i;

    for (i = 0; i < STAR_MAX; i++)
        if (!sStar[i].used || !StarAlive(&sStar[i]))
            break;
    if (i == STAR_MAX)
        return NULL;
    sStar[i].used = 1;
    sStar[i].src = (u8)src;
    sStar[i].id = (u8)id;
    sStar[i].obj = obj;
    sStar[i].task = TaskOf(obj);
    return &sStar[i];
}

static void StarQueue(u32 kind, u32 id, u32 src, const struct Object *o, u32 ability, u32 flags, u32 slot)
{
    u8 *r;

    if (sStarQN >= STAR_Q)
        return;
    r = sStarQ[sStarQN++];
    memset(r, 0, 12);
    r[0] = (u8)kind;
    r[1] = (u8)id;
    r[2] = (u8)src;
    if (o != NULL) {
        r[3] = (u8)ability;
        Put16(r + 4, o->base.roomId);
        Put16(r + 6, (u32)(u16)(s16)(o->base.x >> 8));
        Put16(r + 8, (u32)(u16)(s16)(o->base.y >> 8));
        r[10] = o->type;
    }
    r[11] = (u8)(flags | (slot << 4));
}

/* sub_080A9038 (inhalable_star.c) made `star` for `kirby`. */
void PortCoopStarDropped(struct Kirby *kirby, struct Object *star)
{
    u32 k = kirby->base.unk56, flags;

    if (!sShare || sApplying || star == NULL || k >= 4 || PORT_COOP_IS_PUPPET(k))
        return;
    sStarId++;
    if (StarAdd(0xFF, sStarId, star) == NULL)
        return;
    flags = (star->base.flags & 1) | ((u32)(star->unk9E & 3) << 1);
    StarQueue(1, sStarId, 0xFF, star, kirby->ability, flags, k);
    sStat[6]++;
}

/* A star is being swallowed here (object.c): the others' copies go. */
void PortCoopStarSwallowed(struct Object *star)
{
    int i;

    if (!sShare)
        return;
    for (i = 0; i < STAR_MAX; i++) {
        struct Star *s = &sStar[i];

        if (s->used && s->obj == star) {
            StarQueue(2, s->id, s->src, NULL, 0, 0, 0);
            s->used = 0;
            sStat[8]++;
            return;
        }
    }
}

/* What happened to stars here: n records of 12 bytes -- u8 kind (1 dropped,
 * 2 swallowed), u8 id, u8 src (0xFF: this game's own star, else the source
 * the page gave it), u8 ability, u16 room, s16 x, s16 y, u8 type, u8 flags
 * (bit 0 facing left, bits 1-2 its bounce, bits 4-5 the sender's Kirby slot). */
COOP_EXPORT int PortCoopStarTake(unsigned char *out, int max)
{
    int n = sStarQN < max ? sStarQN : max;

    if (out == NULL)
        return 0;
    memcpy(out, sStarQ, (size_t)n * 12);
    memmove(sStarQ, sStarQ + n, (size_t)(sStarQN - n) * 12);
    sStarQN -= n;
    return n;
}

/* A star another game dropped (src: the page's source for that player, id:
 * theirs), next to `slot` (the puppet showing whoever dropped it; 0 when
 * none).  Made only when the local player is in that room. */
COOP_EXPORT int PortCoopStarSpawn(int src, int id, int room, int x, int y, int ability, int type, int flags, int slot)
{
    struct Object *star;
    struct Kirby *parent;
    struct Task *cur;

    if (!Ready() || src < 0 || src >= 0xFF || (u32)room != LocalRoom())
        return 0;
    if ((type != OBJ_ABILITY_STAR_1 && type != OBJ_ABILITY_STAR_2) || ability <= 0 || ability >= KIRBY_ABILITY_COUNT)
        return 0;
    if (StarFind((u32)src, (u32)id) != NULL)
        return 0;
    parent = &gKirbys[slot >= 1 && slot < PORT_COOP_MAX_SLOTS && PORT_COOP_IS_PUPPET(slot) ? slot : gLocalPlayerId];
    sApplying = 1;
    cur = gCurTask;
    gCurTask = NULL;
    star = CreateObjTemplateAndObj(gLocalPlayerId, 1, 0x24, x, y, 0, 0x1F, 0, 0, (u8)type,
        (u8)ability, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    star->base.parent = parent;
    star->unk9E = (u8)((flags >> 1) & 3);
    if (flags & 1)
        star->base.flags |= 1;
    if (parent != &gKirbys[gLocalPlayerId])
        CreateEffectObject(&parent->base, 0, 0x2A9, 0);
    gCurTask = cur;
    sApplying = 0;
    StarAdd((u32)src, (u32)id, star);
    sStat[7]++;
    return 1;
}

/* Someone swallowed star (src, id) (src 0xFF: one this game dropped). */
COOP_EXPORT void PortCoopStarTaken(int src, int id)
{
    struct Star *s = StarFind((u32)src & 0xFF, (u32)id & 0xFF);

    if (s == NULL)
        return;
    if (StarAlive(s)) {
        s->obj->base.flags |= 0x1000;
        sStat[9]++;
    }
    s->used = 0;
}

/* A source the page gave to someone else now: their stars are just stars. */
COOP_EXPORT void PortCoopRoomForget(int src)
{
    int i;

    for (i = 0; i < STAR_MAX; i++)
        if (sStar[i].used && sStar[i].src == (u32)src)
            sStar[i].used = 0;
}

/* --- per frame, from PortCoopFrameTop --------------------------------------------- */

void PortCoopRoomFrame(void)
{
    int i;

    gPortCoopRoomOn = sShare;
    if (!Ready())
        return;
    if (LocalRoom() != sTileRoom) {
        sTileRoom = LocalRoom();
        sTileSetN = 0;
    }
    SnapFrame();
    for (i = 0; i < STAR_MAX; i++)
        if (sStar[i].used && !StarAlive(&sStar[i]))
            sStar[i].used = 0;
}

COOP_EXPORT void PortCoopRoomShare(int on)
{
    sShare = on ? 1 : 0;
    gPortCoopRoomOn = sShare;
    if (!sShare) {
        sTileQN = 0;
        sStarQN = 0;
        sBtnQN = 0;
        memset(sBtnHeld, 0, sizeof(sBtnHeld));
        sSnapLen = 0;
        memset(sStar, 0, sizeof(sStar));
    }
}

/* --- tests and diagnostics ------------------------------------------------------ */

/* Breakable tiles (attribute 0x1000 or 0x40) of the local room: n x {u8 x, u8 y}. */
COOP_EXPORT int PortCoopTestTiles(unsigned char *out, int max)
{
    u8 p = gLocalPlayerId;
    const struct LevelInfo *li = &gCurLevelInfo[p];
    u32 x, y, w, h;
    int n = 0;

    if (out == NULL || gUnk_02023354 == NULL || li->currentRoom == 0xFFFF)
        return 0;
    w = li->roomWidth >> 4;
    h = li->roomHeight >> 4;
    for (y = 0; y < h; y++)
        for (x = 0; x < w && n < max; x++)
            if (gCollisionAttributes[GetCollisionTile(p, (u16)x, (u16)y)] & (0x1000 | 0x40)) {
                out[n * 2] = (u8)x;
                out[n * 2 + 1] = (u8)y;
                n++;
            }
    return n;
}

struct TestList { u32 type; u8 *out; int max, n; };

static int TestOne(struct Object *o, void *arg)
{
    struct TestList *l = (struct TestList *)arg;
    u8 *r;

    if (l->n >= l->max)
        return 1;
    if (o->type != l->type || o->base.roomId != LocalRoom() || (o->base.flags & 0x1000))
        return 0;
    r = l->out + l->n * 16;
    memset(r, 0, 16);
    r[0] = (u8)(uintptr_t)o; r[1] = (u8)((uintptr_t)o >> 8);
    r[2] = (u8)((uintptr_t)o >> 16); r[3] = (u8)((uintptr_t)o >> 24);
    Put16(r + 4, (u32)(u16)(s16)(o->base.x >> 8));
    Put16(r + 6, (u32)(u16)(s16)(o->base.y >> 8));
    r[8] = o->unk83;
    r[9] = o->kirbyAbility;
    r[10] = (u8)PortCoopObjectIndex(o);
    l->n++;
    return 0;
}

/* Live objects of `type` in the local room: n x 16 bytes {u32 address,
 * s16 x, s16 y (pixels), u8 unk83, u8 kirbyAbility, u8 list index}. */
COOP_EXPORT int PortCoopTestObjects(int type, unsigned char *out, int max)
{
    struct TestList l;

    if (out == NULL || gUnk_02023354 == NULL)
        return 0;
    l.type = (u32)type;
    l.out = out;
    l.max = max;
    l.n = 0;
    PortCoopForEachObject(TestOne, &l);
    return l.n;
}

/* As if the local Kirby swallowed the star at `addr` (the hook in object.c). */
COOP_EXPORT void PortCoopTestSwallow(unsigned int addr)
{
    if (addr != 0)
        PortCoopStarSwallowed((struct Object *)(uintptr_t)addr);
}

COOP_EXPORT int PortCoopRoomStat(int i)
{
    return i >= 0 && i < (int)(sizeof(sStat) / sizeof(sStat[0])) ? (int)sStat[i] : 0;
}

#ifdef __cplusplus
}
#endif
