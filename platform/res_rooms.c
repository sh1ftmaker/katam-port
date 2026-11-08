/* The room-bundle loader: MODDING.md R2 for the room bundle.
 *
 * A room bundle (katam-native/1, written by `katam compile` --
 * tools/katam/lib/native.mjs linkBundle) is a position-independent object
 * file: blobs, the pointer fields inside them, and table entries naming which
 * game-table slot points at which blob.  This file is its linker.  It knows no
 * room layout -- every layout fact stays in the compiler -- only which tables
 * it may grow and how big their vanilla extents are.
 *
 *   place    the grown tables, then every blob of every bundle, bump-allocated
 *            in the resource arena after whatever the pack put there,
 *            16-byte aligned, in bundle order: the same bundles give the same
 *            addresses on every host (MODDING.md §1.4).
 *   grow     each named table is copied from where it is served now (the ROM,
 *            or the pack's arena copy) into a table with room for the largest
 *            index any bundle names; so room ids >= 983 and content indices
 *            >= 287 resolve wherever the game indexes these tables.  Vanilla
 *            entries keep pointing at the vanilla records, so a vanilla room
 *            plays exactly as before, pointer for pointer.
 *   link     relocations become arena addresses; entries fill table slots
 *            (a pointer, or for gRoomProps the record itself).
 *
 * Sources, lowest priority first: <pack>/rooms.katn, each mod's rooms.katn in
 * load order, then --rooms FILE / the web page's in-memory bundle.  A later
 * bundle wins for the same table slot.  The SHA-256 of the bundles in that
 * order is the "rooms" term of levelSetHash (MODDING.md §3.5).
 *
 * Nothing here is game state: the arena is immutable for the session (the
 * playtest page's reload re-links into the other half of the free arena, a
 * local-only editor operation that never runs in netplay). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "port/port.h"
#include "port/res.h"
#include "res_internal.h"
#include "res_hash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUNDLE_VERSION 1
#define TABLE_NAME_LEN 24
#define MAX_BUNDLES 20

/* The tables a bundle may grow: vanilla extent, and the inline record size
 * for an array table (0: a table of 4-byte pointers).  The extents are the
 * ones platform/res.c's kinds carry (katam.elf symbol spacing). */
static const struct {
    const char *label;
    u32 count, stride;
    int kind;
} kTables[] = {
    { "gRoomProps",          983, 0x28, RES_ROOM_PROPS },
    { "gForegroundTilemaps", 287, 0,    RES_ROOM_TILEDBG },
    { "gSolidityMaps",       287, 0,    RES_ROOM_COLLISION },
    { "gLevelObjLists",      287, 0,    RES_ROOM_OBJLIST },
    { "gUnk_08D640A4",       287, 0,    RES_ROOM_PROGRESS },
    { "gUnk_08D63C28",       287, 0,    RES_ROOM_NAV },
    { "gForegroundInfo",      34, 0,    RES_FG_ANIM },
    { "gRoomTiledBGs",        36, 0,    RES_ROOM_BG },
    /* Room-id tables beside gRoomProps (no ResKind yet): the AI helpers'
     * room graph + area (1000 slots in the ROM, 286 used) and each room's
     * default cell (u8 x, y; 984 entries, read by sub_08002D40). */
    { "gUnk_08D6CD0C",      1000, 0,    -1 },
    { "gUnk_0893CB44",       984, 2,    -1 },
};
#define NUM_TABLES (sizeof kTables / sizeof kTables[0])

struct Bundle {
    u8 *data;            /* owned copy */
    u32 size;
    char what[160];      /* for messages */
};

static struct Bundle sBundles[MAX_BUNDLES];
static u32 sNumBundles;

/* Session state, kept for a reload: where each table was served from before
 * any bundle (the pack or the ROM), and where the next free arena byte was. */
static uintptr_t sPreBase[NUM_TABLES];
static u32 sPreLayer[NUM_TABLES];
static u32 sArenaStart;
static int sInstalled, sBank;
static u8 sRoomsSha[32];
static u32 sNewRooms, sMaxRoom;

