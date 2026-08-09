/* The reference transport: a link cable with both ends in this process.
 *
 * There is no second console here and there cannot be one -- the port reserves
 * the GBA memory map at its true addresses inside a single wasm linear memory,
 * so a second copy of the game would want EWRAM at 0x02000000 as well.  What
 * this file does instead is put a *unit* on the other end of the cable: a
 * small, complete MultiSio endpoint that streams properly framed packets, and
 * reads and checksums the ones the game sends back.
 *
 * That is worth more than it sounds.  The MultiSio packet layer is where every
 * assumption about the protocol is testable: the 0xFEFE sync word, one halfword
 * per transfer, fourteen halfwords a packet, the checksum that has to come out
 * at -15 on the receiver, the triple-buffer rotation on the last payload
 * halfword.  A peer that speaks it end to end proves the port's SIO unit and
 * the game's own driver agree about all of it, and it does so with no network,
 * no second process and nothing to configure.
 *
 * What it is not: it is not the other player.  It answers at the packet layer
 * and its payload is a fixed pattern, so nothing above MultiSio -- the lobby
 * handshake in multi_08030C94.c, the input ring in sub_08030E44 -- is exercised
 * by it.  docs/MULTIPLAYER.md says what reaching those would take.
 *
 * Written as the mirror image of platform/multi_sio_intr.c and of
 * MultiSioSendDataSet / MultiSioRecvDataCheck in src/multi_sio.c, which is
 * where the framing and both checksums come from.
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

#define PACKET_HW ((s32)(sizeof(struct MultiSioPacket) / 2))

struct LoopPeer {
    int present;

    /* send: one halfword per transfer, a fresh packet each frame */
    s32 sendCounter;
    u16 packet[PACKET_HW];
    u8  frameCounter;

    /* receive: the same state machine MultiSioIntr runs, plus the check
     * MultiSioRecvDataCheck runs on top of it */
    s32 recvCounter;
    u16 recvBuf[PACKET_HW];
    u8  payload[MULTI_SIO_BLOCK_SIZE];
    int packets;                /* packets from the game that checksummed  */
    int badChecksums;
};

static struct LoopPeer sPeers[PORT_MP_PLAYERS];
static int sPlayers = 2;
static int sSelfId;
static int sOpen;

/* --- payload mode: the real remote players, on the local cable ------------
 *
 * The whole of KATAM's link session above the packet layer is a 20-byte
 * user block per unit per frame (multi_08030C94.c): 0x20 input messages
 * carrying eight frames of button samples, and the pat2 world-sync
 * negotiation.  Everything below it -- framing, checksums, the strict word
 * streams -- is what a network relay keeps breaking.  So payload mode
 * relays the *blocks* and keeps the bus local: each remote unit on this
 * synthetic cable speaks the last block its real player sent, rebuilt into
 * a properly framed, checksummed packet every frame.  MultiSio is
 * send-latest-state by design, so a late block repeating is exactly what a
 * dropped frame looks like on hardware; past HOLD_LAG frames of staleness
 * the port pauses the game's timestep instead (platform/sio.c), which is
 * the console-online lockstep presentation.  The game's own world sync,
 * input ring, per-sample desync hash and cable-out handling all run
 * unchanged -- they ride inside the blocks. */
#define HOLD_LAG 6              /* inside the input ring's eight frames    */

static int sPayloadMode;
static u32 sLocalFrame;         /* frames since payload mode began         */
static struct {
    int everFed;
    u32 senderFrame;            /* the sender's own stamp, for dedup       */
    u32 fedAt;                  /* sLocalFrame at receipt, for freshness   */
    u8  block[MULTI_SIO_BLOCK_SIZE];
} sFeeds[PORT_MP_PLAYERS];

/* --- the peer's send side ------------------------------------------------- */

/* MultiSioSendDataSet, for a unit whose "user send buffer" is a pattern.
 *
 * The checksum rule is not obvious and is worth writing down: the sender sums
 * the first twelve halfwords with the checksum field zeroed and stores
 * ~sum - 14, so the receiver's sum of the same twelve comes out at exactly
 * -15 as a signed 16-bit value.  Get the field order wrong by one halfword and
 * every packet is silently discarded, which looks identical to no cable. */
