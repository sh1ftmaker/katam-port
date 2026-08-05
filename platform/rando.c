/* The enemy and boss randomizer.
 *
 * Every enemy, mid-boss and boss in the game is a *record in a level's object
 * list*: 36 bytes in ROM holding a position, a room, some parameters and one
 * byte of type.  The game turns that byte into a constructor by indexing
 * gSpawnFuncTable1, and the constructor gets the whole record.  Change the
 * byte and a different enemy is built in the same place, from the same
 * record, by the game's own code.  That is the entire mechanism.
 *
 * What makes it cheap here rather than a mod-loader's worth of work is that
 * KATAM allocates an object's graphics when the object is *created*, not when
 * the room is loaded: sub_0809A630 calls VramMalloc(gUnk_08351648[type]
 * .numTiles) and sub_0803DFAC loads the palette on demand.  So a substituted
 * enemy brings its own tiles and its own palette with it and there is no
 * per-room tileset to keep in step.  Nothing else has to change.
 *
 * --- where the substitution happens ---------------------------------------
 *
 * Not at the constructor call.  There are three of those (the room-entry
 * rescan in CreateObject, and the two scroll-in spawns in sub_0800A2B4 and
 * sub_0800A460), they all take `const struct Object *`, and -- this is the
 * part that decides the design -- the constructor *keeps* that pointer.
 * struct Object2 has an `object` back-pointer and sub_0809A630 dereferences
 * it every frame the object is alive.  So a hook at the call site would have
 * to hand out a rewritten record that outlives the call, which means owning a
 * pool of them and knowing when each one dies.
 *
 * The list itself has that lifetime already.  FillLevelInfo sets
 *
 *     levelInfo->objlistPtr = gLevelObjLists[...].obj;
 *
 * exactly once per room load, and every walk of the room's objects starts
 * from there.  So the whole list is copied and rewritten at that one point,
 * one buffer per player, and every consumer downstream agrees about what is
 * in the room without knowing this file exists.  Records live exactly as long
 * as the room does, which is exactly as long as the objects made from them.
 *
 * --- what it will and will not swap ---------------------------------------
 *
 * The destination pool is *the set of types the retail game actually places*,
 * measured by walking all 287 object lists in the ROM.  A type that ships in
 * a level is known to work as a level-placed object; a type that does not is
 * an unknown, and several of the unplaced ones in the enemy range are aliases
 * or fragments (OBJ_EMPTY_2C builds a Droppy, OBJ_EMPTY_31 a Cookin).  This
 * is a stronger guarantee than any reading of the constructors would give.
 *
 * Bosses and mid-bosses are separate pools and separate switches, because
 * they are the ones that can cost a player their run: a boss room is a door
 * that opens when the boss dies, and although every boss in the pool is
 * defeated the same way -- the room's own trigger watches the object, not the
 * species -- an arena built for Kracko is not built for King Golem.  Default
 * off; enemies default on.
 *
 * Excluded from both ends, with reasons:
 *
 *   OBJ_SHADOW_KIRBY       six scripted story encounters, not an enemy
 *                          placement.  Removing one removes the scene;
 *                          putting one in a corridor starts the scene there.
 *   OBJ_DARK_META_KNIGHT   the endgame sequence, and 0x51 is a *trigger*
 *   OBJ_DARK_MIND_FORM_*   rather than a boss.  Nothing good is downstream
 *   OBJ_DARK_MIND_BOMB     of randomising the last fight in the game.
 *   OBJ_TITAN_HEAD         a component of Mega Titan, placed by the fight
 *   OBJ_CRAZY_HAND_2       a variant spawned by its own boss
 *
 * --- the two things that can go wrong, and what is done about them --------
 *
 * **Video memory.**  The tile budget is a heap and VramMalloc's answer to a
 * full one is to return ewram_end -- a real address, so the game writes tiles
 * into the end of EWRAM and carries on looking healthy.  Enemy tile counts
 * run from 4 (Minny) to 90 (Sparky), so a blind shuffle can put four Sparkys
 * in a room built for four Waddle Dees and quietly corrupt memory.  So the
 * default swaps within a size class taken from the ROM's own numTiles, which
 * keeps a room's tile demand close to what it was built for.
 * PORT_RANDO_ANY_SIZE turns that off and is honest about being the risky one.
 *
 * **Abilities.**  Some rooms want an ability that only one enemy carries, and
 * a shuffle that replaces the only Sword Knight with a Waddle Dee can leave a
 * player without the tool a later block needs.  gUnk_08351648[type]
 * .kirbyAbility says which enemies carry one, so by default an
 * ability-carrying enemy is replaced by another ability-carrying enemy.  It
 * does not preserve *which* ability -- that would pin most enemies to
 * themselves, since most abilities have exactly one source -- so this is not
 * a logic guarantee.  It is the difference between "a different ability is
 * here" and "no ability is here", which is where the soft locks live.
 *
 * --- determinism -----------------------------------------------------------
 *
 * The mapping is a pure function of (seed, room, index-in-list) and reads
 * nothing that changes while the game runs -- not gRngVal, not the frame
 * counter, not which player triggered the load.  Four players in a shared
 * room therefore compute the same list, and so would two units on a link
 * cable given the same seed.  Nothing here sends the seed anywhere; agreeing
 * on it is a transport's problem.  See docs/RANDO.md.
 */

