#ifndef GUARD_PORT_COOP_H
#define GUARD_PORT_COOP_H

/* Online co-op, stage 1: remote players as puppet Kirbys (platform/coop.c).
 *
 * Every player runs their own, independent game.  Nothing here is lockstep,
 * rollback or shared input: the page sends the local Kirby's *pose* (where it
 * is and what it looks like) to the other players, and writes the poses it
 * receives into CPU Kirby slots 1..3, which then stop simulating and only
 * draw.  gNumHumanPlayers stays 1 and gLocalPlayerId stays 0, so every
 * "all players" test in the game still means "me".
 *
 * --- the pose: PORT_COOP_POSE_SIZE (36) bytes, little-endian, no pointers ---
 *
 *   off size field      meaning
 *     0  i32  x          base.x, Q8 pixels (x >> 8 = pixel in the room)
 *     4  i32  y          base.y, Q8 pixels
 *     8  u16  room       base.roomId (the live room, not the respawn record)
 *    10  u8   flags      PORT_COOP_F_*; bit 0 = hidden/absent-like: the JS
 *                        side must not interpolate from or towards it
 *   ---- everything below is opaque to JS (it copies the newer pose whole) ----
 *    11  u8   version    PORT_COOP_POSE_VERSION; any other value = absent
 *    12  u16  bodyAnim   base.sprite.animId    (resolved sprite, not a table
 *    14  u8   bodyVar    base.sprite.variant    index: the per-slot animation
 *    15  u8   hatVar     sprites[1].variant     table differs between slots
 *    16  u16  hatAnim    sprites[1].animId      and abilities)
 *    18  u16  ovlAnim    sprites[0].animId     overlay; drawn only when
 *    20  u8   ovlVar     sprites[0].variant    animIndex < 10 && F_OVERLAY
 *    21  u8   animIndex  animationIndex (gates the hat and overlay draw)
 *    22  u8   ability    ability (hat palette, HUD icon)
 *    23  u8   unkE0      the "hat without ability" field (sub_0803F46C gate)
 *    24  u8   color      the sender's colour (0..13), applied to the puppet;
 *                        with a custom colour, the nearest of the fourteen
 *    25  u8   animSpeed  base.sprite.unk1C (0x10 = normal speed)
 *    26  u8   frame      base.header.unk1 (frames into the animation)
 *    27  u8   flags2     PORT_COOP_F2_*
 *    28  s8   hp         display only
 *    29  u8   lives      display only
 *    30  u16  seq        sender's pose counter (wraps)
 *    32  u16  custom     0, or a colour of the sender's own
 *                        (platform/kirby_color.c): body BGR555 |
 *                        PORT_KIRBY_COLOR_POSE_CUSTOM (0x8000)
 *    34  u16  customFeet the feet, BGR555 (0 unless custom)
 *
 * Shake/squash offsets (objBase54/55) are not carried: the game clears them
 * inside the draw before a pose can be read.
 */

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_COOP_POSE_SIZE    36
#define PORT_COOP_POSE_VERSION 1
#define PORT_COOP_MAX_SLOTS    4      /* slot 0 is always the local player */
#define PORT_COOP_BUFFER_POSES 8      /* PortCoopPoseBuffer holds this many */

enum {
    PORT_COOP_F_HIDDEN  = 1 << 0,   /* absent, not in a level, or not drawn   */
    PORT_COOP_F_LEFT    = 1 << 1,   /* base.flags & 1 (facing left)           */
    PORT_COOP_F_FLASH   = 1 << 2,   /* (base.unk5C & 7) == 6 (hurt/invincible) */
    PORT_COOP_F_WATER   = 1 << 3,   /* base.unkC & 0x20000                    */
    PORT_COOP_F_ALT     = 1 << 4,   /* base.flags & 0x80                      */
    PORT_COOP_F_F40     = 1 << 5,   /* base.flags & 0x40                      */
    PORT_COOP_F_OVERLAY = 1 << 6,   /* base.flags & 0x4000000                 */
    PORT_COOP_F_FREEZE  = 1 << 7,   /* base.flags & 8 (animation not stepped) */
};
enum {
    PORT_COOP_F2_LOOP    = 1 << 0,  /* base.flags & 4                          */
    PORT_COOP_F2_INLEVEL = 1 << 1,  /* sender was PortCoopReady()             */
    PORT_COOP_F2_TUTORIAL = 1 << 2, /* sender's file is still in its tutorial
                                     * (it runs no puppets, so it cannot run
                                     * the CPU Kirbys for the others)        */
};

