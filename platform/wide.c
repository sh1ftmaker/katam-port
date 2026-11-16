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
#include "port/localmp.h"
#include "task.h"

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

/* --- The live count ---------------------------------------------------------
 *
 * EXPERIMENTAL > CPU KIRBYS (platform/menu_poc.c) changes how many Kirbys
 * there are while the game runs: 4 (you and the game's own three helpers)
 * up to KATAM_KIRBYS.  The Kirbys in play are always the prefix
 * 0 .. gNumKirbys - 1 -- every loop in the game is `i < gNumKirbys`, and the
 * room-slot choice (level.c FillLevelInfo) is recomputed from those Kirbys
 * each time, so a slot nobody below gNumKirbys holds is free again.
 *
 * The count wanted (gPortSim->wideCtl.target) is simulation state, set by a
 * frame-stamped input (`kirbys FRAME N`, platform/inputlog.c) the host
 * schedules from the setting.  Once a frame, after the latch, while the
 * story is in ordinary play (not the title, the demo, a sub-game, the intro
 * walk, a pause, a door), one Kirby comes or goes:
 *
 *   - in: Kirby gNumKirbys is made as the play start makes Kirbys
 *     (sub_080332BC: CreateKirby, its CPU brain, its level info) and arrives
 *     beside player 1 by the respawn route (PortLmpCarryTo, the game's own
 *     room change).  Only with room in the task pool for it and its effects.
 *   - out: the last Kirby.  Unless it already shares a room with a Kirby
 *     that stays, it is first carried to player 1 (so its room, if it was
 *     alone there, is torn down by the game's own room change); once it is
 *     there and settled, the objects it owns (parent chain) and its two
 *     tasks are destroyed and gNumKirbys drops.  A Kirby that cannot be
 *     moved (dying, mid-door) is waited for.
 *
 * The four the game's start code makes are never taken away (the target is
 * at least 4), so nothing that is unrolled for Kirbys 0..3 ever meets a
 * missing one.
 *
 * People (the human-slot layer, platform/localmp.c).  The count in play is
 * max(4, the setting, the highest slot that is a person or waits to be one
 * + 1): PortWideTarget, from simulation state only, so every device agrees.
 * A join of a slot past the count is held (localmp.c PortLmpSetSlotHuman:
 * waiting), the count rises to it one Kirby a frame, and the join goes
 * ahead once its Kirby exists.  The count shrinks from the end only, and
 * never past a person's Kirby: lowering the setting below a person leaves
 * the count at that person until they leave.  Online, the setting is the
 * host's, sealed (PORT_LN_EV_KIRBYS, platform/lmpnet.c); seats are
 * PORT_LN_EV_HUMAN as at four. */

#define WC (gPortSim->wideCtl)
#define WIDE_TASK_HEADROOM 12       /* free task slots kept for effects */

extern void sub_08055920(struct Kirby *);
void sub_0800ECAC(u8, u16, u16);
void sub_0800EF60(void);

int PortWideTarget(void)
{
    int t = WC.target ? WC.target : K, p = (int)PortLmpPeopleEnd();

    if (t < p)
        t = p;              /* never below a person's Kirby (or a held join) */
    return t < 4 ? 4 : t > K ? K : t;
}

u8 PortWideStartKirbys(void)
{
    return (u8)PortWideTarget();
}

void PortWideSetTarget(int n)
{
    n = n < 4 ? 4 : n > K ? K : n;
    if (WC.target == (u8)n)
        return;
    WC.target = (u8)n;
    PortLog("[katam-port] wide: f%u kirbys -> %d (now %u)", (unsigned)gPortSim->frame.frameCount,
            n, (unsigned)gNumKirbys);
}

static int Settled(const struct Kirby *k)
{
    return k->hp > 0 && k->base.roomId != 0xFFFF && !(k->base.flags & 0x100)
        && gCurLevelInfo[k->base.unk56].currentRoom == k->base.roomId;
}

