#ifndef GUARD_CHEST_H
#define GUARD_CHEST_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct Chest
{
    struct Object obj;
    u8 fillerB4[0x28];
    u32 unkDC;
    u16 unkE0; // item id
    u16 unkE2; // treasure index
    u16 unkE4; // player id that opened the chest
}; /* size = 0xE8 */

struct ChestItemPopup
{
    struct EffectObject effectObject;
    PTR32_FN(void, unk48, (struct ChestItemPopup *));
    PTR32(struct Chest) unk4C;
    u16 unk50; // item id
    u8 filler52[6];
}; /* size = 0x58 */

void *CreateChest(struct ObjectTemplate *, u8);
void sub_0800BD4C(struct Chest *);

#ifdef __cplusplus
}
#endif

#endif // GUARD_CHEST_H
