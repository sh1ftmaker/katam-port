/* The netplay v2.1 engine -- see platform/port/netsim.h and
 * docs/design/NETPLAY-V2.md "Model v2.1".
 *
 * No rollback here, by design (v2.1): a peer never waits and never rewinds
 * to fix a misprediction.  A frame whose inputs are not sealed yet runs on
 * "repeat last" for the missing seats -- the same rule the server seals late
 * inputs with, so most frames agree anyway -- and whatever drift is left is
 * repaired by a world sync: a donor's snapshot (full, delta or pages) applied
 * at its frame, followed by a headless catch-up over this peer's own input
 * ring to the frame it was at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/dma.h"
#include "port/inputlog.h"
#include "port/rollback.h"
#include "port/simblock.h"
#include "port/netsim.h"
#include "port/game_addrs.h"
#include "../third_party/zstd/zstd.h"

#ifdef __cplusplus
extern "C" {
#endif

void PortRbApplyKeys(u16 keys);
u32  PortPpuStateSize(void);
void PortPpuStateSave(void *dest);
void PortPpuStateLoad(const void *src);
void PortRlNoteCorrection(u32 frame);
u64  PortHostMicros(void);

/* --- the canonical image --------------------------------------------------- */

#define DMA_BYTES (PORT_DMA_CANON_WORDS * 4u)
#define PPU_BYTES 16u

/* Parts, in order.  The sim block's size is its used bytes (header). */
struct Part { const char *name; u32 base, size; };
static const struct Part kParts[] = {
    { "ewram",  GBA_EWRAM_BASE, GBA_EWRAM_SIZE },
    { "iwram",  GBA_IWRAM_BASE, GBA_IWRAM_SIZE },
    { "simblk", PORT_SIM_BASE,  0 },                 /* PortSimUsedBytes()   */
    { "io",     GBA_IO_BASE,    GBA_IO_SIZE },
    { "dma",    0,              DMA_BYTES },
    { "ppu",    0,              PPU_BYTES },
    { "pltt",   GBA_PLTT_BASE,  GBA_PLTT_SIZE },
    { "vram",   GBA_VRAM_BASE,  GBA_VRAM_SIZE },
    { "oam",    GBA_OAM_BASE,   GBA_OAM_SIZE },
    { "sram",   GBA_SRAM_BASE,  GBA_SRAM_SIZE },
};
#define NUM_PARTS ((int)(sizeof kParts / sizeof kParts[0]))

static u32 PartSize(int i)
{
    return kParts[i].base == PORT_SIM_BASE ? PortSimUsedBytes() : kParts[i].size;
}

static u32 PartOffset(int i)
{
    u32 off = 0;
    int k;

    for (k = 0; k < i; k++)
        off += PartSize(k);
    return off;
}

u32 PortNetImageSize(void)
{
    return PartOffset(NUM_PARTS);
}

void PortNetImageCapture(u8 *out)
{
    int i;

    for (i = 0; i < NUM_PARTS; i++) {
        u8 *d = out + PartOffset(i);
        u32 n = PartSize(i);

        if (kParts[i].base == GBA_IO_BASE) {
            int ch;

            memcpy(d, (const void *)(uintptr_t)GBA_IO_BASE, n);
            /* The DMA source/destination mirrors: platform/dma.c writes the
             * host address there, and nothing reads them back. */
            for (ch = 0; ch < 4; ch++)
                memset(d + 0xB0 + ch * 12, 0, 8);
        } else if (!strcmp(kParts[i].name, "dma")) {
            u32 w[PORT_DMA_CANON_WORDS];
            int k;

            PortDmaStateCanon(w);
            for (k = 0; k < PORT_DMA_CANON_WORDS; k++) {
                d[k * 4] = (u8)w[k]; d[k * 4 + 1] = (u8)(w[k] >> 8);
                d[k * 4 + 2] = (u8)(w[k] >> 16); d[k * 4 + 3] = (u8)(w[k] >> 24);
            }
        } else if (!strcmp(kParts[i].name, "ppu")) {
            memset(d, 0, PPU_BYTES);
            if (PortPpuStateSize() <= PPU_BYTES)
                PortPpuStateSave(d);
        } else {
            memcpy(d, (const void *)(uintptr_t)kParts[i].base, n);
        }
    }
}

