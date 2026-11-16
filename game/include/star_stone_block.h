#ifndef GUARD_STAR_STONE_BLOCK_H
#define GUARD_STAR_STONE_BLOCK_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"
#include "constants/object_types.h"

struct StarStoneBlock {
    /* 0x00 */ struct Object obj;
#if KATAM_KIRBYS > 4
    /* PORT: obj.unk90 holds the contact bits, 2 a Kirby, in a u8: Kirbys
     * 0..3 only.  The wide build keeps Kirbys 4..'s in the first word of
     * the console's unused filler (task memory, so sim state): bits 2*(i-4)
     * and 2*(i-4)+1 as unk90's for i (object_collision.c), set, cleared and
     * read exactly as unk90 is for 0..3 (ONE-WORLD-16.md "Slot audit"). */
    union {
        /* 0xB4 */ u8 fillerB4[0x2C];
        u32 portContactHi;
    };
#else
    /* 0xB4 */ u8 fillerB4[0x2C];
#endif
    /* 0xE0 */ s8 unkE0;
    /* 0xE1 */ u8 unkE1;
    /* 0xE2 */ u8 unkE2;
    /* 0xE3 */ u8 unkE3;
}; /* size = 0xE4 */

#if KATAM_KIRBYS > 4
/* The object types built on struct StarStoneBlock (their Create* call
 * InitObject on it): the ones whose contact with Kirbys 4.. is kept.
 * Kind 1 only: object_collision.c also writes unk90 on what is not an object. */
#define PORT_IS_STAR_STONE_BLOCK(obj) ((obj)->base.header.kind == 1 && ((obj)->type == OBJ_STAR_STONE_BLOCK \
    || (obj)->type == OBJ_LARGE_STAR_STONE_BLOCK_7D || (obj)->type == OBJ_LARGE_STAR_STONE_BLOCK_7E \
    || (obj)->type == OBJ_LARGE_STAR_STONE_BLOCK_88 || (obj)->type == OBJ_LARGE_STAR_STONE_BLOCK_89))
/* Where obj.unk90 is cleared: the same for Kirbys 4.. */
#define PORT_STAR_STONE_CONTACT_CLEAR(obj) do { if (PORT_IS_STAR_STONE_BLOCK(obj)) \
    ((struct StarStoneBlock *)(obj))->portContactHi = 0; } while (0)
#else
#define PORT_STAR_STONE_CONTACT_CLEAR(obj) do { } while (0)
#endif

extern const struct AnimInfo gUnk_08357CC8[];

void *CreateStarStoneBlock(struct ObjectTemplate *, u8);
void sub_0811EC80(struct StarStoneBlock *);
void sub_0811F0CC(struct StarStoneBlock *);

#ifdef __cplusplus
}
#endif

#endif
