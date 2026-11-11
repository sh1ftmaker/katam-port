/* Online co-op, stage 2: shared progress.  The API and the record layout are
 * in platform/port/coop.h; this is the engine half.
 *
 * --- what is shared -------------------------------------------------------
 *
 * Exactly what the game itself writes to the save as world progress,
 * gWorldProps in game/src/save.c, and nothing else:
 *
 *   gUnk_02028BF0   u8[0x20]     save-scope despawn bits (unused by vanilla
 *                                level data, kept for mods)
 *   gUnk_02028C10   u32[0x20]    save-scope state slots: the big switches
 *                                (world map lines, shortcuts), special
 *                                doors, the hub mirrors, DMK's first meeting
 *   gVisitedDoors   u16[287]     per room: doors walked through, chests
 *                                opened (the area map)
 *   gTreasures      chests, shards, spray paints, the sound player and its
 *                   sheets, the big (map) chests -- bitfields -- and the
 *                   vitality count
 *   gShadowKirbyEncounters, gMirraEncounters   bitfields
 *   gAIKirbyState   the story stage (100 tutorial .. 300 normal .. 10000)
 *
 * Everything but the vitality count and the story stage is a set of flags
 * that only ever turn on (every writer in game/src ORs in a bit or stores a
 * 1), so merging two players' progress is a bitwise OR, and merging is
 * idempotent and order-free: a record can arrive twice, late, or before
 * another, and the result is the same.  Consumables (food, 1-ups, lives,
 * abilities) are per area or per player and stay per player, as in Ship of
 * Harkinian's Anchor (WorldSync.h): progress is shared, consumables are not.
 *
 * The vitality count is the one counter.  A vitality chest sets its chest
 * bit and increments the count in the same frame (chest.c sub_0800AFC8), so
 * a delta record carries both, and the receiver adds the increment only when
 * the record's chest bit is new to it -- the flag that produced the item
 * travels with it, so two players opening one chest do not get two hearts.
 * A full record (a whole image, sent to someone joining) counts the vitality
 * chests it has that the receiver lacks, using the chest ids found in the
 * ROM's room object lists (VitalityChests, below).
 *
 * The story stage is merged as a maximum, and only once both sides are past
 * the tutorial (>= AI_KIRBY_STATE_NORMAL): the tutorial's own transitions
 * run on cutscenes that a jump in the stage would skip.  Until the local
 * player is past the tutorial nothing is tracked and incoming records wait.
 *
 * --- how it moves ---------------------------------------------------------
 *
 * The engine keeps a shadow of the image as last reported.  Each frame the
 * page calls PortCoopWorldTake: the bits set since the shadow are a delta
 * record (an empty return when nothing changed), and the shadow catches up.
 * Bits merged from other players go into the shadow as they are applied, so
 * nothing is echoed back.  Records that arrive while the game cannot take
 * them (title screen, file select, tutorial, a level loading) are queued in
 * order and applied at the next frame top where it can.
 *
 * When tracking (re)starts -- a file was loaded, or the adventure came back
 * from a sub-game or the title -- the first Take returns a FULL record of the
 * whole image, which the page sends to everyone: the file may be a different
 * one.  The page also asks for a full record whenever someone joins.
 *
 * Applying a record writes the save (UpdateSaveBufferByOffset, the call the
 * game makes when you open a chest yourself), so merged progress survives a
 * reload, and fixes up what is already on screen: chests in a loaded room
 * show open, and the local Kirby's max HP follows a new vitality.
 *
 * Like the poses, none of this is simulation state in the netplay sense:
 * co-op is refused under rollback, the lockstep session and seats.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "level.h"
#include "task.h"
#include "object.h"
#include "chest.h"
#include "treasures.h"
#include "save.h"
#include "constants/kirby.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/res.h"
#include "port/rollback.h"
#include "port/netsim.h"
#include "port/seats.h"
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

/* --- the image ------------------------------------------------------------
 * The OR-merged part, flattened in gWorldProps order.  Offsets are part of
 * the record format (coop.h); a new region goes at the end. */
