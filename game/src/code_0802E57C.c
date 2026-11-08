
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "code_0802E57C.h"
#include "code_0802F8D8.h"
#include "data.h"
#include "main.h"
#include "malloc_vram.h"
#include "random.h"
#include "constants/songs.h"
#include "gba/m4a.h"

/* PORT: replaced by port/rom_data.h -- extern const u16 *const gUnk_08D60B24[]; */
/* PORT: replaced by port/rom_data.h -- extern const u16 gUnk_082EB954[][2]; */
/* PORT: replaced by port/rom_data.h -- extern const u8 gUnk_082EB984[]; */

static void sub_0802E71C(struct Task *);
static void sub_0802E78C(void);
static void sub_0802EA4C(struct Unk_0802E57C *);
static void sub_0802EBB0(struct Unk_0802E57C *);
static void sub_0802ED44(struct Unk_0802E57C *);
static void sub_0802EDC8(struct Unk_0802E57C *);
static void sub_0802EE54(struct Unk_0802E57C *);
static void sub_0802F110(struct Unk_0802E57C *, struct Unk_0802E57C_C *);
static void sub_0802F240(struct Unk_0802E57C *, struct Unk_0802E57C_C *);
static void sub_0802F354(struct Unk_0802E57C *, struct Unk_0802E57C_C *);
static void sub_0802F464(struct Unk_0802E57C *, struct Unk_0802E57C_C *);
static void sub_0802F810(struct Unk_0802E57C *, struct Unk_0802E57C_C *);

static const s16 gUnk_082EB6DC[][2] = {
    { -0x40, 0x80 }, { 0x130, 0x80 }, { -0x60, 0x80 }, { 0x150, 0x80 },
};

static const bool32 gUnk_082EB6EC[] = { FALSE, TRUE, FALSE, TRUE };

static const struct S16Vec2 gUnk_082EB6FC = { 0x78, 0x50 };

static const struct Unk_0802E57C_174 gUnk_082EB700[] = {
    { -0x380, 0x280, 0, 0 },
    { 0x20, -0x13, 32, 0 },
    { 0xA, 0, 30, 0 },
    { 8, -0x20, 30, 0 },
    { 6, -0x30, 60, 0 },
    { 4, -0x40, 30, 0 },
    { 0, -0x50, 15, 0 },
    { 0, 0, 0, 1 },
};

static const struct Unk_0802E57C_174 gUnk_082EB740[] = {
    { 0, 0, 0, 0 },
    { 0x16, -0xC, 32, 0 },
    { -0x10, 0x18, 20, 0 },
    { -0x10, 0xC, 32, 0 },
    { -0x10, -0x14, 20, 0 },
    { -0x1C, -0x18, 10, 0 },
    { -8, -0x28, 15, 0 },
    { 0x10, 0x28, 5, 0 },
    { 0x1C, 0x28, 10, 0 },
    { 0x10, 0x28, 10, 0 },
    { 0x1C, 0x20, 10, 0 },
    { 0x20, -0x10, 10, 0 },
    { 0x1C, -0x20, 10, 0 },
    { 0x10, -0x28, 10, 0 },
    { 0x10, -0x18, 10, 0 },
    { 0, 0, 10, 0 },
    { 0x15, -0x15, 60, 0 },
    { 0, 0, 0, 1 },
};

struct Unk_0802E57C *sub_0802E57C(void) {
    struct Task *t = TaskCreate(FN(sub_0802E78C), sizeof(struct Unk_0802E57C), 0x100, TASK_USE_IWRAM, FN(sub_0802E71C));
    struct Unk_0802E57C *tmp = TaskGetStructPtr(t), *x = tmp;
    struct Sprite *s;
    u16 i;

    CpuFill16(0, x, sizeof(struct Unk_0802E57C));
    x->unk0 = t;
    x->unk4 = FN(sub_0802FD3C);
    gDispCnt |= DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP;
    s = &x->unk14C;
    SpriteInitNoFunc(s, VramMalloc(0x10), 0x280, 0x2DE, 0, 0, 0xFF, 0x10, 0xB, 0, 0, 0);
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        struct Unk_0802E57C_C *c = &x->unkC[i];

