/* The wide build's moved per-player tables (port/wide.h): the relocation
 * table, the console-layout mirror read, power-on and RAM-reset handling.
 * Empty unless KATAM_MAX_KIRBYS is set. */

#include "port/wide.h"

#if KATAM_WIDE

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "main.h"
#include "pause_help.h"
#include "palette_effects.h"
#include "port/port.h"
#include "port/simblock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define K  KATAM_KIRBYS
#define RS KATAM_ROOM_SLOTS

#if defined(__cplusplus)
#define WIDE_ASSERT(c, m) static_assert(c, m)
#else
#define WIDE_ASSERT(c, m) _Static_assert(c, m)
#endif
WIDE_ASSERT(sizeof(struct Kirby) == WIDE_SZ_KIRBY, "wide: struct Kirby");
WIDE_ASSERT(sizeof(struct AnimInfo) * 185 == WIDE_SZ_ANIM, "wide: AnimInfo");
WIDE_ASSERT(sizeof(struct LevelInfo) == WIDE_SZ_LEVELINFO, "wide: LevelInfo");
WIDE_ASSERT(sizeof(struct Unk_02038590) == WIDE_SZ_BRAIN, "wide: brain");
WIDE_ASSERT(sizeof(struct PauseMenu) == WIDE_SZ_PAUSE, "wide: pause menu");
WIDE_ASSERT(sizeof(union Unk_02028EE0) == WIDE_SZ_RS_EE0, "wide: tilemap");
WIDE_ASSERT(sizeof(struct Unk_020382D0) == WIDE_SZ_INPUT, "wide: input block");
WIDE_ASSERT(sizeof(struct Unk_03000510) <= WIDE_SZ_FREEZE, "wide: freeze mask");
WIDE_ASSERT(sizeof(struct PaletteEffectManager) == WIDE_SZ_PALFX, "wide: palette effects");
#if K == 4
/* W0: the moved tables keep their exact console shape. */
WIDE_ASSERT(sizeof(struct Unk_020382D0) == 688, "W0: input block shape");
WIDE_ASSERT(sizeof(struct Unk_03000510) == 5, "W0: freeze mask shape");
#endif

/* Every moved table: where the console had it (and how big it was there),
 * and where it is now.  `stride` is the per-entry size now and origStride the
 * console's, equal for every table except the input block, whose inner [4]s
 * widen (stride 0: not an array of entries). */
