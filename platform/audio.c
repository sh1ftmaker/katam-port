/* The sound driver's platform half: what m4a needs that the GBA gave it for
 * free, and the once-a-frame decision of what to hand the speakers.
 *
 * src/m4a.c compiles as written -- tools/legacy-sync/portify.py puts it back into the
 * build and patches four sites.  asm/m4a_asm.s has no C anywhere and is
 * platform/m4a_mixer.c.  What is left over is here.
 *
 * The hardware m4a does not need
 * -----------------------------
 * Almost all of it.  The mixer never touches sound hardware: it reads sample
 * data through `struct WaveData *` pointers into ROM and writes PCM into a
 * plain array.  DMA1/2, FIFO A and B, Timer 0 and SOUNDCNT exist only to move
 * that array to the DAC.  This port maps the ROM at its real address, so the
 * pointers work, and it can read the samples out of the mixer directly -- so
 * none of that has to be emulated.  The register writes m4a still makes land
 * in the mapped-but-inert I/O region and do nothing, which is exactly right.
 *
 * The one piece of sound hardware that is genuinely real is the four PSG
 * channels.  They are not implemented yet: CgbSound runs every frame and keeps
 * their envelopes and register images up to date in the I/O region, so what is
 * missing is only the oscillator that would read them.
 *
 * Where the frame boundary is
 * ---------------------------
 * SoundMain renders one VBlank's worth of samples.  It has three call sites --
 * GameLoop's tail, the VBlank handler, and TasksExec -- which gMainFlags bit
 * 0x1000000 and gExecSoundMain make mutually exclusive, so it runs once a frame
 * today but is not structurally guaranteed to.  m4aSoundVSync is, so that is
 * where the finished block leaves the port.  It also matches the hardware: this
 * is the moment the PCM DMA would have flipped to the block SoundMain filled
 * last frame.
 */

#include <math.h>
#include <string.h>

#include "port/port.h"
#include "port/audio.h"
#include "port/audio_ear.h"
#include "port/lmpnet.h"
#include "gba/gba.h"
#include "gba/m4a.h"

/* C linkage for the 64-bit builds.
 *
 * Those compile the game as C++ and tools/legacy-sync/cxxify.py gives it C linkage, so
 * everything on the seam has to agree.  It is applied to platform/*.c as a
 * class rather than to the files that happened to break: the failure is an
 * undefined reference to a mangled name, or -- for a `const`, which is
 * internal-linkage in C++ and external in C -- to a symbol that is plainly
 * defined a few lines away.  Neither says which file to fix, and the set of
 * files that need it changes whenever a declaration moves.  A no-op in C.
 *
 * The block opens below the includes so that SDL and the system headers stay
 * outside it. */
