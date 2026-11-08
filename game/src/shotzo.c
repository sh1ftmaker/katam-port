
#ifdef __cplusplus
extern "C" {
#endif

#include "shotzo.h"
#include "functions.h"
#include "kirby.h"
#include "object.h"
#include "code_0806F780.h"
#include "parasol.h"
#include "constants/songs.h"

void sub_0811A874(struct Object *);
void sub_0811A8D4(struct Object *);
void sub_0811A934(struct Object *);
void sub_0811A9D4(struct Object *);
void sub_0811AA2C(struct Object *);
void sub_0811AA80(struct Object *);
void sub_0811AB74(struct Object *);
void sub_0811AEDC(struct Object *);
void sub_0811B2A4(struct Object *);
void sub_0811B34C(struct Object *);
void sub_0811B3E4(struct Object *);
void sub_0811B400(struct Object *);
void sub_0811B44C(struct Object *);
void sub_0811B48C(struct Object *);

/* PORT: replaced by port/rom_data.h -- extern const u16 gUnk_08357C14[]; */
/* PORT: replaced by port/rom_data.h -- extern const u16 gUnk_08357C24[]; */
/* PORT: replaced by port/rom_data.h -- extern const u16 gUnk_08357C34[]; */

#define ShotzoFire(shotzo) ({ \
    switch ((shotzo)->subtype) \
    { \
    case 0: \
        (shotzo)->base.counter = 0x50; \
        break; \
    case 1: \
        (shotzo)->base.counter = 0x3C; \
        break; \
    case 2: \
        (shotzo)->base.counter = 0x28; \
        break; \
    default: \
        (shotzo)->base.counter = 0x14; \
        break; \
    } \
    (shotzo)->unk9E = 0; \
    (shotzo)->unk78 = sub_0811A934; \
    sub_0811AEDC(shotzo); \
})

void *CreateShotzo(struct ObjectTemplate *template_, u8 a2)
{
    struct Task *t = TaskCreate(ObjectMain, sizeof(struct Object), 0x1000, TASK_USE_IWRAM, ObjectDestroy);
    struct Object *tmp = TaskGetStructPtr(t), *shotzo = tmp;

    InitObject(tmp, template_, a2);
    shotzo->unk85 = 0;
    shotzo->unk9F = 0;
    shotzo->base.unkC |= 0x100;
    shotzo->base.flags |= 0x8000;
    shotzo->base.unk5C |= 0x20;
    sub_0803E2B0(&shotzo->base, -5, -6, 5, 3);
    ObjectSetBounds(&shotzo->base, -6, -7, 6, 7);
    ObjectInitSprite(shotzo);
    if (shotzo->objTemplate->subtype1 == 0)
    {
        if (shotzo->base.x > shotzo->kirby3->base.x)
            shotzo->base.flags |= 1;
        else
            shotzo->base.flags &= ~1;
    }
    if (shotzo->objTemplate->subtype1 == 4)
    {
        if (shotzo->base.x > shotzo->kirby3->base.x)
            shotzo->base.flags |= 1;
        else
            shotzo->base.flags &= ~1;
        sub_0811B400(shotzo);
    }
    else
        gUnk_08351648[shotzo->type].unk10(shotzo);
    shotzo->unk9E = 0;
    shotzo->unk7C = NULL;
    return shotzo;
}

void sub_0811A874(struct Object *shotzo)
{
    if (shotzo->objTemplate->subtype1 != 0 && shotzo->objTemplate->subtype1 != 4)
        sub_0811A9D4(shotzo);
    else
    {
        switch (shotzo->subtype)
        {
        case 0:
            shotzo->base.counter = 0x50;
            break;
        case 1:
            shotzo->base.counter = 0x3C;
            break;
        case 2:
            shotzo->base.counter = 0x28;
            break;
        default:
            shotzo->base.counter = 0x14;
            break;
        }
        shotzo->base.xspeed = 0;
        shotzo->unk9E = 0;
        shotzo->unk78 = sub_0811A8D4;
    }
}

