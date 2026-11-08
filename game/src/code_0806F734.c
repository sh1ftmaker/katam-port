
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"
#include "task.h"
#include "kirby.h"
#include "object.h"
#include "constants/kirby.h"
#include "constants/songs.h"

void sub_08098F6C(void);
void sub_0809924C(void);
void sub_08099668(void);
void sub_080998B8(void);

// code_0806F780.c
void sub_08098400(void);
void sub_08098700(void);
bool32 EffectObjectPostUpdate(struct EffectObject *);
void EffectObjectInitSprite(struct EffectObject *, struct Sprite *, u32, u16, u8, u16);
struct EffectObject *CreateEffectObject(struct ObjectBase *, u32, u16, u8);
extern const s16 gUnk_0835104A[];

void sub_08099B28(struct Task *);

// sub_08098E54: functionally equivalent; remaining diff is register allocation.
#ifndef NONMATCHING
void sub_08098E54(struct Kirby *kirby, u8 a2, u8 a3, u16 a4, u16 a5) { PortMissingFunction("sub_08098E54"); }
#else
void sub_08098E54(struct Kirby *kirby, u8 a2, u8 a3, u16 a4, u16 a5) {
    struct Task *t = TaskCreate(FN(sub_08098F6C), sizeof(struct EffectObject), 0x3500, TASK_USE_EWRAM, FN(ObjectBaseDestroy));
    struct EffectObject *tmp = TaskGetStructPtr(t), *obj4 = tmp;

    ClearEffectObject(tmp);
    obj4->header.kind = 3;
    obj4->x = kirby->base.x;
    obj4->y = kirby->base.y;
    obj4->parent = kirby;
    obj4->roomId = kirby->base.roomId;
    obj4->flags |= 0x4000;
    obj4->unk3C = a4;
    obj4->unk3E = a5;
    obj4->y += (s8)a3 << 8;
    if (kirby->base.flags & 1)
        obj4->x -= (s8)a2 << 8;
    else
        obj4->x += (s8)a2 << 8;
    if (Macro_0810B1F4(&kirby->base))
        obj4->flags |= 0x2000;
    obj4->flags |= 0x4000;
    EffectObjectInitSprite(obj4, &obj4->sprite, 0x6012000, 0x297, 1, 12);
}
#endif

void sub_08098F6C(void) {
    struct EffectObject *tmp = TaskGetStructPtr(gCurTask), *obj4 = tmp;
    struct Kirby *kirby2;
    u16 dead = obj4->flags & 0x1000;

    if (dead) {
        TaskDestroy(gCurTask);
    } else {
        kirby2 = obj4->parent;
        if (kirby2) {
            if (kirby2->base.header.kind && kirby2->base.flags & 0x1000) {
                obj4->parent = NULL;
                kirby2 = NULL;
            }
            if (!kirby2) goto _08099000;
            if (Macro_0810B1F4(&kirby2->base) && !(obj4->flags & 0x2000)) {
                EffectObjectDisplaySprite(obj4);
                return;
            }
        } else {
        _08099000:
            KirbySomething(obj4);
        }
        Macro_0809E55C(obj4);
        if (obj4->flags & 2) {
            obj4->flags |= 0x1000;
            return;
        }
        ++obj4->unk4;
        if (!(obj4->flags & 0x800)) {
            obj4->x += obj4->unk3C;
            obj4->y -= obj4->unk3E;
        }
        EffectObjectPostUpdate(obj4);
    }
}

void sub_08099118(struct Kirby *kirby) {
    struct Task *t = TaskCreate(FN(sub_0809924C), sizeof(struct EffectObject), 0x3500, TASK_USE_EWRAM, FN(ObjectBaseDestroy));
    struct EffectObject *obj4 = TaskGetStructPtr(t);
    u16 zero;
    u16 fl;

    ClearEffectObject(obj4);
    zero = 0;
    obj4->header.kind = 3;
    obj4->x = kirby->base.x;
    obj4->y = kirby->base.y;
    obj4->parent = kirby;
    obj4->roomId = kirby->base.roomId;
    fl = obj4->flags;
    fl |= 0x4000;
    fl |= 0x400;
    obj4->unk3C = zero;
    obj4->unk3E = zero;
    obj4->y += -0xA000;
    obj4->x += -0xA000;
    fl |= 0x4000;
    obj4->flags = fl;
    EffectObjectInitSprite(obj4, &obj4->sprite, 0x6012000, 0x29C, 1, 0x1A);
    PlaySfx(&kirby->base, SE_08D5B84C);
}

