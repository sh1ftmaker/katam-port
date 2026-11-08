#ifndef GUARD_MULTI_BOOT_UTIL_H
#define GUARD_MULTI_BOOT_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "multi_boot.h"

struct MultiBootDataRecv {
    vu16 data[1 + MULTIBOOT_NCHILD];
};

struct MultiBootStruct { // TODO: needs a better name
    vu8 unk00;
    vu8 unk01;
    vu8 unk02;
    vu8 unk03;
    vu8 unk04;
    vu16 unk06;
    vu16 unk08;
    vu16 unk0A;
    vu16 unk0C;
    vu16 unk0E;
    PTR32(const u8) volatile srcp;
    PTR32(const u8) volatile endp;
    vs32 length;
    u16 filler1C;
    vu16 unk1E[MULTIBOOT_NCHILD];
    vu8 unk24;
    vu8 unk25;
    vu8 unk26;
    vu16 unk28;
    vu8 unk2A;
    vu8 unk2B;
};

/* PORT: replaced by port/ram_symbols.h -- extern IntrFunc gTimer3IntrBkp; */
/* PORT: replaced by port/ram_symbols.h -- extern struct MultiBootDataRecv gMultiBootDataRecv; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gUnk_03000480; // always zero. debug variable? */
/* PORT: replaced by port/ram_symbols.h -- extern IntrFunc gMultiSioIntrBkp; */
/* PORT: replaced by port/ram_symbols.h -- extern struct MultiBootStruct gMultiBootStruct; */
/* PORT: replaced by port/ram_symbols.h -- extern struct MultiBootParam gMultiBootParam; */
/* PORT: replaced by port/ram_symbols.h -- extern vs32 gUnk_0300050C; */

void sub_0803024C(void);
void sub_08030B38(void);
void MultiBootInitWithParams(const void *start, const void *end);
void sub_08030C1C(void);
void sub_08030C40(u16);

#ifdef __cplusplus
}
#endif

#endif // GUARD_MULTI_BOOT_UTIL_H
