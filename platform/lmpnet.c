/* Online play = local multiplayer with player 2's pad on another device.
 * Proof of concept, off by default (PortLnStart turns it on).
 *
 * --- the design -------------------------------------------------------------
 *
 * Local multiplayer (platform/localmp.c) runs two humans in ONE simulation
 * with ONE shared camera: no per-device focus player.  So two devices that
 * run that same simulation from the same state with the same per-frame
 * inputs hold byte-identical memory -- everything, VRAM and sound included.
 * Online play is then only a question of agreeing on the inputs:
 *
 *   host   simulates the authoritative timeline and never rewinds.  At the
 *          latch of frame f it seals (its own pad for f, the guest's pad for
 *          f if it has arrived, else the guest's last known pad) plus every
 *          event scheduled for f.
 *   guest  runs a few frames ahead of the host, uses its own pad at once,
 *          predicts the host's as the last sealed one, and when a sealed
 *          frame differs from what it simulated (its own pad overridden
 *          included) restores the nearest keyframe and replays.
 *
 * History is sparse keyframes (one every kfEvery frames, kfCount of them):
 * a rewind replays from the nearest one at or before the first wrong frame.
 * Replay is the game's own loop with the picture off (2.5-6 us a frame); a
 * keyframe is one ~470 KiB copy.  History is never silently exceeded: a
 * sealed frame that differs and is older than the oldest keyframe is counted
 * (historyExceeded) and raises repairWanted, and the caller fetches the
 * host's image.
 *
 * Verification: both sides hash, at the top of every frame, everything a
 * keyframe restores -- the six GBA regions, the port sim block, the port's
 * out-of-map host state (DMA channels, PPU latches) and SRAM -- with no
 * masks.  The guest compares a frame once it is final (every earlier frame
 * sealed and simulated with the sealed inputs).  A mismatch raises
 * repairWanted.  Repair, and joining, are the same thing: the host's image
 * at its frame F, loaded on the guest at its next frame top, then replayed
 * from F to where the guest was with the inputs it holds.
 *
 * --- why this is not platform/rollback.c ----------------------------------
 *
 * rollback.c's machinery is the game's link-play input branch (slot words in
 * gUnk_020382D0, the NETPLAY event, a per-device focus slot), a ring of one
 * snapshot per frame tied to the rollback depth, and "late drop" as the
 * answer to anything older than the ring.  All three are what this design
 * removes.  What is reused is its snapshot (PortRbSnapshotSave/Load: the same
 * bytes, the same safe point at the top of PortPresentFrame) and the latch
 * the local-multiplayer pads already go through (PortLmpSetHostKeys ->
 * platform/inputlog.c -> PortLmpLatch), so player 2's pad reaches the game
 * by exactly the path a second local pad does.
 *
 * --- more than two ------------------------------------------------------------
 *
 * Up to four devices in a star (one host, guests 1..3), each with up to four
 * local seats; the Kirby slots are mapped onto (device, seat) by the host
 * (port/lmpnet.h, "slots").  The sealed record carries every slot's keys;
 * each guest predicts every slot it does not play; the host fills a guest
 * seat's slot from that guest's inputs.  Who has which slot (sOwn) changes
 * only through sealed PORT_LN_EV_HUMAN events, so every device switches on
 * the same frame; it is held in keyframes and images and hashed with the
 * host-state region, so a rewind restores it and a device that disagrees is
 * repaired.  The host's wishes (sWant) reach the timeline through Reconcile,
 * at the latch.
 *
 * Everything here is host memory: session machinery, not simulation state.
 * The simulation state it moves is the snapshot (and the slot table). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "object.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/simblock.h"
#include "port/rollback.h"
#include "port/localmp.h"
#include "port/inputlog.h"
#include "port/lmpnet.h"
#include "port/audio_ear.h"
#include "port/kirby_paint.h"
#include "port/menu_poc.h"
#include "pause_help.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

u64  PortHostMicros(void);
void PortWarp(u8 playerId, u16 room, u8 cellX, u8 cellY);
void PortInputLogPutView(u32 frame, s32 w, s32 h);
void PortInputLogDropViewsAfter(u32 frame);
void PortSramSetRestored(void);
int  PortMenuPoc(void);
void PortMenuPocSet(int on);
u32  PortLnExtrasSize(void);
void PortLnExtrasSave(u8 *dest);
void PortLnExtrasLoad(const u8 *src, u32 len);

int gPortLnOn;

/* --- telemetry: one block of u32, self-describing ---------------------------- */
#define LN_TELEMETRY(X) \
    X(role) X(frame) X(top) X(sealedUpTo) X(verifiedUpTo) X(floor) \
    X(hashed) X(verified) X(mismatches) X(firstMismatch) X(lastMismatch) X(unverifiable) \
    X(rewinds) X(rewindFrames) X(replayedFrames) X(rewindDepthMax) X(rewindsForced) \
    X(depth1) X(depth2) X(depth4) X(depth8) X(depth16) X(depth32) X(depth64) X(depth128) X(depthMore) \
    X(rewindsInput) X(rewindsEvent) \
    X(sealedRecv) X(sealedDup) X(sealedTooOld) X(predictedFrames) X(mispredHost) \
    X(ownOverridden) X(ownOverriddenSim) X(localFrames) \
    X(historyExceeded) X(repairWanted) X(repairFrame) X(imagesSaved) X(imagesLoaded) X(imageJumpFwd) \
    X(kfSaved) X(kfSaveUsMax) X(kfSaveUsSum) X(kfLoadUsMax) X(kfLoadUsSum) X(kfBytes) X(kfCount) X(historyFrames) \
    X(hashUsMax) X(hashUsSum) X(topUsMax) X(catchUpUsMax) X(catchUpUsSum) X(catchUpFramesMax) \
    X(imageSaveUs) X(imageLoadUs) X(imageBytes) \
    X(hostSealed) X(hostKeyOnTime) X(hostKeyLate) X(hostKeyMissing) X(hostKeyLateDiffer) X(hostRewinds) \
    X(events) X(eventsLate) X(eventsApplied) \
    X(soundsLive) X(soundsJudged) X(soundsRefire) X(soundsSpurious) X(soundsMissed) X(soundOverflow) \
    X(sramWrites) X(sramWritesUnsealed) X(sramWritesReplay) \
    X(settingsApplied) X(settingsUnsealed) X(settingsReplay) X(settingsDeviceApplied) X(settingsOtherEdits) \
    X(audioBlocksMuted) X(dumpFrame) X(latches) X(liveFrames) \
    X(self) X(own0) X(own1) X(own2) X(own3) X(managed) \
    X(seatGrants) X(seatReleases) X(seatFull) X(seatAsks) X(reconciles) X(slotEvents) \
    X(d1KeyOnTime) X(d1KeyMissing) X(d2KeyOnTime) X(d2KeyMissing) X(d3KeyOnTime) X(d3KeyMissing) \
    X(mispredOther) X(ownSlots) \
    X(rooms) X(canonFrame) X(canonRuns) X(canonFrames) X(canonUsSum) X(canonUsMax) X(canonSwapUsSum) \
    X(digCompared) X(digEqual) X(digMismatch) X(digFirstMismatch) X(digUpTo) X(persRepairs) X(view)

#define X_FIELD(n) u32 n;
struct LnTelemetry { LN_TELEMETRY(X_FIELD) };
#undef X_FIELD
#define X_NAME(n) #n ","
static const char kTelNames[] = LN_TELEMETRY(X_NAME);
#undef X_NAME

static struct LnTelemetry T;

/* --- regions hashed --------------------------------------------------------- */
enum { RG_EWRAM, RG_IWRAM, RG_IO, RG_PLTT, RG_VRAM, RG_OAM, RG_SIM, RG_DMA, RG_PPU, RG_SRAM, RG_HOST, NRGN };
static const char kRgnNames[] = "ewram,iwram,io,pltt,vram,oam,simblk,dma,ppu,sram,host";

/* --- the timeline ------------------------------------------------------------ */
#define TLN 8192u
#define NONE 0xFFFFFFFFu

enum {
    E_SEALED = 1, E_SIM = 2, E_LOCAL = 4, E_REMOTE = 8, E_HASH = 16, E_RHASH = 32,
    E_VERIFIED = 64, E_MISMATCH = 128, E_ONTIME = 256, E_OVR = 512, E_REPLAYED = 1024,
    E_HEARD = 2048, E_JUDGED = 4096,
};

#define SND_MAX 4

/* Per frame: sk = the sealed keys of slots 0..3, uk = the keys this device
 * simulated with; lk = this device's own seats' keys (lm: which arrived);
 * host: rk = each guest device's seats' keys (rm: bit (dev-1)*4+seat);
 * own = slot ownership as latched (PORT_LN_OWN codes, 0 = CPU). */
struct Ent {
    u32 frame;
    u16 sk[4], uk[4];
    u16 lk[4];
    u16 rk[PORT_LN_MAX_DEV - 1][4];
    u16 rm;
    u8  lm;
    u8  own[4];
    u16 flags;
    u32 evSig, usedEvSig;
    u32 h[2], rh[2];
    u32 heard[SND_MAX], last[SND_MAX];
    u8  nHeard, nLast;
    u8  set[PORT_SIM_LNSET_BYTES];  /* the pause page's session settings at the top */
};

static struct Ent *sTl;
static u32 (*sRgn)[NRGN][2];

static struct Ent *E(u32 f) { return &sTl[f % TLN]; }
static int Have(u32 f) { return sTl[f % TLN].frame == f; }
static struct Ent *Claim(u32 f)
{
    struct Ent *e = E(f);

    if (e->frame != f) {
        memset(e, 0, sizeof *e);
        e->frame = f;
    }
    return e;
}

/* --- events ------------------------------------------------------------------- */
struct Ev { u32 frame; s32 type, a, b, c, d; };
#define MAXEV 4096
static struct Ev sEv[MAXEV];
static int sNumEv;

static void EvPrune(u32 below)
{
    int i, j = 0;

    for (i = 0; i < sNumEv; i++)
        if (sEv[i].frame >= below)
            sEv[j++] = sEv[i];
    sNumEv = j;
}

static int EvAdd(u32 f, int type, int a, int b, int c, int d)
{
    int i;

    if (sNumEv >= MAXEV)
        EvPrune(f > TLN ? f - TLN : 0);
    if (sNumEv >= MAXEV)
        return 0;
    for (i = sNumEv; i > 0 && sEv[i - 1].frame > f; i--)
        sEv[i] = sEv[i - 1];
    sEv[i].frame = f; sEv[i].type = type;
    sEv[i].a = a; sEv[i].b = b; sEv[i].c = c; sEv[i].d = d;
    sNumEv++;
    return 1;
}

static u32 EvSig(u32 f)
{
    u32 h = 2166136261u;
    int i;

    for (i = 0; i < sNumEv; i++) {
        const struct Ev *v = &sEv[i];

        if (v->frame != f)
            continue;
        h = (h ^ (u32)v->type) * 16777619u;
        h = (h ^ (u32)v->a) * 16777619u;
        h = (h ^ (u32)v->b) * 16777619u;
        h = (h ^ (u32)v->c) * 16777619u;
        h = (h ^ (u32)v->d) * 16777619u;
    }
    return h;
}

