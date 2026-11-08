#ifndef GUARD_MIRROR_DOOR_H
#define GUARD_MIRROR_DOOR_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct MirrorDoor
{
    struct Object obj;
    PTR32(struct EffectObject) unkB4[2];
    u16 unkBC;
    u16 unkBE;
    u16 unkC0;
    u8 unkC2;
}; /* size = 0xC4 */

void *CreateMirrorDoor(struct ObjectTemplate *, u8);

void sub_0811A624(struct MirrorDoor *);

extern const struct AnimInfo gUnk_08357BEC[];

#ifdef __cplusplus
}
#endif

#endif