#ifdef __cplusplus
extern "C" {
#endif

/* Allocated by linker.ld on hardware (data/sound_data.o(ewram_data)) with no C
 * definition anywhere, and reached only through gMPlayTable in the ROM -- which
 * holds their EWRAM addresses.  The port maps EWRAM at its real address, so
 * those pointers are already valid and nothing has to be declared here at all.
 * They are listed for the record:
 *
 *   gMPlayTrack_0  0x02020080  16 tracks
 *   gMPlayTrack_1  0x02020580  10
 *   gMPlayTrack_2  0x020208A0  10
 *   gMPlayTrack_3  0x02020BC0  10
 *
 * The MusicPlayerInfo structures they pair with are not so lucky: the game
 * names those directly, so they have to exist as symbols *and* be at the
 * addresses the ROM expects.  See platform/port/prelude.h. */

/* ------------------------------------------------------------------------- *
 * Block clocking.
 * ------------------------------------------------------------------------- */

static int sRate;            /* device rate, 0 until the page grants audio */
static int sNominal;         /* samples per frame at exactly 60 Hz */
static int sBlock;           /* samples in the block being rendered now */
static int sStarted;
static u32 sToneHz;
static u32 sTonePhase;

static s16 sSilence[PORT_AUDIO_MAX_BLOCK * 2];

void PortAudioTestTone(u32 hz)
{
    sToneHz = hz;
    sTonePhase = 0;
    PortLog("[katam-port] audio test tone %s (%u Hz), device rate %d",
            hz ? "on" : "off", (unsigned)hz, sRate);
}

int PortAudioBlockSamples(void)
{
    return sBlock;
}

/* Track the device clock.
 *
 * requestAnimationFrame is not the audio clock: a 59.94 Hz panel, a browser
 * that throttles, and an AudioContext running off a different crystal all pull
 * the two apart, and a fixed block size turns any of them into a slow drift
 * that ends in a gap or a growing delay.  So the block is nudged by up to ~3%
 * either way to hold the queue at its target depth.
 *
 * Two and a half frames of queue is the target: one is the block currently
 * playing, the rest is headroom for a main thread that just spent 20 ms
 * decompressing a tileset and for a device that takes its samples 512 at a
 * time rather than 128.
 *
 * In the browser that is a starting point rather than a constant.  A phone
 * whose paints arrive in uneven pairs, or whose frames run long, needs more
 * than a desktop does, and nothing says which this is except the ring running
 * dry.  So each time it does the target grows by half a frame, up to six, and
 * every thirty seconds without one it gives an eighth of a frame back: latency
 * is spent only where it buys silence-free sound.
 */
static int sTarget;          /* queue depth to hold, in samples */
static int sSeenUnderruns;
static int sQuietFrames;
static int sQueueAvg;        /* running average of the depth, x16 */

/* An online session (below) starts deeper: a guest's frames arrive less
 * evenly than a lone game's (it holds and skips to keep its lead), so the
 * floor there is three and a half frames. */
static int sCanon;
#define TARGET_FLOOR() (sCanon ? sNominal * 7 / 2 : sNominal * 5 / 2)

static void TendTarget(void)
{
    if (sTarget == 0)
        sTarget = TARGET_FLOOR();
#ifdef __EMSCRIPTEN__
    {
        int u = PortAudioUnderruns();

        if (u != sSeenUnderruns) {
            sSeenUnderruns = u;
            sQuietFrames = 0;
            sTarget += sNominal / 2;
            if (sTarget > sNominal * 6)
                sTarget = sNominal * 6;
        } else if (++sQuietFrames >= 1800) {
            sQuietFrames = 0;
            sTarget -= sNominal / 8;
            if (sTarget < TARGET_FLOOR())
                sTarget = TARGET_FLOOR();
        }
    }
#endif
}

static void ChooseBlockSize(void)
{
    int queued, err, adj;
    int limit = sNominal / 32;

    TendTarget();
    /* The depth is only known to within one device callback -- 128 samples in
     * a desktop browser, 512 or more on a phone -- and it beats against the
     * frame rate, so the raw reading swings by that much from one frame to the
     * next.  Steering on it directly slams the block between its two limits.
     * A short running average sees the trend instead. */
    queued = PortAudioQueuedFrames();
    if (sQueueAvg == 0)
        sQueueAvg = queued << 4;
    sQueueAvg += queued - (sQueueAvg >> 4);
    err = sTarget - (sQueueAvg >> 4);
    adj = err / 8;

    if (adj > limit) adj = limit;
    if (adj < -limit) adj = -limit;

    sBlock = sNominal + adj;
    if (sBlock < 16)
        sBlock = 16;
    if (sBlock > PORT_AUDIO_MAX_BLOCK)
        sBlock = PORT_AUDIO_MAX_BLOCK;
}

/* Is the queue a block and a half short of where it should be?
 *
 * That is not something block-length nudging can fix: it means the game did
 * not deliver a frame in the time the device played one.  The web host uses
 * this to run the sound driver again on its own (platform/web/host_web.c), so
 * a game that drops to 40 frames a second still plays its music at full speed
 * instead of in 40 pieces. */
int PortAudioStarving(void)
{
    if (sRate == 0 || sTarget == 0)
        return 0;
    return PortAudioQueuedFrames() < sTarget - sNominal - sNominal / 2;
}

/* A square wave, in place of everything the mixer would have produced.
 *
 * "No sound" has two entirely different causes -- a mixer that produces silence
 * and a transport that drops what it is given -- and from the couch they are
 * the same event.  This proves the second half on its own.  Exported to JS as
 * _PortAudioTestTone; the headless harness drives it with TONE=440. */
static void RenderTone(s16 *dst, int frames)
{
    /* Phase in 0.32 fixed point, so the frequency is exact at any device rate
     * and the wave does not step between blocks. */
    u32 inc = sRate ? (u32)(((u64)sToneHz << 32) / (u32)sRate) : 0;
    int i;

    for (i = 0; i < frames; i++) {
        s16 v = (sTonePhase & 0x80000000u) ? 6000 : -6000;
        dst[i * 2] = v;
        dst[i * 2 + 1] = v;
        sTonePhase += inc;
    }
}

/* ------------------------------------------------------------------------- *
 * Online sessions: the canonical device.
 *
 * Two devices in an online session (platform/lmpnet.c) run the same
 * simulation and must hold byte-identical memory, and the sound engine's
 * state is in that memory: gSoundInfo's header (the rate, the samples per
 * frame) and every channel's envelope and sample position, which the mixer
 * advances by the block it renders.  Off a session the block follows the
 * device -- its rate, its callback size, its clock (ChooseBlockSize) -- and
 * no device at all means no mixing; all of that reaches game memory
 * (tools/netaudit: audio-on-off, audio-rate, audio-block, pace-burst-audio).
 *
 * So for a session the mixer runs against a device that exists nowhere: a
 * fixed CANON_BLOCK samples a frame at CANON_RATE, on every simulated frame
 * including replayed ones, whether or not this device has audio at all.
 * Only what reaches the speakers adapts, outside the image: the canonical
 * block is resampled to the device's rate, and the output length per frame
 * is steered gently around the device's own clock from the queue depth --
 * small ratio changes around a target fill, a dropped block (cross-faded)
 * only when far too much is queued.  A device that will not play the block
 * (none, or a replayed frame whose sound was already heard) gets the channel
 * state without the samples (m4a_mixer.c, AdvanceOnly), which is exact.
 *
 * CANON_RATE is exactly CANON_BLOCK frames of the GBA's 59.7275 Hz, so the
 * canonical stream is pitch-true at the game's own frame rate, and its
 * resampled output is pitch-true at any device rate.  Off a session none of
 * this runs and nothing above or below is changed by it. */
#define CANON_BLOCK 800
#define CANON_RATE  47782               /* 800 * 59.7275 */

static int sReplayMix;                  /* measuring: mix unheard blocks too */

/* The resampler: a 32-tap windowed sinc, 256 phases, nearest phase. */
#define RS_TAPS   32
#define RS_PHASES 256
#define RS_CAP    4096                  /* canonical frames held (power of 2) */
static float sRsH[RS_PHASES][RS_TAPS];
static float sRsIn[RS_CAP][2];
static s64 sRsW;                        /* canonical frames written         */
static s64 sRsPos;                      /* read position, 32.32 frames      */
static int sRsSkip;                     /* frames of a dropped block, to skip */
static int sRsDesignedFor;
static u32 sCenterAcc;
static int sCQueueAvg;
static s16 sRsOut[PORT_AUDIO_MAX_BLOCK * 2];
static int sPadCool;
static u32 sCanonOut[7];                /* pushes, frames out, drops, pads,
                                           muted, frames in, us resampling
                                           (diagnostics) */
u64 PortHostMicros(void);

static void RsDesign(int outRate)
{
    double fc = 0.5 * 0.92 * (outRate < CANON_RATE ? (double)outRate / CANON_RATE : 1.0);
    const double pi = 3.14159265358979323846;
    int ph, t;

    for (ph = 0; ph < RS_PHASES; ph++) {
        double sum = 0, frac = (double)ph / RS_PHASES;

        for (t = 0; t < RS_TAPS; t++) {
            double d = (double)t - (RS_TAPS / 2 - 1) - frac;
            double x = 2.0 * fc * d;
            double k = d == 0.0 ? 2.0 * fc : sin(pi * x) / (pi * d);
            double u = d / RS_TAPS;     /* -0.5 .. 0.5 */
            double w = 0.42 + 0.5 * cos(2.0 * pi * u) + 0.08 * cos(4.0 * pi * u);

            sRsH[ph][t] = (float)(k * w);
            sum += k * w;
        }
        for (t = 0; t < RS_TAPS; t++)
            sRsH[ph][t] = (float)(sRsH[ph][t] / sum);
    }
    sRsDesignedFor = outRate;
}

static void RsReset(void)
{
    memset(sRsIn, 0, sizeof sRsIn);
    sRsW = 0;
    /* Lag the writer by half the filter, so the last output of a block has
     * its whole window already written. */
    sRsPos = -((s64)(RS_TAPS / 2 + 1) << 32);
    sRsSkip = 0;
    sCQueueAvg = 0;
    sCenterAcc = 0;
    sPadCool = 0;
}

static void RsFeed(const s16 *block, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        float *d = sRsIn[(sRsW + i) & (RS_CAP - 1)];

        d[0] = block ? block[i * 2] * (1.0f / 32768.0f) : 0.0f;
        d[1] = block ? block[i * 2 + 1] * (1.0f / 32768.0f) : 0.0f;
    }
    sRsW += n;
}