        SpriteInitNoPointer2(&c->unk0, 0, 0x100, 0x15, i, 0, 0xFF, 0x10, i + 0xC, 0, 0, 0x80000);
        SpriteInitNoFunc(&c->unk0, VramMalloc(0x10), 0x100, 0, 0, 0, 0xFF, 0x10, i + 0xC, 0, 0, 0x40000);
        c->unk2C |= 4;
        c->unk46 = i;
    }
    return x;
}

void sub_0802E6C4(struct Unk_0802E57C *x) {
    struct Sprite s;

    x->unk4 = FN(sub_0802EA4C);
    if (!x->unk8) {
        x->unk8 = VramMalloc(0x100);
        SpriteInitNoPointer(&s, x->unk8, 0, 0x292, 0, 0, 0xFF, 0x10, 0xB, 0, 0, 0);
    }
}

static void sub_0802E71C(struct Task *t) {
    struct Unk_0802E57C *tmp = TaskGetStructPtr(t), *x = tmp;
    u16 i;

    if (tmp->unk8) {
        VramFree(tmp->unk8);
        tmp->unk8 = 0;
    }
    if (tmp->unk14C.tilesVram) {
        VramFree(tmp->unk14C.tilesVram);
        tmp->unk14C.tilesVram = 0;
    }
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        if (x->unkC[i].unk0.tilesVram) {
            VramFree(x->unkC[i].unk0.tilesVram);
            x->unkC[i].unk0.tilesVram = 0;
        }
    }
}

static void sub_0802E78C(void) {
    struct Unk_0802E57C *tmp = TaskGetStructPtr(gCurTask), *x = tmp;

    if (!(x->unk188 & 0x40000000)) {
        if (x->unk188 & 0x100 && !(x->unk18C & 0xF)) {
            u16 idx = Rand16() & 0x1F;
            u16 duration = (Rand16() & 0xF) + 50;
            s16 speed[2] = { -x->unk180, -x->unk182 };
            s32 pos[2];
            s16 diff;
            s16 lo = 0; // fake var lo is for using the other zero register

            pos[0] = x->unk178 + (((Rand16() & 0xF) - 8) * 0x100);
            pos[1] = x->unk17C + (((Rand16() & 0xF) - 8) * 0x100);
            diff = speed[0] > speed[1] ? speed[0] - speed[1] : speed[1] - speed[0];
            diff = diff < lo ? lo : diff;
            if ((u16)diff <= 0xFF) {
                speed[0] = 0;
                speed[1] = 0x80;
            } else {
                speed[0] = speed[0] < -0x400 ? -0x400 : (speed[0] > 0x400 ? 0x400 : speed[0]);
                speed[1] = speed[1] < -0x400 ? -0x400 : (speed[1] > 0x400 ? 0x400 : speed[1]);
            }
            sub_0802F8D8(x, gUnk_082EB954[gUnk_082EB984[idx]][0], gUnk_082EB954[gUnk_082EB984[idx]][1], x->unk8,
                         pos[0], pos[1], speed[0], speed[1], duration);
        }
        CALL(x->unk4)(x);
        x->unk18C++;
    } else if (x->unk188 & 0x80000000) {
        TaskDestroy(gCurTask);
    } else {
        x->unk188 |= 0xA0000000;
    }
}

void sub_0802E97C(struct Unk_0802E57C *x) {
    u16 i;

    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        struct Unk_0802E57C_C *c = &x->unkC[i];

        if (c->unk2C & 2) {
            c->unk40++;
            if ((c->unk3C[c->unk40 * 2] & c->unk3C[c->unk40 * 2 + 1]) == 0xFFFF) {
                c->unk3C = gUnk_08D60B24[Rand16() & 7];
                c->unk40 = 0;
            }
            c->unk2C &= ~2;
            c->unk42 = c->unk3C[c->unk40 * 2];
            c->unk44 = c->unk3C[c->unk40 * 2 + 1];
            c->unk28 = gUnk_082EB7D0[c->unk42];
        }
        sub_0802EF90(x, c);
    }
}

