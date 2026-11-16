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
#include "port/wide.h"

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
#define PORT_LN_EV_HUMAN   5   /* a=slot 1..PORT_LN_SLOTS-1 b=1 joins / 0 leaves (localmp.c)
                                * c=owner: 0 = unchanged (a test's bare join),
                                * else 1 + device*4 + seat (see "slots" below) */
#define PORT_LN_EV_PAINT   6   /* a=slot 0..3 b=paint (PORT_PAINT_*, kirby_paint.c)
                                * c=feet (PORT_PAINT_FEET_ON | BGR555, 0 = with
                                * the body)                                  */
#define PORT_LN_EV_ROOMS   7   /* a=1: rooms mode (localmp.c), the host's choice,
                                * sealed at the session's start              */
#define PORT_LN_EV_PSET    8   /* a=slot 0..3 b=that slot's device's settings
                                * (platform/menu_poc.c PortPocSettingsWord),
                                * c=its other sections' online rows (...ExtWord),
                                * for the pause SETTINGS page; only with the
                                * menus on                                   */
#define PORT_LN_EV_NAME    9   /* a=slot b,c=its player's name, 4 + 4 letters
                                * (low byte first), as the host has it from
                                * the cards: drawn by the pause SETTINGS
                                * page's ONLINE list; only with the menus on */
#define PORT_LN_EV_KIRBYS 10   /* a=Kirbys in all, 4..KATAM_KIRBYS: the host's
                                * CPU KIRBYS setting (platform/wide.c
                                * PortWideSetTarget); ignored by the default
                                * build.  The count in play never drops below
                                * a person's Kirby (wide.c "People"), so seats
                                * need no event of their own: a seat past the
                                * count is an ordinary EV_HUMAN, held in the
                                * simulation until its Kirby has been made */
#define PORT_LN_EV_CPUAI  11   /* a=the CPU Kirbys' modes, 2 bits per slot
                                * (port/cpu_ai.h): the host's CPU AI
                                * setting, sealed whenever it differs from
                                * the session's                              */

/* --- slots, devices, seats -------------------------------------------------
 * PORT_LN_SLOTS Kirby slots (4; the wide build: one per Kirby slot,
 * KATAM_KIRBYS) over up to PORT_LN_MAX_DEV devices (as many as slots) in a
 * star: the host is device 0, guests 1..PORT_LN_MAX_DEV-1 (the host numbers
 * them; on a w8 module devices 4..7 are numbered exactly like 1..3).  Each device has up to four local SEATS: seat 0 is the
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
#define PORT_LN_SLOTS     PORT_LMP_SLOTS
#define PORT_LN_MAX_DEV   PORT_LN_SLOTS
#define PORT_LN_OWN(dev, seat) (1 + (dev) * 4 + (seat))   /* <= 64: a u8 */

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

/* The pause menu online is this device's own (lmpnet.c "the pause overlay"):
 * START in play opens it on this device only, over a private copy of the
 * game (the OVERLAY FORK) that runs the game's own pause screens with this
 * device's pad, while the shared game runs on behind it.
 *   PortLnOverlayRunning  the fork is the state in memory now (one frame of
 *                         it each frame while open: never drawn as the
 *                         game, never heard, never part of the session)
 *   PortLnOverlayQuiet    the live frame after it: simulated, presented
 *                         with the fork's picture (no composition of its own)
 *   PortLnOverlayOpen     this device shows its pause overlay (0 closed, 1
 *                         opening, 2 open)
 *   PortLnShadowRunning   the canonical shadow or the fork: nothing of this
 *                         device's (sound, telemetry, digests) may come from it */
int  PortLnOverlayRunning(void);
int  PortLnOverlayQuiet(void);
/* the fork runs but is not the picture (its pause has not begun yet) */
int  PortLnOverlayHidden(void);
int  PortLnOverlayOpen(void);
/* the seat (0..3) of this device whose overlay is up, -1 none: its player
 * card is the one its SETTINGS > PLAYERS > PLAYER CARD edits */
int  PortLnOverlaySeat(void);
int  PortLnShadowRunning(void);
/* menu_poc.c: an edit made on the overlay's SETTINGS page, carried out on
 * this device at the next live frame (as a final frame's edit is). */
void PortLnOverlayEdit(int item, int dir);
/* Tests: the overlay as of its last frame -- [0] state (0/1/2), [1] seat,
 * [2] slot, [3] the fork's pauser, [4] its pause page (gPauseMenus[].menuId),
 * [5] the SETTINGS page's row (-1 off it), [6] the settings model
 * (PortPocScreenModel), [7] the fork's frame, [8] opens, [9] closes, [10]
 * refused (START where the fork could not pause), [11] fork frames run,
 * [12] the help page's room line up (PortPocHelpLineShown), [13] the
 * settings word its page shows (PortPocShownWord: this device's own).
 * Returns the words written (14). */
int  PortLnOverlayObserve(s32 *out);

/* --- inputs ------------------------------------------------------------- */
void PortLnLocalInput(u32 frame, u16 keys);          /* own seat 0's pad      */
void PortLnRemoteInput(u32 frame, u16 keys);         /* host: device 1 seat 0 */
/* Returns the keys that go into the session for that seat (what a guest
 * sends): the pad, less START in play and while this device's pause overlay
 * has the seat (lmpnet.c "the pause overlay"). */
u32  PortLnLocalInputSeat(u32 frame, int seat, u16 keys);
void PortLnRemoteInputSeat(u32 frame, int dev, int seat, u16 keys); /* host */
int  PortLnEvent(int type, int a, int b, int c, int d); /* host: at next latch */

/* --- slots (see above) ---------------------------------------------------- */
/* host: device `dev` wants its seat `seat` to play (on) or not (off).  The
 * slot it had last if free, else the lowest free one (the wide build: a slot
 * whose Kirby exists first; past the live count the join is held in the
 * simulation until the count, raised by it, has made that Kirby).  Returns
 * the slot, or -1 (on: every slot taken; off: it had none).  Sealed at the
 * next latch. */
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
/* The same for every slot, PORT_LN_SEALED_WORDS words: out[0..8] exactly as
 * PortLnSealedGet, out[9] = PORT_LN_SLOTS, out[10 + i] = the keys of slot
 * 4 + i for i < PORT_LN_SLOTS - 4 (none in the default build).  Returns 0 if
 * not sealed. */
#define PORT_LN_SEALED_WORDS (10 + PORT_LN_SLOTS - 4)
int  PortLnSealedGetN(u32 frame, u32 *out);
int  PortLnSealedEventGet(u32 frame, int idx, s32 *out);  /* type,a,b,c,d */
/* guest: a sealed frame from the host (its events first). */
void PortLnSealedEvent(u32 frame, int type, int a, int b, int c, int d);
void PortLnSealed(u32 frame, u16 k0, u16 k1, int nev);
void PortLnSealed4(u32 frame, u16 k0, u16 k1, u16 k2, u16 k3, int nev);
/* guest: a sealed frame with n slots' keys (keys[0..n-1], slot order; slots
 * from n on are 0).  n is at most PORT_LN_SLOTS; PortLnSealed4 is n = 4. */
void PortLnSealedN(u32 frame, const u16 *keys, int n, int nev);
/* What this build's sessions are made of (the page and the rig ask): */
int  PortLnSlots(void);                      /* PORT_LN_SLOTS                 */
int  PortLnMaxDevices(void);                 /* PORT_LN_MAX_DEV               */
/* host: how many Kirby slots seats may be given, 1..PORT_LN_SLOTS (slot 0 is
 * the host's).  4 until the caller raises it: the transport must carry every
 * seated slot's keys (PortLnSealedGetN / PortLnSealedN), and today's wire
 * carries k0..k3.  Kept across sessions; returns the value set.
 * PortLnFreeSlots counts within it. */
int  PortLnSetSeatSlots(int n);
int  PortLnSeatSlots(void);
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

/* Leaving a session without reloading (the page's "Leave online game"):
 * stop it and, at the next frame top, put back the picture of this device's
 * own game taken just after power-on -- it boots to the title again, with its
 * own save (a guest's from before the host's image), its own view, and the
 * frame counter running on.  0 if there is no picture (then only stopped). */
int  PortLnGoHome(void);
/* The same outside a session: the pause menu's MAIN MENU.  0 if there is no
 * picture (a replay, or a session). */
int  PortLnGoHomeLocal(void);
/* The host, before its session's first frame: the room code (4 letters),
 * which the session carries for the pause menus (platform/menu_poc.c). */
void PortLnSetRoomCode(int c0, int c1, int c2, int c3);
/* The host: a guest device's settings as a session copy (web/ln_session.js
 * `pset`), sealed for the slots that device plays (PORT_LN_EV_PSET). */
void PortLnDeviceSettings(int dev, u32 word, u32 ext);
int  PortLnHomeReady(void);
u32  PortLnHomeLoads(void);
void PortLnHomeFrame(void);               /* platform/main.c, after PortLnFrame */
/* game/src/logo.c: 1 on the first logo frame after a home load (the logo
 * then goes straight to the title); PortLnHomeExpressed counts them. */
int  PortLnHomeExpress(void);
u32  PortLnHomeExpressed(void);

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
