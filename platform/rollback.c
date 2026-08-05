/* Deterministic rollback, snapshots, and a join/leave timeline.
 *
 * The argument for all of it, and the measurements it rests on, are in
 * platform/port/rollback.h and docs/MULTIPLAYER.md §8-9.  This file is the
 * machinery; there is no network in it and there is not meant to be.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/rollback.h"

#ifdef __cplusplus
extern "C" {
#endif

void PortRbApplyKeys(u16 keys);

/* Defined further down, next to the code they belong with; declared here
 * because the snapshot serialiser above them needs both. */
static u32 Fnv(const void *base, u32 len);
static void Put32(u8 *p, u32 v);
static u32  Get32(const u8 *p);

/* --- what a snapshot is ---------------------------------------------------
 *
 * The six regions of the GBA map the game can write, at their true addresses,
 * plus the port's own out-of-map simulation state.  387 KiB and six memcpys,
 * because of the decision in docs/ARCHITECTURE.md rather than anything here.
 *
 * ROM is not in it: it is 16 MiB and read-only.  SRAM is not in it either, and
 * that is a judgement rather than an oversight -- it is the player's save file,
 * it changes a handful of times a session, and rewinding it would mean a
 * rollback could un-write a save.  A transport that needs save state to agree
 * should send it once at session start, which is what the game's own
 * multi-cart mode already does with the parent's world properties. */
static const struct { uintptr_t base; u32 size; const char *name; } kRegions[] = {
    { GBA_EWRAM_BASE, GBA_EWRAM_SIZE, "ewram" },
    { GBA_IWRAM_BASE, GBA_IWRAM_SIZE, "iwram" },
    { GBA_IO_BASE,    GBA_IO_SIZE,    "io"    },
    { GBA_PLTT_BASE,  GBA_PLTT_SIZE,  "pltt"  },
    { GBA_VRAM_BASE,  GBA_VRAM_SIZE,  "vram"  },
    { GBA_OAM_BASE,   GBA_OAM_SIZE,   "oam"   },
};
#define NUM_REGIONS (sizeof kRegions / sizeof kRegions[0])

static u32 MapBytes(void)
{
    u32 n = 0, i;

    for (i = 0; i < NUM_REGIONS; i++)
        n += kRegions[i].size;
    return n;
}

static u32 HostBytes(void)
{
    return PortDmaStateSize() + PortFrameStateSize() + PortPpuStateSize();
}

static void SaveTo(u8 *dest)
{
    u32 i, off = 0;

    for (i = 0; i < NUM_REGIONS; i++) {
        memcpy(dest + off, (const void *)kRegions[i].base, kRegions[i].size);
        off += kRegions[i].size;
    }
    PortDmaStateSave(dest + off);   off += PortDmaStateSize();
    PortFrameStateSave(dest + off); off += PortFrameStateSize();
    PortPpuStateSave(dest + off);
}

/* PORT_RB_BREAK=1 restores the GBA map but *not* the port's own state -- the
 * armed DMA channels, the VBlank budget, the affine reference points.
 *
 * This is a negative control, and it is here permanently because a self-test
 * that cannot fail proves nothing.  The snapshot's whole risk is that it
 * misses simulation state living outside the map, and this is what that looks
 * like: the restore still produces a plausible frame, and the re-simulation
 * then drifts.  If PORT_RB_BREAK=1 ever *passes*, either the test has stopped
 * testing or the port has stopped keeping state out here -- and the second one
 * would be worth knowing too.
 *
 * Two modes, because the first one turned out not to fail:
 *
 *   PORT_RB_BREAK=1  skip the DMA channels and the affine reference points.
 *                    This *passes*, and that is a measured fact about the port
 *                    rather than a broken test: the game re-arms its DMA every
 *                    frame (UpdateScreenDma, ClearOamBufferDma, and main.c's
 *                    HBlank channel) and the affine points are re-latched from
 *                    the IO registers, which are in the map.  So on this path
 *                    the port's out-of-map state does not reach the
 *                    simulation.  It is still saved, because "does not, here,
 *                    today" is not "cannot".
 *
 *   PORT_RB_BREAK=2  skip restoring IWRAM, which holds gTasks -- the entire
 *                    task list.  This must fail, and it is what demonstrates
 *                    the test can detect a bad snapshot at all.
 *
 * The frame state is restored under both, because sFrameCount lives there and
 * without it the restored run never arrives back at the frame the test waits
 * for -- the first attempt skipped all three and produced no output at all,
 * which broke the harness rather than demonstrating a divergence. */
static int BreakMode(void)
{
    static int mode = -1;

    if (mode < 0) {
        const char *e = getenv("PORT_RB_BREAK");
        mode = (e != NULL && *e != '\0') ? atoi(e) : 0;
        if (mode)
            PortLog("[katam-port] rollback: PORT_RB_BREAK=%d -- damaging the "
                    "restore on purpose", mode);
    }
    return mode;
}

static int BreakHostState(void) { return BreakMode() == 1; }
static int BreakIwram(void)     { return BreakMode() == 2; }

static void LoadFrom(const u8 *src)
{
    u32 i, off = 0;

    for (i = 0; i < NUM_REGIONS; i++) {
        if (!(BreakIwram() && kRegions[i].base == GBA_IWRAM_BASE))
            memcpy((void *)kRegions[i].base, src + off, kRegions[i].size);
        off += kRegions[i].size;
    }
    /* The frame state is restored either way, even under PORT_RB_BREAK.  It
     * holds sFrameCount, and without it a restored run never arrives back at
     * the frame the test is waiting for -- the control would break the
     * *harness* rather than demonstrate anything.  That it does so is itself
     * worth knowing: the frame counter is not optional. */
    if (!BreakHostState())
        PortDmaStateLoad(src + off);
    off += PortDmaStateSize();
    PortFrameStateLoad(src + off); off += PortFrameStateSize();
    if (!BreakHostState())
        PortPpuStateLoad(src + off);
}

/* --- the input timeline ---------------------------------------------------
 *
 * Stored uncompressed while a session runs, because a rollback has to index it
 * by frame; run-length encoded only on the way out (PortRbEncodeLog).  A frame
 * costs eight bytes for four players, so an hour is 1.7 MiB -- affordable, and
 * the alternative is decoding runs on every random access.
 *
 * `known` is the bit that makes it a rollback engine rather than a recorder: an
 * entry that is not known is a *prediction*, and a confirmation that disagrees
 * with a prediction already simulated is what triggers a rollback. */
