/* The resource seam: gResBase[], Res_Get, and the per-session overlay build.
 * docs/design/MODDING.md §1; the API is platform/port/res.h.
 *
 * Stage R0 of the migration (MODDING.md §1.5): every slot starts at the ROM
 * address port/rom_tables.h gives it, so with no pack configured nothing
 * moves.  With a pack (--res-pack, platform/res_pack.c) the kinds the pack
 * compiler knows are compiled into the resource arena and their slots
 * retargeted there; everything else stays on the ROM. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "port/port.h"
#include "port/res.h"
#include "res_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- the slots ------------------------------------------------------------ */

uintptr_t gResBase[RT_COUNT] = {
#define ROM_TABLE(label, addr) (uintptr_t)(addr),
#include "port/rom_tables.h"
#undef ROM_TABLE
};

static const u32 kSlotRom[RT_COUNT] = {
#define ROM_TABLE(label, addr) (addr),
#include "port/rom_tables.h"
#undef ROM_TABLE
};

static const char *const kSlotName[RT_COUNT] = {
#define ROM_TABLE(label, addr) #label,
#include "port/rom_tables.h"
#undef ROM_TABLE
};

static u8 sSlotLayer[RT_COUNT];   /* RES_LAYER_ROM == 0 */

u32 Res_SlotLayer(u32 slot)       { return slot < RT_COUNT ? sSlotLayer[slot] : 0; }
u32 Res_SlotRomAddr(u32 slot)     { return slot < RT_COUNT ? kSlotRom[slot] : 0; }
const char *Res_SlotName(u32 slot){ return slot < RT_COUNT ? kSlotName[slot] : "?"; }

void Res_SetSlot(u32 slot, void *base, u32 layer)
{
    if (slot >= RT_COUNT)
        return;
    gResBase[slot] = (uintptr_t)base;
    sSlotLayer[slot] = (u8)layer;
}

static void ResetSlots(void)
{
    u32 i;

    for (i = 0; i < RT_COUNT; i++) {
        gResBase[i] = kSlotRom[i];
        sSlotLayer[i] = RES_LAYER_ROM;
    }
}

/* --- relocations: ROM range -> served copy -------------------------------- */

#define MAX_RELOCS 512
static struct { u32 lo, size; u8 *dst; } sRelocs[MAX_RELOCS];
static u32 sNumRelocs;

void ResAddRelocation(u32 romLo, u32 size, void *dst)
{
    if (sNumRelocs < MAX_RELOCS) {
        sRelocs[sNumRelocs].lo = romLo;
        sRelocs[sNumRelocs].size = size;
        sRelocs[sNumRelocs].dst = (u8 *)dst;
        sNumRelocs++;
    }
}

void *Res_ServedAddr(u32 romAddr)
{
    u32 i;

    for (i = 0; i < sNumRelocs; i++)
        if (romAddr - sRelocs[i].lo < sRelocs[i].size)
            return sRelocs[i].dst + (romAddr - sRelocs[i].lo);
    return (void *)(uintptr_t)romAddr;
}

/* --- kinds ------------------------------------------------------------------
 * How (kind, id) maps onto a slot.  Counts are the table extents in the
 * vanilla ROM (from katam.elf symbol spacing; assets/manifest.json carries the
 * same numbers for the extractor).  PTRTAB tables are arrays of 4-byte ROM
 * pointers to the records; ARRAY tables hold the records inline. */

enum { ARRAY, PTRTAB };

