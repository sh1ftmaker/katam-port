/* Single-player save states, rewind and fast-forward / slow motion.
 *
 * Built on the rollback engine's snapshot (platform/rollback.c,
 * PortRbSnapshotSave: the GBA map the game writes, the port's sim block, the
 * DMA channels and PPU latches -- 400 KiB, everything a frame depends on):
 *
 *   - a SLOT is one snapshot, zstd-compressed (~50 KiB) with a header, handed
 *     to the page to keep in the browser (web/states.js, IndexedDB).  "slots"
 *     below says why it is tied to the build that wrote it.
 *
 *   - the REWIND ring is the rollback snapshot (platform/rollback.c,
 *     PortRbSnapshotSave) taken every RW_EVERY frames, kept as a backward
 *     chain of XOR deltas: sCur is the newest state whole, and entry k is
 *     state k XOR state k+1, zstd-compressed (a few KiB: most of the world
 *     does not change in two frames).  Holding rewind pops one entry a frame
 *     -- sCur ^= delta, load it -- so the picture runs backwards at RW_EVERY
 *     times speed and stops at the oldest state kept.
 *
 * Both are applied at the top of PortPresentFrame, the point the rollback
 * engine restores at (port/rollback.h says why it is safe), and neither runs
 * while the SETTINGS screen is up: that screen's state is partly the host's
 * (platform/menu_poc.c), and a state taken or restored under it would not
 * match.  A request made there waits until it closes.
 *
 * Never online (PortStAllowed): any session driver, seats or the online
 * local-multiplayer engine turns the whole thing off and drops the ring.
 * Recording is off until the page turns it on (PortStSetPrefs), so the
 * headless harness, the goldens and the native build pay nothing; nothing
 * here writes game memory unless asked to load. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/rollback.h"
#include "port/netsim.h"
#include "ui/settings.h"
#include "port/lmpnet.h"
#include "port/seats.h"
#include "port/states.h"
#include "../third_party/zstd/zstd.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define ST_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define ST_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

u32  PortPocScreenLastDraw(void);
void PortRlNoteCorrection(u32 frame);

#define ST_MAGIC   0x5453534Bu      /* 'KSST' */
#define ST_VERSION 1u
#define ST_HDR     32u

#define RW_EVERY   2                /* frames between rewind states        */
#define RW_MAX     450              /* entries: 15 s of play               */
#define RW_BUDGET  (16u << 20)      /* compressed bytes kept, at most      */

static struct {
    int enabled, rewindOn, fast, slow, slot;
    u32 have;
    int held;
    int saveReq;                    /* slot to save, 0 none                */
    int loadReq;                    /* slot whose blob is in loadBuf       */
    u8 *loadBuf; u32 loadLen;
    /* the ring */
    u32 snap;                       /* PortRbSnapshotBytes                 */
    u8 *cur, *tmp;
    int curValid;
    u8 *ent[RW_MAX]; u32 entLen[RW_MAX];
    int head, count;                /* head: the next entry's index        */
    u32 bytes;
    u32 tick;
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
} S;

/* The defaults until the page says otherwise (PortStSetPrefs). */
static void Defaults(void)
{
    static int done;

    if (done)
        return;
    done = 1;
    S.rewindOn = 1; S.fast = 3; S.slow = 2; S.slot = 1;
}

/* --- the page -------------------------------------------------------------- */
#ifdef __EMSCRIPTEN__
EM_JS(void, StNote, (const char *msg), {
    if (Module.portStatesNote) Module.portStatesNote(UTF8ToString(msg));
});
EM_JS(void, StStore, (int slot, const u8 *p, u32 len), {
    if (Module.portStatesStore) Module.portStatesStore(slot, HEAPU8.slice(p, p + len));
});
EM_JS(void, StFetch, (int slot), {
    if (Module.portStatesFetch) Module.portStatesFetch(slot);
});
EM_JS(void, StPrefsChanged, (void), {
    if (Module.portStatesPrefs) Module.portStatesPrefs();
});
#else
static void StNote(const char *msg) { PortLog("[states] %s", msg); }
static void StStore(int slot, const u8 *p, u32 len) { (void)slot; (void)p; (void)len; }
static void StFetch(int slot) { (void)slot; }
static void StPrefsChanged(void) { }
#endif