#define TIMELINE_CHUNK 4096

struct Timeline {
    u16 *keys;     /* [frame * PORT_RB_PLAYERS + player]                     */
    u8  *known;    /* same indexing; 1 once the real input has arrived       */
    u32 cap;       /* frames allocated                                        */
    u32 len;       /* frames written                                          */
};

static struct Timeline sTl;

static int TimelineReserve(u32 frame)
{
    u32 want;
    u16 *k;
    u8 *n;

    if (frame < sTl.cap)
        return 1;

    want = ((frame / TIMELINE_CHUNK) + 1) * TIMELINE_CHUNK;
    k = (u16 *)realloc(sTl.keys, (size_t)want * PORT_RB_PLAYERS * sizeof *k);
    if (k == NULL)
        return 0;
    sTl.keys = k;
    n = (u8 *)realloc(sTl.known, (size_t)want * PORT_RB_PLAYERS * sizeof *n);
    if (n == NULL)
        return 0;
    sTl.known = n;

    memset(sTl.keys + (size_t)sTl.cap * PORT_RB_PLAYERS, 0,
           (size_t)(want - sTl.cap) * PORT_RB_PLAYERS * sizeof *sTl.keys);
    memset(sTl.known + (size_t)sTl.cap * PORT_RB_PLAYERS, 0,
           (size_t)(want - sTl.cap) * PORT_RB_PLAYERS * sizeof *sTl.known);
    sTl.cap = want;
    return 1;
}

/* The prediction, and it is the simplest one that works: assume a player keeps
 * holding what they last held.  Measured on the ROM's own recorded play, the
 * mean run of identical input is 22.8 frames, so "the same again" is right
 * about 96% of the time at 60 Hz.  A cleverer predictor would buy very little
 * and would have to be identical on every client, since a misprediction is
 * only cheap if everyone makes the same one. */
static u16 PredictFor(int player, u32 frame)
{
    u32 f = frame;

    while (f-- > 0) {
        u32 i = f * PORT_RB_PLAYERS + player;

        if (f < sTl.len && sTl.known[i])
            return sTl.keys[i];
    }
    return 0;
}

/* --- the event timeline ---------------------------------------------------
 *
 * Join and leave.  Kept separate from the inputs because it is sparse, and
 * applied at the top of a frame before the game runs, so that a replay and a
 * live session apply it at exactly the same point.
 *
 * The player count is the game's own gUnk_0203AD30, which decides which Kirbys
 * are AI-driven *and* scales enemy difficulty, so it is simulation state and
 * every client has to change it on the same frame.  See docs/MULTIPLAYER.md §9
 * for why only the highest-numbered player can leave cleanly. */
#define MAX_EVENTS 256

struct Event {
    u32 frame;
    u8  type;
    u8  a;
    u8  b;
};

static struct Event sEvents[MAX_EVENTS];
static int sNumEvents;

/* The game's own storage, by address.  This file is platform code and must not
 * depend on the generated tree, so the addresses are written out and the
 * offsets come from platform/port/gba_layout.h, which asserts every one of
 * them against the console layout on every build.
 *
 *   gUnk_0203AD30                  the number of Kirbys the game treats as
 *                                  network-driven -- the threshold in
 *                                  src/kirby.c:6475.
 *   gUnk_020382D0.unk8[3][4]       per-player input at offset 8: [0] held,
 *                                  [1] pressed this frame, [2] released.
 *                                  Written by the link layer at
 *                                  src/multi_08030C94.c:332 and read by Kirby.
 *   gUnk_02038590[i].unk9E         the AI controller's synthesised buttons,
 *                                  written by src/code_0800ECAC_2.c.  Stride
 *                                  244, field at 158; both asserted.
 */
#define GAME_PLAYER_COUNT (*(vu8 *)0x0203AD30)
#define GAME_HELD(slot)     (*(vu16 *)(0x020382D0 + 8      + 2 * (slot)))
#define GAME_PRESSED(slot)  (*(vu16 *)(0x020382D0 + 8 +  8 + 2 * (slot)))
#define GAME_RELEASED(slot) (*(vu16 *)(0x020382D0 + 8 + 16 + 2 * (slot)))
#define GAME_AI_INPUT(slot) (*(vu8  *)(0x02038590 + 244 * (slot) + 158))

/* slot -> peer, or -1 for "the AI is driving this Kirby".  Timeline state:
 * changed only by an event, so a replay reproduces it. */
static s8 sSlotPeer[PORT_RB_PLAYERS];
static u8 sSlotsInPlay = PORT_RB_PLAYERS;

/* --- session -------------------------------------------------------------- */

static int sActive;
static int sDepth;
static int sPlayers = 2;
static u32 sSnapBytes;
static u8 *sRing;              /* sDepth snapshots, back to back              */
static u32 sRingFrame[PORT_RB_MAX_FRAMES];
static u8  sRingValid[PORT_RB_MAX_FRAMES];
static u32 sFrame;             /* the frame about to run                      */
static u32 sConfirmed;         /* every player's input known below this       */
static int sCatchUp;
static u32 sCatchUpTo;
static struct PortRbStats sStats;

static u8 *SlotFor(u32 frame)
{
    return sRing + (size_t)(frame % (u32)sDepth) * sSnapBytes;
}

int PortRbActive(void) { return sActive; }
int PortRbCatchingUp(void) { return sCatchUp; }

