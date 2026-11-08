
#ifdef __cplusplus
extern "C" {
#endif

#include "fire.h"
#include "kirby.h"
#include "functions.h"

static void FireCounterIncrement(struct Object* arg0); // declaration

extern const struct AnimInfo gUnk_08357BE8[] = {
    { 0x305, 0, 0 },
};

void* CreateFire(struct ObjectTemplate* arg0, u8 arg1) {
    struct Object *obj;
    struct Task *task = TaskCreate(FN(ObjectMain), sizeof(struct Object), 0x1000, TASK_USE_IWRAM, FN(ObjectDestroy));
    obj = TaskGetStructPtr(task);
    InitObject(obj, arg0, arg1);
    obj->base.flags |= 0x18000;
    obj->base.unk5C |= 0x20;
    sub_0803E2B0(&obj->base, -6, -12, 6, -3);
    ObjectSetBounds(&obj->base, -7, -14, 7, -2);
    ObjectInitSprite(obj);
    CALL(gUnk_08351648[obj->type].unk10)(obj);
    return obj;
}

void FireCounterInit(struct Object* arg0) {
    arg0->base.counter = 0;
    arg0->unk78 = FN(FireCounterIncrement);
}

static void FireCounterIncrement(struct Object* arg0) {
    arg0->base.counter++;
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_fire) {
    PORT_FNID(CreateFire),
    PORT_FNID(FireCounterInit),
    PORT_FNID(FireCounterIncrement),
    PORT_FNID_END
};
/* PORT-FNID end */
