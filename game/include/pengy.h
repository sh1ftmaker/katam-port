#ifndef GUARD_PENGY_H
#define GUARD_PENGY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreatePengy(struct ObjectTemplate*, u8);
void sub_080BBBF8(struct Object*);

extern const struct AnimInfo gUnk_083547E4[];

#ifdef __cplusplus
}
#endif

#endif