int PortRbInit(int depth, int players)
{
    PortRbShutdown();

    if (depth < 1) depth = 1;
    if (depth > PORT_RB_MAX_FRAMES) depth = PORT_RB_MAX_FRAMES;
    if (players < 1) players = 1;
    if (players > PORT_RB_PLAYERS) players = PORT_RB_PLAYERS;

    sSnapBytes = MapBytes() + HostBytes();
    sRing = (u8 *)malloc((size_t)sSnapBytes * depth);
    if (sRing == NULL) {
        PortError("[katam-port] rollback: no memory for %d snapshots of %u bytes",
                  depth, (unsigned)sSnapBytes);
        return 0;
    }

    memset(sRingValid, 0, sizeof sRingValid);
    memset(&sStats, 0, sizeof sStats);
    sDepth = depth;
    sPlayers = players;
    sFrame = PortFrameNumber();
    sConfirmed = sFrame;
    sCatchUp = 0;
    sNumEvents = 0;
    sActive = 1;

    /* Peers take slots one for one to begin with, which is what the game's own
     * lobby produces; anything else is an event away. */
    {
        int i;

        for (i = 0; i < PORT_RB_PLAYERS; i++)
            sSlotPeer[i] = (i < players) ? (s8)i : (s8)-1;
        sSlotsInPlay = (u8)PORT_RB_PLAYERS;
    }

    if (!TimelineReserve(sFrame + TIMELINE_CHUNK)) {
        PortRbShutdown();
        return 0;
    }
    sTl.len = sFrame;

    PortLog("[katam-port] rollback: %d players, %d frames deep, "
            "%u KiB a snapshot, %u KiB of ring",
            players, depth, (unsigned)(sSnapBytes / 1024),
            (unsigned)((sSnapBytes * (u32)depth) / 1024));
    return 1;
}

void PortRbShutdown(void)
{
    free(sRing);   sRing = NULL;
    free(sTl.keys);  sTl.keys = NULL;
    free(sTl.known); sTl.known = NULL;
    sTl.cap = sTl.len = 0;
    sActive = 0;
    sCatchUp = 0;
    sNumEvents = 0;
}

/* --- inputs --------------------------------------------------------------- */

static int SelfId(void)
{
    struct PortMpLink link;

    if (PortMpLinkState(&link))
        return link.selfId;
    return 0;
}

void PortRbSetLocalInput(u16 keys)
{
    u32 i;

    if (!sActive || !TimelineReserve(sFrame))
        return;
    i = sFrame * PORT_RB_PLAYERS + (u32)SelfId();
    sTl.keys[i] = keys & PORT_RB_KEYS_MASK;
    sTl.known[i] = 1;
    if (sFrame >= sTl.len)
        sTl.len = sFrame + 1;
}

void PortRbConfirmInput(int player, u32 frame, u16 keys)
{
    u32 i;

    if (!sActive || player < 0 || player >= PORT_RB_PLAYERS)
        return;

    keys &= PORT_RB_KEYS_MASK;

    /* Older than the ring: the state it would correct is gone.  Nothing here
     * can fix that, so it is counted and dropped -- a caller watching
     * lateDrops climb is watching its rollback window be too short. */
    if (sFrame >= (u32)sDepth && frame + (u32)sDepth <= sFrame) {
        sStats.lateDrops++;
        return;
    }
    if (!TimelineReserve(frame))
        return;

    i = frame * PORT_RB_PLAYERS + (u32)player;
    if (sTl.known[i]) {
        if (sTl.keys[i] != keys)
            PortError("[katam-port] rollback: two different confirmations for "
                      "player %d frame %u (0x%03X then 0x%03X)",
                      player, (unsigned)frame, sTl.keys[i], keys);
        return;
    }

    /* Already simulated on a prediction?  If the prediction was right there is
     * nothing to do; if it was wrong, everything from `frame` on is void. */
    if (frame < sFrame) {
        u16 predicted = sTl.keys[i];

        sTl.keys[i] = keys;
        sTl.known[i] = 1;
        if (predicted != keys) {
            sStats.mispredictions++;
            if (!sRingValid[frame % (u32)sDepth]
             || sRingFrame[frame % (u32)sDepth] != frame) {
                sStats.lateDrops++;   /* no snapshot to go back to */
                return;
            }
            /* Ask for a rollback; PortRbFrame does it, because doing it here
             * would restore the map underneath whatever is on the stack. */
            if (!sCatchUp || frame < sCatchUpTo) {
                sCatchUpTo = frame;
                sCatchUp = 2;         /* 2 = rollback requested, not yet done */
            }
        }
        return;
    }

    sTl.keys[i] = keys;
    sTl.known[i] = 1;
    if (frame >= sTl.len)
        sTl.len = frame + 1;
}

u16 PortRbInputAt(int player, u32 frame)
{
    u32 i;

    if (!sActive || player < 0 || player >= PORT_RB_PLAYERS)
        return 0;
    if (!TimelineReserve(frame))
        return 0;
    i = frame * PORT_RB_PLAYERS + (u32)player;
    return sTl.known[i] ? sTl.keys[i] : PredictFor(player, frame);
}

/* Every player's input known, as far as it goes. */
static void AdvanceConfirmed(void)
{
    while (sConfirmed < sTl.len) {
        int p;

        for (p = 0; p < sPlayers; p++)
            if (!sTl.known[sConfirmed * PORT_RB_PLAYERS + (u32)p])
                return;
        sConfirmed++;
    }
}

/* --- events --------------------------------------------------------------- */

u32 PortRbSuggestEventFrame(void)
{
    /* Beyond the rollback window, so the event is confirmed before it is
     * applied and can never be rolled back.  A little past it, because the
     * caller still has to get it to everyone else. */
    return sFrame + (u32)sDepth + 8;
}

int PortRbScheduleEvent(u32 frame, enum PortRbEventType type, u8 a, u8 b)
{
    int i;

    if (!sActive || sNumEvents >= MAX_EVENTS)
        return 0;
    if (type == PORT_RB_EV_PLAYERS && (a < 1 || a > PORT_RB_PLAYERS))
        return 0;
    if (type == PORT_RB_EV_SLOT) {
        if (a >= PORT_RB_PLAYERS)
            return 0;
        if (b != PORT_RB_SLOT_AI && b >= PORT_RB_PLAYERS)
            return 0;
    }

    /* Keep them ordered by frame; there are never many. */
    for (i = sNumEvents; i > 0 && sEvents[i - 1].frame > frame; i--)
        sEvents[i] = sEvents[i - 1];
    sEvents[i].frame = frame;
    sEvents[i].type = (u8)type;
    sEvents[i].a = a;
    sEvents[i].b = b;
    sNumEvents++;

    if (type == PORT_RB_EV_SLOT)
        PortLog("[katam-port] rollback: frame %u -- slot %u %s%u",
                (unsigned)frame, a,
                b == PORT_RB_SLOT_AI ? "-> AI" : "-> peer ",
                b == PORT_RB_SLOT_AI ? 0 : b);
    else
        PortLog("[katam-port] rollback: frame %u -- Kirbys in play -> %u",
                (unsigned)frame, a);
    return 1;
}

