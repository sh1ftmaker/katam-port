#ifndef GUARD_INTRO_H
#define GUARD_INTRO_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "sprite.h"
#include "data.h"
#include "code_0814F274.h"

struct Unk_08387814 {
    u32 unk0;
    u8 unk4;
    u8 unk5;
    u8 unk6;
    u8 unk7;
    u8 unk8;
    u8 unk9;
    u16 unkA;
    s16 unkC;
    u8 unkE;
    u8 unkF;
    u16 unk10;
    u16 unk12;
    u8 unk14;
    u8 unk15;
    s16 unk16;
}; /* size = 0x18 */

struct Unk_08145B64;

struct Unk_08145B64_5EC_24_Pat2 {
            u8 unk0;
            u8 unk1;
            u16 unk2;
            u16 unk4;
            u8 unk6;
            u8 unk7;
            u8 unk8;
            u8 unk9;
            u8 unkA;
            u8 unkB;
            u8 unkC;
        };

struct Unk_08145B64_5EC_24_Pat3 {
            u16 unk0;
            u16 unk2;
            u8 unk4;
            u8 unk5;
            u16 unk6;
            u16 unk8;
            u8 unkA;
            u8 unkB;
            u8 unkC;
        };

struct Unk_08145B64_5EC_24_Pat4 {
            s16 unk0;
            s16 unk2;
            u16 unk4;
            u16 unk6;
            u8 unk8;
            u8 unk9;
            u8 unkA;
            u8 unkB;
            u8 unkC;
            u8 unkD;
            u8 unkE;
            u8 unkF;
        };

struct Unk_08145B64_5EC_24_Pat5 {
            s16 unk0;
            s16 unk2;
            u8 unk4;
            u8 unk5;
            u16 unk6;
            u16 unk8;
            u8 unkA;
            u8 unkB;
            u8 unkC;
        };

union Unk_08145B64_5EC_24 { // TODO: I'm confused
        s16 pat1[8];
        struct Unk_08145B64_5EC_24_Pat2 pat2;
        struct Unk_08145B64_5EC_24_Pat3 pat3;
        struct Unk_08145B64_5EC_24_Pat4 pat4;
        struct Unk_08145B64_5EC_24_Pat5 pat5;
    };

struct Unk_08145B64_5EC {
    u8 unk0;
    s8 unk1;
    u8 unk2;
    s8 unk3; // index in Unk_08145B64::unk48
    s8 unk4; // index in Unk_08145B64::unk5EC
    u8 unk5;
    u8 unk6;
    u8 unk7;
    u16 unk8;
    s16 unkA;
    s16 unkC;
    s16 unkE;
    s16 unk10;
    PTR32(struct Unk_08145B64_5EC) unk14;
    PTR32(struct Unk_08145B64_5EC) unk18;
    PTR32_FN(void, unk1C, (struct Unk_08145B64_5EC *));
    PTR32(struct Unk_08145B64) unk20;
    union Unk_08145B64_5EC_24 unk24;
}; /* size = 0x34 */

struct Unk_08145B64 {
    u32 unk0;
    u32 unk4;
    u32 unk8;
    u32 unkC;
    u8 unk10;
    u8 unk11;
    u8 unk12[0x10];
    struct Object11 unk24;
    u16 filler30;
    u16 unk32[2];
    u16 unk36[2];
    PTR32_FN(void, unk3C[2], (struct Unk_08145B64 *, u8));
    s16 unk44;
    s16 unk46;
    struct Sprite unk48[0x20];
    PTR32(struct Sprite) unk548[0x20];
    u8 unk5C8;
    u8 unk5C9;
    u8 unk5CA;
    u8 unk5CB;
    u8 unk5CC[0x20];
    struct Unk_08145B64_5EC unk5EC[0x40];
    PTR32(struct Unk_08145B64_5EC) unk12EC[0x40];
    PTR32(struct Unk_08145B64_5EC) unk13EC;
    PTR32(struct Unk_08145B64_5EC) unk13F0;
    u8 unk13F4;
    u8 unk13F5;
    s8 unk13F6;
    PTR32_FN(bool32, unk13F8, (struct Unk_08145B64 *));
    s16 unk13FC;
    PTR32_FN(void, unk1400, (struct Unk_08145B64 *));
    u32 filler1404;
    PTR32_FN(void, unk1408, (struct Unk_08145B64 *));
    u16 unk140C;
    s16 unk140E;
    PTR32_FN(void, unk1410, (struct Unk_08145B64 *));
    s8 unk1414;
    s8 unk1415;
    u8 unk1416;
    u8 unk1417;
    u8 unk1418;
    u8 unk1419;
}; /* size = 0x141C */

typedef void (*const PortFnpUnk_08387348_unkC)(struct Unk_08145B64_5EC *);

struct Unk_08387348 {
    u8 unk0;
    s8 unk1;
    u8 unk2;
    u8 unk3;
    PTR32_FN(void, unk4, (struct Unk_08145B64_5EC *, const struct Unk_08387814 *));
    PTR32_FN(void, unk8, (struct Unk_08145B64_5EC *));
    PTR32(PortFnpUnk_08387348_unkC) unkC;
}; /* size = 0x10 */

void sub_08145B64(u16);

extern const u16 gUnk_083877EE[];

#ifdef __cplusplus
}
#endif

#endif
