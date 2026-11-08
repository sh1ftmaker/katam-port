#ifndef GUARD_EIGHT_DIR_CANNON_H
#define GUARD_EIGHT_DIR_CANNON_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"
#include "kirby.h"
#include "object.h"

struct EightDirCannon {
    struct Object obj;
    u8 unkB4;
    u8 unkB5;
    u8 unkB6;
    u8 unkB7;
    u8 unkB8;
    u8 unkB9;
    u8 unkBA;
    u8 unkBB;
    PTR32(struct Kirby) unkBC;
}; /* size = 0xC0 */

void *CreateEightDirCannon(struct ObjectTemplate *, u8);
void EightDirCannonSetFunc(struct EightDirCannon *);

/* PORT: replaced by port/rom_data.h -- extern const u8 gUnk_08357EA4[8][10]; */

#ifdef __cplusplus
}
#endif

#endif
