/* Online co-op: what spawns -- projectiles, ability effects, particles.  The
 * API is in platform/port/coop.h; this is the engine half.
 *
 * --- the problem ------------------------------------------------------------
 *
 * A puppet (a remote player's Kirby, coop.c) and a puppeted enemy
 * (coop_enemy.c) only *draw*: their state machines do not run here, so
 * nothing they would have spawned exists in this game.  A remote player's
 * beam, fire, cutter, star spit and dust, and a shared enemy's shots, were
 * invisible.
 *
 * --- the model ----------------------------------------------------------------
 *
 * Visual replication at the sprite level.  The game that runs the spawner
 * reports what those spawned objects *draw*; every other game draws copies
 * ("ghosts") through the game's own sprite code.  Nothing is simulated here,
 * so nothing can diverge: a ghost has no collision, cannot be hit and is gone
 * the frame its source stops reporting it.  Hits stay where they already
 * were: a player's attacks land in their own game (on owned or puppeted
 * enemies, coop_enemy.c), so another player only needs to see them.
 *
 *   who spawned it   TaskCreate (task.c) tags every new task with the origin
 *                    of the task that created it: a Kirby slot's task is that
 *                    Kirby; a regular enemy from the room's object list that
 *                    is running its own state (not a puppet) is that enemy;
 *                    anything else passes its own origin on.  Bosses,
 *                    spawner children and items start no origin: they exist
 *                    in every game already.
 *   what it drew     DisplaySprite (sprite.c) hands every sprite drawn by a
 *                    tagged task to PortCoopFxCapture, minus the Kirbys'
 *                    own sprites (puppets draw those): animation, variant,
 *                    script cursor, world position, palette, flags, layer.
 *   drawing ghosts   at the top of the next frame (PortCoopFxFrame), after
 *                    every task: the script is walked to the cursor to find
 *                    the frame and its tiles, tiles go into OBJ VRAM the game
 *                    has free *this frame* (allocated and freed again at once:
 *                    the game's own objects always get it first), the upload
 *                    joins the game's own queue, and DisplaySprite does the
 *                    rest -- both renderers see an ordinary sprite.
 *
 * Palettes are the one thing a frame cannot carry.  A sprite names an OBJ
 * palette slot: 0..3 the Kirbys' own and 4..7 their hats (the page remaps
 * the sender's slots to the puppets showing those Kirbys), 8..14 the game's
 * per-room palette registry (gUnk_02022E60: a slot holds the palette of a
 * given animation; sent as that animation and variant, looked up here and
 * loaded only while the registry has room to spare), 14/15 fixed.
 *
 * Budgets: at most PORT_COOP_FX_MAX ghosts per source, FX_RING_MARGIN entries
 * of the 64-entry tile queue left for the game, FX_BYTES_MAX of uploads a
 * frame, and DisplaySprite's own OAM limit -- ghosts go in last, so they get
 * what is left.  A ghost that does not fit is skipped, never the game's.
 *
 * Not simulation state: co-op is refused under rollback, netplay and seats,
 * every hook is one load and a branch while gPortCoopFxOn is 0, and the
 * capture side only reads.  Drawing a ghost changes what the game draws
 * (OAM, the tile queue, and at worst one registry palette per room), which
 * is presentation. */

#include <stddef.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "main.h"
#include "task.h"
#include "sprite.h"
#include "malloc_vram.h"
#include "kirby.h"
#include "object.h"
#include "gba/m4a.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/coop.h"
#include "port/res.h"
#include "fnid_ids.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define COOP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define COOP_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern u8 sub_0803DFAC(u16, u8);      /* kirby.c: load a registry palette */

u8 gPortCoopFxOn;

#define FX_RING_MARGIN 12           /* tile-queue entries never used by ghosts */
#define FX_BYTES_MAX   (8 * 1024)   /* ghost tile uploads per frame          */
#define FX_PAL_LOADS   5            /* load a registry palette only below this */
#define FX_STALE       2            /* frames a source's last set is drawn   */
#define FX_WALK_MAX    96           /* script commands walked per ghost      */

enum { OR_NONE = 0, OR_KIRBY = 1 /* ..4: slot + 1 */, OR_ENEMY = 8 };

static u8  sOrigin[MAX_TASK_NUM];
static u16 sOriginIdx[MAX_TASK_NUM];
static u8  sPalVariant[16];          /* registry slot -> variant it was loaded with */
static u8  sShare;
static u8  sDrawing;

/* capture: filled during the frame's tasks, handed over at the next top */
static u8  sCap[PORT_COOP_FX_BYTES], sOut[PORT_COOP_FX_BYTES];
static int sCapN, sOutLen;
static u8  sCapPer[MAX_TASK_NUM];    /* sprites captured per task this frame */

/* ghosts: what the page set per source, and the sprites drawn from it */
struct FxSource {
    u8  buf[PORT_COOP_FX_BYTES];
    int len;
    u32 frame;                       /* PortFrameNumber() at the set         */
    u8  ids[PORT_COOP_FX_MAX];       /* ghost slot -> id it showed last      */
    u8  used[PORT_COOP_FX_MAX];
    u16 sndLast;                     /* newest sender stamp whose sounds ran */
    u8  sndInit;
    u8  loopN;                       /* sounds started here from this source */
    struct { u16 num; u32 at; } loop[8];
};
static struct FxSource sSrc[PORT_COOP_FX_SOURCES];
static struct Sprite sGhost[PORT_COOP_FX_SOURCES][PORT_COOP_FX_MAX];
static u8 sScratch[PORT_COOP_FX_BYTES];

/* hazards found by this frame's scan, and the stand-ins hits are made with */
static u8  sHaz[PORT_COOP_FX_HAZ_MAX * PORT_COOP_FX_HAZ_REC];
static int sHazN;
static struct ObjectBase sProxy[PORT_COOP_FX_SOURCES][PORT_COOP_FX_HAZ_MAX];

/* sounds: the last few frames' worth, repeated in every frame sent */
static struct { u16 num, room, src, stamp; } sSnd[32];
static int sSndAt, sSndN;
static u8  sPlaying;                 /* the receiver is playing: not captured */
static u32 sSndRoom = 0xFFFF;        /* the local room sounds were last played in */

/* captured, capture dropped (full), frames handed out, ghosts drawn,
 * skipped: palette, frame lookup, VRAM, queue/budget, wrong room, enemy not
 * a puppet here, registry palettes loaded; then hazards: 11 reported,
 * 12 tested here, 13 hit a Kirby here, 14 skipped (the enemy is not a puppet
 * here); sounds: 15 reported, 16 played here, 17 stopped here, 18 skipped
 * (room, enemy, bad number), 19 forwarded loops stopped by the safety net */
