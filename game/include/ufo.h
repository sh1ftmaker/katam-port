#ifndef GUARD_UFO_H
#define GUARD_UFO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateUFO(struct ObjectTemplate *, u8);

void sub_080C4ACC(struct Object *);

extern const struct AnimInfo gUnk_083555C0[];

#ifdef __cplusplus
}
#endif

#endif
