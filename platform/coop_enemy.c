/* Online co-op, stage 3: shared enemies.  The API is in platform/port/coop.h;
 * this is the engine half.
 *
 * --- the model ------------------------------------------------------------
 *
 * Every player still runs every enemy near their own camera; enemies only
 * ever chase the local Kirby (puppets are skipped by the targeting code,
 * coop.c).  Two things are added on top:
 *
 *   one owner per room    the page decides it (the lowest member id among the
 *                         players in the room) and, on the owner, streams the
 *                         room's live enemies -- position, animation, facing,
 *                         hp -- with PortCoopEnemyCollect.  Every other
 *                         player in the room hands those to
 *                         PortCoopEnemySetPoses, and its own copy of each of
 *                         those enemies becomes a *puppet* for as long as the
 *                         stream keeps coming: ObjectMain (object.c) skips
 *                         its state function, gravity, movement and terrain,
 *                         puts it where the owner has it and draws it.  It
 *                         stays solid to attacks and still hurts on contact.
 *                         An enemy the owner does not have (outside the
 *                         owner's camera) is simply run locally.
 *   events                a kill anywhere kills the same enemy everywhere
 *                         (as if defeated: the poof, no item, no score for
 *                         the local player); a hit on a puppet is sent to
 *                         the owner, who takes the hp off its own copy.  The
 *                         hitter's copy runs its own knockback for a few
 *                         frames (the feedback a hit should have) and is then
 *                         put back on the owner's pose.
 *
 * Enemies are matched by room and the index of their record in the room's
 * object list (coop_objects.c), the same on every client.  Objects made at
 * run time (spawner children, projectiles) have no index and stay local.
 * Only the regular enemies (types 0x00..0x37) are puppeted.
 *
 * --- stage 4: minibosses and bosses ----------------------------------------
 *
 * A boss is many objects and a long state machine; drawing one game's boss
 * in another is not attempted.  Instead every player fights their own copy
 * and the copies share one health pool: any hp a copy loses here is sent as
 * a hit, and every other copy takes the same damage through the game's own
 * reaction (PortCoopObjectDamage, object.c: flash, shake, and on 0 hp the
 * game's own death -- explosion, shard, music).  A boss that dies anywhere
 * dies everywhere.  A player who walks in late finds the boss as hurt as the
 * others have made it (the hp is broadcast once a second and taken by a copy
 * nobody has touched yet).  Bosses are matched by room and object-list
 * index, or by room and type when the game spawned them at run time (one
 * boss of a type per room -- the forms of Dark Mind, the Titan head).
 *
 * gPortCoopEnemyOn is 0 unless a puppet table entry is live, so with co-op
 * off the ObjectMain hook is one load and a branch.  Not simulation state:
 * co-op is refused under rollback, the lockstep session and seats.
 */

#include <string.h>

#include "global.h"
#include "data.h"
#include "level.h"
#include "object.h"
#include "kirby.h"
#include "constants/object_types.h"
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

u8 gPortCoopEnemyOn;

#define EN_MAX       64
#define EN_FRESH     4      /* frames a pose keeps an enemy a puppet         */
#define EN_HIT_HOLD  24     /* frames the hitter's copy runs its knockback   */
#define EV_MAX       48
#define DEAD_MAX     64

struct EnemyPuppet {
    u16 room, idx;
    s32 x, y;
    s16 hp;            /* the owner's                                        */
    u8  anim;          /* unk83: the type's animation index                  */
    u8  flags;         /* PORT_COOP_EN_*                                     */
    u32 frame;         /* PortFrameNumber() when last set                    */
    u32 holdUntil;     /* a local hit: no puppeting until this frame         */
    s16 hpSeen;        /* what the puppet's unk80 was set to                 */
    u8  used;
    u8  puppet;        /* was a puppet last frame                            */
};

static struct EnemyPuppet sEn[EN_MAX];
static struct { u8 kind; u8 slot; u16 room, idx; s16 dmg; } sEv[EV_MAX];
static int sEvCount;
static const struct Object *sDead[DEAD_MAX], *sDeadNext[DEAD_MAX];
static int sDeadCount;
static u8 sShare;
static u32 sStats[8];   /* puppet steps, hits out, kills out, kills in, hits in, dropped,
                         * boss hits out, boss hits in */

