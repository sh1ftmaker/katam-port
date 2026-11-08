#ifndef GUARD_CPU_KIRBY_TRIGGER_H
#define GUARD_CPU_KIRBY_TRIGGER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

extern const struct AnimInfo gUnk_08357D18[];

void *CreateCPUKirbyTrigger(struct ObjectTemplate *, u8);
void sub_0812098C(struct Object *);

#ifdef __cplusplus
}
#endif

#endif
