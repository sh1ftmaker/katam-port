#ifndef GUARD_LOGO_H
#define GUARD_LOGO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "data.h"
#include "global.h"
#include "task.h"

struct LogoStruct;

typedef void (*LogoFunc) (struct LogoStruct*);

struct LogoStruct {
    u32 unk0;
    u16 unk4;
    u8 filler6[2];
    PTR32_TD(LogoFunc) unk8;
    s16 unkC;
    u16 unkE;
};

extern void CreateLogo(void);

#ifdef __cplusplus
}
#endif

#endif
