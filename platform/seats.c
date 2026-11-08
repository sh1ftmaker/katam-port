/* Several game states through one instance -- see platform/port/seats.h. */
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/rollback.h"
#include "port/seats.h"
#include "port/game_addrs.h"
#include "port/census.h"
#include "port/inputlog.h"

static int sActive;
static int sSeats;
static int sCur;
static u32 sSnapBytes;
static u8 *sStates;                 /* sSeats snapshots, sSnapBytes apart    */

/* platform/main.c, the key latch.  main.c defines it inside extern "C", so
 * the LP64 build (all C++) has to say so here too, or the call mangles and
 * fails to link. */
#ifdef __cplusplus
extern "C"
#endif
void PortRbApplyKeys(u16 keys);

static u16 sPending[PORT_SEAT_MAX]; /* set by the host for the next round    */
static u8  sPendingUndriven[PORT_SEAT_MAX];   /* inverted (0 = driven), so the
                                             * default is "everyone driven"
                                             * at any PORT_SEAT_MAX)          */
static u16 sRound[PORT_SEAT_MAX];   /* latched when seat 0 begins a round    */
static u8  sRoundDriven[PORT_SEAT_MAX];
static u8  sPlayerCount = PORT_SEAT_MAX;

/* Census mode (platform/census.c): the seats differ only in gLocalPlayerId
 * and the display task's player.  No input words, no player count: the
 * replayed recording drives the game exactly as it does without seats. */
static int sCensus;
static int sCensusArm;
/* Session mode, from frame sCensusF0 on: the shape of a NETPLAY-V2 session.
 * All four slots exist; the game's network-input path (gUnk_0203AD10 bit 1)
 * with the player count at four; slot 0 driven by the recording's keys, the
 * other slots by the game's own AI, written where the game reads them -- what
 * platform/rollback.c's ApplySlotInputs does for a session with one human.
 * Before F0 every seat keeps focus 0, so the world is created identically
 * and the seats differ from F0 by exactly what a joiner's re-pin does. */
static s32 sCensusF0 = -1;

void PortSeatCensusArm(int seats, s32 sessionFrom)
{
    sCensusArm = seats;
    sCensusF0 = sessionFrom;
}

/* Seat i is local player (i + PORT_CENSUS_FOCUS) % 4: with one seat, a single
 * instance of any focus, for chasing one divergence under a debugger. */
static int CensusFocus(void)
{
    const char *e = getenv("PORT_CENSUS_FOCUS");

    return (sCur + (e ? atoi(e) : 0)) % 4;
}

static void CensusBeginFrame(void)
{
    u32 frame = PortFrameNumber();
    int slot;

    if (sCensusF0 < 0) {
        GAME_REPIN_FOCUS(CensusFocus());
        return;
    }
    if (frame < (u32)sCensusF0)
        return;
    GAME_MODE_FLAGS |= 2;
    gPortSim->hdr.flags |= PORT_SIM_F_SESSION;
    GAME_REPIN_FOCUS(CensusFocus());
    GAME_PLAYER_COUNT = 4;
    for (slot = 0; slot < 4; slot++) {
        u16 prev = GAME_HELD(slot);
        u16 now = slot == 0 ? (u16)(PortInputLogKeysAt(frame) & 0x3FF)
                            : (u16)GAME_AI_INPUT(slot);

        GAME_HELD(slot) = now;
        GAME_PRESSED(slot) = (u16)(now & ~prev);
        GAME_RELEASED(slot) = (u16)(prev & ~now);
    }
}

/* What each seat's KEYINPUT must latch at the end of its PortPresentFrame:
 * the keys applied at its top.  Per seat, because the switch happens between
 * the two and the loaded seat's top was a round ago. */
static u16 sSeatKeys[PORT_SEAT_MAX];
static u8  sStarted[PORT_SEAT_MAX];     /* brought out at least once           */

static u8 *StateOf(int seat)
{
    return sStates + (size_t)seat * sSnapBytes;
}

int PortSeatInit(int seats)
{
    int i;

    if (seats < 1 || seats > PORT_SEAT_MAX) {
        PortError("[katam-port] seats: %d is not 1..%d", seats, PORT_SEAT_MAX);
        return 0;
    }
    if (PortRbActive()) {
        PortError("[katam-port] seats: shut the rollback session down first");
        return 0;
    }
    PortSeatShutdown();

    sSnapBytes = PortRbSnapshotBytes();
    sStates = (u8 *)malloc((size_t)sSnapBytes * (size_t)seats);
    if (sStates == NULL) {
        PortError("[katam-port] seats: no memory for %d x %u bytes",
                  seats, sSnapBytes);
        return 0;
    }
    /* Every seat starts as a copy of this moment.  Their histories diverge
     * from the next frame, when each gets its own camera. */
    PortRbSnapshotSave(StateOf(0));
    for (i = 1; i < seats; i++)
        memcpy(StateOf(i), StateOf(0), sSnapBytes);

    /* The round in progress: the inputs the host set before calling this
     * are the ones the cloned frame was simulated with, so they are what
     * every seat's console latches when it is first brought out.  From here
     * on the host sets the *next* round's. */
    for (i = 0; i < PORT_SEAT_MAX; i++) {
        sRound[i] = sPending[i];
        sRoundDriven[i] = !sPendingUndriven[i];
        sSeatKeys[i] = sRoundDriven[i] ? sRound[i] : 0;
        sStarted[i] = 0;
    }
    sSeats = seats;
    /* The frame this was called from is the last of a round: the switch at
     * its end brings seat 0 out, so every round runs 0..seats-1, and seat 0's
     * top -- where the next round's inputs are latched -- comes first. */
    sCur = seats - 1;
    sActive = 1;
    PortLog("[katam-port] seats: %d seats, %u bytes each", seats, sSnapBytes);
    return 1;
}