static void BuildPacket(struct LoopPeer *p, int id)
{
    u8 *bytes = (u8 *)p->packet;
    s32 sum = 0;
    s32 i;

    memset(p->packet, 0, sizeof(p->packet));

    bytes[0] = p->frameCounter++;
    bytes[1] = 0;               /* recvErrorFlags:4, load bits, reserved     */
    /* bytes[2..3] is the checksum, filled in below */

    if (sPayloadMode && sFeeds[id].everFed) {
        /* The real player's latest block, verbatim.  Repeating it while a
         * fresher one is in flight is what MultiSio expects of a dropped
         * frame; the framing around it is rebuilt locally either way. */
        memcpy(bytes + 4, sFeeds[id].block, MULTI_SIO_BLOCK_SIZE);
    } else {
        /* The 20-byte user block, at halfword 2.  Distinctive on purpose:
         * the first byte names this peer's slot and the second counts its
         * packets, so a payload that turns up in gMultiSioRecv can only
         * have come from here. */
        bytes[4] = (u8)(0xA0 | id);
        bytes[5] = p->frameCounter;
        for (i = 2; i < MULTI_SIO_BLOCK_SIZE; i++)
            bytes[4 + i] = (u8)(id * 0x10 + i);
    }

    for (i = 0; i < PACKET_HW - 2; i++)
        sum += p->packet[i];
    p->packet[1] = (u16)(~sum - 14);

    p->sendCounter = -1;
}

static u16 PeerSend(struct LoopPeer *p)
{
    u16 w;

    if (p->sendCounter == -1) {
        w = MULTI_SIO_SYNC_DATA;
    } else {
        s32 i = p->sendCounter;

        if (i >= PACKET_HW)
            i = PACKET_HW - 1;      /* the counter clamps, and so does this  */
        w = p->packet[i];
    }
    if (p->sendCounter < PACKET_HW - 1)
        ++p->sendCounter;
    return w;
}

/* --- the peer's receive side ---------------------------------------------- */

static void PeerRecv(struct LoopPeer *p, u16 w)
{
    if (w == MULTI_SIO_SYNC_DATA && p->recvCounter > PACKET_HW - 3) {
        p->recvCounter = -1;
    } else {
        if (p->recvCounter >= 0 && p->recvCounter < PACKET_HW)
            p->recvBuf[p->recvCounter] = w;

        if (p->recvCounter == PACKET_HW - 3) {
            s32 sum = 0;
            s32 i;

            for (i = 0; i < PACKET_HW - 2; i++)
                sum += p->recvBuf[i];
            if ((s16)sum == -15) {
                memcpy(p->payload, (const u8 *)p->recvBuf + 4,
                       MULTI_SIO_BLOCK_SIZE);
                p->packets++;
            } else {
                p->badChecksums++;
            }
        }
    }
    if (p->recvCounter < PACKET_HW - 1)
        ++p->recvCounter;
}

/* --- the transport -------------------------------------------------------- */

static int LoopOpen(struct PortMpTransport *t, int players)
{
    int i;

    (void)t;
    sPlayers = players;
    memset(sPeers, 0, sizeof(sPeers));
    for (i = 0; i < PORT_MP_PLAYERS; i++) {
        sPeers[i].present = (i < sPlayers && i != sSelfId);
        if (sPeers[i].present)
            BuildPacket(&sPeers[i], i);
    }
    sOpen = 1;
    return 1;
}

static void LoopClose(struct PortMpTransport *t)
{
    (void)t;
    sOpen = 0;
}

static void LoopPoll(struct PortMpTransport *t, struct PortMpLink *link)
{
    int i;

    (void)t;
    link->up = (u8)(sOpen ? 1 : 0);
    link->selfId = (u8)sSelfId;
    link->players = (u8)sPlayers;
    link->error = 0;

    /* Once a frame, exactly where MultiSioSendDataSet does it: rewind the send
     * counter so the next transfer emits the sync word, and refill the packet
     * behind it.  Sixteen transfers a frame against fifteen halfwords of sync
     * plus packet is where the slack in PORT_SIO_SLOTS goes. */
    if (!sOpen)
        return;
    if (sPayloadMode)
        sLocalFrame++;
    for (i = 0; i < PORT_MP_PLAYERS; i++)
        if (sPeers[i].present)
            BuildPacket(&sPeers[i], i);
}

