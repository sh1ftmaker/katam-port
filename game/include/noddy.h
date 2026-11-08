#ifndef GUARD_NODDY_H
#define GUARD_NODDY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateNoddy(struct ObjectTemplate *, u8);

void sub_080C22E4(struct Object *);

extern const struct AnimInfo gUnk_0835544C[];

#ifdef __cplusplus
}
#endif

#endif
