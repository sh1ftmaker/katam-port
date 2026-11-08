
#ifdef __cplusplus
extern "C" {
#endif

#include "master_sword.h"
#include "kirby.h"
#include "functions.h"

static void sub_08025440(struct Object* arg0);

extern const struct AnimInfo gUnk_082DEB0C[] = {
    { 0x2C3, 4, 0 },
};

void* CreateMasterSwordNonCollectible(struct ObjectTemplate* arg0, u8 arg1) {
    struct Object *obj;
    struct Task *task = TaskCreate(FN(ObjectMain), sizeof(struct Object), 0x1000, TASK_USE_IWRAM, FN(ObjectDestroy));
    obj = TaskGetStructPtr(task);
    InitObject(obj, arg0, arg1);
    obj->unk85 = 0;
    obj->base.flags |= 0x8200;
    obj->base.unk68 &= -8;
    ObjectSetBounds(&obj->base, -8, -12, 8, 12);
    ObjectInitSprite(obj);
    CALL(gUnk_08351648[obj->type].unk10)(obj);
    return obj;
}

void sub_08025434(struct Object* arg0) {
    arg0->unk78 = FN(sub_08025440);
}

static void sub_08025440(struct Object* arg0) {
    arg0->base.flags |= 4;
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_master_sword) {
    PORT_FNID(CreateMasterSwordNonCollectible),
    PORT_FNID(sub_08025434),
    PORT_FNID(sub_08025440),
    PORT_FNID_END
};
/* PORT-FNID end */
