#ifndef GUARD_GBA_SYSCALL_H
#define GUARD_GBA_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif


#define RESET_EWRAM      0x01
#define RESET_IWRAM      0x02
#define RESET_PALETTE    0x04
#define RESET_VRAM       0x08
#define RESET_OAM        0x10
#define RESET_SIO_REGS   0x20
#define RESET_SOUND_REGS 0x40
#define RESET_REGS       0x80
#define RESET_ALL        0xFF

void SoftReset(u32 resetFlags);

void RegisterRamReset(u32 resetFlags);

void VBlankIntrWait(void);

u16 Sqrt(u32 num);
/* PORT: a distance from Sqrt used as a divisor (the homing enemies: batty,
 * flamer, hot_head, parasol, soarar, ufo, dark_meta_knight, dark_mind).  It
 * is 0 only when both offsets are 0 -- Sqrt(n * 256) >= 16 for n >= 1 -- and
 * then the dividend is 0 as well.  On the GBA agbcc's __divsi3 returns 0 for
 * x / 0 (lib1thumb.asm, Ldiv0); wasm traps ("divide by zero": the page's game
 * stops -- seen in tools/rooms/gates.mjs hostlocal2, seed 2, flamer.c) and
 * x86 raises SIGFPE.  Dividing by 1 instead gives the GBA's 0. */
#define PORT_DIVISOR(d) ((d) ? (d) : 1)

u16 ArcTan2(s16 x, s16 y);

#define CPU_SET_SRC_FIXED 0x01000000
#define CPU_SET_16BIT     0x00000000
#define CPU_SET_32BIT     0x04000000

void CpuSet(const void *src, void *dest, u32 control);

#define CPU_FAST_SET_SRC_FIXED 0x01000000

void CpuFastSet(const void *src, void *dest, u32 control);

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count);

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset);

void LZ77UnCompWram(const u32 *src, void *dest);

void LZ77UnCompVram(const u32 *src, void *dest);

void RLUnCompWram(const void *src, void *dest);

void RLUnCompVram(const void *src, void *dest);

int MultiBoot(struct MultiBootParam *mp);

s32 Div(s32 num, s32 denom);

s32 Mod(s32 num, s32 denom);

void SoundBiasReset(void);

void SoundBiasSet(void);

#ifdef __cplusplus
}
#endif

#endif // GUARD_GBA_SYSCALL_H
