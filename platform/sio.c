/* The serial port, in multi-play mode.
 *
 * The link cable is four consoles on one bus.  One of them -- the parent --
 * clocks it; each transfer moves one halfword from every unit to every unit at
 * once, so after a transfer all four consoles hold the same four halfwords in
 * SIOMULTI0..3, each in the slot of the unit that sent it.  That is the whole
 * of the hardware.  Everything above it, including all of the game's link
 * play, is a protocol built out of repeating that.
 *
 * What the port has to supply
 * ---------------------------
 * Three things the console does and a wasm module does not:
 *
 *   the status bits.  SIOCNT reports which slot this unit is in (bits 4-5),
 *   whether it is the parent (bit 2, SI, low if it is), and whether every unit
 *   on the cable is ready (bit 3, SD).  The game's driver reads all three
 *   before it will start -- MultiSioMain's state 0 promotes itself to parent
 *   only on `id == 0 && sd && !si && !enable`.
 *
 *   the transfer.  Setting bit 7 of SIOCNT starts one; hardware clears it when
 *   the transfer lands and raises the serial interrupt.
 *
 *   the clock.  On hardware the parent re-arms from timer 3, set to
 *   SYSTEM_CLOCK / 60 / 16 so that sixteen transfers happen per frame.  That
 *   is one MultiSio packet (14 halfwords plus a sync word) per frame with two
 *   slots of slack, and it is where PORT_SIO_SLOTS comes from.
 *
 * Why this is a poll and not a hook
 * ---------------------------------
 * The port maps the I/O block at 0x04000000 as plain memory, so there is no
 * way to notice a store to SIOCNT -- the same problem DISPSTAT has, and the
 * same shape of answer (see PortVBlankConsume in port/port.h).  Nothing here
 * needs to react to the write *at* the write: a transfer that hardware would
 * have run some microseconds later is run at the next slot instead.  So the
 * frame loop calls PortSioFrame once a frame, and it walks sixteen slots,
 * running a transfer at each one the game has armed.
 *
 * The interrupt is the interesting part.  MultiSioMain switches the parent off
 * the serial interrupt and onto timer 3 (multi_sio.c, state 0), and crt0.s's
 * IntrMain tests those two flags *together*, before anything else, and sends
 * both to gIntrTable[0].  So both units run the same handler and the port only
 * has one interrupt to raise; PortDispatchInterrupt reproduces that routing.
 */

#include <string.h>

#include "port/port.h"
#include "port/mp.h"
#include "global.h"
#include "main.h"
#include "multi_sio.h"

#define SIOCNT      (*(vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOCNT))
#define SIOMLT_SEND (*(vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOMLT_SEND))
#define SIOMULTI    ((vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOMULTI0))

/* SIOCNT, multi-play mode.  Named here rather than taken from io_reg.h because
 * the decomp's names for these are the ones the *game* uses them under, and a
 * couple of them are misleading out of context -- SIO_MULTI_PARENT is the SD
 * terminal, not a "this unit is the parent" flag. */
#define CNT_SI      0x0004  /* 0 = this unit clocks the cable                */
#define CNT_SD      0x0008  /* 1 = every unit on the cable is ready          */
#define CNT_ID      0x0030  /* which slot this unit occupies                 */
#define CNT_ID_SHIFT     4
#define CNT_ERROR   0x0040
#define CNT_START   0x0080  /* write to start a transfer; clear when it lands */
#define CNT_MODE    0x3000

static u32 sTransfers;      /* transfers run since the transport was attached */
static u32 sStalls;         /* slots the transport declined                   */
static int sEverUp;

/* Multi-play, and not one of the other three things the same register drives.
 * The game does use the others: multi_sio_08158934.c puts SIOCNT into 32-bit
 * normal mode for a handshake of its own, and running multi-play transfers
 * underneath that would corrupt it. */
static int InMultiMode(void)
{
    return (SIOCNT & CNT_MODE) == SIO_MULTI_MODE;
}

/* The bits the console drives and the game only reads.  Refreshed every frame
 * rather than once, because MultiSioInit clears the whole register
 * (`*(vu32 *)REG_ADDR_SIOCNT = SIO_MULTI_MODE`) and the game calls it from at
 * least five places. */
static void RefreshStatus(const struct PortMpLink *link)
{
    u16 cnt = SIOCNT;

    cnt &= ~(CNT_SI | CNT_SD | CNT_ID | CNT_ERROR);
    if (link->up) {
        cnt |= (u16)((link->selfId << CNT_ID_SHIFT) & CNT_ID);
        /* SD low means "not everybody is ready", which is what a cable with
         * nothing on it reads as, and it is what stops MultiSioMain promoting
         * a lone console to parent and talking to itself. */
        cnt |= CNT_SD;
        if (link->selfId != 0)
            cnt |= CNT_SI;
    }
    if (link->error)
        cnt |= CNT_ERROR;
    SIOCNT = cnt;
}

/* The parent starts each transfer by setting bit 7; a child is clocked by the
 * parent and has nothing to arm.  MultiSioIntr sets the bit again at the end
 * of every interrupt, which is what keeps the chain going once MultiSioMain
 * has kicked it once. */
static int Armed(const struct PortMpLink *link)
{
    if (link->selfId == 0)
        return (SIOCNT & CNT_START) != 0;
    return 1;
}

static int RunTransfer(const struct PortMpLink *link)
{
    u16 recv[PORT_MP_PLAYERS];
    u16 send = SIOMLT_SEND;
    int i;

    for (i = 0; i < PORT_MP_PLAYERS; i++)
        recv[i] = 0xFFFF;

    if (!PortMpExchange(send, recv)) {
        sStalls++;
        return 0;
    }

    /* A unit reads its own word back out of its own slot.  Doing it here
     * rather than asking the transport for it means a transport never has to
     * know which slot it is in to be correct. */
    if (link->selfId < PORT_MP_PLAYERS)
        recv[link->selfId] = send;

    for (i = 0; i < PORT_MP_PLAYERS; i++)
        SIOMULTI[i] = recv[i];

    SIOCNT &= (u16)~CNT_START;
    sTransfers++;

    PortDispatchInterrupt(INTR_FLAG_SERIAL);
    return 1;
}

void PortSioFrame(void)
{
    struct PortMpLink link;
    int slot;

    if (!PortMpLinkState(NULL))
        return;                     /* no transport attached; nothing to do */

    PortMpPoll(&link);
    if (!InMultiMode())
        return;

    RefreshStatus(&link);
    if (!link.up)
        return;

    if (!sEverUp) {
        sEverUp = 1;
        PortLog("[katam-port] link up: %u player(s), this unit is slot %u (%s)",
                link.players, link.selfId,
                link.selfId == 0 ? "parent" : "child");
    }

    for (slot = 0; slot < PORT_SIO_SLOTS; slot++) {
        if (!Armed(&link))
            break;
        if (!RunTransfer(&link))
            break;
    }
}

void PortSioStats(u32 *transfers, u32 *stalls)
{
    if (transfers)
        *transfers = sTransfers;
    if (stalls)
        *stalls = sStalls;
}

void PortSioReset(void)
{
    sTransfers = 0;
    sStalls = 0;
    sEverUp = 0;
}
