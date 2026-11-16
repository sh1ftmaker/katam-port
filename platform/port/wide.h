#ifndef GUARD_PORT_WIDE_H
#define GUARD_PORT_WIDE_H
/* More than four Kirbys in one world: the build flag and the memory layout.
 *
 * docs/design/ONE-WORLD-16.md has the plan and the proof ladder.  In short:
 *
 *   KATAM_MAX_KIRBYS unset   the game as it always was: four Kirby slots, every
 *                            per-player table at the GBA address linker.ld
 *                            gave it.  Nothing in this file changes that build:
 *                            KATAM_WIDE is 0 and KATAM_KIRBYS is 4, and every
 *                            use of them below spells the original constant.
 *   KATAM_MAX_KIRBYS=N       (4 <= N <= 16) the per-player tables -- the Kirbys,
 *                            their animation info, the level info (camera, room,
 *                            BGs), the room slots, the per-slot collision lists,
 *                            the input block, the CPU brains, the pause menus,
 *                            the freeze mask -- move out of EWRAM/IWRAM into a
 *                            "wide area" inside the port sim block
 *                            (port/simblock.h), sized for N.  Being inside the
 *                            sim block, the wide area is in every snapshot, every
 *                            state hash, every state transfer and every dump with
 *                            no change to any of them.
 *
 * N = 4 with the flag set is proof W0: the same program at a different layout.
 * The old EWRAM homes of the moved tables stay mapped and simply stay zero.
 *
 * Room slots.  The game loads each room someone is in into one of four "room
 * slots" (level.c, the free-slot mask over gCurLevelInfo[i].unk65E).  With N
 * Kirbys there can be N rooms in use, so the wide build has N slots.
 *
 * Why inside the sim block and not a region of its own: the snapshot, the
 * three state hashes, the dump and the online image all already cover
 * [PORT_SIM_BASE, PORT_SIM_BASE + PortSimUsedBytes()).  A new region would
 * be a new case in each of them.
 *
 * gCurLevelInfo[0xFF].  An object that belongs to no player has 0xFF as its
 * player id and the game indexes gCurLevelInfo with it (platform/native/mem.c):
 * reads of zero memory ~400 KiB past the array.  So the level info is the LAST
 * table of the wide area, and the sim span is grown under the flag so that
 * gCurLevelInfo[0xFF] still lands in mapped memory nobody writes.  The mirror
 * reads (EWRAM_MIRROR_U8, global.h) are emulated exactly: PortWideMirrorU8
 * below computes the address the console would have read in its own layout
 * and reads that byte wherever it now lives. */

#include "gba/types.h"

#ifdef KATAM_MAX_KIRBYS
#define KATAM_WIDE 1
#define KATAM_KIRBYS KATAM_MAX_KIRBYS
#if KATAM_MAX_KIRBYS < 4 || KATAM_MAX_KIRBYS > 16
#error "KATAM_MAX_KIRBYS must be 4..16 (the owner byte and the u16 masks; see ONE-WORLD-16.md)"
#endif
#else
#define KATAM_WIDE 0
#define KATAM_KIRBYS 4
#endif

/* Room slots: one per Kirby (the most rooms N Kirbys can be in). */
#define KATAM_ROOM_SLOTS KATAM_KIRBYS

/* A Kirby's console hardware slot: OBJ palettes i and i+4 and the 2 KiB of
 * OBJ VRAM at 0x06010000 + (i << 11).  The console has four.  The game no
 * longer uses this: it was the wide build's first placeholder (Kirbys 4..
 * sharing slot i & 3, frames and colours colliding on screen), replaced by
 * KATAM_KIRBY_TILES / KATAM_KIRBY_PAL below, which give every Kirby memory
 * of its own.  Kept for code on other branches that still spells it. */
/* Palette effects (palette_effects.c): the console has eight slots, 0..3 one
 * per Kirby (door fades) and 4..7 for the screen.  The wide build has one per
 * Kirby and the same four screen slots after them; PAL_FX_GLOBAL(g) is the
 * game's screen slot g (4..7) in that numbering. */
