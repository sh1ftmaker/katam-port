#ifndef GUARD_CODE_080257A8_H
#define GUARD_CODE_080257A8_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct Unk_080257A8 {
    /* 0x00 */ struct Background unk0;
    /* 0x40 */ struct Background unk40;
    /* 0x80 */ PTR32_FN(void, unk80, (struct Unk_080257A8 *));
    /* 0x84 */ u16 unk84;
}; /* size = 0x88 */

void sub_08025A80(void);
void sub_08025AD0(void);

#ifdef __cplusplus
}
#endif

#endif
