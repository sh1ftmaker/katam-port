#ifndef GUARD_HEAVY_KNIGHT_H
#define GUARD_HEAVY_KNIGHT_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateHeavyKnight(struct ObjectTemplate *, u8);

void sub_080BFBE0(struct Object*);

extern const struct AnimInfo gUnk_08354BE4[];

#ifdef __cplusplus
}
#endif

#endif
