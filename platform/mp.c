/* Multiplayer: the transport registry, and the page's way in.
 *
 * platform/port/mp.h says what a transport is; platform/sio.c turns one into
 * the serial hardware the game reads.  This file is what sits between them --
 * which transport is attached, what it last said about the link, and the two
 * ready-made ones: the same-process loopback in mp_loopback.c, and a transport
 * whose four calls are JavaScript, so a page can supply its own without any of
 * this being rebuilt.
 *
 * The JS binding lives here rather than in the header on purpose.  A native
 * build registers a C transport through exactly the same PortMpAttach, and
 * nothing it includes mentions emscripten.
 *
 * Nothing here is reached unless something attaches a transport.  With none
 * attached PortSioFrame returns immediately, SIOCNT keeps reading as an
 * unplugged cable, and the port behaves exactly as it did before any of this
 * existed -- which is the single-player build, and is why it is safe for this
 * to be compiled in unconditionally.
 */

#include <string.h>

#include "port/port.h"
#include "port/mp.h"
#include "global.h"
#include "main.h"
#include "multi_sio.h"

/* C linkage for the 64-bit builds.
 *
 * Those compile the game as C++ and tools/cxxify.py gives it C linkage, so
 * everything on the seam has to agree.  It is applied to platform/*.c as a
 * class rather than to the files that happened to break: the failure is an
 * undefined reference to a mangled name, or -- for a `const`, which is
 * internal-linkage in C++ and external in C -- to a symbol that is plainly
 * defined a few lines away.  Neither says which file to fix, and the set of
 * files that need it changes whenever a declaration moves.  A no-op in C.
 *
 * The block opens below the includes so that SDL and the system headers stay
 * outside it. */