#define BOSS_MAX 16
struct BossTrack {
    const struct Object *obj;
    u16 room, key;
    s16 lastHp, initHp;
    u32 seen;
    u8  used, dead, touched;
};
static struct BossTrack sBoss[BOSS_MAX];

extern void PortCoopObjectDamage(struct Object *obj, s32 dmg);   /* object.c */

static u32 Now(void) { return PortFrameNumber(); }

static void Put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void Put32(u8 *p, u32 v) { Put16(p, v); Put16(p + 2, v >> 16); }
static u32  Get16(const u8 *p)  { return (u32)p[0] | ((u32)p[1] << 8); }
static u32  Get32(const u8 *p)  { return Get16(p) | (Get16(p + 2) << 16); }

/* A regular enemy, alive (not dying, not being destroyed). */
static int IsEnemy(const struct Object *o)
{
    return o->type <= 0x37;
}

static int IsBoss(const struct Object *o)
{
    return o->type >= 0x38 && o->type <= 0x52;
}

static int Alive(const struct Object *o)
{
    return o->unk80 > 0 && !(o->base.flags & 0x1000) && !(o->base.unkC & 0x2000);
}

static struct EnemyPuppet *Find(u32 room, u32 idx)
{
    int i;

    for (i = 0; i < EN_MAX; i++)
        if (sEn[i].used && sEn[i].room == room && sEn[i].idx == idx)
            return &sEn[i];
    return NULL;
}

static struct EnemyPuppet *FindOrAdd(u32 room, u32 idx)
{
    struct EnemyPuppet *e = Find(room, idx), *old = NULL;
    int i;

    if (e != NULL)
        return e;
    for (i = 0; i < EN_MAX; i++) {
        if (!sEn[i].used) { e = &sEn[i]; break; }
        if (old == NULL || sEn[i].frame < old->frame)
            old = &sEn[i];
    }
    if (e == NULL)
        e = old;
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->room = (u16)room;
    e->idx = (u16)idx;
    e->hpSeen = 0x7FFF;
    return e;
}

static void Event(u32 kind, u32 room, u32 idx, s32 dmg, u32 slot)
{
    if (sEvCount >= EV_MAX) {
        sStats[5]++;
        return;
    }
    sEv[sEvCount].kind = (u8)kind;
    sEv[sEvCount].slot = (u8)slot;
    sEv[sEvCount].room = (u16)room;
    sEv[sEvCount].idx = (u16)idx;
    sEv[sEvCount].dmg = (s16)dmg;
    sEvCount++;
}

static int Dead(const struct Object *o)
{
    int i;

    for (i = 0; i < sDeadCount; i++)
        if (sDead[i] == o)
            return 1;
    return 0;
}

static void MarkDead(const struct Object *o)
{
    if (!Dead(o) && sDeadCount < DEAD_MAX)
        sDead[sDeadCount++] = o;
}

/* --- the ObjectMain hook --------------------------------------------------- */

/* 1: the object is a puppet this frame and its pose has been applied (the
 * caller registers it for collisions and draws it, nothing else). */
