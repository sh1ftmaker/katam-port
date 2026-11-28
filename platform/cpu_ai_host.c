/* CPU AI modes: the word in the sim block, and asking for this device's
 * setting (port/cpu_ai.h).  The brains themselves are game code:
 * game/src/cpu_ai_modes.c.
 *
 * Who asks.  The modes are simulation state, so a change is an input: a
 * recorded `cpuai FRAME WORD` line offline (platform/inputlog.c), the host's
 * sealed PORT_LN_EV_CPUAI online (platform/lmpnet.c).  This device asks only
 * in live play -- the in-game menus on (the web page; KATAM_MENU_POC=1
 * natively), not replaying, not a guest -- or when KATAM_CPU_AI=WORD (hex)
 * says so for a headless run.  Everything else keeps the power-on word, 0:
 * every CPU Kirby CLASSIC, the game's own brain. */
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "port/port.h"
#include "port/simblock.h"
#include "port/cpu_ai.h"
#include "port/inputlog.h"
#include "port/lmpnet.h"
#include "port/menu_poc.h"
#include "ui/settings.h"
#include "data.h"
#include "kirby.h"
#include "treasures.h"

#define CPUAI_WORD (gPortSim->game.reserved[0])

u32 PortCpuAiWord(void)
{
    return CPUAI_WORD;
}

static int sLog = -1;

int PortCpuAiLogOn(void)
{
    if (sLog < 0) {
        const char *e = getenv("KATAM_CPUAI_LOG");

        sLog = e != NULL && atoi(e) > 0 ? atoi(e) : 0;
    }
    return sLog;
}

const char *PortCpuAiName(int mode)
{
    static const char *const kNames[PORT_CPUAI_MODES] = { "CLASSIC", "BODYGUARD", "EXPLORER", "ADVENTURER" };

    return kNames[mode & 3];
}

static int sDumped;

void PortCpuAiApply(u32 word)
{
    if (PortCpuAiLogOn() > 1 && !sDumped) {
        sDumped = 1;
        CpuAiDumpGraph();
    }
    word &= ~3u;                        /* slot 0 is always a person */
    if (word != CPUAI_WORD)
        PortLog("[cpuai] f%u modes %08X -> %08X", (unsigned)gPortSim->frame.frameCount,
                (unsigned)CPUAI_WORD, (unsigned)word);
    CPUAI_WORD = word;
}

/* Host state: what this device last asked for, and when. */
static u32 sAsked, sAskedAt;
static int sPending, sEnv = -1;
static u32 sEnvWord, sEnvAt;

void PortCpuAiSettingChanged(void)
{
    sPending = 0;
}

/* Diagnostics (KATAM_CPUAI_LOG): every Kirby's room changes, deaths and
 * the shard count, whatever the modes -- so CLASSIC runs compare. */
static u16 sObsRoom[PORT_CPUAI_SLOTS];
static s8 sObsHp[PORT_CPUAI_SLOTS];
static u16 sObsShards;

static void Observe(u32 now)
{
    int i, n = gNumKirbys < PORT_CPUAI_SLOTS ? gNumKirbys : PORT_CPUAI_SLOTS;

    for (i = 0; i < n; i++) {
        u16 r = gCurLevelInfo[i].currentRoom;
        s8 hp = gKirbys[i].hp;

        if (r != sObsRoom[i] && r != 0xFFFF) {
            PortLog("[cpuai-obs] f%u k%d room %u", (unsigned)now, i, (unsigned)r);
            sObsRoom[i] = r;
        }
        if (hp <= 0 && sObsHp[i] > 0)
            PortLog("[cpuai-obs] f%u k%d died in %u", (unsigned)now, i, (unsigned)r);
        sObsHp[i] = hp;
    }
    if (NumShardsCollected() != sObsShards) {
        sObsShards = NumShardsCollected();
        PortLog("[cpuai-obs] f%u shards %u", (unsigned)now, (unsigned)sObsShards);
    }
}