#define PAL_FX_SLOTS (KATAM_KIRBYS + 4)
#define PAL_FX_GLOBAL(g) (KATAM_KIRBYS - 4 + (g))

#if KATAM_WIDE
#define KATAM_HW_SLOT(i) ((i) & 3)
#else
#define KATAM_HW_SLOT(i) (i)
#endif

/* A Kirby's own sprite memory (platform/wide_sprites.c has the whole story).
 *
 *   KATAM_KIRBY_TILES(i)  its 2 KiB of tile memory, as a sprite's tilesVram.
 *                         Kirbys 0..3: 0x06010000 + (i << 11), the console's.
 *                         Kirbys 4..: 2 KiB each in the wide area of the sim
 *                         block (WIDE_OFF_OBJX), which the game's own tile
 *                         queue fills as it fills VRAM.
 *   KATAM_KIRBY_PAL(i)    its first OBJ palette line, as a sprite's palId;
 *   KATAM_KIRBY_PAL2(i)   its second (the console's i + 4).  Kirbys 4.. get
 *                         lines past the console's sixteen, in groups of four
 *                         laid out as the console's: 16 * (i / 4) + i % 4,
 *                         and that + 4.  So `palId & 15` is the console slot
 *                         i & 3 (OAM's four bits: what the classic renderer
 *                         shows) and the game's own `palId + 4` still finds a
 *                         Kirby's second line from its first.
 *   KATAM_OAM_TILES(v)    the OAM tile number of tilesVram v: for the wide
 *                         area, the tile of the same place in console slot
 *                         i & 3 (the classic renderer's placeholder); the GPU
 *                         renderer reads the real one from the struct Sprite.
 *   KATAM_OBJ_PAL(off)    &gObjPalette[off] for off up to 16 * 64.
 *   KATAM_PAL_MASK        what the game's `palId & 0xF` keeps.
 *
 * The default build spells the console's expressions. */
#if KATAM_WIDE
#define KATAM_KIRBY_TILES(i) ((u32)(i) < 4 ? 0x6010000u + ((u32)(i) << 11) \
                                           : WIDE_OBJX_TILES + (((u32)(i) - 4u) << 11))
#define KATAM_KIRBY_PAL(i)   ((((i) >> 2) << 4) | ((i) & 3))
#define KATAM_OAM_TILES(v)   PortWideOamTiles(v)
#define KATAM_OBJ_PAL(off)   PortWideObjPal(off)
#define KATAM_PAL_MASK       0xFF
#define KATAM_PAL_OFF        u16     /* a palette offset: lines past 15   */
#else
#define KATAM_KIRBY_TILES(i) (((i) << 11) + 0x6010000)
#define KATAM_KIRBY_PAL(i)   (i)
#define KATAM_OAM_TILES(v)   (((v) - 0x6010000u) >> 5)
#define KATAM_OBJ_PAL(off)   (&gObjPalette[off])
#define KATAM_PAL_MASK       0xF
#define KATAM_PAL_OFF        u8
#endif
#define KATAM_KIRBY_PAL2(i)  (KATAM_KIRBY_PAL(i) + 4)

/* The console HUD's other-Kirby icons (hud.c: struct GameplayHud keeps
 * sprites for four).  PLACEHOLDER in the wide build: they show Kirbys 0..3
 * only, and a screen whose own player is 4.. shows no calling-Kirby icon.  A
 * HUD for more players is its own work (the port's party HUD).  (The pause
 * maps' markers have their own count: KATAM_MAP_MARKERS below.) */
#if KATAM_WIDE
#define KATAM_HUD_KIRBYS (gNumKirbys < 4 ? gNumKirbys : 4)
#else
#define KATAM_HUD_KIRBYS gNumKirbys
#endif

