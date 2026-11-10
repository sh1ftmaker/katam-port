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
 *    24  u8   color      the sender's colour; carried, not applied (a puppet
 *                        keeps its slot's colour, like a CPU helper)
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

/* --- game-side hooks ------------------------------------------------------
 * gPortCoopPuppets: bit n set while Kirby slot n is a puppet.  0 whenever
 * co-op is off, so every hook in game/src is one load and a branch. */
extern u8 gPortCoopPuppets;
#define PORT_COOP_IS_PUPPET(i) (gPortCoopPuppets & (1u << (i)))

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