static u32 Rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void Wr32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 SlotOf(const char *label)
{
    u32 s;

    for (s = 0; s < RT_COUNT; s++)
        if (strcmp(Res_SlotName(s), label) == 0)
            return s;
    return RT_COUNT;
}

static int TableOf(const u8 *name)
{
    char n[TABLE_NAME_LEN + 1];
    u32 i;

    memcpy(n, name, TABLE_NAME_LEN);
    n[TABLE_NAME_LEN] = '\0';
    for (i = 0; i < NUM_TABLES; i++)
        if (strcmp(n, kTables[i].label) == 0)
            return (int)i;
    return -1;
}

/* --- sources --------------------------------------------------------------- */

void ResRoomsReset(void)
{
    u32 i;

    for (i = 0; i < sNumBundles; i++)
        free(sBundles[i].data);
    sNumBundles = 0;
}

int ResRoomsAddMemory(const void *data, u32 size, const char *what)
{
    struct Bundle *b;

    if (sNumBundles >= MAX_BUNDLES) {
        PortLog("[res] more than %d room bundles; ignoring %s", MAX_BUNDLES, what);
        return -1;
    }
    b = &sBundles[sNumBundles];
    b->data = (u8 *)malloc(size ? size : 1);
    if (!b->data)
        return -1;
    memcpy(b->data, data, size);
    b->size = size;
    snprintf(b->what, sizeof b->what, "%s", what);
    sNumBundles++;
    return 0;
}

/* 1 if the file was there and was added, 0 if absent, -1 on error. */
int ResRoomsAddFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    u8 *buf;
    int rc;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (u8 *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf || n <= 0 || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        PortLog("[res] %s: cannot read", path);
        return -1;
    }
    fclose(f);
    rc = ResRoomsAddMemory(buf, (u32)n, path);
    free(buf);
    return rc == 0 ? 1 : -1;
}

/* --- checking ---------------------------------------------------------------- */

static int Check(const struct Bundle *b)
{
    const u8 *d = b->data;
    u32 nb, nr, ne, data, i, o;

    if (b->size < 0x20 || memcmp(d, "KATN", 4) != 0) {
        PortLog("[res] %s: not a room bundle (katam-native/1)", b->what);
        return -1;
    }
    if (Rd32(d + 4) != BUNDLE_VERSION) {
        PortLog("[res] %s: bundle version %u; this build reads %u -- recompile it",
                b->what, (unsigned)Rd32(d + 4), BUNDLE_VERSION);
        return -1;
    }
    nb = Rd32(d + 8); nr = Rd32(d + 12); ne = Rd32(d + 16); data = Rd32(d + 20);
    if (nb > 0x10000 || nr > 0x10000 || ne > 0x10000
     || 0x20 + nb * 16 + nr * 16 + ne * (TABLE_NAME_LEN + 12) > data || data > b->size) {
        PortLog("[res] %s: bundle header is inconsistent", b->what);
        return -1;
    }
    for (i = 0, o = 0x20; i < nb; i++, o += 16) {
        u32 off = Rd32(d + o), size = Rd32(d + o + 4);

        if (off > b->size - data || size > b->size - data - off) {
            PortLog("[res] %s: blob %u runs past the end", b->what, (unsigned)i);
            return -1;
        }
    }
    for (i = 0; i < nr; i++, o += 16) {
        u32 blob = Rd32(d + o), at = Rd32(d + o + 4), target = Rd32(d + o + 8);

        if (blob >= nb || target >= nb || at + 4 > Rd32(d + 0x20 + blob * 16 + 4) || (at & 3)) {
            PortLog("[res] %s: relocation %u is out of range", b->what, (unsigned)i);
            return -1;
        }
    }
    for (i = 0; i < ne; i++, o += TABLE_NAME_LEN + 12) {
        int t = TableOf(d + o);
        u32 idx = Rd32(d + o + TABLE_NAME_LEN), blob = Rd32(d + o + TABLE_NAME_LEN + 4);
        u32 inl = Rd32(d + o + TABLE_NAME_LEN + 8);

        if (t < 0) {
            PortLog("[res] %s: entry %u names a table the loader cannot grow", b->what, (unsigned)i);
            return -1;
        }
        if (blob >= nb || idx >= 0xFFFF || inl != (kTables[t].stride ? 1u : 0u)
         || (inl && Rd32(d + 0x20 + blob * 16 + 4) != kTables[t].stride)) {
            PortLog("[res] %s: entry %u (%s[%u]) is malformed", b->what, (unsigned)i,
                    kTables[t].label, (unsigned)idx);
            return -1;
        }
    }
    return 0;
}