static u32 sStat[20];
/* FrameAt failures, by reason, and the last few (anim, variant, cursor,
 * reason) -- PortCoopFxStat(32 + reason), PortCoopFxStat(48 + 4 * i + k) */
static u32 sFail[8];
static u32 sFailLog[8][4];
static int sFailAt;
static u32 sPalFail[8][4];          /* pal, palKey, registry count, anim: 80 + 4 * i + k */
static int sPalFailAt;
static int sRingLow;
static u32 sBigBytes, sBigAnim, sBigVar, sBigFull;
static u32 sDemand, sDemandN, sDemandMax, sDemandMaxN;
static int Fail(int why, u32 anim, u32 var, u32 cursor)
{
    if (why >= 0 && why < 8)
        sFail[why]++;
    sFailLog[sFailAt][0] = anim;
    sFailLog[sFailAt][1] = var;
    sFailLog[sFailAt][2] = cursor;
    sFailLog[sFailAt][3] = (u32)why;
    sFailAt = (sFailAt + 1) & 7;
    return 0;
}

static void Put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u32  Get16(const u8 *p)  { return (u32)p[0] | ((u32)p[1] << 8); }

static int TaskIndex(const struct Task *t)
{
    ptrdiff_t d;

    if (t == NULL)
        return -1;
    d = t - gTasks;
    return d >= 0 && d < MAX_TASK_NUM ? (int)d : -1;
}

static u32 TaskMainId(const struct Task *t)
{
    u32 fn;

    memcpy(&fn, (const u8 *)t + 8, 4);     /* Task::main, a code ID (port/fnid.h) */
    return fn;
}

static int KirbyOfTask(const struct Task *t)
{
    int k;

    for (k = 0; k < 4; k++) {
        const struct Task *kt = gKirbys[k].task;

        if (kt == t)
            return k;
    }
    return -1;
}

/* --- origin -------------------------------------------------------------- */

void PortCoopFxTaskCreated(struct Task *task)
{
    const struct Task *cur = gCurTask;
    int ti = TaskIndex(task), ci = TaskIndex(cur), k;

    if (ti < 0)
        return;
    sOrigin[ti] = OR_NONE;
    sOriginIdx[ti] = 0;
    if (ci < 0 || ci == ti)
        return;
    if ((k = KirbyOfTask(cur)) >= 0) {
        sOrigin[ti] = (u8)(OR_KIRBY + k);
        return;
    }
    if (TaskMainId(cur) == FNID_ObjectMain) {
        const struct Object *o = (const struct Object *)TaskGetStructPtr((struct Task *)cur);
        int idx;

        if (o->base.header.kind == 1 && o->type <= 0x37
            && (idx = PortCoopObjectIndex(o)) >= 0) {
            /* a shared enemy: its children are reported only while this copy
             * runs its own state; a puppet's copy spawns nothing worth it */
            if (!PortCoopEnemyIsPuppet(o)) {
                sOrigin[ti] = OR_ENEMY;
                sOriginIdx[ti] = (u16)idx;
            }
            return;
        }
    }
    sOrigin[ti] = sOrigin[ci];
    sOriginIdx[ti] = sOriginIdx[ci];
}

void PortCoopFxPalette(u32 slot, u32 key, u32 variant)
{
    (void)key;
    if (slot < 16)
        sPalVariant[slot] = (u8)variant;
}

/* --- capture ------------------------------------------------------------- */

static int InKirbys(const struct Sprite *s)
{
    uintptr_t a = (uintptr_t)s, lo = (uintptr_t)&gKirbys[0], hi = (uintptr_t)&gKirbys[4];

    return a >= lo && a < hi;
}

void PortCoopFxCapture(struct Sprite *s)
{
    const struct Task *cur;
    int ti, o, k, n;
    s32 sx, sy, wx, wy;
    u8 *r, pal = 0xFF;
    u16 palKey = 0, src;
    u32 p;

    if (sDrawing || !sShare || s == NULL)
        return;
    cur = gCurTask;
    if ((ti = TaskIndex(cur)) < 0)
        return;
    o = sOrigin[ti];
    if (o == OR_NONE && (k = KirbyOfTask(cur)) >= 0)
        o = OR_KIRBY + k;               /* the Kirby task's own extra sprites */
    if (o == OR_NONE || InKirbys(s))
        return;
    if (o >= OR_KIRBY && o < OR_KIRBY + 4) {
        k = o - OR_KIRBY;
        if (k != gLocalPlayerId && PORT_COOP_IS_PUPPET(k))
            return;
        src = (u16)(k << 2);
    } else {
        src = (u16)(1 | (sOriginIdx[ti] << 2));
    }
    if (s->unk4 == (u32)-1 || s->animId == 0)
        return;
    if (sCapN >= PORT_COOP_FX_MAX || sCapPer[ti] >= 8) {
        sStat[1]++;
        return;
    }

    p = s->palId;
    if (p < 8) {
        pal = (u8)p;
    } else if (p - 8 < gUnk_02022EA0 && p - 8 < 7) {
        pal = 0x10;
        palKey = (u16)((gUnk_02022E60[p - 8] & 0xFFF) | ((u32)(sPalVariant[p] & 0xF) << 12));
    } else if (p == 14 || p == 15) {
        pal = (u8)p;
    }
    if (pal == 0xFF)
        return;

    sx = s->x;
    sy = s->y;
    if (s->unk8 & 0x20000) {
        sx -= gUnk_030023F4.unk0;
        sy -= gUnk_030023F4.unk2;
    }
    wx = sx + (gCurLevelInfo[gLocalPlayerId].viewportPosition.x >> 8) - gUnk_0203AD18[0];
    wy = sy + (gCurLevelInfo[gLocalPlayerId].viewportPosition.y >> 8) - gUnk_0203AD18[1];

    n = sCapPer[ti]++;
    r = sCap + 4 + sCapN * PORT_COOP_FX_REC;
    Put16(r + 0, s->animId);
    r[2] = s->variant;
    /* id: the task (0..127); its second sprite +0x80; any more, hashed */
    r[3] = (u8)(n == 0 ? ti : n == 1 ? (ti | 0x80) : (((ti * 7 + n * 31) & 0x7F) | 0x80));
    Put16(r + 4, (u32)(u16)(s16)wx);
    Put16(r + 6, (u32)(u16)(s16)wy);
    Put16(r + 8, s->animCursor);
    r[10] = pal;
    r[11] = (u8)((s->unk14 & 0x7C0) >> 6);
    Put16(r + 12, (s->unk8 >> 7) & 0xFFFF);
    Put16(r + 14, palKey);
    Put16(r + 16, src);
    sCapN++;
    sStat[0]++;
}

/* --- the frame a script cursor is at ------------------------------------- */

static u32 sAnimCount;   /* animations in the table, 0 = not counted yet */