static void RsAt(s64 pos, float *l, float *r)
{
    s64 ip = pos >> 32;
    const float *h = sRsH[(u32)(pos >> 24) & (RS_PHASES - 1)];
    s64 base = ip - (RS_TAPS / 2 - 1);
    float a = 0, b = 0;
    int t;

    for (t = 0; t < RS_TAPS; t++) {
        const float *x = sRsIn[(base + t) & (RS_CAP - 1)];

        a += x[0] * h[t];
        b += x[1] * h[t];
    }
    *l = a;
    *r = b;
}

static s16 ToS16(float v)
{
    v *= 32768.0f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (s16)lrintf(v);
}

/* Render `outN` device frames from the next `n` canonical ones (plus a
 * dropped block before them, cross-faded out). */
static void RsOut(int n, int outN)
{
    s64 step = ((s64)n << 32) / outN;
    int xf = sRsSkip ? (outN < 256 ? outN : 256) : 0;
    int i;

    for (i = 0; i < outN; i++) {
        s64 pos = sRsPos + step * i;
        float l, r;

        if (sRsSkip) {
            float l2, r2;

            RsAt(pos + ((s64)sRsSkip << 32), &l2, &r2);
            if (i < xf) {
                float g = (float)(i + 1) / (float)xf;

                RsAt(pos, &l, &r);
                l = l * (1.0f - g) + l2 * g;
                r = r * (1.0f - g) + r2 * g;
            } else {
                l = l2; r = r2;
            }
        } else {
            RsAt(pos, &l, &r);
        }
        sRsOut[i * 2] = ToS16(l);
        sRsOut[i * 2 + 1] = ToS16(r);
    }
    sRsPos += (s64)(n + sRsSkip) << 32;
    sRsSkip = 0;
}

