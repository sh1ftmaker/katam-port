#ifndef GUARD_BANG_BANG_H
#define GUARD_BANG_BANG_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreateBangBang(struct ObjectTemplate*, u8);

void sub_080C70D8(struct Object*);

extern const struct AnimInfo gUnk_083556D0[];

#ifdef __cplusplus
}
#endif

#endif