int PortCoopEnemyStep(struct Object *obj)
{
    struct EnemyPuppet *e;
    int idx;
    u32 now = Now();

    if (!sShare || !IsEnemy(obj) || obj->base.roomId != gKirbys[gLocalPlayerId].base.roomId)
        return 0;
    idx = PortCoopObjectIndex(obj);
    if (idx < 0 || (e = Find(obj->base.roomId, (u32)idx)) == NULL)
        return 0;
    if (now - e->frame > EN_FRESH || !Alive(obj) || now < e->holdUntil) {
        e->puppet = 0;
        return 0;
    }
    /* A hit landed on the puppet since last frame (the collision task runs
     * after the objects): tell the owner, and let this copy play the
     * knockback the hit set up before it is put back on the owner's pose. */
    if (e->puppet && obj->unk80 < e->hpSeen) {
        Event(PORT_COOP_EV_HIT, e->room, e->idx, e->hpSeen - obj->unk80, 0);
        sStats[1]++;
        e->holdUntil = now + EN_HIT_HOLD;
        e->puppet = 0;
        e->hpSeen = obj->unk80;
        return 0;
    }
    sStats[0]++;
    obj->base.unk48 = obj->base.x;
    obj->base.unk4C = obj->base.y;
    obj->base.x = e->x;
    obj->base.y = e->y;
    obj->base.xspeed = obj->base.yspeed = 0;
    obj->unk98 = obj->unk9A = 0;
    obj->unk83 = e->anim;
    if (e->flags & PORT_COOP_EN_LEFT)
        obj->base.flags |= 1;
    else
        obj->base.flags &= ~1;
    if (e->flags & PORT_COOP_EN_HIDDEN)
        obj->base.flags |= 0x400;
    else
        obj->base.flags &= ~0x400;
    if (e->hp > 0 && e->hp < obj->unk80)
        obj->unk80 = e->hp;
    e->hpSeen = obj->unk80;
    e->puppet = 1;
    return 1;
}

/* coop_fx.c: is this copy of a shared enemy a puppet (its children are then
 * the owner's to report), and is the copy at (room, idx) one here (a ghost
 * of the owner's children is drawn only over a puppet, never beside a copy
 * that spawns its own)? */
int PortCoopEnemyIsPuppet(const struct Object *obj)
{
    struct EnemyPuppet *e;
    int idx;

    if (!sShare || !IsEnemy(obj))
        return 0;
    idx = PortCoopObjectIndex(obj);
    return idx >= 0 && (e = Find(obj->base.roomId, (u32)idx)) != NULL && e->puppet
        && Now() - e->frame <= EN_FRESH;
}

int PortCoopEnemyPuppetAt(u32 room, u32 idx)
{
    struct EnemyPuppet *e;

    if (!sShare || (e = Find(room, idx)) == NULL)
        return 0;
    return e->puppet && Now() - e->frame <= EN_FRESH;
}

/* --- per frame, from PortCoopFrameTop ---------------------------------------
 * Kills: any regular enemy, puppet or not, whose hp went to 0 here since the
 * last frame (a hit, an inhale) and that was not killed by an event. */
static int ScanKills(struct Object *o, void *arg)
{
    int idx;

    (void)arg;
    if (!IsEnemy(o) || o->base.roomId != gKirbys[gLocalPlayerId].base.roomId)
        return 0;
    if (o->unk80 > 0 && !(o->base.unkC & 0x2000))
        return 0;
    if (!Dead(o)) {
        idx = PortCoopObjectIndex(o);
        if (idx >= 0) {
            Event(PORT_COOP_EV_KILL, o->base.roomId, (u32)idx, 0, 0);
            sStats[2]++;
        }
    }
    {
        int i, have = 0;

        for (i = 0; i < DEAD_MAX && sDeadNext[i] != NULL; i++)
            if (sDeadNext[i] == o) { have = 1; break; }
        if (!have && i < DEAD_MAX)
            sDeadNext[i] = o;
    }
    return 0;
}

/* --- minibosses and bosses ----------------------------------------------- */

static u32 BossKey(const struct Object *o)
{
    int idx = PortCoopObjectIndex(o);

    return idx >= 0 ? (u32)idx : 0x8000u | o->type;
}

static struct BossTrack *BossFind(const struct Object *o, u32 key)
{
    int i;

    for (i = 0; i < BOSS_MAX; i++)
        if (sBoss[i].used && sBoss[i].obj == o && sBoss[i].key == key)
            return &sBoss[i];
    return NULL;
}