static int LoopExchange(struct PortMpTransport *t, u16 send,
                        u16 recv[PORT_MP_PLAYERS])
{
    int i;

    (void)t;
    if (!sOpen)
        return 0;

    for (i = 0; i < PORT_MP_PLAYERS; i++) {
        if (!sPeers[i].present) {
            /* An empty slot on the cable reads as 0xFFFF, which is what the
             * game's driver uses to tell "nobody there" from "somebody sent
             * me zero". */
            recv[i] = 0xFFFF;
            continue;
        }

        /* MultiBoot's client recognition runs on the same bus as MultiSio and
         * has to be answered before the lobby will believe there is a cable at
         * all -- the master's 0x62xx is not a MultiSio packet and the peer's
         * packet builder would otherwise answer it with sequencer traffic.
         * The reply comes from platform/multi_boot.c so that every transport
         * gives the same one. */
        {
            u16 mb = PortMpPeerLobbyReply(i, send);

            if (mb != PORT_MP_PEER_PASS) {
                recv[i] = mb;
                continue;
            }
        }

        recv[i] = PeerSend(&sPeers[i]);
        PeerRecv(&sPeers[i], send);
    }
    return 1;
}

static struct PortMpTransport sLoopback = {
    "loopback", LoopOpen, LoopClose, LoopPoll, LoopExchange, NULL,
};

struct PortMpTransport *PortMpLoopback(void)
{
    return &sLoopback;
}

/* Which slot the *game* occupies.  0 -- the default -- makes the game the
 * parent, so it clocks the cable and the port's SIO unit only runs a transfer
 * when the game has armed one.  Anything else makes the game a child and the
 * loopback drives the clock, which is the other half of the interface and
 * wants testing too. */
void PortMpLoopbackSelfId(int id)
{
    if (id >= 0 && id < PORT_MP_PLAYERS)
        sSelfId = id;
}

int PortMpLoopbackPeerPackets(int peer)
{
    if (peer < 0 || peer >= PORT_MP_PLAYERS || !sPeers[peer].present)
        return -1;
    return sPeers[peer].packets;
}

int PortMpLoopbackPeerRecv(int peer, u8 *dest, u32 size)
{
    if (peer < 0 || peer >= PORT_MP_PLAYERS || !sPeers[peer].present)
        return 0;
    if (size > MULTI_SIO_BLOCK_SIZE)
        size = MULTI_SIO_BLOCK_SIZE;
    memcpy(dest, sPeers[peer].payload, size);
    return 1;
}

int PortMpLoopbackPeerBadChecksums(int peer)
{
    if (peer < 0 || peer >= PORT_MP_PLAYERS || !sPeers[peer].present)
        return -1;
    return sPeers[peer].badChecksums;
}

/* --- payload mode's page-facing half -------------------------------------- */

void PortMpLoopbackPayloadMode(int on)
{
    sPayloadMode = on ? 1 : 0;
    sLocalFrame = 0;
    memset(sFeeds, 0, sizeof(sFeeds));
}

/* A remote player's latest 20-byte block, from the relay.  Stale or replayed
 * stamps (a reconnect re-sends its recent window) are dropped; fresh ones
 * update the block the synthetic unit speaks and the freshness clock the
 * hold below reads. */
void PortMpFeedPayload(int player, u32 senderFrame, const u8 *block)
{
    if (player < 0 || player >= PORT_MP_PLAYERS || player == sSelfId)
        return;
    if (sFeeds[player].everFed && senderFrame <= sFeeds[player].senderFrame)
        return;
    sFeeds[player].everFed = 1;
    sFeeds[player].senderFrame = senderFrame;
    sFeeds[player].fedAt = sLocalFrame;
    memcpy(sFeeds[player].block, block, MULTI_SIO_BLOCK_SIZE);
}

/* A player past their reconnect grace stops being on the cable at all --
 * their unit reads as 0xFFFF and the game handles the cable-out its own
 * way, which is the authentic behaviour. */
void PortMpSetPeerPresent(int player, int present)
{
    if (player < 0 || player >= PORT_MP_PLAYERS || player == sSelfId)
        return;
    sPeers[player].present = present ? 1 : 0;
    if (!present)
        sFeeds[player].everFed = 0;
}

/* Should the frame loop pause?  Yes while any present, ever-heard-from
 * player's latest block is more than HOLD_LAG of our frames old -- inside
 * that, the input ring's redundancy hides the repeat entirely.  Players
 * never heard from are the session still assembling; players marked absent
 * are the game's problem now. */
int PortMpPayloadHold(void)
{
    int i;

    if (!sPayloadMode || !sOpen)
        return 0;
    for (i = 0; i < PORT_MP_PLAYERS; i++) {
        if (i == sSelfId || !sPeers[i].present || !sFeeds[i].everFed)
            continue;
        if (sLocalFrame - sFeeds[i].fedAt > HOLD_LAG)
            return 1;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
