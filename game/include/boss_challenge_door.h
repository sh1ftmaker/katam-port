#ifndef GUARD_BOSS_CHALLENGE_DOOR_H
#define GUARD_BOSS_CHALLENGE_DOOR_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

struct BossChallengeDoor {
    struct Object obj;
    /* 0xB4 */ PTR32(struct EffectObject) unkB4;
    /* 0xB8 */ PTR32(struct EffectObject) unkB8;
}; /* size = 0xBC */

void *CreateBossChallengeDoor(struct ObjectTemplate *, u8);

#ifdef __cplusplus
}
#endif

#endif