/* The pause screens' per-player menus (pause_help.c gPauseMenus: a menu per
 * Kirby slot in the wide build, port/wide.h WIDE_OFF_PAUSE).  The console's
 * loops visit four.  With more than four Kirby slots they visit every Kirby
 * in play, so that a pauser, or a screen's own player (rooms mode), from 5 on
 * has a menu that is set up, read and closed like 1..4's.  4 at N = 4 (W0).
 * KATAM_PAUSE_MENUS_ALL: every entry (PauseMenuInitRetained, at power-on and
 * each game's start, so a Kirby made later finds its menu ready). */
#if KATAM_KIRBYS > 4
#define KATAM_PAUSE_MENUS     ((int)(gNumKirbys > 4 ? gNumKirbys : 4))
#define KATAM_PAUSE_MENUS_ALL KATAM_KIRBYS
#else
#define KATAM_PAUSE_MENUS     4
#define KATAM_PAUSE_MENUS_ALL 4
#endif

/* The pause area map's and world map's Kirby markers (pause_area_map.c,
 * pause_world_map.c).  The console's maps keep four, one per Kirby, each
 * with 256 bytes of OBJ tiles at 0x06013800 + i * 0x100.  With more Kirby
 * slots they keep up to eight, at 0x06013800 .. 0x06014000 (where the
 * game's own OBJ VRAM heap starts, gVramHeapStartAddr): every Kirby while
 * there are eight or fewer, otherwise the pauser, the screen's own player,
 * the other people and then CPU Kirbys, lowest slot first, eight in all. */
#if KATAM_KIRBYS > 4
#define KATAM_MAP_MARKERS (KATAM_KIRBYS < 8 ? KATAM_KIRBYS : 8)
#else
#define KATAM_MAP_MARKERS 4
#endif

/* min(n, 4) in the wide build, n otherwise: a loop over the Kirbys that
 * indexes one of the game's four-entry tables which are not per Kirby (the
 * screen-shake rooms, code_0806F780.c).  PLACEHOLDER: at most four of them. */
#if KATAM_WIDE
#define KATAM_MIN4(n) ((n) < 4 ? (n) : 4)
#else
#define KATAM_MIN4(n) (n)
#endif

/* An index into the per-room-slot collision lists (gUnk_02022F50: 64 per
 * slot).  The console's is a u8, which holds four slots and no more. */
#if KATAM_WIDE
#define KATAM_COLL_IDX u16
#else
#define KATAM_COLL_IDX u8
#endif

/* May an object be added to a room slot's collision list that holds `count`
 * (object.h SetPointerSomething)?  The console never asks; the wide build's
 * lists stop at 31 (PortWideCollRoom, as code_0806F780.c's two adders). */
#if KATAM_WIDE
#define KATAM_COLL_ROOM(count) PortWideCollRoom(count)
#else
#define KATAM_COLL_ROOM(count) 1
#endif

/* A Kirby's row in one of the game's four-row ROM tables (spawn offsets, CPU
 * path data per room...).  PLACEHOLDER in the wide build: Kirby i uses row
 * i & 3.  Which rows Kirbys 4.. should have is design data nobody wrote. */
#if KATAM_WIDE
#define KATAM_ROM_ROW(i) ((i) & 3)
#else
#define KATAM_ROM_ROW(i) (i)
#endif

#if KATAM_WIDE

/* Sizes of the console structures, asserted against the real types in
 * platform/wide.c (a game translation unit), so this header needs no game
 * header. */
#define WIDE_SZ_KIRBY      0x1A8u     /* struct Kirby                          */
#define WIDE_SZ_ANIM       (185u * 4u) /* struct AnimInfo[185]                 */
#define WIDE_SZ_LEVELINFO  0x668u     /* struct LevelInfo                      */
#define WIDE_SZ_BRAIN      0xF4u      /* struct Unk_02038590                   */
#define WIDE_SZ_PAUSE      0x14u      /* struct PauseMenu                      */
#define WIDE_SZ_RS_ED0     1950u      /* gUnk_02024ED0[slot]: solidity         */
#define WIDE_SZ_RS_D60     1954u      /* gUnk_02026D60[slot]                   */
#define WIDE_SZ_RS_EE0     15600u     /* union Unk_02028EE0: tilemap           */
/* struct Unk_020382D0 with every inner [4] widened (main.h). */
#define WIDE_SZ_INPUT      (((8u + 6u * KATAM_KIRBYS + 0x80u + 0x80u * KATAM_KIRBYS \
                              + 3u * KATAM_KIRBYS + 2u) + 3u) & ~3u)