struct WorldRegion {
    u32 addr;          /* GBA address (game/generated/port/ram_symbols.h) */
    u16 off, len;      /* in the image */
};

#define W_TREASURE_ADDR 0x02038960u
static const struct WorldRegion kRegions[] = {
    { 0x02028BF0u,              0,   0x20 },   /* gUnk_02028BF0             */
    { 0x02028C10u,              32,  0x80 },   /* gUnk_02028C10             */
    { 0x02028CA0u,              160, 574  },   /* gVisitedDoors[0..286]     */
    { W_TREASURE_ADDR + 0x00u,  734, 16   },   /* gTreasures.chestFields    */
    { W_TREASURE_ADDR + 0x10u,  750, 4    },   /*   shardField              */
    { W_TREASURE_ADDR + 0x14u,  754, 4    },   /*   sprayPaintField         */
    { W_TREASURE_ADDR + 0x18u,  758, 4    },   /*   musicPlayerAndSheets    */
    { W_TREASURE_ADDR + 0x1Cu,  762, 4    },   /*   bigChestField           */
    { 0x020229D0u,              766, 4    },   /* gShadowKirbyEncounters    */
    { 0x020229D8u,              770, 4    },   /* gMirraEncounters          */
};
#define NREGIONS (sizeof(kRegions) / sizeof(kRegions[0]))

#define W_OFF_STATESLOTS 32
#define W_OFF_DOORS      160
#define W_OFF_CHESTS     734
#define W_OFF_SHARDS     750
#define W_OFF_SPRAY      754
#define W_OFF_MUSIC      758
#define W_OFF_BIGCHEST   762
#define W_OFF_ENCOUNTERS 766

/* Record header (coop.h). */
#define REC_MAGIC   0x50
#define REC_VERSION 1
#define REC_HDR     10

#define PEND_RECORDS 32

static struct {
    u8  share;                     /* PortCoopWorldShare                     */
    u8  tracking;                  /* the shadow belongs to the loaded file  */
    u8  wantFull;                  /* next Take returns a full record        */
    u16 shadowVit, shadowStory;
    u8  shadow[PORT_COOP_WORLD_BYTES];
    u32 news;                      /* applied from the queue, not yet read   */
    u32 applied, queued, dropped;  /* diagnostics                            */
    u32 sent;
} sW;

static struct {
    u16 len;
    u8  data[PORT_COOP_RECORD_MAX];
} sPend[PEND_RECORDS];
static u8 sPendHead, sPendCount;

static u8 sScratch[PORT_COOP_RECORD_MAX];

/* Vitality chest ids (gTreasures.chestFields bit numbers), from the ROM. */
static u8  sVitMask[16];
static int sVitScanned;

/* --- access -------------------------------------------------------------- */

static u8 *RegionPtr(const struct WorldRegion *r)
{
    return (u8 *)(uintptr_t)r->addr;
}

static void ReadImage(u8 *img)
{
    unsigned i;

    for (i = 0; i < NREGIONS; i++)
        memcpy(img + kRegions[i].off, RegionPtr(&kRegions[i]), kRegions[i].len);
}

static u8 *ImageByte(u32 off)
{
    unsigned i;

    for (i = 0; i < NREGIONS; i++)
        if (off >= kRegions[i].off && off < (u32)kRegions[i].off + kRegions[i].len)
            return RegionPtr(&kRegions[i]) + (off - kRegions[i].off);
    return NULL;
}

static u16 Vitality(void)    { return gTreasures.vitalityField; }
static u16 Story(void)       { return (u16)gAIKirbyState; }

static int Popcount8(u32 v)
{
    int n = 0;

    for (v &= 0xFF; v; v &= v - 1)
        n++;
    return n;
}