void sub_0811A8D4(struct Object *shotzo)
{
    if (!--shotzo->base.counter)
        sub_0811AB74(shotzo);
    if (shotzo->unk83 == 2)
    {
        if (++shotzo->unk9E > 3)
        {
            switch (shotzo->unk9F)
            {
            case 0:
                shotzo->unk83 = 3;
                break;
            case 2:
                shotzo->base.flags &= ~1;
                shotzo->unk83 = 3;
                break;
            default:
                shotzo->unk83 = 1;
                break;
            }
            shotzo->unk9F = 0;
        }
    }
}

// sub_0811A934: functionally equivalent; remaining diff is register allocation.
#ifndef NONMATCHING
void sub_0811A934(struct Object *shotzo) { PortMissingFunction("sub_0811A934"); }
#else
void sub_0811A934(struct Object *shotzo)
{
    u8 *framePtr = &shotzo->unk9E;
    u32 frameShifted = *framePtr << 24;

    if (frameShifted >> 24 <= 7)
    {
        const u16 *offsetTable;

        switch (shotzo->unk83)
        {
        case 0:
            offsetTable = gUnk_08357C14;
            break;
        case 1:
            offsetTable = gUnk_08357C24;
            break;
        case 3:
            offsetTable = gUnk_08357C34;
            break;
        default:
            goto skip;
        }
        shotzo->base.objBase54 = offsetTable[(frameShifted >> 25) * 2] >> 8;
        shotzo->base.objBase55 = offsetTable[*framePtr | 1] >> 8;
    skip:
        shotzo->unk9E++;
        if (shotzo->base.flags & 1)
            shotzo->base.objBase54 = -shotzo->base.objBase54;
    }
    if (!--shotzo->base.counter)
        sub_0811A874(shotzo);
}
#endif

void sub_0811A9D4(struct Object *shotzo)
{
    switch (shotzo->objTemplate->subtype1)
    {
    case 1:
        shotzo->unk83 = 1;
        shotzo->base.flags |= 1;
        break;
    case 2:
        shotzo->unk83 = 3;
        shotzo->base.flags &= ~1;
        break;
    default:
        shotzo->unk83 = 1;
        shotzo->base.flags &= ~1;
        break;
    }
    shotzo->base.xspeed = 0;
    shotzo->unk9E = 0;
    shotzo->base.counter = 0x64;
    shotzo->unk78 = sub_0811B3E4;
}

void sub_0811AA2C(struct Object *shotzo)
{
    switch (shotzo->subtype)
    {
    case 0:
        shotzo->base.counter = 0x32;
        break;
    case 1:
        shotzo->base.counter = 0x28;
        break;
    case 2:
        shotzo->base.counter = 0x1E;
        break;
    case 3:
        shotzo->base.counter = 0x16;
        break;
    default:
        shotzo->base.counter = 0x11;
        break;
    }
    shotzo->unk9E = 0;
    shotzo->unk9F = 0;
    shotzo->unk78 = sub_0811AA80;
    sub_0811AEDC(shotzo);
}

// sub_0811AA80: functionally equivalent; remaining diff is register allocation.
#ifndef NONMATCHING
void sub_0811AA80(struct Object *shotzo) { PortMissingFunction("sub_0811AA80"); }
#else
void sub_0811AA80(struct Object *shotzo)
{
    u8 *framePtr = &shotzo->unk9E;
    u32 frameShifted = *framePtr << 24;

    if (frameShifted >> 24 <= 7)
    {
        const u16 *offsetTable;

        switch (shotzo->unk83)
        {
        case 0:
            offsetTable = gUnk_08357C14;
            break;
        case 1:
            offsetTable = gUnk_08357C24;
            break;
        case 3:
            offsetTable = gUnk_08357C34;
            break;
        default:
            goto skip;
        }
        shotzo->base.objBase54 = offsetTable[(frameShifted >> 25) * 2] >> 8;
        shotzo->base.objBase55 = offsetTable[*framePtr | 1] >> 8;
    skip:
        shotzo->unk9E++;
        if (shotzo->base.flags & 1)
            shotzo->base.objBase54 = -shotzo->base.objBase54;
    }
    if (!--shotzo->base.counter)
    {
        shotzo->unk9E = 0;
        if (++shotzo->unk9F > 2)
            sub_0811A9D4(shotzo);
        else
        {
            switch (shotzo->subtype)
            {
            case 0:
                shotzo->base.counter = 0x32;
                break;
            case 1:
                shotzo->base.counter = 0x28;
                break;
            case 2:
                shotzo->base.counter = 0x1E;
                break;
            case 3:
                shotzo->base.counter = 0x16;
                break;
            default:
                shotzo->base.counter = 0x11;
                break;
            }
            sub_0811AEDC(shotzo);
        }
    }
}
#endif