/* --- exported to the page (PORT_EXPORTS) and callable natively -----------
 *
 * Call between frames (the page's frame callback).  Order: PortCoopEnable(1),
 * then PortCoopSetPuppet(slot, 1); the slot is taken over at the top of the
 * next frame in which PortCoopReady() is 1, and shows nothing until a pose
 * arrives.  A pose is kept until replaced: the puppet stays exactly where
 * the last one put it.  PortCoopSetAbsent hides it (HUD icon and pause-map
 * marker too) and keeps its last valid room.  PortCoopSetPuppet(slot, 0) or
 * PortCoopEnable(0) gives the slot back to the CPU, respawned next to the
 * local player, at the next ready frame.  Puppets are dropped by themselves
 * when the adventure ends (title, game over, sub-game) and taken over again,
 * if still wanted, when it is back.  A pose is drawn only when its version
 * matches, F2_INLEVEL is set and its room exists on this client. */
int            PortCoopPoseSize(void);
void           PortCoopEnable(int on);       /* refused under rollback/net/seats */
int            PortCoopReady(void);          /* local player in normal gameplay */
void           PortCoopSetPuppet(int slot, int on);  /* 1..3; applied next frame */
void           PortCoopSetPose(int slot, const unsigned char *pose);
void           PortCoopSetAbsent(int slot);
int            PortCoopLocalPose(unsigned char *out);  /* 1 when valid */
unsigned char *PortCoopPoseBuffer(void);     /* 8 * PortCoopPoseSize() bytes */
/* The local Kirby's colour while co-op is on, 0..13 (0 pink, 1 yellow, 2 red,
 * 3 green, ...), unless the player chose a spray paint; -1 gives the game
 * its own colour back.  Carried in the pose, applied to puppets. */
void           PortCoopSetLocalColor(int color);

/* --- stage 2: shared progress (platform/coop_world.c) ----------------------
 *
 * The world progress the game saves (gWorldProps, game/src/save.c) is merged
 * between players: flags by OR, the vitality count by the chest that gave
 * it, the story stage by maximum once past the tutorial.  The page moves
 * opaque *records*; this is their layout, little-endian:
 *
 *   off size field
 *     0  u8   magic      0x50
 *     1  u8   version    1
 *     2  u8   kind       PORT_COOP_REC_DELTA (what just happened) or
 *                        PORT_COOP_REC_FULL (the whole image, for a joiner)
 *     3  u8   reserved   0
 *     4  u16  vitality   DELTA: increment; FULL: the count
 *     6  u16  story      gAIKirbyState
 *     8  u16  nruns
 *    10  runs: u16 offset into the image, u8 len (1..255), len bytes (bits)
 *
 * The image (PORT_COOP_WORLD_BYTES): gUnk_02028BF0 @0 (32), gUnk_02028C10
 * @32 (128), gVisitedDoors[0..286] @160 (574), gTreasures chest bits @734
 * (16), shards @750, spray paints @754, sound player/sheets @758, big
 * chests @762, shadow Kirby encounters @766, Mirra encounters @770 (4 each).
 *
 * Call PortCoopWorldShare(1) once; then, every frame, send whatever
 * PortCoopWorldTake returns to everyone, and hand every record received to
 * PortCoopWorldApply.  When someone joins, send them PortCoopWorldTake(buf,
 * 1).  Records that arrive while the game cannot take them are queued. */
