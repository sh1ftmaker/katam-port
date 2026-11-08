#ifndef GUARD_METAL_GUARDIAN_H
#define GUARD_METAL_GUARDIAN_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreateMetalGuardian(struct ObjectTemplate*, u8);
void sub_080BE228(struct Object*);

extern const struct AnimInfo gUnk_08354A58[];

#ifdef __cplusplus
}
#endif

#endif