static int RomPtr(const void *p, u32 len)
{
    uintptr_t a = (uintptr_t)p;

    return a >= GBA_ROM_BASE && gPortRomSize != 0 && a + len <= GBA_ROM_BASE + gPortRomSize;
}

static u32 AnimCount(void)
{
    if (sAnimCount == 0 && gSpriteTables != NULL) {
        u32 n = 0;

        while (n < 4096) {
            const void *v = gSpriteTables->anims[n];

            if (!RomPtr(v, 4))
                break;
            n++;
        }
        sAnimCount = n ? n : 1;
    }
    return sAnimCount;
}

/* Sizes of the animation script commands, by ~cmdId (sprite.h). */
static const u8 kFxCmdSize[13] = { 0, 0xC, 0xC, 8, 4, 8, 0xC, 8, 0xC, 8, 0x10, 8, 8 };

struct FxFrame {
    s32 index;                 /* AnimCmd_ShowFrame.index, as the game stores it */
    const u8 *tiles;           /* sub frames: the GetTiles source             */
    u32 bytes;                 /* sub frames: bytes to upload                 */
    u32 vramTiles;             /* 4bpp tile slots to allocate                 */
    u32 runs;                  /* tile-queue entries the upload takes         */
    u32 base;                  /* nonzero: draw from this block, upload nothing */
    u32 skip;                  /* tiles below the first one the frame uses     */
    u8  full;
};

/* Tiles (4bpp units) a frame's OAM entries reach from its base: the highest
 * tile any of its subsprites uses, plus that subsprite's size.  0 when it
 * cannot tell. */
static const u8 kFxObjTiles[12] = {
    1, 4, 16, 64,      /* square 8, 16, 32, 64 */
    2, 4, 8, 32,       /* wide 16x8, 32x8, 32x16, 64x32 */
    2, 4, 8, 32,       /* tall 8x16, 8x32, 16x32, 32x64 */
};
static u32 FrameTiles(u32 anim, const struct SpriteAttributes_Sub *sub, u32 *first)
{
    const u16 *oam = gSpriteTables->oamData[anim];
    u32 k, need = 0, lo = 0xFFFF;

    if (sub->numSubframes == 0 || sub->numSubframes > 64)
        return 0;
    for (k = 0; k < sub->numSubframes; k++) {
        const u16 *t = &oam[3 * ((sub->bitfield & 0x3FFF) + k)];
        u32 shape, tiles, end;

        if (!RomPtr(t, 6))
            return 0;
        shape = ((t[0] & 0xC000) >> 12) | ((t[1] & 0xC000) >> 14);
        if (shape >= 12)
            return 0;
        tiles = kFxObjTiles[shape];
        if (t[0] & 0x2000) {
            end = (t[2] & 0x3FF) * 2 + tiles * 2;    /* 8bpp: two slots a tile */
            if ((t[2] & 0x3FF) * 2 < lo) lo = (t[2] & 0x3FF) * 2;
        } else {
            end = (t[2] & 0x3FF) + tiles;
            if ((t[2] & 0x3FF) < lo) lo = t[2] & 0x3FF;
        }
        if (end > need)
            need = end;
    }
    *first = lo == 0xFFFF ? 0 : lo;
    return need;
}

/* A block this game already holds with the tiles a shared-tile animation
 * draws from (sub_0803DE54 / sub_0803DD58, kirby.c): its address, or 0. */
static u32 GameBlock(u32 anim)
{
    int i;

    for (i = 0; i < 8; i++) {
        u32 key = gUnk_02022E70[i];

        if (key == 0xFFFF)
            break;
        if (key == anim + 0x100)
            return gUnk_02022E80[i];
        if (key < 0x100 && gUnk_08351648[key].numTiles != 0xFFFFFFFF) {
            const void *e = gUnk_08351648[key].unk14;

            if (e != NULL && gUnk_08351648[key].unk14->animId == anim)
                return gUnk_02022E80[i];
        }
    }
    return 0;
}

/* The first GetTiles in (anim, variant)'s opening commands, or NULL. */
static const struct AnimCmd_GetTiles *FirstTiles(u32 anim, u32 variant)
{
    const union AnimCmd *base = gSpriteTables->anims[anim];
    union AnimCmd v;
    const s32 *w;
    int n;

    if (!RomPtr(&base[variant], 4))
        return NULL;
    v = base[variant];
    w = &v.words[0];
    for (n = 0; n < 8; n++) {
        s32 id;

        if (!RomPtr(w, 16) || (id = w[0]) >= 0 || -id >= (s32)sizeof(kFxCmdSize) || id == -3 || id == -4)
            return NULL;
        if (id == -1)
            return (const struct AnimCmd_GetTiles *)(const void *)w;
        w += kFxCmdSize[-id] / 4;
    }
    return NULL;
}

/* A variant with no GetTiles of its own draws from tiles another variant of
 * the same animation loaded once into a block shared by every object of the
 * kind (sub_0803DE54, kirby.c: the variant the first object was made with).
 * Which one that was is not recorded anywhere, so: the first variant that
 * loads any.  Remembered per animation (0xFF = none found). */
static u8 sSharedVariant[1024];
static u8 sSharedKnown[1024 / 8];
static const struct AnimCmd_GetTiles *SharedTiles(u32 anim)
{
    u32 v;

    if (anim >= 1024)
        return NULL;
    if (!(sSharedKnown[anim >> 3] & (1 << (anim & 7)))) {
        sSharedVariant[anim] = 0xFF;
        for (v = 0; v < 16; v++)
            if (FirstTiles(anim, v) != NULL) { sSharedVariant[anim] = (u8)v; break; }
        sSharedKnown[anim >> 3] |= (u8)(1 << (anim & 7));
    }
    return sSharedVariant[anim] == 0xFF ? NULL : FirstTiles(anim, sSharedVariant[anim]);
}

/* Walk (anim, variant)'s script, without taking jumps, up to `cursor` (the
 * sprite's animCursor: just past the ShowFrame shown, or -- once the
 * animation has ended -- at the end command, past the last one).  The frame
 * is the last ShowFrame that ends at or before the cursor, and the tiles the
 * last GetTiles written before that frame: the one in force on the first
 * pass through a loop or any later one. */
