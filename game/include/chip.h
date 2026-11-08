#ifndef GUARD_CHIP_H
#define GUARD_CHIP_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

void* CreateChip(struct ObjectTemplate* arg0, u8 arg1);
void sub_080AB720(struct Object *);

extern const struct AnimInfo gUnk_08353928[];

#ifdef __cplusplus
}
#endif

#endif