static int ScanBoss(struct Object *o, void *arg)
{
    struct BossTrack *t;
    u32 key, *now = (u32 *)arg;
    int i;

    if (!IsBoss(o) || o->base.roomId != gKirbys[gLocalPlayerId].base.roomId || (o->base.flags & 0x1000))
        return 0;
    key = BossKey(o);
    t = BossFind(o, key);
    if (t == NULL) {
        for (i = 0; i < BOSS_MAX && sBoss[i].used; i++)
            ;
        if (i == BOSS_MAX)
            return 0;
        t = &sBoss[i];
        memset(t, 0, sizeof(*t));
        t->used = 1;
        t->obj = o;
        t->room = o->base.roomId;
        t->key = (u16)key;
        t->lastHp = t->initHp = o->unk80;
        t->dead = o->unk80 <= 0;
    }
    t->seen = *now;
    if (o->unk80 < t->lastHp) {
        Event(PORT_COOP_EV_BOSS_HIT, t->room, t->key, t->lastHp - o->unk80, 0);
        sStats[6]++;
        t->touched = 1;
    }
    t->lastHp = o->unk80;
    if (o->unk80 <= 0 && !t->dead) {
        Event(PORT_COOP_EV_BOSS_KILL, t->room, t->key, 0, 0);
        t->dead = 1;
    }
    /* once a second: the hp, for a player who walks in late */
    if (!t->dead && t->touched && *now % 60 == t->key % 60)
        Event(PORT_COOP_EV_BOSS_HP, t->room, t->key, o->unk80, 0);
    return 0;
}

static void BossFrame(u32 now)
{
    int i;

    PortCoopForEachObject(ScanBoss, &now);
    for (i = 0; i < BOSS_MAX; i++)
        if (sBoss[i].used && now - sBoss[i].seen > 2)
            sBoss[i].used = 0;
}

struct BossMatch { u32 room, key; struct Object *obj; };

static int MatchBoss(struct Object *o, void *arg)
{
    struct BossMatch *m = (struct BossMatch *)arg;

    if (!IsBoss(o) || o->base.roomId != m->room || o->unk80 <= 0 || (o->base.flags & 0x1000))
        return 0;
    if (BossKey(o) != m->key)
        return 0;
    m->obj = o;
    return 1;
}

static void BossEvent(int kind, u32 room, u32 key, s32 val)
{
    struct BossMatch m;
    struct BossTrack *t;

    m.room = room;
    m.key = key;
    m.obj = NULL;
    PortCoopForEachObject(MatchBoss, &m);
    if (m.obj == NULL)
        return;
    t = BossFind(m.obj, key);
    if (kind == PORT_COOP_EV_BOSS_HP) {
        /* only a copy nobody here has touched: a late arrival */
        if (t != NULL && !t->touched && val > 0 && val < m.obj->unk80 && m.obj->unk80 == t->initHp) {
            m.obj->unk80 = (s16)val;
            t->lastHp = (s16)val;
            t->touched = 1;
        }
        return;
    }
    if (kind == PORT_COOP_EV_BOSS_KILL)
        val = m.obj->unk80;
    PortCoopObjectDamage(m.obj, val);
    sStats[7]++;
    if (t != NULL) {
        t->lastHp = m.obj->unk80;
        t->touched = 1;
        if (m.obj->unk80 <= 0)
            t->dead = 1;        /* died of someone else's hit: theirs to tell */
    }
}

void PortCoopEnemyFrame(void)
{
    int i, any = 0;
    u32 now = Now();

    if (!sShare || !PortCoopReady()) {
        gPortCoopEnemyOn = 0;
        sDeadCount = 0;
        return;
    }
    BossFrame(now);
    memset(sDeadNext, 0, sizeof(sDeadNext));
    PortCoopForEachObject(ScanKills, NULL);
    for (sDeadCount = 0; sDeadCount < DEAD_MAX && sDeadNext[sDeadCount] != NULL; sDeadCount++)
        sDead[sDeadCount] = sDeadNext[sDeadCount];
    for (i = 0; i < EN_MAX; i++) {
        if (!sEn[i].used)
            continue;
        if (now - sEn[i].frame > 600)
            sEn[i].used = 0;            /* forgotten after 10 s */
        else if (now - sEn[i].frame <= EN_FRESH || now < sEn[i].holdUntil)
            any = 1;
    }
    gPortCoopEnemyOn = (u8)any;
}

/* --- exported -------------------------------------------------------------- */

COOP_EXPORT void PortCoopEnemyShare(int on)
{
    if (!!on != !!sShare)
        PortLog("[katam-port] coop: shared enemies %s", on ? "on" : "off");
    sShare = on ? 1 : 0;
    if (!on) {
        memset(sEn, 0, sizeof(sEn));
        gPortCoopEnemyOn = 0;
        sEvCount = 0;
    }
}