/* The other large excursion, too little queued (the frames are arriving
 * slower than the device plays: a guest holding back to its lead): this
 * block twice, the second pass cross-faded in over the end of the first --
 * one frame more in the queue without a gap or a pitch change. */
static int RsOutPad(int n, int outN)
{
    s64 step = ((s64)n << 32) / outN;
    int xf = outN < 256 ? outN / 2 : 256;
    int total = 2 * outN - xf, i;

    if (total > PORT_AUDIO_MAX_BLOCK)
        return 0;
    for (i = 0; i < total; i++) {
        float l, r;

        if (i < outN - xf) {
            RsAt(sRsPos + step * i, &l, &r);
        } else if (i < outN) {
            float l2, r2, g = (float)(i - (outN - xf) + 1) / (float)xf;

            RsAt(sRsPos + step * i, &l, &r);
            RsAt(sRsPos + step * (i - (outN - xf)), &l2, &r2);
            l = l * (1.0f - g) + l2 * g;
            r = r * (1.0f - g) + r2 * g;
        } else {
            RsAt(sRsPos + step * (i - (outN - xf)), &l, &r);
        }
        sRsOut[i * 2] = ToS16(l);
        sRsOut[i * 2 + 1] = ToS16(r);
    }
    sRsPos += (s64)n << 32;
    return total;
}

