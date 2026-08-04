/* BIOS syscalls.
 *
 * asm/libagbsyscall.s is 15 one-instruction `swi` wrappers.  Every one of them
 * is a documented, self-contained routine, so this file is a straight
 * reimplementation rather than anything emulated.
 *
 * The one that matters structurally is VBlankIntrWait: on hardware it halts
 * the CPU until the display finishes drawing.  Here it is the frame boundary
 * -- it renders the frame from whatever the game last wrote to VRAM, runs the
 * game's VBlank handler, and yields to the browser.
 */

#include <math.h>
#include <string.h>

#include "port/port.h"
#include "gba/gba.h"
#include "main.h"

/* --- arithmetic ---------------------------------------------------------- */

s32 Div(s32 num, s32 denom)
{
    if (denom == 0)
        return 0;   /* hardware faults; the port stays alive instead */
    return num / denom;
}

s32 Mod(s32 num, s32 denom)
{
    if (denom == 0)
        return 0;
    return num % denom;
}

u16 Sqrt(u32 num)
{
    /* Integer square root, matching the BIOS's truncating result. */
    u32 rem = 0, root = 0;
    int i;

    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | (num >> 30);
        num <<= 2;
        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }
    return (u16)(root >> 1);
}

u16 ArcTan2(s16 x, s16 y)
{
    /* BIOS angle units: 0x10000 is a full turn, measured counter-clockwise. */
    double a = atan2((double)y, (double)x);
    s32 angle = (s32)(a * 65536.0 / (2.0 * 3.14159265358979323846));
    return (u16)(angle & 0xFFFF);
}

/* --- block moves --------------------------------------------------------- */

void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & 0x1FFFFF;
    int fixed = (control & CPU_SET_SRC_FIXED) != 0;
    u32 i;

    PortVBlankConsume(count * ((control & CPU_SET_32BIT) ? 4 : 2));

    if (control & CPU_SET_32BIT) {
        const u32 *s = (const u32 *)src;
        u32 *d = (u32 *)dest;
        for (i = 0; i < count; i++)
            d[i] = fixed ? *s : s[i];
    } else {
        const u16 *s = (const u16 *)src;
        u16 *d = (u16 *)dest;
        for (i = 0; i < count; i++)
            d[i] = fixed ? *s : s[i];
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    /* Hardware moves 8 words at a time and rounds the count up to a multiple
     * of 8.  Callers rely on that rounding. */
    u32 count = ((control & 0x1FFFFF) + 7) & ~7u;
    int fixed = (control & CPU_FAST_SET_SRC_FIXED) != 0;
    const u32 *s = (const u32 *)src;
    u32 *d = (u32 *)dest;
    u32 i;

    PortVBlankConsume(count * 4);

    for (i = 0; i < count; i++)
        d[i] = fixed ? *s : s[i];
}

/* --- decompression -------------------------------------------------------
 * Both formats carry a 4-byte header: type in the low nibble of byte 0,
 * decompressed size in the upper 3 bytes.  The Vram variants exist on hardware
 * only because VRAM cannot take 8-bit writes; here they are the same routine. */

static void LZ77UnComp(const u32 *srcp, void *dest)
{
    const u8 *src = (const u8 *)srcp;
    u8 *dst = (u8 *)dest;
    u32 size = (src[1] | (src[2] << 8) | (src[3] << 16));
    u32 written = 0;

    src += 4;
    while (written < size) {
        u8 flags = *src++;
        int bit;

        for (bit = 0; bit < 8 && written < size; bit++) {
            if (flags & 0x80) {
                u32 b0 = *src++;
                u32 b1 = *src++;
                u32 len = (b0 >> 4) + 3;
                u32 disp = (((b0 & 0xF) << 8) | b1) + 1;
                u32 i;

                for (i = 0; i < len && written < size; i++, written++) {
                    dst[written] = dst[written - disp];
                }
            } else {
                dst[written++] = *src++;
            }
            flags <<= 1;
        }
    }
}

void LZ77UnCompWram(const u32 *src, void *dest) { LZ77UnComp(src, dest); }
void LZ77UnCompVram(const u32 *src, void *dest) { LZ77UnComp(src, dest); }

