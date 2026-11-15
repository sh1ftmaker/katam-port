/* Online sessions: what the guest's speakers hear.
 *
 * The problem
 * -----------
 * A guest runs a few frames ahead of the host, guesses the host player's
 * pad, and rewinds and replays when the real one arrives (platform/lmpnet.c).
 * The replayed frames are simulated exactly -- the sound engine included,
 * since its state is game memory -- but nobody can hear them: they happen
 * in a burst inside one presented frame.  So every sound the other player
 * causes starts in a frame the guest only replays, one lead late, and the
 * speakers pick it up wherever its sequence has got to by then: its attack
 * is lost, and a short effect is lost entirely.  Sounds the guess caused that
 * the real pad did not are cut off by the rewind, with a click.
 *
 * The design
 * ----------
 * The in-image engine stays canonical and untouched.  What changes is only
 * which samples go to the speakers, and when -- device state, outside the
 * image, never hashed, never read back by the simulation.
 *
 * The mixer renders each music player's channels into a group of its own
 * (platform/m4a_mixer.c; a channel belongs to the player whose track holds
 * it, and keeps that owner through its release).  The speakers then hear
 * each player's stream through a reader that may run behind the simulation:
 *
 *   main reader   slot `now - d` of the player's stream; d is 0 except
 *                 after a correction made the player start a sound the
 *                 speakers have not heard.
 *   insert        a bounded stretch [from, to) of the stream played once,
 *                 from its beginning, alongside the main reader: a sound the
 *                 final timeline starts and then cuts off with another start
 *                 on the same player.
 *
 * A "slot" is one canonical block (800 samples), keyed by the frame whose
 * latch it was rendered under; a player's slots are kept for DMAX frames
 * when something may read them late.  At the end of every replay (the first
 * block the speakers will hear again) the replayed window's sound starts,
 * per player, are compared with the ones the timeline had before the rewind
 * and with what the speakers actually heard:
 *
 *   - a start heard before and still there (same song, same frame, or the
 *     same song within SHIFT_TOL frames): nothing; it keeps playing as it is
 *     (no re-fire).
 *   - a start that is in the final timeline but was not heard: played from
 *     its beginning, late by the correction (the main reader moves back to
 *     it, or an insert plays it if a later start on that player cuts it).
 *   - a start heard that the timeline no longer has: its player's stream is
 *     cross-faded to the corrected one over XF samples (~8 ms).
 *
 * Every player's main reader is cross-faded across a correction from the
 * block the speakers would have heard to the corrected one -- a no-op when
 * the two are the same, which they nearly always are for music.  A delayed
 * reader comes back to real time by itself: at once when the player's stream
 * is silent between the reader and now, or by skipping one block in four
 * (cross-faded, never a block holding a start) after HOLD frames.  And a
 * sound started live on a player that is behind brings it to real time at
 * once (what was behind finishes as an insert), so the guest's own actions
 * are never delayed.
 *
 * The host never rewinds, so none of this is used there: its mixer path is
 * the one from before.  Off a session nothing here runs at all.
 *
 * Accounting
 * ----------
 * Both with the reconciler on and off (PortEarSet(0): the speakers get the
 * current timeline's block, which is what the build before this did), each
 * slot records the starts the latest timeline has in it and which of them the
 * speakers heard from their first block (and when).  When lmpnet says a frame
 * is final, its starts are judged against that: heard from the start (and
 * how late), never heard from the start, heard twice; and the heard starts
 * that are not final (spurious, and for how long they sounded). */

#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/audio.h"
#include "port/audio_ear.h"
#include "port/lmpnet.h"
#include "gba/gba.h"
#include "gba/m4a.h"

