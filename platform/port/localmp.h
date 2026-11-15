#ifndef GUARD_PORT_LOCALMP_H
#define GUARD_PORT_LOCALMP_H
/* Local multiplayer: several humans in this one game, one screen.
 * platform/localmp.c has the design.  Off unless the host or a recording
 * turns it on (native KATAM_LOCAL_MP=2, web ?localmp=2, `option localmp N`
 * in a recording); off, every hook below is a test of one zero byte. */

#include "gba/types.h"
#include "port/simblock.h"
#include "port/ram_symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- host / session ------------------------------------------------------ */
/* How many human players the next game started gets (2..4; 0 or 1 = off).
 * Takes effect at the game's own start (new game, continue), not mid-game. */
void PortLmpSetHumans(int n);
int  PortLmpSetting(void);
/* The same, chosen in the game's MULTIPLAYER > LOCAL menu (or 0 by ONE
 * PLAYER): not a host option, so not written into a recording as one -- the
 * menu keys that chose it are. */
void PortLmpChoose(int n);
int  PortLmpSettingFromHost(void);
/* A local-multiplayer game is being played now (host key routing). */
int  PortLmpInPlay(void);
/* Can player 2 press anything here (the web page: not on a touch-only
 * device until a key or gamepad shows up)?  Native: always. */
void PortLmpSetP2Input(int available);
int  PortLmpP2InputAvailable(void);
/* The host's pad for player p (1..3 = P2..P4).  Latched once a frame with
 * the console's own keys (platform/inputlog.c), recorded and replayed. */
void PortLmpSetHostKeys(int player, u16 mask);
u16  PortLmpHostKeys(int player);
/* What the latch gives players 2..4 (platform/inputlog.c): the host keys,
 * or, during an online session, the slots' keys the session decided
 * (platform/lmpnet.c sets them every latch; NULL hands back). */
void PortLmpSetSessionKeys(const u16 *keys4);
u16  PortLmpLatchKeys(int player);
/* The smallest zoom the shared camera may use (presentation). */
float PortLmpMinZoom(void);
/* The world rectangle (pixels) the shared camera wants to show this frame:
 * the humans in the local player's room plus a margin, at most the room.
 * Returns 0 when local multiplayer is not in play.  Presentation only. */
int  PortLmpViewNeed(s32 *w, s32 *h);

/* --- the latch (platform/inputlog.c, platform/main.c) -------------------- */
void PortLmpLatch(u16 keys[4]);
void PortLmpFrame(void);

/* --- human slots: drop in, drop out (platform/localmp.c, "human slots") --- */
/* Kirby slot s (1..3) becomes a person (on) or goes back to the CPU (off).
 * Applied at the latch of the frame it is stamped with: from a recording's
 * `human FRAME SLOT ON` line (platform/inputlog.c), from an online session's
 * PORT_LN_EV_HUMAN event (platform/lmpnet.c).  Simulation input; never call
 * it from the host directly -- schedule it (PortLmpRequestHuman). */
void PortLmpSetSlotHuman(int slot, int on);
/* The host asks for that at the next latch; returns the frame it was put on,
 * or 0 when refused (replaying, a session that owns the slots, bad slot).
 * Recorded as a `human` line. */
u32  PortLmpRequestHuman(int slot, int on);
/* Is slot s a person, as far as the simulation knows (1 for slot 0)?  True
 * from the latch that applied the join, also outside play (a join on the
 * title screen is held for the next game).  Read-only, for the host.
 * During an online session: is this device's local seat s playing (see
 * localmp.c, "the device layer's door"); PortLmpSlotPerson is always the
 * simulation's answer. */
int  PortLmpSlotIsHuman(int slot);
int  PortLmpSlotPerson(int slot);
/* Can the host ask for joins now (no rollback/netsim/seat session; in an
 * online session yes: the request goes to the session's host)? */
int  PortLmpCanRequest(void);
/* Would a join of slot s take effect now (not a Kirby the story is walking
 * through the intro)?  The device core prefers such slots.  Read-only. */
int  PortLmpSlotJoinable(int slot);
/* Tests (--devices `when`): is Kirby slot s in state `cond` -- dead, alive,
 * away (not in player 1's room), door (between rooms), boss (a boss is alive
 * in its room), human, cpu, anim:N, flag:HEX (base.flags)?  Read-only. */
int  PortLmpProbe(int slot, const char *cond);

/* --- game hooks ----------------------------------------------------------- */
/* sub_080332BC's human count at a single-player start. */
u8   PortLmpStartHumans(u8 n);
/* sub_080332BC, once every Kirby has its CPU brain: the humans join. */
void PortLmpStartedSlow(void);
#define PortLmpStarted() do { if (gPortSim->lmp.slots) PortLmpStartedSlow(); } while (0)
/* Is Kirby slot s driven by a local human other than the console's own? */
#define PortLmpHumanSlot(s) \
    (gPortSim->lmp.humans && (u32)(s) - 1u < 3u && ((gPortSim->lmp.slots >> (s)) & 1))

