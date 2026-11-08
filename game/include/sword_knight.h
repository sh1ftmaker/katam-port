#ifndef GUARD_SWORD_KNIGHT_H
#define GUARD_SWORD_KNIGHT_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"
#include "object.h"

void *CreateSwordKnight(struct ObjectTemplate *, u8);
void sub_080B3A18(struct Object *);

#ifdef __cplusplus
}
#endif

#endif