// sub_0809924C: functionally equivalent; remaining diff is register allocation.
#ifndef NONMATCHING
void sub_0809924C(void) { PortMissingFunction("sub_0809924C"); }
#else
void sub_0809924C(void) {
    struct EffectObject *tmp = TaskGetStructPtr(gCurTask), *obj4 = tmp;
    struct Kirby *kirby = obj4->parent, *kirby2;
    u16 dead = obj4->flags & 0x1000;
    s16 counter;

    if (dead) {
        TaskDestroy(gCurTask);
    } else {
        kirby2 = obj4->parent;
        if (kirby2) {
            if (kirby2->base.header.kind && kirby2->base.flags & 0x1000) {
                obj4->parent = NULL;
                kirby2 = NULL;
            }
            if (!kirby2) goto _080992E0;
            if (Macro_0810B1F4(&kirby2->base) && !(obj4->flags & 0x2000)) {
                EffectObjectDisplaySprite(obj4);
                return;
            }
        } else {
        _080992E0:
            KirbySomething(obj4);
        }
        {
            bool32 flag = TRUE;
            u8 i = gNumKirbys;

            counter = obj4->unk4;
            if (gNumKirbys) {
                while (1) {
                    if (gCurLevelInfo[i - 1].currentRoom == obj4->roomId
                        && !(gUnk_02026D50[EWRAM_MIRROR_U8(&gCurLevelInfo[i - 1].unk65E)] & 8)) {
                        flag = FALSE;
                        break;
                    }
                    if (!--i)
                        break;
                }
            }
            if (flag)
                obj4->flags |= 0x1000;
        }
        if (counter > 0x20)
            goto _080994EC;
        if (obj4->flags & 0x400) {
            if (!(kirby->base.flags & 0x200)) {
                if (kirby->animationIndex == 0x63 || kirby->ability == KIRBY_ABILITY_UFO) {
                    if (kirby->animationIndex == 0x36)
                        return;
                    if (kirby->ability != KIRBY_ABILITY_UFO)
                        return;
                }
                goto _08099484;
            }
            obj4->flags &= ~0x400;
            obj4->y = kirby->base.y + -0xA000;
            obj4->x = kirby->base.x + -0xA000;
            if (gLocalPlayerId != kirby->base.unk56) {
                struct EffectObject *effect = CreateEffectObject(&kirby->base, 0, 0x292, 0);

                effect->x = obj4->x;
                effect->y = obj4->y;
            }
        }
        if ((kirby->animationIndex == 0x63 || kirby->ability == KIRBY_ABILITY_UFO)
            && (kirby->animationIndex == 0x36 || kirby->ability != KIRBY_ABILITY_UFO)
            && (kirby->base.unkC & 0x40000))
            goto _0809952C;
        {
            struct EffectObject *effect = CreateEffectObject(&kirby->base, 0, 0x292, 0);

            effect->x = obj4->x;
            effect->y = obj4->y;
        }
    _08099484:
        PlaySfx(&kirby->base, SE_WARPSTAR_ACTIVATE_DESTROY);
    _080994EC:
        obj4->flags |= 0x1000;
        return;

    _0809952C:
        obj4->flags |= 4;
        {
            u16 t = gUnk_0835104A[(0x20 - obj4->unk4) >> 2];
            obj4->unk3E = kirby->base.yspeed - t + kirby->movementOverride.y;
        }
        obj4->unk3C = gUnk_0835104A[obj4->unk4 >> 2] + kirby->base.xspeed
            + kirby->movementOverride.x;
        ++obj4->unk4;
        if (!(obj4->flags & 0x800)) {
            obj4->x += obj4->unk3C;
            obj4->y -= obj4->unk3E;
        }
        EffectObjectPostUpdate(obj4);
    }
}
#endif

void sub_080995AC(struct ThrowAbilityObject *obj3) {
    struct Task *t = TaskCreate(FN(sub_08099668), sizeof(struct EffectObject), 0x3500, TASK_USE_IWRAM, FN(ObjectBaseDestroy));
    struct EffectObject *obj4 = TaskGetStructPtr(t);
    u16 speed;

    ClearEffectObject(obj4);
    obj4->header.kind = 3;
    obj4->x = obj3->base.x;
    obj4->y = obj3->base.y;
    obj4->parent = obj3;
    obj4->roomId = obj3->base.roomId;
    speed = 0x200;
    obj4->unk3C = speed;
    if (obj3->base.xspeed > 0)
        obj4->unk3C = -speed;
    obj4->unk3E = -0x200;
    if (obj3->base.yspeed < 0)
        obj4->unk3E = speed;
    obj4->flags |= 0x4000;
    EffectObjectInitSprite(obj4, &obj4->sprite, 0x6012000, 0x2A4, 0, 12);
}

