#ifndef GUARD_WHEELIE_H
#define GUARD_WHEELIE_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreateWheelie(struct ObjectTemplate*, u8);
void sub_080BC594(struct Object*);

extern const struct AnimInfo gUnk_0835493C[];

#ifdef __cplusplus
}
#endif

#endif
