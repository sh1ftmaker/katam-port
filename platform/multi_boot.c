/* MultiBoot, the part of it this game actually needs.
 *
 * On hardware this is Nintendo's SDK library, hand-written ARM assembly, and
 * tools/portify.py keeps it out of the build (REPLACED_FILES) because there is
 * no C for it.  Everything here is the port supplying that library.
 *
 * --- why a port needs MultiBoot at all -------------------------------------
 *
 * It looked like a question about download play, and download play is out of
 * scope: it boots a *different program* on another console, which is not
 * something a WebAssembly module can be on the far end of.
 *
 * But the multi-cart four-player mode needs the library too, and not to
 * download anything.  src/multi_08030C94.c:809 calls
 *
 *     MultiBootInitWithParams((void *)0, (void *)0x100);   // zero-length
 *
 * -- a probe, not a transfer.  The game uses MultiBoot's *client recognition*
 * phase as its link detector.  In the lobby's state 0 the only thing that
 * clocks the cable on the parent is MultiBootMain, so with it stubbed the
 * cable never moves, the thirty frames of stable replies never accumulate, and
 * the lobby sits on "Please connect the Game Boy Advance Game Link cable"
 * forever.  That was measured, not inferred -- docs/MULTIPLAYER.md §5.
 *
 * --- what the game reads back ----------------------------------------------
 *
 * Exactly two fields, from src/multi_boot_util.c:183-237:
 *
 *   client_bit    bits 1..3, one per recognised client.  The player count is
 *                 1 + popcount(client_bit & 0xE), and sixteen consecutive
 *                 frames with it non-zero is what moves the lobby on.
 *   probe_count   0 during ordinary recognition.  The game watches for 0xD1
 *                 and >= 0xE0, which are stages of the *program transfer* --
 *                 download play only, and never reached here.
 *
 * So the multi-cart path needs the recognition phase and nothing else, which
 * is what this implements.  MultiBootStartMaster and MultiBootCheckComplete
 * are left refusing, loudly, rather than half-written: a transfer that starts
 * and does not finish would move the failure somewhere much less obvious than
 * the screen that currently says what is wrong.
 *
 * --- transport independence ------------------------------------------------
 *
 * The recognition handshake is four halfwords on the multi-play bus, so it
 * goes through the same seam everything else does -- PortMpExchange, in
 * platform/port/mp.h.  Nothing here knows whether the far end is the
 * same-process loopback, a datachannel or a socket.  A transport that already
 * answers `exchange` needs no new entry point to support this; its *peer* has
 * to answer the probe.
 *
 * What the peer answers -- for recognition and for the rest of the lobby that
 * follows it -- lives in platform/mp_peer_lobby.c.  It used to be a function
 * here, which put half the protocol in the MultiBoot file and left the other
 * half nowhere, and that split is most of why the lobby took three attempts:
 * the recognition reply is only the first of four words, and the peer has to
 * know which phase it is in to choose between them.
 */

#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/mp.h"
#include "gba/multi_boot.h"
#include "gba/io_reg.h"

/* The registers this drives, named the way platform/sio.c names them. */
#define SIOCNT      (*(vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOCNT))
#define SIOMLT_SEND (*(vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOMLT_SEND))
#define SIOMULTI    ((vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOMULTI0))
#define CNT_START   0x0080