int PortNetImageApply(const u8 *in)
{
    int i, lost = 0;

    for (i = 0; i < NUM_PARTS; i++) {
        const u8 *s = in + PartOffset(i);
        u32 n = PartSize(i);

        if (!strcmp(kParts[i].name, "dma")) {
            u32 w[PORT_DMA_CANON_WORDS];
            int k;

            for (k = 0; k < PORT_DMA_CANON_WORDS; k++)
                w[k] = (u32)s[k * 4] | ((u32)s[k * 4 + 1] << 8)
                     | ((u32)s[k * 4 + 2] << 16) | ((u32)s[k * 4 + 3] << 24);
            lost = PortDmaStateFromCanon(w);
        } else if (!strcmp(kParts[i].name, "ppu")) {
            if (PortPpuStateSize() <= PPU_BYTES)
                PortPpuStateLoad(s);
        } else {
            memcpy((void *)(uintptr_t)kParts[i].base, s, n);
        }
    }
    return lost;
}

/* --- hashing ---------------------------------------------------------------
 * 64-bit, word at a time, little-endian words: the same number on every
 * host for the same bytes.  Not cryptographic; a desync detector. */
static u64 Hash64(u64 h, const u8 *p, u32 n)
{
    u32 i;

    for (i = 0; i + 8 <= n; i += 8) {
        u64 w;

        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    if (i < n) {
        u64 w = 0;

        memcpy(&w, p + i, n - i);
        h ^= w ^ ((u64)(n - i) << 56);
        h *= 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h ^ n;
}
#define HASH_SEED 0x4B41544D4E455431ull   /* "KATMNET1" */

static void Put64(u8 *p, u64 v) { int k; for (k = 0; k < 8; k++) p[k] = (u8)(v >> (8 * k)); }
static u64  Get64(const u8 *p)  { u64 v = 0; int k; for (k = 7; k >= 0; k--) v = (v << 8) | p[k]; return v; }
static void Put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32  Get32(const u8 *p)  { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* simVersion: what two peers must share for an image to mean the same
 * thing.  The layout of the image and the block, and the build's sim
 * identity if the build stamps one (KATAM_SIM_VERSION; tools/stamp_build.py
 * is meant to).  Native and wasm builds of one commit agree. */
#ifndef KATAM_SIM_VERSION
#define KATAM_SIM_VERSION "unstamped"
#endif
static u64 SimVersion(void)
{
    u8 b[32];
    u64 h;

    Put32(b, PORT_NET_VERSION);
    Put32(b + 4, PORT_SIM_VERSION);
    Put32(b + 8, PORT_MAX_SEATS);
    Put32(b + 12, PortNetImageSize());
    Put32(b + 16, PORT_SIM_CORE_BYTES);
    Put32(b + 20, (u32)NUM_PARTS);
    Put32(b + 24, 0);
    Put32(b + 28, 0);
    h = Hash64(HASH_SEED, b, sizeof b);
    return Hash64(h, (const u8 *)KATAM_SIM_VERSION, (u32)strlen(KATAM_SIM_VERSION));
}

/* --- state ------------------------------------------------------------------ */

#define NUM_BASES 4
#define NUM_CHECKPOINTS 64

struct Base { u32 frame; int valid; u8 *img; };

static struct {
    int active;
    u32 seats, localSeat, humans;
    u32 pendingHumansFrame, pendingHumans;
    int pendingHumansSet;
    /* inputs */
    u16 keys[PORT_NET_RING][PORT_MAX_SEATS];
    u32 tag[PORT_NET_RING];          /* frame + 1 the row is for, 0 = empty */
    u8  sealed[PORT_NET_RING];       /* bit s: seat s sealed                */
    u8  known[PORT_NET_RING];        /* bit s: seat s known (sealed/local)  */
    u16 last[PORT_MAX_SEATS];        /* last known per seat                 */
    /* images */
    u32 imgSize;
    u8 *cur;                         /* the latest frame-top image          */
    u32 curFrame;
    int curValid;
    u8 *scratch, *scratch2;
    struct Base bases[NUM_BASES];
    int nextBase;
    u32 cpFrame[NUM_CHECKPOINTS];
    u64 cpHash[NUM_CHECKPOINTS];
    u8  cpValid[NUM_CHECKPOINTS];
    /* a queued load */
    u8 *pending;
    u32 pendingFrame;
    int pendingCatchUp, havePending;
    /* catch-up */
    int catchingUp;
    u32 catchUpTo;
    u64 catchUpStart;
    s32 preX[4], preY[4];
    u16 preRoom[4];
    struct PortNetCorrection corr;
    struct PortNetStats st;
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
} N;

static int Ensure(void)
{
    u32 n = PortNetImageSize();
    int i;

    if (N.cur != NULL && N.imgSize == n)
        return 1;
    free(N.cur); free(N.scratch); free(N.scratch2); free(N.pending);
    for (i = 0; i < NUM_BASES; i++) {
        free(N.bases[i].img);
        N.bases[i].img = NULL;
        N.bases[i].valid = 0;
    }
    N.imgSize = n;
    N.cur = (u8 *)malloc(n);
    N.scratch = (u8 *)malloc(n);
    N.scratch2 = (u8 *)malloc(n);
    N.pending = (u8 *)malloc(n);
    for (i = 0; i < NUM_BASES; i++)
        N.bases[i].img = (u8 *)malloc(n);
    if (!N.cctx) N.cctx = ZSTD_createCCtx();
    if (!N.dctx) N.dctx = ZSTD_createDCtx();
    N.curValid = 0;
    N.st.rawBytes = n;
    return N.cur && N.scratch && N.scratch2 && N.pending && N.cctx && N.dctx;
}

static struct Base *FindBase(u32 frame)
{
    int i;

    for (i = 0; i < NUM_BASES; i++)
        if (N.bases[i].valid && N.bases[i].frame == frame)
            return &N.bases[i];
    return NULL;
}

static void KeepBase(u32 frame, const u8 *img)
{
    struct Base *b = FindBase(frame);

    if (b == NULL) {
        b = &N.bases[N.nextBase];
        N.nextBase = (N.nextBase + 1) % NUM_BASES;
    }
    memcpy(b->img, img, N.imgSize);
    b->frame = frame;
    b->valid = 1;
}

/* --- session ------------------------------------------------------------------ */

int PortNetSessionStart(u32 seats, u32 localSeat, u32 humanMask)
{
    u32 f = PortFrameNumber();

    if (seats < 1 || seats > PORT_MAX_SEATS || seats > 4 || localSeat >= seats) {
        PortError("[katam-port] netsim: %u seats, local %u is not a session",
                  (unsigned)seats, (unsigned)localSeat);
        return 0;
    }
    if (!Ensure())
        return 0;
    memset(N.tag, 0, sizeof N.tag);
    memset(N.sealed, 0, sizeof N.sealed);
    memset(N.known, 0, sizeof N.known);
    memset(N.last, 0, sizeof N.last);
    N.seats = seats;
    N.localSeat = localSeat;
    N.humans = humanMask & ((1u << seats) - 1u);
    N.pendingHumansSet = 0;
    N.havePending = 0;
    N.catchingUp = 0;
    N.active = 1;
    gPortSim->hdr.flags |= PORT_SIM_F_SESSION;
    GAME_MODE_FLAGS |= 2;
    PortLog("[katam-port] netsim: session at frame %u, %u seats, local %u, "
            "humans 0x%X, image %u bytes", (unsigned)f, (unsigned)seats,
            (unsigned)localSeat, (unsigned)N.humans, (unsigned)N.imgSize);
    return 1;
}

void PortNetSessionStop(void)
{
    N.active = 0;
    N.catchingUp = 0;
    N.havePending = 0;
}

int PortNetActive(void) { return N.active; }
int PortNetCatchingUp(void) { return N.catchingUp; }

int PortNetSetHumans(u32 frame, u32 humanMask)
{
    N.pendingHumansFrame = frame;
    N.pendingHumans = humanMask;
    N.pendingHumansSet = 1;
    return 1;
}

/* --- inputs ----------------------------------------------------------------- */

static int Row(u32 frame)
{
    int r = (int)(frame % PORT_NET_RING);

    if (N.tag[r] != frame + 1) {
        N.tag[r] = frame + 1;
        N.sealed[r] = N.known[r] = 0;
        memset(N.keys[r], 0, sizeof N.keys[r]);
    }
    return r;
}

void PortNetSetLocalInput(u32 frame, u16 keys)
{
    int r = Row(frame);

    if (N.sealed[r] & (1u << N.localSeat))
        return;
    N.keys[r][N.localSeat] = keys & 0x3FF;
    N.known[r] |= (u8)(1u << N.localSeat);
}

void PortNetSealFrame(u32 frame, const u16 *keys, u32 seats)
{
    int r = Row(frame);
    u32 s;

    for (s = 0; s < seats && s < N.seats; s++) {
        N.keys[r][s] = keys[s] & 0x3FF;
        N.sealed[r] |= (u8)(1u << s);
        N.known[r] |= (u8)(1u << s);
    }
}

/* The input seat s plays at `frame`: known, or the newest known before it
 * still in the ring ("repeat last"), else 0. */
static u16 InputAt(u32 s, u32 frame)
{
    u32 k;

    for (k = 0; k < PORT_NET_RING && k <= frame; k++) {
        u32 f = frame - k;
        int r = (int)(f % PORT_NET_RING);

        if (N.tag[r] == f + 1 && (N.known[r] & (1u << s)))
            return N.keys[r][s];
    }
    return 0;
}

/* The menu owner: the lowest human seat (NETPLAY-V2 R8).  Its column is what
 * the global KEYINPUT latch carries, on every peer alike. */
static u32 MenuOwner(void)
{
    u32 s;

    for (s = 0; s < N.seats; s++)
        if (N.humans & (1u << s))
            return s;
    return 0;
}

static u16 sLatchKeys;

static void DriveFrame(u32 frame)
{
    int slot;

    if (N.pendingHumansSet && frame >= N.pendingHumansFrame) {
        N.humans = N.pendingHumans & ((1u << N.seats) - 1u);
        N.pendingHumansSet = 0;
    }
    GAME_MODE_FLAGS |= 2;
    gPortSim->hdr.flags |= PORT_SIM_F_SESSION;
    GAME_REPIN_FOCUS((int)N.localSeat);
    GAME_PLAYER_COUNT = 4;
    for (slot = 0; slot < 4; slot++) {
        u16 prev = GAME_HELD(slot), now;

        if ((u32)slot < N.seats && (N.humans & (1u << slot)))
            now = InputAt((u32)slot, frame);
        else
            now = (u16)GAME_AI_INPUT(slot);
        GAME_HELD(slot) = now;
        GAME_PRESSED(slot) = (u16)(now & ~prev);
        GAME_RELEASED(slot) = (u16)(prev & ~now);
    }
    sLatchKeys = InputAt(MenuOwner(), frame);
    PortRbApplyKeys(sLatchKeys);
}

int PortNetKeyOverride(u16 *keys)
{
    if (!N.active)
        return 0;
    *keys = sLatchKeys;
    return 1;
}

/* --- blobs ------------------------------------------------------------------ */

u32 PortNetSnapBound(void)
{
    Ensure();
    return PORT_NET_HDR_BYTES + (u32)ZSTD_compressBound(N.imgSize + PORT_NET_PAGE / 8 + 64);
}

static void Header(u8 *h, u32 kind, u32 frame, u32 base, u32 payload, u64 stateHash)
{
    Put32(h, PORT_NET_MAGIC);
    h[4] = (u8)PORT_NET_VERSION; h[5] = 0;
    h[6] = (u8)kind; h[7] = 0;
    Put32(h + 8, frame);
    Put32(h + 12, base);
    Put32(h + 16, N.imgSize);
    Put32(h + 20, payload);
    Put64(h + 24, SimVersion());
    Put64(h + 32, stateHash);
}

static u32 Compress(u8 *out, u32 cap, const u8 *src, u32 n)
{
    size_t r;
    u64 t0 = PortHostMicros();

    if (cap < PORT_NET_HDR_BYTES)
        return 0;
    r = ZSTD_compressCCtx(N.cctx, out + PORT_NET_HDR_BYTES, cap - PORT_NET_HDR_BYTES,
                          src, n, 1);
    N.st.compressUs = (u32)(PortHostMicros() - t0);
    if (ZSTD_isError(r))
        return 0;
    return (u32)r;
}

static u64 ImageHash(const u8 *img)
{
    return Hash64(HASH_SEED, img, N.imgSize);
}

u32 PortNetSnapSave(u8 *out, u32 cap)
{
    u32 n;

    if (!Ensure() || !N.curValid)
        return 0;
    n = Compress(out, cap, N.cur, N.imgSize);
    if (n == 0)
        return 0;
    Header(out, PORT_NET_FULL, N.curFrame, 0, n, ImageHash(N.cur));
    KeepBase(N.curFrame, N.cur);
    N.st.lastFullBytes = n + PORT_NET_HDR_BYTES;
    return n + PORT_NET_HDR_BYTES;
}

u32 PortNetDeltaSave(u8 *out, u32 cap, u32 baseFrame)
{
    struct Base *b;
    u32 i, n;

    if (!Ensure() || !N.curValid || (b = FindBase(baseFrame)) == NULL)
        return 0;
    for (i = 0; i < N.imgSize; i++)
        N.scratch[i] = N.cur[i] ^ b->img[i];
    n = Compress(out, cap, N.scratch, N.imgSize);
    if (n == 0)
        return 0;
    Header(out, PORT_NET_DELTA, N.curFrame, baseFrame, n, ImageHash(N.cur));
    KeepBase(N.curFrame, N.cur);
    N.st.lastDeltaBytes = n + PORT_NET_HDR_BYTES;
    return n + PORT_NET_HDR_BYTES;
}

u32 PortNetPageCount(void)
{
    Ensure();
    return (N.imgSize + PORT_NET_PAGE - 1) / PORT_NET_PAGE;
}

static u64 PageHash(const u8 *img, u32 p)
{
    u32 off = p * PORT_NET_PAGE;
    u32 len = N.imgSize - off < PORT_NET_PAGE ? N.imgSize - off : PORT_NET_PAGE;

    return Hash64(HASH_SEED ^ p, img + off, len);
}

u32 PortNetPageHashes(u8 *out, u32 cap)
{
    u32 n = PortNetPageCount(), p;

    if (!N.curValid)
        return 0;
    for (p = 0; p < n && (p + 1) * 8 <= cap; p++)
        Put64(out + p * 8, PageHash(N.cur, p));
    return p;
}

/* Pages payload: u32 count, then per page u32 index + PORT_NET_PAGE bytes
 * (the last page short), all compressed. */
u32 PortNetPagesSave(u8 *out, u32 cap, const u8 *peer, u32 np)
{
    u32 n = PortNetPageCount(), p, w = 4, cnt = 0, z;

    if (!Ensure() || !N.curValid)
        return 0;
    for (p = 0; p < n; p++) {
        u32 off = p * PORT_NET_PAGE;
        u32 len = N.imgSize - off < PORT_NET_PAGE ? N.imgSize - off : PORT_NET_PAGE;

        if (p < np && Get64(peer + p * 8) == PageHash(N.cur, p))
            continue;
        if (w + 4 + len > N.imgSize)
            return PortNetSnapSave(out, cap);    /* nearly everything: send it all */
        Put32(N.scratch + w, p);
        memcpy(N.scratch + w + 4, N.cur + off, len);
        w += 4 + len;
        cnt++;
    }
    Put32(N.scratch, cnt);
    z = Compress(out, cap, N.scratch, w);
    if (z == 0)
        return 0;
    Header(out, PORT_NET_PAGES, N.curFrame, 0, z, ImageHash(N.cur));
    N.st.lastPagesBytes = z + PORT_NET_HDR_BYTES;
    return z + PORT_NET_HDR_BYTES;
}

int PortNetSnapInfo(const u8 *in, u32 len, u32 *frame, u32 *kind, u32 *base)
{
    if (len < PORT_NET_HDR_BYTES || Get32(in) != PORT_NET_MAGIC || in[4] != PORT_NET_VERSION)
        return -1;
    if (frame) *frame = Get32(in + 8);
    if (kind) *kind = in[6];
    if (base) *base = Get32(in + 12);
    return 0;
}

/* Reconstruct the full image a blob describes into N.pending. */
static int Reconstruct(const u8 *in, u32 len)
{
    u32 kind, frame, base, payload;
    size_t r;
    u64 t0;

    if (!Ensure())
        return -4;
    if (PortNetSnapInfo(in, len, &frame, &kind, &base) < 0)
        return -1;
    if (Get64(in + 24) != SimVersion() || Get32(in + 16) != N.imgSize)
        return -2;
    payload = Get32(in + 20);
    if (PORT_NET_HDR_BYTES + payload > len)
        return -4;
    t0 = PortHostMicros();
    r = ZSTD_decompressDCtx(N.dctx, N.scratch2, N.imgSize,
                            in + PORT_NET_HDR_BYTES, payload);
    N.st.decompressUs = (u32)(PortHostMicros() - t0);
    if (ZSTD_isError(r))
        return -4;
    if (kind == PORT_NET_FULL) {
        if (r != N.imgSize)
            return -4;
        memcpy(N.pending, N.scratch2, N.imgSize);
    } else if (kind == PORT_NET_DELTA) {
        struct Base *b = FindBase(base);
        u32 i;

        if (b == NULL)
            return -3;
        if (r != N.imgSize)
            return -4;
        for (i = 0; i < N.imgSize; i++)
            N.pending[i] = b->img[i] ^ N.scratch2[i];
    } else if (kind == PORT_NET_PAGES) {
        u32 cnt, w = 4, k;

        /* Onto this peer's own image of the latest frame. */
        if (!N.curValid)
            return -3;
        memcpy(N.pending, N.cur, N.imgSize);
        cnt = Get32(N.scratch2);
        for (k = 0; k < cnt; k++) {
            u32 p = Get32(N.scratch2 + w), off = p * PORT_NET_PAGE;
            u32 l = N.imgSize - off < PORT_NET_PAGE ? N.imgSize - off : PORT_NET_PAGE;

            if (off >= N.imgSize || w + 4 + l > (u32)r)
                return -4;
            memcpy(N.pending + off, N.scratch2 + w + 4, l);
            w += 4 + l;
        }
    } else {
        return -1;
    }
    if (ImageHash(N.pending) != Get64(in + 32))
        return -5;
    N.pendingFrame = frame;
    return 0;
}

static int Queue(const u8 *in, u32 len, int catchUp)
{
    int rc = Reconstruct(in, len);

    if (rc < 0) {
        N.st.badLoads++;
        PortError("[katam-port] netsim: snapshot rejected (%d)", rc);
        return rc;
    }
    if (catchUp && N.active && PortFrameNumber() > N.pendingFrame
     && PortFrameNumber() - N.pendingFrame >= PORT_NET_RING) {
        N.st.badLoads++;
        return -6;
    }
    N.havePending = 1;
    N.pendingCatchUp = catchUp;
    return 0;
}

int PortNetApplyAt(const u8 *in, u32 len) { return Queue(in, len, 1); }
int PortNetSnapLoad(const u8 *in, u32 len) { return Queue(in, len, 0); }

/* --- hashes ----------------------------------------------------------------- */

u32 PortNetImageFrame(void) { return N.curFrame; }

void PortNetStateHash(u8 out[8])
{
    Put64(out, N.curValid ? ImageHash(N.cur) : 0);
}

/* NETPLAY-V2 §5.1's regions, over the image.  0 core globals and RNG (IWRAM
 * outside the task table and heaps), 1 gKirbys, 2 gCurLevelInfo, 3 AI,
 * 4 EWRAM task heap, 5 IWRAM tasks + heap, 6 other EWRAM, 7 (spare: other
 * IWRAM is region 0), 8 the sim block, 9 the session save, 10 the mod store
 * (in 8 until used), 11 I/O + DMA + PPU, 12 VRAM, 13 palette + OAM. */
struct Span { u32 lo, hi; u8 region; };
static const struct Span kSpans[] = {
    { 0x02000000, 0x02020080, 4 },  { 0x02020EE0, 0x02021580, 1 },
    { 0x02023530, 0x02024ED0, 2 },  { 0x02038590, 0x02038980, 3 },
    { 0x030019F0, 0x03002400, 5 },  { 0x03003A20, 0x03006024, 5 },
};

void PortNetRegionHashes(u8 out[PORT_NET_REGIONS * 8])
{
    u64 h[PORT_NET_REGIONS];
    int i, r;

    for (r = 0; r < PORT_NET_REGIONS; r++)
        h[r] = HASH_SEED ^ (u64)r;
    if (N.curValid) {
        for (i = 0; i < NUM_PARTS; i++) {
            const u8 *p = N.cur + PartOffset(i);
            u32 n = PartSize(i), a;
            const char *nm = kParts[i].name;

            if (!strcmp(nm, "ewram") || !strcmp(nm, "iwram")) {
                /* 64-byte granules, each to its region: cheap and stable. */
                for (a = 0; a < n; a += 64) {
                    u32 addr = kParts[i].base + a;
                    int reg = !strcmp(nm, "ewram") ? 6 : 0;
                    u32 k;

                    for (k = 0; k < sizeof kSpans / sizeof kSpans[0]; k++)
                        if (addr >= kSpans[k].lo && addr < kSpans[k].hi)
                            reg = kSpans[k].region;
                    h[reg] = Hash64(h[reg], p + a, 64);
                }
                continue;
            }
            r = !strcmp(nm, "simblk") ? 8 : !strcmp(nm, "sram") ? 9
              : !strcmp(nm, "vram") ? 12 : (!strcmp(nm, "pltt") || !strcmp(nm, "oam")) ? 13 : 11;
            h[r] = Hash64(h[r], p, n);
        }
    }
    for (r = 0; r < PORT_NET_REGIONS; r++)
        Put64(out + r * 8, h[r]);
}

int PortNetCheckpointHash(u32 frame, u8 out[8])
{
    int i = (int)((frame / PORT_NET_CHECKPOINT) % NUM_CHECKPOINTS);

    if (!N.cpValid[i] || N.cpFrame[i] != frame)
        return 0;
    Put64(out, N.cpHash[i]);
    return 1;
}

int PortNetLastCorrection(struct PortNetCorrection *out)
{
    if (out)
        *out = N.corr;
    return (int)N.corr.seq;
}

void PortNetGetStats(struct PortNetStats *out)
{
    if (out)
        *out = N.st;
}

/* --- the frame hook --------------------------------------------------------- */

#define KIRBY(k) (0x02020EE0u + (u32)(k) * 424u)
static void KirbyPos(s32 *x, s32 *y, u16 *room)
{
    int k;

    for (k = 0; k < 4; k++) {
        memcpy(&x[k], (const void *)(uintptr_t)(KIRBY(k) + 64), 4);
        memcpy(&y[k], (const void *)(uintptr_t)(KIRBY(k) + 68), 4);
        memcpy(&room[k], (const void *)(uintptr_t)(KIRBY(k) + 96), 2);
    }
}

void PortNetFrameTop(void)
{
    u32 frame;

    if (!N.active && !N.havePending)
        return;

    /* A queued snapshot: now is the frame boundary. */
    if (N.havePending) {
        u32 was = PortFrameNumber();
        u64 t0 = PortHostMicros();

        N.havePending = 0;
        if (N.active && N.pendingCatchUp)
            KirbyPos(N.preX, N.preY, N.preRoom);
        N.st.lostDmaPtrs += (u32)PortNetImageApply(N.pending);
        N.st.applyUs = (u32)(PortHostMicros() - t0);
        N.st.loads++;
        KeepBase(N.pendingFrame, N.pending);
        if (N.active && N.pendingCatchUp && was > PortFrameNumber()) {
            N.catchingUp = 1;
            N.catchUpTo = was;
            N.catchUpStart = PortHostMicros();
            N.corr.fromFrame = PortFrameNumber();
            PortSetRenderEnabled(0);
        } else if (N.active) {
            N.corr.fromFrame = N.corr.frame = PortFrameNumber();
            N.corr.seq++;
            PortRlNoteCorrection(PortFrameNumber());
        }
    }

    frame = PortFrameNumber();
    if (N.catchingUp && frame >= N.catchUpTo) {
        s32 x[4], y[4];
        u16 room[4];
        int k;

        N.catchingUp = 0;
        PortSetRenderEnabled(1);
        N.st.catchUpUs = (u32)(PortHostMicros() - N.catchUpStart);
        N.st.catchUpFrames = frame - N.corr.fromFrame;
        KirbyPos(x, y, room);
        N.corr.kirbyMask = 0;
        for (k = 0; k < 4; k++) {
            N.corr.oldX[k] = N.preX[k]; N.corr.oldY[k] = N.preY[k];
            N.corr.newX[k] = x[k];      N.corr.newY[k] = y[k];
            N.corr.oldRoom[k] = N.preRoom[k]; N.corr.newRoom[k] = room[k];
            if (x[k] != N.preX[k] || y[k] != N.preY[k] || room[k] != N.preRoom[k])
                N.corr.kirbyMask |= 1u << k;
        }
        N.corr.frame = frame;
        N.corr.seq++;
        PortRlNoteCorrection(frame);
    }
    if (!N.active)
        return;

    /* The frame-top image, before anything of this frame is applied: what a
     * save sends and what checkpoints hash. */
    {
        u64 t0 = PortHostMicros();

        PortNetImageCapture(N.cur);
        N.curFrame = frame;
        N.curValid = 1;
        N.st.captureUs = (u32)(PortHostMicros() - t0);
        if (frame % PORT_NET_CHECKPOINT == 0) {
            int i = (int)((frame / PORT_NET_CHECKPOINT) % NUM_CHECKPOINTS);

            N.cpFrame[i] = frame;
            N.cpHash[i] = ImageHash(N.cur);
            N.cpValid[i] = 1;
        }
    }
    DriveFrame(frame);
}

#ifdef __cplusplus
}
#endif
