#ifndef GUARD_DEMO_H
#define GUARD_DEMO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "task.h"

struct DemoStruct;

typedef void (*CreateFunc)(void);
typedef void (*DemoFunc)(struct DemoStruct*);

struct DemoStruct {
    PTR32_TD(DemoFunc) unk0;
    u16 unk4;
    u8 filler6[2];
};

/* PORT: replaced by port/ram_symbols.h -- extern u8 gNumHumanPlayers; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_02038990[4][0x402]; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03000554; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03000558; */

extern void CreateDemo(u16);
extern u16 sub_08025F2C(void);

#ifdef __cplusplus
}
#endif

#endif
