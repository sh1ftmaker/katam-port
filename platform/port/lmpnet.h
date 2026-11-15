#ifndef GUARD_PORT_LMPNET_H
#define GUARD_PORT_LMPNET_H
/* Online play as local multiplayer with the other players' pads on the far
 * side of a network: up to four Kirbys over up to four devices
 * (platform/lmpnet.c says why and how; "slots" below says who plays which).
 *
 * Off by default.  Nothing here runs until PortLnStart: every hook the frame
 * loop and the game call tests gPortLnOn first, and with it zero the build is
 * the build it was (the goldens prove it).
 *
 * Roles.  The HOST simulates the authoritative timeline and never rewinds:
 * at the latch of frame f it seals (its own keys for f, the guest's keys for
 * f if they have arrived, else the guest's last known keys) plus any events
 * scheduled for f.  The GUEST runs ahead of the host, applies its own keys at
 * once, predicts the host's as "same as the last sealed", and rewinds to the
 * nearest keyframe and replays whenever a sealed frame differs from what it
 * simulated.  Both hash everything a snapshot restores at the top of every
 * frame; the guest compares against the host's hashes once a frame is final.
 *
 * The transport is not here: the caller moves sealed frames, inputs, hashes
 * and images between the two engines (tools/netproof/ is one such caller,
 * over a simulated link). */

#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

#define PORT_LN_HOST  1
#define PORT_LN_GUEST 2

/* PortLnStart flags */
#define PORT_LN_F_HASH_EVERY   0x01   /* hash every frame (else every 16th)   */
#define PORT_LN_F_HOST_KEYS    0x02   /* keyframes on the host too (tests)    */
#define PORT_LN_F_NO_SRAM_KF   0x04   /* leave SRAM out of keyframes (tests)  */
#define PORT_LN_F_LOCKSTEP     0x08   /* guest: never predict (R0 control)    */
#define PORT_LN_F_NO_AUDIO_CU  0x10   /* keep pushing audio from replayed frames */

