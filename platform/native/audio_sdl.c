/* Audio transport, native side: an SDL callback and the ring it drains.
 *
 * The producer half is platform/audio_out.c and platform/audio.c, shared with
 * the web build.  Nothing about the mixer or the block clocking changes here;
 * only where the samples go.  The web build's queue lives inside an
 * AudioWorklet, this one is a plain single-producer single-consumer ring, and
 * both answer the same five functions in platform/port/backend.h.
 *
 * SDL_OpenAudioDevice with a callback rather than SDL_QueueAudio: the queue API
 * has no way to say "you are running dry" and no bound on how much it will
 * accumulate, and the block-size feedback in audio.c needs both.
 *
 * The ring is lock-free rather than wrapped in SDL_LockAudioDevice.  One
 * producer (the game thread, once a frame), one consumer (SDL's audio thread),
 * and the only shared state is two indices -- so a release store on the write
 * index and an acquire load on the read index is the whole synchronisation.
 * Taking the device lock would work too, but it stalls the audio thread behind
 * whatever the game happens to be doing, which is the thing that makes ports
 * like this crackle under load.
 */

#include <string.h>

#include <SDL.h>

#include "native.h"
#include "port/audio.h"

/* Frames of stereo float.  Sized from the ringFrames the transport asks for,
 * rounded up to a power of two so the wrap is a mask -- 8192 frames is 170 ms
 * at 48 kHz, four times the target depth, which leaves room for a frame that
 * took 60 ms to render without the producer ever having to drop a block. */
#define RING_FRAMES 8192
#define RING_MASK   (RING_FRAMES - 1)

static float sRing[RING_FRAMES * 2];
static SDL_atomic_t sWrite;         /* frames written, monotonic */
static SDL_atomic_t sRead;          /* frames read, monotonic */
static SDL_atomic_t sUnderruns;

static SDL_AudioDeviceID sDevice;
static int sRate;
static int sOpened;

/* The device thread.  Never blocks, never allocates, never calls back into the
 * port: if the ring is short it pads with silence and counts it, because a
 * callback that waits for the game is a callback that misses its deadline. */
static void SDLCALL Drain(void *userdata, Uint8 *stream, int lenBytes)
{
    float *out = (float *)stream;
    int want = lenBytes / (int)(2 * sizeof(float));
    int rd = SDL_AtomicGet(&sRead);
    int have = SDL_AtomicGet(&sWrite) - rd;
    int take = have < want ? have : want;
    int i;

    (void)userdata;

    for (i = 0; i < take; i++) {
        int slot = ((rd + i) & RING_MASK) * 2;

        out[i * 2]     = sRing[slot];
        out[i * 2 + 1] = sRing[slot + 1];
    }
    if (take < want) {
        memset(out + take * 2, 0,
               (size_t)(want - take) * 2 * sizeof(float));
        SDL_AtomicAdd(&sUnderruns, 1);
    }
    SDL_AtomicSet(&sRead, rd + take);
}

void PortAudioOpen(int ringFrames)
{
    SDL_AudioSpec want, got;

    (void)ringFrames;           /* the ring is a fixed power of two; see above */

    if (sOpened)
        return;
    sOpened = 1;

    if (gPortNativeNoAudio) {
        /* Rate 0 is a configuration the rest of the port supports: audio.c
         * leaves the engine coherent and pushes nothing.  It is not an error
         * path. */
        PortLog("[katam-port] audio disabled (--no-audio)");
        return;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        PortError("[katam-port] no audio: %s", SDL_GetError());
        return;
    }

    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    /* 512 frames is ~10.7 ms at 48 kHz -- under one game frame, so the queue
     * depth the producer reads back tracks something finer than the block it
     * is deciding about. */
    want.samples = 512;
    want.callback = Drain;

    /* Let SDL give us a different rate if the device insists, exactly as the
     * browser's AudioContext does; the mixer is configured from whatever comes
     * back.  Channels and format are not negotiable -- the ring is stereo
     * float -- so they are not in the allow-changes mask. */
    sDevice = SDL_OpenAudioDevice(NULL, 0, &want, &got,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE
                                  | SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (sDevice == 0) {
        PortError("[katam-port] no audio device: %s", SDL_GetError());
        return;
    }

    sRate = got.freq;
    SDL_PauseAudioDevice(sDevice, 0);
}

int PortAudioRate(void)
{
    return sRate;
}

int PortAudioQueued(void)
{
    if (sDevice == 0)
        return 0;
    return SDL_AtomicGet(&sWrite) - SDL_AtomicGet(&sRead);
}

int PortAudioUnderruns(void)
{
    return SDL_AtomicGet(&sUnderruns);
}

void PortAudioSubmit(const float *samples, int frames)
{
    int wr, space, i;

    if (sDevice == 0 || frames <= 0)
        return;

    wr = SDL_AtomicGet(&sWrite);
    space = RING_FRAMES - (wr - SDL_AtomicGet(&sRead));
    /* Overflow means the producer is ahead of the device.  Drop the tail of
     * the incoming block rather than cutting into what is already queued: that
     * is what plays next, and overwriting it is audible. */
    if (frames > space)
        frames = space;

    for (i = 0; i < frames; i++) {
        int slot = ((wr + i) & RING_MASK) * 2;

        sRing[slot]     = samples[i * 2];
        sRing[slot + 1] = samples[i * 2 + 1];
    }
    /* Publish after the samples are in place.  SDL_AtomicSet is a full barrier,
     * which is stronger than the release this needs and free on x86. */
    SDL_AtomicSet(&sWrite, wr + frames);
}

void PortNativeAudioClose(void)
{
    if (sDevice != 0) {
        SDL_CloseAudioDevice(sDevice);
        sDevice = 0;
    }
}
