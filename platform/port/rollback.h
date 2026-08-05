#ifndef GUARD_PORT_ROLLBACK_H
#define GUARD_PORT_ROLLBACK_H
/* C linkage for the 64-bit builds -- see platform/port/backend.h. */
#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"
#include "port/mp.h"

/* ---------------------------------------------------------------------------
 * Deterministic rollback, with no network in it.
 *
 * The game's own link play is lockstep with five frames of input delay and a
 * hard stall when a peer is late (docs/MULTIPLAYER.md §1).  On a cable a stall
 * is invisible; over the internet it is a freeze.  Rollback is the standard
 * answer: predict what the absent players did, keep simulating, and when the
 * truth arrives and differs, restore the last agreed state and re-run.
 *
 * Nothing here knows what a socket is.  A transport calls PortRbConfirmInput
 * when a peer's real input arrives and PortRbSetLocalInput once a frame; this
 * file decides when to roll back and re-simulates.  That is deliberate -- the
 * same machinery has to serve a loopback test, a WebRTC datachannel and a
 * replay from a file, and only the first of those exists today.
 *
 * Why this port can afford it (all measured, docs/MULTIPLAYER.md §8):
 *
 *   a snapshot is 387 KiB and six memcpys, because the whole GBA map lives at
 *   fixed addresses -- 5.0 us to save, 4.7 us to restore;
 *
 *   a re-simulated frame costs 2.5 us natively and 6.2 us in wasm, because
 *   drawing is ~99% of a frame and a re-simulated frame is never drawn;
 *
 *   so an eight-frame rollback is ~25 us against a 16.7 ms budget.
 *
 * And why it is *correct* to try: the port is bit-deterministic given identical
 * input across two ABIs, three architectures and two hosts, measured against
 * exactly the quantity the game's own desync detector hashes.  That was the
 * precondition and it is met (docs/MULTIPLAYER.md §6).
 *
 * --- where this hooks in ---------------------------------------------------
 *
 * VBlankIntrWait() is PortPresentFrame(), called from inside GameLoop, and
 * GameLoop keeps nothing in locals across it -- the C stack is
 * main -> AgbMain -> GameLoop -> VBlankIntrWait -> PortPresentFrame on every
 * single frame, identical in shape.  So restoring the GBA map at the top of
 * PortPresentFrame and returning normally puts the game back at that frame
 * with no stack surgery at all: the loop simply runs the frames again.
 *
 * That is the whole reason this is ~700 lines rather than a rewrite of the
 * frame loop, and it is a property of the port's architecture rather than
 * anything clever here.  If GameLoop ever starts carrying state in a local
 * across VBlankIntrWait, PortRbSelfTest below stops passing.
 * ------------------------------------------------------------------------- */

#define PORT_RB_PLAYERS      PORT_MP_PLAYERS
#define PORT_RB_MAX_FRAMES   32     /* deepest rollback; ring size            */
#define PORT_RB_KEYS_MASK    0x03FFu

/* --- lifecycle ----------------------------------------------------------- */

/* `depth` frames of history, 1..PORT_RB_MAX_FRAMES.  Snapshots are allocated
 * up front -- 387 KiB each, so depth 8 is 3.1 MiB.  Returns non-zero on
 * success.  Safe to call twice; the second call resets. */
int  PortRbInit(int depth, int players);
void PortRbShutdown(void);
int  PortRbActive(void);

/* --- input ---------------------------------------------------------------
 *
 * The local player's input for the frame about to run.  Called by the host
 * before the frame; what actually reaches the game is the *timeline*, so this
 * is recorded rather than applied directly. */
void PortRbSetLocalInput(u16 keys);

/* A peer's real input for a frame, from wherever it came from.  May arrive
 * late, out of order, or for a frame already simulated with a prediction --
 * that last case is what causes a rollback.  Ignored for a frame older than
 * the ring, which is a desync the caller has to handle at a higher level;
 * PortRbStats reports how often it happened. */
void PortRbConfirmInput(int player, u32 frame, u16 keys);

/* What this player is holding on `frame`, prediction included.  For a
 * transport that wants to send more than one frame per packet, as the game's
 * own protocol does (eight frames per packet). */
u16  PortRbInputAt(int player, u32 frame);

/* --- the timeline --------------------------------------------------------
 *
 * Join and leave are *events on the input timeline*, not commands.  The
 * distinction is the whole of why this is safe: rewriting history desyncs,
 * appending to it does not.  "player 2 becomes AI at frame F" is applied at F
 * by every client and by every replayer, so a replay from frame 0 reproduces
 * the change at the same point.
 *
 * Schedule F beyond the rollback window -- PortRbSuggestEventFrame() returns a
 * frame that is -- so the event is confirmed before it is applied and can
 * never be mispredicted. */