static void sub_0802EA4C(struct Unk_0802E57C *x) {
    struct Sprite *s;
    u16 i;

    x->unk178 = gUnk_082EB6FC.x * 0x100;
    x->unk17C = gUnk_082EB6FC.y * 0x100;
    s = &x->unk14C;
    s->animId = 0x2DE;
    s->variant = 0;
    s->unk1B = 0xFF;
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        struct Unk_0802E57C_C *c = &x->unkC[i];

        c->unk0.animId = 4;
        c->unk0.variant = 0;
        c->unk0.unk1B = 0xFF;
        c->unk2C &= ~8;
        c->unk2C &= ~0x10;
        c->unk3C = PORT_ROM_TABLE(const u16 *, gUnk_082EB7F8, 0x082EB7F8); /* PORT-FORK (NETPLAY-V2 R6): the ROM's copy, at its GBA address, not the C one -- the same bytes, and a pointer every host stores alike. */
        c->unk40 = 0;
        c->unk42 = 0;
        c->unk44 = 0;
        switch (i) {
        case 0:
            c->unk2C &= ~1;
            c->unk30 = x->unk178 - 0x1400;
            c->unk34 = 0x8000;
            c->unk38 = 0x40;
            c->unk3A = -0x400;
            break;
        case 1:
            c->unk2C |= 1;
            c->unk30 = x->unk178 + 0x1400;
            c->unk34 = 0x8000;
            c->unk38 = -0x40;
            c->unk3A = -0x400;
            break;
        case 2:
            c->unk2C |= 1;
            c->unk30 = x->unk178 + 0x2800;
            c->unk34 = 0x8000;
            c->unk38 = -0xC0;
            c->unk3A = -0x400;
            break;
        case 3:
            c->unk2C &= ~1;
            c->unk30 = x->unk178 - 0x2800;
            c->unk34 = 0x8000;
            c->unk38 = 0xC0;
            c->unk3A = -0x400;
            break;
        }
        c->unk28 = FN(sub_0802FC08);
    }
    x->unk188 |= 0x100;
    x->unk4 = FN(sub_0802EBB0);
    sub_0802EBB0(x);
}

static void sub_0802EBB0(struct Unk_0802E57C *x) {
    struct Sprite *s = &x->unk14C;
    u16 i;

    if (!sub_08155128(s)) {
        s->unk1B = 0xFF;
    }
    s->x = x->unk178 >> 8;
    s->y = x->unk17C >> 8;
    DisplaySprite(s);
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        x->unkC[i].unk3A += 0x26;
        sub_0802EF90(x, &x->unkC[i]);
    }
    if (x->unkC[0].unk3A >= -0x10) {
        for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
            struct Unk_0802E57C_C *c = &x->unkC[i];

            c->unk0.animId = 0x2D;
            c->unk0.variant = i;
            /* Double assignment of unk1B is needed for matching */
            c->unk0.unk1B = 0xFF;
            c->unk0.unk1B = 0xFF;
            c->unk2C &= ~8;
            c->unk2C |= 0x10;
            c->unk30 = x->unk178;
            c->unk34 = x->unk17C;
            c->unk38 = 0;
            c->unk3A = 0;
            c->unk2C &= ~1;
            c->unk2C |= 0x80;
        }
        sub_0802FA40(x, 0x292, 0, x->unk8, x->unk178, x->unk17C, 0, 0, 1);
        m4aSongNumStart(SE_KIRBY_STAR_DESTROY);
        x->unk4 = FN(sub_0802FE30);
    }
}

void sub_0802ECBC(struct Unk_0802E57C *x) {
    struct Sprite *s = &x->unk14C;
    u16 i;

    if (!sub_08155128(s)) {
        s->unk1B = 0xFF;
    }
    s->x = x->unk178 >> 8;
    s->y = x->unk17C >> 8;
    DisplaySprite(s);
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        struct Unk_0802E57C_C *c = &x->unkC[i];

        c->unk30 = x->unk178;
        c->unk34 = x->unk17C;
        sub_0802EF90(x, c);
    }
    if (x->unk190++ > 30) {
        x->unk4 = FN(sub_0802ED44);
    }
}