void PortSeatShutdown(void)
{
    free(sStates);
    sStates = NULL;
    sActive = 0;
    sSeats = 0;
    sCur = 0;
}

int PortSeatActive(void)  { return sActive; }
int PortSeatCount(void)   { return sSeats; }
int PortSeatCurrent(void) { return sCur; }
int PortSeatIsLast(void)  { return !sActive || sCur == sSeats - 1; }

void PortSeatSetInput(int slot, u16 keys)
{
    if (slot >= 0 && slot < PORT_SEAT_MAX)
        sPending[slot] = keys & 0x03FF;
}

void PortSeatSetDriven(int slot, int driven)
{
    if (slot >= 0 && slot < PORT_SEAT_MAX)
        sPendingUndriven[slot] = (u8)(driven == 0);
}

void PortSeatSetPlayerCount(int count)
{
    if (count >= 1 && count <= PORT_SEAT_MAX)
        sPlayerCount = (u8)count;
}

/* The top of PortPresentFrame: the same two jobs, in the same order, that
 * PortRbFrame does for a timeline frame -- pin the camera to this seat's
 * Kirby, then write every slot's held/pressed/released words. */
void PortSeatBeginFrame(void)
{
    int slot;

    if (!sActive)
        return;

    if (sCensus) {
        CensusBeginFrame();
        return;
    }

    if (sCur == 0) {
        memcpy(sRound, sPending, sizeof sRound);
        int i;

        for (i = 0; i < PORT_SEAT_MAX; i++)
            sRoundDriven[i] = !sPendingUndriven[i];
    }

    GAME_REPIN_FOCUS(sCur);

    GAME_PLAYER_COUNT = sPlayerCount;
    for (slot = 0; slot < PORT_SEAT_MAX; slot++) {
        u16 prev = GAME_HELD(slot);
        u16 now;

        if (slot >= sPlayerCount)
            continue;
        now = sRoundDriven[slot] ? sRound[slot] : (u16)GAME_AI_INPUT(slot);
        GAME_HELD(slot) = now;
        GAME_PRESSED(slot) = (u16)(now & ~prev);
        GAME_RELEASED(slot) = (u16)(prev & ~now);
    }

    /* This seat's own buttons, as its console would read them. */
    sSeatKeys[sCur] = sRoundDriven[sCur] ? sRound[sCur] : 0;
    PortRbApplyKeys(sSeatKeys[sCur]);
}

/* The end of PortPresentFrame, before the key latch: this seat is at the
 * frame boundary, so put it away and bring the next one out. */
void PortSeatEndFrame(void)
{
    /* A census starts here, at the switch point, so the clones resume exactly
     * where they were taken (PortSeatInit's contract). */
    if (sCensusArm > 0 && !sActive) {
        int n = sCensusArm;

        sCensusArm = 0;
        if (PortSeatInit(n))
            sCensus = 1;
    }
    if (!sActive)
        return;
    PortRbSnapshotSave(StateOf(sCur));
    if (sCensus && sCur == sSeats - 1) {
        const u8 *st[PORT_SEAT_MAX];
        int i;

        for (i = 0; i < sSeats; i++)
            st[i] = StateOf(i);
        PortCensusRound(st, sSeats);
    }
    sCur = (sCur + 1) % sSeats;
    PortRbSnapshotLoad(StateOf(sCur));
    /* Its camera is its own from the first instruction it runs, not from
     * its next top: the clone every seat started as was focused on Kirby 0,
     * and the rest of this PortPresentFrame plus one GameLoop iteration
     * run before PortSeatBeginFrame gets to say otherwise.  Measured as
     * one differing frame per seat against a real instance whose focus
     * was set (PortRbNetPlay: this one write, no more) at the blit the
     * clone was taken from.  Once only, and only the id: re-pointing the
     * display task belongs at the top of the frame, where the reference
     * does it -- doing it here as well cost 30-85 differing frames. */
    if (!sStarted[sCur]) {
        sStarted[sCur] = 1;
        if (!(sCensus && sCensusF0 >= 0))
            GAME_FOCUS = (u8)(sCensus ? CensusFocus() : sCur);
    }
}

int PortSeatKeyOverride(u16 *keys)
{
    if (!sActive || sCensus)
        return 0;
    *keys = sSeatKeys[sCur];
    return 1;
}
