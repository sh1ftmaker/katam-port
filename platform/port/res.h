#ifndef GUARD_PORT_RES_H
#define GUARD_PORT_RES_H

/* The resource seam (docs/design/MODDING.md §1).
 *
 * Every ROM table the game names (game/generated/port/rom_data.h) reads its
 * base address from gResBase[], one slot per label, instead of from a literal.
 * At power-on each slot holds the label's ROM address, so a build that never
 * resolves anything else behaves exactly as the address macros did.  The
 * resource layer retargets a slot -- at a compiled pack or a mod -- once per
 * session, before the game starts; never per frame.
 *
 * Call sites do not change: `gRoomProps[id].songIdx` still compiles, it just
 * loads the base first.  New code and tools address resources as
 * (kind, numeric id) through Res_Get.
 *
 * Force-included into every translation unit through port/rom_data.h, so this
 * header stays small and C/C++-neutral. */

#include <stdint.h>
#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One slot per ROM_TABLE line of port/rom_tables.h, in that order. */
enum ResRomTable {
#define ROM_TABLE(label, addr) RT_##label,
#include "port/rom_tables.h"
#undef ROM_TABLE
    RT_COUNT
};

/* The retargetable bases, as integers so the table is constant-initialised in
 * the C++ (LP64) build as well -- a pointer cast is not a constant expression
 * there, and a dynamically initialised table could be read by another file's
 * static constructor before it is filled.  Host width (8 bytes under LP64):
 * they are never
 * stored in game memory, only read to form an address.  What the game *does*
 * store -- a pointer into a table -- keeps its PTR32 width from the macro's
 * type, and stays below 4 GiB because every layer lives in the GBA map
 * (the ROM image, or the resource arena below). */
extern uintptr_t gResBase[RT_COUNT];

/* `label` is pasted, not expanded, so a macro may name its own slot. */
#define RES_BASE(label) (gResBase[RT_##label])

/* --- kinds ---------------------------------------------------------------
 * MODDING.md §1.2.  The realm split is binding for the content hash; see
 * there.  RES_ROM_TABLE is the escape hatch for every label that has no kind
 * of its own yet: id = RT_* slot, result = the slot's base. */
typedef enum ResKind {
    /* --- sim realm --- */
    RES_ROOM_PROPS,      /* struct RoomProps, id = room id (0..982)                       */
    RES_ROOM_TILEDBG,    /* struct RoomTiledBG (fg tilemap header), id = mapDataIdx       */
    RES_ROOM_BG,         /* struct RoomTiledBG (shared background), id = backgroundIdx    */
    RES_ROOM_COLLISION,  /* struct SolidityMap, id = solidityMapIdx                       */
    RES_ROOM_OBJLIST,    /* struct LevelInfo_1E4, id = objectListIdx                      */
    RES_ROOM_PROGRESS,   /* gUnk_08D640A4 entry, id = doorsIdx                            */
    RES_ROOM_NAV,        /* gUnk_08D63C28 entry, id = unk22                               */
    RES_FG_ANIM,         /* struct ForegroundInfo, id = pixelDataIdx                      */
    RES_FG_PALANIM,      /* struct ForegroundPalette, id = paletteDataIdx                 */
    RES_OBJ_DESC,        /* struct Unk_08351648, id = object type (0..0xDA)               */
    RES_SONG_TABLE,      /* struct Song, id = song number                                 */
    /* --- presentation realm (sim-class until NETPLAY-V2 masks VRAM/PLTT) --- */
    RES_PALETTE,         /* fg room palette, 6 banks x 16 BGR555, id = paletteDataIdx     */
    /* --- untyped --- */
    RES_ROM_TABLE,       /* any rom_data.h label, id = RT_* slot                          */
    RES_KIND_COUNT
} ResKind;

typedef struct ResView {
    const void *ptr;     /* NULL if absent */
    u32 size;            /* bytes at ptr that belong to this resource (0 if unknown) */
    u32 layer;           /* RES_LAYER_* that supplied it */
    u32 hash32;          /* FNV-1a over size bytes; 0 if size is 0 */
} ResView;

/* Overlay layers, lowest priority first (MODDING.md §1.3). */
enum {
    RES_LAYER_ROM = 0,   /* the mapped ROM image */
    RES_LAYER_PACK,      /* the extracted vanilla pack, compiled */
    RES_LAYER_BUILTIN,   /* first-party mods (rando, later) -- not populated yet */
    RES_LAYER_MOD,       /* user mods, in load order */
    RES_LAYER_COUNT
};