static void sub_0802ED44(struct Unk_0802E57C *x) {
    struct Sprite *s = &x->unk14C;
    u16 i;

    if (!sub_08155128(s)) {
        s->unk1B = 0xFF;
    }
    s->x = x->unk178 >> 8;
    s->y = x->unk17C >> 8;
    DisplaySprite(s);
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        struct Unk_0802E57C_C *c = &x->unkC[i];

        c->unk30 = x->unk178;
        c->unk34 = x->unk17C;
        sub_0802EF90(x, c);
    }
    if (x->unk188 & 0x200) {
        x->unk4 = FN(sub_0802EDC8);
    }
}

static void sub_0802EDC8(struct Unk_0802E57C *x) {
    if (gMultiSioEnabled == 1) {
        x->unk174 = gUnk_082EB740;
    } else {
        x->unk174 = gUnk_082EB700;
    }
    x->unk184 = 1;
    x->unk186 = 0;
    x->unk180 = x->unk174->unk0;
    x->unk182 = x->unk174->unk2;
    if (gMultiSioEnabled == 1) {
        m4aSongNumStart(SE_08D5BE24);
    } else {
        m4aSongNumStart(SE_08D5C490);
    }
    x->unk4 = FN(sub_0802EE54);
    sub_0802EE54(x);
}

static void sub_0802EE54(struct Unk_0802E57C *x) {
    struct Sprite *s;
    u16 i;

    if (x->unk174[x->unk184].unk6 & 1 || x->unk178 < -0x4000 || x->unk178 > 0x13000 || x->unk17C < -0x4000
        || x->unk17C > 0xE000) {
        x->unk188 &= ~0x100;
        x->unk4 = FN(sub_0802FE4C);
        return;
    }
    if (x->unk174[x->unk184].unk4 < x->unk186++) {
        x->unk186 = 0;
        x->unk184++;
    }
    x->unk180 += x->unk174[x->unk184].unk0;
    x->unk182 += x->unk174[x->unk184].unk2;
    x->unk178 += x->unk180;
    x->unk17C += x->unk182;
    s = &x->unk14C;
    if (!sub_08155128(s)) {
        s->unk1B = 0xFF;
    }
    s->x = x->unk178 >> 8;
    s->y = x->unk17C >> 8;
    DisplaySprite(s);
    for (i = 0; i < ARRAY_COUNT(x->unkC); i++) {
        struct Unk_0802E57C_C *c = &x->unkC[i];

        c->unk30 = x->unk178;
        c->unk34 = x->unk17C;
        sub_0802EF90(x, c);
    }
}

void sub_0802EF90(struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    struct Sprite *s;

    c->unk30 += c->unk38;
    c->unk34 += c->unk3A;
    if (!(c->unk2C & 0x80)) {
        if (c->unk30 < -0x8000) {
            c->unk30 = -0x8000;
        } else if (c->unk30 > 0x17000) {
            c->unk30 = 0x17000;
        }
        if (c->unk34 > 0x7FFF) {
            c->unk2C &= ~0x40;
            c->unk34 = 0x8000;
            c->unk3A = 0;
        } else {
            c->unk2C |= 0x40;
            if (c->unk34 < -0x8000) {
                c->unk34 = -0x8000;
            }
        }
    }
    CALL(c->unk28)(x, c);
    s = &c->unk0;
    if (c->unk2C & 1) {
        s->unk8 |= 0x400;
    } else {
        s->unk8 &= ~0x400;
    }
    if (!sub_08155128(s)) {
        c->unk2C |= 8;
    } else {
        c->unk2C &= ~8;
    }
    if (c->unk2C & 8) {
        if (c->unk2C & 0x10) {
            s->unk1B = 0xFF;
            x->unk188 &= ~8;
        }
    }
    s->x = c->unk30 >> 8;
    s->y = c->unk34 >> 8;
    DisplaySprite(s);
}