/* struct Unk_03000510 widened: u8 unk0[N], then the mask (u8 at 4, u16 above). */
#define WIDE_SZ_FREEZE     (KATAM_KIRBYS <= 4 ? 5u : (KATAM_KIRBYS + 2u))

#define WIDE_AL(x) (((x) + 0xFFu) & ~0xFFu)

/* Offsets inside the wide area.  Each table starts on a 256-byte boundary. */
#define WIDE_OFF_KIRBYS    0u
#define WIDE_OFF_ANIM      WIDE_AL(WIDE_OFF_KIRBYS   + KATAM_KIRBYS * WIDE_SZ_KIRBY)
#define WIDE_OFF_CEB0      WIDE_AL(WIDE_OFF_ANIM     + KATAM_KIRBYS * WIDE_SZ_ANIM)
#define WIDE_OFF_CEC0      WIDE_AL(WIDE_OFF_CEB0     + KATAM_ROOM_SLOTS * 2u)
#define WIDE_OFF_CF40      WIDE_AL(WIDE_OFF_CEC0     + KATAM_ROOM_SLOTS * 8u * 4u)
#define WIDE_OFF_CF50      WIDE_AL(WIDE_OFF_CF40     + 16u)
#define WIDE_OFF_RS358     WIDE_AL(WIDE_OFF_CF50     + KATAM_ROOM_SLOTS * 64u * 4u)
#define WIDE_OFF_RS388     WIDE_AL(WIDE_OFF_RS358    + KATAM_ROOM_SLOTS * 4u)
#define WIDE_OFF_RS508     WIDE_AL(WIDE_OFF_RS388    + KATAM_ROOM_SLOTS * 16u * 4u)
#define WIDE_OFF_RS510     WIDE_AL(WIDE_OFF_RS508    + KATAM_ROOM_SLOTS * 2u)
#define WIDE_OFF_RS518     WIDE_AL(WIDE_OFF_RS510    + KATAM_ROOM_SLOTS * 2u)
#define WIDE_OFF_RS520     WIDE_AL(WIDE_OFF_RS518    + KATAM_ROOM_SLOTS * 2u)
#define WIDE_OFF_RSED0     WIDE_AL(WIDE_OFF_RS520    + KATAM_ROOM_SLOTS * 4u)
#define WIDE_OFF_RSD50     WIDE_AL(WIDE_OFF_RSED0    + KATAM_ROOM_SLOTS * WIDE_SZ_RS_ED0)
#define WIDE_OFF_RSD60     WIDE_AL(WIDE_OFF_RSD50    + KATAM_ROOM_SLOTS * 2u)
#define WIDE_OFF_RSEE0     WIDE_AL(WIDE_OFF_RSD60    + KATAM_ROOM_SLOTS * WIDE_SZ_RS_D60)
#define WIDE_OFF_INPUT     WIDE_AL(WIDE_OFF_RSEE0    + KATAM_ROOM_SLOTS * WIDE_SZ_RS_EE0)
#define WIDE_OFF_BRAINS    WIDE_AL(WIDE_OFF_INPUT    + WIDE_SZ_INPUT)
#define WIDE_OFF_PAUSE     WIDE_AL(WIDE_OFF_BRAINS   + KATAM_KIRBYS * WIDE_SZ_BRAIN)
#define WIDE_OFF_FREEZE    WIDE_AL(WIDE_OFF_PAUSE    + KATAM_KIRBYS * WIDE_SZ_PAUSE)
#define WIDE_OFF_BGTAB     WIDE_AL(WIDE_OFF_FREEZE   + WIDE_SZ_FREEZE)
#define WIDE_OFF_PALFX     WIDE_AL(WIDE_OFF_BGTAB    + 3u * KATAM_KIRBYS * 4u)
#define WIDE_SZ_PALFX      (PAL_FX_SLOTS * 0x10u + PAL_FX_SLOTS * 4u)
/* Kirbys 4..'s sprite memory (KATAM_KIRBY_TILES/PAL above), in groups of four
 * Kirbys past the console's group: per group 8 KiB of tiles and 16 OBJ
 * palette lines, three copies of those (the game's buffer, gObjPalette's
 * twin; the palette-effect source, gSourcePalettes.obj's; and the "hardware"
 * copy UpdateScreen makes, OBJ_PLTT's).  Empty at N = 4. */
