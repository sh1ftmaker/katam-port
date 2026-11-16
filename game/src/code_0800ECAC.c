
#ifdef __cplusplus
extern "C" {
#endif

#include "object_collision.h"
#include "code_0800ECAC.h"
#include "functions.h"
#include "kirby.h"
#include "constants/kirby.h"
#include "port/localmp.h"
#include "port/simblock.h"

void CpuBrainTask(void);
void CpuScriptStep(struct CpuBrain *);
void CpuActThink(struct CpuBrain *);
void CpuBrainTaskDestroy(struct Task *);
void CpuGoalNone(struct CpuBrain *);
void CpuSenseNone(struct CpuBrain *);
bool32 CpuAiGoal(struct CpuBrain *);
bool32 CpuAiWants(struct CpuBrain *);

void CpuBrainCreate(u8 slot, u16 startRoom, u16 homeRoom) {
    struct CpuBrain *brain = &gUnk_02038590[slot];
    struct Task *t = TaskCreate(FN(CpuBrainTask), sizeof(struct CpuBrainTaskData), slot | 0x2100, TASK_USE_IWRAM, FN(CpuBrainTaskDestroy));
    struct CpuBrainTaskData *taskData = TaskGetStructPtr(t);
    const struct RoomProps *rp;

    CpuFill16(0, taskData, sizeof(struct CpuBrainTaskData));
    CpuFill16(0, brain, sizeof(struct CpuBrain));
    taskData->slot = slot;
    brain->levelInfo = &gCurLevelInfo[slot];
    brain->homeRoom = homeRoom;
    brain->kirby = &gKirbys[slot];
    brain->leader = 0;
    brain->destRoom = startRoom;
    rp = &gRoomProps[startRoom];
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalNone);
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseNone);
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    brain->flags = 0;
    brain->curNode = 0xFF;
    brain->goalNode = 0xFF;
    brain->nextNode = 0xFF;
    brain->nodeChanges = 0;
    brain->swallowCooldown = 0;
    brain->followKirby = 0;
    brain->script = gUnk_08D63C28[rp->unk22]->unk0[KATAM_ROM_ROW(slot)];
    brain->nearHumanMeter = 0;
    brain->roomHopTimer = 0;
    brain->followStillFrames = 0;
    brain->scriptPc = 0;
    brain->unkBA = 0;
    brain->action = CPU_ACT_NONE;
    brain->prevAction = 0xFF;
}

void CpuBrainReset(u8 slot, u32 a2 __attribute__((unused))) {
    struct CpuBrain *brain = &gUnk_02038590[slot];
    const struct RoomProps *rp;

    rp = &gRoomProps[brain->levelInfo->currentRoom];
    brain->layers[CPU_LAYER_SCRIPT] = FN(CpuScriptStep);
    brain->layers[CPU_LAYER_GOAL] = FN(CpuGoalNone);
    brain->layers[CPU_LAYER_SENSE] = FN(CpuSenseNone);
    brain->layers[CPU_LAYER_ACTION] = FN(CpuActThink);
    brain->flags = 0;
    brain->curNode = 0xFF;
    brain->goalNode = 0xFF;
    brain->nextNode = 0xFF;
    brain->nodeChanges = 0;
    brain->swallowCooldown = 0;
    brain->followKirby = 0;
    brain->script = gUnk_08D63C28[rp->unk22]->unk0[KATAM_ROM_ROW(slot)];
    brain->nearHumanMeter = 0;
    brain->roomHopTimer = 0;
    brain->followStillFrames = 0;
    brain->scriptPc = 0;
    brain->unkBA = 0;
    brain->wantAbility = 0;
    brain->action = CPU_ACT_NONE;
    brain->prevAction = 0xFF;
}

u8 CpuNavNodeAt(struct ObjectBase *a1) {
    struct LevelInfo *li = &gCurLevelInfo[a1->unk56];
    s16 vars[] = { a1->x >> 12, a1->y >> 12 };

    if (vars[0] < 0)
        vars[0] = 0;
    else if (vars[0] >= li->roomWidth >> 4)
        vars[0] = (li->roomWidth >> 4) - 1;
    if (vars[1] < 0)
        vars[1] = 0;
    else if (vars[1] >= li->roomHeight >> 4)
        vars[1] = (li->roomHeight >> 4) - 1;
    return sub_080026A8(a1->unk56, vars[0], vars[1]);
}

