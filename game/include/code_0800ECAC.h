#ifndef GUARD_CODE_0800ECAC_H
#define GUARD_CODE_0800ECAC_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "data.h"

struct CpuBrainTaskData { // TODO: may be the same as struct KirbyIdx?
    u8 slot;
} __attribute__((aligned(4))); /* size = 4 */

/* struct CpuBrain (data.h): the CPU Kirby brain.  docs/naming/cpu-brain.md
 * describes the whole machine. */

/* CpuBrain.layers[]: called in this order every frame by CpuBrainTask. */
enum CpuBrainLayer {
    CPU_LAYER_SCRIPT = 0,  /* the room's per-slot script (CpuScriptStep) */
    CPU_LAYER_GOAL   = 1,  /* where to go: script node, roam, follow, off-screen travel */
    CPU_LAYER_SENSE  = 2,  /* terrain grid, path vectors, enemy scan */
    CPU_LAYER_ACTION = 3,  /* what to press (CpuBrain.action, CPU_ACT_*) */
};

/* CpuBrain.flags */
#define CPU_FLAG_GOAL_REACHED   0x01  /* the goal layer reached (or timed out on) its node */
#define CPU_FLAG_ACTION_BUSY    0x02  /* committed to an action: skip the enemy scan */
#define CPU_FLAG_FREE           0x04  /* script finished or abandoned: free roam/follow */
#define CPU_FLAG_SCRIPT_WAITING 0x08  /* a script command is running */
#define CPU_FLAG_FOLLOWING      0x10  /* sticking to CpuBrain.followKirby */
#define CPU_FLAG_WITH_LEADER    0x20  /* off-screen: arrived in the leader's room */

/* CpuBrain.action: the id each action's entry function stores.  Most
 * entries check `action != X` first so a running action is not restarted. */
enum CpuAction {
    CPU_ACT_THINK          = 0x00, /* CpuActThink: pick the next action */
    CPU_ACT_DROP_ABILITY   = 0x01, /* press Select */
    CPU_ACT_ANSWER_PHONE   = 0x02, /* press R after 0..15 frames */
    CPU_ACT_MOVE           = 0x03, /* head for the waypoint: walk, swim or UFO */
    CPU_ACT_WALK           = 0x04,
    CPU_ACT_HOLD_DOWN      = 0x05, /* hold Down 60..91 frames (target straight below) */
    CPU_ACT_LADDER         = 0x06, /* best guess: climb (animations 0x2D/0x2E) */
    CPU_ACT_JUMP           = 0x07,
    CPU_ACT_FLY            = 0x08,
    CPU_ACT_SWIM           = 0x09,
    CPU_ACT_UFO_MOVE       = 0x0A,
    CPU_ACT_SLIDE          = 0x0B, /* Down, then Down+A; best guess */
    CPU_ACT_AIR_PUFF       = 0x0C, /* B while flying */
    CPU_ACT_SPIT           = 0x0D, /* B with a full mouth */
    CPU_ACT_INHALE         = 0x0E, /* hold B */
    CPU_ACT_SWALLOW        = 0x0F, /* Down with a full mouth */
    CPU_ACT_ATTACK         = 0x10, /* hold B with a copy ability */
    CPU_ACT_WATER_ATTACK   = 0x11, /* underwater: B plus a direction */
    CPU_ACT_TURN           = 0x12, /* face actArg, then run actNext */
    CPU_ACT_BOMB           = 0x13,
    CPU_ACT_STONE_TOGGLE   = 0x14, /* become stone / turn back */
    CPU_ACT_STONE_DROP     = 0x15, /* jump, turn to stone over the target */
    CPU_ACT_THROW          = 0x16,
    CPU_ACT_UFO_SHOT       = 0x17,
    CPU_ACT_CUPID_SHOT     = 0x18,
    CPU_ACT_MISSILE        = 0x19,
    CPU_ACT_MISSILE_HOMING = 0x1A,
    CPU_ACT_ROOM_397       = 0x1B, /* room 0x397 only: random walk, B every 90..121 frames */
    CPU_ACT_NONE           = 0xFF, /* before the first CpuActThink */
};

/* Script opcodes (u16 stream, CpuBrain.script; CpuScriptStep). */
enum CpuScriptOp {
    CPU_OP_NOP            = 0,
    CPU_OP_WAIT           = 1,      /* n: stand still n seconds */
    CPU_OP_GOTO_NODE      = 2,      /* n: walk to nav node n */
    CPU_OP_SET_ABILITY    = 3,      /* n: wantAbility = n */
    CPU_OP_FACE           = 4,      /* n: turn to face (1 left, 0 right) */
    CPU_OP_INHALE         = 5,      /* n: inhale n seconds */
    CPU_OP_SLIDE          = 6,      /* slide once */
    CPU_OP_HALT           = 7,      /* do nothing, forever (pc does not advance) */
    CPU_OP_SKIP           = 8,      /* n: skip n words */
    CPU_OP_IF_ROOM_STATE  = 9,      /* cmp, v, n: skip n words if room state 0xF cmp v */
    CPU_OP_ROAM           = 0xA,    /* roam the room */
    CPU_OP_END            = 0xFFFF, /* script done: CPU_FLAG_FREE */
};

extern void CpuBrainCreate(u8, u16, u16);

#ifdef __cplusplus
}
#endif

#endif
