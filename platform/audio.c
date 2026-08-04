/* Sound: deliberately absent.
 *
 * The MP2K driver is asm/m4a_asm.s -- 42 hand-written ARM functions that were
 * never a decompilation target.  Porting it is a self-contained project (either
 * a C reimplementation of the mixer or routing the sequencer at a host audio
 * backend), and none of it is needed to get the game playable.
 *
 * So the port drops src/m4a.c and answers the whole m4a API here.  Every call
 * is a no-op that keeps the game's own state machine happy: the game asks for
 * songs and fades constantly, and expects those calls to return.
 *
 * Keeping the signatures exact matters -- wasm validates them at link time.
 */

#include "port/port.h"
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

void m4aSoundInit(void)
{
    PortUnimplemented("sound engine (m4a) -- running silent");
}

void m4aSoundMain(void) { }
void m4aSoundVSync(void) { }
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