/* Event types (sealed with the frame they apply at; replayed identically). */
#define PORT_LN_EV_WARP    1   /* a=player b=room c=cellX d=cellY            */
#define PORT_LN_EV_VIEW    2   /* a=w b=h: the session (sim) view            */
#define PORT_LN_EV_LMP     3   /* a=humans: local-multiplayer setting        */
#define PORT_LN_EV_POKE    4   /* a=GBA address b=byte (tests: an "option")  */
#define PORT_LN_EV_HUMAN   5   /* a=slot 1..3 b=1 joins / 0 leaves (localmp.c)
                                * c=owner: 0 = unchanged (a test's bare join),
                                * else 1 + device*4 + seat (see "slots" below) */
#define PORT_LN_EV_PAINT   6   /* a=slot 0..3 b=paint (PORT_PAINT_*, kirby_paint.c) */
#define PORT_LN_EV_ROOMS   7   /* a=1: rooms mode (localmp.c), the host's choice,
                                * sealed at the session's start              */

/* --- slots, devices, seats -------------------------------------------------
 * Up to four devices in a star: the host is device 0, guests 1..3 (the host
 * numbers them).  Each device has up to four local SEATS: seat 0 is the
 * console's own keys, seats 1..3 the pads platform/localmp.c's host keys
 * carry (PortLmpSetHostKeys).  The session maps seats onto Kirby SLOTS: slot
 * 0 is always the host's seat 0; slots 1..3 are owned by a (device, seat) or
 * are the CPU's.  Ownership is session state -- carried by sealed
 * PORT_LN_EV_HUMAN events (c = owner), held in keyframes and images, hashed
 * -- and only the host decides it (PortLnSeatRequest, PortLnDeviceGone).
 *
 * Until the host first calls one of those two, slot 1 is device 1 seat 0 and
 * nothing is reconciled: a caller that predates slots (two devices, the
 * legacy PortLnLocalInput / PortLnRemoteInput) sees exactly what it did. */
#define PORT_LN_MAX_DEV   4
#define PORT_LN_OWN(dev, seat) (1 + (dev) * 4 + (seat))

extern int gPortLnOn;

int  PortLnStart(int role, int kfEvery, int kfCount, int flags);
void PortLnStop(void);
int  PortLnActive(void);
int  PortLnRole(void);
int  PortLnCatchingUp(void);
/* Rooms mode on a guest (lmpnet.c "the canonical shadow"): the shadow is the
 * live state now -- nothing of this device's may reach it. */
int  PortLnCanonRunning(void);
void PortLnSetCanonBatch(int frames);
int  PortLnCanonPoke(u32 addr, u32 xorMask);     /* tests */

/* --- inputs ------------------------------------------------------------- */
void PortLnLocalInput(u32 frame, u16 keys);          /* own seat 0's pad      */
void PortLnRemoteInput(u32 frame, u16 keys);         /* host: device 1 seat 0 */
void PortLnLocalInputSeat(u32 frame, int seat, u16 keys);
void PortLnRemoteInputSeat(u32 frame, int dev, int seat, u16 keys); /* host */
int  PortLnEvent(int type, int a, int b, int c, int d); /* host: at next latch */

/* --- slots (see above) ---------------------------------------------------- */
/* host: device `dev` wants its seat `seat` to play (on) or not (off).  The
 * slot it had last if free, else the lowest free one.  Returns the slot, or
 * -1 (on: all four taken; off: it had none).  Sealed at the next latch. */
int  PortLnSeatRequest(int dev, int seat, int on);
void PortLnDeviceGone(int dev);              /* host: its slots to the CPU   */
int  PortLnFreeSlots(void);                  /* host: slots nobody has        */
int  PortLnSlotWant(int slot);               /* host: decided owner, -1 CPU   */
int  PortLnSlotOwner(int slot);              /* applied: dev*4+seat, -1 CPU   */
int  PortLnSeatSlot(int dev, int seat);      /* applied: its slot, or -1      */
void PortLnSetSelf(int dev);                 /* guest: the id the host gave   */
int  PortLnSelf(void);
int  PortLnSelfSeats(void);                  /* bit per seat this device plays or asked for */
/* Bit per Kirby slot this device's people play now (applied ownership, the
 * frame being simulated); 0 when no session is on.  For presentation that
 * marks "local" players (a party HUD): host = bit 0 plus its own seats. */
int  PortLnSelfSlots(void);
/* This device's own seat, asked for by its controllers (platform/localmp.c's
 * PortLmpRequestHuman routes here during a session).  The host decides at
 * once; a guest queues it for the page to send (PortLnSeatAskPoll).  Returns
 * 1 if taken up (not: full on the host, bad seat). */
int  PortLnSeatAsk(int seat, int on);
int  PortLnSeatAskPoll(void);                /* guest: 1 + seat + 4*on, 0 none */

/* host: the sealed record of a frame it has latched.  out: k0, k1, nev,
 * ontime (device 1 seat 0's key was on time), hashLo, hashHi, hashed, k2,
 * k3.  Returns 0 if not sealed (yet, or no longer held). */
int  PortLnSealedGet(u32 frame, u32 *out);
int  PortLnSealedEventGet(u32 frame, int idx, s32 *out);  /* type,a,b,c,d */
/* guest: a sealed frame from the host (its events first). */
void PortLnSealedEvent(u32 frame, int type, int a, int b, int c, int d);
void PortLnSealed(u32 frame, u16 k0, u16 k1, int nev);
void PortLnSealed4(u32 frame, u16 k0, u16 k1, u16 k2, u16 k3, int nev);
void PortLnRemoteHash(u32 frame, u32 lo, u32 hi);

/* --- state ----------------------------------------------------------------- */
int  PortLnHashAt(u32 frame, u32 *out);   /* lo, hi, then 2 words per region */
int  PortLnRegionCount(void);
const char *PortLnRegionNames(void);      /* comma-separated                 */
int  PortLnHashFinal(u32 frame);          /* guest: no longer subject to change */

/* Images: everything a keyframe holds, plus a header.  Taken / applied at the
 * top of the next frame (the only safe point). */
int  PortLnImageRequest(void);
u32  PortLnImageReady(void);              /* frame + 1, or 0                 */
u8  *PortLnImagePtr(void);
u32  PortLnImageBytes(void);
u8  *PortLnImageInBuffer(void);           /* the caller writes an image here */
int  PortLnImageLoad(u32 len);
/* Tests (R5): what an image load must restore besides memory, one bit each. */
#define PORT_LN_SKIP_LMP      0x01   /* the local-multiplayer switch       */
#define PORT_LN_SKIP_MENUS    0x02   /* the new-menus switch (pause SETTINGS) */
#define PORT_LN_SKIP_SRAM     0x04   /* save memory                         */
#define PORT_LN_SKIP_SRAMLOAD 0x08   /* "the save is already loaded"        */
#define PORT_LN_SKIP_VIEW     0x10   /* the session view's event list       */
#define PORT_LN_SKIP_HOST     0x20   /* platform/menu_poc.c's host state     */
#define PORT_LN_SKIP_RANDO    0x40   /* the randomizer's settings            */
#define PORT_LN_SKIP_OWN      0x80   /* the slot ownership                   */
void PortLnSetImageSkip(int mask);

/* Diagnostics: a full image of the state at the top of `frame`, refreshed
 * every time that frame's top runs (the last pass wins). */
void PortLnDumpAt(u32 frame);
u32  PortLnDumpReady(void);               /* frame + 1 of the dump held      */
u8  *PortLnDumpPtr(void);

void PortLnForceRewind(u32 depth);         /* tests: rewind next top         */

/* --- telemetry --------------------------------------------------------- */
u32 *PortLnTelemetry(void);
int  PortLnTelemetryWords(void);
const char *PortLnTelemetryNames(void);
/* Correction records (rewinds and image loads), newest last. */
int  PortLnCorrectionCount(void);
int  PortLnCorrection(int i, s32 *out);   /* see kCorrFields in .c           */
const char *PortLnCorrectionNames(void);
/* Kirby slot i: x, y (px), room, hp, anim, flags */
int  PortLnKirby(int i, s32 *out);
int  PortLnObserve(s32 *out);
int  PortLnFindEnemy(s32 *out);            /* tests: a live enemy object    */
int  PortLnSnapPart(int i, u32 *out);      /* base, offset, size of part i  */              /* scenario coverage, see .c      */

/* --- hooks (called by the frame loop / the game; no-ops when off) ------- */
void PortLnFrame(void);                    /* platform/main.c, frame top     */
int  PortLnKeyOverride(u16 *keys);         /* platform/main.c, the latch     */
void PortLnNoteSoundSlow(u32 id, const void *mplayInfo); /* game/src/m4a.c MPlayStart */
u32  PortLnLatchFrame(void);               /* the latest latch, or ~0         */
u32  PortLnSealedFrontier(void);           /* frames below it are sealed       */
void PortLnNoteSramWriteSlow(u32 bytes);   /* platform/sram.c WriteSram      */
void PortLnNoteSettingSlow(int row);       /* platform/menu_poc.c, a value   */
int  PortLnMuteAudio(void);                /* platform/audio.c: replayed frame */
int  PortLnAudioMuted(void);               /* platform/m4a_mixer.c: block unheard */

/* platform/lnsession.c: the device side of a session (canonical audio,
 * the page's view experiment held off, the guest's own save kept out). */
void PortLnDeviceBegin(int role);
void PortLnDeviceEnd(void);

#define PortLnNoteSound(id, mp)  do { if (gPortLnOn) PortLnNoteSoundSlow(id, mp); } while (0)
#define PortLnNoteSramWrite(n)   do { if (gPortLnOn) PortLnNoteSramWriteSlow(n); } while (0)
#define PortLnNoteSetting(r)     do { if (gPortLnOn) PortLnNoteSettingSlow(r); } while (0)

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_LMPNET_H */