const void *Res_Get(ResKind kind, u32 id);   /* NULL if absent; stable for the session */
ResView     Res_View(ResKind kind, u32 id);
u32         Res_Count(ResKind kind);         /* one past the largest valid id */
void        Res_Invalidate(ResKind kind, u32 id); /* editor hot reload; a no-op for now */

/* Which layer currently supplies a slot (RES_LAYER_*). */
u32         Res_SlotLayer(u32 slot);
/* Point a slot somewhere else.  Session build only. */
void        Res_SetSlot(u32 slot, void *base, u32 layer);
/* The ROM address a slot started at (its rom_tables.h address). */
u32         Res_SlotRomAddr(u32 slot);
const char *Res_SlotName(u32 slot);

/* Session build: resolve every layer the host configured (Res_Configure*)
 * and retarget the slots.  Called once, after the ROM is mapped and before
 * PortLoadRomDataCopies / PortPatchRomFunctionPointers.  Returns 0 on success;
 * on failure every slot is back at its ROM address and the error is logged. */
int         Res_BuildSession(void);

/* Host configuration, before Res_BuildSession. */
void        Res_ConfigurePack(const char *dir);    /* L1: a folder written by --extract */
void        Res_AddMod(const char *dir);           /* L3: a mod folder; call in load order */
/* Room bundles (katam-native/1, `katam compile` OUT/mod/rooms.katn; MODDING.md
 * R2, platform/res_rooms.c).  Besides <pack>/rooms.katn and each mod's
 * rooms.katn: a file (--rooms FILE) and one in-memory bundle (the web page).
 * They need no pack: without one they link over the ROM's tables. */
void        Res_AddRoomBundle(const char *path);
void        Res_SetRoomBundleMemory(const void *data, u32 size);
/* The playtest page's "reload room": re-link a new bundle mid-session (into
 * the other half of the arena); the caller then warps into the room.  Local
 * sessions only -- never in netplay. 0 on success. */
int         Res_ReloadRooms(const void *data, u32 size);
/* MODDING.md §3.5 levelSetHash of the session (pack sim kinds + rooms);
 * 0 when nothing but the ROM is served. */
int         Res_LevelSetHash(u8 out[32]);

/* Room ids.  The original game has 983 room slots (0..982, 286 real rooms);
 * rooms a bundle adds are numbered from 983 up.  Vanilla code that tests
 * "roomId > 0x3D3" and the like means rooms 980-982, and is told apart from
 * new rooms with this. */
#define RES_FIRST_NEW_ROOM 983u
/* gVisitedDoors (u16[0x120], indexed by RoomProps.doorsIdx, entries 0..286
 * saved to SRAM) for any content index: vanilla's entries for 0..286, a
 * port-owned array for the content indices new rooms get (287 and up), which
 * is never written to the save -- visiting a new room leaves a vanilla save
 * byte-identical.  platform/res_rooms.c. */
u16        *PortVisitedDoors(u32 doorsIdx);
void        Res_LogNewRoomProgress(void);
#define PORT_VISITED_DOORS(i) (*PortVisitedDoors(i))
#define RES_ROOM_IS_NEW(r) ((u32)(u16)(r) >= RES_FIRST_NEW_ROOM && (u16)(r) != 0xFFFFu)

/* One-shot command-line actions, run by main() once the ROM is mapped and
 * before anything patches it (platform/res_pack.c):
 *   RES_CLI_EXTRACT    write the vanilla pack for this ROM into dir
 *   RES_CLI_ROUNDTRIP  compile dir and compare with the ROM (gate F0)
 * Res_RunCliAction returns -1 when none was asked for, else an exit status. */
enum { RES_CLI_NONE, RES_CLI_EXTRACT, RES_CLI_ROUNDTRIP };
void        Res_SetCliAction(int action, const char *dir);
int         Res_RunCliAction(void);

/* Where a ROM address is served from now: the arena copy if the resolved
 * table that contains it was relocated, otherwise the address itself.  Used
 * by PortPatchRomFunctionPointers so function pointers patched into ROM
 * structs reach the served copy as well. */
void       *Res_ServedAddr(u32 romAddr);

/* The resource arena (MODDING.md §1.4): fixed address, inside the ROM
 * window already reserved on every host, above save memory. */
#define RES_ARENA_BASE 0x09100000u
#define RES_ARENA_SIZE 0x00F00000u

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_RES_H */
