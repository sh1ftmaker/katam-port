/* The far end of the cable, while the game is still in its link lobby.
 *
 * --- what this is, and what it is not --------------------------------------
 *
 * A real transport -- a datachannel, a socket, a relay -- has another copy of
 * this game on the far end, and that copy speaks the lobby protocol itself
 * because it is the same code.  Nothing here is needed for that case.
 *
 * This is the *synthetic* peer: the answer the in-process loopback puts on the
 * bus so the lobby can be driven, and reached, and tested, with one process and
 * no network at all.  It is a test double, and it is the only way the lobby is
 * exercised in CI -- so it has to be faithful, not merely enough to get past.
 *
 * It is deliberately transport-agnostic: a pure function of the word the master
 * sent plus a little per-slot state, with no knowledge of how that word
 * arrived.  Any transport that wants a synthetic opponent -- for a soak test,
 * for a single-player-with-AI mode, for bringing a new transport up before the
 * far end exists -- calls this and gets the same behaviour the loopback gets.
 *
 * --- the protocol, read off the game's own child code ----------------------
 *
 * src/multi_boot_util.c:sub_08030898 is the serial handler the game installs
 * over gIntrTable[0] while the lobby runs.  It has a parent half and a child
 * half, and this is the child half.  Four words matter, and the master's word
 * says which phase it is in -- which is the whole answer to the problem this
 * file exists to solve, because the peer cannot know the phase any other way:
 *
 *   0x62XX   MultiBoot client recognition.  Answer 0x8F5<slot>.
 *
 *            0x8F50 rather than the MultiBoot client reply 0x7200: the
 *            classifier at multi_boot_util.c:56-77 masks with 0xFFF0 and sorts
 *            0x7200 -> "a bare console waiting for a download" (which the
 *            multi-cart lobby reports as error 8) from 0x8F50 -> "another
 *            cartridge running this game".  Answering 0x720X is correct for
 *            download play and wrong for the only mode a port can reach.
 *
 *   0x2XXX   the lobby's sequence counter.  The parent sends a number that
 *            climbs by one a frame; the child follows it and counts how long
 *            it has stayed consecutive, then answers
 *
 *              0x8F51   "still here"           while the count is <= 0x1E
 *              0x70AE   "here and settled"     once it passes 0x1E
 *
 *            and 0x70AE is what the parent counts: its loop over the child
 *            slots does `++unk01` for each slot answering 0x70AE.  `unk01 > 1`
 *            is one of the two conditions the lobby's advance needs, and
 *            nothing else in the protocol produces it.
 *
 *   0xE4E4   the parent says the game is starting.  Echo it.  The parent sets
 *            unk02 = 3 on seeing it, which is the other condition.
 *
 * Anything else is not a lobby word -- it is MultiSio game traffic -- and the
 * answer is PORT_MP_PEER_PASS so the caller falls through to whatever the
 * transport would otherwise have sent.
 *
 * --- why phase-awareness, rather than always answering 0x8F5X --------------
 *
 * Measured, and this is the tension that held the lobby up.  A peer that
 * answers 0x8F5X unconditionally does get classified -- and takes its MultiSio
 * packets off the bus, because every exchange is now a lobby reply; the
 * loopback's sequencer traffic stops dead.  A peer that answers only 0x62XX
 * keeps the packets (3808 transfers, 0 stalls) and never gets past
 * recognition, because it never says 0x70AE.
 *
 * Both are the same mistake: treating the reply as a property of the peer
 * rather than of the exchange.  The master's word already carries the phase,
 * so the peer does not need to be told and does not need to guess.
 */

#include "port/port.h"
#include "port/mp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GBATEK, "Multiboot Transfer Protocol", and the game's own constants. */
#define MB_MASTER_HELLO     0x6200
#define MB_REPLY_MASK       0xFFF0
#define MB_SAME_GAME_REPLY  0x8F50

/* multi_boot_util.c: the three words the lobby exchanges once it is past
 * recognition. */
#define LOBBY_CHILD_ALIVE   0x8F51
#define LOBBY_CHILD_READY   0x70AE
#define LOBBY_START         0xE4E4

/* The parent's sequence word: multi_boot_util.c keeps the counter in the low
 * 13 bits and a tag in the top three, and compares only the low ones. */
#define SEQ_MASK            0x1FFF
#define SEQ_MIN             0x0100
/* `if (unk0A > 0x1E)` -- thirty-one consecutive frames, not thirty. */
#define SETTLE_FRAMES       0x1E

enum PeerPhase {
    PEER_IDLE,       /* nothing has been recognised; not in a lobby */
    PEER_LOBBY,      /* recognised; following the parent's counter */
    PEER_PLAYING     /* the parent said start; game traffic from here */
};

static struct PeerLobby {
    u8  phase;
    u16 lastSeq;     /* the parent's previous word, as unk08 keeps it */
    u16 settled;     /* consecutive frames the counter has been consecutive */
    u8  haveSeq;
} sPeers[PORT_MP_PLAYERS];

void PortMpPeerLobbyReset(int slot)
{
    if (slot >= 0 && slot < PORT_MP_PLAYERS) {
        sPeers[slot].phase = PEER_IDLE;
        sPeers[slot].lastSeq = 0;
        sPeers[slot].settled = 0;
        sPeers[slot].haveSeq = 0;
    } else if (slot < 0) {
        int i;

        for (i = 0; i < PORT_MP_PLAYERS; i++)
            PortMpPeerLobbyReset(i);
    }
}

u16 PortMpPeerLobbyReply(int slot, u16 masterWord)
{
    struct PeerLobby *p;

    /* Slot 0 clocks the cable and never replies to itself. */
    if (slot <= 0 || slot >= PORT_MP_PLAYERS)
        return PORT_MP_PEER_PASS;
    p = &sPeers[slot];

    if ((masterWord & MB_REPLY_MASK) == MB_MASTER_HELLO) {
        /* Recognition.  It repeats -- the parent probes until thirty frames of
         * identical replies accumulate -- and it also happens again if the
         * lobby restarts, which is why this resets the counter state rather
         * than assuming a lobby is only entered once. */
        p->phase = PEER_LOBBY;
        p->settled = 0;
        p->haveSeq = 0;
        return (u16)(MB_SAME_GAME_REPLY | slot);
    }

    if (masterWord == LOBBY_START) {
        /* Echo, and stop answering as a lobby peer: what follows is the game.
         * The parent sets unk02 = 3 on seeing its own 0xE4E4 come back. */
        p->phase = PEER_PLAYING;
        return LOBBY_START;
    }

    if (p->phase != PEER_LOBBY)
        return PORT_MP_PEER_PASS;

    /* The counter, followed exactly as multi_boot_util.c's child half follows
     * it: step the previous word by one, wrap into 13 bits, floor it at 0x100,
     * and require the parent's low bits to be that.  Anything else restarts the
     * count -- a dropped or reordered frame has to cost the settle, or a peer
     * would claim to be settled on a link that is not carrying. */
    {
        u16 expect = (u16)((p->lastSeq + 1) & SEQ_MASK);

        if (expect < SEQ_MIN)
            expect = SEQ_MIN;

        if (p->haveSeq && expect == (u16)(masterWord & SEQ_MASK)) {
            if (p->settled < 0xFFFF)
                p->settled++;
        } else {
            p->settled = 0;
        }
        p->lastSeq = masterWord;
        p->haveSeq = 1;
    }

    return p->settled > SETTLE_FRAMES ? LOBBY_CHILD_READY : LOBBY_CHILD_ALIVE;
}

#ifdef __cplusplus
}
#endif
