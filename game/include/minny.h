#ifndef GUARD_MINNY_H
#define GUARD_MINNY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"
#include "object.h"

void* CreateMinny(struct ObjectTemplate* arg0, u8 arg1);
void MinnyStart(struct Object* arg0);

extern const struct AnimInfo gUnk_08354074[];

#ifdef __cplusplus
}
#endif

#endif
