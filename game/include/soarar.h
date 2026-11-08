#ifndef GUARD_SOARAR_H
#define GUARD_SOARAR_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateSoarar(struct ObjectTemplate *, u8);
void sub_080AB8DC(struct Object *);
void sub_080AC380(struct Object *);
void sub_080AC45C(struct Object *);
void sub_080AC8CC(struct Object *);
bool32 sub_080AC5E0(struct Object *, struct ObjectHeader *);

#ifdef __cplusplus
}
#endif

#endif // GUARD_SOARAR_H
