#ifndef GUARD_WADDLE_DEE_H
#define GUARD_WADDLE_DEE_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"
#include "functions.h"

void* CreateWaddleDee(struct ObjectTemplate* arg0, u8 arg1);
void sub_080A3CF0(struct Object*);
void WaddleDee37ChooseXSpeed(struct Object *);

extern const struct AnimInfo gUnk_08353408[];

#ifdef __cplusplus
}
#endif

#endif
