#ifndef GUARD_HAMMER_PEG_H
#define GUARD_HAMMER_PEG_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateHammerPeg(struct ObjectTemplate *, u8);

void sub_08025320(struct Object *);

extern const struct AnimInfo gUnk_082DEB04[];

#ifdef __cplusplus
}
#endif

#endif