struct Collect { u8 *out; int max, n; };

static int CollectOne(struct Object *o, void *arg)
{
    struct Collect *c = (struct Collect *)arg;
    struct EnemyPuppet *e;
    int idx;
    u8 *p, f = 0;

    if (c->n >= c->max)
        return 1;
    if (!IsEnemy(o) || !Alive(o) || o->base.roomId != gKirbys[gLocalPlayerId].base.roomId)
        return 0;
    idx = PortCoopObjectIndex(o);
    if (idx < 0)
        return 0;
    e = Find(o->base.roomId, (u32)idx);
    if (e != NULL && e->puppet)
        return 0;                       /* someone else's: not ours to send */
    if (o->base.flags & 1)     f |= PORT_COOP_EN_LEFT;
    if (o->base.flags & 0x400) f |= PORT_COOP_EN_HIDDEN;
    p = c->out + 4 + c->n * PORT_COOP_EN_REC;
    Put16(p + 0, (u32)idx);
    Put32(p + 2, (u32)o->base.x);
    Put32(p + 6, (u32)o->base.y);
    Put16(p + 10, (u32)(u16)o->unk80);
    p[12] = o->unk83;
    p[13] = f;
    c->n++;
    return 0;
}

/* The owner's snapshot of the local room's live regular enemies:
 * u16 room, u16 n, then n records of PORT_COOP_EN_REC bytes (coop.h).
 * Returns the length in bytes, 0 when there is nothing to send. */
COOP_EXPORT int PortCoopEnemyCollect(unsigned char *out, int maxBytes)
{
    struct Collect c;

    if (out == NULL || !sShare || !PortCoopReady() || maxBytes < 4)
        return 0;
    c.out = out;
    c.max = (maxBytes - 4) / PORT_COOP_EN_REC;
    c.n = 0;
    PortCoopForEachObject(CollectOne, &c);
    Put16(out, gKirbys[gLocalPlayerId].base.roomId);
    Put16(out + 2, (u32)c.n);
    return 4 + c.n * PORT_COOP_EN_REC;
}

/* Poses from the room's owner, in PortCoopEnemyCollect's format; each keeps
 * its enemy a puppet for EN_FRESH frames. */
COOP_EXPORT void PortCoopEnemySetPoses(const unsigned char *buf, int len)
{
    u32 room, n, i;
    u32 now = Now();

    if (buf == NULL || len < 4 || !sShare)
        return;
    room = Get16(buf);
    n = Get16(buf + 2);
    if ((u32)len < 4 + n * PORT_COOP_EN_REC)
        return;
    for (i = 0; i < n; i++) {
        const u8 *p = buf + 4 + i * PORT_COOP_EN_REC;
        struct EnemyPuppet *e = FindOrAdd(room, Get16(p));

        e->x = (s32)Get32(p + 2);
        e->y = (s32)Get32(p + 6);
        e->hp = (s16)Get16(p + 10);
        e->anim = p[12];
        e->flags = p[13];
        e->frame = now;
    }
    if (n)
        gPortCoopEnemyOn = 1;
}

/* Events that happened here since the last call: n records of 8 bytes
 * (u8 kind, u8 0, u16 room, u16 idx, s16 dmg).  Returns n. */
COOP_EXPORT int PortCoopEnemyTake(unsigned char *out, int max)
{
    int i, n = sEvCount < max ? sEvCount : max;

    if (out == NULL)
        return 0;
    for (i = 0; i < n; i++) {
        u8 *p = out + i * 8;

        p[0] = sEv[i].kind;
        p[1] = 0;
        Put16(p + 2, sEv[i].room);
        Put16(p + 4, sEv[i].idx);
        Put16(p + 6, (u32)(u16)sEv[i].dmg);
    }
    sEvCount = 0;
    return n;
}

struct Match { u32 room, idx; struct Object *obj; };

static int MatchOne(struct Object *o, void *arg)
{
    struct Match *m = (struct Match *)arg;

    if (o->base.roomId != m->room || !Alive(o))
        return 0;
    if ((u32)PortCoopObjectIndex(o) != m->idx)
        return 0;
    m->obj = o;
    return 1;
}