int PortRbAssignSlot(u32 frame, int slot, int peer)
{
    if (slot < 0 || slot >= PORT_RB_PLAYERS)
        return 0;
    return PortRbScheduleEvent(frame, PORT_RB_EV_SLOT, (u8)slot,
                               peer < 0 ? PORT_RB_SLOT_AI : (u8)peer);
}

int PortRbSlotPeer(int slot)
{
    if (slot < 0 || slot >= PORT_RB_PLAYERS)
        return -1;
    return sSlotPeer[slot];
}

int PortRbPeerSlot(int peer)
{
    int i;

    for (i = 0; i < PORT_RB_PLAYERS; i++)
        if (sSlotPeer[i] == (s8)peer)
            return i;
    return -1;
}

/* Applied at the top of every frame, live and replayed alike, so the two
 * cannot disagree about when it happened. */
static void ApplyEventsFor(u32 frame)
{
    int i;

    for (i = 0; i < sNumEvents; i++) {
        if (sEvents[i].frame != frame)
            continue;
        switch (sEvents[i].type) {
        case PORT_RB_EV_PLAYERS:
            sSlotsInPlay = sEvents[i].a;
            break;
        case PORT_RB_EV_SLOT:
            /* A peer can only hold one slot.  Taking a new one vacates the old
             * one to the AI, so a "move player 2 to slot 0" is one event and
             * cannot leave them driving two Kirbys. */
            if (sEvents[i].b != PORT_RB_SLOT_AI) {
                int old = PortRbPeerSlot(sEvents[i].b);

                if (old >= 0 && old != sEvents[i].a)
                    sSlotPeer[old] = -1;
                sSlotPeer[sEvents[i].a] = (s8)sEvents[i].b;
            } else {
                sSlotPeer[sEvents[i].a] = -1;
            }
            break;
        default:
            break;
        }
    }
}

/* Every slot's buttons, written where the game reads them.
 *
 * This is the whole of "arbitrary joining and leaving" at run time.  The game
 * asks gUnk_020382D0.unk8[0][slot] what Kirby `slot` is holding; the link
 * layer would normally fill that from the received packets, and here the
 * engine fills it from the timeline instead -- for an occupied slot -- or from
 * the AI controller's own output for a vacant one.  So a Kirby whose player
 * left keeps playing, driven by the same AI that drives the CPU buddies in
 * single-player, and nothing about the Kirby moved.
 *
 * unk8[1] and unk8[2] are the pressed and released edges.  The game computes
 * them the same way at src/multi_08030C94.c:333, and menus and abilities
 * edge-trigger off them, so writing only the held word would give a Kirby that
 * can hold a direction but never press a button.
 *
 * gUnk_0203AD30 is held at the number of Kirbys in play so that every
 * participating slot takes the network branch of the test in src/kirby.c:6475.
 * The threshold stays a threshold -- the human-or-AI choice moves up here,
 * where it can be made per slot. */
static void ApplySlotInputs(u32 frame)
{
    int slot;

    GAME_PLAYER_COUNT = sSlotsInPlay;

    for (slot = 0; slot < PORT_RB_PLAYERS; slot++) {
        int peer = sSlotPeer[slot];
        u16 prev = GAME_HELD(slot);
        u16 now;

        if (slot >= (int)sSlotsInPlay)
            continue;              /* not in play; the game ignores it */

        if (peer >= 0)
            now = sTl.keys[frame * PORT_RB_PLAYERS + (u32)peer]
                & PORT_RB_KEYS_MASK;
        else
            now = (u16)GAME_AI_INPUT(slot);

        GAME_HELD(slot) = now;
        GAME_PRESSED(slot) = (u16)(now & ~prev);
        GAME_RELEASED(slot) = (u16)(prev & ~now);
    }
}

/* --- the frame hook ------------------------------------------------------- */

void PortRbFrame(void)
{
    if (!sActive)
        return;

    /* A rollback was asked for while the last frame was running.  Now is when
     * it is safe: the stack is main -> AgbMain -> GameLoop -> VBlankIntrWait ->
     * PortPresentFrame and holds nothing the game will read again. */
    if (sCatchUp == 2) {
        u32 to = sCatchUpTo;
        u32 slot = to % (u32)sDepth;
        u32 depthNow = sFrame - to;

        LoadFrom(SlotFor(to));
        sFrame = sRingFrame[slot];
        sStats.rollbacks++;
        sStats.resimFrames += depthNow;
        if (depthNow > sStats.deepest)
            sStats.deepest = depthNow;
        sCatchUp = 1;             /* now re-simulating */
        PortSetRenderEnabled(0);
    }

    /* Caught up?  Give the picture back. */
    if (sCatchUp == 1 && sFrame >= sCatchUpTo && sFrame >= sTl.len - 1) {
        sCatchUp = 0;
        PortSetRenderEnabled(1);
    }

    if (!TimelineReserve(sFrame))
        return;

    ApplyEventsFor(sFrame);

    /* Drive the game from the timeline, never from the host.  A predicted
     * input is written back so that a later confirmation can be compared
     * against what was actually simulated -- without that, a rollback could
     * not tell a right prediction from a wrong one. */
    {
        int self = SelfId();
        int p;

        for (p = 0; p < sPlayers; p++) {
            u32 i = sFrame * PORT_RB_PLAYERS + (u32)p;

            if (!sTl.known[i]) {
                sTl.keys[i] = PredictFor(p, sFrame);
                if (p != self)
                    sStats.predictions++;
            }
        }
        if (sFrame >= sTl.len)
            sTl.len = sFrame + 1;

        PortRbApplyKeys(sTl.keys[sFrame * PORT_RB_PLAYERS + (u32)self]);
    }

    ApplySlotInputs(sFrame);

    /* Snapshot *before* the frame runs, so restoring lands here again. */
    {
        u32 slot = sFrame % (u32)sDepth;

        SaveTo(SlotFor(sFrame));
        sRingFrame[slot] = sFrame;
        sRingValid[slot] = 1;
    }

    AdvanceConfirmed();
    sStats.frame = sFrame;
    sStats.confirmed = sConfirmed;
    sFrame++;
}

