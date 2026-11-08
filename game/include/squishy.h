#ifndef GUARD_SQUISHY_H
#define GUARD_SQUISHY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateSquishy(struct ObjectTemplate *, u8);

void sub_080AF754(struct Object *);

extern const struct AnimInfo gUnk_08353C38[];

#ifdef __cplusplus
}
#endif

#endif
