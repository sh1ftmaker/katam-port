/* Sound: the m4a API, and the clocking of audio out of the port.
 *
 * The MP2K driver is asm/m4a_asm.s -- 44 hand-written ARM functions that were
 * never a decompilation target.  The port drops src/m4a.c and answers the whole
 * m4a API here; every call is a no-op that keeps the game's own state machine
 * happy, because the game asks for songs and fades constantly and expects those
 * calls to return.
 *
 * Keeping the signatures exact matters -- wasm validates them at link time.
 *
 * What is here on top of the stubs is the transport clocking: the device is
 * opened at m4aSoundInit and one block of PCM is pushed per VBlank.  With the
 * mixer still absent that block is silence, or a square wave when the test tone
 * is on -- which is the point.  "No sound" has two independent causes, a mixer
 * that produces nothing and a transport that drops what it is given, and this
 * half can be proved on its own.  See platform/audio_out.c.
 */

#include <string.h>

#include "port/port.h"
#include "port/audio.h"
#include "gba/gba.h"
#include "gba/m4a.h"

/* The game reads back player state after starting a song, so these have to
 * exist as real storage rather than stubs. */
struct MusicPlayerInfo gMPlayInfo_0;
struct MusicPlayerInfo gMPlayInfo_1;
struct MusicPlayerInfo gMPlayInfo_2;
struct MusicPlayerInfo gMPlayInfo_3;
struct SoundInfo gSoundInfo;
u8 gMPlayMemAccArea[0x10];

/* ------------------------------------------------------------------------- *
 * Block clocking.
 * ------------------------------------------------------------------------- */

#define PORT_AUDIO_MAX_BLOCK 2048

static int sRate;            /* device rate, 0 until the page grants audio */
static int sNominal;         /* samples per frame at exactly 60 Hz */
static int sBlock;           /* samples in the block being pushed now */
static u32 sToneHz;
static u32 sTonePhase;

static s16 sBlockPcm[PORT_AUDIO_MAX_BLOCK * 2];

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
 * either way to hold the queue at its target depth -- the same correction every
 * console port that pushes rather than pulls ends up needing.
 *
 * Two frames of queue is the target: one is the block currently playing, one is
 * headroom for a main thread that just spent 20 ms decompressing a tileset.
 */
static void ChooseBlockSize(void)
{
    int queued = PortAudioQueuedFrames();
    int err = sNominal * 2 - queued;
    int adj = err / 8;
    int limit = sNominal / 32;

    if (adj > limit) adj = limit;
    if (adj < -limit) adj = -limit;

    sBlock = sNominal + adj;
    if (sBlock < 16)
        sBlock = 16;
    if (sBlock > PORT_AUDIO_MAX_BLOCK)
        sBlock = PORT_AUDIO_MAX_BLOCK;
}

static void RenderTone(s16 *dst, int frames)
{
    /* Phase in 0.32 fixed point, so the frequency is exact at any device rate
     * and the wave does not walk between blocks. */
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
 * The m4a API.
 * ------------------------------------------------------------------------- */

void m4aSoundInit(void)
{
    PortUnimplemented("sound engine (m4a) -- running silent");

    sRate = PortAudioOpenDevice();
    if (sRate == 0) {
        PortUnimplemented("no audio device -- the page has no AudioContext");
        return;
    }
    sNominal = (sRate + 30) / 60;
    if (sNominal > PORT_AUDIO_MAX_BLOCK)
        sNominal = PORT_AUDIO_MAX_BLOCK;
    sBlock = sNominal;
}

void m4aSoundMain(void)
{
    /* GameLoop calls this at the tail of its VBlank work, immediately before
     * spinning until VBlank ends.  On hardware the mixer's own runtime is part
     * of what ends it, so this is the backstop that guarantees the spin exits
     * even on a frame that transferred almost nothing. */
    PortVBlankEnd();
}

/* Once per VBlank, from the game's own VBlank handler.  On hardware this flips
 * the PCM DMA to the next block; here it is where a block leaves the port.
 *
 * It is deliberately not m4aSoundMain: that has three call sites (GameLoop's
 * tail, the VBlank handler, and TasksExec) which are mutually excluded by
 * gMainFlags bit 0x1000000 and gExecSoundMain, so it runs once per frame today
 * but is not structurally guaranteed to.  m4aSoundVSync is. */
void m4aSoundVSync(void)
{
    if (sRate == 0)
        return;

    if (sToneHz)
        RenderTone(sBlockPcm, sBlock);
    else
        memset(sBlockPcm, 0, (size_t)sBlock * 2 * sizeof(s16));

    PortAudioPush(sBlockPcm, sBlock);

    /* Pick the next block's length from the queue depth left after that push,
     * so the correction lands on the block that is actually late. */
    ChooseBlockSize();
}

void m4aSoundVSyncOn(void) { }
void m4aSoundVSyncOff(void) { }
void m4aSoundMode(u32 mode) { (void)mode; }

void m4aSongNumStart(u16 n) { (void)n; }
void m4aSongNumStop(u16 n) { (void)n; }
void m4aSongNumContinue(u16 n) { (void)n; }
void m4aSongNumStartOrChange(u16 n) { (void)n; }
void m4aSongNumStartOrContinue(u16 n) { (void)n; }

void m4aMPlayAllStop(void) { }
void m4aMPlayAllContinue(void) { }
void m4aMPlayContinue(struct MusicPlayerInfo *info) { (void)info; }
void m4aMPlayStop(struct MusicPlayerInfo *info) { (void)info; }
void m4aMPlayStart(struct MusicPlayerInfo *info, struct SongHeader *song)
{
    (void)info; (void)song;
}

void m4aMPlayFadeIn(struct MusicPlayerInfo *info, u16 speed) { (void)info; (void)speed; }
void m4aMPlayFadeOut(struct MusicPlayerInfo *info, u16 speed) { (void)info; (void)speed; }
void m4aMPlayFadeOutTemporarily(struct MusicPlayerInfo *info, u16 speed)
{
    (void)info; (void)speed;
}

void m4aMPlayTempoControl(struct MusicPlayerInfo *info, u16 tempo)
{
    (void)info; (void)tempo;
}
void m4aMPlayVolumeControl(struct MusicPlayerInfo *info, u16 trackBits, u16 volume)
{
    (void)info; (void)trackBits; (void)volume;
}
void m4aMPlayPitchControl(struct MusicPlayerInfo *info, u16 trackBits, s16 pitch)
{
    (void)info; (void)trackBits; (void)pitch;
}
void m4aMPlayPanpotControl(struct MusicPlayerInfo *info, u16 trackBits, s8 pan)
{
    (void)info; (void)trackBits; (void)pan;
}
void m4aMPlayModDepthSet(struct MusicPlayerInfo *info, u16 trackBits, u8 modDepth)
{
    (void)info; (void)trackBits; (void)modDepth;
}
void m4aMPlayLFOSpeedSet(struct MusicPlayerInfo *info, u16 trackBits, u8 lfoSpeed)
{
    (void)info; (void)trackBits; (void)lfoSpeed;
}
void m4aMPlayImmInit(struct MusicPlayerInfo *info) { (void)info; }