/* sub_0800EF60: a person's CPU brain waits -- except while a joiner is still
 * on its way to the group (gPortSim->lmp.joining): a CPU parked away (in the
 * hub, waiting to be called) is moved only by its brain. */
#define PortLmpBrainWaits(s) \
    (PortLmpHumanSlot(s) && !((gPortSim->lmp.joining >> (s)) & 1))

/* The game's own "is Kirby slot s a person" is `s < gNumHumanPlayers`: the
 * humans are a prefix of the slots.  With drop-in/out they are any set
 * (gPortSim->lmp.slots) and gNumHumanPlayers is how many there are, so the
 * game's slot tests and loops over humans go through these.  Not engaged
 * (gPortSim->lmp.humans == 0: every game nobody joined, link play, the demo,
 * the subgames) each one is exactly the expression it replaced. */
static inline int PortLmpSlotBit(u32 s)
{
    return s < 4 && ((gPortSim->lmp.slots >> s) & 1);
}
/* A macro, so that not engaged it is the original comparison in the
 * original operand's type.  The operand is read twice: pass a plain value. */
#define PortIsHuman(s) \
    (gPortSim->lmp.humans ? PortLmpSlotBit((u32)(s)) : ((s) < gNumHumanPlayers))
/* Loops `for (i = 0; i < gNumHumanPlayers; ++i)` over the humans become
 * `for (i = 0; i < PortHumanEnd(); i = PortHumanNext(i))` (slot 0 is always
 * a human); a test after the loop of `i == gNumHumanPlayers` ("none found")
 * becomes `i >= PortHumanEnd()`. */
static inline u32 PortHumanEnd(void)
{
    return gPortSim->lmp.humans ? 4u : gNumHumanPlayers;
}
static inline u32 PortHumanNext(u32 i)
{
    if (!gPortSim->lmp.humans)
        return i + 1;
    for (i++; i < 4 && !((gPortSim->lmp.slots >> i) & 1); i++)
        ;
    return i;
}
/* Loops `for (i = gNumHumanPlayers; i < N; ++i)` over the CPU Kirbys. */
static inline u32 PortCpuNext(u32 i)
{
    if (!gPortSim->lmp.humans)
        return i + 1;
    for (i++; i < 4 && ((gPortSim->lmp.slots >> i) & 1); i++)
        ;
    return i;
}
static inline u32 PortCpuFirst(void)
{
    return gPortSim->lmp.humans ? PortCpuNext(0) : gNumHumanPlayers;
}
/* The slots in "humans first" order: position p < gNumHumanPlayers is the
 * p-th human, the rest the CPUs, each ascending.  For loops whose position
 * matters beyond membership (FindTargetKirby: the humans first, the CPUs
 * only if none was found; the CPU's leader pick: the n-th human). */
static inline u32 PortSlotOrder(u32 p)
{
    u32 s, n = 0;

    if (!gPortSim->lmp.humans || p >= 4)
        return p;
    for (s = 0; s < 4; s++)
        if (((gPortSim->lmp.slots >> s) & 1) && n++ == p)
            return s;
    for (s = 0; s < 4; s++)
        if (!((gPortSim->lmp.slots >> s) & 1) && n++ == p)
            return s;
    return p;
}
#define PortLmpKeys(s)      (gPortSim->lmp.keys[((s) - 1) % 3u])
#define PortLmpPressed(s)   ((s) >= 1 && (s) <= 3 ? gPortSim->frame.lmpPressed[(s) - 1] : 0)
/* The camera's follow target for LevelInfo `player`: Kirby's position, or
 * the group's when local multiplayer is on and this is the local screen. */
void PortLmpCamTargetSlow(u8 player, s32 *x, s32 *y);
#define PortLmpCamTarget(player, x, y) \
    do { if (gPortSim->lmp.humans) PortLmpCamTargetSlow((player), (x), (y)); } while (0)

/* hud.c: is this Kirby inside the widest frame the screen may show? */
struct ObjectBase;
int  PortLmpInFrameSlow(const struct ObjectBase *o);
#define PortLmpInFrame(o) (gPortSim->lmp.humans && PortLmpInFrameSlow(o))
/* The pause screens: whose menu it is -- the player who paused, when that
 * is another person on this screen; otherwise the console's own player. */
#define PortLmpViewer() \
    (PortLmpHumanSlot(gUnk_0203AD50) ? gUnk_0203AD50 : gLocalPlayerId)
/* The title screen: whatever the last game was, local players are over. */
void PortLmpEndSlow(void);
#define PortLmpEnd() do { if (gPortSim->lmp.humans) PortLmpEndSlow(); } while (0)

/* sub_080562D0's end: Kirby `k` has arrived in a room (door, warp, respawn). */
struct Kirby;
void PortLmpArrivedSlow(struct Kirby *k);
#define PortLmpArrived(k) \
    do { if (gPortSim->lmp.humans) PortLmpArrivedSlow(k); } while (0)

#ifdef __cplusplus
}
#endif
#endif /* GUARD_PORT_LOCALMP_H */