static int FrameAt(u32 anim, u32 variant, u32 cursor, struct FxFrame *f)
{
    const union AnimCmd *base;
    union AnimCmd v;
    const s32 *w, *w0;
    const struct AnimCmd_GetTiles *gt = NULL, *gtShown = NULL;
    int n, have = 0;

    memset(f, 0, sizeof(*f));
    if (gSpriteTables == NULL || anim >= AnimCount() || variant > 255 || cursor == 0 || cursor > 0x400)
        return Fail(0, anim, variant, cursor);
    base = gSpriteTables->anims[anim];
    if (!RomPtr(&base[variant], 4))
        return Fail(1, anim, variant, cursor);
    v = base[variant];
    w0 = w = &v.words[0];
    for (n = 0; n < FX_WALK_MAX && (u32)(w - w0) < cursor; n++) {
        s32 id;

        if (!RomPtr(w, 16))
            return Fail(1, anim, variant, cursor);
        id = w[0];
        if (id >= 0) {
            const struct AnimCmd_ShowFrame *sf = (const struct AnimCmd_ShowFrame *)(const void *)w;

            w += sizeof(struct AnimCmd_ShowFrame) / 4;
            if ((u32)(w - w0) <= cursor) {
                f->index = sf->index;
                gtShown = gt;
                have = 1;
            }
            continue;
        }
        if (-id >= (s32)sizeof(kFxCmdSize))
            return Fail(2, anim, variant, cursor);
        if (id == -4)
            break;              /* the end: the last frame shown stays up */
        if (id == -1)
            gt = (const struct AnimCmd_GetTiles *)(const void *)w;
        w += kFxCmdSize[-id] / 4;
    }
    if (!have)
        return Fail(3, anim, variant, cursor);

    if (((u32)f->index >> 28) == 1) {
        const struct SpriteAttributes_Full *full;
        const s32 *list;
        union SpriteAttributes at = gSpriteTables->attrs[anim];
        u32 cnt, i, unit;

        full = &at.full[(u32)f->index & 0x0FFFFFFF];
        if (!RomPtr(full, sizeof(*full)))
            return Fail(4, anim, variant, cursor);
        cnt = full->unkC >> 24;
        list = gSpriteTables->unk18 + (full->unkC & 0xFFFFFF);
        if (cnt == 0 || cnt > 128 || !RomPtr(list, cnt * 4))
            return Fail(4, anim, variant, cursor);
        unit = list[0] >= 0 ? 1 : 2;
        f->full = 1;
        f->vramTiles = cnt * unit;
        f->bytes = cnt * 32 * unit;
        f->runs = 1;
        for (i = 1; i < cnt; i++)
            if (list[i] != list[i - 1] + 1)
                f->runs++;
    } else {
        const void *t4 = gSpriteTables->tiles4bpp, *t8 = gSpriteTables->tiles8bpp;
        const struct SpriteAttributes_Sub *sub;
        union SpriteAttributes at = gSpriteTables->attrs[anim];

        sub = &at.sub[f->index];
        if (!RomPtr(sub, sizeof(*sub)))
            return Fail(4, anim, variant, cursor);
        if (gtShown == NULL) {
            /* the game's own copy of the shared tiles, if it holds them */
            if ((f->base = GameBlock(anim)) != 0)
                return 1;
            gtShown = SharedTiles(anim);
        }
        if (gtShown == NULL || gtShown->numTilesToCopy == 0 || gtShown->numTilesToCopy > 256)
            return Fail(5, anim, variant, cursor);
        {
            /* only as many tiles as the frame's OAM entries reach: a block
             * shared by a kind of object holds every frame's tiles */
            u32 lo, need = FrameTiles(anim, sub, &lo), have = gtShown->numTilesToCopy;
            u32 unit = gtShown->tileIndex < 0 ? 2 : 1;

            if (need == 0 || need > have * unit || lo >= need) {
                need = have * unit;
                lo = 0;
            }
            if (gtShown->tileIndex < 0)
                f->tiles = (const u8 *)t8 + 0x40 * gtShown->tileIndex;
            else
                f->tiles = (const u8 *)t4 + 0x20 * gtShown->tileIndex;
            /* from the first tile used: the sprite's base then sits that many
             * tiles before the upload (DrawSource checks it stays in OBJ VRAM) */
            f->skip = lo;
            f->tiles += 0x20 * lo;
            f->bytes = 0x20 * (need - lo);
            f->vramTiles = need - lo;
        }
        if (!RomPtr(f->tiles, f->bytes ? f->bytes : 1))
            return Fail(6, anim, variant, cursor);
        f->runs = (f->bytes + 0xFFDF) / 0xFFE0;
    }
    return 1;
}

/* --- ghosts -------------------------------------------------------------- */

static int RingFree(void)
{
    return 63 - (int)((gUnk_030039A4 - gUnk_03006078) & 0x3F);
}

static int RegistrySlot(u32 palKey)
{
    u32 key = palKey & 0xFFF, i, n = gUnk_02022EA0;

    if (key == 0)
        return -1;
    for (i = 0; i < n && i < 7; i++)
        if ((gUnk_02022E60[i] & 0xFFF) == key)
            return (int)i + 8;
    if (n >= FX_PAL_LOADS)
        return -1;
    sStat[10]++;
    return sub_0803DFAC((u16)key, (u8)(palKey >> 12));
}

/* the ghost slot for id: the one that showed it last frame, else a free one */
static int GhostSlot(struct FxSource *s, u8 id, u8 *taken)
{
    int i;

    for (i = 0; i < PORT_COOP_FX_MAX; i++)
        if (s->used[i] && s->ids[i] == id && !taken[i])
            return i;
    for (i = 0; i < PORT_COOP_FX_MAX; i++)
        if (!s->used[i] && !taken[i])
            return i;
    for (i = 0; i < PORT_COOP_FX_MAX; i++)
        if (!taken[i])
            return i;
    return -1;
}

static u32 sAlloc[PORT_COOP_FX_SOURCES * PORT_COOP_FX_MAX];
static int sAllocN;
static u32 sBytes;
/* this frame's uploads, for ghosts that show the same tiles */
static struct { const u8 *tiles; u32 bytes, anim, vram; s32 index; u8 full; } sUp[64];
static int sUpN;

