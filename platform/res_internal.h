#ifndef GUARD_PORT_RES_INTERNAL_H
#define GUARD_PORT_RES_INTERNAL_H

/* Shared between platform/res.c (slots, Res_Get, session build) and
 * platform/res_pack.c (extractor, pack compiler, overlay install). */

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An fg room palette: 6 banks of 16 BGR555 colours (LoadLevelPaletteEffect-
 * SourcePalettes copies 0xC0 bytes of it into gSourcePalettes.bg). */
#define RES_FG_PALETTE_BYTES 0xC0u

void ResAddRelocation(u32 romLo, u32 size, void *dst);

/* res_pack.c: compile the pack (with mods overlaid, whole-file replace) into
 * the arena and retarget the slots.  0 on success. */
int  ResPackInstall(const char *packDir, const char *const *mods, u32 numMods);
int  ResExtract(const char *dir);
int  ResRoundTrip(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
