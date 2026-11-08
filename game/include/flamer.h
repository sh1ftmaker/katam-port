#ifndef GUARD_FLAMER_H
#define GUARD_FLAMER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateFlamer(struct ObjectTemplate *, u8);
void sub_080BA334(struct Object *);

extern const struct AnimInfo gUnk_08354670[];

#ifdef __cplusplus
}
#endif

#endif
