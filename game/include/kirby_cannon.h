#ifndef GUARD_KIRBY_CANNON_H
#define GUARD_KIRBY_CANNON_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct KirbyCannon {
    /* 0x00 */ struct Object obj;
    /* 0xB4 */ u8 unkB4;
    /* 0xB5 */ u8 unkB5;
    /* 0xB6 */ u8 unkB6;
    /* 0xB7 */ u8 unkB7;
#if KATAM_KIRBYS > 4
    /* PORT: the Kirbys inside, one bit each: a u16 for up to 16 (it takes
     * the padding byte at 0xB9; the size is unchanged). */
    /* 0xB8 */ u16 unkB8;
#else
    /* 0xB8 */ u8 unkB8;
#endif
    /* 0xBA */ u16 unkBA;
    /* 0xBC */ s8 unkBC;
    /* 0xBD */ s8 unkBD;
}; /* size = 0xC0 */

extern const struct AnimInfo gUnk_08357E64[];

void *CreateKirbyCannon(struct ObjectTemplate *, u8);
void sub_08121AF8(struct KirbyCannon *);

#ifdef __cplusplus
}
#endif

#endif