static void Put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u32  Get16(const u8 *p)  { return (u32)p[0] | ((u32)p[1] << 8); }

/* --- when progress can be read and written ---------------------------------
 * The normal one-player adventure with a file loaded and the tutorial done.
 * Same test as the puppets' (coop.c InAdventure), plus the story stage; a
 * level need not be live -- a room transition is fine, the props are
 * plain memory. */
static int Trackable(void)
{
    if (PortRbActive() || PortNetActive() || PortSeatActive())
        return 0;
    if (gNumHumanPlayers != 1 || gLocalPlayerId != 0 || gNumKirbys != 4)
        return 0;
    if (gUnk_0203AD10 & (2 | 4 | 0x10))
        return 0;
    if (gUnk_02023354 == NULL)
        return 0;
    return Story() >= AI_KIRBY_STATE_NORMAL;
}

static void SaveWorld(void)
{
    if (gUnk_0203AD10 & 0x10)
        return;
    UpdateSaveBufferByOffset(SAVE_BUFFER_TYPE_WORLD_PROPS, gSaveID > 2 ? 0 : gSaveID);
}

/* --- vitality chests, from the ROM -----------------------------------------
 * A chest object's template carries its item (subtype1; 6 is vitality,
 * chest.c sub_0800AFC8) and its treasure index (unk11).  Walk every room's
 * object list once. */
static void VitalityChests(void)
{
    u32 room, n, i, rooms;

    if (sVitScanned)
        return;
    sVitScanned = 1;
    memset(sVitMask, 0, sizeof(sVitMask));
    rooms = Res_Count(RES_ROOM_PROPS);
    for (room = 0; room < rooms; room++) {
        const struct ObjectTemplate *t = PortCoopRoomObjects(room, &n);

        for (i = 0; t != NULL && i < n; i++) {
            if ((t[i].type == OBJ_SMALL_CHEST || t[i].type == OBJ_BIG_CHEST)
             && t[i].subtype1 == 6 && t[i].unk11 < 128)
                sVitMask[t[i].unk11 >> 3] |= (u8)(1u << (t[i].unk11 & 7));
        }
    }
    n = 0;
    for (i = 0; i < 16; i++)
        n += (u32)Popcount8(sVitMask[i]);
    PortLog("[katam-port] coop: %u vitality chests in the room data", (unsigned)n);
}

/* --- on-screen fix-ups ----------------------------------------------------- */

/* chest.c's states are static: compare and store their function ids
 * (game/generated/fnid_ids.h), as the game itself stores them. */
extern void sub_080029F4(u8, u16);    /* code_080023A4.c: room counter */
static u32  Fn78(const struct Object *o)      { u32 v; memcpy(&v, &o->unk78, 4); return v; }
static void SetFn78(struct Object *o, u32 id) { memcpy(&o->unk78, &id, 4); }

static int OpenChest(struct Object *obj, void *arg)
{
    struct Chest *c = (struct Chest *)obj;

    (void)arg;
    if (obj->type != OBJ_SMALL_CHEST && obj->type != OBJ_BIG_CHEST)
        return 0;
    if (!HasChest(c->unkE2) || Fn78(obj) != FNID_sub_0800AEB0)
        return 0;
    obj->unk83 = c->unkE0 != 0x63 ? 1 : 3;
    SetFn78(obj, FNID_sub_0800BD9C);
    sub_080029F4(EWRAM_MIRROR_U8(&gCurLevelInfo[obj->base.unk56].unk65E), 1);
    return 0;
}

/* --- records --------------------------------------------------------------- */

/* Runs of nonzero bytes of img; returns the record length, 0 if it would
 * not fit (it always does: the image is 774 bytes). */
