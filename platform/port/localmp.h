#ifndef GUARD_PORT_LOCALMP_H
#define GUARD_PORT_LOCALMP_H
/* Local multiplayer: several humans in this one game, one screen.
 * platform/localmp.c has the design.  Off unless the host or a recording
 * turns it on (native KATAM_LOCAL_MP=2, web ?localmp=2, `option localmp N`
 * in a recording); off, every hook below is a test of one zero byte. */

#include "gba/types.h"
#include "port/simblock.h"

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
/* The smallest zoom the shared camera may use (presentation). */
float PortLmpMinZoom(void);
/* The world rectangle (pixels) the shared camera wants to show this frame:
 * the humans in the local player's room plus a margin, at most the room.
 * Returns 0 when local multiplayer is not in play.  Presentation only. */
int  PortLmpViewNeed(s32 *w, s32 *h);

/* --- the latch (platform/inputlog.c, platform/main.c) -------------------- */
void PortLmpLatch(u16 keys[4]);
void PortLmpFrame(void);

/* --- game hooks ----------------------------------------------------------- */
/* sub_080332BC's human count at a single-player start. */
u8   PortLmpStartHumans(u8 n);
/* Is Kirby slot s driven by a local human other than the console's own? */
#define PortLmpHumanSlot(s) ((s) != 0 && (s) < gPortSim->lmp.humans)
#define PortLmpKeys(s)      (gPortSim->lmp.keys[(s) & 3])
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
    (PortLmpHumanSlot(gUnk_0203AD50) && gUnk_0203AD50 < gNumHumanPlayers \
     ? gUnk_0203AD50 : gLocalPlayerId)
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
