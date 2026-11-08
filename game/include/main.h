#ifndef GUARD_MAIN_H
#define GUARD_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"
#include "multi_08030C94.h"

#define MAIN_FLAG_BG_PALETTE_SYNC_ENABLE                  0x1
#define MAIN_FLAG_OBJ_PALETTE_SYNC_ENABLE                 0x2
#define MAIN_FLAG_HBLANK_INTR_ENABLE                      0x8
#define MAIN_FLAG_BG_PALETTE_TRANSFORMATION_ENABLE    0x10000
#define MAIN_FLAG_OBJ_PALETTE_TRANSFORMATION_ENABLE   0x20000

struct Unk_020382D0_sub {
    u32 unk0;
    u16 unk4;
}; /* size = 0x8 */

struct Unk_020382D0 {
    u32 unk0;
    u16 unk4;
    u8 unk6;
    u16 unk8[3][4];
    struct Unk_020382D0_sub unk20[0x10];
    struct Unk_020382D0_sub unkA0[4][0x10];
    u8 unk2A0[4];
    u8 unk2A4[4];
    u8 unk2A8[4];
    u8 unk2AC;
    s8 unk2AD;
};

struct Unk_030023F4 {
    s16 unk0;
    s16 unk2;
};

struct BgAffineReg {
    u16 pa, pb, pc, pd;
    u32 x, y;
}; /* size = 0x10 */

struct BlendRegs {
    u16 bldCnt;
    u16 bldAlpha;
    u16 bldY;
};

struct Unk_03002EC0 {
    /* TODO: fixing types breaks sub_081525DC. */
    u32 unk0; // const void *
    u32 unk4; // void *
    u16 unk8;
    u8 fillerA[2];
};

union Unk_030060A0 {
    u32 full;
    u8 parts[4];
};

struct MultiSioData_0_0 {
    u8 unk0;
    u8 unk1;
    u16 hword[6];
    u8 unkE;
    u32 unk10;
};

struct MultiSioData_0_1 {
    u8 unk0;
    u8 unk1;
    u8 unk2;
    u8 unk3;
    u8 unk4[0x10];
};

struct MultiSioData_0_2 {
    u8 unk0;
    u8 unk1;
    u16 unk2;
    union Unk_020382A0_8 unk4;
    u16 unkC;
    u8 unkE;
    u32 unk10;
};

union MultiSioData {
    struct MultiSioData_0_0 pat0;
    struct MultiSioData_0_1 pat1;
    struct MultiSioData_0_2 pat2;
}; /* size = MULTI_SIO_BLOCK_SIZE */

/* PORT: replaced by port/ram_symbols.h -- extern struct Unk_020382D0 gUnk_020382D0; */

/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_030023F0; */
/* PORT: replaced by port/ram_symbols.h -- extern struct Unk_030023F4 gUnk_030023F4; */

/* PORT: replaced by port/ram_symbols.h -- extern u32 gMainFlags; */

/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002450[0x20]; */
/* PORT: replaced by port/ram_symbols.h -- extern FuncType_030068C0 gUnk_03002470[4]; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_03002480; */
/* PORT: replaced by port/ram_symbols.h -- extern u8* gUnk_03002484; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gVramHeapMaxTileSlots; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gNumHBlankCallbacks; */
/* PORT: replaced by port/ram_symbols.h -- extern union MultiSioData gMultiSioRecv[4]; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gUnk_030024E0; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gNumHBlankIntrs; */
/* PORT: replaced by port/ram_symbols.h -- extern struct BlendRegs gBldRegs; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_030024F0; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002514; */
/* PORT: replaced by port/ram_symbols.h -- extern struct BgAffineReg gBgAffineRegs[2]; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gVramHeapStartAddr; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gVramHeapState[0x200]; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_03002544; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002548; */
/* PORT: replaced by port/ram_symbols.h -- extern s16 gUnk_0300254C; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002550; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gMultiSioStatusFlags; */
/* PORT: replaced by port/ram_symbols.h -- extern bool8 gMultiSioEnabled; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002760[2][0x280] __attribute__((aligned(4))); */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gObjPalette[0x100] __attribute__((aligned(4))); */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gFrameCount; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gWinRegs[6]; */
/* Four per-BG rectangles of BG tilemap entries, measured in tiles, that
 * sub_08154B14 clears to gUnk_030060A0.parts[bg] and then zeroes. One 4-byte
 * entry per BG, indexed bg * 4:
 *     +0 left, +1 top, +2 right, +3 bottom -- both corners inclusive.
 * An all-zero entry means "nothing to clear". A right of 0xFF means "full
 * width", which lets the whole span of rows go out in a single DMA. */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002E80[4 * 4]; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002E82[]; // == gUnk_03002E80 + 2 */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03002E83[]; // == gUnk_03002E80 + 3 */