static void Put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 Get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

ST_EXPORT int PortStAllowed(void)
{
    return !(gPortLnOn || PortLnActive() || PortRbActive() || PortNetActive()
             || PortSeatActive());
}

/* The SETTINGS screen drew within the last two frames. */
static int ScreenUp(void)
{
    return (u32)(PortFrameNumber() - PortPocScreenLastDraw()) <= 2u;
}

static int Ctx(void)
{
    if (!S.cctx) S.cctx = ZSTD_createCCtx();
    if (!S.dctx) S.dctx = ZSTD_createDCtx();
    return S.cctx && S.dctx;
}

/* --- the ring -------------------------------------------------------------- */
static void RingDropOldest(void)
{
    int i = (S.head - S.count + RW_MAX) % RW_MAX;

    S.bytes -= S.entLen[i];
    free(S.ent[i]);
    S.ent[i] = NULL;
    S.count--;
}

static void RingClear(void)
{
    while (S.count > 0)
        RingDropOldest();
    S.curValid = 0;
}

static int RingBuffers(void)
{
    u32 n = PortRbSnapshotBytes();

    if (S.cur && S.snap == n)
        return 1;
    RingClear();
    free(S.cur); free(S.tmp);
    S.snap = n;
    S.cur = (u8 *)malloc(n);
    S.tmp = (u8 *)malloc(n);
    return S.cur && S.tmp;
}

static void RingRecord(void)
{
    u32 i, cap;
    size_t r;
    u8 *out;

    if (!RingBuffers() || !Ctx())
        return;
    if (!S.curValid) {
        PortRbSnapshotSave(S.cur);
        S.curValid = 1;
        return;
    }
    PortRbSnapshotSave(S.tmp);
    for (i = 0; i < S.snap; i++)
        S.cur[i] ^= S.tmp[i];       /* cur is now the delta */
    cap = (u32)ZSTD_compressBound(S.snap);
    out = (u8 *)malloc(cap);
    r = out ? ZSTD_compressCCtx(S.cctx, out, cap, S.cur, S.snap, 1) : 0;
    memcpy(S.cur, S.tmp, S.snap);
    if (!out || ZSTD_isError(r)) {
        free(out);
        RingClear();                /* a broken chain is worse than none */
        PortRbSnapshotSave(S.cur);
        S.curValid = 1;
        return;
    }
    out = (u8 *)realloc(out, r);
    if (S.count == RW_MAX)
        RingDropOldest();
    S.ent[S.head] = out;
    S.entLen[S.head] = (u32)r;
    S.head = (S.head + 1) % RW_MAX;
    S.count++;
    S.bytes += (u32)r;
    while (S.bytes > RW_BUDGET && S.count > 1)
        RingDropOldest();
}

/* One step back: the newest delta off the chain, applied. */
static void RingStepBack(void)
{
    u32 i;
    size_t r;
    int k;

    if (!S.curValid)
        return;
    if (S.count > 0) {
        k = (S.head - 1 + RW_MAX) % RW_MAX;
        r = ZSTD_decompressDCtx(S.dctx, S.tmp, S.snap, S.ent[k], S.entLen[k]);
        S.bytes -= S.entLen[k];
        free(S.ent[k]);
        S.ent[k] = NULL;
        S.head = k;
        S.count--;
        if (ZSTD_isError(r) || r != S.snap) {
            RingClear();
            return;
        }
        for (i = 0; i < S.snap; i++)
            S.cur[i] ^= S.tmp[i];
    }
    /* At the oldest state the picture holds there. */
    PortRbSnapshotLoad(S.cur);
    PortRlNoteCorrection(PortFrameNumber());
}

ST_EXPORT int PortStRingFrames(void)
{
    return S.count * RW_EVERY;
}