#define PORT_COOP_WORLD_BYTES 774
#define PORT_COOP_RECORD_MAX  1024
enum { PORT_COOP_REC_DELTA = 0, PORT_COOP_REC_FULL = 1 };
enum {                                /* PortCoopWorldApply / News bits     */
    PORT_COOP_NEWS_SHARD     = 1 << 0,
    PORT_COOP_NEWS_BIGCHEST  = 1 << 1,   /* an area map                      */
    PORT_COOP_NEWS_SPRAY     = 1 << 2,
    PORT_COOP_NEWS_MUSIC     = 1 << 3,
    PORT_COOP_NEWS_VITALITY  = 1 << 4,
    PORT_COOP_NEWS_SWITCH    = 1 << 5,   /* a big switch / world state slot  */
    PORT_COOP_NEWS_CHEST     = 1 << 6,   /* any chest (also set with 1..4)   */
    PORT_COOP_NEWS_MAP       = 1 << 7,   /* doors and rooms on the area map  */
    PORT_COOP_NEWS_ENCOUNTER = 1 << 8,   /* shadow Kirby / Mirra             */
    PORT_COOP_NEWS_STORY     = 1 << 9,
    PORT_COOP_NEWS_OTHER     = 1 << 10,
};
int            PortCoopWorldBytes(void);
unsigned char *PortCoopRecordBuffer(void);   /* PORT_COOP_RECORD_MAX bytes */
void           PortCoopWorldShare(int on);
int            PortCoopWorldTracking(void);
int            PortCoopWorldTake(unsigned char *out, int full);
int            PortCoopWorldApply(const unsigned char *rec, int len);
int            PortCoopWorldNews(void);
int            PortCoopWorldCount(int kind);
int            PortCoopWorldVitalityIds(unsigned char *out, int max);
void           PortCoopWorldFrame(void);     /* from PortCoopFrameTop */

/* --- stage 3: shared enemies (platform/coop_enemy.c) -----------------------
 *
 * The page picks one owner per room (the lowest member id among the players
 * in it).  The owner sends PortCoopEnemyCollect's snapshot to the others in
 * the room a few dozen times a second; they hand it to PortCoopEnemySetPoses
 * and their copies of those enemies follow it.  Events (PortCoopEnemyTake ->
 * everyone -> PortCoopEnemyEvent): a kill kills the enemy everywhere, a hit
 * on a puppet takes hp off the owner's copy.
 *
 * Snapshot: u16 room, u16 n, then n records of PORT_COOP_EN_REC bytes:
 *   u16 idx (record in the room's object list), i32 x, i32 y (Q8), s16 hp,
 *   u8 anim (Object.unk83), u8 flags (PORT_COOP_EN_*). */
struct Object;
#define PORT_COOP_EN_REC 14
enum { PORT_COOP_EN_LEFT = 1 << 0, PORT_COOP_EN_HIDDEN = 1 << 1 };
enum {
    PORT_COOP_EV_KILL = 1, PORT_COOP_EV_HIT = 2,          /* idx: object-list index */
    PORT_COOP_EV_BOSS_HIT = 3, PORT_COOP_EV_BOSS_KILL = 4, /* idx: index, or 0x8000|type */
    PORT_COOP_EV_BOSS_HP = 5,                              /* dmg field: the hp        */
};
void PortCoopEnemyShare(int on);
int  PortCoopEnemyCollect(unsigned char *out, int maxBytes);
void PortCoopEnemySetPoses(const unsigned char *buf, int len);
int  PortCoopEnemyTake(unsigned char *out, int max);      /* 8-byte events */
void PortCoopEnemyEvent(int kind, int room, int idx, int dmg, int from);
int  PortCoopEnemyStat(int i);
int  PortCoopEnemyList(unsigned char *out, int max);     /* 20-byte records */
void PortCoopEnemyFrame(void);                            /* from PortCoopFrameTop */
/* ObjectMain's hook: gPortCoopEnemyOn is 0 unless an enemy puppet is live. */
extern u8 gPortCoopEnemyOn;
int  PortCoopEnemyStep(struct Object *obj);

