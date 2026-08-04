#ifndef GUARD_PORT_AUDIO_H
#define GUARD_PORT_AUDIO_H

#include "gba/types.h"

/* --- transport (platform/audio_out.c) ------------------------------------
 *
 * Open the device, then push one block of interleaved stereo s16 per game
 * frame.  Everything is push-driven because the game clocks the audio: m4a
 * renders exactly one VBlank's worth of samples and there is no callback to
 * hand them to.
 *
 * PortAudioOpenDevice returns the device's real sample rate, or 0 if there is
 * no audio at all (no AudioContext, or the page refused one).  It is safe to
 * call every frame; only the first call does anything.  The rate has to be
 * known before the mixer is configured, which is why this is separate from the
 * first push.
 */
int  PortAudioOpenDevice(void);
int  PortAudioSampleRate(void);
void PortAudioPush(const s16 *interleaved, int frames);
/* Frames the device has queued but not yet played.  The producer uses this to
 * track the device clock; see PortAudioBlockSamples. */
int  PortAudioQueuedFrames(void);
int  PortAudioUnderruns(void);

/* --- the sound engine (platform/audio.c, platform/m4a_mixer.c) ----------- */

/* Samples this frame's block should contain, nudged around the nominal
 * hostRate/60 to keep the output queue at its target depth. */
int  PortAudioBlockSamples(void);

/* A 440 Hz square, pushed in place of the mixer's output while it is on.
 * This exists because "no sound" has two entirely different causes -- a mixer
 * that produces silence, and a transport that drops everything -- and they are
 * indistinguishable from the couch.  Turning this on takes m4a out of the
 * picture completely.  Exported to JS as _PortAudioTestTone.
 *   0   off
 *   >0  frequency in Hz */
void PortAudioTestTone(u32 hz);

#endif /* GUARD_PORT_AUDIO_H */