#ifdef __cplusplus
extern "C" {
#endif

static struct PortMpTransport *sTransport;
static struct PortMpLink sLink;
static int sPolledThisFrame;

/* --- registry ------------------------------------------------------------ */

int PortMpAttach(struct PortMpTransport *t, int players)
{
    PortMpDetach();
    if (!t || !t->exchange || !t->poll)
        return 0;
    if (players < 2)
        players = 2;
    if (players > PORT_MP_PLAYERS)
        players = PORT_MP_PLAYERS;

    if (t->open && !t->open(t, players)) {
        PortError("[katam-port] mp: transport '%s' refused to open", t->name);
        return 0;
    }
    sTransport = t;
    memset(&sLink, 0, sizeof(sLink));
    PortSioReset();
    PortLog("[katam-port] mp: transport '%s' attached, %d player(s)",
            t->name, players);
    return 1;
}

void PortMpDetach(void)
{
    if (!sTransport)
        return;
    if (sTransport->close)
        sTransport->close(sTransport);
    PortLog("[katam-port] mp: transport '%s' detached", sTransport->name);
    sTransport = NULL;
    memset(&sLink, 0, sizeof(sLink));
}

const struct PortMpTransport *PortMpCurrent(void)
{
    return sTransport;
}

int PortMpLinkState(struct PortMpLink *out)
{
    if (out)
        *out = sLink;
    return sTransport != NULL;
}

void PortMpPoll(struct PortMpLink *link)
{
    if (!sTransport) {
        memset(&sLink, 0, sizeof(sLink));
    } else if (!sPolledThisFrame) {
        sTransport->poll(sTransport, &sLink);
        sPolledThisFrame = 1;
        if (sLink.players > PORT_MP_PLAYERS)
            sLink.players = PORT_MP_PLAYERS;
        if (sLink.selfId >= PORT_MP_PLAYERS)
            sLink.selfId = 0;
    }
    if (link)
        *link = sLink;
}

int PortMpExchange(u16 send, u16 recv[PORT_MP_PLAYERS])
{
    if (!sTransport)
        return 0;
    return sTransport->exchange(sTransport, send, recv);
}

/* --- a JavaScript transport ----------------------------------------------
 *
 * Supplied by the host half: platform/web/mp_js.c binds Module.portMp, and
 * platform/native/mp_native.c returns NULL because there is no page.  The
 * binding lives there rather than here so that this file -- the registry the
 * game actually goes through -- includes nothing host-specific. */
struct PortMpTransport *PortMpJsTransport(void);


/* --- the frame loop ------------------------------------------------------ */

void PortMpFrame(void)
{
    sPolledThisFrame = 0;
    PortMpSelfTestStep();
    PortSioFrame();
}

/* --- self test -----------------------------------------------------------
 *
 * The game calls MultiSioMain once a frame while a link session is running --
 * GameLoop does it under `if (gUnk_03002558 != 0)`, between packing the local
 * buttons and unpacking everybody's.  Reaching that from a headless run means
 * getting through the title screen, the main menu, the link submenu and the
 * lobby, and the lobby does not complete without MultiBoot; see
 * docs/MULTIPLAYER.md.
 *
 * So this makes the same call the game makes, on the game's own globals, with
 * the game's own library -- but from here rather than from GameLoop.  What it
 * proves is everything below MultiSioMain: the transfers, the interrupt, the
 * packet framing, both checksums and the buffer rotation.  What it does not
 * prove is that the game gets here by itself.  The distinction is the point;
 * it is why this is called a self test and not a demo. */

static int sSelfTest;
static u32 sLastStatus = 0xFFFFFFFF;

void PortMpSelfTest(int on)
{
    sSelfTest = on;
    sLastStatus = 0xFFFFFFFF;
    if (on)
        PortLog("[katam-port] mp: self test on -- MultiSioMain will be called "
                "once a frame from the platform layer, not by the game");
}

void PortMpSelfTestStep(void)
{
    u8 *send = (u8 *)&gMultiSioSend;
    u32 status;
    int i;

    if (!sSelfTest || !sTransport)
        return;

    /* A payload with this unit's fingerprint on it, so that what the loopback
     * peer reports receiving can only have come from here. */
    send[0] = 0x4B;                 /* 'K' */
    send[1] = (u8)(sLastStatus + 1);
    for (i = 2; i < MULTI_SIO_BLOCK_SIZE; i++)
        send[i] = (u8)(0xC0 + i);

    status = MultiSioMain(&gMultiSioSend, gMultiSioRecv, 0);
    gMultiSioStatusFlags = status;

    if (status != sLastStatus) {
        sLastStatus = status;
        PortLog("[katam-port] mp: MultiSioMain -> 0x%04X "
                "(recv=%X connected=%X %s%s%s)",
                (unsigned)status,
                (unsigned)(status & MULTI_SIO_RECV_ID_MASK),
                (unsigned)((status & MULTI_SIO_CONNECTED_ID_MASK) >> 8),
                (status & MULTI_SIO_TYPE) ? "parent" : "child",
                (status & MULTI_SIO_HARD_ERROR) ? " HARD-ERROR" : "",
                (status & MULTI_SIO_RECV_FLAGS_AVAILABLE) ? " flags-valid" : "");
    }
}

/* --- diagnostics --------------------------------------------------------- */

void PortMpReport(void)
{
    u32 transfers, stalls;
    u8 peer[MULTI_SIO_BLOCK_SIZE];
    int p;

    if (!sTransport) {
        PortLog("[katam-port] mp: no transport attached");
        return;
    }

    PortSioStats(&transfers, &stalls);
    PortLog("[katam-port] mp: transport='%s' up=%u self=%u players=%u "
            "transfers=%u stalls=%u",
            sTransport->name, sLink.up, sLink.selfId, sLink.players,
            (unsigned)transfers, (unsigned)stalls);

    for (p = 0; p < PORT_MP_PLAYERS; p++) {
        const u8 *r = (const u8 *)&gMultiSioRecv[p];

        PortLog("[katam-port] mp:   gMultiSioRecv[%d] = "
                "%02X %02X %02X %02X %02X %02X %02X %02X",
                p, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }

    for (p = 0; p < PORT_MP_PLAYERS; p++) {
        int packets = PortMpLoopbackPeerPackets(p);

        if (packets < 0)
            continue;
        if (PortMpLoopbackPeerRecv(p, peer, sizeof(peer)))
            PortLog("[katam-port] mp:   peer %d took %d packet(s) from us, "
                    "last payload %02X %02X %02X %02X %02X %02X %02X %02X",
                    p, packets, peer[0], peer[1], peer[2], peer[3],
                    peer[4], peer[5], peer[6], peer[7]);
    }
}

/* --- entry points the page and the harness call -------------------------- */

int PortMpUseLoopback(int players)
{
    return PortMpAttach(PortMpLoopback(), players);
}

int PortMpUseJs(int players)
{
    struct PortMpTransport *t = PortMpJsTransport();

    if (!t)
        return 0;
    return PortMpAttach(t, players);
}

#ifdef __cplusplus
}
#endif
