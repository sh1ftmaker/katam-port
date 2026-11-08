#ifndef GUARD_GBPLAYER_H
#define GUARD_GBPLAYER_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"

struct RfuSIO32Id {
    /* 0x00 */ u8 MS_mode;
    /* 0x02 */ u16 count;
    /* 0x04 */ u16 send_id;
    /* 0x06 */ u16 recv_id;
    /* 0x08 */ u8 filler8[2];
    /* 0x0A */ u16 lastId;
}; /* size = 0xC */

/* PORT: replaced by port/ram_symbols.h -- extern struct RfuSIO32Id gSio32Id; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gSio32SendData; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gSio32ReceivedData; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gSio32RemoteDeviceId; // only bit 0 of the command 1 reply's payload is stored */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gSio32CommState; */

void Sio32TimeoutIntr(void);
void nullsub_1(void);
void nullsub_2(void);

#ifdef __cplusplus
}
#endif

#endif