/* --- install ------------------------------------------------------------------ */

#define ALIGN16(x) (((x) + 15u) & ~15u)

static int Link(u32 arenaLo, u32 arenaHi, u32 *usedOut)
{
    u32 need[NUM_TABLES], tableAt[NUM_TABLES];
    u32 *blobAt[MAX_BUNDLES];
    u32 next = ALIGN16(arenaLo), t, k, i, o;
    int rc = -1;

    memset(blobAt, 0, sizeof blobAt);
    for (t = 0; t < NUM_TABLES; t++)
        need[t] = 0;

    /* How big each table must grow. */
    for (k = 0; k < sNumBundles; k++) {
        const u8 *d = sBundles[k].data;
        u32 nb = Rd32(d + 8), nr = Rd32(d + 12), ne = Rd32(d + 16);

        for (i = 0, o = 0x20 + nb * 16 + nr * 16; i < ne; i++, o += TABLE_NAME_LEN + 12) {
            int ti = TableOf(d + o);
            u32 idx = Rd32(d + o + TABLE_NAME_LEN);

            if (need[ti] < kTables[ti].count)
                need[ti] = kTables[ti].count;
            if (idx + 1 > need[ti])
                need[ti] = idx + 1;
        }
    }

    /* Place: the grown tables (in kTables order), then the blobs. */
    for (t = 0; t < NUM_TABLES; t++) {
        tableAt[t] = 0;
        if (!need[t])
            continue;
        tableAt[t] = next;
        next = ALIGN16(next + need[t] * (kTables[t].stride ? kTables[t].stride : 4));
    }
    for (k = 0; k < sNumBundles; k++) {
        const u8 *d = sBundles[k].data;
        u32 nb = Rd32(d + 8);

        blobAt[k] = (u32 *)calloc(nb ? nb : 1, sizeof(u32));
        if (!blobAt[k])
            goto out;
        for (i = 0; i < nb; i++) {
            blobAt[k][i] = next;
            next = ALIGN16(next + Rd32(d + 0x20 + i * 16 + 4));
        }
    }
    if (next > arenaHi) {
        PortLog("[res] the room bundles need %u bytes of arena; %u are free",
                (unsigned)(next - arenaLo), (unsigned)(arenaHi - arenaLo));
        goto out;
    }

    /* Grow: copy each table from where it is served now. */
    for (t = 0; t < NUM_TABLES; t++) {
        u32 stride = kTables[t].stride ? kTables[t].stride : 4;
        u8 *dst = (u8 *)(uintptr_t)tableAt[t];

        if (!need[t])
            continue;
        /* Past the vanilla extent: a room-props slot reads as vanilla's
         * filler (all 0xFF), anything else as zero / NULL. */
        memset(dst, t == 0 ? 0xFF : 0x00, need[t] * stride);
        memcpy(dst, (const void *)sPreBase[t], kTables[t].count * stride);
    }

    /* Copy and link the blobs, then fill the table slots. */
    for (k = 0; k < sNumBundles; k++) {
        const u8 *d = sBundles[k].data;
        u32 nb = Rd32(d + 8), nr = Rd32(d + 12), ne = Rd32(d + 16), data = Rd32(d + 20);

        for (i = 0; i < nb; i++)
            memcpy((void *)(uintptr_t)blobAt[k][i], d + data + Rd32(d + 0x20 + i * 16),
                   Rd32(d + 0x20 + i * 16 + 4));
        for (i = 0, o = 0x20 + nb * 16; i < nr; i++, o += 16) {
            u32 blob = Rd32(d + o), at = Rd32(d + o + 4), target = Rd32(d + o + 8);

            Wr32((u8 *)(uintptr_t)blobAt[k][blob] + at, blobAt[k][target] + Rd32(d + o + 12));
        }
        for (i = 0; i < ne; i++, o += TABLE_NAME_LEN + 12) {
            int ti = TableOf(d + o);
            u32 idx = Rd32(d + o + TABLE_NAME_LEN), blob = Rd32(d + o + TABLE_NAME_LEN + 4);
            u8 *slot;

            if (kTables[ti].stride) {
                slot = (u8 *)(uintptr_t)tableAt[ti] + idx * kTables[ti].stride;
                memcpy(slot, (const void *)(uintptr_t)blobAt[k][blob], kTables[ti].stride);
            } else {
                slot = (u8 *)(uintptr_t)tableAt[ti] + idx * 4;
                Wr32(slot, blobAt[k][blob]);
            }
        }
    }

    /* Retarget.  Res_Count follows, so Res_Get answers for the new ids. */
    sNewRooms = 0;
    sMaxRoom = 0;
    for (t = 0; t < NUM_TABLES; t++) {
        u32 slot = SlotOf(kTables[t].label);

        if (!need[t]) {
            Res_SetSlot(slot, (void *)sPreBase[t], sPreLayer[t]);
            ResSetKindCount(kTables[t].kind, kTables[t].count);
            continue;
        }
        Res_SetSlot(slot, (void *)(uintptr_t)tableAt[t], RES_LAYER_MOD);
        ResSetKindCount(kTables[t].kind, need[t]);
        if (t == 0) {
            sMaxRoom = need[t] - 1;
            sNewRooms = need[t] - kTables[t].count;
        }
    }
    *usedOut = next - arenaLo;
    rc = 0;
out:
    for (k = 0; k < sNumBundles; k++)
        free(blobAt[k]);
    return rc;
}

