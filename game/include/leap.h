#ifndef GUARD_LEAP_H
#define GUARD_LEAP_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateLeap(struct ObjectTemplate *, u8);
bool32 sub_080B0758(struct Object *, struct Kirby *);
void sub_080B0F28(struct Object *);

#ifdef __cplusplus
}
#endif

#endif
