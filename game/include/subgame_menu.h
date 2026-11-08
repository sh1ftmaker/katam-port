#ifndef GUARD_SUBGAME_MENU_H
#define GUARD_SUBGAME_MENU_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "task.h"
#include "sprite.h"
#include "data.h"

struct SubGameMenu;

typedef void (*SubGameMenuFunc)(struct SubGameMenu*);

struct SubGameMenu {
    struct Background unk0[2];
    struct Sprite unk80[5];
    u32 unk148;
    u32 unk14C;
    u32 unk150;
    PTR32_TD(SubGameMenuFunc) unk154;
    u16 unk158[3][4];
    u16 unk170;
    u16 unk172;
    u16 unk174;
    u16 unk176;
    u16 unk178;
}; /* size = 0x17C */

void sub_0801E630(s32);
void sub_0801E6C4(s32);
void sub_0801E754(s32);
void sub_0801FCA8(s32);

extern const u16 gUnk_082DE69C[];

#ifdef __cplusplus
}
#endif

#endif // GUARD_SUBGAME_MENU_H