static void RLUnComp(const void *srcp, void *dest)
{
    const u8 *src = (const u8 *)srcp;
    u8 *dst = (u8 *)dest;
    u32 size = (src[1] | (src[2] << 8) | (src[3] << 16));
    u32 written = 0;

    src += 4;
    while (written < size) {
        u8 flags = *src++;

        if (flags & 0x80) {
            u32 len = (flags & 0x7F) + 3;
            u8 value = *src++;
            while (len-- && written < size)
                dst[written++] = value;
        } else {
            u32 len = (flags & 0x7F) + 1;
            while (len-- && written < size)
                dst[written++] = *src++;
        }
    }
}

void RLUnCompWram(const void *src, void *dest) { RLUnComp(src, dest); }
void RLUnCompVram(const void *src, void *dest) { RLUnComp(src, dest); }

/* --- affine helpers ------------------------------------------------------ */

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    /* `offset` is the byte stride between consecutive matrix entries: 2 for a
     * packed array, 8 when writing straight into OAM's affine slots. */
    s16 *out = (s16 *)dest;
    s32 stride = offset / 2;
    s32 i;

    for (i = 0; i < count; i++) {
        double theta = (src[i].rotation >> 8) * 2.0 * 3.14159265358979323846 / 256.0;
        double c = cos(theta), s = sin(theta);
        s16 sx = src[i].xScale, sy = src[i].yScale;

        out[stride * 0] = (s16)(sx * c);
        out[stride * 1] = (s16)(-sx * s);
        out[stride * 2] = (s16)(sy * s);
        out[stride * 3] = (s16)(sy * c);
        out += stride * 4;
    }
}

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    s32 i;

    for (i = 0; i < count; i++) {
        double theta = (src[i].alpha >> 8) * 2.0 * 3.14159265358979323846 / 256.0;
        double c = cos(theta), s = sin(theta);
        s32 sx = src[i].sx, sy = src[i].sy;
        s16 pa = (s16)(sx * c);
        s16 pb = (s16)(-sx * s);
        s16 pc = (s16)(sy * s);
        s16 pd = (s16)(sy * c);

        dest[i].pa = pa;
        dest[i].pb = pb;
        dest[i].pc = pc;
        dest[i].pd = pd;
        dest[i].dx = src[i].texX - (pa * src[i].scrX + pb * src[i].scrY);
        dest[i].dy = src[i].texY - (pc * src[i].scrX + pd * src[i].scrY);
    }
}

/* --- system -------------------------------------------------------------- */

void VBlankIntrWait(void)
{
    PortPresentFrame();
}

void SoftReset(u32 resetFlags)
{
    (void)resetFlags;
    /* The game soft-resets on A+B+Start+Select.  Restarting the wasm module is
     * a host concern; report it and keep running rather than dropping into an
     * unreachable state. */
    PortUnimplemented("SoftReset");
}

void RegisterRamReset(u32 resetFlags)
{
    if (resetFlags & RESET_EWRAM)
        memset((void *)GBA_EWRAM_BASE, 0, GBA_EWRAM_SIZE);
    if (resetFlags & RESET_IWRAM)
        memset((void *)GBA_IWRAM_BASE, 0, GBA_IWRAM_SIZE - 0x200);
    if (resetFlags & RESET_PALETTE)
        memset((void *)GBA_PLTT_BASE, 0, GBA_PLTT_SIZE);
    if (resetFlags & RESET_VRAM)
        memset((void *)GBA_VRAM_BASE, 0, GBA_VRAM_SIZE);
    if (resetFlags & RESET_OAM)
        memset((void *)GBA_OAM_BASE, 0, GBA_OAM_SIZE);
}

int MultiBoot(struct MultiBootParam *mp)
{
    (void)mp;
    PortUnimplemented("MultiBoot (link cable)");
    return 1;   /* non-zero: transfer failed */
}

void SoundBiasReset(void) { }
void SoundBiasSet(void) { }

void PortHalt(void)
{
    /* `swi 3` waits for any interrupt.  The only interrupt source the port
     * drives is the frame boundary, so halting is waiting for a frame. */
    PortPresentFrame();
}
