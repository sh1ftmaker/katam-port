#ifndef GUARD_SNOOTER_H
#define GUARD_SNOOTER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateSnooter(struct ObjectTemplate *, u8);
void sub_080ACBA4(struct Object *);
void sub_080ACBEC(struct Object *);
void sub_080ACC60(struct Object *);

#ifdef __cplusplus
}
#endif

#endif // GUARD_SNOOTER_H