static int Encode(u8 *out, int full, const u8 *img, u32 vit, u32 story)
{
    u32 off = 0, nruns = 0, o = REC_HDR;

    out[0] = REC_MAGIC;
    out[1] = REC_VERSION;
    out[2] = full ? PORT_COOP_REC_FULL : PORT_COOP_REC_DELTA;
    out[3] = 0;
    Put16(out + 4, vit);
    Put16(out + 6, story);
    while (off < PORT_COOP_WORLD_BYTES) {
        u32 start, len;

        if (img[off] == 0) {
            off++;
            continue;
        }
        start = off;
        /* a run swallows gaps of up to 2 zero bytes: cheaper than a header */
        while (off < PORT_COOP_WORLD_BYTES && off - start < 255
            && (img[off] != 0
             || (off + 1 < PORT_COOP_WORLD_BYTES && img[off + 1] != 0)
             || (off + 2 < PORT_COOP_WORLD_BYTES && img[off + 2] != 0)))
            off++;
        len = off - start;
        if (o + 3 + len > PORT_COOP_RECORD_MAX)
            return 0;
        Put16(out + o, start);
        out[o + 2] = (u8)len;
        memcpy(out + o + 3, img + start, len);
        o += 3 + len;
        nruns++;
    }
    Put16(out + 8, nruns);
    return (int)o;
}

static int Valid(const u8 *rec, int len)
{
    u32 n, i, o = REC_HDR;

    if (rec == NULL || len < REC_HDR || len > PORT_COOP_RECORD_MAX)
        return 0;
    if (rec[0] != REC_MAGIC || rec[1] != REC_VERSION || rec[2] > PORT_COOP_REC_FULL)
        return 0;
    n = Get16(rec + 8);
    for (i = 0; i < n; i++) {
        u32 off, l;

        if (o + 3 > (u32)len)
            return 0;
        off = Get16(rec + o);
        l = rec[o + 2];
        if (l == 0 || off + l > PORT_COOP_WORLD_BYTES || o + 3 + l > (u32)len)
            return 0;
        o += 3 + l;
    }
    return o == (u32)len;
}

/* What a set of new bits in the image means, for the page's notices. */
static u32 NewsOf(u32 off, u32 bits)
{
    if (!bits)
        return 0;
    if (off >= W_OFF_SHARDS && off < W_OFF_SHARDS + 4)       return PORT_COOP_NEWS_SHARD;
    if (off >= W_OFF_BIGCHEST && off < W_OFF_BIGCHEST + 4)   return PORT_COOP_NEWS_BIGCHEST;
    if (off >= W_OFF_SPRAY && off < W_OFF_SPRAY + 4)         return PORT_COOP_NEWS_SPRAY;
    if (off >= W_OFF_MUSIC && off < W_OFF_MUSIC + 4)         return PORT_COOP_NEWS_MUSIC;
    if (off >= W_OFF_CHESTS && off < W_OFF_CHESTS + 16)      return PORT_COOP_NEWS_CHEST;
    if (off >= W_OFF_STATESLOTS && off < W_OFF_DOORS)        return PORT_COOP_NEWS_SWITCH;
    if (off >= W_OFF_DOORS && off < W_OFF_CHESTS)            return PORT_COOP_NEWS_MAP;
    if (off >= W_OFF_ENCOUNTERS)                              return PORT_COOP_NEWS_ENCOUNTER;
    return PORT_COOP_NEWS_OTHER;
}

/* Merge a valid record into the live game.  Returns PORT_COOP_NEWS_* bits
 * for what was new here (0: nothing). */