#ifdef __cplusplus
extern "C" {
#endif

/* GBATEK, "Multiboot Transfer Protocol".  The master advertises
 * 0x6200 | client_mask; a client that hears it answers 0x7200 | its own bit.
 * Those two words are the whole of the recognition phase. */
#define MB_MASTER_HELLO   0x6200
#define MB_CLIENT_REPLY   0x7200
#define MB_REPLY_MASK     0xFFF0
#define MB_CLIENT_BITS    0x0E      /* slots 1..3; slot 0 is the master */
/* What a peer running *this game* answers, as opposed to a bare console
 * waiting for a download.  src/multi_boot_util.c sorts the two apart and the
 * multi-cart lobby only accepts the first. */
#define MB_SAME_GAME_REPLY 0x8F50

void MultiBootInit(struct MultiBootParam *mp)
{
    if (mp == NULL)
        return;

    /* The SDK zeroes its own working area and leaves the caller's pointers
     * alone.  multi_boot_util.c has already filled in boot_srcp/boot_endp and
     * server_type by the time this runs, so clearing the whole struct here
     * would undo the caller's setup -- which is the kind of thing that looks
     * like a transport bug for a day. */
    memset(mp->system_work, 0, sizeof mp->system_work);
    memset(mp->system_work2, 0, sizeof mp->system_work2);
    mp->handshake_data = 0;
    mp->handshake_timeout = 0;
    mp->probe_count = 0;
    mp->client_bit = 0;
    mp->response_bit = 0;
    mp->probe_target_bit = 0;
    mp->sendflag = 0;
    mp->check_wait = 0;
    memset(mp->client_data, 0, sizeof mp->client_data);
}

s32 MultiBootMain(struct MultiBootParam *mp)
{
    struct PortMpLink link;
    u16 recv[PORT_MP_PLAYERS];
    u16 send;
    int i;

    if (mp == NULL)
        return 1;

    if (!PortMpLinkState(&link) || !link.up) {
        /* No cable.  Not an error -- it is the answer to "is anyone there",
         * and the lobby is asking that question every frame. */
        mp->client_bit = 0;
        mp->response_bit = 0;
        mp->probe_count = 0;
        return 0;
    }

    /* Only the unit that clocks the cable probes.  A child answers, and its
     * answer comes from whatever is on the far end of the transport -- see
     * platform/mp_peer_lobby.c, which is what the loopback peer uses. */
    if (link.selfId != 0) {
        mp->client_bit = 0;
        mp->probe_count = 0;
        return 0;
    }

    /* Arm a transfer; do not run one.
     *
     * The first version called PortMpExchange directly from here and it was
     * wrong in a way worth recording.  The game installs its *own* serial
     * handler over gIntrTable[0] while the lobby runs (sub_0803024C sets it to
     * sub_08030898), and that handler is what collects each peer's word into
     * gMultiBootStruct.unk1E[] -- which is what the lobby's classifier reads.
     * Exchanging behind the SIO layer's back filled nothing and dispatched no
     * interrupt, so the classifier saw zeroes forever no matter how correct
     * the handshake was.
     *
     * Setting SIOMLT_SEND and the start bit hands it to platform/sio.c, which
     * runs the transfer, writes SIOMULTI0..3 and raises INTR_FLAG_SERIAL --
     * the same path a real transfer takes.  The game's handler then sees it. */
    SIOMLT_SEND = (u16)(MB_MASTER_HELLO | (mp->probe_target_bit & MB_CLIENT_BITS));
    SIOCNT |= CNT_START;

    /* client_bit from what the *previous* transfer brought back, which is what
     * is in the registers now.  A frame late, and that is correct: the game
     * reads this after the handler has run. */
    /* A peer is a peer whichever way it answered.
     *
     * client_bit is a *presence* mask -- multi_boot_util.c turns it into the
     * player count (1 + popcount) -- while the 0x7200-or-0x8F50 distinction is
     * about what *kind* of peer it is, and the lobby's classifier makes that
     * call separately.  Counting only 0x720X here was a real bug for one
     * revision: the peer had just been corrected to answer 0x8F5X, so the mask
     * went to zero, the player count stuck at 1, and the recognition counter
     * never climbed past its first frame. */
    (void)send; (void)recv;
    mp->client_bit = 0;
    for (i = 1; i < PORT_MP_PLAYERS; i++) {
        u16 w = SIOMULTI[i] & MB_REPLY_MASK;

        if (w == MB_CLIENT_REPLY || w == MB_SAME_GAME_REPLY)
            mp->client_bit |= (u8)(1 << i);
    }

    /* response_bit is "probably connected" and client_bit is "recognised".
     * Nothing in this game distinguishes them, so they are the same set; a
     * transport with a notion of a half-open peer could separate them. */
    mp->response_bit = mp->client_bit;
    mp->probe_target_bit = mp->client_bit;

    /* Stays 0: anything else is a stage of the program transfer below. */
    mp->probe_count = 0;
    return 0;
}

void MultiBootStartMaster(struct MultiBootParam *mp, const u8 *srcp,
                          s32 length, u8 palette_color, s8 palette_speed)
{
    (void)mp; (void)srcp; (void)length; (void)palette_color; (void)palette_speed;

    /* Download play: send a whole program to a cartridge-less console.  Out of
     * scope for a port -- the far end would have to *become* another program,
     * and a WebAssembly module cannot be booted into over a cable.
     *
     * Refused rather than half-implemented on purpose.  A transfer that starts
     * and never completes leaves the game waiting on a state it will not
     * reach, which is a much worse place to debug from than this message. */
    PortUnimplemented("MultiBootStartMaster (download play -- see "
                      "docs/MULTIPLAYER.md section 5)");
}

s32 MultiBootCheckComplete(struct MultiBootParam *mp)
{
    (void)mp;
    /* Nothing was ever started, so nothing can complete.  Non-zero is the
     * SDK's "not done". */
    return 1;
}

#ifdef __cplusplus
}
#endif