#define WIDE_OBJX_GROUPS   ((KATAM_KIRBYS + 3u) / 4u - 1u)
#define WIDE_SZ_OBJX_TILES (WIDE_OBJX_GROUPS * 0x2000u)
#define WIDE_OBJX_LINES    (WIDE_OBJX_GROUPS * 16u)
#define WIDE_SZ_OBJX_PAL   (WIDE_OBJX_LINES * 32u)
#define WIDE_OFF_OBJX      WIDE_AL(WIDE_OFF_PALFX    + WIDE_SZ_PALFX)
#define WIDE_SZ_OBJX       (WIDE_SZ_OBJX_TILES + 3u * WIDE_SZ_OBJX_PAL)
#define WIDE_OFF_LEVELINFO WIDE_AL(WIDE_OFF_OBJX     + WIDE_SZ_OBJX)
#define WIDE_BYTES         WIDE_AL(WIDE_OFF_LEVELINFO + KATAM_KIRBYS * WIDE_SZ_LEVELINFO)

/* The wide area's GBA address: PORT_SIM_BASE + offsetof(PortSimBlock, wide).
 * It is the last member of the block, after the rando lists (which the wide
 * build sizes for N seats: 0xB4 + 4130 bytes per seat), page-aligned; asserted
 * in simblock.h. */
#define WIDE_SIM_OFFSET    (((0xB4u + 4130u * KATAM_KIRBYS) + 0xFFFu) & ~0xFFFu)
#define WIDE_BASE          (0x02040000u + WIDE_SIM_OFFSET)
#define WIDE_ADDR(off)     (WIDE_BASE + (off))

/* Kirbys 4..'s tiles and palettes (GBA addresses). */
#define WIDE_OBJX_TILES    WIDE_ADDR(WIDE_OFF_OBJX)
#define WIDE_OBJX_PAL      (WIDE_OBJX_TILES + WIDE_SZ_OBJX_TILES)
#define WIDE_OBJX_SRCPAL   (WIDE_OBJX_PAL + WIDE_SZ_OBJX_PAL)
#define WIDE_OBJX_HWPAL    (WIDE_OBJX_SRCPAL + WIDE_SZ_OBJX_PAL)

/* Where gCurLevelInfo[0xFF] reaches, which must stay mapped and unwritten. */
#define WIDE_LI_REACH      (WIDE_ADDR(WIDE_OFF_LEVELINFO) + 256u * WIDE_SZ_LEVELINFO)

/* The live Kirby count (EXPERIMENTAL > CPU KIRBYS, platform/wide.c "The live
 * count").  Simulation state, at the start of the sim block's padding before
 * the wide area (port/simblock.h), so it is in every snapshot and hash.
 * Zero (power-on, and every build that never asks) means KATAM_KIRBYS. */
struct PortWideCtl {
    u8  target;         /* Kirbys wanted, 4..KATAM_KIRBYS; 0 = KATAM_KIRBYS  */
    u8  leaving;        /* the Kirby being taken away (+1), 0 = none         */
    u16 pad;
    u32 leavingSince;   /* the frame it was asked to come to player 1        */
    u32 added, removed; /* counts, for the log and the tests                 */
};

/* The human-slot layer (platform/localmp.c, port/localmp.h) at the wide
 * build's width: one entry per Kirby slot.  In the default build these live
 * in the console-sized structs of the sim block (lmp, frame.lmpPressed,
 * view.lmpDev*, whose sizes are asserted and must not move); the wide build
 * keeps them here, after wideCtl in the padding before the wide area, and
 * leaves those four-slot fields zero.  Simulation state: snapshotted, hashed
 * and carried by images like the rest of the block.  The masks are u32 so the
 * same layer serves 16 (bit s = slot s). */
