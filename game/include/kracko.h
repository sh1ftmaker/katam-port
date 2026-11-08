#ifndef GUARD_KRACKO_H
#define GUARD_KRACKO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"

struct Kracko
{
    struct Object obj;
    PTR32(struct EffectObject) unkB4;
    PTR32(struct EffectObject) unkB8;
    PTR32(struct Object6) unkBC;
    u8 unkC0;
    u8 unkC1;
    u8 unkC2;
    u8 unkC3;
}; /* size = 0xC4 */

void *CreateKracko(struct ObjectTemplate *, u8);
void sub_080DBE3C(struct Kracko *);

extern const struct AnimInfo gUnk_083563EC[];

#ifdef __cplusplus
}
#endif

#endif
