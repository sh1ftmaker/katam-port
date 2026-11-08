#ifndef GUARD_GBA_FLASH_INTERNAL_H
#define GUARD_GBA_FLASH_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif


#define FLASH_BASE ((u8 *)0xE000000)

#define FLASH_WRITE(addr, data) ((*(vu8 *)(FLASH_BASE + (addr))) = (data))

#define FLASH_ROM_SIZE_1M 131072 // 1 megabit ROM

#define SECTORS_PER_BANK 16

struct FlashSector
{
    u32 size;
    u8 shift;
    u16 count;
    u16 top;
};

struct FlashType {
    u32 romSize;
    struct FlashSector sector;
    u16 wait[2]; // game pak bus read/write wait

    // TODO: add support for anonymous unions/structs if possible
    union {
        struct {
        u8 makerId;
        u8 deviceId;
        } separate;
        u16 joined;
    } ids;
};

struct FlashSetupInfo
{
    PTR32_FN(u16, programFlashByte, (u16, u32, u8));
    PTR32_FN(u16, programFlashSector, (u16, void *));
    PTR32_FN(u16, eraseFlashChip, (void));
    PTR32_FN(u16, eraseFlashSector, (u16));
    PTR32_FN(u16, WaitForFlashWrite, (u8, u8 *, u8));
    PTR32(const u16) maxTime;
    struct FlashType type;
};

#ifdef __cplusplus
}
#endif

#endif // GUARD_GBA_FLASH_INTERNAL_H