static u32 Apply(const u8 *rec, int len)
{
    u32 n = Get16(rec + 8), i, o = REC_HDR, news = 0, vit = Get16(rec + 4);
    u32 story = Get16(rec + 6), newVitChests = 0;
    int full = rec[2] == PORT_COOP_REC_FULL, newChest = 0;

    (void)len;
    if (full)
        VitalityChests();
    for (i = 0; i < n; i++) {
        u32 off = Get16(rec + o), l = rec[o + 2], j;

        for (j = 0; j < l; j++) {
            u8 *p = ImageByte(off + j);
            u32 add;

            if (p == NULL)
                continue;
            add = rec[o + 3 + j] & ~*p;
            if (!add)
                continue;
            if (off + j >= W_OFF_CHESTS && off + j < W_OFF_CHESTS + 16) {
                newChest = 1;
                newVitChests += (u32)Popcount8(add & sVitMask[off + j - W_OFF_CHESTS]);
            }
            *p |= (u8)add;
            sW.shadow[off + j] |= (u8)add;
            news |= NewsOf(off + j, add);
        }
        o += 3 + l;
    }

    /* the vitality count (see the top) */
    if (!full && vit > 0 && newChest)
        newVitChests = vit;
    else if (!full)
        newVitChests = 0;
    if (newVitChests) {
        u32 v = Vitality() + newVitChests;
        struct Kirby *me = &gKirbys[gLocalPlayerId];

        if (v > 0xFF)
            v = 0xFF;
        gTreasures.vitalityField = (u16)v;
        sW.shadowVit = (u16)v;
        me->maxHp = (s8)(v + 6);
        me->hp = (s8)(me->hp + newVitChests > (u32)me->maxHp ? me->maxHp : me->hp + newVitChests);
        news |= PORT_COOP_NEWS_VITALITY;
    }
    if (story > Story() && Story() >= AI_KIRBY_STATE_NORMAL && story >= AI_KIRBY_STATE_NORMAL
     && story <= AI_KIRBY_STATE_UNK2) {
        gAIKirbyState = (enum AIKirbyState)story;
        sW.shadowStory = (u16)story;
        news |= PORT_COOP_NEWS_STORY;
    }
    if (news) {
        PortCoopForEachObject(OpenChest, NULL);
        SaveWorld();
        sW.applied++;
    }
    return news;
}

static void StartTracking(void)
{
    ReadImage(sW.shadow);
    sW.shadowVit = Vitality();
    sW.shadowStory = Story();
    sW.tracking = 1;
    sW.wantFull = 1;
}

/* From PortCoopFrameTop: tracking on/off, and the queue. */
void PortCoopWorldFrame(void)
{
    if (!sW.share) {
        sW.tracking = 0;
        return;
    }
    if (!Trackable()) {
        sW.tracking = 0;
        return;
    }
    if (!sW.tracking)
        StartTracking();
    while (sPendCount) {
        const u8 *r = sPend[sPendHead].data;
        int l = sPend[sPendHead].len;

        sW.news |= Apply(r, l);
        sPendHead = (u8)((sPendHead + 1) % PEND_RECORDS);
        sPendCount--;
    }
}

/* --- exported ---------------------------------------------------------------- */

COOP_EXPORT int PortCoopWorldBytes(void)
{
    return PORT_COOP_WORLD_BYTES;
}

COOP_EXPORT unsigned char *PortCoopRecordBuffer(void)
{
    return sScratch;
}

COOP_EXPORT void PortCoopWorldShare(int on)
{
    if (!!on != !!sW.share)
        PortLog("[katam-port] coop: shared progress %s", on ? "on" : "off");
    sW.share = on ? 1 : 0;
    if (!on) {
        sW.tracking = 0;
        sPendCount = 0;
    }
}

COOP_EXPORT int PortCoopWorldTracking(void)
{
    return sW.share && sW.tracking && Trackable();
}

/* A record of the local progress for the page to send: FULL when tracking
 * just (re)started or `full` asks for one, else a DELTA of what is new since
 * the last call.  Returns its length (0: nothing to send). */
