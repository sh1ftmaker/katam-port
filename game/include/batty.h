#ifndef GUARD_BATTY_H
#define GUARD_BATTY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreateBatty(struct ObjectTemplate* arg0, u8 arg1);
void sub_080BD3CC(struct Object*);

extern const struct AnimInfo gUnk_08354960[];

#ifdef __cplusplus
}
#endif

#endif