#ifdef __cplusplus
extern "C" {
#endif

u64 PortHostMicros(void);
u32 PortLnLatchFrame(void);

#define NP      PORT_EAR_PLAYERS
#define NG      PORT_EAR_GROUPS
#define BLK     PORT_EAR_BLOCK
#define DMAX    48              /* frames a reader may run behind (800 ms)  */
#define NSLOT   64              /* audio slots kept per player (> DMAX)     */
#define NREC    1024            /* start records (> finalisation lag)       */
#define XF      400             /* cross-fade, canonical samples (8.4 ms)   */
#define JUDGE_LAG (DMAX + 4)
#define SHIFT_TOL 12
#define HOLD    60
#define MAXINS  4
#define MAXMARK 32
#define NOF     0xFFFFFFFFu

/* The four MusicPlayerInfo, at their hardware addresses (platform/port/prelude.h). */
static const u32 kInfo[NP] = { 0x030016A0u, 0x030016E0u, 0x03001720u, 0x03001770u };

/* --- counters ---------------------------------------------------------------- */
#define EAR_STATS(X) \
    X(on) X(renders) X(replayRenders) X(replayGroupMixes) X(replayUs) X(composeUs) \
    X(reconciles) X(splices) X(inserts) X(shifted) X(clamped) X(liveCuts) X(collapses) X(skips) \
    X(finalStarts) X(heardFromStart) X(neverHeard) X(doubles) X(spurious) \
    X(early) X(late0) X(late1_4) X(late5_8) X(late9_16) X(late17_32) X(late33) X(lateSum) X(lateMax) X(lateN) \
    X(spurDurSum) X(spurDur0_4) X(spurDur5_8) X(spurDur9_16) X(spurDur17_32) X(spurDur33) X(spurDurMax) \
    X(spurOpen) X(delayedFrames) X(maxDelay) X(resets) X(composeRenders)

#define X_FIELD(n) u32 n;
struct EarStats { EAR_STATS(X_FIELD) };
#undef X_FIELD
#define X_NAME(n) #n ","
static const char kEarNames[] = EAR_STATS(X_NAME);
#undef X_NAME
static struct EarStats S;

/* Measuring: the last starts judged never heard from their start, and the
 * last second hearings of one start (top bit of the slot). */
#define NDBG 256
static u32 sDbg[NDBG][4];
static u32 sDbgN;
u32 PortEarDebug(int which) { return which == 0 ? sDbgN : which == 1 ? (u32)(uintptr_t)sDbg : NDBG; }
static void DebugNote(u32 s, int p, u32 song, u32 heard)
{
    u32 *d = sDbg[sDbgN++ % NDBG];

    d[0] = s; d[1] = (u32)p; d[2] = song; d[3] = heard;
}

/* --- start records, one per slot ------------------------------------------------ */
struct Rec {
    u32 tag;                    /* the slot (render frame) this describes       */
    u32 song[NP];               /* starts in it, latest timeline (header or 0)  */
    u32 oldSong[NP];            /* ...before the replay now in progress         */
    u32 oldGen;
    u32 heardSong[NP];          /* what the speakers heard start here           */
    u32 heardAt[NP];            /* the render it was heard in                   */
    u32 cutAt[NP];              /* a heard start the timeline lost: cut here    */
    u8  heardN[NP];
    u8  final, judged;
};
static struct Rec *sRec;
static u32 sOwnTag[NREC];
static u8  sOwnAt[NREC][MAX_DIRECTSOUND_CHANNELS];

static struct Rec *R(u32 s)
{
    struct Rec *r = &sRec[s % NREC];

    return r->tag == s ? r : NULL;
}

static u32 RecSong(u32 s, int p)
{
    struct Rec *r = R(s);

    return r ? r->song[p] : 0;
}

/* --- audio: group accumulators and each player's slot ring ---------------------------- */
static s32 sAcc[NG][2][BLK];    /* [g][0] right, [g][1] left -- as the mixer's */
static u32 sDirty, sLoud;       /* groups written / with an active channel     */
typedef s32 Block[2][BLK];
static Block *sSlot;            /* [NP * NSLOT] */
static u32 sSlotTag[NP][NSLOT];
static u8  sSlotLoud[NP][NSLOT];
static Block sOld[NP];          /* what each player's main reader would have played */
static u8  sOldValid[NP];

static const Block *Slot(int p, u32 s)
{
    if (sSlotTag[p][s % NSLOT] != s || !sSlotLoud[p][s % NSLOT])
        return NULL;
    return (const Block *)&sSlot[p * NSLOT + s % NSLOT];
}

static int SlotKnown(int p, u32 s)
{
    return sSlotTag[p][s % NSLOT] == s;
}

static void Store(int p, u32 s)
{
    sSlotTag[p][s % NSLOT] = s;
    sSlotLoud[p][s % NSLOT] = (u8)((sLoud >> p) & 1);
    if ((sLoud >> p) & 1)
        memcpy(&sSlot[p * NSLOT + s % NSLOT], sAcc[p], sizeof(Block));
}

/* --- readers ------------------------------------------------------------------------ */
struct Ins { u32 from, to, startN, song; };
struct Pl {
    u32 d, hold;
    int nIns;
    struct Ins ins[MAXINS];
    u8 startSeen;
    u8 xmode;                   /* next compose: 1 cross-fade from sOld (or
                                   sXsrc), 2 fade sOld out under a start */
};
static struct Pl sPl[NP];
static const Block *sXsrc[NP];  /* cross-fade source other than sOld (a skip) */

/* --- session ------------------------------------------------------------------------- */
static int sOn = 1, sLive, sPolicy;
static u32 sN = NOF;            /* the last render the speakers got */
static u8  sOwner[MAX_DIRECTSOUND_CHANNELS];
static struct { int active; u32 from, at, gen; } sRp;
static struct { u32 s, n, song; u8 p, src; } sMark[MAXMARK];
static int sNMark;

void PortEarSet(int on) { sOn = on & 1; sPolicy = on >> 1; }
u32 *PortEarStats(void) { S.on = (u32)sOn; return (u32 *)&S; }
int PortEarStatsWords(void) { return (int)(sizeof S / sizeof(u32)); }
const char *PortEarStatsNames(void) { return kEarNames; }

static void ResetAll(void)
{
    int i;

    for (i = 0; i < NREC; i++) {
        sRec[i].tag = NOF;
        sOwnTag[i] = NOF;
    }
    memset(sSlotTag, 0xFF, sizeof sSlotTag);
    memset(sPl, 0, sizeof sPl);
    memset(sOldValid, 0, sizeof sOldValid);
    memset(sOwner, NG - 1, sizeof sOwner);
    sRp.active = 0;
    sNMark = 0;
    sN = NOF;
    S.resets++;
}

static int Alloc(void)
{
    if (sRec == NULL)
        sRec = (struct Rec *)calloc(NREC, sizeof *sRec);
    if (sSlot == NULL)
        sSlot = (Block *)malloc(sizeof(Block) * NP * NSLOT);
    return sRec != NULL && sSlot != NULL;
}

void PortEarSessionEnd(void)
{
    free(sRec);
    free(sSlot);
    sRec = NULL;
    sSlot = NULL;
    sLive = 0;
}

/* Measuring (tools/netproof/sound_eval.mjs): per render, each player's
 * share of the speakers' block -- on the host too, where the ear is a pass
 * through (it never rewinds, so nothing is ever delayed or cross-faded and
 * the block is the same sum). */
#define NPROBE 512
static int sProbe;
static u32 sProbeN;
static struct { u32 tag; float e[NG]; } sProbeRec[NPROBE];
static float sProbeAcc[NG];

/* ...and every start noted (slot, player, song): on the host, its final
 * timeline. */
#define NSTARTLOG 32768
static u32 (*sStartLog)[3];
static u32 sStartLogN;

void PortEarProbe(int on)
{
    sProbe = on != 0;
    sProbeN = 0;
    sStartLogN = 0;
    if (sProbe && sStartLog == NULL)
        sStartLog = (u32 (*)[3])calloc(NSTARTLOG, sizeof *sStartLog);
}
u32 PortEarStartLog(int which)
{
    return which == 0 ? sStartLogN : which == 1 ? (u32)(uintptr_t)sStartLog : NSTARTLOG;
}

static void LogStart(u32 s, int p, u32 song)
{
    if (!sProbe || sStartLog == NULL)
        return;
    sStartLog[sStartLogN % NSTARTLOG][0] = s;
    sStartLog[sStartLogN % NSTARTLOG][1] = (u32)p;
    sStartLog[sStartLogN % NSTARTLOG][2] = song;
    sStartLogN++;
}
u32 PortEarProbeCount(void) { return sProbeN; }
void *PortEarProbeRing(void) { return sProbeRec; }
int PortEarProbeCap(void) { return NPROBE; }

static void ProbeAdd(int g, const Block *b, const Block *old, int fade)
{
    double e = 0;
    int i;

    if (!sProbe || (b == NULL && old == NULL))
        return;
    for (i = 0; i < BLK; i++) {
        double v = b ? (double)(*b)[0][i] + (*b)[1][i] : 0;

        if (old && fade && i < XF)
            v = v * (fade == 2 ? 1.0 : (double)(i + 1) / XF)
              + ((double)(*old)[0][i] + (*old)[1][i]) * (double)(XF - i - 1) / XF;
        e += v * v;
    }
    sProbeAcc[g] += (float)e;
}

static void ProbeCommit(u32 n)
{
    u32 k;
    int g;

    if (!sProbe)
        return;
    k = sProbeN % NPROBE;
    sProbeRec[k].tag = n;
    for (g = 0; g < NG; g++) {
        sProbeRec[k].e[g] = sProbeAcc[g];
        sProbeAcc[g] = 0;
    }
    sProbeN++;
}

int PortEarActive(int frames)
{
    if (!gPortLnOn || frames != BLK || !PortAudioHasDevice() || PortLnCanonRunning())
        return 0;
    if (PortLnRole() != PORT_LN_GUEST && !sProbe)
        return 0;
    if (!sLive) {
        if (!Alloc())
            return 0;
        sLive = 1;
        ResetAll();
    }
    return 1;
}

/* --- ownership -------------------------------------------------------------------------- */
int PortEarOwner(struct SoundInfo *si, int ch)
{
    u32 t = (u32)(uintptr_t)si->chans[ch].track;
    int g;

    if (t != 0) {
        for (g = 0; g < NP; g++) {
            const struct MusicPlayerInfo *mp = (const struct MusicPlayerInfo *)(uintptr_t)kInfo[g];
            u32 base = (u32)(uintptr_t)mp->tracks;

            if (t >= base && t < base + (u32)mp->trackCount * (u32)sizeof(struct MusicPlayerTrack))
                break;
        }
        sOwner[ch] = (u8)(g < NP ? g : NG - 1);
    }
    g = sOwner[ch];
    sLoud |= 1u << g;
    return g;
}

s32 *PortEarAcc(int g, int right)
{
    return sAcc[g][right ? 0 : 1];
}

void PortEarClear(u32 groups)
{
    int g;

    for (g = 0; g < NG; g++)
        if ((groups & sDirty) >> g & 1)
            memset(sAcc[g], 0, sizeof sAcc[g]);
    sDirty &= ~groups;
    sLoud = 0;
}

/* --- starts --------------------------------------------------------------------------------- */
/* A start noted under latch f sounds from the render after latch f+1 (the
 * game's logic for a latch runs after that latch's SoundMain), so it is
 * recorded in slot f+1. */
void PortEarLatch(u32 f)
{
    struct Rec *r;
    u32 s = f + 1;

    if (!sLive)
        return;
    r = &sRec[s % NREC];
    if (r->tag != s) {
        memset(r, 0, sizeof *r);
        r->tag = s;
    } else {
        /* Simulated again.  A frame already final is simulated the same
         * way again (and lmpnet judges it only once), so `final` stays. */
        memset(r->song, 0, sizeof r->song);
    }
}

void PortEarNoteStart(u32 latch, const void *mplayInfo, u32 song)
{
    u32 a = (u32)(uintptr_t)mplayInfo;
    struct Rec *r;
    int p;

    if (!sLive || latch == NOF)
        return;
    for (p = 0; p < NP && kInfo[p] != a; p++)
        ;
    if (p == NP)
        return;
    r = R(latch + 1);
    if (r == NULL)
        return;
    r->song[p] = song;
    if (sRp.active)
        sPl[p].startSeen = 1;
    else
        LogStart(latch + 1, p, song);
}

/* --- what was heard ------------------------------------------------------------------------- */
static void Mark(u32 s, int p, u32 n, int src)
{
    u32 song = RecSong(s, p);

    if (song == 0 || sNMark >= MAXMARK)
        return;
    sMark[sNMark].s = s;
    sMark[sNMark].p = (u8)p;
    sMark[sNMark].n = n;
    sMark[sNMark].song = song;
    sMark[sNMark].src = (u8)src;
    sNMark++;
}

/* A composed block is heard once the next one is composed without a replay
 * in between (a replay throws the pending block away unplayed). */
static void CommitMarks(void)
{
    int i;

    for (i = 0; i < sNMark; i++) {
        struct Rec *r = R(sMark[i].s);
        int p = sMark[i].p;

        if (r == NULL)
            continue;
        if (r->heardN[p] && r->heardSong[p] == sMark[i].song) {
            if (r->heardN[p] < 255) r->heardN[p]++;
            DebugNote(0x80000000u | sMark[i].s, p | (sMark[i].src << 8), r->heardAt[p], sMark[i].n);
        } else {
            r->heardSong[p] = sMark[i].song;
            r->heardAt[p] = sMark[i].n;
            r->heardN[p] = 1;
            r->cutAt[p] = 0;
        }
    }
    sNMark = 0;
}

static int Heard(const struct Rec *r, int p, u32 song)
{
    return r->heardN[p] && r->heardSong[p] == song;
}

static void Bucket(u32 v, u32 *b0, u32 *b1, u32 *b2, u32 *b3, u32 *b4, u32 *b5)
{
    if (v == 0) (*b0)++;
    else if (v <= 4) (*b1)++;
    else if (v <= 8) (*b2)++;
    else if (v <= 16) (*b3)++;
    else if (v <= 32) (*b4)++;
    else (*b5)++;
}

static void Judge(struct Rec *r)
{
    int p;

    if (r->judged)
        return;
    r->judged = 1;
    for (p = 0; p < NP; p++) {
        u32 f = r->song[p];

        if (f) {
            S.finalStarts++;
            if (Heard(r, p, f)) {
                s32 late = (s32)(r->heardAt[p] - r->tag);

                S.heardFromStart++;
                if (r->heardN[p] > 1) S.doubles++;
                if (late < 0) {
                    /* heard a little early: the same sound, relabelled */
                    S.early++;
                } else {
                    Bucket((u32)late, &S.late0, &S.late1_4, &S.late5_8, &S.late9_16, &S.late17_32, &S.late33);
                    S.lateSum += (u32)late;
                    S.lateN++;
                    if ((u32)late > S.lateMax) S.lateMax = (u32)late;
                }
            } else {
                S.neverHeard++;
                DebugNote(r->tag, p, f, r->heardN[p] ? r->heardSong[p] : 0);
            }
        }
        if (r->heardN[p] && r->heardSong[p] != f) {
            u32 dur;

            S.spurious++;
            if (r->cutAt[p]) {
                dur = r->cutAt[p] - r->heardAt[p];
            } else {
                dur = 0;
                S.spurOpen++;
            }
            Bucket(dur, &S.spurDur0_4, &S.spurDur0_4, &S.spurDur5_8, &S.spurDur9_16, &S.spurDur17_32, &S.spurDur33);
            S.spurDurSum += dur;
            if (dur > S.spurDurMax) S.spurDurMax = dur;
        }
    }
}

void PortEarFinal(u32 latch)
{
    struct Rec *r;

    if (!sLive)
        return;
    r = R(latch + 1);
    if (r == NULL)
        return;
    r->final = 1;
    if (sN != NOF && r->tag + JUDGE_LAG <= sN)
        Judge(r);
}

/* --- a rewind or an image: the replay of from..at-1 is about to run ------------------------ */
static void RestoreOwners(u32 f)
{
    u32 i = f % NREC;

    if (f != 0 && sOwnTag[i] == f)
        memcpy(sOwner, sOwnAt[i], sizeof sOwner);
    else
        memset(sOwner, NG - 1, sizeof sOwner);
}

static void Snapshot(u32 from, u32 to)
{
    u32 s;

    for (s = from; s < to; s++) {
        struct Rec *r = R(s);

        if (r && r->oldGen != sRp.gen) {
            memcpy(r->oldSong, r->song, sizeof r->song);
            r->oldGen = sRp.gen;
        }
    }
}

void PortEarReplay(u32 from, u32 at)
{
    int p;

    if (!sLive)
        return;
    if (from >= at || at - from > NREC / 2) {
        /* A join, a jump forward, or deeper than any record: start over. */
        ResetAll();
        return;
    }
    RestoreOwners(from - 1);
    if (sRp.active) {
        /* An image during a replay: still the same pending correction. */
        if (from < sRp.from) {
            Snapshot(from, sRp.from);
            sRp.from = from;
        }
        if (at > sRp.at) {
            Snapshot(sRp.at, at);
            sRp.at = at;
        }
        return;
    }
    sRp.active = 1;
    sRp.gen++;
    sRp.from = from;
    sRp.at = at;
    sNMark = 0;
    Snapshot(from, at);
    /* What each main reader was about to play: the block composed at the
     * last render, which a replay throws away unplayed. */
    for (p = 0; p < NP; p++) {
        const struct Pl *q = &sPl[p];
        const Block *b = NULL;

        sPl[p].startSeen = 0;
        sOldValid[p] = 0;
        if (sN == NOF)
            continue;
        if (q->d == 0)
            b = (sDirty >> p & 1) ? (const Block *)sAcc[p] : NULL;
        else
            b = Slot(p, sN - q->d);
        if (b)
            memcpy(sOld[p], *b, sizeof(Block));
        else
            memset(sOld[p], 0, sizeof(Block));
        sOldValid[p] = 1;
    }
}

/* Players to mix in a render nobody hears: only the end of a replay, and
 * only players whose stream may be read late. */
u32 PortEarReplayMask(void)
{
    u32 t = PortLnLatchFrame(), end, m = 0;
    int p;

    if (!sOn || !sRp.active || t == NOF)
        return 0;
    end = sRp.at - 1;
    if (t + DMAX < end)
        return 0;
    for (p = 0; p < NP; p++)
        if (sPl[p].startSeen || sPl[p].d > 0 || sPl[p].nIns > 0)
            m |= 1u << p;
    return m;
}

/* --- the correction -------------------------------------------------------------------------- */
static void AddIns(int p, u32 from, u32 to, u32 n, u32 song)
{
    struct Pl *q = &sPl[p];

    int i;

    if (from >= to || q->nIns >= MAXINS)
        return;
    /* Already playing that stretch (a second correction at the same render,
     * before the first one's block was heard): once is enough. */
    for (i = 0; i < q->nIns; i++) {
        u32 at = q->ins[i].from + (n - q->ins[i].startN);

        if (from >= q->ins[i].from && from < q->ins[i].to && at <= from)
            return;
    }
    q->ins[q->nIns].from = from;
    q->ins[q->nIns].to = to;
    q->ins[q->nIns].startN = n;
    q->ins[q->nIns].song = song;
    q->nIns++;
}

static void Reconcile(u32 n)
{
    int p;

    sRp.active = 0;
    S.reconciles++;
    for (p = 0; p < NP; p++) {
        struct Pl *q = &sPl[p];
        u32 r = n - q->d, x = NOF, s, cs[64], csong[64];
        u8 matched[64];
        int nc = 0, i, k;

        q->startSeen = 0;
        if (sOn && sOldValid[p])
            q->xmode = 1;
        /* An insert for a start the timeline no longer has: drop it. */
        for (i = 0; i < q->nIns; ) {
            struct Rec *rr = R(q->ins[i].from);

            if (rr && rr->song[p] != q->ins[i].song && sOn) {
                q->ins[i] = q->ins[--q->nIns];
                continue;
            }
            i++;
        }
        for (s = sRp.from; s <= n; s++) {
            struct Rec *rr = R(s);

            if (rr && rr->oldGen == sRp.gen && rr->oldSong[p] != rr->song[p]) {
                x = s;
                break;
            }
        }
        if (x == NOF || x >= r)
            continue;
        for (s = x; s < r && nc < 64; s++) {
            u32 sg = RecSong(s, p);

            if (sg) {
                cs[nc] = s;
                csong[nc] = sg;
                matched[nc] = 0;
                nc++;
            }
        }
        /* Heard starts the timeline lost there: the same sound a little
         * earlier or later is the same sound (relabelled, not replayed);
         * anything else is cut by this correction. */
        for (s = x; s < r; s++) {
            struct Rec *rr = R(s);
            u32 old;

            if (rr == NULL || rr->oldGen != sRp.gen)
                continue;
            old = rr->oldSong[p];
            if (old == 0 || old == rr->song[p] || !Heard(rr, p, old))
                continue;
            for (k = 0; k < nc; k++) {
                struct Rec *rc = R(cs[k]);

                if (!matched[k] && csong[k] == old && rc && !Heard(rc, p, old)
                    && (cs[k] > s ? cs[k] - s : s - cs[k]) <= SHIFT_TOL)
                    break;
            }
            if (k < nc) {
                struct Rec *rc = R(cs[k]);

                matched[k] = 1;
                rc->heardSong[p] = rr->heardSong[p];
                rc->heardAt[p] = rr->heardAt[p];
                rc->heardN[p] = rr->heardN[p];
                rc->cutAt[p] = 0;
                rr->heardN[p] = 0;
                S.shifted++;
            } else if (rr->cutAt[p] == 0) {
                rr->cutAt[p] = n;
            }
        }
        if (!sOn)
            continue;
        /* Final-so-far starts the speakers have not heard: from their
         * beginning.  The last one becomes the main reader's position; an
         * earlier one, which a later start on this player cuts, plays as
         * an insert up to that cut. */
        for (k = 0; k < nc; k++) {
            struct Rec *rc = R(cs[k]);
            u32 from = cs[k];

            if (matched[k] || (rc && Heard(rc, p, csong[k])))
                continue;
            if ((sPolicy & 1) && cs[k] >= PortLnSealedFrontier() + 1)
                continue;
            if (k == nc - 1) {
                u32 d = n - from;

                if (d > DMAX) {
                    d = DMAX;
                    S.clamped++;
                }
                if (!SlotKnown(p, n - d))
                    continue;
                /* The main reader plays on from here: an insert from an
                 * earlier correction over the same stretch would play it
                 * twice. */
                for (i = 0; i < q->nIns; ) {
                    if (q->ins[i].from + (n - q->ins[i].startN) >= n - d) {
                        q->ins[i] = q->ins[--q->nIns];
                        continue;
                    }
                    i++;
                }
                q->d = d;
                q->hold = n + HOLD;
                q->xmode = sOldValid[p] ? 2 : 0;
                S.splices++;
                if (d > S.maxDelay) S.maxDelay = d;
            } else {
                if (from + DMAX < n) {
                    from = n - DMAX;
                    S.clamped++;
                }
                if (from < cs[k + 1] && SlotKnown(p, from)) {
                    AddIns(p, from, cs[k + 1], n, csong[k]);
                    S.inserts++;
                }
            }
        }
    }
}

/* --- the speakers' block --------------------------------------------------------------------- */
static void AddBlock(s32 *outR, s32 *outL, const Block *b)
{
    int i;

    if (b == NULL)
        return;
    for (i = 0; i < BLK; i++) {
        outR[i] += (*b)[0][i];
        outL[i] += (*b)[1][i];
    }
}

/* old -> b over XF samples; `keep` leaves b at full gain (a start fading in
 * over the old block would lose its attack). */
static void AddFade(s32 *outR, s32 *outL, const Block *old, const Block *b, int keep)
{
    int i;

    for (i = 0; i < BLK; i++) {
        s32 nr = b ? (*b)[0][i] : 0, nl = b ? (*b)[1][i] : 0;
        s32 orr = old ? (*old)[0][i] : 0, ol = old ? (*old)[1][i] : 0;

        if (i < XF) {
            s64 k = i + 1;

            outR[i] += (s32)(((s64)orr * (XF - k) + (s64)nr * (keep ? XF : k)) / XF);
            outL[i] += (s32)(((s64)ol * (XF - k) + (s64)nl * (keep ? XF : k)) / XF);
        } else {
            outR[i] += nr;
            outL[i] += nl;
        }
    }
}

static int SilentBetween(int p, u32 a, u32 b)
{
    u32 s;

    for (s = a; s < b; s++)
        if (!SlotKnown(p, s) || sSlotLoud[p][s % NSLOT])
            return 0;
    return 1;
}

static void Compose(u32 n, s32 *outR, s32 *outL)
{
    int p, i;

    if (sDirty >> (NG - 1) & 1) {
        AddBlock(outR, outL, (const Block *)sAcc[NG - 1]);
        ProbeAdd(NG - 1, (const Block *)sAcc[NG - 1], NULL, 0);
    }
    for (p = 0; p < NP; p++) {
        struct Pl *q = &sPl[p];
        const Block *live = (sDirty >> p & 1) ? (const Block *)sAcc[p] : NULL;
        const Block *src, *xs;

        if (q->d > 0) {
            Store(p, n);
            if (sOn && RecSong(n, p) != 0) {
                /* A start now on a player that is behind: it is heard now,
                 * and what was behind plays out as an insert. */
                AddIns(p, n - q->d, n, n, RecSong(n - q->d, p));
                q->d = 0;
                q->xmode = 0;
                S.liveCuts++;
            } else if (SilentBetween(p, n - q->d, n)) {
                /* Nothing audible between: skipping it loses nothing, and
                 * a start in it (a sequence that opens with a rest) counts
                 * as heard from its start. */
                u32 k;

                for (k = n - q->d; k < n; k++)
                    Mark(k, p, n, 2);
                q->d = 0;
                S.collapses++;
            } else if (sOn && n >= q->hold && n % 4 == 0 && q->xmode == 0
                       && RecSong(n - q->d, p) == 0 && RecSong(n - q->d + 1, p) == 0) {
                sXsrc[p] = Slot(p, n - q->d);
                q->d--;
                q->xmode = 3;
                S.skips++;
            }
        }
        src = q->d ? Slot(p, n - q->d) : live;
        Mark(n - q->d, p, n, q->d ? 1 : 0);
        if (q->d) {
            S.delayedFrames++;
            if (q->d > S.maxDelay) S.maxDelay = q->d;
        }
        switch (q->xmode) {
        case 1: xs = (const Block *)sOld[p]; AddFade(outR, outL, xs, src, 0); ProbeAdd(p, src, xs, 1); break;
        case 2: xs = (const Block *)sOld[p]; AddFade(outR, outL, xs, src, 1); ProbeAdd(p, src, xs, 2); break;
        case 3: AddFade(outR, outL, sXsrc[p], src, 0); ProbeAdd(p, src, sXsrc[p], 1); break;
        default: AddBlock(outR, outL, src); ProbeAdd(p, src, NULL, 0); break;
        }
        q->xmode = 0;
        for (i = 0; i < q->nIns; ) {
            struct Ins *in = &q->ins[i];
            u32 s = in->from + (n - in->startN);

            if (s >= in->to) {
                *in = q->ins[--q->nIns];
                continue;
            }
            Mark(s, p, n, 3);
            AddBlock(outR, outL, Slot(p, s));
            ProbeAdd(p, Slot(p, s), NULL, 0);
            i++;
        }
    }
}

void PortEarRendered(u32 used, int audible, s32 *outR, s32 *outL)
{
    u32 t = PortLnLatchFrame(), j;
    u64 t0 = PortHostMicros();
    int p;

    sDirty |= used & sLoud;
    S.renders++;
    if (t != NOF) {
        sOwnTag[t % NREC] = t;
        memcpy(sOwnAt[t % NREC], sOwner, sizeof sOwner);
    }
    if (!audible) {
        S.replayRenders++;
        for (p = 0; p < NP; p++)
            if (used >> p & 1) {
                Store(p, t);
                S.replayGroupMixes++;
            }
        S.replayUs += (u32)(PortHostMicros() - t0);
        return;
    }
    if (sRp.active) {
        if (t + 1 == sRp.at) {
            Reconcile(t);
        } else {
            /* Replayed frames that are heard (PORT_LN_F_NO_AUDIO_CU): no
             * correction to make. */
            sRp.active = 0;
            for (p = 0; p < NP; p++) sPl[p].xmode = 0;
        }
    }
    CommitMarks();
    Compose(t, outR, outL);
    ProbeCommit(t);
    sN = t;
    if (t != NOF && t >= JUDGE_LAG) {
        struct Rec *r = R(t - JUDGE_LAG);

        if (r && r->final && !r->judged)
            Judge(r);
    }
    (void)j;
    S.composeRenders++;
    S.composeUs += (u32)(PortHostMicros() - t0);
}

#ifdef __cplusplus
}
#endif
