#ifndef GUARD_JACK_H
#define GUARD_JACK_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

void *CreateJack(struct ObjectTemplate *, u8);
void *CreateJackStar(struct ObjectTemplate *, u8);

void sub_080A8904(struct Object *);
void sub_080A8788(struct Object *);

extern const struct AnimInfo gUnk_08353648[];
extern const struct AnimInfo gUnk_08353668[];

#ifdef __cplusplus
}
#endif

#endif