/* --- spawned objects: projectiles, effects, particles (platform/coop_fx.c) --
 *
 * A puppet only draws, so nothing it would have spawned exists in another
 * game.  The game that runs a spawner reports what its spawned objects draw
 * each frame; the others draw copies ("ghosts") through the game's own sprite
 * code.  Reported: whatever the local Kirby, a CPU Kirby this game runs, or a
 * shared enemy running its own state spawned.  With the sprites go the
 * shared enemies' shots as hazards (they hurt the players of the other
 * games) and the sounds those games cannot make themselves.
 *
 * Frame (PortCoopFxTake / PortCoopFxSet): u16 room, u8 n, u8 nh, then n
 * sprite records, nh hazard records, and (when the frame is long enough for
 * it) u8 ns and ns sound records.  A sprite, PORT_COOP_FX_REC bytes:
 *    0 u16 animId    2 u8 variant    3 u8 id (stable while the object lives)
 *    4 s16 x, 6 s16 y  world pixels of the sprite's anchor
 *    8 u16 animCursor (the script position after the frame shown)
 *   10 u8  pal: 0..3 Kirby slot k's palette, 4..7 its hat's (the page remaps
 *          the sender's slots to the puppets showing them; 0xFF = none),
 *          0x0E/0x0F the fixed ones, 0x10 the registry palette in palKey
 *   11 u8  OAM layer (Sprite.unk14 >> 6)
 *   12 u16 Sprite.unk8 >> 7 (flip, priority, blend)
 *   14 u16 palKey: animation (12 bits) | variant << 12, for pal 0x10
 *   16 u16 src: bits 0-1 kind (0 a Kirby: bits 2-3 its slot on the sender;
 *          1 a shared enemy: bits 2-15 its object-list index)
 * A hazard, PORT_COOP_FX_HAZ_REC bytes (an object in the sender's collision
 * lists that hits Kirbys, spawned by a shared enemy; ObjectBase fields):
 *    0 s16 x, 2 s16 y  world pixels (x >> 8, y >> 8)
 *    4 s8  unk38, unk39, unk3A, unk3B   the hitbox
 *    8 s8  sprite.unk20[0].unk4..unk7   the sprite's hitbox (flags bit 1)
 *   12 u8  flags: bit 0 facing left, bit 1 sprite hitbox valid
 *   13 s8  unk63 (damage)   14 s16 unk64   16 s16 unk66 (knockback)
 *   18 u32 unk68 (attack flags)   22 u16 the enemy's object-list index
 * A sound, PORT_COOP_FX_SND_REC bytes, repeated in the frames of the next
 * PORT_COOP_FX_SND_KEEP frames so a frame the receiver skips loses none:
 *    0 u16 song number | 0x4000 (PlaySfxAlt) | 0x8000 (a stop)
 *    2 u16 room   4 u16 src (as a sprite's)   6 u16 sender frame stamp */
#define PORT_COOP_FX_REC     18
#define PORT_COOP_FX_MAX     32
#define PORT_COOP_FX_HAZ_REC 24
#define PORT_COOP_FX_HAZ_MAX 10
#define PORT_COOP_FX_SND_REC 8
#define PORT_COOP_FX_SND_MAX 16
#define PORT_COOP_FX_SND_KEEP 8
#define PORT_COOP_FX_BYTES   (4 + PORT_COOP_FX_MAX * PORT_COOP_FX_REC \
                              + PORT_COOP_FX_HAZ_MAX * PORT_COOP_FX_HAZ_REC \
                              + 1 + PORT_COOP_FX_SND_MAX * PORT_COOP_FX_SND_REC)
#define PORT_COOP_FX_SOURCES 8
extern u8 gPortCoopFxOn;
struct Task;
struct Sprite;
struct ObjectBase;
void           PortCoopFxShare(int on);
unsigned char *PortCoopFxBuffer(void);        /* PORT_COOP_FX_BYTES scratch */
int            PortCoopFxTake(unsigned char *out, int max);
void           PortCoopFxSet(int src, const unsigned char *buf, int len);
int            PortCoopFxStat(int i);
void           PortCoopFxFrame(void);         /* from PortCoopFrameTop */
void           PortCoopFxCapture(struct Sprite *sprite);   /* DisplaySprite */
void           PortCoopFxTaskCreated(struct Task *task);   /* TaskCreate    */
void           PortCoopFxPalette(u32 slot, u32 key, u32 variant); /* sub_0803DFAC */
/* Hazards (object_collision.c): the scan at the top of the collision task,
 * the hits after the game's own; PortCoopHazardTouch is the game's reaction
 * to a projectile, for a Kirby here that the hazard overlaps. */