static void DrawSource(int si, u32 room)
{
    struct FxSource *s = &sSrc[si];
    u8 taken[PORT_COOP_FX_MAX], used[PORT_COOP_FX_MAX], ids[PORT_COOP_FX_MAX];
    const u8 *b = s->buf;
    int n = b[2], i;
    s32 vx = gCurLevelInfo[gLocalPlayerId].viewportPosition.x >> 8;
    s32 vy = gCurLevelInfo[gLocalPlayerId].viewportPosition.y >> 8;

    memset(taken, 0, sizeof(taken));
    memset(used, 0, sizeof(used));
    if (Get16(b) != room) {
        sStat[8] += (u32)n;
        memset(s->used, 0, sizeof(s->used));
        return;
    }
    if (n > PORT_COOP_FX_MAX || 4 + n * PORT_COOP_FX_REC > s->len)
        n = (s->len - 4) / PORT_COOP_FX_REC;
    for (i = 0; i < n; i++) {
        const u8 *r = b + 4 + i * PORT_COOP_FX_REC;
        u32 anim = Get16(r), var = r[2], cursor = Get16(r + 8), src = Get16(r + 16);
        s32 wx = (s16)Get16(r + 4), wy = (s16)Get16(r + 6);
        int pal = r[10], slot, palId;
        struct FxFrame f;
        struct Sprite *g;
        u32 vram, attr;
        int fresh = 0;

        if ((src & 3) == 1 && !PortCoopEnemyPuppetAt(room, src >> 2)) {
            sStat[9]++;
            continue;
        }
        if (pal < 8) {
            palId = pal;
        } else if (pal == 14 || pal == 15) {
            palId = (pal == 14 && gUnk_02022EA0 >= 7) ? -1 : pal;
        } else if (pal == 0x10) {
            palId = RegistrySlot(Get16(r + 14));
        } else {
            palId = -1;
        }
        if (palId < 0 || palId > 15) {
            sStat[4]++;
            sPalFail[sPalFailAt][0] = (u32)pal;
            sPalFail[sPalFailAt][1] = Get16(r + 14);
            sPalFail[sPalFailAt][2] = gUnk_02022EA0;
            sPalFail[sPalFailAt][3] = anim;
            sPalFailAt = (sPalFailAt + 1) & 7;
            continue;
        }
        if (!FrameAt(anim, var, cursor, &f)) {
            sStat[5]++;
            continue;
        }
        sDemand += f.bytes;
        sDemandN++;
        if (f.bytes > sBigBytes) { sBigBytes = f.bytes; sBigAnim = anim; sBigVar = var; sBigFull = f.full; }
        /* the same tiles already went up this frame (particles of one kind,
         * the same frame twice): point at them */
        vram = f.base;
        if (vram == 0) {
            int u;

            for (u = 0; u < sUpN; u++)
                if (sUp[u].full == f.full && (f.full ? (sUp[u].anim == anim && sUp[u].index == f.index)
                                                     : (sUp[u].tiles == f.tiles && sUp[u].bytes >= f.bytes))) {
                    vram = sUp[u].vram;
                    break;
                }
        }
        if (vram == 0 && ((int)f.runs > RingFree() - FX_RING_MARGIN || sBytes + f.bytes > FX_BYTES_MAX)) {
            sStat[7]++;
            Fail((int)f.runs > RingFree() - FX_RING_MARGIN ? 6 : 7, anim, f.bytes, sBytes);
            if (!sRingLow || RingFree() < sRingLow) sRingLow = RingFree();
            continue;
        }
        if (vram == 0) {
            vram = VramMalloc(f.vramTiles);
            if (vram == (u32)(uintptr_t)ewram_end) {
                sStat[6]++;
                continue;
            }
            if (vram - 0x20 * f.skip < 0x6010000u) {
                /* the frame's first tile is too far in to start the sprite
                 * that far before this block: take the whole range instead */
                VramFree(vram);
                if (sBytes + f.bytes + 0x20 * f.skip > FX_BYTES_MAX
                    || (vram = VramMalloc(f.vramTiles + f.skip)) == (u32)(uintptr_t)ewram_end) {
                    sStat[6]++;
                    continue;
                }
                f.tiles -= 0x20 * f.skip;
                f.bytes += 0x20 * f.skip;
                f.vramTiles += f.skip;
                f.skip = 0;
            }
            sAlloc[sAllocN++] = vram;
            sBytes += f.bytes;
            if (sUpN < (int)(sizeof(sUp) / sizeof(sUp[0]))) {
                sUp[sUpN].tiles = f.tiles;
                sUp[sUpN].bytes = f.bytes;
                sUp[sUpN].anim = anim;
                sUp[sUpN].index = f.index;
                sUp[sUpN].full = f.full;
                sUp[sUpN].vram = vram;
                sUpN++;
            }
            fresh = 1;
        }

        slot = GhostSlot(s, r[3], taken);
        if (slot < 0)
            continue;
        taken[slot] = 1;
        used[slot] = 1;
        ids[slot] = r[3];
        g = &sGhost[si][slot];
        memset(g, 0, sizeof(*g));
        attr = ((u32)Get16(r + 12) << 7) & ~(0x4000u | 0x20000u | 0x40000u | 0x80000u);
        g->tilesVram = vram - (f.base ? 0 : 0x20 * f.skip);
        g->unk4 = (u32)f.index;
        g->unk8 = attr | (f.full && fresh ? 0x4000000u : 0);
        g->animId = (u16)anim;
        g->animCursor = (u16)cursor;
        g->x = (s16)(wx - vx + gUnk_0203AD18[0]);
        g->y = (s16)(wy - vy + gUnk_0203AD18[1]);
        g->unk14 = (u16)(r[11] << 6);
        g->unk18 = (u16)anim;
        g->variant = (u8)var;
        g->unk1B = (u8)var;
        g->unk1C = 0x10;
        g->palId = (u8)palId;
        g->unk20[0].unk0 = -1;
        if (!f.full && fresh) {
            u32 left = f.bytes, off = 0;

            while (left) {
                u32 chunk = left > 0xFFFF - 31 ? 0xFFE0 : left;

                gUnk_03002EC0[gUnk_030039A4].unk0 = (u32)(uintptr_t)(f.tiles + off);
                gUnk_03002EC0[gUnk_030039A4].unk4 = vram + off;
                gUnk_03002EC0[gUnk_030039A4].unk8 = (u16)chunk;
                gUnk_030039A4 = (gUnk_030039A4 + 1) & 0x3F;
                left -= chunk;
                off += chunk;
            }
        }
        DisplaySprite(g);
        sStat[3]++;
    }
    for (i = 0; i < PORT_COOP_FX_MAX; i++) {
        s->used[i] = used[i];
        if (used[i])
            s->ids[i] = ids[i];
    }
}

/* --- hazards --------------------------------------------------------------------
 *
 * A ghost cannot touch anyone, so a shared enemy's shots could never hurt a
 * player whose game shows that enemy as a puppet -- only the player whose
 * game runs it.  The game that runs the enemy therefore also reports what its
 * spawning has in the collision lists to hit Kirbys this frame: hitbox,
 * facing, attack flags, damage, knockback.  Every other game in the room
 * tests those against its own Kirbys right after its own collisions, with the
 * game's overlap test, and a touch goes through the game's own reaction to a
 * projectile (PortCoopHazardTouch, object_collision.c): damage, knockback,
 * losing the ability, the invincible frames that keep one shot from hitting
 * twice.  Left out: grabs (attack flag 0x20 hands the Kirby to the enemy's
 * state, which does not run here) and the enemies' own bodies (puppets,
 * solid already).  A hazard is applied only over a puppet of its enemy, as
 * a ghost is drawn: a copy that runs its own state shoots its own shots. */

static int TaskOfStruct(const void *p)
{
    const struct Task *cur = gCurTask;
    int i = TaskIndex(cur);

    if (i >= 0 && cur->prev != 0 && cur->structOffset != 0
        && TaskGetStructPtr((struct Task *)cur) == p)
        return i;
    for (i = 0; i < MAX_TASK_NUM; i++) {
        struct Task *t = &gTasks[i];

        if (t->prev == 0 || t->structOffset == 0)
            continue;
        if (TaskGetStructPtr(t) == p)
            return i;
    }
    return -1;
}

