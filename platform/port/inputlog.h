#ifndef GUARD_PORT_INPUTLOG_H
#define GUARD_PORT_INPUTLOG_H
/* Input recording, replay, and the per-frame simulation hash.
 *
 * This is the determinism/regression harness (docs/DETERMINISM.md).  Three
 * pieces, all in platform/inputlog.c so that the web and native builds run the
 * identical code at the identical point in the frame:
 *
 *   record   every key mask the game latches, and every warp the host asks
 *            for, indexed by frame, written out as a small text file.
 *   replay   the same file fed back: it overrides whatever the host polled
 *            at the one latch (platform/main.c, UpdateKeyInput), so a replay
 *            is independent of the keyboard, --mash and the harness.
 *   hash     one line per frame of FNV-1a hashes over the simulation state,
 *            taken at the top of PortPresentFrame -- the same point rollback
 *            snapshots -- and handed to the host (PortHostFrameState).
 *
 * The file format is text, versioned, and holds inputs only:
 *
 *     katam-input 1
 *     frames 1400
 *     option rando SEED FLAGS
 *     option view MODE PADX PADY CULL PINHUD TILEMAP
 *     k FRAME MASK        the latched mask from FRAME on (hex)
 *     warp FRAME PLAYER ROOM CELLX CELLY
 *
 * Lines starting with '#' are comments.  Nothing in it comes from the ROM. */
#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

/* Load a recording for replay.  Returns the number of frames it covers, or -1
 * if it does not parse.  Applies its options (rando, view) immediately. */
long PortInputLogLoad(const char *text, u32 len);
/* The frame count a loaded recording covers, 0 if none. */
u32  PortInputLogFrames(void);
int  PortInputLogReplaying(void);

/* Start recording.  The host calls PortInputLogSerialize at exit. */
void PortInputLogRecordStart(void);
/* How many frames the run lasted, so a replay stops where the run did. */
void PortInputLogNoteFrames(u32 frames);
void PortInputLogNoteRando(u32 flags, u32 seed);
void PortInputLogNoteView(u32 mode, s32 padX, s32 padY, u32 cull, u32 pinHud,
                          u32 tilemap);
/* A malloc'd text recording of everything latched so far; *len its length. */
char *PortInputLogSerialize(u32 *len);

/* A warp request at a frame, from the host.  Executed (PortWarp) on that frame
 * at the latch, and recorded.  Ignored while replaying -- the recording's own
 * warps are the ones that happen. */
void PortInputLogScheduleWarp(u32 frame, u8 player, u16 room, u8 cellX, u8 cellY);

/* Per-frame hashes on/off.  When on, PortHostFrameState is called once a
 * frame.  PortSetStateDumpAt additionally hands the host the canonical state
 * bytes at that frame (a list; call it once per frame wanted). */
void PortSetFrameStateLog(int on);
void PortSetStateDumpAt(u32 frame);

/* platform/main.c's hooks.  Not for hosts. */
void PortInputLogFrameStart(u32 frame);
void PortInputLogLatch(u32 frame, u16 *keys);

/* The hashes themselves, for anything that wants one now. */
#define PORT_HASH_SIM  0     /* EWRAM, IWRAM, IO (less host addresses), DMA, frame */
#define PORT_HASH_VID  1     /* PLTT, VRAM, OAM, the PPU's affine latches          */
#define PORT_HASH_SRAM 2     /* save memory                                          */
#define PORT_HASH_PIX  3     /* the picture: the frame PortRenderFrame just drew     */
#define PORT_HASH_COUNT 4
void PortStateHashes(u32 out[PORT_HASH_COUNT]);

/* platform/dma.c: the channel table with host pointers made comparable. */
#define PORT_DMA_CANON_WORDS (4 * 7)
void PortDmaStateCanon(u32 out[PORT_DMA_CANON_WORDS]);

#ifdef __cplusplus
}
#endif
#endif
