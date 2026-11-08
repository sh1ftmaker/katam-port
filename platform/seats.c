/* Several game states through one instance -- see platform/port/seats.h. */
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/rollback.h"
#include "port/seats.h"
#include "port/game_addrs.h"

static int sActive;
static int sSeats;
static int sCur;
static u32 sSnapBytes;
static u8 *sStates;                 /* sSeats snapshots, sSnapBytes apart    */

/* C linkage: main.c defines it inside extern "C", and the 64-bit builds
 * compile this file as C++ -- a bare declaration here links against a
 * mangled name nothing defines. */
#ifdef __cplusplus
extern "C"
#endif
void PortRbApplyKeys(u16 keys);    /* platform/main.c, the key latch      */

static u16 sPending[PORT_SEAT_MAX]; /* set by the host for the next round    */
static u8  sPendingDriven[PORT_SEAT_MAX] = { 1, 1, 1, 1 };
static u16 sRound[PORT_SEAT_MAX];   /* latched when seat 0 begins a round    */
static u8  sRoundDriven[PORT_SEAT_MAX];
static u8  sPlayerCount = PORT_SEAT_MAX;

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
        sRoundDriven[i] = sPendingDriven[i];
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
        sPendingDriven[slot] = (u8)(driven != 0);
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

    if (sCur == 0) {
        memcpy(sRound, sPending, sizeof sRound);
        memcpy(sRoundDriven, sPendingDriven, sizeof sRoundDriven);
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
    if (!sActive)
        return;
    PortRbSnapshotSave(StateOf(sCur));
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
        GAME_FOCUS = (u8)sCur;
    }
}

int PortSeatKeyOverride(u16 *keys)
{
    if (!sActive)
        return 0;
    *keys = sSeatKeys[sCur];
    return 1;
}