static const struct {
    u16 slot;
    u16 count;
    u8  shape;
    u16 size;     /* record size in bytes; 0 = variable/unknown */
} kKinds[RES_KIND_COUNT] = {
    /* RES_ROOM_PROPS     */ { RT_gRoomProps,          983, ARRAY,  sizeof(struct RoomProps) },
    /* RES_ROOM_TILEDBG   */ { RT_gForegroundTilemaps, 287, PTRTAB, sizeof(struct RoomTiledBG) },
    /* RES_ROOM_BG        */ { RT_gRoomTiledBGs,        36, PTRTAB, sizeof(struct RoomTiledBG) },
    /* RES_ROOM_COLLISION */ { RT_gSolidityMaps,       287, PTRTAB, sizeof(struct SolidityMap) },
    /* RES_ROOM_OBJLIST   */ { RT_gLevelObjLists,      287, PTRTAB, 0 },
    /* RES_ROOM_PROGRESS  */ { RT_gUnk_08D640A4,       287, PTRTAB, 0 },
    /* RES_ROOM_NAV       */ { RT_gUnk_08D63C28,       287, PTRTAB, 0 },
    /* RES_FG_ANIM        */ { RT_gForegroundInfo,      34, PTRTAB, 0 },
    /* RES_FG_PALANIM     */ { RT_gForegroundPalettes,  42, PTRTAB, sizeof(struct ForegroundPalette) },
    /* RES_OBJ_DESC       */ { RT_gUnk_08351648,       219, ARRAY,  sizeof(struct Unk_08351648) },
    /* RES_SONG_TABLE     */ { RT_gSongTable,          620, ARRAY,  8 },
    /* RES_PALETTE        */ { RT_gForegroundPalettes,  42, PTRTAB, RES_FG_PALETTE_BYTES },
    /* RES_ROM_TABLE      */ { 0,                 RT_COUNT, ARRAY,  0 },
};

/* The extents in force this session: the vanilla ones, grown by the room
 * bundle loader (platform/res_rooms.c) when a bundle names a larger index. */
static u32 sKindCount[RES_KIND_COUNT];

void ResSetKindCount(int kind, u32 count)
{
    if ((u32)kind < RES_KIND_COUNT)
        sKindCount[kind] = count;
}

static void ResetKindCounts(void)
{
    u32 k;

    for (k = 0; k < RES_KIND_COUNT; k++)
        sKindCount[k] = kKinds[k].count;
}

u32 Res_Count(ResKind kind)
{
    return (u32)kind < RES_KIND_COUNT ? sKindCount[kind] : 0;
}

