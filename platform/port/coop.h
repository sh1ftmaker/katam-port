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
 *    24  u8   color      the sender's colour (0..13), applied to the puppet
 *    25  u8   animSpeed  base.sprite.unk1C (0x10 = normal speed)
 *    26  u8   frame      base.header.unk1 (frames into the animation)
 *    27  u8   flags2     PORT_COOP_F2_*
 *    28  s8   hp         display only
 *    29  u8   lives      display only
 *    30  u16  seq        sender's pose counter (wraps)
 *    32  u32  reserved   0
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
void PortCoopPuppetStep(struct Kirby *kirby);  /* replaces sub_0803EE18's body */
int  PortCoopPuppetAbsent(u32 slot);           /* HUD/map: show nothing at all */
u16  PortCoopMapRoom(u32 slot);                /* pause map room, 0xFFFF = none */
void PortCoopFrameTop(void);                   /* from PortPresentFrame */
void PortCoopTestKeys(u32 frame, u16 *keys);   /* self-test only, key latch */
void PortCoopSelfTest(int start, int mix, int leave, int pause); /* exported */

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_COOP_H */