static void HashBundles(void)
{
    /* The bundles in load order: "katam-rooms-v1", then per bundle its size
     * and bytes.  The bytes hold no addresses, so this is placement-free. */
    u8 *all;
    u32 n = 14, k, o;

    for (k = 0; k < sNumBundles; k++)
        n += 4 + sBundles[k].size;
    all = (u8 *)malloc(n);
    if (!all)
        return;
    memcpy(all, "katam-rooms-v1", 14);
    for (k = 0, o = 14; k < sNumBundles; k++) {
        Wr32(all + o, sBundles[k].size);
        memcpy(all + o + 4, sBundles[k].data, sBundles[k].size);
        o += 4 + sBundles[k].size;
    }
    ResSha256(all, n, sRoomsSha);
    free(all);
}

/* Session build, after the pack: link every bundle added since the last
 * ResRoomsReset.  arenaLo is the first free arena byte. */
int ResRoomsInstall(u32 arenaLo)
{
    u32 t, k, used = 0, entries = 0;
    char hex[65];

    sInstalled = 0;
    sBank = 0;
    if (!sNumBundles)
        return 0;
    for (k = 0; k < sNumBundles; k++) {
        if (Check(&sBundles[k]) != 0)
            return -1;
        entries += Rd32(sBundles[k].data + 16);
    }
    for (t = 0; t < NUM_TABLES; t++) {
        u32 slot = SlotOf(kTables[t].label);

        sPreBase[t] = gResBase[slot];
        sPreLayer[t] = Res_SlotLayer(slot);
    }
    sArenaStart = ALIGN16(arenaLo);
    if (Link(sArenaStart, RES_ARENA_BASE + RES_ARENA_SIZE / 2, &used) != 0)
        return -1;
    sInstalled = 1;
    HashBundles();
    ResHex(sRoomsSha, 32, hex);
    for (k = 0; k < sNumBundles; k++)
        PortLog("[res] rooms %u: %s (%u bytes)", (unsigned)k, sBundles[k].what,
                (unsigned)sBundles[k].size);
    PortLog("[res] room bundles linked: %u entries, %u bytes of arena at 0x%08X; "
            "%u room ids past the original 983 (up to %u)", (unsigned)entries,
            (unsigned)used, (unsigned)sArenaStart, (unsigned)sNewRooms, (unsigned)sMaxRoom);
    PortLog("[res]   rooms       sim          sha256 %.16s", hex);
    return 0;
}