static const struct PortWideReloc kRelocs[] = {
#define R(name, orig, origSize, off, nowSize, stride) \
    { name, orig, origSize, WIDE_ADDR(off), nowSize, stride, stride }
    R("gKirbys",       0x02020EE0u, 4 * WIDE_SZ_KIRBY,     WIDE_OFF_KIRBYS,    K * WIDE_SZ_KIRBY,     WIDE_SZ_KIRBY),
    R("gUnk_02021590", 0x02021590u, 4 * WIDE_SZ_ANIM,      WIDE_OFF_ANIM,      K * WIDE_SZ_ANIM,      WIDE_SZ_ANIM),
    R("gUnk_02022EB0", 0x02022EB0u, 8,                     WIDE_OFF_CEB0,      RS * 2,                2),
    R("gUnk_02022EC0", 0x02022EC0u, 0x80,                  WIDE_OFF_CEC0,      RS * 32,               32),
    R("gUnk_02022F40", 0x02022F40u, 16,                    WIDE_OFF_CF40,      16,                    1),
    R("gUnk_02022F50", 0x02022F50u, 0x400,                 WIDE_OFF_CF50,      RS * 256,              256),
    R("gUnk_02023358", 0x02023358u, 16,                    WIDE_OFF_RS358,     RS * 4,                4),
    R("gUnk_02023388", 0x02023388u, 0x100,                 WIDE_OFF_RS388,     RS * 64,               64),
    R("gUnk_02023508", 0x02023508u, 8,                     WIDE_OFF_RS508,     RS * 2,                2),
    R("gUnk_02023510", 0x02023510u, 8,                     WIDE_OFF_RS510,     RS * 2,                2),
    R("gUnk_02023518", 0x02023518u, 8,                     WIDE_OFF_RS518,     RS * 2,                2),
    R("gUnk_02023520", 0x02023520u, 16,                    WIDE_OFF_RS520,     RS * 4,                4),
    R("gCurLevelInfo", 0x02023530u, 4 * WIDE_SZ_LEVELINFO, WIDE_OFF_LEVELINFO, K * WIDE_SZ_LEVELINFO, WIDE_SZ_LEVELINFO),
    R("gUnk_02024ED0", 0x02024ED0u, 4 * WIDE_SZ_RS_ED0,    WIDE_OFF_RSED0,     RS * WIDE_SZ_RS_ED0,   WIDE_SZ_RS_ED0),
    R("gUnk_02026D50", 0x02026D50u, 8,                     WIDE_OFF_RSD50,     RS * 2,                2),
    R("gUnk_02026D60", 0x02026D60u, 4 * WIDE_SZ_RS_D60,    WIDE_OFF_RSD60,     RS * WIDE_SZ_RS_D60,   WIDE_SZ_RS_D60),
    R("gUnk_02028EE0", 0x02028EE0u, 4 * WIDE_SZ_RS_EE0,    WIDE_OFF_RSEE0,     RS * WIDE_SZ_RS_EE0,   WIDE_SZ_RS_EE0),
    R("gUnk_020382D0", 0x020382D0u, 688,                   WIDE_OFF_INPUT,     WIDE_SZ_INPUT,         0),
    R("gUnk_02038590", 0x02038590u, 4 * WIDE_SZ_BRAIN,     WIDE_OFF_BRAINS,    K * WIDE_SZ_BRAIN,     WIDE_SZ_BRAIN),
    R("gPauseMenus",   0x0203ACC0u, 4 * WIDE_SZ_PAUSE,     WIDE_OFF_PAUSE,     K * WIDE_SZ_PAUSE,     WIDE_SZ_PAUSE),
    R("gUnk_03000510", 0x03000510u, 5,                     WIDE_OFF_FREEZE,    WIDE_SZ_FREEZE,        0),
    R("gPaletteEffectManager", 0x02022930u, 0xA0,          WIDE_OFF_PALFX,     WIDE_SZ_PALFX,         0),
    /* Not RAM on the console: the ROM table gUnk_082D8D74, rebuilt here. */
    R("gUnk_082D8D74", 0x082D8D74u, 48,                    WIDE_OFF_BGTAB,     3 * K * 4,             0),
#undef R
};
#define NUM_RELOCS ((int)(sizeof kRelocs / sizeof kRelocs[0]))

/* Diagnostics only: host memory, never read by the simulation. */
static struct PortWideStats sStats;

int PortWideCollRoom(u32 count)
{
    if (count + 1 > sStats.collPeak)
        sStats.collPeak = count + 1;
    if (count >= 31) {
        sStats.collDrops++;
        return 0;
    }
    return 1;
}

void PortWideTaskFull(void)
{
    sStats.taskFull++;
}

void PortWideTaskCount(u32 n)
{
    if (n > sStats.taskPeak)
        sStats.taskPeak = n;
}

static void PrintStats(void)
{
    fprintf(stderr, "[katam-port] wide: kirbys %d, task peak %u/128, task pool full %u times, "
            "collision list peak %u/32, collision adds refused %u\n", K,
            (unsigned)sStats.taskPeak, (unsigned)sStats.taskFull,
            (unsigned)sStats.collPeak, (unsigned)sStats.collDrops);
}

const struct PortWideStats *PortWideGetStats(void)
{
    return &sStats;
}

int PortWideRelocs(const struct PortWideReloc **out)
{
    *out = kRelocs;
    return NUM_RELOCS;
}

/* A console address in a moved table, to where that byte is now.  Exact for
 * N = 4; above 4 the first four entries of each table are where the console
 * had them, which is all a console address can mean. */