void PortCoopFxHazardScan(void)
{
    u32 g, kind, j, n, room;

    sHazN = 0;
    if (!sShare || !PortCoopReady())
        return;
    g = gCurLevelInfo[gLocalPlayerId].unk65E;
    if (g >= 4)
        return;
    room = gKirbys[gLocalPlayerId].base.roomId;
    for (kind = 0; kind < 2; kind++) {
        n = gUnk_02022EB0[g][kind];
        for (j = 0; j < n && j < 0x1F && sHazN < PORT_COOP_FX_HAZ_MAX; j++) {
            struct ObjectBase *e = gUnk_02022F50[g * 0x40 + kind * 0x20 + j];
            u8 *r;
            int ti;
            u32 f = 0;

            if (e == NULL || e->roomId != room || (e->flags & 0x200) || !(e->flags & 0x10000000))
                continue;
            if (e->unk68 & (0x20 | 0x800000))
                continue;                   /* a grab, or harmless to Kirbys */
            if (e->header.kind == 1 && ((struct Object *)e)->type <= 0x52)
                continue;                   /* an enemy or a boss itself     */
            if ((ti = TaskOfStruct(e)) < 0 || sOrigin[ti] != OR_ENEMY)
                continue;
            if (e->flags & 1)
                f |= 1;
            if (e->sprite.unk20[0].unk0 == 0)
                f |= 2;
            r = sHaz + sHazN * PORT_COOP_FX_HAZ_REC;
            Put16(r + 0, (u32)(u16)(s16)(e->x >> 8));
            Put16(r + 2, (u32)(u16)(s16)(e->y >> 8));
            r[4] = (u8)e->unk38;
            r[5] = (u8)e->unk39;
            r[6] = (u8)e->unk3A;
            r[7] = (u8)e->unk3B;
            r[8] = (u8)e->sprite.unk20[0].unk4;
            r[9] = (u8)e->sprite.unk20[0].unk5;
            r[10] = (u8)e->sprite.unk20[0].unk6;
            r[11] = (u8)e->sprite.unk20[0].unk7;
            r[12] = (u8)f;
            r[13] = (u8)e->unk63;
            Put16(r + 14, (u32)(u16)e->unk64);
            Put16(r + 16, (u32)(u16)e->unk66);
            Put16(r + 18, (u32)e->unk68 & 0xFFFF);
            Put16(r + 20, (u32)e->unk68 >> 16);
            Put16(r + 22, sOriginIdx[ti]);
            sHazN++;
            sStat[11]++;
        }
    }
}

void PortCoopFxHazardHits(void)
{
    u32 now = PortFrameNumber(), room;
    int si, i;

    if (!sShare || !PortCoopReady())
        return;
    room = gKirbys[gLocalPlayerId].base.roomId;
    for (si = 0; si < PORT_COOP_FX_SOURCES; si++) {
        struct FxSource *s = &sSrc[si];
        const u8 *b = s->buf;
        int n, nh, off;

        if (s->len < 4 || now - s->frame > FX_STALE || Get16(b) != room)
            continue;
        n = b[2];
        nh = b[3];
        off = 4 + n * PORT_COOP_FX_REC;
        if (nh > PORT_COOP_FX_HAZ_MAX)
            nh = PORT_COOP_FX_HAZ_MAX;
        if (off + nh * PORT_COOP_FX_HAZ_REC > s->len)
            nh = off < s->len ? (s->len - off) / PORT_COOP_FX_HAZ_REC : 0;
        for (i = 0; i < nh; i++) {
            const u8 *r = b + off + i * PORT_COOP_FX_HAZ_REC;
            struct ObjectBase *a = &sProxy[si][i];

            if (!PortCoopEnemyPuppetAt(room, Get16(r + 22))) {
                sStat[14]++;
                continue;
            }
            memset(a, 0, sizeof(*a));
            a->header.kind = 2;
            a->flags = 0x10000000 | (r[12] & 1);
            a->x = (s32)(s16)Get16(r + 0) << 8;
            a->y = (s32)(s16)Get16(r + 2) << 8;
            a->unk38 = (s8)r[4];
            a->unk39 = (s8)r[5];
            a->unk3A = (s8)r[6];
            a->unk3B = (s8)r[7];
            a->sprite.unk20[0].unk0 = (r[12] & 2) ? 0 : -1;
            a->sprite.unk20[0].unk4 = (s8)r[8];
            a->sprite.unk20[0].unk5 = (s8)r[9];
            a->sprite.unk20[0].unk6 = (s8)r[10];
            a->sprite.unk20[0].unk7 = (s8)r[11];
            a->unk56 = (u8)gLocalPlayerId;
            a->roomId = (u16)room;
            a->unk63 = (s8)r[13];
            a->unk64 = (s16)Get16(r + 14);
            a->unk66 = (s16)Get16(r + 16);
            a->unk68 = (s32)(Get16(r + 18) | (Get16(r + 20) << 16));
            sStat[12]++;
            if (PortCoopHazardTouch(a))
                sStat[13]++;
        }
    }
}

/* --- sounds -----------------------------------------------------------------------
 *
 * The game plays a sound for the local Kirby and for objects in the local
 * room (PlaySfx, data.h).  A puppet runs no state and a ghost is not an
 * object, so another player's jumps, inhale and attacks, and everything a
 * shared enemy does in the game that runs it, were silent everywhere else.
 * The game that makes such a sound reports it (the local Kirby's own, what
 * its spawning or a CPU Kirby's spawning plays -- a CPU Kirby itself is
 * silent in the game too -- and a shared enemy's), with the stops of the
 * looping ones (inhale, fire breath, the slide); the others play it when
 * they are in that room and, for an enemy's, show that enemy as a puppet.
 * Songs on the music player (0) are never sent: the invincibility tune is
 * the sender's own business.
 *
 * Each frame sent repeats the sounds of the last PORT_COOP_FX_SND_KEEP
 * frames with the frame they were made in, and the receiver plays each stamp
 * once, so a frame the page skips or shows twice loses or doubles nothing.
 * A sound started here from a source is stopped again if that source goes
 * quiet, leaves, or has not sent its stop within ten seconds. */

#define SND_LATE   12     /* frames: an older sound is not worth playing late */
#define SND_LOOP   600    /* frames a sound started here may run unstopped     */
#define SND_QUIET  30     /* frames without a set before a source's are stopped */
#define SND_LOOPS  8      /* sounds started here per source, tracked for stops */

