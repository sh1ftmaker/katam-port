
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"

static const u16 sUnusedBgPalette[] = {
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB(14, 22, 31) | 0x0000, RGB(17, 20, 17) | 0x0000, RGB(24, 26, 23) | 0x8000, RGB(31, 31, 31) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000
};

#define sUnusedBgTileset (*(const u32 (*)[0x3c1])0x08AC5D74) /* INCBIN_U32("graphics/rooms/backgrounds/unused_bg/tileset.4bpp.lz"), 3844 bytes, read from the ROM */
#define sUnusedBgTilemap (*(const u16 (*)[0x384])0x08AC6C78) /* INCBIN_U16("graphics/rooms/backgrounds/unused_bg/tilemap.bin"), 1800 bytes, read from the ROM */

extern const struct RoomTiledBG gUnusedBgRoomTiledBG = {
    .width = 45,
    .height = 20,
    .unk4 = 0,
    .unk6 = 0,
    .unk7 = 0,
    .tileset = sUnusedBgTileset,
    .tilesetSize = 900,
    .palette = sUnusedBgPalette,
    .paletteOffset = 0x60,
    .paletteSize = ARRAY_COUNT(sUnusedBgPalette),
    .tilemap = sUnusedBgTilemap,
    .unk1C = 0x2620000
};

#ifdef __cplusplus
}
#endif