static int EvCount(u32 f)
{
    int i, n = 0;

    for (i = 0; i < sNumEv; i++)
        n += sEv[i].frame == f;
    return n;
}

/* --- session state ------------------------------------------------------------ */
static int sRole, sFlags, sKfEvery, sKfCount;
static u32 sSnapBytes, sKfBytes;
static u8 *sKf;
static u32 *sKfFrame;
static u8 *sKfValid;
static u32 sTop = NONE;           /* frame of the latest top               */
static u32 sCurLatch = NONE;      /* frame of the latest latch             */
static int sCurLatchReplay;
static u32 sDirty = NONE;         /* guest: first frame simulated wrong    */
static int sDirtyEvent;
static u32 sForce;                /* tests: rewind depth asked for         */
static int sCatch;
static u32 sCatchTo;
static int sRenderWas = 1;
static u64 sCatchT0;
static u32 sCatchFrom;
static u32 sFloor;                /* guest: nothing before this is ours    */
static u32 sSealedUpTo, sVerify;
static u16 sLastLocal[4], sLastSealedK[4];
static u8  sLastSealedOwn[4];
/* Slots (port/lmpnet.h): sOwn is applied ownership -- simulation-side session
 * state, replayed, in keyframes and images, hashed.  The rest is the host's:
 * sWant what it decided, sMem the slot each (device, seat) last had, sRecon
 * the next frame a slot may be reconciled again.  sAsk: a guest's own seat
 * requests waiting for the page. */
static u8  sOwn[4], sWant[4];
static int sManaged, sSelf;
static s8  sMem[PORT_LN_MAX_DEV][4];
static u32 sRecon[4];
static int sAsk[8], sAskN;
static u8  sAskSeats;
static int sForceKf;
static int sSetInit, sSetApplied = -1;

/* Rooms mode on a guest (the canonical shadow, below "rooms mode"): */
static u8 *sCanon, *sCanonSwap;   /* C's state; the live state while C runs */
static u32 sCanonF = NONE;        /* C is parked at this frame top          */
static u32 sCanonDone = NONE;     /* C has hashed every top up to this one  */
static int sCanonRun;             /* C is the live state now               */
static u32 sCanonTo, sCanonP;     /* C runs to this top; the live frame     */
static u64 sCanonT0;
static u32 sDigV = NONE;          /* the next digest to compare            */
static int sPersRepair;           /* the live state is to be rebuilt from C */
static int RoomsGuest(void);
/* The frame the live (personal) timeline is at: while C runs, the frame
 * number in memory is C's. */
static u32 NowF(void) { return sCanonRun ? sCanonP : PortFrameNumber(); }
static void RoomsDevices(void);
static void RoomsSetView(void);
static void CanonAdopt(void);
static int  PersRepair(void);

static u8 *sImg;  static u32 sImgBytes, sImgFrame1;  static int sImgReq;
static u8 *sImgIn; static u32 sImgInLen; static int sImgLoad;
static u8 *sDump; static u32 sDumpAt = NONE, sDumpReady;

/* correction records */
#define CORR_MAX 4096
#define CORR_FIELDS 16
static const char kCorrNames[] =
    "kind,at,target,kf,depth,replayed,us,dx0,dy0,dx1,dy1,room0,room1,sameRoom,top,cause";
static s32 sCorr[CORR_MAX][CORR_FIELDS];
static int sNumCorr;
static s32 sPend[CORR_FIELDS];
static int sPendOpen;

#define MICROS() PortHostMicros()

/* --- hashing --------------------------------------------------------------- */
static void HashWords64(const void *base, u32 bytes, u32 seed, u32 out[2])
{
    const u32 *p = (const u32 *)base;
    u32 n = bytes / 4, i;
    u64 h = 0xCBF29CE484222325ull ^ ((u64)seed << 32);

    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    if (bytes & 3) {
        const u8 *q = (const u8 *)base + n * 4;
        u32 w = 0, k;

        for (k = 0; k < (bytes & 3); k++)
            w |= (u32)q[k] << (8 * k);
        h = (h ^ w) * 0x100000001B3ull;
    }
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    out[0] = (u32)h;
    out[1] = (u32)(h >> 32);
}

static u8 sHostBuf[4096];

