/* The sound driver's platform half: what m4a needs that the GBA gave it for
 * free, and the once-a-frame decision of what to hand the speakers.
 *
 * src/m4a.c compiles as written -- tools/portify.py puts it back into the
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

#include <string.h>

#include "port/port.h"
#include "port/audio.h"
#include "gba/gba.h"
#include "gba/m4a.h"

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
 * The hooks src/m4a.c calls into.
 * ------------------------------------------------------------------------- */

/* Replaces the body of SampleFreqSet (see tools/portify.py).
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
        PortUnimplemented("no audio device -- the page granted no AudioContext");
        return;
    }
    sNominal = (sRate + 30) / 60;
    if (sNominal > PORT_AUDIO_MAX_BLOCK)
        sNominal = PORT_AUDIO_MAX_BLOCK;
    sBlock = sNominal;
    PortMixerInit(sRate, sNominal);
    PortLog("[katam-port] audio: %d Hz, %d samples per frame", sRate, sNominal);
}