int PortAudioCanonical(void)
{
    return sCanon;
}

/* Called by the mixer at every render in a session: the fixed voices' step
 * from the engine's own rate index (so it is the same on every device even
 * if an image brought a different one), and whether to make samples. */
int PortAudioCanonRender(struct SoundInfo *soundInfo)
{
    u32 index = soundInfo->freq;

    if (index >= 1 && index <= 12)
        PortMixerSetFixedRate((597275 * gPcmSamplesPerVBlankTable[index - 1] + 5000) / 10000);
    else
        PortMixerSetFixedRate(0);
    return sRate != 0 && (sReplayMix || !PortLnAudioMuted());
}

void PortAudioSetReplayMix(int on)
{
    sReplayMix = on != 0;
}

int PortAudioHasDevice(void)
{
    return sRate != 0;
}

static void CanonHeader(struct SoundInfo *soundInfo)
{
    soundInfo->pcmSamplesPerVBlank = CANON_BLOCK;
    soundInfo->pcmFreq = CANON_RATE;
    soundInfo->divFreq = (0x1000000 / CANON_RATE + 1) >> 1;
    soundInfo->pcmDmaPeriod = 1;
}

static int SoundInfoLive(const struct SoundInfo *si)
{
    return si != NULL && si->ident >= ID_NUMBER && si->ident <= ID_NUMBER + 1;
}

/* The frame's block, to the device. */
static void CanonVSync(struct SoundInfo *soundInfo)
{
    const s16 *block = NULL;
    int have = 0, n, center, outN, queued, err, adj, limit;
    u64 t0;

    /* Rooms mode: an online guest's canonical shadow runs inside one of the
     * live frames; the live block waits for that frame (platform/lmpnet.c).
     * So does the pause overlay's fork, between two live frames. */
    if (PortLnShadowRunning())
        return;
    if (SoundInfoLive(soundInfo))
        have = PortMixerTakeBlock(&block);
    if (sRate == 0)
        return;
    if (PortLnMuteAudio()) {
        sCanonOut[4]++;
        return;
    }
    if (sToneHz) {
        RenderTone(sSilence, sNominal);
        PortAudioPush(sSilence, sNominal);
        return;
    }
    if (sRsDesignedFor != sRate) {
        RsDesign(sRate);
        RsReset();
    }
    n = have > 0 ? have : CANON_BLOCK;
    RsFeed(have > 0 ? block : NULL, n);
    sCanonOut[5] += (u32)n;

    /* The device's frames per game frame, exactly: rate / 59.7275. */
    sCenterAcc += (u32)sRate * 10000u;
    center = (int)(sCenterAcc / 597275u);
    sCenterAcc %= 597275u;

    TendTarget();
    queued = PortAudioQueuedFrames();
    if (sCQueueAvg == 0)
        sCQueueAvg = queued << 4;
    sCQueueAvg += queued - (sCQueueAvg >> 4);
    err = sTarget - (sCQueueAvg >> 4);

    /* Far too much queued (a burst of live frames, a guest skipping ahead):
     * hold this block back and cross-fade over it with the next. */
    if (err < -3 * sNominal && !sRsSkip) {
        sRsSkip = n;
        sCanonOut[2]++;
        return;
    }
    if (sPadCool > 0)
        sPadCool--;
    /* Otherwise a gentle steer, at most 1% (17 cents) either way. */
    limit = center / 100;
    if (limit < 1) limit = 1;
    adj = err / 16;
    if (adj > limit) adj = limit;
    if (adj < -limit) adj = -limit;
    outN = (int)((s64)(center + adj) * n / CANON_BLOCK);
    if (outN < 16) outN = 16;
    if (outN > PORT_AUDIO_MAX_BLOCK) outN = PORT_AUDIO_MAX_BLOCK;
    sBlock = outN;
    t0 = PortHostMicros();
    if (err > sNominal * 5 / 4 && sPadCool == 0 && !sRsSkip) {
        int total = RsOutPad(n, outN);

        if (total > 0) {
            sPadCool = 30;
            sCanonOut[3]++;
            sCanonOut[6] += (u32)(PortHostMicros() - t0);
            PortAudioPush(sRsOut, total);
            sCanonOut[0]++;
            sCanonOut[1] += (u32)total;
            return;
        }
    }
    RsOut(n, outN);
    sCanonOut[6] += (u32)(PortHostMicros() - t0);
    PortAudioPush(sRsOut, outN);
    sCanonOut[0]++;
    sCanonOut[1] += (u32)outN;
}