static u32 RomWord(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

const void *Res_Get(ResKind kind, u32 id)
{
    const u8 *base;
    const void *rec;

    if ((u32)kind >= RES_KIND_COUNT || id >= Res_Count(kind))
        return NULL;
    if (kind == RES_ROM_TABLE)
        return (const void *)gResBase[id];

    base = (const u8 *)gResBase[kKinds[kind].slot];
    if (kKinds[kind].shape == ARRAY)
        return base + (uintptr_t)id * kKinds[kind].size;

    rec = (const void *)(uintptr_t)RomWord(base + 4u * id);
    if (rec && kind == RES_PALETTE)
        /* The palette is the record's first member, not the record. */
        rec = (const void *)(uintptr_t)RomWord(rec);
    return rec;
}

ResView Res_View(ResKind kind, u32 id)
{
    ResView v;
    u32 i, h = 2166136261u;

    memset(&v, 0, sizeof v);
    v.ptr = Res_Get(kind, id);
    if (!v.ptr)
        return v;
    v.size = kind == RES_ROM_TABLE ? 0 : kKinds[kind].size;
    v.layer = kind == RES_ROM_TABLE ? sSlotLayer[id] : sSlotLayer[kKinds[kind].slot];
    for (i = 0; i < v.size; i++)
        h = (h ^ ((const u8 *)v.ptr)[i]) * 16777619u;
    v.hash32 = v.size ? h : 0;
    return v;
}

void Res_Invalidate(ResKind kind, u32 id)
{
    (void)kind;
    (void)id;
}


/* --- session build ----------------------------------------------------------- */

/* C tables in game/ whose initialisers name ROM tables.  They cannot be static
 * initialisers any more (a slot is a variable), so each file evaluates them
 * in a function, run after every session build so they follow the slots. */
void PortResRebind_hud(void);
void PortResRebind_multi_08019F28(void);
void PortResRebind_code_08026044(void);

static void RebindGameTables(void)
{
    PortResRebind_hud();
    PortResRebind_multi_08019F28();
    PortResRebind_code_08026044();
}

static const char *sPackDir;
#define MAX_MODS 16
static const char *sMods[MAX_MODS];
static u32 sNumMods;

void Res_ConfigurePack(const char *dir) { sPackDir = dir; }

#define MAX_ROOM_FILES 8
static const char *sRoomFiles[MAX_ROOM_FILES];
static u32 sNumRoomFiles;
static u8 *sRoomMem;
static u32 sRoomMemSize;

void Res_AddRoomBundle(const char *path)
{
    if (sNumRoomFiles < MAX_ROOM_FILES)
        sRoomFiles[sNumRoomFiles++] = path;
    else
        PortLog("[res] more than %d --rooms files; ignoring %s", MAX_ROOM_FILES, path);
}

void Res_SetRoomBundleMemory(const void *data, u32 size)
{
    free(sRoomMem);
    sRoomMem = NULL;
    sRoomMemSize = 0;
    if (data && size) {
        sRoomMem = (u8 *)malloc(size);
        if (sRoomMem) {
            memcpy(sRoomMem, data, size);
            sRoomMemSize = size;
        }
    }
}

static u32 sArenaTop;
void ResSetArenaTop(u32 addr) { sArenaTop = addr; }

/* Gather the room bundles in priority order (pack, mods, --rooms, memory)
 * and link them.  Returns -1 if any was given and could not be installed. */
static int InstallRooms(int packInstalled)
{
    char path[1024];
    u32 i;
    int bad = 0;

    ResRoomsReset();
    if (packInstalled) {
        snprintf(path, sizeof path, "%s/rooms.katn", sPackDir);
        bad |= ResRoomsAddFile(path) < 0;
    }
    for (i = 0; i < sNumMods; i++) {
        snprintf(path, sizeof path, "%s/rooms.katn", sMods[i]);
        bad |= ResRoomsAddFile(path) < 0;
    }
    for (i = 0; i < sNumRoomFiles; i++) {
        int r = ResRoomsAddFile(sRoomFiles[i]);

        if (r == 0)
            PortLog("[res] --rooms %s: no such file", sRoomFiles[i]);
        bad |= r <= 0;
    }
    if (sRoomMem)
        bad |= ResRoomsAddMemory(sRoomMem, sRoomMemSize, "(page)") != 0;
    if (bad || ResRoomsInstall(sArenaTop ? sArenaTop : RES_ARENA_BASE) != 0) {
        ResRoomsReset();
        return -1;
    }
    return 0;
}

static u8 sLevelSet[32];
static int sHaveLevelSet;

int Res_LevelSetHash(u8 out[32])
{
    if (sHaveLevelSet)
        memcpy(out, sLevelSet, 32);
    return sHaveLevelSet;
}

int Res_ReloadRooms(const void *data, u32 size)
{
    int rc = ResRoomsReload(data, size);

    if (rc == 0)
        sHaveLevelSet = ResFinishLevelSetHash(sLevelSet);
    return rc;
}

void Res_AddMod(const char *dir)
{
    if (sNumMods < MAX_MODS)
        sMods[sNumMods++] = dir;
    else
        PortLog("[res] more than %d mods; ignoring %s", MAX_MODS, dir);
}

int Res_BuildSession(void)
{
    int rc;

    ResetSlots();
    ResetKindCounts();
    sNumRelocs = 0;
    sArenaTop = 0;
    sHaveLevelSet = 0;

    /* MODDING.md §1.4's open risk: the game reads past the end of its own
     * data in places, and the arena sits in the ROM window those reads used
     * to find empty.  KATAM_RES_POISON=1 fills the (unused) arena with 0xA5
     * before the game starts, so a regression run shows whether any such
     * read lands in it: none of the four recordings moves (R0 soak). */
    {
        const char *p = getenv("KATAM_RES_POISON");

        if (p && *p == '1')
            memset((void *)(uintptr_t)RES_ARENA_BASE, 0xA5, RES_ARENA_SIZE);
    }

    rc = 0;
    if (sPackDir) {
        rc = ResPackInstall(sPackDir, sMods, sNumMods);
        if (rc != 0) {
            PortLog("[res] pack %s not installed; running from the ROM", sPackDir);
            ResetSlots();
            sNumRelocs = 0;
        }
    }
    else if (sNumMods)
        PortLog("[res] mods' assets need the vanilla pack (--res-pack DIR); "
                "only their room bundles are used (%u mod(s))", (unsigned)sNumMods);

    /* The room bundles (MODDING.md R2): on top of the pack, or of the ROM. */
    if (InstallRooms(sPackDir && rc == 0) != 0) {
        PortLog("[res] room bundles not installed; the game runs without them");
        rc = rc ? rc : -1;
    }
    sHaveLevelSet = ResFinishLevelSetHash(sLevelSet);

    RebindGameTables();
    return rc;
}

#ifdef __cplusplus
}
#endif
