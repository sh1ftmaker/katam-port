#ifndef GUARD_LAVA_WALL_H
#define GUARD_LAVA_WALL_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"
#include "task.h"

void *CreateLavaWall(struct ObjectTemplate *, u8);
void sub_0811C758(struct Object *);

extern const struct AnimInfo gUnk_08357C5C[];

#ifdef __cplusplus
}
#endif

#endif