u32 *PortAudioCanonOutStats(void)
{
    return sCanonOut;
}

/* platform/lmpnet.c, as a session starts and stops. */
void PortAudioSessionBegin(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;

    if (sCanon)
        return;
    sCanon = 1;
    if (sTarget < TARGET_FLOOR())
        sTarget = TARGET_FLOOR();
    PortMixerSetCanon(CANON_RATE);
    PortMixerInit(CANON_RATE, CANON_BLOCK);
    RsReset();
    if (SoundInfoLive(si)) {
        CanonHeader(si);
        PortAudioCanonRender(si);
    }
    PortLog("[katam-port] audio: online session -- canonical %d Hz x %d per frame, "
            "device %d Hz", CANON_RATE, CANON_BLOCK, sRate);
}

void PortAudioSessionEnd(void)
{
    struct SoundInfo *si = SOUND_INFO_PTR;

    if (!sCanon)
        return;
    sCanon = 0;
    PortEarSessionEnd();
    PortMixerSetCanon(0);
    if (sRate != 0) {
        PortMixerInit(sRate, sNominal);
        sBlock = sNominal;
    } else {
        PortMixerInit(0, 0);
    }
    if (SoundInfoLive(si))
        PortSampleRateSet(si);
}

/* ------------------------------------------------------------------------- *
 * The hooks src/m4a.c calls into.
 * ------------------------------------------------------------------------- */

/* Replaces the body of SampleFreqSet (see tools/legacy-sync/portify.py).
 *
 * On hardware this picks one of twelve fixed rates out of
 * gPcmSamplesPerVBlankTable and programs Timer 0 to clock the DAC at it.  Here
 * the device picks the rate and the mixer renders natively at it, which means
 * there is no resampling stage anywhere in the port -- the phase arithmetic in
 * platform/m4a_mixer.c is doing that work already, and doing it from the note's
 * own frequency rather than after the fact.
 *
 * Two rates matter, and they are not the same one:
 *
 *   pcmFreq is the device rate, so pitch is correct.
 *   pcmSamplesPerVBlank is derived from 60 Hz rather than the LCD's 59.7275 Hz,
 *     because what clocks this port is requestAnimationFrame.
 *
 * And a third, which does not live in SoundInfo: the rate the *hardware* would
 * have run at.  TONEDATA_TYPE_FIX samples carry no pitch -- the hardware reads
 * them one sample per output sample, so they play at whatever the mixer's rate
 * is.  At 48 kHz that would be three times too fast, so the mixer is told what
 * the GBA rate would have been and resamples them to it.
 */