void CpuBrainTask(void) {
    struct CpuBrainTaskData *taskData = TaskGetStructPtr(gCurTask);
    struct CpuBrain *brain = &gUnk_02038590[taskData->slot];
    struct Kirby *kirby = brain->kirby;

    /* PORT: local multiplayer -- a person drives this Kirby now; the brain
     * waits, and is reset when the CPU takes it back (platform/localmp.c). */
    if (PortLmpBrainWaits(taskData->slot))
        return;
    if (!Macro_0810B1F4(&brain->kirby->base)
        || kirby->base.flags & 0x2000) {
        brain->prevKeys = brain->keys;
        brain->keys = 0;
        if (kirby->hp > 0
            && !(kirby->base.unkC & 0x8000)
            && brain->levelInfo->currentRoom != 0xFFFF
            && !(kirby->base.flags & 0x1000000)
            && kirby->animationIndex != 0x5A) {
            CALL(brain->layers[CPU_LAYER_SCRIPT])(brain);
            /* PORT: CPU AI modes (game/src/cpu_ai_modes.c, port/cpu_ai.h):
             * all zero -- every Kirby CLASSIC, and no planner directive
             * ever (docs/design/CPU-HAIKU.md) -- is the stock brain. */
            if (!CpuAiWants(brain) || !CpuAiGoal(brain))
                CALL(brain->layers[CPU_LAYER_GOAL])(brain);
            CALL(brain->layers[CPU_LAYER_SENSE])(brain);
            CALL(brain->layers[CPU_LAYER_ACTION])(brain);
        }
    }
}