static void HashState(u32 f)
{
    static const struct { uintptr_t base; u32 size; } r[6] = {
        { GBA_EWRAM_BASE, GBA_EWRAM_SIZE }, { GBA_IWRAM_BASE, GBA_IWRAM_SIZE },
        { GBA_IO_BASE, GBA_IO_SIZE }, { GBA_PLTT_BASE, GBA_PLTT_SIZE },
        { GBA_VRAM_BASE, GBA_VRAM_SIZE }, { GBA_OAM_BASE, GBA_OAM_SIZE } };
    struct Ent *e = Claim(f);
    u32 (*g)[2] = sRgn[f % TLN];
    u64 t0 = MICROS(), dt, h = 0;
    u32 i, n;

    for (i = 0; i < 6; i++)
        HashWords64((const void *)r[i].base, r[i].size, i, g[i]);
    HashWords64((const void *)(uintptr_t)PORT_SIM_BASE, PortSimUsedBytes(), RG_SIM, g[RG_SIM]);
    n = PortDmaStateSize();
    memset(sHostBuf, 0, sizeof sHostBuf);
    if (n <= sizeof sHostBuf) PortDmaStateSave(sHostBuf);
    HashWords64(sHostBuf, n, RG_DMA, g[RG_DMA]);
    n = PortPpuStateSize();
    memset(sHostBuf, 0, sizeof sHostBuf);
    if (n <= sizeof sHostBuf) PortPpuStateSave(sHostBuf);
    HashWords64(sHostBuf, n, RG_PPU, g[RG_PPU]);
    HashWords64((const void *)(uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE, RG_SRAM, g[RG_SRAM]);
    {
        /* host state the simulation reads back, and the slot ownership */
        static u8 *x;
        u32 xn = PortLnExtrasSize();

        if (x == NULL) x = (u8 *)malloc(xn + 4);
        if (x != NULL) {
            PortLnExtrasSave(x);
            memcpy(x + xn, sOwn, 4);
            HashWords64(x, xn + 4, RG_HOST, g[RG_HOST]);
        }
    }
    for (i = 0; i < NRGN; i++)
        h = (h ^ (((u64)g[i][1] << 32) | g[i][0])) * 0x9E3779B97F4A7C15ull + i;
    h ^= h >> 31;
    e->h[0] = (u32)h;
    e->h[1] = (u32)(h >> 32);
    e->flags |= E_HASH;
    e->flags &= ~(E_VERIFIED | E_MISMATCH);
    dt = MICROS() - t0;
    T.hashed++;
    T.hashUsSum += (u32)dt;
    if (dt > T.hashUsMax) T.hashUsMax = (u32)dt;
}

/* --- keyframes ---------------------------------------------------------------- */
static int KfSram(void) { return !(sFlags & PORT_LN_F_NO_SRAM_KF); }

static u32 KfSramOff(void) { return sSnapBytes; }
static u32 KfExtraOff(void) { return sSnapBytes + (KfSram() ? GBA_SRAM_SIZE : 0); }
static u32 KfOwnOff(void) { return KfExtraOff() + PortLnExtrasSize(); }
#define KF_OWN_BYTES 4u

static void SaveState(u8 *d)
{
    PortRbSnapshotSave(d);
    if (KfSram())
        memcpy(d + KfSramOff(), (const void *)(uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE);
    PortLnExtrasSave(d + KfExtraOff());
    memcpy(d + KfOwnOff(), sOwn, KF_OWN_BYTES);
}

static void LoadState(const u8 *s, int sram)
{
    PortRbSnapshotLoad(s);
    if (sram)
        memcpy((void *)(uintptr_t)GBA_SRAM_BASE, s + KfSramOff(), GBA_SRAM_SIZE);
    PortLnExtrasLoad(s + KfExtraOff(), PortLnExtrasSize());
    memcpy(sOwn, s + KfOwnOff(), KF_OWN_BYTES);
}

static void KfSave(u32 f)
{
    int i, slot = -1;
    u32 oldest = NONE;
    u64 t0, dt;

    if (sKf == NULL)
        return;
    for (i = 0; i < sKfCount; i++)
        if (sKfValid[i] && sKfFrame[i] == f) { slot = i; break; }
    if (slot < 0)
        for (i = 0; i < sKfCount; i++) {
            if (!sKfValid[i]) { slot = i; break; }
            if (sKfFrame[i] < oldest) { oldest = sKfFrame[i]; slot = i; }
        }
    t0 = MICROS();
    SaveState(sKf + (size_t)slot * sKfBytes);
    dt = MICROS() - t0;
    sKfFrame[slot] = f;
    sKfValid[slot] = 1;
    T.kfSaved++;
    T.kfSaveUsSum += (u32)dt;
    if (dt > T.kfSaveUsMax) T.kfSaveUsMax = (u32)dt;
}

static int KfFind(u32 atOrBefore)
{
    int i, best = -1;

    for (i = 0; i < sKfCount; i++)
        if (sKfValid[i] && sKfFrame[i] <= atOrBefore && sKfFrame[i] >= sFloor
            && (best < 0 || sKfFrame[i] > sKfFrame[best]))
            best = i;
    return best;
}

static u32 KfOldest(void)
{
    int i;
    u32 o = NONE;

    for (i = 0; i < sKfCount; i++)
        if (sKfValid[i] && sKfFrame[i] < o)
            o = sKfFrame[i];
    return o;
}

static void KfInvalidateAll(void)
{
    if (sKfValid)
        memset(sKfValid, 0, (size_t)sKfCount);
}

/* --- Kirby positions, for the correction records ----------------------------- */
int PortLnKirby(int i, s32 *out)
{
    const struct Kirby *k;

    if (i < 0 || i > 3)
        return 0;
    k = &gKirbys[i];
    out[0] = k->base.x >> 8;
    out[1] = k->base.y >> 8;
    out[2] = k->base.roomId;
    out[3] = k->hp;
    out[4] = k->animationIndex;
    out[5] = (s32)k->base.flags;
    return 1;
}

/* What the rig watches the host's game do (scenario coverage). */
int PortLnObserve(s32 *out)
{
    u8 p = gUnk_0203AD50;

    out[0] = (gMainFlags & 0x400) ? 1 : 0;
    out[1] = p;
    out[2] = p < 4 ? gPauseMenus[p].menuId : -1;
    out[3] = gNumHumanPlayers;
    out[4] = gPortSim->lmp.humans;
    out[5] = gKirbys[0].battery;
    out[6] = gKirbys[1].battery;
    out[7] = gKirbys[0].lives;
    out[8] = gKirbys[1].lives;
    out[9] = gKirbys[0].ability;
    out[10] = gKirbys[1].ability;
    out[11] = gPortSim->lmp.groupRoom;
    out[12] = gCurLevelInfo[gLocalPlayerId].currentRoom;
    out[13] = gLocalPlayerId;
    out[14] = (s32)gRngVal;
    out[15] = gUnk_0203AD10;
    return 16;
}

/* Tests (R4): the first live enemy (or boss) object in the screen's room,
 * by walking the task table.  out: GBA address, type, x, y.  0 if none. */
int PortLnFindEnemy(s32 *out)
{
    int i;
    uintptr_t best = 0;
    u16 room = gCurLevelInfo[gLocalPlayerId].currentRoom;

    for (i = 0; i < MAX_TASK_NUM; i++) {
        const struct Task *t = &gTasks[i];
        const struct Object *o;
        uintptr_t a;

        if (!t->main || !t->structOffset)
            continue;
        a = (t->flags & TASK_USE_EWRAM) ? (uintptr_t)EWRAM_START + ((uintptr_t)t->structOffset << 2)
                                        : (uintptr_t)IWRAM_START + t->structOffset;
        o = (const struct Object *)a;
        if (o->base.header.kind != 1 || o->type >= 0x5E || o->base.roomId != room)
            continue;
        /* prefer one that gives an ability; else the first */
        if (!o->kirbyAbility && best == 0) { best = a; continue; }
        if (!o->kirbyAbility)
            continue;
        out[0] = (s32)(u32)a;
        out[1] = o->type;
        out[2] = o->base.x >> 8;
        out[3] = o->base.y >> 8;
        out[4] = o->kirbyAbility;
        return 1;
    }
    if (best) {
        const struct Object *o = (const struct Object *)best;

        out[0] = (s32)(u32)best;
        out[1] = o->type;
        out[2] = o->base.x >> 8;
        out[3] = o->base.y >> 8;
        out[4] = 0;
        return 1;
    }
    return 0;
}

/* The correction records' "own" Kirby: this device's first slot (a guest
 * with none yet, or the host: slot 1). */
static int sPendOwn = 1;
static int OwnSlot(void)
{
    int s;

    if (sRole == PORT_LN_GUEST)
        for (s = 1; s < 4; s++)
            if (sOwn[s] && (sOwn[s] - 1) / 4 == sSelf)
                return s;
    return 1;
}

static void PendOpen(int kind, u32 at, u32 target, u32 kf, int cause)
{
    s32 a[6], b[6];

    memset(sPend, 0, sizeof sPend);
    sPendOwn = OwnSlot();
    PortLnKirby(0, a);
    PortLnKirby(sPendOwn, b);
    sPend[0] = kind; sPend[1] = (s32)at; sPend[2] = (s32)target; sPend[3] = (s32)kf;
    sPend[4] = (s32)(at - target); sPend[5] = (s32)(at > kf ? at - kf : 0);
    sPend[7] = a[0]; sPend[8] = a[1]; sPend[9] = b[0]; sPend[10] = b[1];
    sPend[11] = a[2]; sPend[12] = b[2];
    sPend[14] = (s32)sTop;
    sPend[15] = cause;
    sPendOpen = 1;
}

static void PendClose(u64 us)
{
    s32 a[6], b[6];

    if (!sPendOpen)
        return;
    PortLnKirby(0, a);
    PortLnKirby(sPendOwn, b);
    sPend[6] = (s32)us;
    sPend[13] = (a[2] == sPend[11] ? 1 : 0) | (b[2] == sPend[12] ? 2 : 0);
    sPend[7] = a[0] - sPend[7]; sPend[8] = a[1] - sPend[8];
    sPend[9] = b[0] - sPend[9]; sPend[10] = b[1] - sPend[10];
    if (sNumCorr < CORR_MAX)
        memcpy(sCorr[sNumCorr++], sPend, sizeof sPend);
    sPendOpen = 0;
}

/* --- lifecycle ----------------------------------------------------------------- */
int PortLnActive(void) { return gPortLnOn; }
int PortLnRole(void) { return sRole; }
int PortLnCatchingUp(void) { return gPortLnOn && sCatch; }
u32 PortLnLatchFrame(void) { return gPortLnOn ? sCurLatch : NONE; }
u32 PortLnSealedFrontier(void) { return sSealedUpTo; }

void PortLnStop(void)
{
    if (sCatch)
        PortSetRenderEnabled(sRenderWas);
    free(sKf); sKf = NULL;
    free(sKfFrame); sKfFrame = NULL;
    free(sKfValid); sKfValid = NULL;
    free(sTl); sTl = NULL;
    free(sRgn); sRgn = NULL;
    free(sImg); sImg = NULL;
    free(sImgIn); sImgIn = NULL;
    free(sDump); sDump = NULL;
    free(sCanon); sCanon = NULL;
    free(sCanonSwap); sCanonSwap = NULL;
    sCanonRun = 0;
    sCanonF = sCanonDone = sDigV = NONE;
    sPersRepair = 0;
    gPortLnOn = 0;
    sRole = 0;
    sCatch = 0;
    sNumEv = 0;
    sAskN = 0;
    sAskSeats = 0;
    PortLmpSetSessionKeys(NULL);
    PortLnDeviceEnd();
}

static u32 ImageBytes(void);
static void HomeKeepOwnSave(void);

int PortLnStart(int role, int kfEvery, int kfCount, int flags)
{
    u32 f;

    PortLnStop();
    if (role != PORT_LN_HOST && role != PORT_LN_GUEST)
        return 0;
    if (kfEvery < 1) kfEvery = 1;
    if (kfCount < 2) kfCount = 2;
    sRole = role;
    sFlags = flags;
    if (role == PORT_LN_GUEST)
        HomeKeepOwnSave();
    sKfEvery = kfEvery;
    sKfCount = kfCount;
    sSnapBytes = PortRbSnapshotBytes();
    sKfBytes = sSnapBytes + (KfSram() ? GBA_SRAM_SIZE : 0) + PortLnExtrasSize() + KF_OWN_BYTES;
    sTl = (struct Ent *)calloc(TLN, sizeof *sTl);
    sRgn = (u32 (*)[NRGN][2])calloc(TLN, sizeof *sRgn);
    if (sTl == NULL || sRgn == NULL) {
        PortError("[katam-port] lmpnet: no memory for the timeline");
        PortLnStop();
        return 0;
    }
    {
        u32 i;

        for (i = 0; i < TLN; i++)
            sTl[i].frame = NONE;
    }
    if (role == PORT_LN_GUEST || (flags & PORT_LN_F_HOST_KEYS)) {
        sKf = (u8 *)malloc((size_t)sKfBytes * (size_t)kfCount);
        sKfFrame = (u32 *)calloc((size_t)kfCount, sizeof *sKfFrame);
        sKfValid = (u8 *)calloc((size_t)kfCount, 1);
        if (sKf == NULL || sKfFrame == NULL || sKfValid == NULL) {
            PortError("[katam-port] lmpnet: no memory for %d keyframes of %u bytes",
                      kfCount, (unsigned)sKfBytes);
            PortLnStop();
            return 0;
        }
    }
    memset(&T, 0, sizeof T);
    sNumCorr = 0;
    sPendOpen = 0;
    f = PortFrameNumber();
    T.role = (u32)role;
    T.kfBytes = sKfBytes;
    T.kfCount = sKf ? (u32)kfCount : 0;
    T.firstMismatch = NONE;
    T.lastMismatch = NONE;
    T.repairFrame = NONE;
    T.dumpFrame = NONE;
    sFloor = f;
    sSealedUpTo = f;
    sVerify = f;
    sDirty = NONE;
    sForce = 0;
    sTop = NONE;
    sCurLatch = NONE;
    memset(sLastLocal, 0, sizeof sLastLocal);
    memset(sLastSealedK, 0, sizeof sLastSealedK);
    memset(sLastSealedOwn, 0, sizeof sLastSealedOwn);
    /* Slot 0 the host's seat 0; slot 1 device 1's seat 0 until the host
     * manages the slots (port/lmpnet.h). */
    memset(sOwn, 0, sizeof sOwn);
    sOwn[0] = PORT_LN_OWN(0, 0);
    sOwn[1] = PORT_LN_OWN(1, 0);
    memcpy(sWant, sOwn, sizeof sWant);
    sManaged = 0;
    sSelf = role == PORT_LN_HOST ? 0 : 1;
    memset(sMem, -1, sizeof sMem);
    memset(sRecon, 0, sizeof sRecon);
    sAskN = 0;
    sAskSeats = 0;
    sForceKf = 1;
    sSetInit = 0;
    sSetApplied = -1;
    sNumEv = 0;
    sImgReq = sImgLoad = 0;
    sImgFrame1 = 0;
    sDumpAt = NONE; sDumpReady = 0;
    /* Every view request on this device becomes a request to the session
     * (platform/view.c); only sealed VIEW events reach the game. */
    PortSimViewUseSession();
    /* What about this device must not reach the session (sound, the view
     * experiment, its stored save): platform/lnsession.c. */
    PortLnDeviceBegin(role);
    gPortLnOn = 1;
    PortLog("[katam-port] lmpnet: %s from frame %u, keyframe every %d x %d (%u KiB each)",
            role == PORT_LN_HOST ? "host" : "guest", (unsigned)f, kfEvery,
            sKf ? kfCount : 0, (unsigned)(sKfBytes / 1024));
    return 1;
}

/* --- inputs ------------------------------------------------------------------- */
void PortLnLocalInputSeat(u32 f, int seat, u16 keys)
{
    struct Ent *e;

    if (!gPortLnOn || seat < 0 || seat > 3)
        return;
    e = Claim(f);
    e->lk[seat] = keys & 0x03FF;
    if (seat == 0 && !(e->lm & 1))
        T.localFrames++;
    e->lm |= (u8)(1u << seat);
}

void PortLnLocalInput(u32 f, u16 keys) { PortLnLocalInputSeat(f, 0, keys); }

void PortLnRemoteInputSeat(u32 f, int dev, int seat, u16 keys)
{
    struct Ent *e;
    u32 next = NowF();
    u16 bit;
    int s;

    if (!gPortLnOn || sRole != PORT_LN_HOST || dev < 1 || dev >= PORT_LN_MAX_DEV || seat < 0 || seat > 3)
        return;
    keys &= 0x03FF;
    if (f + TLN / 2 < next)
        return;                                  /* ancient: ignore      */
    e = Claim(f);
    bit = (u16)(1u << ((dev - 1) * 4 + seat));
    if (e->rm & bit)
        return;                                  /* duplicate            */
    e->rk[dev - 1][seat] = keys;
    e->rm |= bit;
    if (f < next && (e->flags & E_SEALED)) {
        T.hostKeyLate++;
        for (s = 0; s < 4; s++)
            if (e->own[s] == PORT_LN_OWN(dev, seat) && e->sk[s] != keys) {
                T.hostKeyLateDiffer++;
                e->flags |= E_OVR;
            }
    }
}

void PortLnRemoteInput(u32 f, u16 keys) { PortLnRemoteInputSeat(f, 1, 0, keys); }

/* What the host knows of a guest seat's pad for frame f: the key for f, else
 * the newest one it has for an earlier frame, else what it last sealed for
 * that slot if the same seat had it. */
static u16 HostGuestKey(u32 f, int slot, int dev, int seat, int *ontime)
{
    u16 bit = (u16)(1u << ((dev - 1) * 4 + seat));
    u32 g;

    *ontime = 0;
    if (Have(f) && (E(f)->rm & bit)) {
        *ontime = 1;
        return E(f)->rk[dev - 1][seat];
    }
    for (g = f; g-- > 0 && g + 120 > f;)
        if (Have(g) && (E(g)->rm & bit))
            return E(g)->rk[dev - 1][seat];
    return sLastSealedOwn[slot] == PORT_LN_OWN(dev, seat) ? sLastSealedK[slot] : 0;
}

/* A guest's prediction of a slot it does not play: as last sealed. */
static u16 PredictSlot(u32 f, int slot)
{
    u32 g;

    for (g = f; g-- > 0 && g + 240 > f;)
        if (Have(g) && (E(g)->flags & E_SEALED))
            return E(g)->sk[slot];
    return sLastSealedK[slot];
}

/* --- slots ------------------------------------------------------------------------ */
static int OwnDev(u8 o) { return o ? (o - 1) / 4 : -1; }
static int OwnSeat(u8 o) { return o ? (o - 1) % 4 : -1; }

static void Manage(void)
{
    if (sManaged)
        return;
    /* From here on the host's table is the truth: slot 1 is nobody's until
     * someone asks (port/lmpnet.h). */
    sManaged = 1;
    memset(sWant, 0, sizeof sWant);
    sWant[0] = PORT_LN_OWN(0, 0);
    T.managed = 1;
}

int PortLnSeatRequest(int dev, int seat, int on)
{
    int s, pick = -1;
    u8 o;

    if (!gPortLnOn || sRole != PORT_LN_HOST || dev < 0 || dev >= PORT_LN_MAX_DEV || seat < 0 || seat > 3)
        return -1;
    Manage();
    o = (u8)PORT_LN_OWN(dev, seat);
    if (o == PORT_LN_OWN(0, 0))
        return on ? 0 : -1;                  /* the host's own player is slot 0 */
    for (s = 1; s < 4; s++)
        if (sWant[s] == o)
            pick = s;
    if (!on) {
        if (pick < 0)
            return -1;
        sWant[pick] = 0;
        sRecon[pick] = 0;
        T.seatReleases++;
        PortLog("[katam-port] lmpnet: slot %d (device %d seat %d) back to the CPU", pick, dev, seat);
        return pick;
    }
    if (pick >= 0)
        return pick;
    s = sMem[dev][seat];
    if (s >= 1 && s < 4 && !sWant[s])
        pick = s;
    for (s = 1; s < 4 && pick < 0; s++)
        if (!sWant[s])
            pick = s;
    if (pick < 0) {
        T.seatFull++;
        PortLog("[katam-port] lmpnet: device %d seat %d refused -- all four Kirbys are people", dev, seat);
        return -1;
    }
    sWant[pick] = o;
    sMem[dev][seat] = (s8)pick;
    sRecon[pick] = 0;
    T.seatGrants++;
    PortLog("[katam-port] lmpnet: slot %d to device %d seat %d", pick, dev, seat);
    return pick;
}

void PortLnDeviceGone(int dev)
{
    int s;

    if (!gPortLnOn || sRole != PORT_LN_HOST || dev < 1 || dev >= PORT_LN_MAX_DEV)
        return;
    Manage();
    for (s = 1; s < 4; s++)
        if (OwnDev(sWant[s]) == dev) {
            sWant[s] = 0;
            sRecon[s] = 0;
            T.seatReleases++;
            PortLog("[katam-port] lmpnet: device %d gone; slot %d to the CPU", dev, s);
        }
}

int PortLnFreeSlots(void)
{
    int s, n = 0;

    if (!gPortLnOn || sRole != PORT_LN_HOST)
        return 0;
    for (s = 1; s < 4; s++)
        n += sManaged ? !sWant[s] : 0;
    return n;
}

int PortLnSlotWant(int slot)
{
    if (!gPortLnOn || slot < 0 || slot > 3 || !sWant[slot])
        return -1;
    return sWant[slot] - 1;
}

int PortLnSlotOwner(int slot)
{
    if (!gPortLnOn || slot < 0 || slot > 3 || !sOwn[slot])
        return -1;
    return sOwn[slot] - 1;
}

int PortLnSeatSlot(int dev, int seat)
{
    int s;

    if (!gPortLnOn)
        return -1;
    for (s = 0; s < 4; s++)
        if (sOwn[s] && sOwn[s] == PORT_LN_OWN(dev, seat))
            return s;
    return -1;
}

void PortLnSetSelf(int dev)
{
    if (gPortLnOn && sRole == PORT_LN_GUEST && dev >= 1 && dev < PORT_LN_MAX_DEV)
        sSelf = dev;
    T.self = (u32)sSelf;
}

int PortLnSelf(void) { return sSelf; }

int PortLnSelfSeats(void)
{
    int s, m = 1;

    for (s = 0; s < 4; s++)
        if (sOwn[s] && OwnDev(sOwn[s]) == sSelf)
            m |= 1 << OwnSeat(sOwn[s]);
    return m | sAskSeats;
}

int PortLnSelfSlots(void)
{
    int s, m = 0;

    if (!gPortLnOn)
        return 0;
    for (s = 0; s < 4; s++)
        if (sOwn[s] && OwnDev(sOwn[s]) == sSelf)
            m |= 1 << s;
    return m;
}

int PortLnSeatAsk(int seat, int on)
{
    if (!gPortLnOn || seat < 0 || seat > 3)
        return 0;
    T.seatAsks++;
    if (sRole == PORT_LN_HOST)
        return PortLnSeatRequest(0, seat, on) >= 0;
    if (on) sAskSeats |= (u8)(1u << seat); else sAskSeats &= (u8)~(1u << seat);
    if (sAskN < 8)
        sAsk[sAskN++] = 1 + seat + 4 * (on != 0);
    return 1;
}

int PortLnSeatAskPoll(void)
{
    int v, i;

    if (!gPortLnOn || sAskN == 0)
        return 0;
    v = sAsk[0];
    for (i = 1; i < sAskN; i++)
        sAsk[i - 1] = sAsk[i];
    sAskN--;
    return v;
}

/* host, at the latch of f: seal whatever brings the simulation and the
 * applied ownership to what the host decided.  Also mends a slot the game
 * itself changed (a start that made a person of a slot nobody has: HOST
 * GAME > START is a two-player start). */
static void Reconcile(u32 f)
{
    int s;

    if (!sManaged)
        return;
    for (s = 1; s < 4; s++) {
        int want = sWant[s] != 0;

        if (sWant[s] == sOwn[s] && PortLmpSlotPerson(s) == want)
            continue;
        if (f < sRecon[s])
            continue;
        sRecon[s] = f + 30;
        if (sWant[s] != sOwn[s]) T.slotEvents++; else T.reconciles++;
        EvAdd(f, PORT_LN_EV_HUMAN, s, want, sWant[s], 0);
        T.events++;
    }
}

int PortLnEvent(int type, int a, int b, int c, int d)
{
    u32 f;

    if (!gPortLnOn || sRole != PORT_LN_HOST)
        return -1;
    f = NowF();                  /* the next latch */
    if (Have(f) && (E(f)->flags & E_SEALED))
        f++;
    if (!EvAdd(f, type, a, b, c, d))
        return -1;
    T.events++;
    return (int)f;
}

int PortLnSealedGet(u32 f, u32 *out)
{
    struct Ent *e;

    if (!gPortLnOn || !Have(f))
        return 0;
    e = E(f);
    if (!(e->flags & E_SEALED))
        return 0;
    out[0] = e->sk[0];
    out[1] = e->sk[1];
    out[2] = (u32)EvCount(f);
    out[3] = (e->flags & E_ONTIME) ? 1 : 0;
    out[4] = (e->flags & E_HASH) ? e->h[0] : 0;
    out[5] = (e->flags & E_HASH) ? e->h[1] : 0;
    out[6] = (e->flags & E_HASH) ? 1 : 0;
    out[7] = e->sk[2];
    out[8] = e->sk[3];
    return 1;
}

int PortLnSealedEventGet(u32 f, int idx, s32 *out)
{
    int i, n = 0;

    for (i = 0; i < sNumEv; i++) {
        if (sEv[i].frame != f)
            continue;
        if (n++ == idx) {
            out[0] = sEv[i].type; out[1] = sEv[i].a; out[2] = sEv[i].b;
            out[3] = sEv[i].c; out[4] = sEv[i].d;
            return 1;
        }
    }
    return 0;
}

/* Events of a sealed frame arrive before its record; a duplicate record
 * (redundancy) brings its events again, which must not double them. */
static u32 sEvStageFrame = NONE;
static struct Ev sEvStage[16];
static int sEvStageN;

void PortLnSealedEvent(u32 f, int type, int a, int b, int c, int d)
{
    if (!gPortLnOn || sRole != PORT_LN_GUEST)
        return;
    if (sEvStageFrame != f) {
        sEvStageFrame = f;
        sEvStageN = 0;
    }
    if (sEvStageN < 16) {
        struct Ev *v = &sEvStage[sEvStageN++];

        v->frame = f; v->type = type; v->a = a; v->b = b; v->c = c; v->d = d;
    }
}

static void AdvanceSealed(void)
{
    while (Have(sSealedUpTo) && (E(sSealedUpTo)->flags & E_SEALED))
        sSealedUpTo++;
}

static void MarkDirty(u32 f, int event)
{
    if (sDirty == NONE || f < sDirty) {
        sDirty = f;
        sDirtyEvent = event;
    }
}

void PortLnSealed4(u32 f, u16 k0, u16 k1, u16 k2, u16 k3, int nev)
{
    struct Ent *e;
    u32 next = NowF();
    u16 k[4];
    int i;

    if (!gPortLnOn || sRole != PORT_LN_GUEST)
        return;
    if (f < sFloor) {
        T.sealedDup++;
        sEvStageFrame = NONE;
        return;
    }
    if (f + TLN - 64 < next) {
        /* So old the timeline no longer holds what was simulated there. */
        T.sealedTooOld++;
        T.historyExceeded++;
        T.repairWanted = 1;
        if (T.repairFrame == NONE) T.repairFrame = f;
        sEvStageFrame = NONE;
        return;
    }
    e = Claim(f);
    if (e->flags & E_SEALED) {
        T.sealedDup++;
        sEvStageFrame = NONE;
        return;
    }
    T.sealedRecv++;
    if (nev > 0 && sEvStageFrame == f)
        for (i = 0; i < sEvStageN && i < nev; i++)
            EvAdd(f, sEvStage[i].type, sEvStage[i].a, sEvStage[i].b, sEvStage[i].c, sEvStage[i].d);
    sEvStageFrame = NONE;
    k[0] = k0; k[1] = k1; k[2] = k2; k[3] = k3;
    for (i = 0; i < 4; i++)
        e->sk[i] = k[i] & 0x03FF;
    e->flags |= E_SEALED;
    e->evSig = EvSig(f);
    /* own keys the host overrode (late or lost), by the ownership this
     * device latched the frame with (or holds now, if not simulated yet) */
    {
        const u8 *own = (e->flags & E_SIM) ? e->own : sOwn;
        int over = 0;

        for (i = 0; i < 4; i++) {
            int seat = OwnSeat(own[i]);

            if (own[i] && OwnDev(own[i]) == sSelf && (e->lm & (1u << seat)) && e->lk[seat] != e->sk[i])
                over = 1;
        }
        if (over)
            T.ownOverridden++;
    }
    if (f < next && f >= sFloor && (e->flags & E_SIM)) {
        int keys = 0, evs = e->usedEvSig != e->evSig, mine = 0, other = 0;

        for (i = 0; i < 4; i++)
            if (e->uk[i] != e->sk[i]) {
                keys = 1;
                if (e->own[i] && OwnDev(e->own[i]) == sSelf) mine = 1;
                else if (i == 0) other |= 1;
                else other |= 2;
            }
        if (other & 1) T.mispredHost++;
        if (other & 2) T.mispredOther++;
        if (mine) T.ownOverriddenSim++;
        if (evs) T.eventsLate++;
        if (keys || evs)
            MarkDirty(f, evs && !keys);
    }
    AdvanceSealed();
    if (sSealedUpTo > 0 && Have(sSealedUpTo - 1))
        for (i = 0; i < 4; i++)
            sLastSealedK[i] = E(sSealedUpTo - 1)->sk[i];
    T.sealedUpTo = sSealedUpTo;
}

void PortLnSealed(u32 f, u16 k0, u16 k1, int nev) { PortLnSealed4(f, k0, k1, 0, 0, nev); }

void PortLnRemoteHash(u32 f, u32 lo, u32 hi)
{
    struct Ent *e;

    if (!gPortLnOn || sRole != PORT_LN_GUEST || f < sFloor)
        return;
    if (f + TLN - 64 < NowF())
        return;
    e = Claim(f);
    e->rh[0] = lo;
    e->rh[1] = hi;
    e->flags |= E_RHASH;
}

int PortLnHashFinal(u32 f)
{
    if (!gPortLnOn || !Have(f) || !(E(f)->flags & E_HASH))
        return 0;
    if (sRole == PORT_LN_HOST)
        return sTop != NONE && f <= sTop;
    if (RoomsGuest())
        /* the canonical shadow's: it runs sealed frames only, once */
        return !sCanonRun && sCanonDone != NONE && f <= sCanonDone && f >= sFloor
            && !(T.repairWanted && T.repairFrame != NONE && f > T.repairFrame);
    if (sCatch || sTop == NONE || f > sTop || f > sSealedUpTo || f < sFloor)
        return 0;
    /* Known wrong (a mismatch, or history exceeded): nothing after it is
     * final until the repair's image lands. */
    if (T.repairWanted && T.repairFrame != NONE && f > T.repairFrame)
        return 0;
    return sDirty == NONE || f <= sDirty;
}

int PortLnHashAt(u32 f, u32 *out)
{
    int i;

    if (!gPortLnOn || !Have(f) || !(E(f)->flags & E_HASH))
        return 0;
    out[0] = E(f)->h[0];
    out[1] = E(f)->h[1];
    for (i = 0; i < NRGN; i++) {
        out[2 + 2 * i] = sRgn[f % TLN][i][0];
        out[3 + 2 * i] = sRgn[f % TLN][i][1];
    }
    return 1 + PortLnHashFinal(f);
}

int PortLnRegionCount(void) { return NRGN; }
const char *PortLnRegionNames(void) { return kRgnNames; }

/* --- sounds, SRAM, settings: side effects that leave the image ---------------- */
static int Unsealed(void)
{
    return sRole == PORT_LN_GUEST && sCurLatch != NONE && sCurLatch >= sSealedUpTo;
}

void PortLnNoteSoundSlow(u32 id, const void *mplayInfo)
{
    struct Ent *e;

    if (sCanonRun)
        return;             /* the canonical shadow is heard by nobody */
    /* The guest's speakers (platform/audio_ear.c): which player, which slot. */
    PortEarNoteStart(sCurLatch, mplayInfo, id);
    if (sCurLatch == NONE || !Have(sCurLatch))
        return;
    e = E(sCurLatch);
    if (e->nLast < SND_MAX)
        e->last[e->nLast++] = id;
    else
        T.soundOverflow++;
}

void PortLnNoteSramWriteSlow(u32 bytes)
{
    (void)bytes;
    if (sCanonRun)
        return;
    T.sramWrites++;
    if (sCatch)
        T.sramWritesReplay++;
    else if (Unsealed())
        T.sramWritesUnsealed++;
}

void PortLnNoteSettingSlow(int row)
{
    (void)row;
    if (sCanonRun)
        return;
    T.settingsApplied++;
    if (sCatch)
        T.settingsReplay++;
    else if (Unsealed())
        T.settingsUnsealed++;
}

/* platform/m4a_mixer.c, as SoundMain renders: will the block it renders now
 * be played?  It is handed over at the next present's VBlank, so it is muted
 * when that present is still a replayed one (the last replayed frame's
 * block is the first live one heard).  Output only: the mixer advances the
 * same game state either way. */
int PortLnAudioMuted(void)
{
    if (sCanonRun)
        return 1;
    return gPortLnOn && sCatch && !(sFlags & PORT_LN_F_NO_AUDIO_CU)
        && PortFrameNumber() < sCatchTo;
}

int PortLnMuteAudio(void)
{
    if (sCanonRun)
        return 1;
    if (!gPortLnOn || !sCatch || (sFlags & PORT_LN_F_NO_AUDIO_CU))
        return 0;
    T.audioBlocksMuted++;
    return 1;
}

/* The latched frame's sounds are complete at the next frame top. */
static void CloseSounds(void)
{
    struct Ent *e;

    if (sCurLatch == NONE || !Have(sCurLatch))
        return;
    e = E(sCurLatch);
    if (!sCurLatchReplay && !(e->flags & E_HEARD)) {
        memcpy(e->heard, e->last, sizeof e->heard);
        e->nHeard = e->nLast;
        e->flags |= E_HEARD;
        T.soundsLive += e->nLast;
    }
}

static int SndIn(const u32 *a, int n, u32 v)
{
    int i;

    for (i = 0; i < n; i++)
        if (a[i] == v)
            return 1;
    return 0;
}

/* Once a frame is final: what was heard live against what the final pass
 * started.  A replayed frame's sounds would play again if replayed audio
 * were not muted; the others were mispredicted. */
static void JudgeSounds(struct Ent *e)
{
    int i;

    if (e->flags & E_JUDGED)
        return;
    e->flags |= E_JUDGED;
    PortEarFinal(e->frame);
    T.soundsJudged += e->nLast;
    if (!(e->flags & E_REPLAYED))
        return;
    for (i = 0; i < e->nHeard; i++) {
        if (SndIn(e->last, e->nLast, e->heard[i])) T.soundsRefire++;
        else T.soundsSpurious++;
    }
    for (i = 0; i < e->nLast; i++)
        if (!SndIn(e->heard, e->nHeard, e->last[i])) T.soundsMissed++;
}

/* --- the latch ------------------------------------------------------------------ */
static void ApplyEvents(u32 f)
{
    int i;

    for (i = 0; i < sNumEv; i++) {
        const struct Ev *v = &sEv[i];

        if (v->frame != f)
            continue;
        T.eventsApplied++;
        switch (v->type) {
        case PORT_LN_EV_WARP:
            PortWarp((u8)v->a, (u16)v->b, (u8)v->c, (u8)v->d);
            break;
        case PORT_LN_EV_VIEW:
            PortInputLogPutView(f, v->a, v->b);
            break;
        case PORT_LN_EV_LMP:
            PortLmpSetHumans(v->a);
            break;
        case PORT_LN_EV_HUMAN:
            /* who plays it first (the keys below follow), then the game */
            if (v->a >= 1 && v->a <= 3) {
                if (!v->b)
                    sOwn[v->a] = 0;
                else if (v->c > 0 && v->c <= PORT_LN_OWN(PORT_LN_MAX_DEV - 1, 3))
                    sOwn[v->a] = (u8)v->c;
            }
            RoomsDevices();
            PortLmpSetSlotHuman(v->a, v->b);
            break;
        case PORT_LN_EV_PAINT:
            PortPaintSetSlot(v->a, (u32)v->b);
            break;
        case PORT_LN_EV_ROOMS:
            /* the session's mode, the host's choice (localmp.c rooms mode) */
            PortLmpRoomsSealed(v->a);
            RoomsDevices();
            break;
        case PORT_LN_EV_POKE:
            if ((u32)v->a >= GBA_EWRAM_BASE && (u32)v->a < GBA_EWRAM_BASE + GBA_EWRAM_SIZE)
                *(u8 *)(uintptr_t)(u32)v->a = (u8)v->b;
            break;
        default:
            break;
        }
    }
}

int PortLnKeyOverride(u16 *keys)
{
    u32 f;
    struct Ent *e;
    u16 k[4];
    int s;

    if (!gPortLnOn)
        return 0;
    f = PortFrameNumber();
    if (sCanonRun) {
        /* The canonical shadow (rooms mode): the sealed frame, nothing of
         * this device's -- no prediction, no records, no speakers. */
        ApplyEvents(f);
        if (Have(f) && (E(f)->flags & E_SEALED))
            memcpy(k, E(f)->sk, sizeof k);
        else
            memset(k, 0, sizeof k);
        *keys = k[0];
        PortLmpSetSessionKeys(k);
        return 1;
    }
    e = Claim(f);
    T.latches++;
    /* This frame's events first: a slot changing hands takes this frame's
     * keys from its new owner (as a recording's `human` line does). */
    if (sRole == PORT_LN_HOST)
        Reconcile(f);
    ApplyEvents(f);
    memcpy(e->own, sOwn, sizeof e->own);
    if (sRole == PORT_LN_HOST) {
        int ontime1 = 0;

        for (s = 0; s < 4; s++) {
            u8 o = sOwn[s];
            int dev = OwnDev(o), seat = OwnSeat(o);

            if (!o) {
                k[s] = 0;
            } else if (dev == 0) {
                k[s] = (e->lm & (1u << seat)) ? e->lk[seat] : sLastLocal[seat];
            } else {
                int ontime;

                k[s] = HostGuestKey(f, s, dev, seat, &ontime);
                if (dev == 1 && seat == 0) ontime1 = ontime;
                if (ontime) { T.hostKeyOnTime++; if (dev == 1) T.d1KeyOnTime++; else if (dev == 2) T.d2KeyOnTime++; else T.d3KeyOnTime++; }
                else { T.hostKeyMissing++; if (dev == 1) T.d1KeyMissing++; else if (dev == 2) T.d2KeyMissing++; else T.d3KeyMissing++; }
            }
            e->sk[s] = k[s];
            sLastSealedK[s] = k[s];
            sLastSealedOwn[s] = o;
        }
        e->flags |= E_SEALED | (ontime1 ? E_ONTIME : 0);
        e->evSig = EvSig(f);
        T.hostSealed++;
        sSealedUpTo = f + 1;
        T.sealedUpTo = sSealedUpTo;
    } else if (e->flags & E_SEALED) {
        memcpy(k, e->sk, sizeof k);
    } else {
        for (s = 0; s < 4; s++) {
            u8 o = sOwn[s];
            int seat = OwnSeat(o);

            if (!o)
                k[s] = 0;
            else if (OwnDev(o) == sSelf)
                k[s] = (e->lm & (1u << seat)) ? e->lk[seat] : sLastLocal[seat];
            else
                k[s] = PredictSlot(f, s);
        }
        T.predictedFrames++;
    }
    for (s = 0; s < 4; s++)
        if (e->lm & (1u << s))
            sLastLocal[s] = e->lk[s];
    if (sCatch)
        e->flags |= E_REPLAYED;
    else
        T.liveFrames++;
    memcpy(e->uk, k, sizeof k);
    if (RoomsGuest())
        RoomsSetView();
    e->usedEvSig = EvSig(f);
    e->flags |= E_SIM;
    e->nLast = 0;
    sCurLatch = f;
    sCurLatchReplay = sCatch;
    PortEarLatch(f);
    *keys = k[0];
    PortLmpSetSessionKeys(k);
    {
        int n = 0;

        for (s = 0; s < 4; s++) {
            (&T.own0)[s] = sOwn[s] ? sOwn[s] - 1u : 0xFFFFFFFFu;
            n += sOwn[s] != 0;
        }
        T.ownSlots = (u32)n;
        T.self = (u32)sSelf;
    }
    T.frame = f;
    return 1;
}

/* --- images ------------------------------------------------------------------- */
#define IMG_MAGIC 0x494E4C4Bu       /* 'KLNI' */
#define IMG_HDR   64u

static u32 ImageBytes(void)
{
    return IMG_HDR + sSnapBytes + GBA_SRAM_SIZE + PortLnExtrasSize();
}

static void Put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 Get32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void SaveImage(u8 *d, u32 f)
{
    s32 w, h;
    u32 x = PortLnExtrasSize();

    memset(d, 0, IMG_HDR);
    Put32(d + 0, IMG_MAGIC);
    Put32(d + 4, 2);
    Put32(d + 8, f);
    Put32(d + 12, sSnapBytes);
    Put32(d + 16, (u32)PortLmpSetting());
    PortInputLogViewAt(f > 0 ? f - 1 : 0, &w, &h);
    Put32(d + 20, (u32)w); Put32(d + 24, (u32)h);
    PortInputLogViewAt(f, &w, &h);
    Put32(d + 28, (u32)w); Put32(d + 32, (u32)h);
    Put32(d + 36, (u32)PortMenuPoc());
    Put32(d + 40, x);
    Put32(d + 44, gPortRandoFlags);
    Put32(d + 48, gPortRandoSeed);
    memcpy(d + 52, sOwn, 4);         /* version 2: the slot ownership */
    PortRbSnapshotSave(d + IMG_HDR);
    memcpy(d + IMG_HDR + sSnapBytes, (const void *)(uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE);
    PortLnExtrasSave(d + IMG_HDR + sSnapBytes + GBA_SRAM_SIZE);
}

int PortLnImageRequest(void)
{
    if (!gPortLnOn)
        return 0;
    sImgReq = 1;
    sImgFrame1 = 0;
    return 1;
}

u32 PortLnImageReady(void) { return sImgFrame1; }
u8 *PortLnImagePtr(void) { return sImg; }
u32 PortLnImageBytes(void) { return gPortLnOn ? ImageBytes() : 0; }

u8 *PortLnImageInBuffer(void)
{
    if (!gPortLnOn)
        return NULL;
    if (sImgIn == NULL)
        sImgIn = (u8 *)malloc(ImageBytes());
    return sImgIn;
}

int PortLnImageLoad(u32 len)
{
    if (!gPortLnOn || sImgIn == NULL || len < IMG_HDR + sSnapBytes || Get32(sImgIn) != IMG_MAGIC
        || Get32(sImgIn + 12) != sSnapBytes)
        return 0;
    sImgInLen = len;
    sImgLoad = 1;
    return 1;
}

static int sImgSkip;

/* Tests (R5): leave items out of an image load, to show each is needed. */
void PortLnSetImageSkip(int mask) { sImgSkip = mask; }

static void ApplyImage(void)
{
    const u8 *s = sImgIn;
    u32 old = PortFrameNumber(), F = Get32(s + 8), f, x;
    u64 t0 = MICROS();
    /* A join: the first image of this session.  Nothing this device ran
     * before it belongs to the session -- in a browser the guest has been
     * running its own front end, its frame counter may be ahead of the
     * host's, and its view list holds its own frames -- so the session
     * starts at F: no catch-up from F to where the guest was, and nothing
     * of its own past F. */
    int join = T.imagesLoaded == 0;

    sImgLoad = 0;
    if (join)
        PortInputLogDropViewsAfter(F);
    PendOpen(2, old, F, F, 0);
    /* What has to travel besides memory (R5): the local-multiplayer switch,
     * the session view (a frame-keyed event list outside the image), the
     * save memory (and that the host's save is the one now held). */
    if (!(sImgSkip & PORT_LN_SKIP_LMP) && (int)Get32(s + 16) != PortLmpSetting())
        PortLmpSetHumans((int)Get32(s + 16));
    if (!(sImgSkip & PORT_LN_SKIP_MENUS) && (int)Get32(s + 36) != PortMenuPoc())
        PortMenuPocSet((int)Get32(s + 36));
    /* The randomizer is read by the simulation (rando.c): the host's. */
    if (!(sImgSkip & PORT_LN_SKIP_RANDO))
        PortRandoSessionSet(Get32(s + 44), Get32(s + 48));
    PortRbSnapshotLoad(s + IMG_HDR);
    if (!(sImgSkip & PORT_LN_SKIP_OWN) && Get32(s + 4) >= 2)
        memcpy(sOwn, s + 52, 4);
    if (!(sImgSkip & PORT_LN_SKIP_SRAM))
        memcpy((void *)(uintptr_t)GBA_SRAM_BASE, s + IMG_HDR + sSnapBytes, GBA_SRAM_SIZE);
    if (!(sImgSkip & PORT_LN_SKIP_SRAMLOAD))
        PortSramSetRestored();
    if (!(sImgSkip & PORT_LN_SKIP_VIEW)) {
        if (F > 0)
            PortInputLogPutView(F - 1, (s32)Get32(s + 20), (s32)Get32(s + 24));
        PortInputLogPutView(F, (s32)Get32(s + 28), (s32)Get32(s + 32));
    }
    x = Get32(s + 40);
    if (!(sImgSkip & PORT_LN_SKIP_HOST) && x && sImgInLen >= IMG_HDR + sSnapBytes + GBA_SRAM_SIZE + x) {
        PortLnExtrasLoad(s + IMG_HDR + sSnapBytes + GBA_SRAM_SIZE, x);
        PortPocHostStateRebuild();
    }
    T.imageLoadUs = (u32)(MICROS() - t0);
    T.imagesLoaded++;
    KfInvalidateAll();
    sFloor = F;
    sDirty = NONE;
    sForce = 0;
    sSetApplied = -1;
    T.repairWanted = 0;
    T.repairFrame = NONE;
    for (f = F; f < old && f < F + TLN; f++)
        if (Have(f))
            E(f)->flags &= ~(E_VERIFIED | E_MISMATCH | E_HASH);
    if (sSealedUpTo < F || join)
        sSealedUpTo = F;
    AdvanceSealed();
    if (sVerify < F || join)
        sVerify = F;
    sForceKf = 1;
    /* Rooms mode: the image is the canonical shadow's as well (it shows
     * player 1, as the host); the live state re-views at its next latch. */
    CanonAdopt();
    /* The guest's speakers: a replay F..old to reconcile, or a fresh start. */
    PortEarReplay(F, old > F && !join ? old : F);
    if (old > F && !join) {
        sCatchTo = old;
        sCatchFrom = F;
        if (!sCatch) sRenderWas = PortRenderEnabled();
        sCatch = 1;
        sCatchT0 = t0;
        PortSetRenderEnabled(0);
    } else {
        T.imageJumpFwd++;
        PendClose(MICROS() - t0);
    }
}

/* --- rewinding ------------------------------------------------------------------ */
static void Rewind(u32 target, int cause)
{
    u32 at = PortFrameNumber(), depth, kfF;
    int k = KfFind(target);
    u64 t0;

    if (sRole != PORT_LN_GUEST) {
        T.hostRewinds++;          /* never: the host is the authority */
        return;
    }
    if (k < 0 && RoomsGuest() && sCanonF != NONE && sCanonF <= target && PersRepair())
        return;             /* rooms mode: the canonical shadow is a keyframe */
    if (k < 0) {
        /* Older than every keyframe: cannot be replayed.  Never dropped
         * silently -- the caller fetches the host's image. */
        T.historyExceeded++;
        T.repairWanted = 1;
        if (T.repairFrame == NONE) T.repairFrame = target;
        return;
    }
    kfF = sKfFrame[k];
    t0 = MICROS();
    PendOpen(1, at, target, kfF, cause);
    /* The guest's speakers: kfF..at is about to be replayed unheard. */
    if (at > kfF)
        PortEarReplay(kfF, at);
    LoadState(sKf + (size_t)k * sKfBytes, KfSram());
    {
        u64 dt = MICROS() - t0;

        T.kfLoadUsSum += (u32)dt;
        if (dt > T.kfLoadUsMax) T.kfLoadUsMax = (u32)dt;
    }
    depth = at - target;
    T.rewinds++;
    T.rewindFrames += depth;
    T.replayedFrames += at - kfF;
    if (depth > T.rewindDepthMax) T.rewindDepthMax = depth;
    if (depth <= 1) T.depth1++; else if (depth <= 2) T.depth2++;
    else if (depth <= 4) T.depth4++; else if (depth <= 8) T.depth8++;
    else if (depth <= 16) T.depth16++; else if (depth <= 32) T.depth32++;
    else if (depth <= 64) T.depth64++; else if (depth <= 128) T.depth128++;
    else T.depthMore++;
    if (cause == 3) T.rewindsForced++;
    else if (cause == 2) T.rewindsEvent++;
    else T.rewindsInput++;
    if (at > kfF) {
        if (!sCatch) sRenderWas = PortRenderEnabled();
        sCatch = 1;
        sCatchTo = at;
        sCatchFrom = kfF;
        sCatchT0 = t0;
        PortSetRenderEnabled(0);
    } else {
        PendClose(MICROS() - t0);
    }
}

void PortLnForceRewind(u32 depth)
{
    if (gPortLnOn)
        sForce = depth ? depth : 1;
}

/* --- rooms mode on a guest: the canonical shadow -------------------------------
 *
 * In rooms mode (platform/localmp.c: the players in different rooms, each
 * device showing its own) a guest's picture is its own player's, so its
 * memory is not the host's: the display, the palettes, the sound engine and
 * the heap layout differ by what it shows (ROOMS-REPORT.md, netproof-eval).
 * The whole-image hash cannot compare it.  So each guest keeps a second
 * state, the CANONICAL SHADOW C: the session as the host shows it (player
 * 1's screen), advanced on sealed frames only, never predicted, never drawn,
 * never heard.  C is what the host's hashes verify, byte for byte, with the
 * machinery of every other session (Verify, repairWanted, the host's image).
 * The live state -- the PERSONAL timeline P: predicted, rewound, drawn and
 * heard as any guest's -- is checked against C on the same device by the
 * gameplay digest (localmp.c PortLmpDigestOf), and rebuilt from C when they
 * differ (PersRepair: C's state, re-viewed, replayed to the present).  No
 * second program: C is one more state image, swapped in at a frame top,
 * run a batch of sealed frames the way a rewind replays them, and swapped
 * out (CanonStart, CanonTop).  The host shows player 1 and has no shadow.
 *
 * C's buffers: SaveState's layout plus SRAM always.  sCanonF: the frame top
 * C is parked at (it has run every frame before it); sCanonDone: the last
 * top C hashed.  While C runs (sCanonRun) the frame number in memory is
 * C's; NowF() is P's. */
#define CANON_BATCH_DEF 16
static int sCanonBatch = CANON_BATCH_DEF;

static int RoomsGuest(void)
{
    return sRole == PORT_LN_GUEST && PortLmpRooms();
}

int PortLnCanonRunning(void)
{
    return sCanonRun;
}

/* Measurement: frames per shadow batch (1..240). */
void PortLnSetCanonBatch(int n)
{
    sCanonBatch = n < 1 ? 1 : n > 240 ? 240 : n;
}

static u32 CanonBytes(void)
{
    return sKfBytes + GBA_SRAM_SIZE;
}

static void CanonSave(u8 *d)
{
    SaveState(d);
    memcpy(d + sKfBytes, (const void *)(uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE);
}

static void CanonLoad(const u8 *src)
{
    LoadState(src, 0);
    memcpy((void *)(uintptr_t)GBA_SRAM_BASE, src + sKfBytes, GBA_SRAM_SIZE);
}

/* The slots' devices, into the simulation (localmp.c's per-device groups:
 * several people on one device share its screen). */
static void RoomsDevices(void)
{
    int dev[4], s;

    for (s = 0; s < 4; s++)
        dev[s] = sOwn[s] ? OwnDev(sOwn[s]) : -1;
    PortLmpSetDevices(dev);
}

/* This device's screen: its first Kirby (the anchor of its group: the
 * shared camera bends that one's), player 1's while it has none. */
static void RoomsSetView(void)
{
    int s, v = 0;

    for (s = 0; s < 4; s++)
        if (sOwn[s] && OwnDev(sOwn[s]) == sSelf) { v = s; break; }
    PortLmpSetView(v);
    T.view = (u32)v;
}

/* An image (a join, a repair) has just been loaded into the live state at
 * F: it is the canonical shadow's too. */
static void CanonAdopt(void)
{
    u32 F = PortFrameNumber();

    T.rooms = (u32)RoomsGuest();
    if (!RoomsGuest())
        return;
    if (sCanon == NULL)
        sCanon = (u8 *)malloc(CanonBytes());
    if (sCanonSwap == NULL)
        sCanonSwap = (u8 *)malloc(CanonBytes());
    if (sCanon == NULL || sCanonSwap == NULL) {
        PortError("[katam-port] lmpnet: rooms mode: no memory for the canonical shadow");
        return;
    }
    CanonSave(sCanon);
    sCanonF = F;
    sCanonDone = F > 0 ? F - 1 : NONE;
    sDigV = F + 1;
    sPersRepair = 0;
    T.canonFrame = F;
}

/* At a frame top of the live state: run the shadow if a batch of sealed
 * frames is waiting for it.  Returns 1 with the shadow live. */
static int CanonTopRun(void);

static int CanonStart(void)
{
    u32 now, target;
    u64 t0;

    if (!RoomsGuest() || sCanon == NULL || sCanonF == NONE || sCatch || sImgLoad || sPersRepair
        || sDirty != NONE || sForce || T.repairWanted)
        return 0;
    now = PortFrameNumber();
    target = sSealedUpTo < now ? sSealedUpTo : now;
    if (target < sCanonF + (u32)sCanonBatch)
        return 0;
    t0 = MICROS();
    CanonSave(sCanonSwap);
    CanonLoad(sCanon);
    T.canonSwapUsSum += (u32)(MICROS() - t0);
    sCanonRun = 1;
    sCanonP = now;
    sCanonTo = target;
    sCanonT0 = t0;
    sRenderWas = PortRenderEnabled();
    sCatch = 1;                 /* the frame loop: no picture, no waiting */
    sCatchTo = target;
    PortSetRenderEnabled(0);
    T.canonRuns++;
    return CanonTopRun();
}

/* A frame top while the shadow runs.  Returns 1 while it still runs; 0 once
 * it has been put away and the live state is back at its own top. */
static int CanonTopRun(void)
{
    u32 f = PortFrameNumber();
    u64 t1, dt;

    if (f < sCanonTo) {
        if ((sFlags & PORT_LN_F_HASH_EVERY) || (f & 15) == 0)
            HashState(f);
        if (sDumpAt == f) {
            if (sDump == NULL)
                sDump = (u8 *)malloc(ImageBytes());
            if (sDump != NULL) {
                SaveImage(sDump, f);
                sDumpReady = f + 1;
                T.dumpFrame = f;
            }
        }
        sCanonDone = f;
        T.canonFrames++;
        return 1;
    }
    t1 = MICROS();
    CanonSave(sCanon);
    sCanonF = f;
    CanonLoad(sCanonSwap);
    sCanonRun = 0;
    sCatch = 0;
    PortSetRenderEnabled(sRenderWas);
    T.canonSwapUsSum += (u32)(MICROS() - t1);
    dt = MICROS() - sCanonT0;
    T.canonUsSum += (u32)dt;
    if (dt > T.canonUsMax) T.canonUsMax = (u32)dt;
    T.canonFrame = f;
    return 0;
}

/* The personal timeline against the shadow: the gameplay digest of every
 * frame both hold final and the shadow has had verified by the host. */
static void DigestCheck(void)
{
    u32 lim, g;

    if (!RoomsGuest() || sCatch || sDigV == NONE || sCanonF == NONE || T.repairWanted || sPersRepair)
        return;
    lim = sCanonF;                                  /* C ran latch g-1 */
    if (sSealedUpTo < lim) lim = sSealedUpTo;       /* P latched it sealed */
    if (PortFrameNumber() < lim) lim = PortFrameNumber();
    if (sDirty != NONE && sDirty < lim) lim = sDirty;
    if (sVerify < lim + 1) lim = sVerify > 0 ? sVerify - 1 : 0;   /* the host agreed */
    for (g = sDigV; g <= lim; g++) {
        u32 a = PortLmpDigestOf(1, g), b = PortLmpDigestOf(0, g);

        if (!a || !b)
            continue;
        T.digCompared++;
        if (a == b) {
            T.digEqual++;
            continue;
        }
        T.digMismatch++;
        if (T.digFirstMismatch == 0) T.digFirstMismatch = g;
        PortLog("[katam-port] lmpnet: f%u rooms: this device's game differs from the canonical one "
                "at f%u (digest %08X vs %08X); rebuilt from it",
                (unsigned)PortFrameNumber(), (unsigned)g, (unsigned)b, (unsigned)a);
        PortLmpDigestExplain(g);
        sPersRepair = 1;
        g++;
        break;
    }
    if (g > sDigV) sDigV = g;
    T.digUpTo = sDigV;
}

/* Tests: flip bits of one byte of the canonical shadow at a GBA address
 * (as the rig corrupts a live state with a poke).  1 if it holds it. */
int PortLnCanonPoke(u32 addr, u32 xorMask)
{
    struct PortRbPart pt;
    int i;

    if (sCanon == NULL || sCanonRun)
        return 0;
    for (i = 0; PortRbSnapshotPart(i, &pt); i++)
        if (addr >= pt.base && addr < pt.base + pt.size) {
            sCanon[pt.offset + (addr - pt.base)] ^= (u8)xorMask;
            return 1;
        }
    return 0;
}

/* The live state rebuilt from the shadow: its state at sCanonF, re-viewed
 * to this device's player at the next latch that may, then replayed to the
 * present with what this device holds.  Like an image from the host, with
 * no network. */
static int PersRepair(void)
{
    u32 old = PortFrameNumber(), F = sCanonF;
    u64 t0 = MICROS();
    u32 f;

    if (sCanon == NULL || F == NONE || F > old)
        return 0;
    PendOpen(2, old, F, F, 4);
    CanonLoad(sCanon);
    KfInvalidateAll();
    sFloor = F;
    sDirty = NONE;
    sForce = 0;
    sForceKf = 1;
    sPersRepair = 0;
    sDigV = F + 1;
    for (f = F; f < old && f < F + TLN; f++)
        if (Have(f))
            E(f)->flags &= ~E_SIM;
    T.persRepairs++;
    PortEarReplay(F, old);
    if (old > F) {
        if (!sCatch) sRenderWas = PortRenderEnabled();
        sCatch = 1;
        sCatchTo = old;
        sCatchFrom = F;
        sCatchT0 = t0;
        PortSetRenderEnabled(0);
    } else {
        PendClose(MICROS() - t0);
    }
    return 1;
}

/* --- the frame top ----------------------------------------------------------- */

/* The pause page's session settings in a final frame: an edit made by this
 * device's player is applied to this device, once (port/simblock.h). */
static void FinalSettings(const struct Ent *e)
{
    /* the pauser's slot (set[9]) is this device's if it owns that slot */
    int me = e->set[9] < 4 && OwnDev(sOwn[e->set[9]]) == sSelf ? e->set[9] : -1;

    if (!e->set[0])
        return;
    if (sSetApplied < 0) {
        sSetApplied = e->set[8];
        return;
    }
    if (e->set[8] == (u8)sSetApplied)
        return;
    sSetApplied = e->set[8];
    if (e->set[9] == me) {
        PortPocSessionSettingsApply(e->set);
        T.settingsDeviceApplied++;
    } else {
        T.settingsOtherEdits++;
    }
}

static void Verify(void)
{
    u32 lim;

    if (sRole != PORT_LN_GUEST || sCatch || sTop == NONE)
        return;
    lim = sSealedUpTo < sTop ? sSealedUpTo : sTop;
    if (RoomsGuest()) {
        /* the hashes are the canonical shadow's (rooms mode) */
        if (sCanonDone == NONE)
            return;
        if (sCanonDone < lim)
            lim = sCanonDone;
    } else if (sDirty != NONE && sDirty < lim)
        lim = sDirty;
    if (T.repairWanted && T.repairFrame != NONE && T.repairFrame < lim)
        lim = T.repairFrame;
    if (sVerify + TLN - 128 < sTop) {
        T.unverifiable += sTop - (TLN - 128) - sVerify;
        sVerify = sTop - (TLN - 128);
    }
    while (sVerify <= lim) {
        struct Ent *e;

        if (Have(sVerify))
            FinalSettings(E(sVerify));
        if (!Have(sVerify) || !(E(sVerify)->flags & E_HASH)) {
            /* never hashed here (hash-every off, or skipped by a jump) */
            if (Have(sVerify) && (E(sVerify)->flags & E_SEALED))
                JudgeSounds(E(sVerify));
            T.unverifiable++;
            sVerify++;
            continue;
        }
        e = E(sVerify);
        if (!(e->flags & E_RHASH))
            break;
        if (e->rh[0] == e->h[0] && e->rh[1] == e->h[1]) {
            e->flags |= E_VERIFIED;
            T.verified++;
        } else {
            e->flags |= E_MISMATCH;
            T.mismatches++;
            if (T.firstMismatch == NONE) T.firstMismatch = sVerify;
            T.lastMismatch = sVerify;
            T.repairWanted = 1;
            if (T.repairFrame == NONE) T.repairFrame = sVerify;
        }
        if (sVerify > 0 && Have(sVerify - 1))
            JudgeSounds(E(sVerify - 1));
        sVerify++;
    }
    T.verifiedUpTo = sVerify;
}

void PortLnFrame(void)
{
    u32 f;
    u64 t0;

    if (!gPortLnOn)
        return;
    t0 = MICROS();
    if (sCanonRun) {
        if (CanonTopRun())
            return;             /* the canonical shadow's top (rooms mode) */
    } else {
        CloseSounds();
        if (CanonStart())
            return;
    }

    if (sImgLoad) {
        ApplyImage();
    } else if (sPersRepair && sRole == PORT_LN_GUEST && !sCatch) {
        PersRepair();
    } else if (sRole == PORT_LN_GUEST && !sCatch && (sDirty != NONE || sForce)) {
        u32 at = PortFrameNumber();
        u32 target = sDirty;
        int cause = sDirtyEvent ? 2 : 1;

        if (sForce) {
            u32 t = at > sForce ? at - sForce : 0;

            if (target == NONE || t < target) { target = t; cause = 3; }
            sForce = 0;
        }
        sDirty = NONE;
        if (target < sFloor)
            target = sFloor;
        Rewind(target, cause);
    }

    if (!sSetInit) {
        sSetInit = 1;
        if (sRole == PORT_LN_HOST && PortMenuPoc())
            PortPocSessionSettingsInit();
    }
    f = PortFrameNumber();
    memcpy(Claim(f)->set, PORT_SIM_LNSET, PORT_SIM_LNSET_BYTES);
    if (sRole == PORT_LN_HOST)
        FinalSettings(E(f));
    if (sCatch && f >= sCatchTo) {
        u64 dt = MICROS() - sCatchT0;
        u32 n = sCatchTo - sCatchFrom;

        sCatch = 0;
        PortSetRenderEnabled(sRenderWas);
        T.catchUpUsSum += (u32)dt;
        if (dt > T.catchUpUsMax) T.catchUpUsMax = (u32)dt;
        if (n > T.catchUpFramesMax) T.catchUpFramesMax = n;
        PendClose(dt);
    }

    if (sKf && (sForceKf || f % (u32)sKfEvery == 0)) {
        KfSave(f);
        sForceKf = 0;
    }
    if (sKf) {
        u32 o = KfOldest();

        T.historyFrames = o == NONE ? 0 : f - o;
    }

    if (RoomsGuest())
        ;                       /* the canonical shadow hashes (rooms mode) */
    else if ((sFlags & PORT_LN_F_HASH_EVERY) || (f & 15) == 0)
        HashState(f);
    else if (Have(f))
        E(f)->flags &= ~E_HASH;

    sTop = f;
    T.top = f;
    if (sRole == PORT_LN_HOST && Have(f))
        JudgeSounds(E(f > 0 ? f - 1 : 0));
    Verify();
    DigestCheck();

    if (sImgReq) {
        u64 t1 = MICROS();

        if (sImg == NULL)
            sImg = (u8 *)malloc(ImageBytes());
        if (sImg != NULL) {
            SaveImage(sImg, f);
            sImgBytes = ImageBytes();
            sImgFrame1 = f + 1;
            T.imagesSaved++;
            T.imageBytes = sImgBytes;
            T.imageSaveUs = (u32)(MICROS() - t1);
        }
        sImgReq = 0;
    }
    if (sDumpAt == f && !RoomsGuest()) {
        if (sDump == NULL)
            sDump = (u8 *)malloc(ImageBytes());
        if (sDump != NULL) {
            SaveImage(sDump, f);
            sDumpReady = f + 1;
            T.dumpFrame = f;
        }
    }
    T.floor = sFloor;
    T.sealedUpTo = sSealedUpTo;
    {
        u64 dt = MICROS() - t0;

        if (!sCatch && dt > T.topUsMax) T.topUsMax = (u32)dt;
    }
}

void PortLnDumpAt(u32 f) { sDumpAt = f; sDumpReady = 0; }
u32  PortLnDumpReady(void) { return sDumpReady; }
u8  *PortLnDumpPtr(void) { return sDump; }

u32 *PortLnTelemetry(void) { return (u32 *)&T; }
int  PortLnTelemetryWords(void) { return (int)(sizeof T / sizeof(u32)); }
const char *PortLnTelemetryNames(void) { return kTelNames; }

int PortLnCorrectionCount(void) { return sNumCorr; }
int PortLnCorrection(int i, s32 *out)
{
    if (i < 0 || i >= sNumCorr)
        return 0;
    memcpy(out, sCorr[i], sizeof sCorr[i]);
    return 1;
}
const char *PortLnCorrectionNames(void) { return kCorrNames; }

/* The snapshot's layout for tools: part i's GBA base, offset, size. */
int PortLnSnapPart(int i, u32 *out)
{
    struct PortRbPart pt;

    if (!PortRbSnapshotPart(i, &pt))
        return 0;
    out[0] = pt.base; out[1] = pt.offset; out[2] = pt.size;
    return 1;
}

/* --- home: leaving a session without reloading the page -------------------------
 *
 * Every device keeps one picture of its own game from just after power-on
 * (the first frame top, before the game has read its save): the GBA map, the
 * port's out-of-map state and the host state an image carries.  Leaving a
 * session (PortLnGoHome) stops it and puts that picture back at the next
 * frame top: the game boots again, to the title, as at power-on -- with this
 * device's own save memory (the host: what it has now, its world; a guest:
 * what it had before the host's image replaced it), its own view, and the
 * frame counter running on (nothing on this side ever sees time go back).
 * Then it can host or join again.  Device state only; outside a session it is
 * taken once and never read. */
static u8 *sHome;                   /* snapshot, then host extras           */
static u32 sHomeSnap, sHomeExtras;
static int sHomeLmp, sHomeLoad, sHomeTried;
static u8 *sOwnSram;                /* a guest's own save, from its start   */
static u32 sHomeLoads;

void PortSramEnsureRestored(void);
void PortSramPersistUnblock(void);

static void HomeSave(void)
{
    sHomeTried = 1;
    sHomeSnap = PortRbSnapshotBytes();
    sHomeExtras = PortLnExtrasSize();
    sHome = (u8 *)malloc(sHomeSnap + sHomeExtras);
    if (sHome == NULL)
        return;
    PortRbSnapshotSave(sHome);
    PortLnExtrasSave(sHome + sHomeSnap);
    sHomeLmp = PortLmpSetting();
}

/* A guest, as its session starts: its own save, before the host's image. */
static void HomeKeepOwnSave(void)
{
    PortSramEnsureRestored();
    if (sOwnSram == NULL)
        sOwnSram = (u8 *)malloc(GBA_SRAM_SIZE);
    if (sOwnSram != NULL)
        memcpy(sOwnSram, (const void *)(uintptr_t)GBA_SRAM_BASE, GBA_SRAM_SIZE);
}

static void HomeLoad(void)
{
    u8 fs[64];
    u32 v;

    sHomeLoad = 0;
    if (sHome == NULL || PortFrameStateSize() > sizeof fs)
        return;
    PortFrameStateSave(fs);             /* the frame counter runs on */
    PortRbSnapshotLoad(sHome);
    PortFrameStateLoad(fs);
    if (sOwnSram != NULL) {             /* a guest: its own save again */
        memcpy((void *)(uintptr_t)GBA_SRAM_BASE, sOwnSram, GBA_SRAM_SIZE);
        free(sOwnSram);
        sOwnSram = NULL;
    }
    PortSramPersistUnblock();
    if (PortLmpSetting() != sHomeLmp)
        PortLmpSetHumans(sHomeLmp);
    PortLnExtrasLoad(sHome + sHomeSnap, sHomeExtras);
    PortPocHostStateRebuild();
    /* this device's own view again (the session's was the host's) */
    v = PortSimViewRequested();
    if (v)
        PortInputLogScheduleView(PortFrameNumber() + 1, (s32)(v >> 16), (s32)(v & 0xFFFF));
    sHomeLoads++;
    PortLog("[katam-port] lmpnet: left the session -- back to power-on (frame %u)",
            (unsigned)PortFrameNumber());
}

/* platform/main.c, right after PortLnFrame: the home picture, taken once. */
void PortLnHomeFrame(void)
{
    if (sHomeLoad && !gPortLnOn) {
        HomeLoad();
        return;
    }
    if (!sHomeTried && !gPortLnOn && !PortInputLogReplaying() && PortFrameNumber() >= 1)
        HomeSave();
}

int PortLnGoHome(void)
{
    PortLnStop();
    if (sHome == NULL)
        return 0;
    sHomeLoad = 1;
    return 1;
}

int PortLnHomeReady(void) { return sHome != NULL; }
u32 PortLnHomeLoads(void) { return sHomeLoads; }

/* Host state outside the GBA map and the sim block that the simulation reads
 * back, carried with every keyframe and image and hashed as region "host"
 * (found by this rig, R1/R2: the pause SETTINGS page's saved OBJ tiles and
 * the rest of platform/menu_poc.c's screen state). */
u32 PortLnExtrasSize(void) { return PortPocHostStateSize(); }
void PortLnExtrasSave(u8 *dest) { PortPocHostStateSave(dest); }
void PortLnExtrasLoad(const u8 *src, u32 len)
{
    if (len >= PortPocHostStateSize())
        PortPocHostStateLoad(src);
}

#ifdef __cplusplus
}
#endif
