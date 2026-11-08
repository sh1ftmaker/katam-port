#ifndef GUARD_FIRE_H
#define GUARD_FIRE_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"
#include "global.h"
#include "object.h"

void* CreateFire(struct ObjectTemplate* arg0, u8 arg1);

void FireCounterInit(struct Object* arg0);

extern const struct AnimInfo gUnk_08357BE8[];

#ifdef __cplusplus
}
#endif

#endif
