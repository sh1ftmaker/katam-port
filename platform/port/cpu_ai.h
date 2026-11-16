#ifndef GUARD_PORT_CPU_AI_H
#define GUARD_PORT_CPU_AI_H
/* CPU AI modes: host-selectable behaviours for the CPU Kirbys
 * (docs/design/CPU-AI.md).  Each CPU Kirby slot has a mode:
 *
 *   CLASSIC     the game's own brain, untouched (code_0800ECAC*.c)
 *   BODYGUARD   stays with the nearest person, walks (not warps) to their
 *               room through doors, eats when hurt
 *   EXPLORER    walks door to door toward rooms nobody has entered yet
 *   ADVENTURER  walks toward the nearest unbeaten boss room through the
 *               game's door graph, fights, grabs shards, eats when hurt
 *
 * The modes are simulation state: two bits per Kirby slot in the sim
 * block's spare word gPortSim->game.reserved[0] (port/simblock.h), all zero
 * = everyone CLASSIC, which is what power-on gives, so every recording,
 * test and online rig made before this is untouched.  The word changes only
 * at a latch: offline from a recorded `cpuai FRAME WORD` input-log line,
 * online from the host's sealed PORT_LN_EV_CPUAI.
 *
 * Live play (the in-game menus on: the web page, or KATAM_MENU_POC=1)
 * asks for this device's setting (gPortSettings.cpuAi, default the mix:
 * Kirby 2 BODYGUARD, Kirby 3 EXPLORER, Kirby 4 ADVENTURER, the wide build's
 * Kirbys 5.. the same three in turn); everything else -- replays, the
 * headless tools, the node rigs without menus, the attract demo -- stays
 * CLASSIC.  KATAM_CPU_AI=WORD (hex) asks for a word in a headless run. */

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum PortCpuAiMode {
    PORT_CPUAI_CLASSIC    = 0,
    PORT_CPUAI_BODYGUARD  = 1,
    PORT_CPUAI_EXPLORER   = 2,
    PORT_CPUAI_ADVENTURER = 3,
    PORT_CPUAI_MODES      = 4,
};

/* The default mix, slots 1..15 (slot 0 is always a person): 1 2 3 1 2 3... */
#define PORT_CPUAI_MIX 0xE79E79E4u
#define PORT_CPUAI_SLOTS 16

#define PortCpuAiWordMode(w, slot) ((int)(((w) >> (2 * ((slot) & 15))) & 3))

/* The applied word (sim state). */
u32  PortCpuAiWord(void);
/* At a latch (platform/inputlog.c, platform/lmpnet.c): the word from now on. */
void PortCpuAiApply(u32 word);
/* Host side, once a frame before the latch (platform/main.c): ask for this
 * device's setting when it differs (see above). */
void PortCpuAiPoll(void);
/* The setting changed (platform/menu_poc.c): ask again at the next poll. */
void PortCpuAiSettingChanged(void);
/* Diagnostics (KATAM_CPUAI_LOG=1): a brain's event, logged with the frame. */
void PortCpuAiNote(int slot, const char *what, int a, int b, int c);
int  PortCpuAiLogOn(void);
const char *PortCpuAiName(int mode);
/* game/src/cpu_ai_modes.c: KATAM_CPUAI_LOG=2 prints the door graph. */
void CpuAiDumpGraph(void);

/* --- planner directives (docs/design/CPU-HAIKU.md) ------------------------
 *
 * A slow planner (a Haiku agent, or the mock) gives one CPU Kirby at a time
 * a DIRECTIVE: a kind, three operands and a time limit.  The rule-based
 * brain carries it out frame by frame; when it ends (done, failed, timed
 * out) the Kirby's own mode runs again until the next one.  A directive is
 * an input: it enters the simulation only at a latch, from a recorded
 * `cpugoal FRAME SLOT KIND A B C SECS` input-log line (platform/inputlog.c),
 * and lives in the brain's filler bytes (cpu_ai_modes.c), so snapshots and
 * hashes carry it and a replay repeats it without asking anyone.
 *
 *   kind        A                    B                C          ends DONE when
 *   goto_room   room                 door group + 1   -          in the room (and group)
 *   goto_door   room (65535: here)   door tile x      tile y     through that door
 *   goto_xy     room (65535: here)   tile x           tile y     within a tile of it
 *   follow      Kirby slot           -                -          time up, in its room
 *   explore     -                    -                -          in a room nobody entered
 *   fight       -                    -                -          no boss/enemy left here
 *   food        -                    -                -          its HP went up
 *   wait        -                    -                -          time up
 *   auto        -                    -                -          time up (its mode ran)
 *   cancel      -                    -                -          (clears it at once)   */
