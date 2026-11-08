
#ifdef __cplusplus
extern "C" {
#endif

#include "kirby.h"
#include "functions.h"
#include "lever_wall.h"
#include "code_0800E0E4.h"

static void LeverWallAction(struct Object*);
static void nullsub_115(struct Object*);

static void LeverWallAction(struct Object* arg0) {
    bool16 r6 = FALSE;
    s16 x, y;
    struct Object *sb;
    switch (arg0->objTemplate->subtype1) {
    case 0:
        if (sub_080029BC(EWRAM_MIRROR_U8(&gCurLevelInfo[arg0->base.unk56].unk65E)) == arg0->objTemplate->unkF) {
            r6 = TRUE;
        }
        break;
    case 1:
        if (sub_080029E4(EWRAM_MIRROR_U8(&gCurLevelInfo[arg0->base.unk56].unk65E)) == arg0->objTemplate->unkF) {
            r6 = TRUE;
        }
        break;
    }
    if (r6) {
        sb = arg0;
        x = arg0->base.x >> 12;
        y = arg0->base.y >> 12;
        sub_08001408(arg0->base.unk56, sub_080025AC(arg0->base.unk56, x, y), 0, 0);
        sub_0800E0E4(arg0, x, y);
        PlaySfx(&arg0->base, SE_BLOCK_BREAK);
        sub_0800E0E4(sb, x, y);
        arg0->unk78 = FN(nullsub_115);
    }
}

void* CreateLeverWall(struct ObjectTemplate* arg0, u8 arg1) {
    struct Object *obj;
    struct Task *task = TaskCreate(FN(ObjectMain), sizeof(struct Object), 0x1000, TASK_USE_IWRAM, FN(ObjectDestroy));
    obj = TaskGetStructPtr(task);
    InitObject(obj, arg0, arg1);
    obj->base.flags |= 0x02018F40;
    obj->base.unkC |= 1;
    CALL(gUnk_08351648[obj->type].unk10)(obj);
    return obj;
}

void LeverWallSetFunc(struct Object* arg0) {
    arg0->unk78 = FN(LeverWallAction);
}

static void nullsub_115(struct Object* arg0) {}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_lever_wall) {
    PORT_FNID(LeverWallAction),
    PORT_FNID(CreateLeverWall),
    PORT_FNID(LeverWallSetFunc),
    PORT_FNID(nullsub_115),
    PORT_FNID_END
};
/* PORT-FNID end */
