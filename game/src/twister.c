
#ifdef __cplusplus
extern "C" {
#endif

#include "twister.h"

#include "functions.h"
#include "kirby.h"
#include "object.h"
#include "task.h"

static void sub_080BBE08(struct Object*);
static void sub_080BBEBC(struct Object*);
static void sub_080BBF54(struct Object*);
static void sub_080BC35C(struct Object*);
static void sub_080BC4A8(struct Object*);

extern const struct AnimInfo gUnk_08354928[] = {
    { 0x325, 0, 0 },
    { 0x325, 1, 0 },
    { 0x325, 2, 0 },
    { 0x325, 3, 0 },
    { 0x325, 4, 0 },
};

void* CreateTwister(struct ObjectTemplate* arg0, u8 arg1) {
    struct Object *obj, *tmp;
    struct Task *task = TaskCreate(FN(ObjectMain), sizeof(struct Object), 0x1000, TASK_USE_EWRAM, FN(ObjectDestroy));
    tmp = TaskGetStructPtr(task);
    obj = tmp;
    InitObject(obj, arg0, arg1);
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

static void sub_080BBE08(struct Object* arg0) {
    arg0->base.flags |= 4;
    if (arg0->objTemplate->subtype1 == 0) {
        if (arg0->base.unk62 & 1) {
            arg0->base.flags ^= 1;
            arg0->base.xspeed = -arg0->base.xspeed;
        }
        if (arg0->unk83 == 0) {
            if (arg0->subtype != 0) {
                if (++arg0->base.counter > 0x50) {
                    arg0->unk83 = 1;
                    arg0->base.counter = 0;
                }
                arg0->base.counter++;
            }
            else {
                if (++arg0->base.counter > 0x8c) {
                    arg0->unk83 = 1;
                    arg0->base.counter = 0;
                }
                arg0->base.counter++;
            }
        }
        else {
            if (arg0->base.flags & 2) {
                if (++arg0->base.counter > 4) {
                    if (arg0->unk83 != 2) {
                        arg0->unk83 = 2;
                        arg0->base.counter = 0;
                        arg0->base.yspeed = 0x40;
                        arg0->base.flags |= 0x40;
                    }
                    else {
                        sub_080BBEBC(arg0);
                    }
                }
            }
        }
    }
}

static void sub_080BBEBC(struct Object* arg0) {
    ObjectSetFunc(arg0, 2, FN(sub_080BBF54));
    if (arg0->base.x > arg0->kirby3->base.x) {
        arg0->base.flags |= 1;
    }
    else {
        arg0->base.flags &= ~1;
    }
    arg0->base.flags |= 0x140;
    sub_0803E2B0(&arg0->base, -8, -8, 8, 8);
    arg0->base.unk5C = ~0x20;
    arg0->base.unk5C &= ~7;
    arg0->base.unk5C |= 1;
    if (arg0->kirby3->base.x < arg0->base.x) {
        arg0->unk85 |= 1;
    }
    else {
        arg0->unk85 &= ~1;
    }
    arg0->unk85 &= ~2;
    arg0->base.counter = 0x78;
}

static void sub_080BBF54(struct Object* arg0) {
    s8 sb;
    s32 lhs, rhs;
    arg0->base.flags |= 4;
    if (arg0->unk85 & 1) {
        sb = -0x40;
    }
    else {
        sb = 0x40;
    }
    if (arg0->subtype != 0) {
        if (++arg0->unk9E > 7) {
            arg0->kirby3 = FindTargetKirby(&arg0->base);
            arg0->unk9E = 0;
            arg0->unkA0 = arg0->kirby3->base.x >> 8;
            arg0->unkA2 = arg0->kirby3->base.y >> 8;
        }
        lhs = arg0->base.y & ~0xfff;
        rhs = ((arg0->unkA2 + 2) << 8) & ~0xfff;
        if (lhs > rhs) {
            arg0->base.yspeed += 0x40;
            if (arg0->base.yspeed > 0x40) {
                arg0->base.yspeed = 0x40;
            }
        }
        else if (lhs < rhs) {
            arg0->base.yspeed -= 0x40;
            if (arg0->base.yspeed < -0x40) {
                arg0->base.yspeed = -0x40;
            }
        }
        else {
            if (arg0->base.yspeed < 0) {
                arg0->base.yspeed += 4;
                if (arg0->base.yspeed > 0) {
                    arg0->base.yspeed = 0;
                }
            }
            else {
                arg0->base.yspeed -= 4;
                if (arg0->base.yspeed < 0) {
                    arg0->base.yspeed = 0;
                }
            }
        }
        lhs = arg0->base.x & ~0xfff;
        rhs = ((arg0->unkA0 + sb) << 8) & ~0xfff;
        if (lhs < rhs) {
            arg0->base.flags &= ~1;
            arg0->base.xspeed += 0x99;
            if (arg0->base.xspeed > 0x300) {
                arg0->base.xspeed = 0x300;
            }
            else {
                if (arg0->base.xspeed < -0x300) {
                    arg0->base.xspeed = -0x300;
                }
            }
        }
        else if (lhs > rhs) {
            arg0->base.flags |= 1;
            if (arg0->base.flags & 1) {
                arg0->base.xspeed -= 0x99;
                if (arg0->base.xspeed < -0x300) {
                    arg0->base.xspeed = -0x300;
                }
                else if (arg0->base.xspeed > 0x300) {
                    arg0->base.xspeed = 0x300;
                }
            }
            else {
                arg0->base.xspeed += 0x99;
                if (arg0->base.xspeed > 0x300) {
                    arg0->base.xspeed = 0x300;
                }
                else if (arg0->base.xspeed < -0x300) {
                    arg0->base.xspeed = -0x300;
                }
            }
        }
        else {
            if (arg0->base.xspeed < 0) {
                arg0->base.xspeed += 4;
                if (arg0->base.xspeed > 0) {
                    arg0->base.xspeed = 0;
                }
            }
            else {
                arg0->base.xspeed -= 4;
                if (arg0->base.xspeed < 0) {
                    arg0->base.xspeed = 0;
                }
            }
        }
    }
    else {
        if (++arg0->unk9E > 7) {
            arg0->kirby3 = FindTargetKirby(&arg0->base);
            arg0->unk9E = 0;
            arg0->unkA0 = arg0->kirby3->base.x >> 8;
            arg0->unkA2 = arg0->kirby3->base.y >> 8;
        }
        lhs = arg0->base.y & ~0xfff;
        rhs = ((arg0->unkA2 + 2) << 8) & ~0xfff;
        if (lhs > rhs) {
            arg0->base.yspeed += 0x40;
            if (arg0->base.yspeed > 0x40) {
                arg0->base.yspeed = 0x40;
            }
        }
        else if (lhs < rhs) {
            arg0->base.yspeed -= 0x40;
            if (arg0->base.yspeed < -0x40) {
                arg0->base.yspeed = -0x40;
            }
        }
        else {
            if (arg0->base.yspeed < 0) {
                arg0->base.yspeed += 4;
                if (arg0->base.yspeed > 0) {
                    arg0->base.yspeed = 0;
                }
            }
            else {
                arg0->base.yspeed -= 4;
                if (arg0->base.yspeed < 0) {
                    arg0->base.yspeed = 0;
                }
            }
        }
        lhs = arg0->base.x & ~0xfff;
        rhs = ((arg0->unkA0 + sb) << 8) & ~0xfff;
        if (lhs < rhs) {
            arg0->base.flags &= ~1;
            arg0->base.xspeed += 0x66;
            if (arg0->base.xspeed > 0x200) {
                arg0->base.xspeed = 0x200;
            }
            else {
                if (arg0->base.xspeed < -0x200) {
                    arg0->base.xspeed = -0x200;
                }
            }
        }
        else if (lhs > rhs) {
            arg0->base.flags |= 1;
            if (arg0->base.flags & 1) {
                arg0->base.xspeed -= 0x66;
                if (arg0->base.xspeed < -0x200) {
                    arg0->base.xspeed = -0x200;
                }
                else if (arg0->base.xspeed > 0x200) {
                    arg0->base.xspeed = 0x200;
                }
            }
            else {
                arg0->base.xspeed += 0x66;
                if (arg0->base.xspeed > 0x200) {
                    arg0->base.xspeed = 0x200;
                }
                else if (arg0->base.xspeed < -0x200) {
                    arg0->base.xspeed = -0x200;
                }
            }
        }
        else {
            if (arg0->base.xspeed < 0) {
                arg0->base.xspeed += 4;
                if (arg0->base.xspeed > 0) {
                    arg0->base.xspeed = 0;
                }
            }
            else {
                arg0->base.xspeed -= 4;
                if (arg0->base.xspeed < 0) {
                    arg0->base.xspeed = 0;
                }
            }
        }
    }
    if (--arg0->base.counter == 0) {
        if (arg0->unk85 & 2) {
            sub_080BC4A8(arg0);
            return;
        }
        else {
            if (abs(arg0->kirby3->base.y - arg0->base.y) <= 0x3fff) {
                arg0->unk85 ^= 1;
                arg0->base.counter = 0x78;
            }
            else {
                arg0->base.counter = 0x28;
            }
            arg0->unk85 |= 2;
        }
    }
    if (arg0->base.header.unk1 == 1) {
        PlaySfx(&arg0->base, SE_TWISTER_FLYING);
    }
}

static void sub_080BC35C(struct Object* arg0) {
    arg0->base.flags |= 4;
    if (arg0->base.counter != 0) {
        s32 a = -5, b = -6, c = -6, d = -7;
        if (++arg0->base.counter > 0x20) {
            ObjectSetFunc(arg0, 0, FN(sub_080BBE08));
            sub_0803E2B0(&arg0->base, a, b, 5, 5);
            ObjectSetBounds(&arg0->base, c, d, 6, 7);
            arg0->kirby3 = FindTargetKirby(&arg0->base);
            arg0->base.flags &= ~0x140;
            arg0->base.xspeed = 0;
            arg0->base.unk5C = 1;
        }
    }
    else {
        arg0->base.yspeed += 0xe7;
        if (arg0->base.yspeed > 0x200) {
            arg0->base.yspeed = 0x200;
        }
        if (!(arg0->unk85 & 4)) {
            if (arg0->base.yspeed == 0x200) {
                if (arg0->base.unk58 & 1) {
                    arg0->base.counter++;
                    arg0->unk83 = 1;
                    arg0->unk85 |= 4;
                    arg0->base.flags &= ~0x140;
                    arg0->base.unk5C = 1;
                }
            }
        }
    }
}

void sub_080BC440(struct Object* arg0) {
    ObjectSetFunc(arg0, 0, FN(sub_080BBE08));
    sub_0803E2B0(&arg0->base, -5, -6, 5, 5);
    ObjectSetBounds(&arg0->base, -6, -7, 6, 7);
    arg0->kirby3 = FindTargetKirby(&arg0->base);
    arg0->base.flags &= ~0x140;
    arg0->base.xspeed = 0;
    arg0->base.unk5C = 1;
}

static void sub_080BC4A8(struct Object* arg0) {
    ObjectSetFunc(arg0, 2, FN(sub_080BC35C));
    arg0->base.xspeed = 0;
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_twister) {
    PORT_FNID(CreateTwister),
    PORT_FNID(sub_080BBE08),
    PORT_FNID(sub_080BBF54),
    PORT_FNID(sub_080BC35C),
    PORT_FNID(sub_080BC440),
    PORT_FNID_END
};
/* PORT-FNID end */