static void SndPush(u32 num, u32 room, u32 src)
{
    int i = (sSndAt + sSndN) & 31;

    if (sSndN == 32) {
        sSndAt = (sSndAt + 1) & 31;
        sSndN--;
        i = (sSndAt + sSndN) & 31;
    }
    sSnd[i].num = (u16)num;
    sSnd[i].room = (u16)room;
    sSnd[i].src = (u16)src;
    sSnd[i].stamp = (u16)PortFrameNumber();
    sSndN++;
    sStat[15]++;
}

/* src for the object a sound or a stop belongs to: a Kirby slot's (k << 2)
 * or a shared enemy's (1 | idx << 2); -1 when no one else needs it. */
static int SourceOfTask(int ti)
{
    int o, k;
    const struct Task *t;

    if (ti < 0)
        return -1;
    t = &gTasks[ti];
    o = sOrigin[ti];
    if (o == OR_NONE && (k = KirbyOfTask(t)) >= 0)
        o = OR_KIRBY + k;
    if (o >= OR_KIRBY && o < OR_KIRBY + 4) {
        k = o - OR_KIRBY;
        if (k != gLocalPlayerId && PORT_COOP_IS_PUPPET(k))
            return -1;
        return k << 2;
    }
    if (o == OR_ENEMY)
        return 1 | (sOriginIdx[ti] << 2);
    if (TaskMainId(t) == FNID_ObjectMain && t->structOffset != 0) {
        const struct Object *ob = (const struct Object *)TaskGetStructPtr((struct Task *)t);
        int idx;

        if (ob->base.header.kind == 1 && ob->type <= 0x37
            && (idx = PortCoopObjectIndex(ob)) >= 0 && !PortCoopEnemyIsPuppet(ob))
            return 1 | (idx << 2);
    }
    return -1;
}

static int Forwardable(u32 num)
{
    return num < Res_Count(RES_SONG_TABLE) && gSongTable[num].ms != 0;
}

void PortCoopFxSound(u32 kind, u32 who, u32 room, const void *obj, u32 num, u32 alt)
{
    int src;

    if (sPlaying || !sShare || !Forwardable(num) || !PortCoopReady())
        return;
    if (kind == 0) {
        /* a Kirby: the local one only (a CPU Kirby is silent in the game) */
        if (who != gLocalPlayerId)
            return;
        src = (int)(who << 2);
    } else if (kind == 1 && ((const struct Object *)obj)->type <= 0x37) {
        const struct Object *ob = (const struct Object *)obj;
        int idx = PortCoopObjectIndex(ob);

        if (idx < 0 || PortCoopEnemyIsPuppet(ob))
            return;
        src = 1 | (idx << 2);
    } else if ((src = SourceOfTask(TaskOfStruct(obj))) < 0) {
        return;
    }
    SndPush(num | (alt ? 0x4000 : 0), room, (u32)src);
}

void PortCoopFxSoundStop(u32 num)
{
    int src;

    if (sPlaying || !sShare || !Forwardable(num) || !PortCoopReady())
        return;
    if ((src = SourceOfTask(TaskIndex(gCurTask))) < 0)
        return;
    SndPush(num | 0x8000, gKirbys[gLocalPlayerId].base.roomId, (u32)src);
}

/* A sound as the game plays it for an object in the local room. */
static void SndPlay(u32 num, u32 alt, u32 room)
{
    struct ObjectBase o;

    memset(&o, 0, sizeof(o));
    o.header.kind = 2;
    o.roomId = (u16)room;
    if (alt)
        PlaySfxAlt(&o, num);
    else
        PlaySfx(&o, num);
}

/* The song is what its player is playing right now. */
static int SongPlaying(u32 num)
{
    const struct Song *song = &gSongTable[num];
    const struct MusicPlayerInfo *mp = gMPlayTable[song->ms].info;

    return mp->songHeader == song->header && (mp->status & MUSICPLAYER_STATUS_TRACK) != 0;
}

/* Forget what has finished; stop what still plays and is due: everything
 * (all), or what has run SND_LOOP frames without its stop. */
static void SndStopLoops(struct FxSource *s, u32 now, int all)
{
    int i;

    sPlaying = 1;
    for (i = 0; i < s->loopN; ) {
        u32 num = s->loop[i].num;

        if (!SongPlaying(num)) {
            s->loop[i] = s->loop[--s->loopN];
        } else if (all || now - s->loop[i].at > SND_LOOP) {
            m4aSongNumStop((u16)num);
            if (!all)
                sStat[19]++;
            s->loop[i] = s->loop[--s->loopN];
        } else {
            i++;
        }
    }
    sPlaying = 0;
}

static void SndFrame(int si, u32 room, u32 now)
{
    struct FxSource *s = &sSrc[si];
    const u8 *b = s->buf, *r;
    int n, nh, off, ns, i;
    u16 newest = 0;

    if (s->len < 4)
        return;
    n = b[2];
    nh = b[3];
    off = 4 + n * PORT_COOP_FX_REC + nh * PORT_COOP_FX_HAZ_REC;
    if (off >= s->len)
        return;
    ns = b[off++];
    if (ns > PORT_COOP_FX_SND_MAX || off + ns * PORT_COOP_FX_SND_REC > s->len)
        ns = (s->len - off) / PORT_COOP_FX_SND_REC;
    if (ns <= 0)
        return;
    newest = (u16)Get16(b + off + (ns - 1) * PORT_COOP_FX_SND_REC + 6);
    for (i = 0; i < ns; i++) {
        u16 st = (u16)Get16(b + off + i * PORT_COOP_FX_SND_REC + 6);

        if ((s16)(st - newest) > 0)
            newest = st;
    }
    /* first contact, or a stamp far from the last one (another sender in
     * this source, a long freeze): only what is new from now on */
    if (!s->sndInit || (s16)(newest - s->sndLast) > 600 || (s16)(newest - s->sndLast) < -600) {
        s->sndInit = 1;
        s->sndLast = (u16)(newest - 3);
    }
    sPlaying = 1;
    for (i = 0; i < ns; i++) {
        u32 num, flags, rroom, src;
        u16 st;
        int k;

        r = b + off + i * PORT_COOP_FX_SND_REC;
        st = (u16)Get16(r + 6);
        if ((s16)(st - s->sndLast) <= 0)
            continue;
        num = Get16(r) & 0x3FFF;
        flags = Get16(r) & 0xC000;
        rroom = Get16(r + 2);
        src = Get16(r + 4);
        if (!Forwardable(num)) {
            sStat[18]++;
            continue;
        }
        if (flags & 0x8000) {
            for (k = 0; k < s->loopN; k++)
                if (s->loop[k].num == num) {
                    m4aSongNumStop((u16)num);
                    s->loop[k] = s->loop[--s->loopN];
                    sStat[17]++;
                    break;
                }
            continue;
        }
        if ((s16)(newest - st) > SND_LATE || rroom != room
            || ((src & 3) == 1 && !PortCoopEnemyPuppetAt(room, src >> 2))) {
            sStat[18]++;
            continue;
        }
        SndPlay(num, flags & 0x4000, room);
        sStat[16]++;
        if (!SongPlaying(num))
            continue;               /* refused (a busier sound), or a blip */
        for (k = 0; k < s->loopN && s->loop[k].num != num; k++)
            ;
        if (k == s->loopN) {
            if (s->loopN == SND_LOOPS) {
                sPlaying = 0;
                SndStopLoops(s, now, 0);    /* drops the finished ones */
                sPlaying = 1;
                if (s->loopN == SND_LOOPS)
                    continue;
            }
            k = s->loopN++;
        }
        s->loop[k].num = (u16)num;
        s->loop[k].at = now;
    }
    sPlaying = 0;
    s->sndLast = newest;
}