enum PortCpuGoalKind {
    PORT_CPUGOAL_NONE    = 0,   /* `cancel`: no directive, the mode runs        */
    PORT_CPUGOAL_ROOM    = 1,
    PORT_CPUGOAL_DOOR    = 2,
    PORT_CPUGOAL_XY      = 3,
    PORT_CPUGOAL_FOLLOW  = 4,
    PORT_CPUGOAL_EXPLORE = 5,
    PORT_CPUGOAL_FIGHT   = 6,
    PORT_CPUGOAL_FOOD    = 7,
    PORT_CPUGOAL_WAIT    = 8,
    PORT_CPUGOAL_AUTO    = 9,
    PORT_CPUGOAL_KINDS   = 10,
};
enum PortCpuGoalStatus {
    PORT_CPUGOAL_ST_NONE    = 0,    /* never had one (or cancelled)           */
    PORT_CPUGOAL_ST_RUNNING = 1,
    PORT_CPUGOAL_ST_DONE    = 2,
    PORT_CPUGOAL_ST_FAILED  = 3,
    PORT_CPUGOAL_ST_TIMEOUT = 4,
};
/* Why a directive ended (its B byte once it has). */
enum PortCpuGoalWhy {
    PORT_CPUGOAL_WHY_OK       = 0,
    PORT_CPUGOAL_WHY_TIME     = 1,  /* the seconds ran out                    */
    PORT_CPUGOAL_WHY_NOROUTE  = 2,  /* no door route from here                */
    PORT_CPUGOAL_WHY_GAVEUP   = 3,  /* walked at a door 30 s and gave it up   */
    PORT_CPUGOAL_WHY_LEFT     = 4,  /* ended up in another room               */
    PORT_CPUGOAL_WHY_NODOOR   = 5,  /* no such door / it is not open now      */
    PORT_CPUGOAL_WHY_NOTHING  = 6,  /* nothing to fight / no food here        */
    PORT_CPUGOAL_WHY_BADARG   = 7,  /* no such room / Kirby                   */
    PORT_CPUGOAL_WHY_COUNT    = 8,
};
#define PORT_CPUGOAL_HERE 0xFFFFu   /* operand A of goto_door / goto_xy */

/* Names in the input log and the planner protocol. */
const char *PortCpuGoalName(int kind);
int  PortCpuGoalKindOf(const char *name);   /* -1 unknown */
const char *PortCpuGoalStatusName(int status);
const char *PortCpuGoalWhyName(int why);

/* The latch (platform/inputlog.c): give CPU Kirby `slot` a directive (kind
 * PORT_CPUGOAL_NONE clears it).  SECS 0 = the kind's default. */
void PortCpuGoalApply(u32 slot, int kind, u32 a, u32 b, u32 c, u32 secs);
void CpuAiDirect(u32 slot, int kind, u32 a, u32 b, u32 c, u32 secs);   /* game side */

/* Read-only views for the planner host (platform/cpu_planner.c); they
 * never write simulation state. */
struct PortCpuGoalState {
    u8  kind, status, why, left;
    u16 a;
    u8  b, c;
};
int CpuAiDirState(u32 slot, struct PortCpuGoalState *out);

#define PORT_CPUAI_VIEW_DOORS 16
struct PortCpuAiView {
    u16 room, prevRoom;
    u8  area, flags;            /* the brain room table's (gUnk_08D6CD0C)    */
    u8  tier;                   /* blocks its ability breaks, 0..3           */
    u8  script;                 /* 1: the room's CPU script is not just roam */
    u8  entered;                /* the room's "entered" flag                 */
    u8  nDoors, moreDoors;
    struct {
        u8  x, y;               /* door tile                                 */
        u8  group;              /* its door group (index of the group's first door) */
        u8  open;               /* the door tile is on the map now           */
        u8  reach;              /* the Kirby can get to it from where it is  */
        u8  entered;            /* the room behind it has been entered       */
        u16 to;                 /* the room behind it                        */
    } doors[PORT_CPUAI_VIEW_DOORS];
    u16 exploreRoom, bossRoom;  /* nearest rooms worth it by door routes, 0xFFFF none */
    u8  exploreX, exploreY, bossX, bossY;   /* their first door here         */
    u8  count[5];               /* food, shard, boss, chest, enemy (capped)  */
    u8  nearX[5], nearY[5];     /* the nearest of each, tiles                */
    u8  nBlocked;
    u16 blockedFrom[3], blockedTo[3];
    u16 roomsEntered;           /* the save's entered rooms, all areas       */
    u8  bossBeaten;             /* bit i: gCpuLinkedRooms[i]'s boss beaten   */
    u16 bossRooms[8][2];        /* (area entrance room, boss room) pairs     */
};
int CpuAiView(u32 slot, struct PortCpuAiView *v);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_CPU_AI_H */