void sub_0802F088(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    c->unk0.animId = 2;
    c->unk0.variant = 0;
    c->unk0.unk1B = 0xFF;
    c->unk2C &= ~8;
    c->unk2C |= 0x10;
    c->unk30 = gUnk_082EB6DC[c->unk46][0] * 0x100;
    c->unk34 = gUnk_082EB6DC[c->unk46][1] * 0x100;
    if (gUnk_082EB6EC[c->unk46]) {
        c->unk2C |= 1;
    } else {
        c->unk2C &= ~1;
    }
    c->unk48 = 0;
    c->unk4A = 0;
    c->unk28 = FN(sub_0802F110);
}

static void sub_0802F110(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk48 > 80) {
        if (!c->unk4A) {
            c->unk0.animId = 0x33;
            c->unk0.variant = 0;
            c->unk0.unk1B = 0xFF;
            c->unk2C &= ~8;
            c->unk2C |= 0x10;
            c->unk4A = 1;
        }
        if (c->unk38 > 0) {
            c->unk38 -= 0x20;
            if (c->unk38 < 0) {
                c->unk38 = 0;
            }
        } else if (c->unk38 < 0) {
            c->unk38 += 0x20;
            if (c->unk38 > 0) {
                c->unk38 = 0;
            }
        }
        if (c->unk38 == 0) {
            c->unk2C |= 2;
        }
    } else {
        if (c->unk2C & 1) {
            c->unk38 -= 0x20;
        } else {
            c->unk38 += 0x20;
        }
        c->unk38 = c->unk38 < -0x1DC ? -0x1DC : (c->unk38 > 0x1DC ? 0x1DC : c->unk38);
        c->unk48++;
    }
}

void sub_0802F1C4(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    c->unk0.animId = 1;
    c->unk0.variant = 0;
    c->unk0.unk1B = 0xFF;
    c->unk2C &= ~8;
    c->unk2C |= 0x10;
    if (c->unk30 <= 0x4FFF) {
        c->unk2C &= ~1;
    } else if (c->unk30 > 0xA000) {
        c->unk2C |= 1;
    }
    c->unk34 = 0x8000;
    c->unk48 = Rand16() & 0x1F;
    c->unk28 = FN(sub_0802F240);
}

static void sub_0802F240(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk44 < c->unk48) {
        if (c->unk38 > 0) {
            c->unk38 -= 0xE;
            if (c->unk38 < 0) {
                c->unk38 = 0;
            }
        } else if (c->unk38 < 0) {
            c->unk38 += 0xE;
            if (c->unk38 > 0) {
                c->unk38 = 0;
            }
        }
        if (c->unk38 == 0) {
            c->unk2C |= 2;
        }
    } else {
        if (c->unk2C & 1) {
            c->unk38 -= 0x20;
        } else {
            c->unk38 += 0x20;
        }
        c->unk38 = c->unk38 < -0x14C ? -0x14C : (c->unk38 > 0x14C ? 0x14C : c->unk38);
        c->unk48++;
    }
}

void sub_0802F2D0(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    c->unk0.animId = 2;
    c->unk0.variant = 0;
    c->unk0.unk1B = 0xFF;
    c->unk2C &= ~8;
    c->unk2C |= 0x10;
    if (c->unk30 <= 0x4FFF) {
        c->unk2C &= ~1;
    } else if (c->unk30 > 0xA000) {
        c->unk2C |= 1;
    }
    c->unk34 = 0x8000;
    c->unk48 = Rand16() & 0x1F;
    c->unk4A = 0;
    c->unk28 = FN(sub_0802F354);
}

static void sub_0802F354(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk44 < c->unk48) {
        if (!c->unk4A) {
            c->unk0.animId = 0x33;
            c->unk0.variant = 0;
            c->unk0.unk1B = 0xFF;
            c->unk2C &= ~8;
            c->unk2C |= 0x10;
            c->unk4A = 1;
        }
        if (c->unk38 > 0) {
            c->unk38 -= 0x20;
            if (c->unk38 < 0) {
                c->unk38 = 0;
            }
        } else if (c->unk38 < 0) {
            c->unk38 += 0x20;
            if (c->unk38 > 0) {
                c->unk38 = 0;
            }
        }
        if (c->unk38 == 0) {
            c->unk2C |= 2;
        }
    } else {
        if (c->unk2C & 1) {
            c->unk38 -= 0x20;
        } else {
            c->unk38 += 0x20;
        }
        c->unk38 = c->unk38 < -0x1DC ? -0x1DC : (c->unk38 > 0x1DC ? 0x1DC : c->unk38);
        c->unk48++;
    }
}