/* --- slots -----------------------------------------------------------------
 *
 * A slot is the rollback snapshot itself, as the ring uses: byte-exact,
 * including the I/O block's DMA address mirrors and the DMA channel table,
 * whose pointers are this build's addresses.  (netsim's canonical image
 * zeroes the mirrors and was measured to drift six frames after a load.)
 * So a slot only means something to the build that wrote it -- the game's
 * task callbacks in EWRAM are function-table indices too -- and the header
 * carries a fingerprint of this build's layout: where its data and a few of
 * its functions landed, and the snapshot's size.  Another build's state is
 * refused with a message, never loaded.  Save memory is not in a snapshot,
 * so a state never rewinds or overwrites the save file. */
static u32 Fingerprint(void)
{
    uintptr_t w[6];
    u32 h = 0x811C9DC5u, i;
    const u8 *b = (const u8 *)w;

    w[0] = (uintptr_t)&S;
    w[1] = (uintptr_t)&gPortSettings;
    w[2] = (uintptr_t)&PortStFrame;
    w[3] = (uintptr_t)&PortRbSnapshotLoad;
    w[4] = (uintptr_t)&PortPresentFrame;
    w[5] = (uintptr_t)PortRbSnapshotBytes();
    for (i = 0; i < sizeof w; i++)
        h = (h ^ b[i]) * 0x01000193u;
    return h;
}

static void DoSave(int slot)
{
    u32 n = PortRbSnapshotBytes(), cap;
    u8 *raw, *out;
    size_t r;
    char msg[48];

    if (!Ctx())
        return;
    raw = (u8 *)malloc(n);
    cap = ST_HDR + (u32)ZSTD_compressBound(n);
    out = (u8 *)malloc(cap);
    if (!raw || !out) {
        free(raw); free(out);
        StNote("NOT ENOUGH MEMORY TO SAVE");
        return;
    }
    PortRbSnapshotSave(raw);
    r = ZSTD_compressCCtx(S.cctx, out + ST_HDR, cap - ST_HDR, raw, n, 3);
    free(raw);
    if (ZSTD_isError(r)) {
        free(out);
        StNote("SAVE FAILED");
        return;
    }
    Put32(out, ST_MAGIC);
    Put32(out + 4, ST_VERSION);
    Put32(out + 8, Fingerprint());
    Put32(out + 12, 0);
    Put32(out + 16, n);
    Put32(out + 20, (u32)r);
    Put32(out + 24, PortFrameNumber());
    Put32(out + 28, 0);
    StStore(slot, out, ST_HDR + (u32)r);
    free(out);
    S.have |= 1u << (slot - 1);
    snprintf(msg, sizeof msg, "STATE %d SAVED", slot);
    StNote(msg);
}

/* 0 ok, else why not (also said on screen). */
static int DoLoad(int slot, const u8 *in, u32 len)
{
    u32 n = PortRbSnapshotBytes();
    u8 *raw;
    size_t r;
    char msg[48];

    if (len < ST_HDR || Get32(in) != ST_MAGIC || Get32(in + 4) != ST_VERSION) {
        StNote("NOT A SAVE STATE");
        return -1;
    }
    if (Get32(in + 8) != Fingerprint() || Get32(in + 16) != n) {
        StNote("STATE IS FROM ANOTHER VERSION");
        return -2;
    }
    if (!Ctx())
        return -3;
    raw = (u8 *)malloc(n);
    if (!raw) {
        StNote("NOT ENOUGH MEMORY TO LOAD");
        return -3;
    }
    r = ZSTD_decompressDCtx(S.dctx, raw, n, in + ST_HDR, len - ST_HDR);
    if (ZSTD_isError(r) || r != n) {
        free(raw);
        StNote("STATE IS DAMAGED");
        return -4;
    }
    PortRbSnapshotLoad(raw);
    free(raw);
    PortRlNoteCorrection(PortFrameNumber());
    /* The ring's chain ends at the state before the load: start it again. */
    RingClear();
    snprintf(msg, sizeof msg, "STATE %d LOADED", slot);
    StNote(msg);
    return 0;
}