void PortCpuAiPoll(void)
{
    u32 want, now = gPortSim->frame.frameCount;

    if (sEnv < 0) {
        const char *e = getenv("KATAM_CPU_AI");

        sEnv = e != NULL && *e;
        if (sEnv)
            sEnvWord = (u32)strtoul(e, NULL, 16) & ~3u;
        /* KATAM_CPU_AI_AT=F: not before frame F (a scripted run that wants
         * the stock brain through the opening, as tests/cpu_ai.sh's `cpuai
         * 1300` lines do) */
        e = getenv("KATAM_CPU_AI_AT");
        sEnvAt = e != NULL ? (u32)strtoul(e, NULL, 10) : 0;
    }
    if (PortCpuAiLogOn() && !gPortLnOn)
        Observe(now);
    if (PortInputLogReplaying())
        return;
    if (gPortLnOn) {
        if (PortLnRole() != PORT_LN_HOST || PortLnShadowRunning())
            return;
    }
    if (sEnv && now < sEnvAt)
        return;
    if (sEnv)
        want = sEnvWord;
    else if (PortMenuPoc())
        want = (u32)gPortSettings.cpuAi & ~3u;
    else
        return;
    if (want == CPUAI_WORD) {
        sPending = 0;
        return;
    }
    if (sPending && sAsked == want && now - sAskedAt < 30)
        return;                         /* asked: it lands at a latch */
    if (gPortLnOn) {
        if (PortLnEvent(PORT_LN_EV_CPUAI, (int)want, 0, 0, 0) < 0)
            return;
    } else {
        PortInputLogScheduleCpuAi(want);
    }
    sPending = 1;
    sAsked = want;
    sAskedAt = now;
}

void PortCpuAiNote(int slot, const char *what, int a, int b, int c)
{
    if (!PortCpuAiLogOn())
        return;
    PortLog("[cpuai] f%u k%d %s %d %d %d", (unsigned)gPortSim->frame.frameCount, slot, what, a, b, c);
}

/* --- planner directives (port/cpu_ai.h, docs/design/CPU-HAIKU.md) ------- */

static const char *const kGoalNames[PORT_CPUGOAL_KINDS] = {
    "cancel", "goto_room", "goto_door", "goto_xy", "follow", "explore", "fight", "food", "wait", "auto",
};

const char *PortCpuGoalName(int kind)
{
    return kind >= 0 && kind < PORT_CPUGOAL_KINDS ? kGoalNames[kind] : "?";
}

int PortCpuGoalKindOf(const char *name)
{
    int k;

    for (k = 0; k < PORT_CPUGOAL_KINDS; k++)
        if (strcmp(name, kGoalNames[k]) == 0)
            return k;
    return -1;
}

const char *PortCpuGoalStatusName(int status)
{
    static const char *const kSt[] = { "none", "running", "done", "failed", "timeout" };

    return status >= 0 && status <= PORT_CPUGOAL_ST_TIMEOUT ? kSt[status] : "?";
}

const char *PortCpuGoalWhyName(int why)
{
    static const char *const kWhy[PORT_CPUGOAL_WHY_COUNT] = {
        "ok", "time", "no_route", "gave_up_door", "left_room", "no_door", "nothing_here", "bad_target",
    };

    return why >= 0 && why < PORT_CPUGOAL_WHY_COUNT ? kWhy[why] : "?";
}

/* The latch (platform/inputlog.c; online, later, a sealed event). */
void PortCpuGoalApply(u32 slot, int kind, u32 a, u32 b, u32 c, u32 secs)
{
    PortLog("[cpugoal] f%u k%u %s %u %u %u %us", (unsigned)gPortSim->frame.frameCount, (unsigned)slot,
            PortCpuGoalName(kind), (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)secs);
    CpuAiDirect(slot, kind, a, b, c, secs);
}
