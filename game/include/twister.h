#ifndef GUARD_TWISTER_H
#define GUARD_TWISTER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreateTwister(struct ObjectTemplate*, u8);
void sub_080BC440(struct Object*);

extern const struct AnimInfo gUnk_08354928[];

#ifdef __cplusplus
}
#endif

#endif
