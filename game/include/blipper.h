#ifndef GUARD_BLIPPER_H
#define GUARD_BLIPPER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateBlipper(struct ObjectTemplate *, u8);

void sub_080A712C(struct Object *);

extern const struct AnimInfo gUnk_08353448[];

#ifdef __cplusplus
}
#endif

#endif