struct PortWideLmp {
    u32 slots;          /* which Kirby slots are people (bit 0 = P1)          */
    u32 joining;        /* slots on their way to the group (joined away)      */
    u32 waiting;        /* joins held: the story has the Kirby, or the Kirby
                         * does not exist yet (the live count is raised)     */
    u32 devSet;         /* rooms mode: slots whose device is known            */
    u16 keys[KATAM_KIRBYS];      /* this frame's held keys, by slot (0 unused) */
    u16 pressed[KATAM_KIRBYS];   /* newly pressed this frame, by slot          */
    u16 groupRoom[KATAM_KIRBYS]; /* rooms mode: each DEVICE's group room + 1   */
    u8  stray[KATAM_KIRBYS];     /* frames each slot has been out of the frame */
    u8  dev[KATAM_KIRBYS];       /* rooms mode: each slot's device (devSet)    */
};

/* Player-card paint for Kirbys 4.. (platform/kirby_paint.c).  The default
 * build's paint table is four slots in the sim block's game struct
 * (game.paint, paintFeet, the paintDirty bits), whose layout must not move;
 * the wide build keeps every other slot's here, after lmpw in the padding
 * before the wide area: snapshotted, hashed and in every image.  Indexed by
 * slot (entries 0..3 unused: those are game.paint's).  `dirty` is a bit per
 * slot whose palette must be reloaded; while any is set, game.paintDirty
 * has PORT_PAINT_DIRTY_WIDE so the game's per-Kirby hook runs. */
struct PortWidePaint {
    u32 paint[KATAM_KIRBYS];     /* PORT_PAINT_* (port/kirby_paint.h)         */
    u16 feet[KATAM_KIRBYS];      /* a feet colour, 0 = with the body          */
    u32 dirty;                   /* bit s: slot s's palette to reload         */
};

#endif /* KATAM_WIDE */

/* The human-slot layer's width (port/localmp.h, platform/lmpnet.c): a slot
 * per Kirby slot.  4 in the default build. */
#define PORT_LMP_SLOTS KATAM_KIRBYS

/* --- addresses the platform side names directly (port/game_addrs.h etc.) -- */
#if KATAM_WIDE
#define PORT_ADDR_KIRBYS     WIDE_ADDR(WIDE_OFF_KIRBYS)
#define PORT_ADDR_LEVELINFO  WIDE_ADDR(WIDE_OFF_LEVELINFO)
#define PORT_ADDR_INPUT      WIDE_ADDR(WIDE_OFF_INPUT)
#define PORT_ADDR_BRAINS     WIDE_ADDR(WIDE_OFF_BRAINS)
#define PORT_INPUT_ROW       (2u * KATAM_KIRBYS)  /* bytes per unk8[] row     */
#else
#define PORT_ADDR_KIRBYS     0x02020EE0u
#define PORT_ADDR_LEVELINFO  0x02023530u
#define PORT_ADDR_INPUT      0x020382D0u
#define PORT_ADDR_BRAINS     0x02038590u
#define PORT_INPUT_ROW       8u
#endif

