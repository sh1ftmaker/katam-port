/* asm/multi_sio_asm.s, in C: the two halves of Nintendo's MultiSio library
 * that were never a decompilation target.
 *
 * src/multi_sio.c is the rest of that library and compiles as written --
 * MultiSioInit, MultiSioMain, MultiSioSendDataSet, MultiSioRecvDataCheck.  The
 * two functions here are the ones it runs with interrupts disabled, which is
 * why they are hand-written ARM: the library copies their machine code into
 * IWRAM buffers (gMultiSioIntrFuncBuf, gMultiSioRecvFuncBuf) and calls them
 * from there, so a cartridge waitstate cannot make a serial interrupt late.
 *
 * That trick is the same one that stopped this port at boot in agb_sram.c: in
 * WebAssembly code is not addressable as data, and a function pointer is a
 * table index rather than an address.  So MultiSioInit's CpuCopy32 of
 * MultiSioIntr copies nothing useful, and the buffer the game then installs in
 * gIntrTable[0] is not callable.  The port answers that in two places:
 * PortDispatchInterrupt recognises the buffer address and calls MultiSioIntr
 * instead, and tools/portify.py rewrites MultiSioRecvDataCheck's call through
 * gMultiSioRecvFuncBuf into a direct one.  Both are noted where they are.
 *
 * Provenance
 * ----------
 * Written from KATAM's own asm/multi_sio_asm.s, against the struct layout in
 * include/multi_sio.h.  The decompilation also carries the SDK's C source for
 * both routines, as the `#ifndef MULTI_SIO_DI_FUNC_FAST` branch of
 * src/multi_sio.c -- the branch the game does not build -- and it agrees with
 * the assembly instruction for instruction, which is a useful check on the
 * transcription and is why the comments below can be confident about intent.
 * No third-party link-cable code was copied; the SIO emulation that exists in
 * emulators and in other GBA ports is GPL, and this repository is not.
 *
 * Two places where this is deliberately not a transcription:
 *
 *   The IME dance in MultiSioRecvBufChange is kept, because the port's SIO
 *   unit really can call MultiSioIntr from inside PortSioFrame while the game
 *   is between calls -- the ordering it protects against is real here too.
 *
 *   The timer writes at the end of MultiSioIntr are kept and do nothing.  On
 *   hardware they re-arm timer 3, which is what clocks the next transfer; in
 *   the port platform/sio.c supplies that cadence from the frame loop.  They
 *   are left in because removing them would make this file disagree with the
 *   assembly for no gain, and because a future timer implementation should
 *   find them already there.
 */

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

#define SIOCNT      (*(vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOCNT))
#define SIOMLT_SEND (*(vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOMLT_SEND))
#define SIOMULTI    ((vu16 *)(GBA_IO_BASE + REG_OFFSET_SIOMULTI0))

/* The packet is 14 halfwords: two of header, ten of payload, and two of
 * "overrun catch" that exist only so that a transfer arriving later than
 * expected has somewhere to land.  Index 11 (PACKET_HW - 3) is therefore the
 * last real halfword, which is where the receive side rotates its buffers, and
 * index 13 is where both counters stop. */
#define PACKET_HW ((s32)(sizeof(struct MultiSioPacket) / 2))

/*------------------------------------------------------------------*/
/*              Update Receive Data/Check Buffer Routine            */
/*------------------------------------------------------------------*/

/* Hands MultiSioRecvDataCheck the frame's completed receive buffers and the
 * four "a packet landed" flags, atomically with respect to the interrupt.
 *
 * The receive side is triple-buffered per unit: the interrupt fills
 * currentRecvBufp, rotates it to lastRecvBufp when a packet completes, and
 * this routine rotates lastRecvBufp to recvCheckBufp for the frame's
 * inspection.  Getting that swap and the flag read done under one IME=0 is the
 * entire reason the routine exists.
 *
 * The flags come back packed into a word because the caller stores them
 * straight into a u8[4]; doing the packing explicitly rather than through a
 * cast keeps it from depending on how the compiler feels about aliasing. */
