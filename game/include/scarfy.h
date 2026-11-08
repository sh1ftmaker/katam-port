#ifndef GUARD_SCARFY_H
#define GUARD_SCARFY_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"
#include "object.h"

void* CreateScarfy(struct ObjectTemplate* arg0, u8 arg1);

void sub_080A80EC(struct Object *);

extern const struct AnimInfo gUnk_083534F8[];

#ifdef __cplusplus
}
#endif

#endif