void           PortCoopFxHazardScan(void);
void           PortCoopFxHazardHits(void);
int            PortCoopHazardTouch(struct ObjectBase *attack);
/* Sounds: PlaySfx/PlaySfxAlt (data.h) and m4aSongNumStop (m4a.c). */
void           PortCoopFxSound(u32 kind, u32 who, u32 room, const void *obj, u32 num, u32 alt);
void           PortCoopFxSoundStop(u32 num);
int            PortCoopEnemyIsPuppet(const struct Object *obj);  /* coop_enemy.c */
int            PortCoopEnemyPuppetAt(u32 room, u32 idx);

/* --- CPU Kirbys (coop.c) ---------------------------------------------------
 * One game runs the CPU Kirbys for everyone (the page picks it: the lowest
 * member id among the players in a level); it sends their poses with its
 * own, and every other game shows them in puppet slots like any remote
 * player.  PortCoopSlotPose is PortCoopLocalPose for slot 1..3: 0 when the
 * slot is a puppet or the game is not ready. */
int            PortCoopSlotPose(int slot, unsigned char *out);
/* The phone.  A player whose game does not run the CPU Kirbys calls them
 * like anyone: their game reports the call (PortCoopTakeCall: 1 once per
 * call, room and Q8 position in out[0..2]), the page sends it to the game
 * that runs them, and that game brings each of its CPU Kirbys not already
 * there into the caller's room next to them (PortCoopAnswerCall) -- where
 * the caller's game sees them arrive as puppets and ends the call as it
 * would have. */
void           PortCoopPhoneCall(struct Kirby *kirby);   /* kirby.c, sub_080572A0 */
int            PortCoopTakeCall(int *out);
int            PortCoopAnswerCall(int room, int x, int y);

/* --- game-side hooks ------------------------------------------------------
 * gPortCoopPuppets: bit n set while Kirby slot n is a puppet.  0 whenever
 * co-op is off, so every hook in game/src is one load and a branch. */
extern u8 gPortCoopPuppets;
#define PORT_COOP_IS_PUPPET(i) (gPortCoopPuppets & (1u << (i)))

/* platform/coop_objects.c: room object records and live objects. */
struct Object;
struct ObjectTemplate;
const struct ObjectTemplate *PortCoopRoomObjects(u32 room, u32 *count);
int  PortCoopObjectIndex(const struct Object *obj);   /* -1: not from the list */
int  PortCoopForEachObject(int (*fn)(struct Object *, void *), void *arg);

struct Kirby;
struct ObjectBase;
void PortCoopPuppetStep(struct Kirby *kirby);  /* replaces sub_0803EE18's body */
/* Enemy targeting (kirby.c): a regular enemy picks among the local Kirby and
 * the puppets standing in its room -- players first, then CPU Kirbys. */
int  PortCoopTargetsPuppets(const struct ObjectBase *o);
struct Kirby *PortCoopFindTarget(const struct ObjectBase *o, int xOnly);
void PortCoopSetPuppetCpu(int slot, int cpu);  /* exported: 1 = a CPU Kirby */
int  PortCoopPuppetAbsent(u32 slot);           /* HUD/map: show nothing at all */
u16  PortCoopMapRoom(u32 slot);                /* pause map room, 0xFFFF = none */
void PortCoopFrameTop(void);                   /* from PortPresentFrame */
void PortCoopTestKeys(u32 frame, u16 *keys);   /* self-test only, key latch */
void PortCoopSelfTest(int start, int mix, int leave, int pause); /* exported */

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_COOP_H */