#include <stddef.h>

#include "port/port.h"
#include "gba/gba.h"
#include "data.h"
#include "constants/object_types.h"

/* --- the pools -----------------------------------------------------------
 *
 * Every entry is a type the retail ROM places in at least one level.  The
 * counts are placements across all 287 object lists, measured from the ROM,
 * and the tile counts are gUnk_08351648[type].numTiles.  Both are here so
 * that a reader can see what a swap costs without going and looking. */

/* Common enemies.  49 of the 51 placed types in 0x00..0x37; the two left out
 * are Shadow Kirby (scripted) and the two Snooter/Scarfy aliases are simply
 * never placed and so were never candidates. */
static const u8 sEnemyPool[] = {
    OBJ_WADDLE_DEE_1,       /* 156 placements,  9 tiles */
    OBJ_BRONTO_BURT,        /* 118,             9 */
    OBJ_BLIPPER,            /*  86,             9 */
    OBJ_GLUNK,              /*  62,             9 */
    OBJ_SQUISHY,            /*   1,             9 */
    OBJ_SCARFY,             /* 110,            16 */
    OBJ_GORDO,              /* 112,            14 -- invincible by design */
    OBJ_SNOOTER_1,          /*  58,            12 */
    OBJ_CHIP,               /*  66,            12 */
    OBJ_SOARAR,             /*  28,            12 */
    OBJ_HALEY,              /*  37,            12 */
    OBJ_ROLYPOLY,           /*  78,             9 */
    OBJ_CUPIE,              /*  13,            20 */
    OBJ_BLOCKIN,            /*  25,            12 */
    OBJ_LEAP,               /*  29,            16 */
    OBJ_JACK,               /*   3,            16 */
    OBJ_BIG_WADDLE_DEE,     /*  24,            25 */
    OBJ_WADDLE_DOO,         /*  98,             9 */
    OBJ_FLAMER,             /*  79,            15 */
    OBJ_HOT_HEAD,           /*  27,             9 */
    OBJ_LASER_BALL,         /*  15,            12 */
    OBJ_PENGY,              /*  25,             9 */
    OBJ_ROCKY,              /*  19,             9 */
    OBJ_SIR_KIBBLE,         /*  55,            12 */
    OBJ_SPARKY,             /*  54,            90 -- by far the largest */
    OBJ_SWORD_KNIGHT,       /*  57,            28 */
    OBJ_UFO,                /*   7,            12 */
    OBJ_TWISTER,            /*  13,            12 */
    OBJ_WHEELIE,            /*  24,             9 */
    OBJ_NODDY,              /*  26,             9 */
    OBJ_GOLEM_1,            /*   5,            56 */
    OBJ_GOLEM_2,            /*   4,            56 */
    OBJ_GOLEM_3,            /*  10,            56 */
    OBJ_FOLEY_1,            /*  69,            16 */
    OBJ_SHOOTY,             /*  24,            12 */
    OBJ_BOXIN,              /*  27,            12 */
    OBJ_COOKIN,             /*   7,            35 */
    OBJ_MINNY,              /*   5,             4 -- the smallest */
    OBJ_BOMBER,             /*  10,             9 */
    OBJ_HEAVY_KNIGHT,       /*  18,            60 */
    OBJ_GIANT_ROCKY,        /*  15,            30 */
    OBJ_METAL_GUARDIAN,     /*   6,            30 */
    OBJ_BATTY,              /*  40,            12 */
    OBJ_BANG_BANG,          /*  14,            16 */
    OBJ_DROPPY,             /*  15,            16 */
    OBJ_PRANK,              /*   5,            36 */
    OBJ_MIRRA,              /*   9,            16 */
    OBJ_SHOTZO_1,           /*  47,            12 -- stationary, invincible */
    OBJ_WADDLE_DEE_2,       /*   1,             9 */
};

