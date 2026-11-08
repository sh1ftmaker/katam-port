#ifndef GUARD_DARK_META_KNIGHT_H
#define GUARD_DARK_META_KNIGHT_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct DarkMetaKnight
{
    struct Object obj;
    PTR32(struct ObjectBase) unkB4;
    PTR32(struct EffectObject) unkB8;
    PTR32(struct EffectObject) unkBC;
    u8 unkC0;
    u8 unkC1;
    u8 unkC2;
    u8 unkC3;
    u8 unkC4;
    u8 unkC5;
    u8 unkC6;
    u8 unkC7;
    u8 unkC8;
    u8 unkC9;
    u8 unkCA;
    s16 unkCC;
}; /* size = 0xD0 */

void *CreateDarkMetaKnight(struct ObjectTemplate *, u8);
void sub_080FFEA4(struct DarkMetaKnight *);

#ifdef __cplusplus
}
#endif

#endif
