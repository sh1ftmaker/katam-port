/* Online co-op, stage 1: puppet Kirbys.  The API and the pose layout are in
 * platform/port/coop.h; this is the engine half.
 *
 * --- the model ------------------------------------------------------------
 *
 * Each player runs their own game.  A remote player is shown by taking over
 * one of the three CPU Kirby slots and turning it into a *puppet*: its task,
 * sub_0803EE18, no longer reads input, runs a state, integrates velocity or
 * collides with tiles -- it copies the last pose it was given into the Kirby
 * and runs only the draw half, sub_0803F46C.  One early branch at the top of
 * sub_0803EE18 (game/src/kirby.c) does that; with co-op off the branch tests a
 * zero byte and falls through.
 *
 * The puppet's own LevelInfo is made inactive (FillLevelInfo(slot, 0xFFFF)),
 * the game's own "this Kirby left its room" path.  The game then simulates no
 * room, camera or spawns for the slot and its AI task idles
 * (code_0800ECAC.c: the AI skips a slot whose LevelInfo is 0xFFFF).  Moving a
 * puppet between rooms is a write of base.roomId and nothing else.
 *
 * --- what a puppet must never do ------------------------------------------
 *
 *   damage / pickups / doors   flags 0x200 takes it out of the collision
 *                              lists, 0x100 out of solid pushing, and the
 *                              tile sweep that finds doors is never run.
 *   ability helper objects     the sword/hammer/flame/... helpers a CPU Kirby
 *                              owns destroy themselves when kirby->ability is
 *                              not theirs (code_0806F780.c); the puppet is
 *                              held at NORMAL for SETTLE_FRAMES after it is
 *                              taken over so they all see it, and a puppet
 *                              never runs the states that create new ones.
 *   enemy targeting            FindTargetKirby, sub_0803D46C, sub_0803D5CC,
 *                              sub_0803E324 skip puppets (kirby.c).
 *   scroll-lock triggers       sub_0811D508/sub_0811D68C skip puppets.
 *   pause area map             reads gCurLevelInfo[slot].currentRoom, which
 *                              is 0xFFFF for a puppet: uses PortCoopMapRoom.
 *   base.roomId == 0xFFFF      the HUD indexes gRoomProps with it unguarded;
 *                              a puppet keeps its last valid room.
 *
 * None of this is simulation state of the local game in the netplay sense:
 * co-op is refused while rollback, the lockstep session or seats are active,
 * and the poses are external input, like keys.  tests/sim_state_allowlist.txt.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "level.h"
#include "task.h"
#include "constants/kirby.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/res.h"
#include "port/rollback.h"
#include "port/netsim.h"
#include "port/seats.h"
#include "port/coop.h"
#include "port/kirby_color.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define COOP_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define COOP_EXPORT
#endif

/* C linkage for the 64-bit builds, as platform/warp.c explains. */
#ifdef __cplusplus
extern "C" {
#endif

/* Game functions with no header the port can include. */
extern void sub_0803F46C(struct Kirby *);
extern void sub_0803ED28(struct Kirby *);
extern void sub_0806E4EC(struct Kirby *);
extern void sub_0806EFF8(struct Kirby *);
extern void sub_08055920(struct Kirby *);
extern s32  sub_0815521C(struct Sprite *, u16);   /* sprite.c: seek to a frame */
extern void PortHudReloadKirbyPalette(void);      /* hud.c */

#define COOP_COLORS 14       /* variants of the Kirby palette, anim 0x15 */

#define SETTLE_FRAMES 4

u8 gPortCoopPuppets;

struct CoopSlot {
    u8  pose[PORT_COOP_POSE_SIZE];
    u8  havePose;      /* a pose has been given since the slot was taken    */
    u8  absent;        /* PortCoopSetAbsent: exists, shown nowhere          */
    u8  wanted;        /* PortCoopSetPuppet(slot, 1) is in force            */
    u8  entered;       /* the puppet step has taken the Kirby over          */
    u8  settle;        /* frames left with ability held at NORMAL           */
    u8  flashing;      /* the flash palette was applied last frame          */
    u8  ability;       /* what the palette was last loaded for              */
    u8  pad;
};

static struct {
    u8  enabled;
    u16 seq;
    u16 lastRoom;      /* the local player's last valid room, for poses     */
    s8  seatColor;     /* PortCoopSetLocalColor: -1 = the game's own choice  */
    struct CoopSlot slot[PORT_COOP_MAX_SLOTS];
} sCoop = { 0, 0, 0xFFFF, -1 };

static u8 sPoseBuffer[PORT_COOP_BUFFER_POSES * PORT_COOP_POSE_SIZE];

/* Diagnostics for the self-test: frames co-op was on but the game was not
 * ready, and times the puppets were let go because the adventure ended. */
static u32 sNotReadyFrames, sNotReadyDrops;
static u32 sPuppetSteps[PORT_COOP_MAX_SLOTS];  /* puppet task runs, per slot */

/* --- little-endian packing ----------------------------------------------- */

static void Put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void Put32(u8 *p, u32 v) { Put16(p, v); Put16(p + 2, v >> 16); }
static u32  Get16(const u8 *p)  { return (u32)p[0] | ((u32)p[1] << 8); }
static u32  Get32(const u8 *p)  { return Get16(p) | (Get16(p + 2) << 16); }

/* --- state checks -------------------------------------------------------- */

/* A room this client can draw a puppet in: inside the room table, and a
 * slot the game actually fills.  A remote player on a different room set
 * (mods, editor rooms) is shown as absent rather than read out of bounds. */
static int RoomValid(u32 room)
{
    if (room == 0xFFFF || room >= Res_Count(RES_ROOM_PROPS))
        return 0;
    return gUnk_08D6CD0C[room] != NULL;
}

static int Blocked(void)
{
    return PortRbActive() || PortNetActive() || PortSeatActive();
}

COOP_EXPORT int PortCoopPoseSize(void)
{
    return PORT_COOP_POSE_SIZE;
}

/* The game is in the normal one-player adventure with its four Kirbys and a
 * level: one human, not a link session (2), a sub-game (4) or the attract
 * demo (0x10).  Outside this, puppets cannot exist: the Kirbys are being torn
 * down or are someone else's (sub-game opponents). */
static int InAdventure(void)
{
    if (gNumHumanPlayers != 1 || gLocalPlayerId != 0 || gNumKirbys != 4)
        return 0;
    if (gUnk_0203AD10 & (2 | 4 | 0x10))
        return 0;
    return gUnk_02023354 != NULL;
}

/* ...and the local player is standing in a room: poses make sense, and a
 * slot can be taken over or given back. */
static int GameReady(void)
{
    const struct Kirby *me;

    if (Blocked() || !InAdventure())
        return 0;
    me = &gKirbys[gLocalPlayerId];
    if (!RoomValid(me->base.roomId))
        return 0;
    if (gCurLevelInfo[gLocalPlayerId].currentRoom == 0xFFFF)
        return 0;
    return 1;
}

COOP_EXPORT int PortCoopReady(void)
{
    return sCoop.enabled && GameReady();
}

COOP_EXPORT void PortCoopEnable(int on)
{
    if (on && Blocked()) {
        PortLog("[katam-port] coop: refused -- rollback, netplay or seats is active");
        return;
    }
    if (on && !sCoop.enabled)
        PortLog("[katam-port] coop: enabled");
    if (!on && sCoop.enabled) {
        int s;

        for (s = 1; s < PORT_COOP_MAX_SLOTS; s++)
            sCoop.slot[s].wanted = 0;
        PortLog("[katam-port] coop: disabled");
    }
    sCoop.enabled = on ? 1 : 0;
}

COOP_EXPORT void PortCoopSetPuppet(int slot, int on)
{
    if (slot < 1 || slot >= PORT_COOP_MAX_SLOTS)
        return;
    sCoop.slot[slot].wanted = on ? 1 : 0;
}

COOP_EXPORT void PortCoopSetPose(int slot, const unsigned char *pose)
{
    struct CoopSlot *s;

    if (slot < 1 || slot >= PORT_COOP_MAX_SLOTS || pose == NULL)
        return;
    s = &sCoop.slot[slot];
    memcpy(s->pose, pose, PORT_COOP_POSE_SIZE);
    s->havePose = 1;
    s->absent = 0;
}

COOP_EXPORT void PortCoopSetAbsent(int slot)
{
    if (slot < 1 || slot >= PORT_COOP_MAX_SLOTS)
        return;
    sCoop.slot[slot].absent = 1;
}

COOP_EXPORT unsigned char *PortCoopPoseBuffer(void)
{
    return sPoseBuffer;
}

/* The local Kirby, as it is about to be drawn.  Meaningful between frames:
 * the page calls this from its frame callback, after the tasks have run. */
COOP_EXPORT int PortCoopLocalPose(unsigned char *out)
{
    const struct Kirby *k = &gKirbys[gLocalPlayerId];
    int ready = PortCoopReady();
    u32 room = k->base.roomId;
    u8 f = 0, f2 = 0;

    if (out == NULL)
        return 0;
    if (ready)
        sCoop.lastRoom = (u16)room;
    else
        room = sCoop.lastRoom;

    if (!ready || (k->base.flags & 0x400) || (k->base.sprite.unk8 & 0x80000))
        f |= PORT_COOP_F_HIDDEN;
    if (k->base.flags & 1)                 f |= PORT_COOP_F_LEFT;
    if ((k->base.unk5C & 7) == 6)          f |= PORT_COOP_F_FLASH;
    if (k->base.unkC & 0x20000)            f |= PORT_COOP_F_WATER;
    if (k->base.flags & 0x80)              f |= PORT_COOP_F_ALT;
    if (k->base.flags & 0x40)              f |= PORT_COOP_F_F40;
    if (k->base.flags & 0x4000000)         f |= PORT_COOP_F_OVERLAY;
    if (k->base.flags & 8)                 f |= PORT_COOP_F_FREEZE;
    if (k->base.flags & 4)                 f2 |= PORT_COOP_F2_LOOP;
    if (ready)                             f2 |= PORT_COOP_F2_INLEVEL;

    memset(out, 0, PORT_COOP_POSE_SIZE);
    Put32(out + 0, (u32)k->base.x);
    Put32(out + 4, (u32)k->base.y);
    Put16(out + 8, room);
    out[10] = f;
    out[11] = PORT_COOP_POSE_VERSION;
    Put16(out + 12, k->base.sprite.animId);
    out[14] = k->base.sprite.variant;
    out[15] = k->sprites[1].variant;
    Put16(out + 16, k->sprites[1].animId);
    Put16(out + 18, k->sprites[0].animId);
    out[20] = k->sprites[0].variant;
    out[21] = (u8)k->animationIndex;
    out[22] = k->ability;
    out[23] = k->unkE0;
    out[24] = k->color;
    out[25] = k->base.sprite.unk1C;
    out[26] = k->base.header.unk1;
    out[27] = f2;
    out[28] = (u8)k->hp;
    out[29] = k->lives;
    Put16(out + 30, sCoop.seq);
    {
        /* A colour of the player's own (platform/kirby_color.c): byte 24
         * holds the nearest of the game's fourteen, for builds that do not
         * read these. */
        u16 body, feet;

        PortKirbyColorLocal(&body, &feet);
        if (body | feet) {
            Put16(out + 32, body | PORT_KIRBY_COLOR_POSE_CUSTOM);
            Put16(out + 34, feet);
        }
    }
    return ready;
}

/* --- the puppet ---------------------------------------------------------- */

/* A pose the puppet can be drawn from: given, current version, not absent. */
static const u8 *UsablePose(const struct CoopSlot *s)
{
    if (!s->havePose || s->absent || s->pose[11] != PORT_COOP_POSE_VERSION)
        return NULL;
    if (!(s->pose[27] & PORT_COOP_F2_INLEVEL))
        return NULL;
    return s->pose;
}

int PortCoopPuppetAbsent(u32 slot)
{
    if (slot >= PORT_COOP_MAX_SLOTS)
        return 1;
    return UsablePose(&sCoop.slot[slot]) == NULL
        || !RoomValid(Get16(sCoop.slot[slot].pose + 8));
}

u16 PortCoopMapRoom(u32 slot)
{
    u32 room;

    if (PortCoopPuppetAbsent(slot))
        return 0xFFFF;
    room = Get16(sCoop.slot[slot].pose + 8);
    return RoomValid(room) ? (u16)room : 0xFFFF;
}

/* The first step a slot is a puppet: what was a CPU Kirby stops being one. */
static void TakeOver(struct Kirby *k, struct CoopSlot *s)
{
    u8 slot = k->base.unk56;
    u16 room = k->base.roomId;
    s32 x = k->base.x, y = k->base.y;

    /* Back to the idle state and NORMAL: the state machine never runs again,
     * and the ability change is what tells every helper object the CPU
     * Kirby owned to destroy itself (see the comment at the top). */
    sub_0803ED28(k);
    k->base.x = x;
    k->base.y = y;
    k->base.roomId = room;
    k->unkE0 = 0;
    k->unkE8 = NULL;
    k->unkE6 = 0;
    k->unkF4 = 0;
    k->movementOverride.x = k->movementOverride.y = 0;
    k->base.unk6C = NULL;
    k->base.unkC &= ~0x8000;
    if (k->hp <= 0)
        k->hp = 1;
    k->color = gUnk_0203AD1C[slot] != 0xFF ? gUnk_0203AD1C[slot] : slot;
    PortKirbyColorSetSlot(slot, 0, 0, 0);
    sub_0806E4EC(k);
    sub_0806EFF8(k);
    s->ability = KIRBY_ABILITY_NORMAL;
    s->settle = SETTLE_FRAMES;
    s->flashing = 0;
    s->entered = 1;
    PortLog("[katam-port] coop: slot %u is a puppet (was room %u, level room %u)",
            (unsigned)slot, (unsigned)room,
            (unsigned)gCurLevelInfo[slot].currentRoom);
}

void PortCoopPuppetStep(struct Kirby *k)
{
    u8 slot = k->base.unk56;
    struct CoopSlot *s = &sCoop.slot[slot & 3];
    const struct Kirby *me = &gKirbys[gLocalPlayerId];
    struct Sprite *body = &k->base.sprite, *hat = &k->sprites[1],
                  *ovl = &k->sprites[0];
    const u8 *p;
    u32 room, f, f2, ability;
    int show;

    sPuppetSteps[slot & 3]++;
    if (!s->entered)
        TakeOver(k, s);

    /* Anything that gave the slot a live room again -- it is not supposed
     * to, but the cost of being wrong is a whole room simulated for nobody.
     * Same call the game makes when a Kirby leaves a room. */
    if (gCurLevelInfo[slot].currentRoom != 0xFFFF && gUnk_02023354 != NULL)
        FillLevelInfo(slot, 0xFFFF, NULL, NULL);

    /* Every frame, not once: objects write states and flags into every Kirby
     * in their room (DMK, shards, the boss door...).  The state never runs;
     * the flags below are what other loops read. */
    k->base.unkC &= ~(0x201494 | 0x8000);
    k->base.flags &= ~(0x1 | 0x4 | 0x8 | 0x40 | 0x80 | 0x400 | 0x4000000 | 0x1000000);
    k->base.flags |= 0x2000000 | 0x800 | 0x200 | 0x100;
    k->base.xspeed = k->base.yspeed = 0;
    k->movementState = k->unk11A = k->unk11C = k->unk11E = 0;
    if (k->hp <= 0)
        k->hp = 1;

    p = UsablePose(s);
    room = p ? Get16(p + 8) : 0xFFFF;
    if (RoomValid(room))
        k->base.roomId = (u16)room;
    else if (!RoomValid(k->base.roomId) && RoomValid(me->base.roomId))
        k->base.roomId = me->base.roomId;
    if (p == NULL || !RoomValid(room)) {
        k->base.flags |= 0x400;
        return;
    }

    f = p[10];
    f2 = p[27];
    k->base.unk48 = k->base.x;
    k->base.unk4C = k->base.y;
    k->base.x = (s32)Get32(p + 0);
    k->base.y = (s32)Get32(p + 4);
    if (f & PORT_COOP_F_LEFT)    k->base.flags |= 1;
    if (f2 & PORT_COOP_F2_LOOP)  k->base.flags |= 4;
    if (f & PORT_COOP_F_FREEZE)  k->base.flags |= 8;
    if (f & PORT_COOP_F_F40)     k->base.flags |= 0x40;
    if (f & PORT_COOP_F_ALT)     k->base.flags |= 0x80;
    if (f & PORT_COOP_F_OVERLAY) k->base.flags |= 0x4000000;
    if (f & PORT_COOP_F_WATER)
        k->base.unkC |= 0x20000;
    else
        k->base.unkC &= ~0x20000;

    /* Ability: the hat palette and the HUD icon.  Held at NORMAL while the
     * CPU's helpers die off; an unknown value is drawn as NORMAL. */
    ability = p[22];
    if (s->settle || ability >= KIRBY_ABILITY_COUNT)
        ability = KIRBY_ABILITY_NORMAL;
    if (s->settle)
        s->settle--;
    k->ability = (u8)ability;
    if (ability != s->ability) {
        sub_0806EFF8(k);
        s->ability = (u8)ability;
    }
    k->animationIndex = p[21];
    k->unkE0 = p[23];
    k->hp = (s8)p[28] > 0 ? (s8)p[28] : 1;
    k->lives = p[29];

    /* The sender's colour (their spray paint, or the seat colour their page
     * gave them -- PortCoopSetLocalColor), so a player looks the same in
     * every game.  The palette reload is the one the flash below uses. */
    {
        u16 body = (u16)Get16(p + 32), feet = (u16)Get16(p + 34);
        int custom = (body & PORT_KIRBY_COLOR_POSE_CUSTOM) != 0;
        int changed = PortKirbyColorSetSlot(slot, custom, body, feet);

        if (p[24] < COOP_COLORS && (k->color != p[24] || changed)) {
            k->color = p[24];
            if (!(f & PORT_COOP_F_FLASH))
                sub_0803E558(slot);
        }
    }

    /* The resolved sprites, and the restart sub_0803F790 does when they
     * change (kirby.c, end of sub_0803F790). */
    body->animId = (u16)Get16(p + 12);
    body->variant = p[14];
    hat->variant = p[15];
    hat->animId = (u16)Get16(p + 16);
    ovl->animId = (u16)Get16(p + 18);
    ovl->variant = p[20];
    body->unk1C = p[25];
    if (!(k->base.flags & 8)
     && (body->unk1B != body->variant || body->unk18 != body->animId)) {
        /* A new animation starts at the frame the sender is at, not at 0:
         * the pose is a snapshot of a running animation (hud.c mirrors a
         * Kirby into a sprite with the same seek).  An animation that ends
         * before that frame simply holds its last one. */
        u8 fr = p[26];

        k->base.header.unk1 = 0;
        k->base.header.unk2 = 0;
        body->unk1B = 0xFF;
        hat->unk1B = 0xFF;
        if (fr != 0 && body->animId != 0 && body->unk1C != 0) {
            sub_0815521C(body, fr);
            if (hat->animId != 0)
                sub_0815521C(hat, fr);
            k->base.header.unk1 = fr;
            k->base.header.unk2 = (u16)(fr << 4);
        }
    }
    if (k->base.flags & 1) {
        body->unk8 |= 0x400;
        hat->unk8 |= 0x400;
        ovl->unk8 |= 0x400;
    } else {
        body->unk8 &= ~0x400;
        hat->unk8 &= ~0x400;
        ovl->unk8 &= ~0x400;
    }

    show = !(f & PORT_COOP_F_HIDDEN) && k->base.roomId == me->base.roomId;
    if (!show)
        k->base.flags |= 0x400;

    /* The hurt/invincible flash is a palette task on the sender; here it is
     * the same two palettes, alternated every 4 frames. */
    if (f & PORT_COOP_F_FLASH) {
        if (PortFrameNumber() & 4)
            sub_0803E868(slot);
        else
            sub_0803E558(slot);
        s->flashing = 1;
    } else if (s->flashing) {
        sub_0803E558(slot);
        s->flashing = 0;
    }

    sub_0803F46C(k);
    k->base.unkC &= ~0x20100;
}

/* --- per frame ----------------------------------------------------------- */

/* Give a puppet slot back to the game as a CPU Kirby: the respawn branch of
 * the room-change state (platform/warp.c Deliver explains why that branch),
 * into the local player's room at the local player's cell, so the game's own
 * FillLevelInfo makes its LevelInfo live again and re-arms the AI. */
static void GiveBack(u8 slot)
{
    struct Kirby *k = &gKirbys[slot];
    const struct Kirby *me = &gKirbys[gLocalPlayerId];
    struct CoopSlot *s = &sCoop.slot[slot];

    gPortCoopPuppets &= ~(1u << slot);
    if (s->entered) {
        sub_0803ED28(k);
        k->color = gUnk_0203AD1C[slot] != 0xFF ? gUnk_0203AD1C[slot] : slot;
        PortKirbyColorSetSlot(slot, 0, 0, 0);
        k->hp = k->maxHp > 0 ? k->maxHp : 1;
        k->base.roomId = me->base.roomId;
        k->base.x = me->base.x;
        k->base.y = me->base.y;
        sub_0806E4EC(k);
        sub_0806EFF8(k);
        sub_08055920(k);
        k->base.unk58 &= ~0x4000;
        k->base.unk62 &= ~0x10;
        k->roomId = me->base.roomId;
        k->spawnLocation.x = (s16)((me->base.x + 0x800) >> 12);
        k->spawnLocation.y = (s16)((me->base.y + 0x701) >> 12);
        k->flyTimer = 0;
    }
    memset(s, 0, sizeof(*s));
    PortLog("[katam-port] coop: slot %u given back to the CPU", (unsigned)slot);
}

/* The local Kirby's colour while co-op is on: unless the player chose one
 * (the setting or the collection room's spray paint), the seat colour the
 * page asked for -- player 2 yellow, 3 red, 4 green, as in the game's own
 * link play -- so every game shows every player in the same colour.
 * platform/kirby_color.c decides and applies it. */
COOP_EXPORT void PortCoopSetLocalColor(int color)
{
    sCoop.seatColor = (s8)(color >= 0 && color < COOP_COLORS ? color : -1);
}

static void SelfTestFrame(void);
static int  SelfTestArmed(void);

void PortCoopFrameTop(void)
{
    int ready, takeOk, i;

    SelfTestFrame();
    PortCoopWorldFrame();
    PortCoopEnemyFrame();
    PortKirbyColorSetSeat(sCoop.enabled ? sCoop.seatColor : -1);
    if (!sCoop.enabled && !gPortCoopPuppets)
        return;
    sCoop.seq++;
    if (gPortCoopPuppets && (Blocked() || !InAdventure())) {
        /* The level is being torn down (title, game over, a sub-game), or a
         * session that snapshots the game started: let go of the slots
         * without touching game state -- the Kirbys they were are gone or
         * about to be rebuilt by the game.  Slots still wanted are taken
         * over again once the adventure is back. */
        gPortCoopPuppets = 0;
        for (i = 1; i < PORT_COOP_MAX_SLOTS; i++) {
            sCoop.slot[i].entered = 0;
            PortKirbyColorSetSlot(i, 0, 0, 0);
        }
        sNotReadyDrops++;
    }
    if (Blocked()) {
        sCoop.enabled = 0;
        memset(sCoop.slot, 0, sizeof(sCoop.slot));
        return;
    }
    ready = GameReady();
    if (!ready)
        sNotReadyFrames++;
    /* Slots are taken only past the tutorial: the opening and the tutorial
     * script the three CPU Kirbys (the split, "they follow your every move",
     * the phone), and a cutscene waiting on a helper that is now a puppet
     * would never end.  Already-taken slots are given back if a file still
     * in its tutorial is loaded.  (The self-test arms itself regardless.) */
    takeOk = ready && (gAIKirbyState >= AI_KIRBY_STATE_NORMAL || SelfTestArmed());
    for (i = 1; i < PORT_COOP_MAX_SLOTS; i++) {
        struct CoopSlot *s = &sCoop.slot[i];
        u32 bit = 1u << i;

        if (s->wanted && sCoop.enabled && !(gPortCoopPuppets & bit)) {
            /* Taken over only from normal gameplay: TakeOver and
             * FillLevelInfo need a level to exist. */
            if (takeOk) {
                s->entered = 0;
                s->settle = 0;
                gPortCoopPuppets |= bit;
            }
        } else if ((!s->wanted || !sCoop.enabled || (ready && !takeOk)) && (gPortCoopPuppets & bit)) {
            /* The game needs a live local room to take the Kirby back into;
             * until then it stays a puppet, shown nowhere. */
            if (ready)
                GiveBack((u8)i);
            else
                s->absent = 1;
        }
    }
}

/* --- self-test: PORT_COOP_SELFTEST=START[:MODE[:LEAVE[:PAUSE]]] -----------
 *
 * Native, no network.  From frame START (or the first ready frame after it)
 * co-op is on and slot 1 is a puppet.  MODE:
 *   absent  slot 1 stays absent the whole run (the control)
 *   mix     phases of 400 frames: the local pose 24 px to the right, the same
 *           pose in another room, absent, no pose at all for 400 frames (the
 *           puppet must not move), then near again until the end, cycling
 *           through every ability (palettes) and the hurt flash
 * LEAVE, when given and not -1, gives slot 1 back to the CPU at that frame.
 * PAUSE, when given, pauses at that frame, switches to the area map (which
 * reads every Kirby's room) and leaves the pause screen 150 frames later.
 *
 * Every frame logs the local player's trajectory and a hash of its Kirby,
 * its LevelInfo and the RNG ("[coop-test] L ..."), which must be identical
 * between an absent run and a mix run, and checks the puppet against what it
 * was told ("[coop-test] FAIL ..." on a mismatch).  tests/coop_selftest.sh
 * runs both and compares. */
static struct {
    int armed;           /* -1 unread, 0 off, 1 on */
    long start, leave, pause;
    int mix;
    int started;
    long t0;
    u8 fed[PORT_COOP_POSE_SIZE];
    int expect;          /* 0 none, 1 visible at fed, 2 hidden at fed room, 3 absent */
    long fails, checks;
    u32 lastSteps;
} sTest = { -1, 0, -1, -1, 0, 0, 0, {0}, 0, 0, 0, 0 };

static int SelfTestArmed(void)
{
    return sTest.armed > 0;
}

static u32 Fnv(u32 h, const void *data, u32 n)
{
    const u8 *b = (const u8 *)data;

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static void TestFail(long f, const char *what, long a, long b)
{
    sTest.fails++;
    if (sTest.fails <= 40)
        PortLog("[coop-test] FAIL f=%ld %s (%ld vs %ld)", f, what, a, b);
}

static u16 OtherRoom(u16 mine)
{
    static const u16 kRooms[] = { 101, 102, 103, 104, 201, 202 };
    unsigned i;

    for (i = 0; i < sizeof(kRooms) / sizeof(kRooms[0]); i++)
        if (kRooms[i] != mine && RoomValid(kRooms[i]))
            return kRooms[i];
    return mine;
}

static void SelfTestFrame(void)
{
    long f = (long)PortFrameNumber();
    const struct Kirby *me = &gKirbys[0], *pk = &gKirbys[1];
    u8 *buf = sPoseBuffer;
    long phase;
    u32 h;

    if (sTest.armed < 0) {
        const char *e = getenv("PORT_COOP_SELFTEST");
        char mode[16] = "mix";

        sTest.armed = 0;
        if (e != NULL && *e != '\0'
         && sscanf(e, "%ld:%15[a-z]:%ld:%ld", &sTest.start, mode, &sTest.leave,
                   &sTest.pause) >= 1) {
            sTest.armed = 1;
            sTest.mix = strcmp(mode, "absent") != 0;
            PortLog("[coop-test] armed: start %ld, mode %s, leave %ld, pause %ld",
                    sTest.start, sTest.mix ? "mix" : "absent", sTest.leave,
                    sTest.pause);
        }
    }
    if (!sTest.armed)
        return;

    /* The trajectory line, from the first frame: the runs must agree before
     * co-op starts too, or the comparison proves nothing. */
    h = Fnv(2166136261u, me, sizeof(*me));
    h = Fnv(h, &gCurLevelInfo[0], sizeof(gCurLevelInfo[0]));
    h = Fnv(h, &gRngVal, sizeof(gRngVal));
    PortLog("[coop-test] L f=%ld x=%ld y=%ld room=%u hp=%d %08X", f,
            (long)me->base.x, (long)me->base.y, (unsigned)me->base.roomId,
            (int)me->hp, (unsigned)h);
    /* Every 500 frames rather than at exit, so a run cut short still says. */
    if (sTest.started && f % 500 == 0)
        PortLog("[coop-test] result f=%ld: %ld checks, %ld failures "
                "(%u not-ready frames, %u drops)", f, sTest.checks, sTest.fails,
                (unsigned)sNotReadyFrames, (unsigned)sNotReadyDrops);

    if (!sTest.started) {
        if (f < sTest.start)
            return;
        PortCoopEnable(1);
        if (!PortCoopReady())
            return;
        PortCoopSetPuppet(1, 1);
        PortCoopSetAbsent(1);
        sTest.started = 1;
        sTest.t0 = f;
        sTest.expect = 0;
        PortLog("[coop-test] start f=%ld: local room %u, slot 1 room %u level %u hp %d",
                f, (unsigned)me->base.roomId, (unsigned)pk->base.roomId,
                (unsigned)gCurLevelInfo[1].currentRoom, (int)pk->hp);
        return;
    }

    /* Check what the Kirby task did with last frame's instruction -- if it
     * ran: the pause screen stops every task. */
    if ((gPortCoopPuppets & 2) && sPuppetSteps[1] != sTest.lastSteps) {
        sTest.checks++;
        if (pk->base.roomId == 0xFFFF)
            TestFail(f, "puppet roomId is 0xFFFF", 0, 0);
        if (gCurLevelInfo[1].currentRoom != 0xFFFF)
            TestFail(f, "puppet LevelInfo is live", gCurLevelInfo[1].currentRoom, 0xFFFF);
        if (pk->hp <= 0)
            TestFail(f, "puppet hp", pk->hp, 1);
        if (sTest.expect == 1 || sTest.expect == 2) {
            if (pk->base.x != (s32)Get32(sTest.fed) || pk->base.y != (s32)Get32(sTest.fed + 4))
                TestFail(f, "puppet x not where told", pk->base.x, (long)(s32)Get32(sTest.fed));
            if (pk->base.roomId != Get16(sTest.fed + 8))
                TestFail(f, "puppet room", pk->base.roomId, Get16(sTest.fed + 8));
        }
        if (sTest.expect == 1 && (pk->base.flags & 0x400)
         && !(sTest.fed[10] & PORT_COOP_F_HIDDEN) && Get16(sTest.fed + 8) == me->base.roomId)
            TestFail(f, "puppet hidden in my room", 0, 1);
        if ((sTest.expect == 2 || sTest.expect == 3) && !(pk->base.flags & 0x400))
            TestFail(f, "puppet drawn while elsewhere/absent", 1, 0);
        if (f % 100 == 0)
            PortLog("[coop-test] P f=%ld x=%ld y=%ld room=%u flags=%08X anim=%u/%u",
                    f, (long)pk->base.x, (long)pk->base.y, (unsigned)pk->base.roomId,
                    (unsigned)pk->base.flags, (unsigned)pk->base.sprite.animId,
                    (unsigned)pk->base.sprite.variant);
    }

    sTest.lastSteps = sPuppetSteps[1];
    if (sTest.leave >= 0 && f == sTest.leave) {
        PortCoopSetPuppet(1, 0);
        sTest.expect = 0;
        PortLog("[coop-test] leave f=%ld", f);
    }
    if (sTest.leave >= 0 && f > sTest.leave) {
        if ((f - sTest.leave) % 200 == 0)
            PortLog("[coop-test] cpu f=%ld slot 1 room %u level %u x=%ld y=%ld puppet=%d",
                    f, (unsigned)pk->base.roomId, (unsigned)gCurLevelInfo[1].currentRoom,
                    (long)pk->base.x, (long)pk->base.y, (gPortCoopPuppets & 2) != 0);
        return;
    }

    /* This frame's instruction. */
    phase = sTest.mix ? (f - sTest.t0) / 400 : -1;
    if (phase < 0) {
        PortCoopSetAbsent(1);
        sTest.expect = 3;
    } else if (phase == 0 || phase >= 4) {
        PortCoopLocalPose(buf);
        Put32(buf, Get32(buf) + (24 << 8));
        if (phase >= 4) {
            /* Late phases also walk the puppet through every ability's
             * palette and the hurt flash: these must not crash or leak. */
            buf[22] = (u8)(((f - sTest.t0) / 50) % KIRBY_ABILITY_COUNT);
            if (((f - sTest.t0) / 100) & 1)
                buf[10] |= PORT_COOP_F_FLASH;
        }
        PortCoopSetPose(1, buf);
        memcpy(sTest.fed, buf, sizeof(sTest.fed));
        sTest.expect = (buf[27] & PORT_COOP_F2_INLEVEL) ? 1 : 3;
    } else if (phase == 1) {
        PortCoopLocalPose(buf);
        Put16(buf + 8, OtherRoom(me->base.roomId));
        PortCoopSetPose(1, buf);
        memcpy(sTest.fed, buf, sizeof(sTest.fed));
        sTest.expect = (buf[27] & PORT_COOP_F2_INLEVEL) ? 2 : 3;
    } else if (phase == 2) {
        PortCoopSetAbsent(1);
        sTest.expect = 3;
    } else {
        /* Phase 3: no instruction at all.  The first frame re-sends the last
         * near pose; after that the puppet is on its own and must stay put. */
        if (f - sTest.t0 == 1200) {
            PortCoopLocalPose(buf);
            Put32(buf, Get32(buf) + (24 << 8));
            PortCoopSetPose(1, buf);
            memcpy(sTest.fed, buf, sizeof(sTest.fed));
            sTest.expect = (buf[27] & PORT_COOP_F2_INLEVEL) ? 1 : 3;
        }
    }
}

/* The same arming for the node build, whose getenv cannot see process.env
 * (tools/headless_test.js forwards PORT_COOP_SELFTEST here). */
COOP_EXPORT void PortCoopSelfTest(int start, int mix, int leave, int pause)
{
    sTest.armed = 1;
    sTest.start = start;
    sTest.mix = mix != 0;
    sTest.leave = leave;
    sTest.pause = pause;
    PortLog("[coop-test] armed: start %ld, mode %s, leave %ld, pause %ld",
            sTest.start, sTest.mix ? "mix" : "absent", sTest.leave, sTest.pause);
}

/* The self-test's START presses, from the key latch (platform/main.c), after
 * the replay: does nothing unless PORT_COOP_SELFTEST asked for a pause. */
void PortCoopTestKeys(u32 frame, u16 *keys)
{
    long f = (long)frame;

    if (sTest.armed <= 0 || sTest.pause < 0)
        return;
    if (f >= sTest.pause && f < sTest.pause + 6)
        *keys = START_BUTTON;           /* pause: the help page */
    else if (f >= sTest.pause + 80 && f < sTest.pause + 86)
        *keys = SELECT_BUTTON;          /* switch: the area map */
    else if (f >= sTest.pause + 150 && f < sTest.pause + 156)
        *keys = B_BUTTON;               /* quit */
    else if (f >= sTest.pause && f < sTest.pause + 200)
        *keys = 0;
}

#ifdef __cplusplus
}
#endif
