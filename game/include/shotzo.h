#ifndef GUARD_SHOTZO_H
#define GUARD_SHOTZO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateShotzo(struct ObjectTemplate *, u8);
void *CreateShotzoBullet(struct ObjectTemplate *, u8);

#ifdef __cplusplus
}
#endif

#endif