void PortSampleRateSet(struct SoundInfo *soundInfo)
{
    u32 index = soundInfo->freq;
    s32 gbaSamplesPerVBlank;

    PortAudioStartup();

    /* Online session: the canonical device, whatever this one is. */
    if (sCanon) {
        CanonHeader(soundInfo);
        PortAudioCanonRender(soundInfo);
        return;
    }

    if (sRate == 0) {
        /* No audio device.  Leave the engine coherent -- the game reads these
         * back -- but with nothing downstream. */
        soundInfo->pcmSamplesPerVBlank = 1;
        soundInfo->pcmFreq = 1;
        soundInfo->divFreq = 0;
        soundInfo->pcmDmaPeriod = 1;
        return;
    }

    soundInfo->pcmSamplesPerVBlank = sNominal;
    soundInfo->pcmFreq = sRate;
    /* Kept only because it is part of the structure the game can read; the
     * mixer computes its own phase step in 64 bits, because this rounds to a
     * whole number and at 48 kHz that is 2.4 cents sharp on every note. */
    soundInfo->divFreq = (0x1000000 / soundInfo->pcmFreq + 1) >> 1;
    soundInfo->pcmDmaPeriod = 1;

    if (index >= 1 && index <= 12) {
        gbaSamplesPerVBlank = gPcmSamplesPerVBlankTable[index - 1];
        PortMixerSetFixedRate((597275 * gbaSamplesPerVBlank + 5000) / 10000);
    }
}

void m4aSoundVSync(void)
{
    struct SoundInfo *soundInfo = SOUND_INFO_PTR;
    const s16 *block = sSilence;
    int frames;

    if (sCanon) {
        CanonVSync(soundInfo);
        return;
    }
    if (sRate == 0)
        return;

    frames = sBlock;
    if (sToneHz) {
        RenderTone(sSilence, frames);
    } else if (soundInfo != NULL
            && soundInfo->ident >= ID_NUMBER && soundInfo->ident <= ID_NUMBER + 1) {
        int have = PortMixerTakeBlock(&block);

        if (have > 0) {
            frames = have;
        } else {
            /* SoundMain has not run since the last push.  That is normal for
             * the first frames of the boot and whenever the game switches
             * sound off; pushing the previous block again would repeat it. */
            block = sSilence;
            memset(sSilence, 0, (size_t)frames * 2 * sizeof(s16));
        }
    } else {
        memset(sSilence, 0, (size_t)frames * 2 * sizeof(s16));
    }

    /* Online play (platform/lmpnet.c): a replayed frame's sound was already
     * heard, or belongs to a timeline that was thrown away.  The sound
     * engine's state is replayed exactly; only the push is skipped. */
    if (!PortLnMuteAudio())
        PortAudioPush(block, frames);

    /* Pick the next block's length from the queue depth left after that push,
     * so the correction lands on the block that is actually late, and tell the
     * mixer before SoundMain runs again later in this frame. */
    ChooseBlockSize();
    PortMixerSetBlock(sBlock);
}

/* ------------------------------------------------------------------------- *
 * Device setup.
 *
 * This runs from src/init.c, ahead of m4aSoundInit, because SoundInit ->
 * SampleFreqSet needs the device rate to size the mixer and asking for it later
 * would mean reconfiguring mid-song.
 * ------------------------------------------------------------------------- */

void PortAudioStartup(void)
{
    if (sStarted)
        return;
    sStarted = 1;
    sRate = PortAudioOpenDevice();
    if (sRate == 0) {
        PortUnimplemented("no audio device -- the host granted none");
        return;
    }
    sNominal = (sRate + 30) / 60;
    if (sNominal > PORT_AUDIO_MAX_BLOCK)
        sNominal = PORT_AUDIO_MAX_BLOCK;
    sBlock = sNominal;
    if (!sCanon)
        PortMixerInit(sRate, sNominal);
    PortLog("[katam-port] audio: %d Hz, %d samples per frame", sRate, sNominal);
}

#ifdef __cplusplus
}
#endif
