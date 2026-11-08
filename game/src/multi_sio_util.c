
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "gbplayer.h"
#include "main.h"
#include "multi_sio.h"
#include "multi_sio_util.h"

void ResetMultiSio(void)
{
    DmaFill32(3, 0, &gMultiSioSend, sizeof(gMultiSioSend));
    DmaFill32(3, 0, gMultiSioRecv, sizeof(gMultiSioRecv));
    gMultiSioStatusFlags = 0;
    gMultiSioEnabled = FALSE;
    MultiSioInit(0);
}

void EnableMultiSio(void)
{
    if (gMainFlags & 0x80000) {
        REG_IME = 0;
        REG_IE &= ~INTR_FLAG_TIMER3;
        REG_IME = 1;
        REG_IME = 0;
        /* PORT-FNID: was (void *)gMultiSioIntrFuncBuf, the IWRAM copy of
         * MultiSioIntr; its code ID is that buffer's GBA address. */
        gIntrTable[0] = FN(MultiSioIntr);
        REG_IME = 1;
        MultiSioInit(0);
    }
    gMultiSioEnabled = TRUE;
}

void DisableMultiSio(void)
{
    gMultiSioEnabled = FALSE;
    gMainFlags &= ~0x100000;
    MultiSioStop();
    MultiSioInit(0);
    if (gMainFlags & 0x80000)
        nullsub_1();
}

#ifdef __cplusplus
}
#endif