// sub_0811AB74: functionally equivalent; remaining diff is register allocation.
#ifndef NONMATCHING
void sub_0811AB74(struct Object *shotzo) { PortMissingFunction("sub_0811AB74"); }
#else
void sub_0811AB74(struct Object *shotzo)
{
    s32 dx, dy;

    shotzo->kirby3 = FindTargetKirby(&shotzo->base);
    dx = (shotzo->kirby3->base.x - shotzo->base.x) << 8;
    dy = (shotzo->kirby3->base.y - shotzo->base.y) << 8;
    switch (ArcTan2(dx >> 16, dy >> 16) >> 12)
    {
    case 7:
    case 8:
        if (shotzo->unk83 == 0)
        {
            if (shotzo->base.flags & 1)
            {
                if (shotzo->unk85 != 0)
                {
                    ShotzoFire(shotzo);
                    return;
                }
                shotzo->unk85 = 1;
                break;
            }
        }
        else
        {
            if (shotzo->base.flags & 1)
            {
                shotzo->unk83 -= 1;
                shotzo->unk85 = 0;
                break;
            }
        }
        if (shotzo->unk83 == 3)
        {
            shotzo->base.flags |= 1;
            shotzo->unk83 = 2;
            shotzo->unk9F = 1;
        }
        else
            shotzo->unk83++;
        shotzo->unk85 = 0;
        break;
    case 9:
    case 10:
        if (shotzo->unk83 == 1)
        {
            if (shotzo->base.flags & 1)
            {
                if (shotzo->unk85 != 0)
                {
                    ShotzoFire(shotzo);
                    return;
                }
                shotzo->unk85 = 1;
                break;
            }
        }
        else
        {
            if (shotzo->base.flags & 1)
            {
                if (shotzo->unk83 == 0)
                {
                    shotzo->unk83++;
                    shotzo->unk85 = 0;
                    break;
                }
                shotzo->unk83--;
                shotzo->unk85 = 0;
                break;
            }
        }
        if (shotzo->unk83 == 3)
        {
            shotzo->base.flags |= 1;
            shotzo->unk83 = 2;
            shotzo->unk9F = 1;
        }
        else
            shotzo->unk83++;
        shotzo->unk85 = 0;
        break;
    case 11:
    case 12:
        if (shotzo->unk83 == 3)
        {
            if (shotzo->base.flags & 1)
                goto rot;
            if (shotzo->unk85 != 0)
            {
                ShotzoFire(shotzo);
                return;
            }
            shotzo->unk85 = 1;
            break;
        }
        else
        {
            if (shotzo->base.flags & 1)
                goto rot;
            shotzo->unk83++;
            shotzo->unk85 = 0;
            break;
        }
    case 13:
    case 14:
        if (shotzo->unk83 == 1)
        {
            if (shotzo->base.flags & 1)
                goto rot;
            if (shotzo->unk85 != 0)
            {
                ShotzoFire(shotzo);
                return;
            }
            shotzo->unk85 = 1;
            break;
        }
        else
        {
            if (shotzo->base.flags & 1)
                goto rot;
            if (shotzo->unk83 == 3)
            {
                if (--shotzo->unk83 == 2)
                    shotzo->unk9F = 1;
                shotzo->unk85 = 0;
                break;
            }
            shotzo->unk83++;
            shotzo->unk85 = 0;
            break;
        }
    case 0:
    case 15:
        if (shotzo->unk83 == 0)
        {
            if (shotzo->base.flags & 1)
                goto rot;
            if (shotzo->unk85 != 0)
            {
                ShotzoFire(shotzo);
                return;
            }
            shotzo->unk85 = 1;
            break;
        }
        else
        {
            if (shotzo->base.flags & 1)
            {
            rot:
                if (shotzo->unk83 == 1)
                {
                    shotzo->unk9F = 2;
                    shotzo->unk83 = 2;
                }
                else
                    shotzo->unk83++;
                shotzo->unk85 = 0;
                break;
            }
            if (--shotzo->unk83 == 2)
                shotzo->unk9F = 1;
            shotzo->unk85 = 0;
            break;
        }
    case 1:
    case 2:
    case 3:
        if (shotzo->base.flags & 1)
        {
            if (shotzo->unk83 == 1)
            {
                shotzo->unk9F = 2;
                shotzo->unk83 = 2;
            }
            else
                shotzo->unk83++;
        }
        else
        {
            if (shotzo->unk83 != 0)
                shotzo->unk83--;
            if (shotzo->unk83 == 2)
                shotzo->unk9F = 1;
        }
        shotzo->unk85 = 0;
        break;
    case 4:
    case 5:
    case 6:
        if (shotzo->base.flags & 1)
        {
            if (shotzo->unk83 != 0)
                shotzo->unk83--;
        }
        else
        {
            if (shotzo->unk83 == 3)
            {
                shotzo->base.flags |= 1;
                shotzo->unk83 = 2;
                shotzo->unk9F = 1;
            }
            else
                shotzo->unk83++;
        }
        shotzo->unk85 = 0;
        break;
    }
    sub_0811A874(shotzo);
}
#endif