/* Mid-bosses.  All eight placed types in 0x38..0x3F, 36 to 112 tiles. */
static const u8 sMidBossPool[] = {
    OBJ_MR_FROSTY, OBJ_BONKERS, OBJ_PHAN_PHAN, OBJ_BATAFIRE,
    OBJ_BOX_BOXER, OBJ_BOXY, OBJ_MASTER_HAND, OBJ_BOMBAR,
};

/* Bosses.  One placement each, one room each.  Kracko's numTiles is 0 --
 * it manages its own graphics rather than taking a VRAM allocation -- so the
 * size classing below has nothing to say about it and it swaps freely. */
static const u8 sBossPool[] = {
    OBJ_KRACKO, OBJ_KING_GOLEM, OBJ_CRAZY_HAND_1, OBJ_GOBBLER,
    OBJ_WIZ, OBJ_MOLEY, OBJ_MEGA_TITAN,
};

#define ARRAY_LEN(a) ((s32)(sizeof(a) / sizeof((a)[0])))

/* Which pool a type belongs to, or none.  A type outside every pool is left
 * exactly as the level data has it -- doors, chests, buttons, blocks, items,
 * spawners, the Dark Mind sequence, all of it. */
#define POOL_NONE     0
#define POOL_ENEMY    1
#define POOL_MIDBOSS  2
#define POOL_BOSS     3

static int PoolOf(u8 type)
{
    s32 i;
    for (i = 0; i < ARRAY_LEN(sEnemyPool); i++)
        if (sEnemyPool[i] == type) return POOL_ENEMY;
    for (i = 0; i < ARRAY_LEN(sMidBossPool); i++)
        if (sMidBossPool[i] == type) return POOL_MIDBOSS;
    for (i = 0; i < ARRAY_LEN(sBossPool); i++)
        if (sBossPool[i] == type) return POOL_BOSS;
    return POOL_NONE;
}

static const u8 *PoolTable(int pool, s32 *countOut)
{
    switch (pool) {
    case POOL_ENEMY:   *countOut = ARRAY_LEN(sEnemyPool);   return sEnemyPool;
    case POOL_MIDBOSS: *countOut = ARRAY_LEN(sMidBossPool); return sMidBossPool;
    case POOL_BOSS:    *countOut = ARRAY_LEN(sBossPool);    return sBossPool;
    default:           *countOut = 0;                       return NULL;
    }
}

/* --- the settings --------------------------------------------------------- */

u32 gPortRandoFlags = 0;        /* 0 disables everything in this file */
u32 gPortRandoSeed = 0;

/* --- what the ROM says about a type --------------------------------------
 *
 * Read live rather than tabulated, because the tables above are a list of
 * *names* and these are numbers that belong to the player's own ROM.  The
 * descriptor table is mapped at its true address like the rest of it. */
static u32 TilesOf(u8 type)   { return gUnk_08351648[type].numTiles; }
static u32 AbilityOf(u8 type) { return gUnk_08351648[type].kirbyAbility; }

/* Four size classes, cut at the gaps in the enemy pool's own distribution:
 * 4/9/12, then 14..25, then 28..36, then 56..90.  A class is not a budget --
 * it is a promise not to make the demand much worse than the level's author
 * made it, which is what stops a room overflowing a heap that it did not
 * overflow before.
 *
 * Four rather than three because three was measured and was not good enough.
 * A single class of "28 and up" contains both Sword Knight at 28 tiles and
 * Sparky at 90, so one swap could add 62 tiles to a room -- and the object
 * heap in a level is 0x200 tiles total (sub_080332BC), shared with Kirby, the
 * HUD and every effect.  tools/rando_preview.py measured the worst room in
 * the game going from 40 tiles to 102 on that classing.  Splitting at 36
 * caps the worst single swap at +34 and the worst room in the game at +51
 * over what it shipped with.  Those numbers are from that tool and can be
 * re-measured; docs/RANDO.md has the rest of them. */