int ResRoomsHash(u8 out[32])
{
    if (!sInstalled)
        return 0;
    memcpy(out, sRoomsSha, 32);
    return 1;
}

/* The playtest page's reload: replace the session's bundles with this one and
 * re-link it into the other half of the arena, so the records the current
 * room still points at stay intact until the caller warps (FillLevelInfo then
 * reads the new ones).  Local sessions only. */
int ResRoomsReload(const void *data, u32 size)
{
    u32 lo, hi, used = 0;
    char hex[65];

    if (!sInstalled) {
        PortLog("[res] reload: no room bundle was installed at boot");
        return -1;
    }
    ResRoomsReset();
    if (ResRoomsAddMemory(data, size, "reload") != 0 || Check(&sBundles[0]) != 0)
        return -1;
    sBank ^= 1;
    lo = sBank ? RES_ARENA_BASE + RES_ARENA_SIZE / 2 : sArenaStart;
    hi = sBank ? RES_ARENA_BASE + RES_ARENA_SIZE : RES_ARENA_BASE + RES_ARENA_SIZE / 2;
    if (Link(lo, hi, &used) != 0)
        return -1;
    HashBundles();
    ResHex(sRoomsSha, 32, hex);
    PortLog("[res] room bundle reloaded: %u bytes at 0x%08X, rooms sha256 %.16s",
            (unsigned)used, (unsigned)lo, hex);
    return 0;
}

/* --- progress of new rooms -------------------------------------------------
 *
 * The game keeps each room's door/chest bits and its visited bit in
 * gVisitedDoors[doorsIdx]: 0x120 u16 in EWRAM, of which 0x11F (content 0..286)
 * are part of the save (game/src/save.c gWorldProps).  A new room's content
 * index is 287 or more, which would run off the end of the array (into the
 * live tilemap buffers at 0x02028EE0).  So those indices live here instead:
 * a port-owned array that the save never sees.  New rooms' progress is per
 * session; a vanilla save is untouched by playing them.
 *
 * Self-contained so it can move into the port sim block (NETPLAY-V2 M1,
 * branch netsim: struct PortSimBlock) when that lands; until then it is host
 * memory, which is outside the rollback snapshot and hash (a local playtest
 * does not roll back). */
#define PORT_NEW_CONTENT_BASE 287u
#define PORT_NEW_CONTENT_MAX  512u

static struct PortRoomProgress {
    u16 visitedDoors[PORT_NEW_CONTENT_MAX];
    u16 overflow;                /* content past the cap: written, never read back as real */
} sRoomProgress;

u16 *PortVisitedDoors(u32 doorsIdx)
{
    if (doorsIdx < PORT_NEW_CONTENT_BASE)
        return &gVisitedDoors[doorsIdx];
    if (doorsIdx - PORT_NEW_CONTENT_BASE < PORT_NEW_CONTENT_MAX)
        return &sRoomProgress.visitedDoors[doorsIdx - PORT_NEW_CONTENT_BASE];
    sRoomProgress.overflow = 0;
    return &sRoomProgress.overflow;
}

/* For `katam playtest` and tests/playtest.sh: the new rooms' progress words
 * that are set, as "content:value" pairs. */
void Res_LogNewRoomProgress(void)
{
    char line[256];
    u32 i, n = 0;
    int o = 0;

    for (i = 0; i < PORT_NEW_CONTENT_MAX && o < (int)sizeof line - 24; i++)
        if (sRoomProgress.visitedDoors[i]) {
            o += snprintf(line + o, sizeof line - (size_t)o, " %u:%04X",
                          (unsigned)(i + PORT_NEW_CONTENT_BASE),
                          (unsigned)sRoomProgress.visitedDoors[i]);
            n++;
        }
    if (n)
        PortLog("[res] new-room progress (port-owned, not saved):%s", line);
}

u32 Res_RoomCount(void)
{
    return Res_Count(RES_ROOM_PROPS);
}

#ifdef __cplusplus
}
#endif