/* --- why there is no snapshot on the wire ---------------------------------
 *
 * There was, briefly, and it worked.  A 387 KiB snapshot with a fingerprint of
 * the image layout, refusing to load into any build that was not byte-for-byte
 * the same program -- because struct Task::main, Object2::unk78, gIntrTable
 * and gMPlayJumpTable all hold values that mean something only to the image
 * that made them.
 *
 * It was measured, and the measurement is worth keeping even though the
 * feature is not.  Two processes of the same binary, dumped at the same frame
 * and compared: identical at six of seven sampled frames, and different at the
 * seventh -- at IO offset 0xD5, which is DMA3SAD.  That is platform/dma.c
 * writing the transfer's real source into the register mirror, and DmaFill's
 * source is `&tmp`, a stack address that ASLR moves every run.  Normalising
 * the mirrors and the disarmed channels made it identical at eight of eight.
 *
 * So it is achievable.  It was dropped anyway, because the property it has is
 * bad: a snapshot is only valid between identical builds, so every redeploy
 * invalidates every session in flight, and the failure mode of getting that
 * wrong is a game that calls whatever now lives at an address it remembered.
 * The input log crosses any two builds because it is just numbers, and
 * replaying ten minutes of it costs 0.19 s in a browser.  One mechanism that
 * always works beats two where the faster one needs a version check.
 *
 * The ring above still snapshots memory, and must -- that is same-process by
 * construction and none of this applies to it. */

/* --- the run-length encoded log -------------------------------------------
 *
 * The game's own format, from sub_080204EC in src/code_08020220.c: a u16 per
 * run, 10 bits of button state and a 6-bit repeat count.  Measured against the
 * twenty-four recorded four-player tapes in the ROM's attract-mode demos it is
 * 17.1x smaller than the game's 12-bits-a-frame wire encoding, with a mean run
 * of 22.8 frames.
 *
 * Runs longer than 64 frames are split rather than escaped, which is what the
 * game does and costs almost nothing: at the measured mean the cap is reached
 * only by a player who is standing still.
 *
 * Layout: a small header, then each player's runs in turn.  Little-endian
 * throughout, because every host this runs on is. */
#define LOG_MAGIC   0x424C524Bu   /* 'KRLB' */
#define RUN_MAX     64

struct LogHeader {
    u32 magic;
    u32 frames;
    u16 players;
    u16 events;
    u32 runs[PORT_RB_PLAYERS];
};

static void Put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16  Get16(const u8 *p)  { return (u16)(p[0] | (p[1] << 8)); }
static void Put32(u8 *p, u32 v) { Put16(p, (u16)v); Put16(p + 2, (u16)(v >> 16)); }
static u32  Get32(const u8 *p)  { return Get16(p) | ((u32)Get16(p + 2) << 16); }

static u32 CountRuns(int player, u32 frames)
{
    u32 f = 0, runs = 0;

    while (f < frames) {
        u16 v = sTl.keys[f * PORT_RB_PLAYERS + (u32)player];
        u32 n = 0;

        while (f + n < frames && n < RUN_MAX
            && sTl.keys[(f + n) * PORT_RB_PLAYERS + (u32)player] == v)
            n++;
        f += n;
        runs++;
    }
    return runs;
}

u32 PortRbEncodeLog(u8 *dest, u32 cap)
{
    struct LogHeader h;
    u32 need, off, frames;
    int p;

    if (!sActive)
        return 0;
    frames = sTl.len;

    h.magic = LOG_MAGIC;
    h.frames = frames;
    h.players = (u16)sPlayers;
    h.events = (u16)sNumEvents;
    need = 4 + 4 + 2 + 2 + 4 * PORT_RB_PLAYERS;
    for (p = 0; p < PORT_RB_PLAYERS; p++) {
        h.runs[p] = (p < sPlayers) ? CountRuns(p, frames) : 0;
        need += h.runs[p] * 2;
    }
    need += (u32)sNumEvents * 7;

    if (dest == NULL || cap < need)
        return need;

    off = 0;
    Put32(dest + off, h.magic);  off += 4;
    Put32(dest + off, h.frames); off += 4;
    Put16(dest + off, h.players); off += 2;
    Put16(dest + off, h.events);  off += 2;
    for (p = 0; p < PORT_RB_PLAYERS; p++) { Put32(dest + off, h.runs[p]); off += 4; }

    for (p = 0; p < sPlayers; p++) {
        u32 f = 0;

        while (f < frames) {
            u16 v = sTl.keys[f * PORT_RB_PLAYERS + (u32)p] & PORT_RB_KEYS_MASK;
            u32 n = 0;

            while (f + n < frames && n < RUN_MAX
                && (sTl.keys[(f + n) * PORT_RB_PLAYERS + (u32)p]
                    & PORT_RB_KEYS_MASK) == v)
                n++;
            Put16(dest + off, (u16)(v | ((n - 1) << 10)));
            off += 2;
            f += n;
        }
    }

    for (p = 0; p < sNumEvents; p++) {
        Put32(dest + off, sEvents[p].frame); off += 4;
        dest[off++] = sEvents[p].type;
        dest[off++] = sEvents[p].a;
        dest[off++] = sEvents[p].b;
    }
    return off;
}