// TODO: decompile CpuScanTerrain.brain
// unk1C has to be a few separate arrays as confirmed in CpuJumpNeeded so why we have out-of-bounds r/w here?
// https://decomp.me/scratch/Au4Qp
/*
void CpuScanTerrain(struct CpuBrain *a1) {
    u16 r0, r0_2;
    struct Kirby *kirby = a1->kirby;
    struct LevelInfo *li = a1->levelInfo;
    s16 array[] = {
        li->unk6C >> 12, li->unk70 >> 12,
        li->unk74 >> 12, li->unk78 >> 12,
    };
    u16 sp10 = kirby->base.x >> 12;
    u16 sl = kirby->base.y >> 12;
    u8 *r5;
    bool32 r6 = FALSE;
    PTR32(struct Object) *r7;
    s16 sp14;
    s16 sp18;

    a1->terrainRow[2] = 0;
    switch (gCollisionAttributes[sub_080024F0(a1->levelInfo, sp10, sl)] & 0xF0000000) {
    case 0x20000000:
    case 0x30000000:
    case 0x40000000:
    case 0x60000000:
        r6 = TRUE;
        break;
    }
    r0 = array[0];
    r5 = a1->terrainRow;
    if (r0 >= sp10 - 1 || sp10 - 1 >= (u16)array[2])
        r5[1] = 0xFF;
    else if (a1->terrainRow[2])
        r5[1] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + -1, sl)];

        if (r1 & 0x2000)
            r5[1] = 1;
        else if (r1 & 0x200) {
            if (r6)
                r5[1] = 0;
            else if (r1 & 0x1000)
                r5[1] = 3;
            else
                r5[1] = 2;
        } else if (r1 & 0x400) {
            r5[1] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[1] = 0;
        }
    }
    if ((u16)array[0] >= sp10 - 2 || sp10 - 2 >= (u16)array[2])
        r5[0] = 0xFF;
    else if (a1->terrainRow[1])
        r5[0] = a1->terrainRow[1];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + -2, sl)];

        if (r1 & 0x2000)
            r5[0] = 1;
        else if (r1 & 0x200) {
            if (r1 & 0x1000)
                r5[0] = 3;
            else
                r5[0] = 2;
        } else if (r1 & 0x400) {
            r5[0] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[0] = 0;
        }
    }
    if ((u16)array[0] >= sp10 + 1 || sp10 + 1 >= (u16)array[2])
        r5[3] = 0xFF;
    else if (a1->terrainRow[2])
        r5[3] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + 1, sl)];

        if (r1 & 0x2000)
            r5[3] = 1;
        else if (r1 & 0x200) {
            if (r6)
                r5[3] = 0;
            else if (r1 & 0x1000)
                r5[3] = 3;
            else
                r5[3] = 2;
        } else if (r1 & 0x400) {
            r5[3] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[3] = 0;
        }
    }
    if ((u16)array[0] >= sp10 + 2 || sp10 + 2 >= (u16)array[2])
        r5[4] = 0xFF;
    else if (a1->terrainRow[3])
        r5[4] = a1->terrainRow[3];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + 2, sl)];

        if (r1 & 0x2000)
            r5[4] = 1;
        else if (r1 & 0x200) {
            if (r1 & 0x1000)
                r5[4] = 3;
            else
                r5[4] = 2;
        } else if (r1 & 0x400) {
            r5[4] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[4] = 0;
        }
    }
    r0_2 = array[1];
    r5 = a1->terrain;
    if (r0_2 >= sl - 1 || sl - 1 >= (u16)array[3])
        r5[7] = 0xFF;
    else if (a1->terrainRow[2])
        r5[7] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10, sl + -1)];

        if (r1 & 0x2000)
            r5[7] = 1;
        else if (r1 & 0x200) {
            if (r1 & 0x1000)
                r5[7] = 3;
            else
                r5[7] = 2;
        } else if (r1 & 0x400) {
            r5[7] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[7] = 0;
        }
    }
    if ((u16)array[1] >= sl - 2 || sl - 2 >= (u16)array[3])
        r5[2] = 0xFF;
    else if (a1->terrain[7])
        r5[2] = a1->terrain[7];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10, sl + -2)];

        if (r1 & 0x2000)
            r5[2] = 1;
        else if (r1 & 0x200) {
            if (r1 & 0x1000)
                r5[2] = 3;
            else
                r5[2] = 2;
        } else if (r1 & 0x400) {
            r5[2] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[2] = 0;
        }
    }
    if ((u16)array[1] >= sl + 1 || sl + 1 >= (u16)array[3])
        r5[0x11] = 0xFF;
    else if (a1->terrainRow[2])
        r5[0x11] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10, sl + 1)];

        if (r1 & 0x2000)
            r5[0x11] = 1;
        else if (r1 & 0x20C) {
            if (r1 & 0x1000)
                r5[0x11] = 3;
            else
                r5[0x11] = 2;
        } else if (r1 & 0x400) {
            r5[0x11] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[0x11] = 0;
        }
    }
    if ((u16)array[1] >= sl + 2 || sl + 2 >= (u16)array[3])
        r5[0x16] = 0xFF;
    else if (a1->terrainBelow[2])
        r5[0x16] = a1->terrainBelow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10, sl + 2)];

        if (r1 & 0x2000)
            r5[0x16] = 1;
        else if (r1 & 0x20C) {
            if (r1 & 0x1000)
                r5[0x16] = 3;
            else
                r5[0x16] = 2;
        } else if (r1 & 0x400) {
            r5[0x16] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[0x16] = 0;
        }
    }
    if ((u16)array[0] >= sp10 - 1 || sp10 - 1 >= (u16)array[2]
        || (u16)array[1] >= sl - 1 || sl - 1 >= (u16)array[3])
        r5[0x6] = 0xFF;
    else if (a1->terrainRow[2])
        r5[0x6] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + -1, sl + -1)];

        if (r1 & 0x2000)
            r5[0x6] = 1;
        else if (r1 & 0x200) {
            if (r1 & 0x1000)
                r5[0x6] = 3;
            else
                r5[0x6] = 2;
        } else if (r1 & 0x400) {
            r5[0x6] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[0x6] = 0;
        }
    }
    if ((u16)array[0] >= sp10 + 1 || sp10 + 1 >= (u16)array[2]
        || (u16)array[1] >= sl - 1 || sl - 1 >= (u16)array[3])
        r5[8] = 0xFF;
    else if (a1->terrainRow[2])
        r5[8] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + 1, sl + -1)];

        if (r1 & 0x2000)
            r5[8] = 1;
        else if (r1 & 0x200) {
            if (r1 & 0x1000)
                r5[8] = 3;
            else
                r5[8] = 2;
        } else if (r1 & 0x400) {
            r5[8] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[8] = 0;
        }
    }
    if ((u16)array[0] >= sp10 + 1 || sp10 + 1 >= (u16)array[2]
        || (u16)array[1] >= sl + 1 || sl + 1 >= (u16)array[3])
        r5[0x12] = 0xFF;
    else if (a1->terrainRow[2])
        r5[0x12] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + 1, sl + 1)];

        if (r1 & 0x2000)
            r5[0x12] = 1;
        else if (r1 & 0x20C) {
            if (r1 & 0x1000)
                r5[0x12] = 3;
            else
                r5[0x12] = 2;
        } else if (r1 & 0x400) {
            r5[0x12] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[0x12] = 0;
        }
    }
    if ((u16)array[0] >= sp10 - 1 || sp10 - 1 >= (u16)array[2]
        || (u16)array[1] >= sl + 1 || sl + 1 >= (u16)array[3])
        r5[0x10] = 0xFF;
    else if (a1->terrainRow[2])
        r5[0x10] = a1->terrainRow[2];
    else {
        u32 r1 = gCollisionAttributes[sub_080024F0(li, sp10 + -1, sl + 1)];

        if (r1 & 0x2000)
            r5[0x10] = 1;
        else if (r1 & 0x20C) {
            if (r1 & 0x1000)
                r5[0x10] = 3;
            else
                r5[0x10] = 2;
        } else if (r1 & 0x400) {
            r5[0x10] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;
        } else {
            r5[0x10] = 0;
        }
    }
    r7 = GetRoomSolidCollisionList(&kirby->base);
    sp14 = kirby->base.x >> 8;
    sp18 = kirby->base.y >> 8;
    if (kirby->base.yspeed > 0) {
        if (kirby->base.flags & 1) {
            for (; *r7; ++r7) {
                s16 r1 = (*r7)->base.x >> 8;
                s16 r2 = (*r7)->base.y >> 8;

                array[0] = (*r7)->base.unk3C + r1;
                array[1] = (*r7)->base.unk3D + r2;
                array[2] = (*r7)->base.unk3E + r1;
                array[3] = (*r7)->base.unk3F + r2;
                if (array[0] <= sp14 && array[2] >= sp14) {
                    if (!r5[7]) {
                        s16 zz = sp18 - 0x10;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[7] = 2;
                    } else if (!r5[2]) {
                        s16 zz = sp18 - 0x20;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[2] = 2;
                    }
                }
                if (array[1] <= sp18 && array[3] >= sp18) {
                    if (!r5[0xB]) {
                        s16 zz = sp14 - 0x10;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xB] = 2;
                    } else if (!r5[0xA]) {
                        s16 zz = sp14 - 0x20;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xA] = 2;
                    }
                }
                if (!r5[0x6]) {
                    s16 zz2 = sp14 - 0x10;
                    s16 zz = sp18 - 0x10;

                    if (array[0] <= zz2 && array[2] >= zz2
                        && array[1] <= zz && array[3] >= zz)
                        r5[0x6] = 2;
                }
            }
        } else {
            for (; *r7; ++r7) {
                s16 r1 = (*r7)->base.x >> 8;
                s16 r2 = (*r7)->base.y >> 8;

                array[0] = (*r7)->base.unk3C + r1;
                array[1] = (*r7)->base.unk3D + r2;
                array[2] = (*r7)->base.unk3E + r1;
                array[3] = (*r7)->base.unk3F + r2;
                if (array[0] <= sp14 && array[2] >= sp14) {
                    if (!r5[7]) {
                        s16 zz = sp18 - 0x10;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[7] = 2;
                    } else if (!r5[2]) {
                        s16 zz = sp18 - 0x20;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[2] = 2;
                    }
                }
                if (array[1] <= sp18 && array[3] >= sp18) {
                    if (!r5[0xD]) {
                        s16 zz = sp14 + 0x10;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xD] = 2;
                    } else if (!r5[0xE]) {
                        s16 zz = sp14 + 0x20;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xE] = 2;
                    }
                }
                if (!r5[8]) {
                    s16 zz2 = sp14 + 0x10;
                    s16 zz = sp18 - 0x10;

                    if (array[0] <= zz2 && array[2] >= zz2
                        && array[1] <= zz && array[3] >= zz)
                        r5[8] = 2;
                }
            }
        }
    } else {
        if (kirby->base.flags & 1) {
            for (; *r7; ++r7) {
                s16 r1 = (*r7)->base.x >> 8;
                s16 r2 = (*r7)->base.y >> 8;

                array[0] = (*r7)->base.unk3C + r1;
                array[1] = (*r7)->base.unk3D + r2;
                array[2] = (*r7)->base.unk3E + r1;
                array[3] = (*r7)->base.unk3F + r2;
                if (array[0] <= sp14 && array[2] >= sp14) {
                    if (!r5[7]) {
                        s16 zz = sp18 - 0x10;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[7] = 2;
                    } else if (!r5[2]) {
                        s16 zz = sp18 - 0x20;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[2] = 2;
                    }
                }
                if (array[1] <= sp18 && array[3] >= sp18) {
                    if (!r5[0xB]) {
                        s16 zz = sp14 - 0x10;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xB] = 2;
                    } else if (!r5[0xA]) {
                        s16 zz = sp14 - 0x20;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xA] = 2;
                    }
                }
                if (!r5[0x10]) {
                    s16 zz2 = sp14 - 0x10;
                    s16 zz = sp18 + 0x10;

                    if (array[0] <= zz2 && array[2] >= zz2
                        && array[1] <= zz && array[3] >= zz)
                        r5[0x10] = 2;
                }
            }
        } else {
            for (; *r7; ++r7) {
                s16 r1 = (*r7)->base.x >> 8;
                s16 r2 = (*r7)->base.y >> 8;

                array[0] = (*r7)->base.unk3C + r1;
                array[1] = (*r7)->base.unk3D + r2;
                array[2] = (*r7)->base.unk3E + r1;
                array[3] = (*r7)->base.unk3F + r2;
                if (array[0] <= sp14 && array[2] >= sp14) {
                    if (!r5[7]) {
                        s16 zz = sp18 - 0x10;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[7] = 2;
                    } else if (!r5[2]) {
                        s16 zz = sp18 - 0x20;
                        if (array[1] <= zz && array[3] >= zz)
                            r5[2] = 2;
                    }
                }
                if (array[1] <= sp18 && array[3] >= sp18) {
                    if (!r5[0xD]) {
                        s16 zz = sp14 + 0x10;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xD] = 2;
                    } else if (!r5[0xE]) {
                        s16 zz = sp14 + 0x20;
                        if (array[0] <= zz && array[2] >= zz)
                            r5[0xE] = 2;
                    }
                }
                if (!r5[0x12]) {
                    s16 zz2 = sp14 + 0x10;
                    s16 zz = sp18 + 0x10;

                    if (array[0] <= zz2 && array[2] >= zz2
                        && array[1] <= zz && array[3] >= zz)
                        r5[0x12] = 2;
                }
            }
        }
    }
}
*/

#ifdef __cplusplus
}
#endif
/* PORT-FNID: these functions were renamed from their address names, so
 * katam.elf no longer names them; pin each code ID to its ROM address
 * (tools/codemod_fnid.py reads these):
 * PORT_FNID_ROM(CpuBrainTask, 0x0800EF61u) */

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_code_0800ECAC) {
    PORT_FNID(CpuBrainTask),
    PORT_FNID_END
};
/* PORT-FNID end */