#ifdef __cplusplus
extern "C" {
#endif
#if KATAM_WIDE
/* The console's own read of a byte past the end of EWRAM (global.h): addr is
 * an address the game computed from a relocated table.  Reads what the console
 * would have read in its own layout, wherever that byte lives now. */
u8 PortWideMirrorU8(uintptr_t addr);
/* Relocation table for tools (tools/wide_unrelocate.py, --wide-map). */
struct PortWideReloc { const char *name; u32 orig, origSize, now, nowSize, stride, origStride; };
int PortWideRelocs(const struct PortWideReloc **out);
/* The collision lists: may an object be added to a list that holds `count`?
 * (31 at most, and the terminator.)  Counts the peak and the refusals. */
int PortWideCollRoom(u32 count);
/* TaskCreate found the 128-task pool full (game/src/task.c). */
void PortWideTaskFull(void);
/* TaskCreate's struct allocation failed (game/src/task.c): kind 0 = the EWRAM
 * heap refused a task that asked for EWRAM, 1 = the IWRAM heap refused one
 * (it falls back to EWRAM), 2 = that fallback failed too. */
void PortWideAllocFail(int kind);
void PortWideTaskCount(u32 n);
/* Telemetry for the gates (platform/wide.c): peak per-kind list length,
 * refused adds, peak tasks in use. */
struct PortWideStats { u32 collPeak, collDrops, taskFull, taskPeak, ewramFail, iwramFail, bothFail; };
const struct PortWideStats *PortWideGetStats(void);
/* Power-on: the port-owned table gUnk_03002E60 points at in levels. */
void PortWideInit(void);
/* RegisterRamReset: clear the moved tables whose console home it clears. */
void PortWideRamReset(u32 flags);
/* gKirbys[slot].base.roomId for a palette-effect slot, which the game reads
 * for its screen slots too (past the end of gKirbys on the console): those
 * read what the console read. */
u16 PortWideKirbyRoomAt(u32 slot);
/* The live count (platform/wide.c "The live count").  Simulation input:
 * PortWideSetTarget is applied at the latch of a frame-stamped `kirbys
 * FRAME N` input (platform/inputlog.c); the host asks with
 * PortWideRequestKirbys, which schedules one (0 when refused: replaying).
 * PortWideStartKirbys is how many Kirbys a play start creates (at least 4,
 * which the game's own start code assumes); PortWideFrame, once a frame
 * after the latch (platform/main.c), brings one Kirby in or takes one away
 * until gNumKirbys is the target. */
void PortWideSetTarget(int n);
int  PortWideTarget(void);
u8   PortWideStartKirbys(void);
void PortWideFrame(void);

/* The pause maps' Kirby markers (KATAM_MAP_MARKERS above): fills out[] with
 * the Kirbys that get one, lowest slot first, and returns how many. */
u8   PortWideMapMarkers(u8 *out);

/* Kirbys 4..'s sprite memory (platform/wide_sprites.c). */
static inline u32 PortWideOamTiles(u32 v)
{
    u32 d = v - WIDE_OBJX_TILES;

    if (d < WIDE_SZ_OBJX_TILES)
        return (d & 0x1FFFu) >> 5;      /* the same place in slot i & 3 */
    return (v - 0x6010000u) >> 5;
}
/* &gObjPalette[off]; off >= 0x100 is a line of the wide area. */
u16 *PortWideObjPal(u32 off);
/* gSourcePalettes.obj's twin: off >= 0x100 only (palette_effects.c). */
u16 *PortWideObjSrcPal(u32 off);
/* The whole-palette operations of the game, done to the wide area's lines
 * as the game does them to the console's sixteen: */
void PortWideObjPalRestore(void);          /* source -> buffer             */
void PortWideObjPalTransformAll(void);     /* gRgbMap over the buffer      */
void PortWideObjPalSync(void);             /* buffer -> "hardware"         */
/* VramFree of a wide-area address: nothing to free (the console never
 * frees a Kirby slot either). */
static inline int PortWideIsObjx(u32 addr)
{
    return addr - WIDE_OBJX_TILES < WIDE_SZ_OBJX_TILES;
}
#endif
/* Every build: more than four Kirbys are in the world, which only the GPU
 * renderer can draw (port/wide.h KATAM_KIRBY_TILES; platform/ui/present.c
 * draws with it while this holds, whatever the GRAPHICS setting).  Always 0
 * in the default build. */
int  PortWideNeedsGpu(void);
/* Every build: the host asks for n Kirbys in all (0 in the default build,
 * which has nothing to change). */
u32  PortWideRequestKirbys(int n);
/* The most Kirbys this build can have (4 without KATAM_MAX_KIRBYS). */
static inline int PortWideMax(void) { return KATAM_KIRBYS; }
#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_WIDE_H */