long PortRbDecodeLog(const u8 *src, u32 len)
{
    u32 off = 0, frames, runs[PORT_RB_PLAYERS];
    int p, players, events, i;

    if (!sActive || src == NULL || len < 16 + 4 * PORT_RB_PLAYERS)
        return -1;
    if (Get32(src) != LOG_MAGIC)
        return -1;
    off = 4;
    frames = Get32(src + off); off += 4;
    players = (int)Get16(src + off); off += 2;
    events = (int)Get16(src + off); off += 2;
    if (players < 1 || players > PORT_RB_PLAYERS)
        return -1;
    for (p = 0; p < PORT_RB_PLAYERS; p++) { runs[p] = Get32(src + off); off += 4; }

    if (!TimelineReserve(frames ? frames - 1 : 0))
        return -1;
    memset(sTl.keys, 0, (size_t)sTl.cap * PORT_RB_PLAYERS * sizeof *sTl.keys);
    memset(sTl.known, 0, (size_t)sTl.cap * PORT_RB_PLAYERS * sizeof *sTl.known);

    for (p = 0; p < players; p++) {
        u32 f = 0, r;

        for (r = 0; r < runs[p]; r++) {
            u16 w, v;
            u32 n;

            if (off + 2 > len)
                return -1;
            w = Get16(src + off); off += 2;
            v = w & PORT_RB_KEYS_MASK;
            n = ((w >> 10) & 0x3F) + 1;
            while (n-- > 0 && f < frames) {
                sTl.keys[f * PORT_RB_PLAYERS + (u32)p] = v;
                sTl.known[f * PORT_RB_PLAYERS + (u32)p] = 1;
                f++;
            }
        }
        if (f != frames)
            return -1;      /* the runs do not cover the frames they claim */
    }

    sNumEvents = 0;
    for (i = 0; i < events && sNumEvents < MAX_EVENTS; i++) {
        if (off + 7 > len)
            return -1;
        sEvents[sNumEvents].frame = Get32(src + off); off += 4;
        sEvents[sNumEvents].type = src[off++];
        sEvents[sNumEvents].a = src[off++];
        sEvents[sNumEvents].b = src[off++];
        sNumEvents++;
    }

    sTl.len = frames;
    sPlayers = players;
    sConfirmed = frames;
    return (long)frames;
}

/* --- replay ---------------------------------------------------------------
 *
 * Not implemented as a loop here, and that is the point: replay is the game's
 * own loop running with the picture off and the inputs coming from the
 * timeline, which is what PortRbFrame already does.  The caller sets the
 * target and returns; the frames happen because the game keeps running.
 *
 * The host has to cooperate by not pacing -- PortRbCatchingUp() is what it
 * asks.  Without that the replay is correct and takes real time, which for
 * ten minutes of history is ten minutes. */
int PortRbReplayTo(u32 toFrame)
{
    if (!sActive || toFrame < sFrame)
        return 0;
    sCatchUpTo = toFrame;
    sCatchUp = 1;
    PortSetRenderEnabled(0);
    return 1;
}

/* --- joining --------------------------------------------------------------
 *
 * A vacant slot is one the map has no peer for.  It is not the same as a slot
 * that is out of play: a vacant slot still has a Kirby in the world, driven by
 * the AI, and joining means taking the controls off the AI rather than
 * spawning anything.  That is why a join needs no new Kirby and no state
 * migration -- see the note on the peer-to-slot map in port/rollback.h. */
int PortRbDescribeSession(struct PortRbSessionInfo *out)
{
    int i;

    if (out == NULL)
        return 0;
    memset(out, 0, sizeof *out);
    if (!sActive)
        return 0;

    out->frame = sFrame;
    out->logBytes = PortRbEncodeLog(NULL, 0);
    out->slotsInPlay = sSlotsInPlay;
    for (i = 0; i < PORT_RB_PLAYERS; i++) {
        out->slotPeer[i] = sSlotPeer[i];
        if (i < (int)sSlotsInPlay && sSlotPeer[i] < 0)
            out->vacant++;
    }
    return 1;
}

int PortRbVacantSlot(void)
{
    int i;

    if (!sActive)
        return -1;
    for (i = 0; i < (int)sSlotsInPlay && i < PORT_RB_PLAYERS; i++)
        if (sSlotPeer[i] < 0)
            return i;
    return -1;
}

long PortRbJoin(const u8 *log, u32 len, int slot, int peer)
{
    long frames;

    if (!sActive) {
        PortError("[katam-port] join: no session -- call PortRbInit first");
        return -1;
    }
    if (slot < 0 || slot >= PORT_RB_PLAYERS || peer < 0
     || peer >= PORT_RB_PLAYERS) {
        PortError("[katam-port] join: slot %d peer %d is not a seat",
                  slot, peer);
        return -1;
    }

    frames = PortRbDecodeLog(log, len);
    if (frames < 0) {
        PortError("[katam-port] join: the log did not decode");
        return -1;
    }

    /* Past the rollback window, so every participant applies it at the same
     * frame and none of them has to roll back to find out about it. */
    if (!PortRbAssignSlot((u32)frames + (u32)sDepth + 8, slot, peer)) {
        PortError("[katam-port] join: could not schedule the slot");
        return -1;
    }

    PortLog("[katam-port] join: %ld frames of history, taking slot %d as peer "
            "%d at frame %u", frames, slot, peer,
            (unsigned)((u32)frames + (u32)sDepth + 8));

    if (!PortRbReplayTo((u32)frames))
        return -1;
    return frames;
}

/* --- diagnostics ---------------------------------------------------------- */

void PortRbGetStats(struct PortRbStats *out)
{
    if (out != NULL)
        *out = sStats;
}

void PortRbReport(void)
{
    if (!sActive) {
        PortLog("[katam-port] rollback: not active");
        return;
    }
    PortLog("[katam-port] rollback: frame %u, confirmed %u, %u rollbacks "
            "(%u frames re-simulated, deepest %u)",
            (unsigned)sStats.frame, (unsigned)sStats.confirmed,
            (unsigned)sStats.rollbacks, (unsigned)sStats.resimFrames,
            (unsigned)sStats.deepest);
    PortLog("[katam-port] rollback: %u predictions, %u wrong, %u inputs "
            "dropped as too late",
            (unsigned)sStats.predictions, (unsigned)sStats.mispredictions,
            (unsigned)sStats.lateDrops);
}

/* --- the self-test --------------------------------------------------------
 *
 * The claim under everything above is that the port's simulation is a pure
 * function of (state, input).  This tests it the only way that is worth
 * anything: from inside a running game, on real state, by doing the thing.
 *
 * At frame A: snapshot, hash.  Run to A+span, hash.  Restore A, verify the
 * state matches the frame-A hash, run to A+span again with the same inputs,
 * and verify the second arrival matches the first.
 *
 * The second half is the one that finds bugs.  A snapshot that omits something
 * -- an armed HBlank DMA, the VBlank budget -- still restores a *plausible*
 * frame A, and the two runs to A+span then diverge.  That is precisely the
 * failure mode this exists to catch, and it is why the test spans frames
 * rather than comparing one restore.
 *
 * It runs inside the frame hook because that is the only place the stack is
 * shallow and known.  FNV-1a, matching the state trace in platform/main.c, so
 * that a failure can be chased with the same instruments.
 */
