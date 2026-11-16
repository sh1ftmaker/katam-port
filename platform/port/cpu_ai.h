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

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_CPU_AI_H */