void sub_08099668(void) {
    struct EffectObject *tmp = TaskGetStructPtr(gCurTask), *obj4 = tmp;
    struct ThrowAbilityObject *obj3;
    u16 dead = obj4->flags & 0x1000;
    u16 c;

    if (dead) {
        TaskDestroy(gCurTask);
    } else {
        obj3 = obj4->parent;
        if (obj3) {
            if (obj3->base.header.kind && obj3->base.flags & 0x1000) {
                obj4->parent = NULL;
                obj3 = NULL;
            }
            if (!obj3) goto _080996FC;
            if (Macro_0810B1F4(&obj3->base) && !(obj4->flags & 0x2000)) {
                EffectObjectDisplaySprite(obj4);
                return;
            }
        } else {
        _080996FC:
            KirbySomething(obj4);
        }
        Macro_0809E55C(obj4);
        if (obj4->flags & 2) {
            obj4->flags |= 0x1000;
            return;
        }
        c = obj4->unk4;
        if (obj4->unk4 == 1)
            obj4->sprite.unk14 = 0x640;
        obj4->unk4 = c + 1;
        if (!(obj4->flags & 0x800)) {
            obj4->x += obj4->unk3C;
            obj4->y -= obj4->unk3E;
        }
        EffectObjectPostUpdate(obj4);
    }
}

void sub_08099828(struct ThrowAbilityObject *obj3) {
    struct Task *t = TaskCreate(FN(sub_080998B8), sizeof(struct EffectObject), 0x3500, TASK_USE_IWRAM, FN(ObjectBaseDestroy));
    struct EffectObject *obj4 = TaskGetStructPtr(t);

    ClearEffectObject(obj4);
    obj4->header.kind = 3;
    obj4->x = obj3->base.x;
    obj4->y = obj3->base.y;
    obj4->parent = obj3;
    obj4->roomId = obj3->base.roomId;
    obj4->flags |= 0x4000;
    EffectObjectInitSprite(obj4, &obj4->sprite, 0x6012000, 0x2A4, 1, 12);
}

void sub_080998B8(void) {
    struct EffectObject *tmp = TaskGetStructPtr(gCurTask), *obj4 = tmp;
    struct ThrowAbilityObject *obj3;
    u16 dead = obj4->flags & 0x1000;

    if (dead) {
        TaskDestroy(gCurTask);
    } else {
        obj3 = obj4->parent;
        if (obj3) {
            if (obj3->base.header.kind && obj3->base.flags & 0x1000) {
                obj4->parent = NULL;
                obj3 = NULL;
            }
            if (!obj3) goto _0809994C;
            if (Macro_0810B1F4(&obj3->base) && !(obj4->flags & 0x2000)) {
                EffectObjectDisplaySprite(obj4);
                return;
            }
        } else {
        _0809994C:
            KirbySomething(obj4);
        }
        Macro_0809E55C(obj4);
        if (obj4->flags & 2) {
            obj4->flags |= 0x1000;
            return;
        }
        if (!(obj4->flags & 0x800)) {
            obj4->x += obj4->unk3C;
            obj4->y -= obj4->unk3E;
        }
        EffectObjectPostUpdate(obj4);
    }
}

void sub_08099A60(struct Kirby *kirby) {
    struct Task *t = TaskCreate(FN(sub_08098400), sizeof(struct EffectObject), 0x3500, TASK_USE_IWRAM, NULL);
    struct EffectObject *obj4 = TaskGetStructPtr(t);

    ClearEffectObject(obj4);
    obj4->header.kind = 3;
    obj4->x = kirby->base.x;
    obj4->y = kirby->base.y;
    obj4->parent = kirby;
    obj4->roomId = kirby->base.roomId;
}

void sub_08099AC4(struct ObjectBase *objBase) {
    struct Task *t = TaskCreate(FN(sub_08098700), sizeof(struct EffectObject), 0x3500, TASK_USE_IWRAM, NULL);
    struct EffectObject *obj4 = TaskGetStructPtr(t);

    ClearEffectObject(obj4);
    obj4->header.kind = 3;
    obj4->x = objBase->x;
    obj4->y = objBase->y;
    obj4->parent = objBase;
    obj4->roomId = objBase->roomId;
}

void sub_08099B28(struct Task *t) {
    struct EffectObject *obj4 = TaskGetStructPtr(t);
    struct Kirby *kirby = obj4->parent;

    if (kirby->base.unk56 == gLocalPlayerId) {
        m4aSongNumStop(SE_ABILITY_CUPID_CHARGE_1);
        if (kirby->base.unk56 == gLocalPlayerId)
            m4aSongNumStop(SE_ABILITY_CUPID_CHARGE_2);
    }
    ObjectBaseDestroy(t);
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_code_0806F734) {
    PORT_FNID(sub_08098F6C),
    PORT_FNID(sub_0809924C),
    PORT_FNID(sub_08099668),
    PORT_FNID(sub_080998B8),
    PORT_FNID(sub_08099B28),
    PORT_FNID_END
};
/* PORT-FNID end */