void sub_0802F40C(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    c->unk3A += 0x26;
    c->unk3A = c->unk3A < -0x280 ? -0x280 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
    if (c->unk3A >= 0) {
        c->unk0.animId = 0x185;
        c->unk0.variant = 3;
        c->unk0.unk1B = 0xFF;
        c->unk2C &= ~8;
        c->unk2C &= ~0x10;
        c->unk28 = FN(sub_0802F464);
    }
}

static void sub_0802F464(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk2C & 0x40) {
        c->unk3A += 0x26;
        c->unk3A = c->unk3A < -0x280 ? -0x280 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
    } else {
        c->unk0.animId = 0;
        c->unk0.variant = 0;
        c->unk0.unk1B = 0xFF;
        c->unk2C &= ~8;
        c->unk2C |= 0x10;
        c->unk28 = FN(sub_08030018);
    }
}

void sub_0802F4BC(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    c->unk0.animId = 6;
    c->unk0.variant = 5;
    c->unk0.unk1B = 0xFF;
    c->unk2C &= ~8;
    c->unk2C &= ~0x10;
    if (c->unk30 <= 0x4FFF) {
        c->unk2C &= ~1;
    } else if (c->unk30 > 0xA000) {
        c->unk2C |= 1;
    }
    c->unk34 = 0x8000;
    c->unk48 = 0;
    c->unk4A = 0;
    c->unk28 = FN(sub_08030024);
}

void sub_0802F51C(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk38 > 0) {
        c->unk38 -= 0xE;
        if (c->unk38 < 0) {
            c->unk38 = 0;
        }
    } else if (c->unk38 < 0) {
        c->unk38 += 0xE;
        if (c->unk38 > 0) {
            c->unk38 = 0;
        }
    }
    if (c->unk2C & 0x40) {
        c->unk3A += 0x26;
        c->unk3A = c->unk3A < -0x150 ? -0x150 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
    }
    if (c->unk2C & 8) {
        c->unk0.animId = 7;
        c->unk0.variant = 0;
        c->unk0.unk1B = 0xFF;
        c->unk2C &= ~8;
        c->unk2C |= 0x10;
        if (c->unk2C & 0x40) {
            c->unk28 = FN(sub_08030068);
        } else {
            c->unk28 = FN(sub_080301DC);
        }
    }
}

void sub_0802F5B8(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk38 > 0) {
        c->unk38 -= 0xE;
        if (c->unk38 < 0) {
            c->unk38 = 0;
        }
    } else if (c->unk38 < 0) {
        c->unk38 += 0xE;
        if (c->unk38 > 0) {
            c->unk38 = 0;
        }
    }
    if (c->unk2C & 0x40) {
        c->unk3A += 0x26;
        c->unk3A = c->unk3A < -0x150 ? -0x150 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
        if (c->unk48++ > 30) {
            c->unk28 = FN(sub_080300A0);
        }
    } else {
        c->unk28 = FN(sub_0803010C);
    }
}

void sub_0802F638(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk2C & 0x40) {
        c->unk3A += 0x26;
        c->unk3A = c->unk3A < -0x150 ? -0x150 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
        if (c->unk2C & 8) {
            c->unk0.animId = 4;
            c->unk0.variant = 4;
            c->unk0.unk1B = 0xFF;
            c->unk2C &= ~8;
            c->unk2C &= ~0x10;
            c->unk28 = FN(sub_080300CC);
        }
    } else {
        c->unk28 = FN(sub_0803017C);
    }
}

