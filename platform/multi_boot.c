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
 * to answer the probe, which platform/mp_loopback.c now does.
 */

#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "port/mp.h"
#include "gba/multi_boot.h"

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
     * PortMpMultiBootReply, which is what the loopback peer uses. */
    if (link.selfId != 0) {
        mp->client_bit = 0;
        mp->probe_count = 0;
        return 0;
    }

    send = (u16)(MB_MASTER_HELLO | (mp->probe_target_bit & MB_CLIENT_BITS));
    for (i = 0; i < PORT_MP_PLAYERS; i++)
        recv[i] = 0xFFFF;

    if (!PortMpExchange(send, recv)) {
        /* The cable stalled this frame.  Leave client_bit as it was: the
         * lobby wants sixteen *consecutive* frames of recognition, and
         * clearing it on a single dropped transfer would restart that count
         * every time a packet was late -- which over a network is often. */
        return 0;
    }

    mp->client_bit = 0;
    for (i = 1; i < PORT_MP_PLAYERS; i++)
        if ((recv[i] & MB_REPLY_MASK) == MB_CLIENT_REPLY)
            mp->client_bit |= (u8)(1 << i);

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

/* The answer a *client* puts on the bus during recognition.
 *
 * A transport's peer needs this to complete the handshake, and it is here
 * rather than in each transport so that they cannot disagree about it.  Slot
 * is the peer's own id, 1..3; slot 0 is the master and never replies. */
u16 PortMpMultiBootReply(int slot, u16 masterWord)
{
    if (slot <= 0 || slot >= PORT_MP_PLAYERS)
        return 0xFFFF;
    if ((masterWord & MB_REPLY_MASK) != MB_MASTER_HELLO)
        return 0xFFFF;
    return (u16)(MB_CLIENT_REPLY | slot);
}

#ifdef __cplusplus
}
#endif
