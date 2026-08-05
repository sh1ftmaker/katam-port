#ifndef GUARD_PORT_AUDIO_H
#define GUARD_PORT_AUDIO_H
/* C linkage for the 64-bit builds.
 *
 * They compile the game as C++ so that its structures keep 4-byte pointer
 * members (docs/SIXTYFOUR.md), and tools/cxxify.py gives every game header and
 * source C linkage so the C++ build links by the same rules the C builds do.
 * This header declares the seam between the two, so it has to say the same
 * thing -- otherwise the game calls a mangled name and the platform defines an
 * unmangled one, or the reverse.  A no-op in C. */
#ifdef __cplusplus
extern "C" {
#endif


#include "gba/types.h"

struct SoundInfo;

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

/* --- the sound engine (platform/audio.c, platform/m4a_mixer.c) -----------
 *
 * The block is one frame's worth of samples, so the ceiling only has to cover
 * the highest device rate anyone will present divided by 60, plus the few per
 * cent the clock correction can add.  2048 covers 96 kHz. */
#define PORT_AUDIO_MAX_BLOCK 2048

/* Samples this frame's block should contain, nudged around the nominal
 * hostRate/60 to keep the output queue at its target depth. */
int  PortAudioBlockSamples(void);

/* The mixer proper (platform/m4a_mixer.c).  SoundMain renders a block; the
 * VBlank handler takes it out one frame later, which is the same one-block
 * delay the hardware's PCM DMA has. */
void PortMixerInit(int sampleRate, int blockSamples);
void PortMixerSetBlock(int blockSamples);
void PortMixerSetFixedRate(int gbaPcmFreq);
void PortMixerRender(struct SoundInfo *soundInfo);
int  PortMixerTakeBlock(const s16 **out);
void PortMixerSilence(void);

/* Opens the device and sizes the mixer.  Called from the patched
 * SampleFreqSet, which is the first thing SoundInit does -- everything after
 * it needs the rate.  Safe to call repeatedly. */
void PortAudioStartup(void);

/* A 440 Hz square, pushed in place of the mixer's output while it is on.
 * This exists because "no sound" has two entirely different causes -- a mixer
 * that produces silence, and a transport that drops everything -- and they are
 * indistinguishable from the couch.  Turning this on takes m4a out of the
 * picture completely.  Exported to JS as _PortAudioTestTone.
 *   0   off
 *   >0  frequency in Hz */
void PortAudioTestTone(u32 hz);

/* The two symbols the sound driver reaches back for.
 *
 * tools/portify.py's M4A_PATCHES injects declarations of these into the game's
 * own m4a.c -- gXcmdTable is redirected to the port's copy because a ROM entry
 * is an ARM code address, and SampleFreqSet is replaced by PortSampleRateSet
 * because the port's mixer runs at the host's rate rather than the console's.
 *
 * Declaring them here as well is what keeps the 64-bit builds linking.  Over
 * there the game has C linkage and platform/audio.c and platform/m4a_mixer.c
 * are compiled as C++, so without a shared declaration in a header that both
 * sides see, the definitions mangle and gPortXcmdTable -- being const -- also
 * takes internal linkage.  Both files already include this header.
 *
 * The types are spelled out rather than named: this header includes only
 * gba/types.h, so XcmdFunc is not in scope here, and the tags are
 * forward-declared because only their addresses are involved.  `const XcmdFunc
 * []` is an array of const function pointers, which is what this expands to. */
struct SoundInfo;
struct MusicPlayerInfo;
struct MusicPlayerTrack;
void PortSampleRateSet(struct SoundInfo *soundInfo);
extern void (*const gPortXcmdTable[])(struct MusicPlayerInfo *,
                                      struct MusicPlayerTrack *);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_AUDIO_H */
