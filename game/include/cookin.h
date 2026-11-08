#ifndef GUARD_COOKIN_H
#define GUARD_COOKIN_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateCookin(struct ObjectTemplate *, u8);
void *CreateCookinPan(struct ObjectTemplate *, u8);
bool32 sub_080B6368(struct Object *, struct Kirby *);
void sub_080B681C(struct Object *);
void sub_080B6880(struct Object *);

#ifdef __cplusplus
}
#endif

#endif