static uintptr_t Translate(u32 a)
{
    int i;

    for (i = 0; i < NUM_RELOCS; i++)
        if (a >= kRelocs[i].orig && a < kRelocs[i].orig + kRelocs[i].origSize
            && (K == 4 || kRelocs[i].stride != 0))
            return (uintptr_t)kRelocs[i].now + (a - kRelocs[i].orig);
    return (uintptr_t)a;
}

/* Every EWRAM_MIRROR_U8 in the game is &gCurLevelInfo[x].unk65E with x an
 * owner byte, 0xFF for an object nobody owns (global.h).  In range it is a
 * plain read; out of range the console read EWRAM at
 * (0x02023530 + x * 0x668 + 0x65E) mirrored to 256 KiB, and so do we. */
u8 PortWideMirrorU8(uintptr_t addr)
{
    u32 a = (u32)addr;
    u32 li = WIDE_ADDR(WIDE_OFF_LEVELINFO), orig;

    if (a >= li && a < li + K * WIDE_SZ_LEVELINFO)
        return *(const u8 *)addr;
    orig = 0x02023530u + (a - li);
    orig = 0x02000000u + ((orig - 0x02000000u) & 0x3FFFFu);
    return *(const u8 *)Translate(orig);
}

u16 PortWideKirbyRoomAt(u32 slot)
{
    u32 c;

    if (slot < K)
        return gKirbys[slot].base.roomId;
    /* a screen slot: the console's numbering is 4..7 */
    c = slot - K + 4;
    return *(const u16 *)Translate(0x02020EE0u + c * WIDE_SZ_KIRBY
                                   + (u32)offsetof(struct Kirby, base.roomId));
}

static void FillBgTab(void)
{
    u32 *t = (u32 *)(uintptr_t)WIDE_ADDR(WIDE_OFF_BGTAB);
    int p, layer;

    for (layer = 0; layer < 3; layer++)
        for (p = 0; p < K; p++)
            t[layer * K + p] = (u32)(uintptr_t)&gCurLevelInfo[p].unk180[layer];
}

void PortWideInit(void)
{
    const char *path = getenv("KATAM_WIDE_MAP");

    FillBgTab();
    {
        static int sAt;

        if (!sAt) {
            sAt = 1;
            atexit(PrintStats);
        }
    }
    /* KATAM_WIDE_MAP=FILE: the relocation table, for tools/wide_unrelocate.py. */
    if (path != NULL && *path != '\0') {
        FILE *f = fopen(path, "w");
        int i;

        if (f != NULL) {
            fprintf(f, "kirbys %d sim_base %08X wide_off %08X used %08X\n", K,
                    (unsigned)PORT_SIM_BASE, (unsigned)WIDE_SIM_OFFSET,
                    (unsigned)PortSimUsedBytes());
            for (i = 0; i < NUM_RELOCS; i++)
                fprintf(f, "%s %08X %X %08X %X %X\n", kRelocs[i].name, (unsigned)kRelocs[i].orig,
                        (unsigned)kRelocs[i].origSize, (unsigned)kRelocs[i].now,
                        (unsigned)kRelocs[i].nowSize, (unsigned)kRelocs[i].stride);
            fclose(f);
        }
    }
}

/* RegisterRamReset (platform/bios.c): the console clears EWRAM and IWRAM, and
 * with them every table that lived there. */
void PortWideRamReset(u32 flags)
{
    int i;

    for (i = 0; i < NUM_RELOCS; i++) {
        u32 o = kRelocs[i].orig;
        int ew = o >= 0x02000000u && o < 0x02040000u, iw = o >= 0x03000000u && o < 0x03008000u;

        if ((ew && (flags & RESET_EWRAM)) || (iw && (flags & RESET_IWRAM)))
            memset((void *)(uintptr_t)kRelocs[i].now, 0, kRelocs[i].nowSize);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* KATAM_WIDE */