COOP_EXPORT int PortCoopWorldTake(unsigned char *out, int full)
{
    u8 img[PORT_COOP_WORLD_BYTES], d[PORT_COOP_WORLD_BYTES];
    u32 i, any = 0, vit, story;
    int lost = 0, len;

    if (out == NULL || !PortCoopWorldTracking())
        return 0;
    ReadImage(img);
    vit = Vitality();
    story = Story();
    if (full || sW.wantFull) {
        memcpy(sW.shadow, img, sizeof(img));
        sW.shadowVit = (u16)vit;
        sW.shadowStory = (u16)story;
        sW.wantFull = 0;
        sW.sent++;
        return Encode(out, 1, img, vit, story);
    }
    for (i = 0; i < PORT_COOP_WORLD_BYTES; i++) {
        d[i] = img[i] & ~sW.shadow[i];
        any |= d[i];
        if (sW.shadow[i] & ~img[i])
            lost = 1;
    }
    if (lost || vit < sW.shadowVit) {
        /* Flags went off: not the game's doing within one file (they only
         * turn on).  A file was swapped under us; start over with a full. */
        memcpy(sW.shadow, img, sizeof(img));
        sW.shadowVit = (u16)vit;
        sW.shadowStory = (u16)story;
        sW.sent++;
        return Encode(out, 1, img, vit, story);
    }
    if (!any && vit == sW.shadowVit && story <= sW.shadowStory)
        return 0;
    len = Encode(out, 0, d, vit > sW.shadowVit ? vit - sW.shadowVit : 0, story);
    memcpy(sW.shadow, img, sizeof(img));
    sW.shadowVit = (u16)vit;
    sW.shadowStory = (u16)story;
    sW.sent++;
    return len;
}

/* Merge a record from another player.  Returns PORT_COOP_NEWS_* bits (0:
 * nothing new), -1 when it was queued for later (title screen, tutorial,
 * loading), -2 when it is malformed or sharing is off. */
COOP_EXPORT int PortCoopWorldApply(const unsigned char *rec, int len)
{
    if (!sW.share || !Valid(rec, len))
        return -2;
    if (PortCoopWorldTracking() && !sPendCount)
        return (int)Apply(rec, len);
    if (sPendCount == PEND_RECORDS) {
        /* full: the oldest goes; a later full record repairs anything lost */
        sPendHead = (u8)((sPendHead + 1) % PEND_RECORDS);
        sPendCount--;
        sW.dropped++;
    }
    {
        u32 at = (sPendHead + sPendCount) % PEND_RECORDS;

        memcpy(sPend[at].data, rec, (size_t)len);
        sPend[at].len = (u16)len;
        sPendCount++;
        sW.queued++;
    }
    return -1;
}

/* News from queued records applied since the last call; read and clear. */
COOP_EXPORT int PortCoopWorldNews(void)
{
    u32 n = sW.news;

    sW.news = 0;
    return (int)n;
}

/* The vitality chests' treasure indices (diagnostics and tests); returns
 * how many, writing up to max of them. */
COOP_EXPORT int PortCoopWorldVitalityIds(unsigned char *out, int max)
{
    int i, n = 0;

    VitalityChests();
    for (i = 0; i < 128; i++)
        if (sVitMask[i >> 3] & (1u << (i & 7))) {
            if (out != NULL && n < max)
                out[n] = (u8)i;
            n++;
        }
    return n;
}

/* For notices: how many of a kind the local file has now. */
COOP_EXPORT int PortCoopWorldCount(int kind)
{
    switch (kind) {
    case PORT_COOP_NEWS_SHARD:    return NumShardsCollected();
    case PORT_COOP_NEWS_CHEST:    return NumChestsCollected();
    case PORT_COOP_NEWS_SPRAY:    return NumSprayPaintsCollected();
    case PORT_COOP_NEWS_MUSIC:    return NumMusicPlayerAndSheetsCollected();
    case PORT_COOP_NEWS_BIGCHEST: return NumBigChestsCollected();
    case PORT_COOP_NEWS_VITALITY: return Vitality();
    case PORT_COOP_NEWS_STORY:    return Story();
    default:                      return 0;
    }
}

#ifdef __cplusplus
}
#endif
