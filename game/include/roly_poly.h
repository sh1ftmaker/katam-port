#ifndef GUARD_ROLY_POLY_H
#define GUARD_ROLY_POLY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"
#include "object.h"

void* CreateRolyPoly(struct ObjectTemplate*, u8);
void sub_080AD5D4(struct Object*);

extern const struct AnimInfo gUnk_083539E4[];

#ifdef __cplusplus
}
#endif

#endif