void sub_0802F6A0(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk38 > 0) {
        c->unk38 -= 0xE;
        if (c->unk38 < 0) {
            c->unk38 = 0;
        }
    } else if (c->unk38 < 0) {
        c->unk38 += 0xE;
        if (c->unk38 > 0) {
            c->unk38 = 0;
        }
    }
    if (c->unk2C & 0x40) {
        c->unk3A += 0x26;
        c->unk3A = c->unk3A < -0x280 ? -0x280 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
    }
    if (c->unk2C & 8) {
        c->unk28 = FN(sub_080301B0);
    }
}

void sub_0802F714(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk38 > 0) {
        c->unk38 -= 0xE;
        if (c->unk38 < 0) {
            c->unk38 = 0;
        }
    } else if (c->unk38 < 0) {
        c->unk38 += 0xE;
        if (c->unk38 > 0) {
            c->unk38 = 0;
        }
    }
    if (c->unk2C & 0x40) {
        c->unk3A += 0x26;
        c->unk3A = c->unk3A < -0x280 ? -0x280 : (c->unk3A > 0x280 ? 0x280 : c->unk3A);
    } else {
        c->unk28 = FN(sub_0803010C);
    }
}

void sub_0802F77C(struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    u16 v = c->unk34 >> 12;

    c->unk4A = Rand16() & 7;
    if (v >= c->unk4A) {
        c->unk0.animId = 7;
        c->unk0.variant = 0;
        c->unk0.unk1B = 0xFF;
        c->unk2C &= ~8;
        c->unk2C |= 0x10;
    } else {
        c->unk0.animId = 7;
        c->unk0.variant = 1;
        c->unk0.unk1B = 0xFF;
        c->unk2C &= ~8;
        c->unk2C |= 0x10;
    }
    if (c->unk30 <= 0x4FFF) {
        c->unk2C &= ~1;
    } else if (c->unk30 > 0xA000) {
        c->unk2C |= 1;
    }
    c->unk28 = FN(sub_0802F810);
    sub_0802F810(x, c);
}

static void sub_0802F810(UNUSED struct Unk_0802E57C *x, struct Unk_0802E57C_C *c) {
    if (c->unk44 < c->unk48) {
        c->unk2C |= 2;
    } else {
        u16 v = c->unk34 >> 12;

        c->unk3A += 0x16;
        c->unk3A = c->unk3A < -0x150 ? -0x150 : (c->unk3A > 0x120 ? 0x120 : c->unk3A);
        if (v >= c->unk4A) {
            c->unk3A -= 0x78;
            c->unk3A = c->unk3A < -0x150 ? -0x150 : (c->unk3A > 0x120 ? 0x120 : c->unk3A);
        }
        if (c->unk2C & 1) {
            c->unk38 -= 0x16;
        } else {
            c->unk38 += 0x16;
        }
        c->unk38 = c->unk38 < -0x110 ? -0x110 : (c->unk38 > 0x110 ? 0x110 : c->unk38);
        if (v == c->unk4A) {
            c->unk28 = FN(sub_0802F77C);
        }
        c->unk48++;
    }
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_code_0802E57C) {
    PORT_FNID(sub_0802E71C),
    PORT_FNID(sub_0802E78C),
    PORT_FNID(sub_0802E97C),
    PORT_FNID(sub_0802EA4C),
    PORT_FNID(sub_0802EBB0),
    PORT_FNID(sub_0802ECBC),
    PORT_FNID(sub_0802ED44),
    PORT_FNID(sub_0802EDC8),
    PORT_FNID(sub_0802EE54),
    PORT_FNID(sub_0802F088),
    PORT_FNID(sub_0802F110),
    PORT_FNID(sub_0802F1C4),
    PORT_FNID(sub_0802F240),
    PORT_FNID(sub_0802F2D0),
    PORT_FNID(sub_0802F354),
    PORT_FNID(sub_0802F40C),
    PORT_FNID(sub_0802F464),
    PORT_FNID(sub_0802F4BC),
    PORT_FNID(sub_0802F51C),
    PORT_FNID(sub_0802F5B8),
    PORT_FNID(sub_0802F638),
    PORT_FNID(sub_0802F6A0),
    PORT_FNID(sub_0802F714),
    PORT_FNID(sub_0802F77C),
    PORT_FNID(sub_0802F810),
    PORT_FNID_END
};
/* PORT-FNID end */