/* Is the story in ordinary play, and player 1 somewhere a Kirby can be
 * brought to? */
static int CanChange(void)
{
    const struct Kirby *p1 = &gKirbys[0];

    if (gNumKirbys < 4 || !PortLmpInStory() || gAIKirbyState < AI_KIRBY_STATE_NORMAL)
        return 0;
    if (gUnk_03000510.unk4 != 0)        /* a pause (or anything) holds rooms */
        return 0;
    return Settled(p1);
}

static struct Task *BrainOf(u8 s)
{
    int i;

    for (i = 0; i < MAX_TASK_NUM; i++) {
        struct Task *t = &gTasks[i];

        if (t->main == FN(sub_0800EF60) && t->priority == (u16)(0x2100 | s) && t->structOffset)
            return t;
    }
    return NULL;
}

static void *StructOf(const struct Task *t)
{
    return (void *)((t->flags & TASK_USE_EWRAM)
            ? (uintptr_t)EWRAM_START + ((uintptr_t)t->structOffset << 2)
            : (uintptr_t)IWRAM_START + t->structOffset);
}

static void Add(u8 i)
{
    struct Kirby *k = &gKirbys[i], *p1 = &gKirbys[0];
    struct LevelInfo *li = &gCurLevelInfo[i];
    s32 pos[2];
    u16 room = p1->base.roomId;

    pos[0] = p1->base.x;
    pos[1] = p1->base.y;
    /* its level info as InitLevelTasks leaves it (the BGs it set up stay) */
    li->currentRoom = 0xFFFF;
    li->unk65C = 0xFFFF;
    CreateKirby(i, k, room, pos, FALSE);        /* ++gNumKirbys */
    /* (not sub_0803E558: Kirby i shares palette slot i & 3 with a Kirby
     * already in play, whose colours it would overwrite) */
    sub_0800ECAC(i, room, gAIKirbyState > 199 ? 0x321 : 0x323);
    li->unk1EC = 1;
    li->unk660 = i;
    PortLmpCarryTo(k, p1);                      /* sub_08055920 + respawn record */
    WC.added++;
    PortLog("[katam-port] wide: f%u Kirby %u joins (room %u), %u Kirbys, %u tasks",
            (unsigned)gPortSim->frame.frameCount, (unsigned)i + 1, (unsigned)room,
            (unsigned)gNumKirbys, (unsigned)gNumTasks);
}

/* Kirby i (the last) shares a room, and so a room slot, with one that stays. */
static int SharesRoom(u8 i)
{
    const struct Kirby *k = &gKirbys[i];
    u8 j;

    for (j = 0; j < i; j++)
        if (gKirbys[j].base.roomId == k->base.roomId && Settled(&gKirbys[j])
            && gCurLevelInfo[j].unk65E == gCurLevelInfo[i].unk65E)
            return 1;
    return 0;
}

static void Remove(u8 i)
{
    struct Kirby *k = &gKirbys[i];
    const void *gone[24];
    int ngone = 0, changed, n, t, g, killed = 0;

    /* what it owns: objects whose parent is it, or is one of those */
    gone[ngone++] = k;
    do {
        changed = 0;
        for (t = 0; t < MAX_TASK_NUM; t++) {
            struct Task *task = &gTasks[t];
            const u8 *o;
            const void *parent;

            if (!task->main || !task->structOffset || task == k->task)
                continue;
            o = (const u8 *)StructOf(task);
            if (o[0] == 3)
                parent = ((const struct EffectObject *)o)->parent;
            else if (o[0] == 1 || o[0] == 2)
                parent = ((const struct ObjectBase *)o)->parent;
            else
                continue;
            for (g = 0; g < ngone; g++)
                if (parent == gone[g] && (const void *)o != gone[g])
                    break;
            if (g == ngone)
                continue;
            if (ngone < (int)ARRAY_COUNT(gone))
                gone[ngone++] = o;
            TaskDestroy(task);
            killed++;
            changed = 1;
        }
    } while (changed);
    {
        struct Task *b = BrainOf(i);

        if (b != NULL)
            TaskDestroy(b);
    }
    if (k->task != NULL)
        TaskDestroy(k->task);
    k->task = NULL;
    k->base.roomId = 0xFFFF;
    gNumKirbys = i;
    WC.removed++;
    n = gNumKirbys;
    PortLog("[katam-port] wide: f%u Kirby %u leaves (%d of its objects), %d Kirbys, %u tasks",
            (unsigned)gPortSim->frame.frameCount, (unsigned)i + 1, killed, n,
            (unsigned)gNumTasks);
}

