#ifndef GUARD_PORT_MP_H
#define GUARD_PORT_MP_H
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

/* ---------------------------------------------------------------------------
 * The multiplayer seam.
 *
 * Kirby & The Amazing Mirror's link play is 32-bit multi-play SIO: up to four
 * consoles on one cable, one of them clocking it, every transfer moving one
 * halfword from each unit to all four at once.  The game's own driver for that
 * is already here and compiles -- src/multi_sio.c is Nintendo's MultiSio
 * library, src/multi_08030C94.c is the game's lockstep input exchange on top
 * of it.  What is missing is the cable.
 *
 * This header is the cable, with the wire left undefined.  A transport answers
 * one question -- "what did the other units put on the bus for this transfer?"
 * -- and everything above it is the game's own code, unmodified.  See
 * docs/MULTIPLAYER.md for the protocol the game speaks over it.
 *
 * Three properties this interface is shaped by, in order of how much they cost
 * to get wrong:
 *
 *   The seam is under the game, not in it.  The game never learns there is a
 *   transport.  platform/sio.c presents SIOCNT, SIOMLT_SEND and SIOMULTI0..3
 *   the way the hardware would and calls the game's own interrupt handler; the
 *   game's driver drives this interface without knowing it exists.
 *
 *   It is plain C.  The web build binds a JavaScript transport in mp.c and a
 *   native build can register a C one; neither shows up here.  No emscripten
 *   type, no Module, no promise.
 *
 *   It does not assume a wire.  `exchange` is the hardware's operation, so a
 *   same-process loopback implements it directly.  A networked transport can
 *   not answer synchronously and is not expected to: `poll` runs once a frame
 *   and is where a transport collects whatever arrived, and `exchange` then
 *   answers from that buffer.  A frame is 16 transfers (see PORT_SIO_SLOTS),
 *   so one frame of buffering is one network message, which is what every
 *   working GBA netplay implementation does.  Returning 0 from `exchange`
 *   stalls the cable for that slot, which is exactly what a real cable does
 *   when a unit is late, and which the game's driver already handles.
 * ------------------------------------------------------------------------- */

#define PORT_MP_PLAYERS 4

/* One multi-play transfer per slot, 16 slots per frame.  Not a guess: the
 * game's own MULTI_SIO_TIMER_COUNT works out to SYSTEM_CLOCK / 60 / 16, i.e.
 * the timer that clocks the cable is set to fire sixteen times a frame, which
 * is one MultiSio packet (14 halfwords plus the sync word) per frame with two
 * slots of slack.  See docs/MULTIPLAYER.md. */
#define PORT_SIO_SLOTS 16

/* What a transport knows about the session.  Refreshed by poll() once a frame;
 * platform/sio.c turns it into the SIOCNT status bits the game reads. */
struct PortMpLink {
    u8 up;        /* 1 once units are connected and transfers may happen     */
    u8 selfId;    /* 0..3.  0 means this unit clocks the cable (the "parent")*/
    u8 players;   /* units in the session, including this one                */
    u8 error;     /* set to raise SIOCNT's error bit; the game reports it    */
};

struct PortMpTransport {
    const char *name;

    /* Bring the session up.  `players` is what the caller asked for; a
     * transport may end up with fewer and says so through poll().  Returns
     * non-zero on success. */
    int  (*open)(struct PortMpTransport *t, int players);

    void (*close)(struct PortMpTransport *t);

    /* Once per frame, before the frame's transfers.  Where a networked
     * transport drains its socket. */
    void (*poll)(struct PortMpTransport *t, struct PortMpLink *link);

    /* One multi-play transfer.  `send` is this unit's SIOMLT_SEND; fill recv[]
     * with what each unit put on the bus, 0xFFFF for a slot with nobody in it.
     * Returns 1 if the transfer happened, 0 to stall the cable this slot.
     *
     * recv[link->selfId] is overwritten by the caller with `send` afterwards,
     * because that is what the hardware does -- a unit sees its own word in
     * its own slot -- so a transport need not echo. */
    int  (*exchange)(struct PortMpTransport *t, u16 send,
                     u16 recv[PORT_MP_PLAYERS]);

    void *user;
};

/* Attach a transport and open it.  Returns non-zero on success.  Attaching
 * replaces whatever was attached before, closing it first. */
int  PortMpAttach(struct PortMpTransport *t, int players);
void PortMpDetach(void);
const struct PortMpTransport *PortMpCurrent(void);

/* The link as of the last poll.  Returns non-zero if a transport is attached. */
int  PortMpLinkState(struct PortMpLink *out);

/* --- the reference transport ---------------------------------------------
 * A same-process loopback: the game on one endpoint, synthetic MultiSio units
 * on the others.  It needs no network, so it is what the interface is tested
 * against.  See platform/mp_loopback.c. */
struct PortMpTransport *PortMpLoopback(void);

/* Which slot the game occupies on the loopback cable; 0 (the default) makes it
 * the parent.  Call before attaching. */
void PortMpLoopbackSelfId(int id);

/* What the loopback's peer made of what the game sent it: the last 20-byte
 * block whose checksum came out right, how many such packets there were, and
 * how many did not.  For tests -- this is the evidence that a payload crossed
 * in the direction the game is sending. */
int  PortMpLoopbackPeerRecv(int peer, u8 *dest, u32 size);
int  PortMpLoopbackPeerPackets(int peer);
int  PortMpLoopbackPeerBadChecksums(int peer);

/* --- what the platform layer calls ---------------------------------------
 * mp.c owns the attached transport; platform/sio.c reaches it through these
 * two rather than through the vtable, so that "which transport" stays in one
 * file.  PortMpPoll polls at most once per frame however often it is called. */
void PortMpPoll(struct PortMpLink *link);
int  PortMpExchange(u16 send, u16 recv[PORT_MP_PLAYERS]);

/* --- the frame loop ------------------------------------------------------ */

/* The one call platform/main.c makes.  Returns immediately with no transport
 * attached, which is the single-player build. */
void PortMpFrame(void);

/* Refresh the SIOCNT status bits and run this frame's transfers. */
void PortSioFrame(void);
void PortSioStats(u32 *transfers, u32 *stalls);
void PortSioReset(void);

/* --- transports the port ships ------------------------------------------- */

int PortMpUseLoopback(int players);
/* Uses Module.portMp -- see the comment in mp.c for the four calls a page
 * has to provide. */
int PortMpUseJs(int players);

/* --- test hooks ----------------------------------------------------------
 * Calls MultiSioMain once a frame from the platform layer, the way GameLoop
 * would if a link session were running.  Exercises the game's own driver
 * without needing the menus; see the comment in mp.c about what that does and
 * does not prove. */
void PortMpSelfTest(int on);
void PortMpSelfTestStep(void);

/* Diagnostics, at the level of "did anything cross". */
void PortMpReport(void);


#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_MP_H */
