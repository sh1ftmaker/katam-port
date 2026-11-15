#ifndef PORT_AUDIO_EAR_H
#define PORT_AUDIO_EAR_H
/* Online sessions: what the guest's speakers hear (platform/audio_ear.c).
 *
 * Device-local, outside the image: the in-image sound engine stays exactly
 * canonical, and this only decides which of its rendered samples reach the
 * speakers, and when.  Off a session, and on the host, nothing here runs. */

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SoundInfo;

#define PORT_EAR_PLAYERS 4          /* music players that can be delayed   */
#define PORT_EAR_GROUPS  5          /* + one for channels with no owner    */
#define PORT_EAR_BLOCK   800        /* the canonical block (platform/audio.c) */

/* platform/m4a_mixer.c, per render in a session */
int  PortEarActive(int frames);              /* group rendering this render? */
int  PortEarOwner(struct SoundInfo *si, int ch);    /* channel -> group     */
u32  PortEarReplayMask(void);                 /* unheard render: groups to mix */
s32 *PortEarAcc(int group, int right);        /* group accumulators        */
void PortEarClear(u32 groups);
/* After the channels: `audible` 0 stores what the replay needs; 1 composes
 * the speakers' block and adds it into outR/outL (which hold the reverb). */
void PortEarRendered(u32 used, int audible, s32 *outR, s32 *outL);

/* platform/lmpnet.c */
void PortEarLatch(u32 latch);                 /* a frame is simulated       */
void PortEarNoteStart(u32 latch, const void *mplayInfo, u32 song);
void PortEarReplay(u32 from, u32 at);         /* a rewind or image, from..at */
void PortEarFinal(u32 latch);                 /* that frame is final        */

/* platform/audio.c */
void PortEarSessionEnd(void);

/* The reconciler on (default) or off (speakers get the current timeline's
 * block as is, which is what the build before it did); the accounting runs
 * either way.  Counters: see kEarNames in audio_ear.c. */
void PortEarSet(int on);
u32 *PortEarStats(void);
void PortEarProbe(int on);                    /* measuring: see audio_ear.c */
u32  PortEarProbeCount(void);
void *PortEarProbeRing(void);
int  PortEarProbeCap(void);
u32  PortEarStartLog(int which);
u32  PortEarDebug(int which);                  /* count, ring, capacity */               /* count, ring, capacity */
int  PortEarStatsWords(void);
const char *PortEarStatsNames(void);

#ifdef __cplusplus
}
#endif
#endif
