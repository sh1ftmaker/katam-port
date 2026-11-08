#ifndef GUARD_PORT_NETSIM_H
#define GUARD_PORT_NETSIM_H
/* The netplay v2.1 engine: portable world snapshots and deltas, region and
 * page hashes, apply-at-frame with a headless catch-up, the correction event,
 * and the per-frame input driver.  docs/design/NETPLAY-V2.md "Model v2.1" is
 * the contract with the lobby/protocol side; platform/netsim.c is the code.
 *
 * Every function here is also a wasm export (_PortNet*).  Sizes and offsets
 * are u32, 64-bit hashes travel as 8 little-endian bytes, so JS never needs
 * a BigInt to call any of it.  All calls are legal at any time from the host
 * (between frames, or inside the present callback): saves read the image
 * captured at the top of the latest frame, and loads are queued and applied
 * at the top of the next one, the one point where the game's state is a
 * clean frame boundary. */
#include "gba/types.h"
#include "port/simblock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- session ------------------------------------------------------------- */

/* Start at the current frame boundary.  `seats` 1..PORT_MAX_SEATS (and at
 * most the game's 4 Kirbys today); seat s drives Kirby slot s.  `localSeat`
 * is this peer (its camera, HUD, pause menu); `humanMask` bit s = seat s is a
 * person, else the game's AI drives that Kirby.  Puts the game on its
 * network-input path with four slots and sets the sim block's session flag
 * (port/simblock.h, PortSimSaveOwner).  Returns non-zero on success. */
int  PortNetSessionStart(u32 seats, u32 localSeat, u32 humanMask);
void PortNetSessionStop(void);
int  PortNetActive(void);
/* Change who is human, applied at the top of `frame` (an agreed event). */
int  PortNetSetHumans(u32 frame, u32 humanMask);

/* --- inputs --------------------------------------------------------------
 * One u16 per seat per frame, bits 0-9 the GBA keys.  A seat's input for a
 * frame that has not been sealed is predicted as its last known input
 * (sealed, or this peer's own). */
#define PORT_NET_RING 1024           /* frames of inputs kept               */
void PortNetSetLocalInput(u32 frame, u16 keys);
/* The server's sealed record for `frame`: authoritative, replaces anything
 * predicted.  keys[seats], seat order. */
void PortNetSealFrame(u32 frame, const u16 *keys, u32 seats);

/* --- snapshots ------------------------------------------------------------
 * A blob is a 40-byte header and a zstd frame:
 *   u32 magic 'KSNP'  u16 version  u16 kind (0 full, 1 delta, 2 pages)
 *   u32 frame  u32 baseFrame  u32 rawSize  u32 payloadSize
 *   u8 simVersion[8]  u8 stateHash[8]          (all little-endian)
 * The raw image is the canonical world (PortNetImage* below); a delta is the
 * XOR of the image against a base both sides hold; a pages blob is the
 * 4 KiB pages whose hashes differ from a peer's. */
#define PORT_NET_MAGIC     0x504E534Bu   /* 'KSNP' */
#define PORT_NET_VERSION   1u
#define PORT_NET_HDR_BYTES 40u
#define PORT_NET_PAGE      4096u
#define PORT_NET_REGIONS   16
enum { PORT_NET_FULL = 0, PORT_NET_DELTA = 1, PORT_NET_PAGES = 2 };

u32  PortNetSnapBound(void);
/* Full snapshot of the latest frame-top image.  Returns its length, or 0. */
u32  PortNetSnapSave(u8 *out, u32 cap);
/* Delta against base `baseFrame` (a frame this peer saved or loaded a blob
 * of, among the last few).  0 if that base is gone: send a full one. */
u32  PortNetDeltaSave(u8 *out, u32 cap, u32 baseFrame);
/* Pages differing from a peer's page hashes (8 bytes each, n of them). */
u32  PortNetPagesSave(u8 *out, u32 cap, const u8 *peerHashes, u32 n);
/* Queue a blob to be applied at the top of the next frame; then re-run this
 * peer's inputs from the blob's frame to the frame it was at, headless.
 * 0 on success, <0 on a bad blob (-1 magic/version, -2 simVersion, -3 base
 * missing, -4 decompress, -5 hash mismatch after reconstruction, -6 frame
 * outside the input ring). */
int  PortNetApplyAt(const u8 *in, u32 len);
/* The same, without the catch-up: the game resumes at the blob's frame. */
int  PortNetSnapLoad(const u8 *in, u32 len);
int  PortNetCatchingUp(void);
/* Parse a header without applying: frame, kind, baseFrame, rawSize. */
int  PortNetSnapInfo(const u8 *in, u32 len, u32 *frame, u32 *kind, u32 *baseFrame);

/* --- hashes (of the latest frame-top image) ------------------------------ */
u32  PortNetImageFrame(void);                 /* which frame that is         */
void PortNetStateHash(u8 out[8]);
void PortNetRegionHashes(u8 out[PORT_NET_REGIONS * 8]);
u32  PortNetPageCount(void);
u32  PortNetPageHashes(u8 *out, u32 cap);     /* 8 bytes per page; returns n */
/* The 64-bit hash of the image at a checkpoint frame (every
 * PORT_NET_CHECKPOINT frames), if still kept.  Returns 1 if found. */
#define PORT_NET_CHECKPOINT 30u
int  PortNetCheckpointHash(u32 frame, u8 out[8]);

/* --- the correction event --------------------------------------------------
 * After an applied blob has caught up to the frame the peer was at: which
 * Kirbys' positions the correction moved.  Also hands the frame to the
 * renderer (PortRlNoteCorrection), which snaps instead of interpolating. */
struct PortNetCorrection {
    u32 frame;                /* the frame the correction landed on          */
    u32 fromFrame;            /* the blob's frame                            */
    u32 kirbyMask;            /* bit k: Kirby k moved or changed room        */
    s32 oldX[4], oldY[4], newX[4], newY[4];   /* 1/256 px, world             */
    u16 oldRoom[4], newRoom[4];
    u32 seq;                  /* increments per correction                   */
};
int  PortNetLastCorrection(struct PortNetCorrection *out);

/* --- statistics ------------------------------------------------------------ */
struct PortNetStats {
    u32 rawBytes;             /* canonical image size                        */
    u32 lastFullBytes, lastDeltaBytes, lastPagesBytes;
    u32 captureUs, compressUs, decompressUs, applyUs, catchUpUs;  /* last    */
    u32 catchUpFrames;
    u32 loads, badLoads, lostDmaPtrs;
};
void PortNetGetStats(struct PortNetStats *out);

/* --- the canonical image -------------------------------------------------- */
u32  PortNetImageSize(void);
/* The world now, canonical: EWRAM, IWRAM, the sim block, I/O (DMA address
 * mirrors zeroed), the DMA channels canonical, the PPU latches, palette,
 * VRAM, OAM, save memory.  Only valid at a frame boundary. */
void PortNetImageCapture(u8 *out);
int  PortNetImageApply(const u8 *in);        /* returns lost DMA pointers   */

/* --- tests ------------------------------------------------------------------ */
void PortNetTestArm(const char *spec);       /* platform/netsim.c says what  */

/* --- frame hooks (platform/main.c) ----------------------------------------- */
void PortNetFrameTop(void);                  /* top of PortPresentFrame     */
int  PortNetKeyOverride(u16 *keys);          /* the KEYINPUT latch          */

#ifdef __cplusplus
}
#endif
#endif /* GUARD_PORT_NETSIM_H */