/* --- the frame ------------------------------------------------------------- */
void PortStFrame(void)
{
    Defaults();
    if (!PortStAllowed()) {
        if (S.count || S.curValid)
            RingClear();
        if (S.saveReq || S.loadReq) {
            S.saveReq = S.loadReq = 0;
            free(S.loadBuf); S.loadBuf = NULL;
            StNote("NOT DURING AN ONLINE GAME");
        }
        S.held = 0;
        return;
    }
    if (ScreenUp())
        return;                     /* requests wait; the ring pauses */
    if (S.loadReq) {
        int slot = S.loadReq;

        S.loadReq = 0;
        DoLoad(slot, S.loadBuf, S.loadLen);
        free(S.loadBuf); S.loadBuf = NULL;
        return;
    }
    if (S.saveReq) {
        int slot = S.saveReq;

        S.saveReq = 0;
        DoSave(slot);
    }
    if (S.held && S.rewindOn) {
        RingStepBack();
        return;
    }
    if (S.enabled && S.rewindOn && ++S.tick >= RW_EVERY) {
        S.tick = 0;
        RingRecord();
    } else if (!S.rewindOn && (S.count || S.curValid)) {
        RingClear();
    }
}

/* --- requests and prefs ---------------------------------------------------- */
ST_EXPORT void PortStRequestSave(int slot)
{
    if (slot < 1 || slot > PORT_ST_SLOTS)
        return;
    if (!PortStAllowed()) { StNote("NOT DURING AN ONLINE GAME"); return; }
    S.saveReq = slot;
}

ST_EXPORT void PortStRequestLoad(int slot)
{
    char msg[40];

    if (slot < 1 || slot > PORT_ST_SLOTS)
        return;
    if (!PortStAllowed()) { StNote("NOT DURING AN ONLINE GAME"); return; }
    if (!(S.have & (1u << (slot - 1)))) {
        snprintf(msg, sizeof msg, "STATE %d IS EMPTY", slot);
        StNote(msg);
        return;
    }
    StFetch(slot);                  /* the page answers with PortStLoadBlob */
}

ST_EXPORT int PortStLoadBlob(int slot, u8 *buf, u32 len)
{
    if (slot < 1 || slot > PORT_ST_SLOTS || !PortStAllowed()) {
        free(buf);
        return -1;
    }
    free(S.loadBuf);
    S.loadBuf = buf;
    S.loadLen = len;
    S.loadReq = slot;
    return 0;
}

ST_EXPORT void PortStSetRewindHeld(int held)
{
    S.held = held && PortStAllowed();
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

ST_EXPORT void PortStSetPrefs(int enabled, int rewindOn, int fast, int slow, int slot)
{
    Defaults();
    S.enabled = enabled != 0;
    S.rewindOn = rewindOn != 0;
    S.fast = Clamp(fast, 2, 8);
    S.slow = Clamp(slow, 2, 4);
    S.slot = Clamp(slot, 1, PORT_ST_SLOTS);
}

ST_EXPORT void PortStSetHave(u32 mask) { S.have = mask & ((1u << PORT_ST_SLOTS) - 1); }

ST_EXPORT int PortStSlot(void) { Defaults(); return S.slot; }
ST_EXPORT int PortStRewindOn(void) { Defaults(); return S.rewindOn; }
ST_EXPORT int PortStFast(void) { Defaults(); return S.fast; }
ST_EXPORT int PortStSlow(void) { Defaults(); return S.slow; }
ST_EXPORT u32 PortStHave(void) { return S.have; }

static const u8 kFast[] = { 2, 3, 4, 8 };

ST_EXPORT void PortStStepPref(int which, int dir)
{
    int i;

    Defaults();

    switch (which) {
    case 0: S.slot = (S.slot - 1 + dir + PORT_ST_SLOTS) % PORT_ST_SLOTS + 1; break;
    case 1: S.rewindOn = !S.rewindOn; break;
    case 2:
        for (i = 0; i < 4 && kFast[i] != S.fast; i++)
            ;
        S.fast = kFast[((i < 4 ? i : 1) + dir + 4) % 4];
        break;
    case 3: S.slow = (S.slow - 2 + dir + 3) % 3 + 2; break;
    default: return;
    }
    StPrefsChanged();
}

#ifdef __cplusplus
}
#endif