/* An event from another player (from: their puppet slot, 1..3, or 0). */
COOP_EXPORT void PortCoopEnemyEvent(int kind, int room, int idx, int dmg, int from)
{
    struct Match m;
    struct EnemyPuppet *e;

    if (!sShare || !PortCoopReady())
        return;
    if (kind == PORT_COOP_EV_BOSS_HIT || kind == PORT_COOP_EV_BOSS_KILL || kind == PORT_COOP_EV_BOSS_HP) {
        BossEvent(kind, (u32)room, (u32)idx, dmg);
        return;
    }
    m.room = (u32)room;
    m.idx = (u32)idx;
    m.obj = NULL;
    PortCoopForEachObject(MatchOne, &m);
    if (m.obj == NULL)
        return;
    if (kind == PORT_COOP_EV_KILL) {
        /* as if defeated: dark_mind.c kills its summons the same way */
        m.obj->unk80 = 0;
        m.obj->unk86 = (u8)(from >= 1 && from <= 3 ? from : 1);
        m.obj->base.unkC |= 0x2000;
        MarkDead(m.obj);
        if ((e = Find((u32)room, (u32)idx)) != NULL)
            e->puppet = 0;
        sStats[3]++;
    } else if (kind == PORT_COOP_EV_HIT && dmg > 0) {
        e = Find((u32)room, (u32)idx);
        if (e != NULL && e->puppet)
            return;                     /* not ours: the owner takes it */
        m.obj->unk80 = (s16)(m.obj->unk80 - dmg);
        m.obj->unk86 = (u8)(from >= 1 && from <= 3 ? from : 1);
        if (m.obj->unk80 <= 0) {
            m.obj->unk80 = 0;
            m.obj->base.unkC |= 0x2000;
            /* not marked dead: the scan reports this kill to everyone */
        }
        sStats[4]++;
    }
}

/* Diagnostics and tests: every regular enemy in the local room, puppets
 * included, and minibosses and bosses: n records of 20 bytes (u16 idx
 * (0xFFFF: none), i32 x, i32 y,
 * s16 hp, u8 anim, u8 flags (bit 7: a puppet this frame, bit 6: dying),
 * u32 address of the struct Object, u16 type).  Returns n. */
struct List { u8 *out; int max, n; };

static int ListOne(struct Object *o, void *arg)
{
    struct List *l = (struct List *)arg;
    struct EnemyPuppet *e;
    int idx;
    u8 *p, f = 0;

    if (l->n >= l->max)
        return 1;
    if (!(IsEnemy(o) || IsBoss(o)) || o->base.roomId != gKirbys[gLocalPlayerId].base.roomId)
        return 0;
    idx = PortCoopObjectIndex(o);
    e = idx >= 0 && IsEnemy(o) ? Find(o->base.roomId, (u32)idx) : NULL;
    if (e != NULL && e->puppet)  f |= 0x80;
    if (!Alive(o))               f |= 0x40;
    if (o->base.flags & 1)       f |= PORT_COOP_EN_LEFT;
    p = l->out + l->n * 20;
    Put16(p + 0, idx >= 0 ? (u32)idx : 0xFFFF);
    Put32(p + 2, (u32)o->base.x);
    Put32(p + 6, (u32)o->base.y);
    Put16(p + 10, (u32)(u16)o->unk80);
    p[12] = o->unk83;
    p[13] = f;
    Put32(p + 14, (u32)(uintptr_t)o);
    Put16(p + 18, o->type);
    l->n++;
    return 0;
}

COOP_EXPORT int PortCoopEnemyList(unsigned char *out, int max)
{
    struct List l;

    if (out == NULL || gUnk_02023354 == NULL)
        return 0;
    l.out = out;
    l.max = max;
    l.n = 0;
    PortCoopForEachObject(ListOne, &l);
    return l.n;
}

/* Diagnostics: puppet steps, hits sent, kills sent, kills received, hits
 * received, events dropped. */
COOP_EXPORT int PortCoopEnemyStat(int i)
{
    return i >= 0 && i < 8 ? (int)sStats[i] : 0;
}

#ifdef __cplusplus
}
#endif
