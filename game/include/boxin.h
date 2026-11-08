#ifndef GUARD_BOXIN_H
#define GUARD_BOXIN_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateBoxin(struct ObjectTemplate *, u8);

void sub_080B5838(struct Object *);

extern const struct AnimInfo gUnk_0835409C[];

#ifdef __cplusplus
}
#endif

#endif