enum PortRbEventType {
    PORT_RB_EV_NONE = 0,
    PORT_RB_EV_PLAYERS,     /* a: the new *human* player count, 1..4        */
};

int  PortRbScheduleEvent(u32 frame, enum PortRbEventType type, u8 a);
u32  PortRbSuggestEventFrame(void);

/* --- the frame hook ------------------------------------------------------
 *
 * The one call platform/main.c makes, at the top of PortPresentFrame and
 * before anything else in the frame.  Does nothing at all when no session is
 * active, which is every build that has not called PortRbInit. */
void PortRbFrame(void);

/* Non-zero while re-simulating.  The host uses it to suppress everything a
 * frame produces that is not state: drawing, audio, and pacing.  Rendering is
 * handled here (PortSetRenderEnabled); audio and pacing are the host's. */
int  PortRbCatchingUp(void);

/* --- the log -------------------------------------------------------------
 *
 * The whole session's input, run-length encoded, for a joining or reconnecting
 * player to replay.  This is the resync mechanism, and it is inputs rather
 * than state on purpose: a snapshot contains host function pointers -- a wasm
 * table index in one build and a code address in another -- so it cannot cross
 * between builds, while an input log is just numbers.
 *
 * The encoding is the game's own (sub_080204EC in src/code_08020220.c): a u16
 * per run, 10 bits of button state and a 6-bit repeat count.  Measured against
 * the twenty-four recorded four-player tapes in the ROM's attract-mode demos:
 * mean run 22.8 frames, 17.1x smaller than the game's 12-bits-a-frame wire
 * format.  Ten minutes of four-player input is about 13 KiB.
 *
 * PortRbEncodeLog returns the number of bytes written, or the number of bytes
 * it would need if `cap` is too small.  PortRbDecodeLog replaces the timeline
 * and returns the number of frames it covers, or -1 if the blob is malformed. */
u32  PortRbEncodeLog(u8 *dest, u32 cap);
long PortRbDecodeLog(const u8 *src, u32 len);

/* Replay the decoded timeline from frame 0 up to `toFrame`, with drawing and
 * pacing off.  This is what a late joiner runs.  Returns non-zero on success.
 *
 * It is not fast because of anything here: it is fast because a frame that is
 * not drawn costs 2.5 us.  36000 frames -- ten minutes -- is 0.09 s natively
 * and 0.19 s in wasm. */
int  PortRbReplayTo(u32 toFrame);

/* --- diagnostics --------------------------------------------------------- */
struct PortRbStats {
    u32 frame;              /* the frame about to run                        */
    u32 confirmed;          /* every player's input known up to here         */
    u32 rollbacks;          /* how many times state was restored             */
    u32 resimFrames;        /* frames re-run in total                        */
    u32 deepest;            /* the deepest single rollback                   */
    u32 lateDrops;          /* inputs that arrived older than the ring       */
    u32 predictions;        /* frames run on a predicted input               */
    u32 mispredictions;     /* ... of those, ones that turned out wrong      */
};
void PortRbGetStats(struct PortRbStats *out);
void PortRbReport(void);

/* --- the self-test -------------------------------------------------------
 *
 * Proves the property everything else rests on, from inside a running game:
 * snapshot at frame A, run to B, restore A, run to B again, and check that the
 * two arrivals at B are bit-identical over every comparable region.
 *
 * This is not a unit test of this file.  It is a test of the claim that the
 * port's simulation is a pure function of (state, input) -- including the
 * port's own statics, which is where it is most likely to be false, because
 * platform/dma.c holds armed VBlank and HBlank transfers that outlive a frame.
 *
 * PORT_RB_SELFTEST=A:B from the environment, or the entry point. */
int  PortRbSelfTest(u32 atFrame, u32 span);
void PortRbSelfTestStep(void);

/* --- the input latch ------------------------------------------------------
 *
 * platform/main.c's UpdateKeyInput calls this at the one point where the
 * host's buttons become the game's buttons.  Returns non-zero, and writes the
 * keys the timeline says, whenever something here is driving: a re-simulated
 * frame has to be fed the input it was fed the first time.  Returns 0 in every
 * ordinary build, and then the latch is exactly what it was. */
int  PortRbKeyOverride(u16 *keys);

/* platform/main.c, for recording what the host most recently supplied. */
u16  PortCurrentKeys(void);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_ROLLBACK_H */