static int SizeClass(u8 type)
{
    u32 tiles = TilesOf(type);

    if (tiles == 0xFFFFFFFF)   /* the "no allocation" marker; see sub_0803DD58 */
        return 0;
    if (tiles <= 12) return 0;
    if (tiles <= 25) return 1;
    if (tiles <= 36) return 2;
    return 3;
}

/* --- the roll -------------------------------------------------------------
 *
 * One 32-bit mix, and nothing that changes while the game runs goes into it.
 * The room id and the record's index in the room's list identify a placement
 * uniquely and permanently: the same seed gives the same level every time it
 * is loaded, in every player's copy of it, in this session and the next.
 * That is what makes the result something a player can share. */
static u32 Mix(u32 x)
{
    /* A 32-bit finalizer.  Any decent avalanche does; this one is here
     * because it is short and has no magic beyond the constants. */
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

/* A fresh type for one placement, or the original if nothing fits.
 *
 * The candidate filter runs twice: once counting, once picking.  Two passes
 * over at most 49 entries is nothing, and it avoids either building a
 * scratch array or biasing the choice by rejection-sampling. */
static u8 RollType(u8 type, int pool, u16 roomId, s32 index)
{
    const u8 *table;
    s32 count, i, matches = 0, pick;
    u32 r;
    int wantClass = SizeClass(type);
    int wantAbility = (AbilityOf(type) != 0);

    table = PoolTable(pool, &count);
    if (!table)
        return type;

    for (i = 0; i < count; i++) {
        u8 cand = table[i];

        if (!(gPortRandoFlags & PORT_RANDO_ANY_SIZE)
            && SizeClass(cand) != wantClass)
            continue;
        if ((gPortRandoFlags & PORT_RANDO_KEEP_ABILITY)
            && (AbilityOf(cand) != 0) != wantAbility)
            continue;
        matches++;
    }
    if (matches == 0)
        return type;   /* nothing in the pool fits: leave the level alone */

    r = Mix(gPortRandoSeed ^ Mix((u32)roomId * 0x9E3779B9U + (u32)index));
    pick = (s32)(r % (u32)matches);

    for (i = 0; i < count; i++) {
        u8 cand = table[i];

        if (!(gPortRandoFlags & PORT_RANDO_ANY_SIZE)
            && SizeClass(cand) != wantClass)
            continue;
        if ((gPortRandoFlags & PORT_RANDO_KEEP_ABILITY)
            && (AbilityOf(cand) != 0) != wantAbility)
            continue;
        if (pick-- == 0)
            return cand;
    }
    return type;       /* unreachable: the two passes agree by construction */
}

/* --- the rewritten lists --------------------------------------------------
 *
 * Two buffers per player, used alternately.  One would be enough if a room
 * load were guaranteed to have destroyed every object made from the previous
 * room's records first -- it is, as far as anything here can tell -- but the
 * objects hold pointers into these bytes and the cost of being wrong is a
 * read through a pointer to a record that has become a different record.  The
 * second buffer makes that stale read return the *previous room's* data,
 * which is inert, instead of a live record for something else.  Nine
 * kilobytes for a whole class of bug not to exist.
 *
 * The size: the largest object list in the ROM is 30 records of 36 bytes.
 * PORT_RANDO_LIST_BYTES is that with room to spare, and a list that does not
 * fit is passed through untouched and reported -- a room the randomizer
 * declines to touch is a room that plays as it shipped, which is a fine
 * failure. */
#define PORT_RANDO_LIST_BYTES 2048
#define PORT_RANDO_SLOTS      4      /* one per player */

static u8 sListBuf[PORT_RANDO_SLOTS][2][PORT_RANDO_LIST_BYTES];
static struct LevelInfo_1E4 sListHdr[PORT_RANDO_SLOTS][2];
static u8 sListParity[PORT_RANDO_SLOTS];
static u8 sReportedOverflow;

/* Diagnostics.  Counted rather than logged per swap: a level load rewrites a
 * couple of dozen records and a line each would bury everything else the port
 * has to say. */
static u32 sRoomsSeen, sSwapsMade;

void PortSetRando(u32 flags, u32 seed)
{
    gPortRandoFlags = flags;
    gPortRandoSeed = seed;
    sRoomsSeen = sSwapsMade = 0;
    if (!(flags & (PORT_RANDO_ENEMIES | PORT_RANDO_MIDBOSSES
                   | PORT_RANDO_BOSSES))) {
        /* The rule bits on their own select nothing to apply them to.  Say
         * "off" rather than printing a setting, so the console does not read
         * as the randomizer being on. */
        PortLog("rando: off");
        return;
    }
    PortLog("rando: flags=0x%x seed=%u (%s%s%s%s%s)", flags, seed,
            (flags & PORT_RANDO_ENEMIES)     ? "enemies "   : "",
            (flags & PORT_RANDO_MIDBOSSES)   ? "midbosses " : "",
            (flags & PORT_RANDO_BOSSES)      ? "bosses "    : "",
            (flags & PORT_RANDO_ANY_SIZE)    ? "any-size "  : "size-matched ",
            (flags & PORT_RANDO_KEEP_ABILITY) ? "keep-ability" : "free-ability");
    /* A room already loaded keeps the list it was given; the change lands on
     * the next door.  Saying so beats a player deciding it does not work. */
}

void PortRandoReport(void)
{
    PortLog("rando: flags=0x%x seed=%u, %u room load(s), %u object(s) swapped",
            gPortRandoFlags, gPortRandoSeed, sRoomsSeen, sSwapsMade);
}

/* Is this pool switched on? */
static int PoolEnabled(int pool)
{
    switch (pool) {
    case POOL_ENEMY:   return (gPortRandoFlags & PORT_RANDO_ENEMIES)   != 0;
    case POOL_MIDBOSS: return (gPortRandoFlags & PORT_RANDO_MIDBOSSES) != 0;
    case POOL_BOSS:    return (gPortRandoFlags & PORT_RANDO_BOSSES)    != 0;
    default:           return 0;
    }
}

/* The one hook.  Called from FillLevelInfo in place of the assignment it
 * replaced; returns `list` unchanged whenever there is nothing to do, which
 * includes every case where this file is switched off. */
const struct LevelInfo_1E4 *PortRandoObjList(u8 playerId, u16 roomId,
                                             const struct LevelInfo_1E4 *list)
{
    const u8 *src;
    u8 *dst;
    s32 used = 0, index = 0;
    u32 mask = PORT_RANDO_ENEMIES | PORT_RANDO_MIDBOSSES | PORT_RANDO_BOSSES;
    int slot, parity;

    if (!(gPortRandoFlags & mask) || list == NULL || list->unk0 == NULL)
        return list;
    if (playerId >= PORT_RANDO_SLOTS)
        return list;

    /* Measure first.  A list that does not fit is not half-copied: a room
     * with its first twenty enemies randomised and the rest missing is far
     * worse than a room that shipped. */
    src = (const u8 *)list->unk0;
    while (src[1] != 0) {
        used += src[1];
        if (used + 36 > PORT_RANDO_LIST_BYTES) {
            if (!sReportedOverflow) {
                sReportedOverflow = 1;
                PortLog("rando: room %u has an object list over %d bytes; "
                        "left as it shipped", roomId, PORT_RANDO_LIST_BYTES);
            }
            return list;
        }
        src += src[1];
    }

    slot = playerId;
    parity = sListParity[slot] ^= 1;
    dst = sListBuf[slot][parity];

    src = (const u8 *)list->unk0;
    while (src[1] != 0) {
        u8 stride = src[1];
        s32 i;

        for (i = 0; i < stride; i++)
            dst[i] = src[i];

        /* offsetof(struct Object, type) is 12 and is frozen by
         * platform/port/gba_layout.h, so the copy is addressed as bytes and
         * only this one is interpreted. */
        {
            u8 type = dst[12];
            int pool = PoolOf(type);

            if (PoolEnabled(pool)) {
                u8 rolled = RollType(type, pool, roomId, index);
                if (rolled != type) {
                    dst[12] = rolled;
                    sSwapsMade++;
                }
            }
        }

        src += stride;
        dst += stride;
        index++;
    }
    /* The terminator.  The walk stops on a zero stride and reads nothing
     * else from it, so a zeroed record is a faithful end of list. */
    {
        s32 i;
        for (i = 0; i < 36; i++)
            dst[i] = 0;
    }

    sListHdr[slot][parity] = *list;      /* unk4/unk8/unkC/unkD unchanged */
    sListHdr[slot][parity].unk0 = (const struct Object *)sListBuf[slot][parity];
    sRoomsSeen++;
    return &sListHdr[slot][parity];
}