static u32 Fnv(const void *base, u32 len)
{
    const u8 *p = (const u8 *)base;
    u32 h = 2166136261u, i;

    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

/* Only the regions two runs can be expected to agree on.  IO is excluded: it
 * holds the DMA registers, and platform/dma.c writes the *host* source address
 * into them, which for DmaFill is the address of a local -- so it differs
 * between two runs of the same binary.  docs/DEBUG-TOOLING.md §7 records this;
 * nothing reads the mirrors back. */
static u32 StateHash(void)
{
    u32 h = 0, i;

    for (i = 0; i < NUM_REGIONS; i++) {
        if (kRegions[i].base == GBA_IO_BASE)
            continue;
        h ^= Fnv((const void *)kRegions[i].base, kRegions[i].size);
        h *= 16777619u;
    }
    return h;
}

static int sTestPhase;          /* 0 off, 1 arming, 2 first pass, 3 second    */
static u32 sTestAt, sTestSpan;
static u32 sTestHashAt, sTestHashEnd;
static u8 *sTestSnap;
/* The input the first pass was given, so the second pass can be given the same.
 *
 * This is not a detail.  The native host counts frames with its own counter,
 * so --mash keeps advancing across a restore and would hand the second pass
 * *different* buttons -- and the test would then fail for a reason that has
 * nothing to do with the snapshot.  Recording the first pass and replaying it
 * is also exactly what the rollback engine does, so the test exercises the
 * same path. */
static u16 *sTestKeys;

/* The input latch -- see platform/main.c's UpdateKeyInput and the note on
 * sTestKeys above.  Two callers can be driving: the self-test replaying its
 * recording, and a live session replaying its timeline. */
int PortRbKeyOverride(u16 *keys)
{
    u32 now = PortFrameNumber();

    if (sTestPhase == 2 && sTestKeys != NULL
     && now >= sTestAt && now < sTestAt + sTestSpan) {
        sTestKeys[now - sTestAt] = PortCurrentKeys();
        return 0;                       /* recording, not driving */
    }
    if (sTestPhase == 3 && sTestKeys != NULL
     && now >= sTestAt && now < sTestAt + sTestSpan) {
        *keys = sTestKeys[now - sTestAt];
        return 1;
    }

    if (sActive && sTl.keys != NULL) {
        int self = SelfId();

        if (now < sTl.len) {
            *keys = sTl.keys[now * PORT_RB_PLAYERS + (u32)self];
            return 1;
        }
    }
    return 0;
}


int PortRbSelfTest(u32 atFrame, u32 span)
{
    if (span == 0)
        return 0;
    if (sTestSnap == NULL) {
        sTestSnap = (u8 *)malloc(MapBytes() + HostBytes());
        if (sTestSnap == NULL)
            return 0;
    }
    free(sTestKeys);
    sTestKeys = (u16 *)calloc(span, sizeof *sTestKeys);
    if (sTestKeys == NULL)
        return 0;
    sTestAt = atFrame;
    sTestSpan = span;
    sTestPhase = 1;
    PortLog("[katam-port] rollback self-test armed: snapshot at frame %u, "
            "span %u", (unsigned)atFrame, (unsigned)span);
    return 1;
}

/* The log and the event timeline, exercised on the input the self-test just
 * recorded -- real button data from a real run rather than a synthetic
 * pattern, which is the difference between testing the encoder and testing it
 * on the thing it will see.
 *
 * Round-trip: encode, decode, re-encode, and require the two blobs to be
 * identical.  That catches a decoder that is merely self-consistent, which a
 * hand-rolled bit format is very good at being.
 */
static void LogRoundTrip(const u16 *keys, u32 frames)
{
    u8 *a = NULL, *b = NULL;
    u32 na, nb;
    long back;
    int savedPlayers = sPlayers, ok = 0;
    u32 f, p;

    if (!PortRbInit(4, PORT_RB_PLAYERS))
        return;

    /* Four players out of one recorded stream: player 0 as recorded, and the
     * others offset so the runs do not all line up and flatter the encoder. */
    if (!TimelineReserve(frames))
        goto done;
    for (f = 0; f < frames; f++)
        for (p = 0; p < PORT_RB_PLAYERS; p++) {
            u32 i = f * PORT_RB_PLAYERS + p;

            sTl.keys[i] = keys[(f + p * 37) % frames] & PORT_RB_KEYS_MASK;
            sTl.known[i] = 1;
        }
    sTl.len = frames;

    /* An event on the timeline too, so the log carries one. */
    PortRbScheduleEvent(frames / 2, PORT_RB_EV_PLAYERS, 3, 0);
    PortRbAssignSlot(frames / 2 + 1, 1, -1);          /* player at slot 1 leaves */
    PortRbAssignSlot(frames / 2 + 2, 1, 2);           /* peer 2 takes slot 1     */

    na = PortRbEncodeLog(NULL, 0);
    a = (u8 *)malloc(na);
    if (a == NULL) goto done;
    if (PortRbEncodeLog(a, na) != na) goto done;

    back = PortRbDecodeLog(a, na);
    if (back != (long)frames) {
        PortError("[katam-port] log self-test: decode returned %ld, expected %u",
                  back, (unsigned)frames);
        goto done;
    }

    nb = PortRbEncodeLog(NULL, 0);
    b = (u8 *)malloc(nb);
    if (b == NULL) goto done;
    if (PortRbEncodeLog(b, nb) != nb) goto done;

    if (na != nb || memcmp(a, b, na) != 0) {
        PortError("[katam-port] LOG SELF-TEST FAILED: re-encoding after a "
                  "decode gave %u bytes, not %u", (unsigned)nb, (unsigned)na);
        goto done;
    }
    if (sNumEvents != 3 || sEvents[0].a != 3
     || sEvents[1].type != PORT_RB_EV_SLOT || sEvents[1].b != PORT_RB_SLOT_AI
     || sEvents[2].type != PORT_RB_EV_SLOT || sEvents[2].b != 2) {
        PortError("[katam-port] LOG SELF-TEST FAILED: the events did not "
                  "survive the round trip");
        goto done;
    }
    ok = 1;

done:
    if (ok) {
        u32 raw = frames * PORT_RB_PLAYERS * 12 / 8;

        PortLog("[katam-port] LOG SELF-TEST PASSED: %u frames x %d players, "
                "%u bytes encoded against %u raw -- %u.%02ux, event survived",
                (unsigned)frames, PORT_RB_PLAYERS, (unsigned)na, (unsigned)raw,
                (unsigned)(raw / (na ? na : 1)),
                (unsigned)((raw * 100 / (na ? na : 1)) % 100));
    }
    free(a);
    free(b);
    sPlayers = savedPlayers;
    PortRbShutdown();
}

/* --- demonstrating that per-slot injection reaches a Kirby ----------------
 *
 * Everything above rests on one claim about the game: that writing
 * gUnk_020382D0.unk8[0][slot] makes Kirby `slot` do what it says.  That is
 * read out of src/kirby.c:6475 rather than measured, and link play cannot be
 * reached to measure it properly -- MultiBoot is still missing (§5).
 *
 * So: demonstrate the mechanism directly.  PORT_RB_SLOTTEST=at:span:keys turns
 * on the game's link-input path at frame `at`, drives slot 0 from the timeline
 * with a constant `keys` for `span` frames, and prints the resulting state
 * hash.  Run it twice with different keys and compare: if the hashes differ,
 * the injected input demonstrably reached the game.  Run it twice with the
 * *same* keys and they must match, which is the determinism half.
 *
 * gUnk_0203AD10 bit 1 is what selects that path (src/code_080332BC.c sets it
 * for a link session).  Forcing it outside a real session is a probe and is
 * labelled as one -- it is not a way to play. */
static int sSlotTest;
static u32 sSlotTestAt, sSlotTestSpan;
static u16 sSlotTestKeys;

#define GAME_MODE_FLAGS (*(vu32 *)0x0203AD10)

int PortRbSlotTest(u32 at, u32 span, u16 keys)
{
    if (span == 0)
        return 0;
    sSlotTestAt = at;
    sSlotTestSpan = span;
    sSlotTestKeys = keys & PORT_RB_KEYS_MASK;
    sSlotTest = 1;
    PortLog("[katam-port] slot-injection probe armed: frame %u, span %u, "
            "slot 0 held at 0x%03X", (unsigned)at, (unsigned)span, keys);
    return 1;
}

static void SlotTestStep(void)
{
    u32 now = PortFrameNumber();
    u32 f;

    if (!sSlotTest)
        return;

    if (sSlotTest == 1 && now == sSlotTestAt) {
        if (!PortRbInit(4, PORT_RB_PLAYERS)) {
            sSlotTest = 0;
            return;
        }
        /* Slot 0 driven by peer 0; the rest handed to the AI, which is the
         * "everyone else left" case. */
        sSlotPeer[0] = 0;
        sSlotPeer[1] = sSlotPeer[2] = sSlotPeer[3] = -1;
        sSlotsInPlay = PORT_RB_PLAYERS;
        GAME_MODE_FLAGS |= 2;

        if (!TimelineReserve(now + sSlotTestSpan + 1)) {
            sSlotTest = 0;
            return;
        }
        for (f = now; f <= now + sSlotTestSpan; f++) {
            sTl.keys[f * PORT_RB_PLAYERS] = sSlotTestKeys;
            sTl.known[f * PORT_RB_PLAYERS] = 1;
        }
        sTl.len = now + sSlotTestSpan + 1;
        sFrame = now;
        sSlotTest = 2;
        return;
    }

    if (sSlotTest == 2 && now >= sSlotTestAt + sSlotTestSpan) {
        PortLog("[katam-port] SLOT PROBE: keys=0x%03X for %u frames -> "
                "state %08X",
                sSlotTestKeys, (unsigned)sSlotTestSpan, (unsigned)StateHash());
        sSlotTest = 0;
        PortRbShutdown();
    }
}

/* Called from PortPresentFrame after PortRbFrame.  Kept separate from the
 * session machinery so the test works with no session attached, which is how
 * it gets run against an ordinary single-player boot. */
void PortRbSelfTestStep(void)
{
    u32 now = PortFrameNumber();

    SlotTestStep();

    if (sTestPhase == 0)
        return;

    if (sTestPhase == 1 && now == sTestAt) {
        SaveTo(sTestSnap);
        sTestHashAt = StateHash();
        PortLog("[katam-port] self-test: snapshot at frame %u, state %08X",
                (unsigned)now, (unsigned)sTestHashAt);
        sTestPhase = 2;
        return;
    }

    if (sTestPhase == 2 && now == sTestAt + sTestSpan) {
        sTestHashEnd = StateHash();
        PortLog("[katam-port] self-test: ran to frame %u, state %08X",
                (unsigned)now, (unsigned)sTestHashEnd);
        LoadFrom(sTestSnap);
        {
            u32 back = StateHash();

            if (back != sTestHashAt) {
                PortError("[katam-port] SELF-TEST FAILED: restore did not "
                          "reproduce frame %u (%08X, expected %08X)",
                          (unsigned)sTestAt, (unsigned)back,
                          (unsigned)sTestHashAt);
                sTestPhase = 0;
                return;
            }
        }
        PortLog("[katam-port] self-test: restored frame %u exactly",
                (unsigned)sTestAt);
        sTestPhase = 3;
        PortSetRenderEnabled(0);
        return;
    }

    if (sTestPhase == 3 && now == sTestAt + sTestSpan) {
        u32 again = StateHash();

        PortSetRenderEnabled(1);
        if (again == sTestHashEnd) {
            PortLog("[katam-port] SELF-TEST PASSED: %u frames re-simulated "
                    "from a restored snapshot, state %08X both times",
                    (unsigned)sTestSpan, (unsigned)again);
            LogRoundTrip(sTestKeys, sTestSpan);
        }
        else
            PortError("[katam-port] SELF-TEST FAILED: re-simulating %u frames "
                      "gave %08X, first run gave %08X -- something the "
                      "snapshot does not cover feeds the simulation",
                      (unsigned)sTestSpan, (unsigned)again,
                      (unsigned)sTestHashEnd);
        sTestPhase = 0;
    }
}

#ifdef __cplusplus
}
#endif