/*
 * Holds key presses as registered by REG_KEYINPUT in each frame.
 */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gHeldKeys; */

/* PORT: replaced by port/ram_symbols.h -- extern u32 gUnk_03002E94; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gRepeatedKeysTestCounter[NUM_KEYS]; */
/* PORT: replaced by port/ram_symbols.h -- extern u8* gUnk_03002EAC; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gBgCntRegs[4]; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gRepeatedKeys; // works like gPressedKeys but can be retriggered by holding */
/* PORT: replaced by port/ram_symbols.h -- extern struct Unk_03002EC0 gUnk_03002EC0[]; */
/* PORT: replaced by port/ram_symbols.h -- extern OamData gUnk_030031C0[]; */
/* PORT: replaced by port/ram_symbols.h -- extern HBlankFunc gHBlankCallbacks[4]; // Only copied to gHBlankIntrs in VBLANK to make the HBLANK interrupts consistent. */
                                       // Never used in katam but used in sonic advance 2. 
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_030035D4; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gKeysFirstRepeatIntervals[NUM_KEYS]; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gReleasedKeys; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gUnk_03003670; */
/* PORT: replaced by port/ram_symbols.h -- extern s16 gUnk_0300367C; */
/* PORT: replaced by port/ram_symbols.h -- extern s16 gBgScrollRegs[4][2]; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gDispCnt; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gKeysContinuedRepeatIntervals[NUM_KEYS]; */
/* PORT: replaced by port/ram_symbols.h -- extern union MultiSioData gMultiSioSend; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_030036C4; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gUnk_030036C8; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03003790; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gBgPalette[0x100] __attribute__((aligned(4))); */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_030039A0; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_030039A4; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gPrevInput; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_030039AC; */

/*
 * Holds a pressed key from gHeldKeys only in the first frame when it was pressed, as can be seen in GetInput().
 * This may be used by tasks to reliably trigger a branch exactly once when a key is pressed, and not for all frames that the key is pressed.
 */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gPressedKeys; */

/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03003A00; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03003A04; */
/* PORT-FORK: in the port sim block, not host .bss (crt0 had it in IWRAM):
 * the VBlank handler copies the callbacks here and HBlank calls them, so it is
 * simulation state.  port/simblock.h, NETPLAY-V2 R7. */
#include "port/simblock.h"
#define gHBlankIntrs (*(PTR32_TD(HBlankFunc) (*)[4])(uintptr_t)(PORT_SIM_BASE + PORT_SIM_GAME_HBLANK))
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03006070; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_03006074; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03006078; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_0300607C; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_03006080[0x20]; */
/* PORT: replaced by port/ram_symbols.h -- extern union Unk_030060A0 gUnk_030060A0; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gOamBuffer[]; */
/* PORT: replaced by port/ram_symbols.h -- extern u8 gUnk_030068B0; */
/* PORT: replaced by port/ram_symbols.h -- extern u16 gUnk_030068B4; */
/* PORT: replaced by port/ram_symbols.h -- extern s16 gUnk_030068B8; */
/* PORT: replaced by port/ram_symbols.h -- extern FuncType_030068C0 gUnk_030068C0[4]; */
/* PORT: replaced by port/ram_symbols.h -- extern const u32 *gInputPlaybackData; */
/* PORT: replaced by port/ram_symbols.h -- extern bool8 gExecSoundMain; */

/* PORT: replaced by port/ram_symbols.h -- extern IntrFunc gIntrTable[]; */
/* PORT: replaced by port/ram_symbols.h -- extern u32 gIntrMainBuf[0x80]; */

extern IntrFunc const gIntrTableTemplate[];

extern void GameLoop(void);
extern void UpdateScreenDma(void);
extern void GameInit(void);
extern void WaitForInput(void);
extern void GetInput(void);
extern void UpdateScreenCpuSet(void);
extern void ClearOamBufferCpuSet(void);
extern void ClearOamBufferDma(void);

#ifdef __cplusplus
}
#endif

#endif
