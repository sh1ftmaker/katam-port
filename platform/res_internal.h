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

/* res.c: the extents in force (the room loader grows them); where the pack's
 * arena blobs end; levelSetHash over the pack's sim kinds and the rooms. */
void ResSetKindCount(int kind, u32 count);
void ResSetArenaTop(u32 addr);
int  ResFinishLevelSetHash(u8 out[32]);   /* res_pack.c; 1 if there is one */

/* res_rooms.c: the room-bundle loader (katam-native/1). */
void ResRoomsReset(void);
int  ResRoomsAddMemory(const void *data, u32 size, const char *what);
int  ResRoomsAddFile(const char *path);   /* 1 added, 0 absent, -1 error */
int  ResRoomsInstall(u32 arenaLo);
int  ResRoomsReload(const void *data, u32 size);
int  ResRoomsHash(u8 out[32]);           /* 1 if bundles are installed */
int  ResRoundTrip(const char *dir);

#ifdef __cplusplus
}
#endif

#endif
