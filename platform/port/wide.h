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
 * OBJ VRAM at 0x06010000 + (i << 11).  The console has four.  PLACEHOLDER in
 * the wide build: Kirby i uses slot i & 3, so Kirbys 4.. share the colours and
 * tile memory of Kirbys 0..3 (and do not overwrite the palettes past 15 or
 * the effect and enemy tiles past 0x06012000).  Real per-Kirby colours and
 * tiles are the renderer's job (ONE-WORLD-16.md, "presentation"). */
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

/* The console HUD's other-Kirby icons (hud.c: struct GameplayHud keeps
 * sprites for four), and the pause area map's Kirby markers (the same).
 * PLACEHOLDER in the wide build: they show Kirbys 0..3 only, and a screen whose own player is 4.. shows no calling-Kirby icon.  A
 * HUD for more players is its own work (the port's party HUD). */
#if KATAM_WIDE
#define KATAM_HUD_KIRBYS (gNumKirbys < 4 ? gNumKirbys : 4)
#else
#define KATAM_HUD_KIRBYS gNumKirbys
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
#define WIDE_OFF_LEVELINFO WIDE_AL(WIDE_OFF_PALFX    + WIDE_SZ_PALFX)
#define WIDE_BYTES         WIDE_AL(WIDE_OFF_LEVELINFO + KATAM_KIRBYS * WIDE_SZ_LEVELINFO)

/* The wide area's GBA address: PORT_SIM_BASE + offsetof(PortSimBlock, wide).
 * It is the last member of the block, after the rando lists (which the wide
 * build sizes for N seats: 0xB4 + 4130 bytes per seat), page-aligned; asserted
 * in simblock.h. */
#define WIDE_SIM_OFFSET    (((0xB4u + 4130u * KATAM_KIRBYS) + 0xFFFu) & ~0xFFFu)
#define WIDE_BASE          (0x02040000u + WIDE_SIM_OFFSET)
#define WIDE_ADDR(off)     (WIDE_BASE + (off))

/* Where gCurLevelInfo[0xFF] reaches, which must stay mapped and unwritten. */
#define WIDE_LI_REACH      (WIDE_ADDR(WIDE_OFF_LEVELINFO) + 256u * WIDE_SZ_LEVELINFO)

#endif /* KATAM_WIDE */

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
void PortWideTaskCount(u32 n);
/* Telemetry for the gates (platform/wide.c): peak per-kind list length,
 * refused adds, peak tasks in use. */
struct PortWideStats { u32 collPeak, collDrops, taskFull, taskPeak; };
const struct PortWideStats *PortWideGetStats(void);
/* Power-on: the port-owned table gUnk_03002E60 points at in levels. */
void PortWideInit(void);
/* RegisterRamReset: clear the moved tables whose console home it clears. */
void PortWideRamReset(u32 flags);
/* gKirbys[slot].base.roomId for a palette-effect slot, which the game reads
 * for its screen slots too (past the end of gKirbys on the console): those
 * read what the console read. */
u16 PortWideKirbyRoomAt(u32 slot);
#endif
#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_WIDE_H */