/* From PortCoopFrameTop, between frames: hand this frame's capture to the
 * page and draw the ghosts the page set. */
void PortCoopFxFrame(void)
{
    u32 now = PortFrameNumber(), room;
    int i;

    /* the capture of the frame that just ran: its sprites, its hazards, and
     * the sounds of the last few frames */
    {
        int ns = 0, len, k;

        while (sSndN > 0 && (u16)(now - sSnd[sSndAt].stamp) >= PORT_COOP_FX_SND_KEEP) {
            sSndAt = (sSndAt + 1) & 31;
            sSndN--;
        }
        ns = sSndN < PORT_COOP_FX_SND_MAX ? sSndN : PORT_COOP_FX_SND_MAX;
        if (sCapN > 0 || sHazN > 0 || ns > 0) {
            Put16(sCap, gKirbys[gLocalPlayerId].base.roomId);
            sCap[2] = (u8)sCapN;
            sCap[3] = (u8)sHazN;
            len = 4 + sCapN * PORT_COOP_FX_REC;
            memcpy(sOut, sCap, (size_t)len);
            memcpy(sOut + len, sHaz, (size_t)(sHazN * PORT_COOP_FX_HAZ_REC));
            len += sHazN * PORT_COOP_FX_HAZ_REC;
            if (ns > 0) {
                sOut[len++] = (u8)ns;
                for (k = sSndN - ns; k < sSndN; k++) {
                    int i = (sSndAt + k) & 31;

                    Put16(sOut + len + 0, sSnd[i].num);
                    Put16(sOut + len + 2, sSnd[i].room);
                    Put16(sOut + len + 4, sSnd[i].src);
                    Put16(sOut + len + 6, sSnd[i].stamp);
                    len += PORT_COOP_FX_SND_REC;
                }
            }
            sOutLen = len;
        } else {
            sOutLen = 0;
        }
    }
    sCapN = 0;
    sHazN = 0;
    memset(sCapPer, 0, sizeof(sCapPer));
    gPortCoopFxOn = sShare;

    if (!PortCoopReady())
        return;
    room = gKirbys[gLocalPlayerId].base.roomId;
    /* sounds first: they do not depend on drawing anything */
    for (i = 0; i < PORT_COOP_FX_SOURCES; i++) {
        struct FxSource *src = &sSrc[i];

        if (src->len >= 4 && now - src->frame <= FX_STALE)
            SndFrame(i, room, now);
        if (src->loopN)
            SndStopLoops(src, now, now - src->frame > SND_QUIET || room != sSndRoom);
    }
    sSndRoom = room;
    sDrawing = 1;
    sAllocN = 0;
    sBytes = 0;
    sUpN = 0;
    for (i = 0; i < PORT_COOP_FX_SOURCES; i++) {
        if (sSrc[i].len < 4 || now - sSrc[i].frame > FX_STALE) {
            memset(sSrc[i].used, 0, sizeof(sSrc[i].used));
            continue;
        }
        DrawSource(i, room);
    }
    /* The tiles are queued and the OAM written: give the VRAM straight back.
     * The picture this frame becomes is composed before any task can queue
     * new tiles into it (the queue drains in order, after the present). */
    for (i = 0; i < sAllocN; i++)
        VramFree(sAlloc[i]);
    sDrawing = 0;
    if (sDemand > sDemandMax) { sDemandMax = sDemand; sDemandMaxN = sDemandN; }
    sDemand = sDemandN = 0;
}

/* --- the page's side ----------------------------------------------------- */

COOP_EXPORT void PortCoopFxShare(int on)
{
    int i;

    sShare = on ? 1 : 0;
    if (!sShare) {
        gPortCoopFxOn = 0;
        sCapN = 0;
        sHazN = 0;
        sSndN = 0;
        sOutLen = 0;
        /* nobody left to send a stop: end what they started here */
        for (i = 0; i < PORT_COOP_FX_SOURCES; i++) {
            if (sSrc[i].loopN)
                SndStopLoops(&sSrc[i], PortFrameNumber(), 1);
            sSrc[i].sndInit = 0;
        }
    }
}

COOP_EXPORT unsigned char *PortCoopFxBuffer(void)
{
    return sScratch;
}

COOP_EXPORT int PortCoopFxTake(unsigned char *out, int max)
{
    int n = sOutLen;

    if (out == NULL || n <= 0 || n > max)
        return 0;
    memcpy(out, sOut, (size_t)n);
    sOutLen = 0;
    sStat[2]++;
    return n;
}

COOP_EXPORT void PortCoopFxSet(int src, const unsigned char *buf, int len)
{
    struct FxSource *s;

    if (src < 0 || src >= PORT_COOP_FX_SOURCES)
        return;
    s = &sSrc[src];
    if (buf == NULL || len < 4 || len > PORT_COOP_FX_BYTES) {
        s->len = 0;
        return;
    }
    if (PortFrameNumber() - s->frame > SND_QUIET)
        s->sndInit = 0;             /* a new stream: none of its backlog */
    memcpy(s->buf, buf, (size_t)len);
    s->len = len;
    s->frame = PortFrameNumber();
}

COOP_EXPORT int PortCoopFxStat(int i)
{
    if (i >= 32 && i < 40)
        return (int)sFail[i - 32];
    if (i >= 48 && i < 80)
        return (int)sFailLog[(i - 48) >> 2][(i - 48) & 3];
    if (i == 40)
        return sRingLow;
    if (i == 41)
        return (int)sDemandMax;
    if (i == 42)
        return (int)sDemandMaxN;
    if (i == 43) return (int)sBigBytes;
    if (i == 44) return (int)sBigAnim;
    if (i == 45) return (int)sBigVar;
    if (i == 46) return (int)sBigFull;
    if (i >= 80 && i < 112)
        return (int)sPalFail[(i - 80) >> 2][(i - 80) & 3];
    return i >= 0 && i < (int)(sizeof(sStat) / sizeof(sStat[0])) ? (int)sStat[i] : 0;
}

#ifdef __cplusplus
}
#endif