u32 MultiSioRecvBufChange(void)
{
    u32 flags;
    s32 i;

    REG_IME = 0;

    for (i = 0; i < MULTI_SIO_PLAYERS_MAX; i++) {
        u16 *tmp = gMultiSioArea.recvCheckBufp[i];

        gMultiSioArea.recvCheckBufp[i] = gMultiSioArea.lastRecvBufp[i];
        gMultiSioArea.lastRecvBufp[i] = tmp;
    }

    flags = (u32)gMultiSioArea.syncRecvFlag[0]
          | (u32)gMultiSioArea.syncRecvFlag[1] << 8
          | (u32)gMultiSioArea.syncRecvFlag[2] << 16
          | (u32)gMultiSioArea.syncRecvFlag[3] << 24;
    gMultiSioArea.syncRecvFlag[0] = 0;
    gMultiSioArea.syncRecvFlag[1] = 0;
    gMultiSioArea.syncRecvFlag[2] = 0;
    gMultiSioArea.syncRecvFlag[3] = 0;

    REG_IME = 1;
    return flags;
}

/*------------------------------------------------------------------*/
/*                  Multi-play Interrupt Routine                    */
/*------------------------------------------------------------------*/

/* One transfer has landed.  Take the four halfwords out of SIOMULTI0..3, put
 * the next halfword of our own packet into SIOMLT_SEND, and -- if we are the
 * parent -- start the next transfer.
 *
 * A packet is streamed one halfword per transfer, preceded by the sync word
 * 0xFEFE.  The sync word is what re-aligns a unit that missed a transfer: a
 * receiver that sees it after the payload should have ended resets its counter
 * to -1 instead of storing it, and picks the next packet up from the top. */
void MultiSioIntr(void)
{
    u16 recvTmp[MULTI_SIO_PLAYERS_MAX];
    u16 *bufpTmp;
    s32 i;

    for (i = 0; i < MULTI_SIO_PLAYERS_MAX; i++)
        recvTmp[i] = SIOMULTI[i];

    gMultiSioArea.hardError = (u8)(SIOCNT & SIO_ERROR);

    /* --- send ---------------------------------------------------------- */
    if (gMultiSioArea.sendBufCounter == -1) {
        /* Top of a packet.  MultiSioSendDataSet has been filling the buffer we
         * are not sending from; swap them here, under the interrupt, so the
         * frame's writer and the transfer never touch the same one. */
        SIOMLT_SEND = MULTI_SIO_SYNC_DATA;
        bufpTmp = gMultiSioArea.currentSendBufp;
        gMultiSioArea.currentSendBufp = gMultiSioArea.nextSendBufp;
        gMultiSioArea.nextSendBufp = bufpTmp;
    } else if (gMultiSioArea.sendBufCounter >= 0) {
        SIOMLT_SEND =
            gMultiSioArea.currentSendBufp[gMultiSioArea.sendBufCounter];
    }
    if (gMultiSioArea.sendBufCounter < PACKET_HW - 1)
        ++gMultiSioArea.sendBufCounter;

    /* --- receive -------------------------------------------------------- */
    for (i = 0; i < MULTI_SIO_PLAYERS_MAX; i++) {
        if (recvTmp[i] == MULTI_SIO_SYNC_DATA
         && gMultiSioArea.recvBufCounter[i] > PACKET_HW - 3) {
            gMultiSioArea.recvBufCounter[i] = -1;
        } else {
            /* The counter is never -1 here: the only assignment of -1 is in
             * the branch above, and the increment below turns it into 0 before
             * this line is reached again.  Worth stating, because the index is
             * signed and a negative one would store a halfword in front of the
             * buffer -- which on hardware is merely the previous field of the
             * same struct, and here would be a real out-of-bounds write. */
            gMultiSioArea.currentRecvBufp[i][gMultiSioArea.recvBufCounter[i]] =
                recvTmp[i];

            if (gMultiSioArea.recvBufCounter[i] == PACKET_HW - 3) {
                bufpTmp = gMultiSioArea.lastRecvBufp[i];
                gMultiSioArea.lastRecvBufp[i] = gMultiSioArea.currentRecvBufp[i];
                gMultiSioArea.currentRecvBufp[i] = bufpTmp;
                gMultiSioArea.syncRecvFlag[i] = 1;
            }
        }
        if (gMultiSioArea.recvBufCounter[i] < PACKET_HW - 1)
            ++gMultiSioArea.recvBufCounter[i];
    }

    /* --- re-arm --------------------------------------------------------- */
    /* The assembly tests `type != 0`, not `type == SIO_MULTI_PARENT`; the
     * field only ever holds 0 or SIO_MULTI_PARENT, so they agree. */
    if (gMultiSioArea.type != 0) {
        REG_MULTI_SIO_TIMER_H = 0;
        SIOCNT |= SIO_ENABLE;
        REG_MULTI_SIO_TIMER_H = TIMER_INTR_ENABLE | TIMER_ENABLE;
    }
}

#ifdef __cplusplus
}
#endif