void sub_0811AEDC(struct Object *shotzo)
{
    struct Object *bullet;

    if (shotzo->base.y < gCurLevelInfo[shotzo->base.unk56].levelMaxPosition.y)
    {
        bullet = CreateObjTemplateAndObjWithSettingParent(shotzo, 1, 0x24,
            shotzo->base.x >> 8, shotzo->base.y >> 8, 0, 0x1F, 0, 0, OBJ_SHOTZO_BULLET,
            0, 0, shotzo->subtype, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        if (shotzo->base.flags & 1)
            bullet->base.flags |= 1;
        switch (shotzo->unk83)
        {
        case 0:
            if (shotzo->base.flags & 1)
                bullet->base.x += -0xC00;
            else
                bullet->base.x += 0xC00;
            bullet->base.y += 0x200;
            switch (shotzo->subtype)
            {
            case 0:
                bullet->base.xspeed = 0xB3;
                bullet->base.yspeed = 0;
                break;
            case 1:
                bullet->base.xspeed = 0x100;
                bullet->base.yspeed = 0;
                break;
            case 2:
                bullet->base.xspeed = 0x166;
                bullet->base.yspeed = 0;
                break;
            case 3:
                bullet->base.xspeed = 0x1CD;
                bullet->base.yspeed = 0;
                break;
            default:
                bullet->base.xspeed = 0x400;
                bullet->base.yspeed = 0;
                break;
            }
            break;
        case 1:
            if (bullet->base.flags & 1)
                bullet->base.x += -0x800;
            else
                bullet->base.x += 0x800;
            bullet->base.y += -0x600;
            switch (shotzo->subtype)
            {
            case 0:
                bullet->base.xspeed = 0x82;
                bullet->base.yspeed = 0x82;
                break;
            case 1:
                bullet->base.xspeed = 0xBB;
                bullet->base.yspeed = 0xBB;
                break;
            case 2:
                bullet->base.xspeed = 0x105;
                bullet->base.yspeed = 0x105;
                break;
            case 3:
                bullet->base.xspeed = 0x150;
                bullet->base.yspeed = 0x150;
                break;
            default:
                bullet->base.xspeed = 0x2EC;
                bullet->base.yspeed = 0x2EC;
                break;
            }
            break;
        case 3:
            bullet->base.y += -0xA00;
            switch (shotzo->subtype)
            {
            case 0:
                bullet->base.xspeed = 0;
                bullet->base.yspeed = 0xB3;
                break;
            case 1:
                bullet->base.xspeed = 0;
                bullet->base.yspeed = 0x100;
                break;
            case 2:
                bullet->base.xspeed = 0;
                bullet->base.yspeed = 0x166;
                break;
            case 3:
                bullet->base.xspeed = 0;
                bullet->base.yspeed = 0x1CD;
                break;
            default:
                bullet->base.xspeed = 0;
                bullet->base.yspeed = 0x400;
                break;
            }
            break;
        }
        if (bullet->base.flags & 1)
            bullet->base.xspeed = -bullet->base.xspeed;
        switch (shotzo->subtype)
        {
        case 0:
            bullet->base.counter = 0xD6;
            break;
        case 1:
            bullet->base.counter = 0x96;
            break;
        case 2:
            bullet->base.counter = 0x6B;
            break;
        case 3:
            bullet->base.counter = 0x53;
            break;
        default:
            bullet->base.counter = 0x26;
            break;
        }
        CreateEffectObject(&bullet->base, 0, 0x298, 1);
    }
}

void *CreateShotzoBullet(struct ObjectTemplate *template_, u8 a2)
{
    struct Task *t = TaskCreate(ObjectMain, sizeof(struct Object), 0x1000, TASK_USE_EWRAM, ObjectDestroy);
    struct Object *bullet = TaskGetStructPtr(t);
    u32 flags, unkC;

    InitObject(bullet, template_, a2);
    bullet->base.unk63 = 1;
    flags = bullet->base.flags;
    flags |= 0x40;
    unkC = bullet->base.unkC;
    unkC |= 1;
    unkC |= 4;
    unkC |= 2;
    bullet->base.unkC = unkC;
    flags |= 0x4000;
    flags |= 0x100;
    flags |= 0x10000000;
    bullet->base.flags = flags;
    bullet->base.unk68 = 0x20000043;
    bullet->base.unk5C |= 0x220;
    bullet->unk9E = 0;
    bullet->unk7C = sub_0809F840;
    sub_0803E2B0(&bullet->base, -2, -2, 2, 2);
    ObjectSetBounds(&bullet->base, 2, 2, 2, 2);
    ObjectInitSprite(bullet);
    gUnk_08351648[bullet->type].unk10(bullet);
    return bullet;
}

void sub_0811B2A4(struct Object *bullet)
{
    ObjectSetFunc(bullet, 0, sub_0811B34C);
    bullet->base.flags |= 0x40;
    bullet->base.flags &= ~0x20;
    PlaySfx(&bullet->base, SE_BULLET_ATTACK);
}

void sub_0811B34C(struct Object *bullet)
{
    bullet->base.flags |= 4;
    bullet->base.flags &= ~0x100;
    if (bullet->base.x <= gCurLevelInfo[bullet->base.unk56].levelMaxPosition.x
        && bullet->base.x >= gCurLevelInfo[bullet->base.unk56].levelMinPosition.x
        && bullet->base.y <= gCurLevelInfo[bullet->base.unk56].levelMaxPosition.y
        && bullet->base.y >= gCurLevelInfo[bullet->base.unk56].levelMinPosition.y)
        sub_0806FC70(&bullet->base);
    bullet->base.flags |= 0x100;
    if (!--bullet->base.counter || bullet->base.unk62)
    {
        sub_0809DA30(bullet);
        bullet->base.flags |= 0x1000;
    }
}

void sub_0811B3E4(struct Object *shotzo)
{
    if (!--shotzo->base.counter)
        sub_0811AA2C(shotzo);
}

void sub_0811B400(struct Object *shotzo)
{
    shotzo->unk83 = 0;
    shotzo->base.xspeed = 0;
    shotzo->base.yspeed = 0;
    shotzo->unk9E = 0;
    shotzo->base.counter = 0;
    sub_0803E2B0(&shotzo->base, -5, -16, 5, 8);
    shotzo->base.flags |= 0x2000;
    shotzo->unk78 = sub_0811B44C;
}

void sub_0811B44C(struct Object *shotzo)
{
    shotzo->base.flags &= ~0x2000;
    sub_080C29C0(shotzo, 0);
    shotzo->unk83 = 0;
    shotzo->base.xspeed = 0;
    shotzo->base.yspeed = 0;
    shotzo->unk9E = 0;
    shotzo->base.counter = 0;
    shotzo->unk78 = sub_0811B48C;
}

void sub_0811B48C(struct Object *shotzo)
{
    if (shotzo->base.unk62 & 4)
    {
        if (shotzo->base.flags & 0x1000000)
        {
            shotzo->base.flags &= ~0x1000000;
            sub_0803E2B0(&shotzo->base, -5, -6, 5, 3);
            sub_0811A874(shotzo);
            return;
        }
        shotzo->base.flags |= 0x1000000;
    }
    if (!(shotzo->base.flags & 0x40))
        shotzo->base.xspeed = 0;
}

#ifdef __cplusplus
}
#endif