/* KATAM_WIDE_TRACE=F: every F frames, each Kirby's room and HP (a log line;
 * reads the simulation, writes nothing). */
static void Trace(void)
{
    static int sEvery = -1;
    char line[16 * 14 + 1];
    int i, n = 0;

    if (sEvery < 0) {
        const char *e = getenv("KATAM_WIDE_TRACE");

        sEvery = e ? atoi(e) : 0;
    }
    if (sEvery <= 0 || gPortSim->frame.frameCount % (u32)sEvery)
        return;
    line[0] = '\0';
    for (i = 0; i < gNumKirbys && i < K; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %u:%d/%d", (unsigned)gKirbys[i].base.roomId,
                      (int)gKirbys[i].hp, (int)gKirbys[i].base.x >> 12);
    PortLog("[katam-port] wide: f%u %u Kirbys, %u tasks:%s", (unsigned)gPortSim->frame.frameCount,
            (unsigned)gNumKirbys, (unsigned)gNumTasks, line);
}

void PortWideFrame(void)
{
    int target = PortWideTarget();
    u8 last;

    Trace();
    if (gNumKirbys == (u8)target && !WC.leaving)
        return;
    if (!CanChange())
        return;
    if (gNumKirbys < (u8)target) {
        WC.leaving = 0;
        if (gNumTasks + 2 + WIDE_TASK_HEADROOM <= MAX_TASK_NUM)
            Add(gNumKirbys);
        return;
    }
    if (gNumKirbys <= (u8)target || gNumKirbys <= 4) {
        WC.leaving = 0;
        return;
    }
    last = (u8)(gNumKirbys - 1);
    if (!Settled(&gKirbys[last]))
        return;                             /* dying, mid-door: wait */
    if (SharesRoom(last)) {
        WC.leaving = 0;
        Remove(last);
        return;
    }
    /* first to player 1 (again, if it wandered off before it arrived) */
    if (WC.leaving != last + 1 || gPortSim->frame.frameCount - WC.leavingSince > 300) {
        WC.leaving = (u8)(last + 1);
        WC.leavingSince = gPortSim->frame.frameCount;
        PortLmpCarryTo(&gKirbys[last], &gKirbys[0]);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* KATAM_WIDE */

/* The host's request, in every build (the default build has nothing to
 * change: the menus show the row as needing the wide module). */
#include "port/inputlog.h"
#include "port/lmpnet.h"
#ifdef __cplusplus
extern "C"
#endif
u32 PortWideRequestKirbys(int n)
{
#if KATAM_WIDE
    n = n < 4 ? 4 : n > KATAM_KIRBYS ? KATAM_KIRBYS : n;
    /* An online session: the host's setting is sealed for every device
     * (PORT_LN_EV_KIRBYS); a guest's is not the session's. */
    if (PortLnActive()) {
        int f = PortLnRole() == PORT_LN_HOST ? PortLnEvent(PORT_LN_EV_KIRBYS, n, 0, 0, 0) : -1;

        return f < 0 ? 0 : (u32)f;
    }
    return PortInputLogScheduleKirbys((u8)n);
#else
    (void)n;
    return 0;
#endif
}
