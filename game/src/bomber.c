
#ifdef __cplusplus
extern "C" {
#endif

#include "bomber.h"
#include "code_080332BC.h"
#include "kirby.h"
#include "functions.h"
#include "code_0806F780.h"

static void sub_080C47EC(struct Object*);
static void sub_080C485C(struct Object*);
static void sub_080C48B8(struct Object*);
static void sub_080C48DC(struct Object*);
static void sub_080C4900(struct Object*);
static void sub_080C4928(struct Object*);
static void sub_080C4968(struct Object*);

extern const struct AnimInfo gUnk_08355590[] = {
    { 0x32B, 0x0, 0x0 },
    { 0x32B, 0x1, 0x0 },
    { 0x32B, 0x2, 0x0 },
    { 0x32B, 0x3, 0x0 },
    { 0x32B, 0x4, 0x0 },
    { 0x32B, 0x5, 0x0 },
};

static const struct Unk_08357260 gUnk_083555A8[] = {
    { RGB(22, 22, 3), 1, 3 },
    {      RGB_BLACK, 0, 2 },
    { RGB(22, 22, 3), 1, 3 },
    {      RGB_BLACK, 0, 2 },
    { RGB(22, 22, 3), 1, 3 },
    {      RGB_BLACK, 0, 0 },
};

void* CreateBomber(struct ObjectTemplate* arg0, u8 arg1) {
    struct Object *obj, *tmp;
    struct Task *task = TaskCreate(FN(ObjectMain), sizeof(struct Object), 0x1000, TASK_USE_EWRAM, FN(ObjectDestroy));
    tmp = TaskGetStructPtr(task);
    obj = tmp;
    InitObject(obj, arg0, arg1);
    obj->base.unkC |= 0x100;
    if (obj->base.x > obj->kirby3->base.x) {
        obj->base.flags |= 1;
    }
    else {
        obj->base.flags &= ~1;
    }
    sub_0803E2B0(&obj->base, -5, -6, 5, 5);
    ObjectSetBounds(&obj->base, -6, -7, 6, 7);
    ObjectInitSprite(obj);
    CALL(gUnk_08351648[obj->type].unk10)(obj);
    obj->unk9E = 0;
    obj->unk7C = NULL;
    return obj;
}

void sub_080C478C(struct Object* arg0) {
    if (arg0->objTemplate->subtype1 == 0 && !(arg0->base.unk62 & 4)) {
        sub_080C48DC(arg0);
    }
    else {
        ObjectSetFunc(arg0, 0, FN(sub_080C485C));
        arg0->base.xspeed = 0x4d;
        if (arg0->base.flags & 1) {
            arg0->base.xspeed = -arg0->base.xspeed;
        }
        if (arg0->objTemplate->subtype1 != 0) {
            arg0->base.xspeed = 0;
        }
    }
}

static void sub_080C47EC(struct Object* arg0) {
    arg0->base.flags |= 4;
    arg0->base.yspeed = 0;
    if (arg0->base.flags & 2) {
        if (arg0->base.counter == 3) {
            arg0->base.xspeed = 0x100;
            if (arg0->base.flags & 1) {
                arg0->base.xspeed = -arg0->base.xspeed;
            }
        }
        if (++arg0->base.counter > 4) {
            sub_080C48DC(arg0);
            return;
        }
    }
    if (!(arg0->base.unk62 & 4)) {
        sub_080C48DC(arg0);
    }
}

static void sub_080C485C(struct Object* arg0) {
    arg0->base.flags |= 4;
    if (arg0->objTemplate->subtype1 == 0) {
        if (arg0->base.unk62 & 1) {
            arg0->base.flags ^= 1;
            arg0->base.xspeed = -arg0->base.xspeed;
        }
        if (sub_0809D998(arg0)) {
            sub_080C48B8(arg0);
        }
        if (!(arg0->base.unk62 & 4)) {
            sub_080C48DC(arg0);
        }
    }
}

static void sub_080C48B8(struct Object* arg0) {
    ObjectSetFunc(arg0, 1, FN(sub_080C47EC));
    arg0->base.xspeed = 0;
    arg0->base.yspeed = 0;
}

static void sub_080C48DC(struct Object* arg0) {
    ObjectSetFunc(arg0, 2, FN(sub_080C4900));
    arg0->base.xspeed = 0;
    arg0->base.yspeed = 0;
}

static void sub_080C4900(struct Object* arg0) {
    if (arg0->base.unk62 & 4) {
        if (!(arg0->base.flags & 0x1000)) {
            sub_080C4928(arg0);
        }
    }
}

static void sub_080C4928(struct Object* arg0) {
    ObjectSetFunc(arg0, 3, FN(sub_080C4968));
    arg0->base.xspeed = 0;
    arg0->base.yspeed = 0;
    arg0->base.flags |= 0x2000;
    arg0->base.flags |= 0x200;
    arg0->base.flags |= 0x2000000;
    sub_08033540(arg0->base.unk56);
}

static void sub_080C4968(struct Object* arg0) {
    arg0->base.flags |= 4;
    if (arg0->base.flags & 2) {
        if (++arg0->base.counter > 3) {
            sub_080335B4(arg0->base.unk56);
            sub_080860A8(&arg0->base, gUnk_083555A8);
            sub_08074420(&arg0->base);
            arg0->base.flags |= 0x1000;
        }
    }
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_bomber) {
    PORT_FNID(CreateBomber),
    PORT_FNID(sub_080C478C),
    PORT_FNID(sub_080C47EC),
    PORT_FNID(sub_080C485C),
    PORT_FNID(sub_080C4900),
    PORT_FNID(sub_080C4968),
    PORT_FNID_END
};
/* PORT-FNID end */
