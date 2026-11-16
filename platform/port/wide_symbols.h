/* The wide build's homes for the per-player tables (port/wide.h).
 *
 * Force-included after port/ram_symbols.h, and only when KATAM_MAX_KIRBYS is
 * set (CMakeLists.txt, Makefile): the default build never sees this file.
 * Each macro replaces the generated one of the same name.  Tables that had no
 * bound in their generated spelling (a bare pointer) get one here, so that
 * -fsanitize=bounds checks every index into them (the W1 gate). */
#ifndef GUARD_PORT_WIDE_SYMBOLS_H
#define GUARD_PORT_WIDE_SYMBOLS_H

#include "port/wide.h"

#if KATAM_WIDE
#define WIDE_K  KATAM_KIRBYS
#define WIDE_RS KATAM_ROOM_SLOTS
#define WIDE_AT(off) ((uintptr_t)WIDE_ADDR(off))

#undef gKirbys
#define gKirbys (*(struct Kirby (*)[WIDE_K])WIDE_AT(WIDE_OFF_KIRBYS))
#undef gUnk_02021590
#define gUnk_02021590 (*(struct AnimInfo (*)[WIDE_K][185])WIDE_AT(WIDE_OFF_ANIM))
#undef gUnk_02022EB0
#define gUnk_02022EB0 (*(u8 (*)[WIDE_RS][2])WIDE_AT(WIDE_OFF_CEB0))
#undef gUnk_02022EC0
#define gUnk_02022EC0 (*(PTR32(struct Object) (*)[WIDE_RS][8])WIDE_AT(WIDE_OFF_CEC0))
#undef gUnk_02022F40
#define gUnk_02022F40 (*(u8 (*)[WIDE_RS])WIDE_AT(WIDE_OFF_CF40))
#undef gUnk_02022F50
#define gUnk_02022F50 (*(PTR32(struct ObjectBase) (*)[WIDE_RS * 64])WIDE_AT(WIDE_OFF_CF50))
#undef gUnk_02023358
#define gUnk_02023358 (*(u8 (*)[WIDE_RS][4])WIDE_AT(WIDE_OFF_RS358))
#undef gUnk_02023388
#define gUnk_02023388 (*(u32 (*)[WIDE_RS][16])WIDE_AT(WIDE_OFF_RS388))
#undef gUnk_02023508
#define gUnk_02023508 (*(u16 (*)[WIDE_RS])WIDE_AT(WIDE_OFF_RS508))
#undef gUnk_02023510
#define gUnk_02023510 (*(u16 (*)[WIDE_RS])WIDE_AT(WIDE_OFF_RS510))
#undef gUnk_02023518
#define gUnk_02023518 (*(u16 (*)[WIDE_RS])WIDE_AT(WIDE_OFF_RS518))
#undef gUnk_02023520
#define gUnk_02023520 (*(u16 (*)[WIDE_RS][2])WIDE_AT(WIDE_OFF_RS520))
#undef gCurLevelInfo
#define gCurLevelInfo (*(struct LevelInfo (*)[WIDE_K])WIDE_AT(WIDE_OFF_LEVELINFO))
#undef gUnk_02024ED0
#define gUnk_02024ED0 (*(u8 (*)[WIDE_RS][1950])WIDE_AT(WIDE_OFF_RSED0))
#undef gUnk_02026D50
#define gUnk_02026D50 (*(u16 (*)[WIDE_RS])WIDE_AT(WIDE_OFF_RSD50))
#undef gUnk_02026D60
#define gUnk_02026D60 (*(u8 (*)[WIDE_RS][1954])WIDE_AT(WIDE_OFF_RSD60))
#undef gUnk_02028EE0
#define gUnk_02028EE0 (*(union Unk_02028EE0 (*)[WIDE_RS])WIDE_AT(WIDE_OFF_RSEE0))
#undef gUnk_020382D0
#define gUnk_020382D0 (*(struct Unk_020382D0 *)WIDE_AT(WIDE_OFF_INPUT))
#undef gUnk_02038590
#define gUnk_02038590 (*(struct Unk_02038590 (*)[WIDE_K])WIDE_AT(WIDE_OFF_BRAINS))
#undef gPauseMenus
#define gPauseMenus (*(struct PauseMenu (*)[WIDE_K])WIDE_AT(WIDE_OFF_PAUSE))
#undef gUnk_03000510
#define gUnk_03000510 (*(struct Unk_03000510 *)WIDE_AT(WIDE_OFF_FREEZE))
#undef gPaletteEffectManager
#define gPaletteEffectManager (*(struct PaletteEffectManager *)WIDE_AT(WIDE_OFF_PALFX))
/* The table of &gCurLevelInfo[p].unk180[layer] that gUnk_03002E60 points at
 * in levels (level.c): the ROM's copy (0x082D8D74) holds the old addresses. */
#define gPortWideBgTab ((const union Unk_03002E60 *)WIDE_AT(WIDE_OFF_BGTAB))
#endif

#endif /* GUARD_PORT_WIDE_SYMBOLS_H */
