/* Audio transport: getting a block of PCM from the mixer to the speakers.
 *
 * This file knows nothing about m4a and nothing about the host.  Its whole job
 * is "here are N stereo frames, play them", and the reason it is separate from
 * audio.c is that the two failed independently while this was being written: a
 * mixer that produces silence and a transport that drops everything look
 * identical from the couch.
 *
 * The queue itself belongs to the host -- an AudioWorklet's ring in the
 * browser (platform/web/audio_out_web.c), an SDL callback's ring natively
 * (platform/native/audio_sdl.c).  This is the five-function interface between
 * them, plus the s16-to-float conversion, which is done here so that neither
 * host has to know the mixer's output format.
 *
 * Push, not pull
 * --------------
 * The game clocks the audio, not the other way round: m4a renders exactly one
 * frame's worth of samples per VBlank and there is nowhere to call back into.
 * So the transport is a queue, and PortAudioQueued() reports its depth so the
 * producer can widen or narrow its blocks to track the device clock.  See
 * ChooseBlockSize() in audio.c.
 */

#include "port/port.h"
#include "port/backend.h"
#include "port/audio.h"

/* How much the host should be willing to hold, in game frames.  Four is ~67 ms:
 * long enough that a single slow frame (a room transition decompressing
 * tilesets, say) does not become an audible hole, short enough that input still
 * feels attached to its sound effect. */
#define PORT_AUDIO_RING_FRAMES 4

/* One frame of stereo float, staged here rather than on the stack: the block
 * is up to ~1600 samples and on the web the mixer already runs deep under
 * Asyncify. */
#define PORT_AUDIO_MAX_FRAME 4096
static float sOut[PORT_AUDIO_MAX_FRAME * 2];

static int sRate;
static int sOpened;

int PortAudioOpenDevice(void)
{
    if (!sOpened) {
        PortAudioOpen(PORT_AUDIO_RING_FRAMES);
        sOpened = 1;
        sRate = PortAudioRate();
    }
    return sRate;
}

int PortAudioSampleRate(void)
{
    return sRate;
}

void PortAudioPush(const s16 *interleaved, int frames)
{
    int i;

    if (frames <= 0 || sRate == 0)
        return;
    if (frames > PORT_AUDIO_MAX_FRAME)
        frames = PORT_AUDIO_MAX_FRAME;

    /* s16 -> float here rather than in the host: the conversion is one
     * multiply per sample and doing it on this side keeps every transport free
     * of any assumption about the mixer's output format. */
    for (i = 0; i < frames * 2; i++)
        sOut[i] = (float)interleaved[i] * (1.0f / 32768.0f);

    PortAudioSubmit(sOut, frames);
}

int PortAudioQueuedFrames(void)
{
    return PortAudioQueued();
}
