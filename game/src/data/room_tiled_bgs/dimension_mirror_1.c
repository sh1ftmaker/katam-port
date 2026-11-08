
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"

static const u16 sDimensionMirror1Palette[] = {
    RGB( 0, 31,  0) | 0x8000, RGB(31, 19, 27) | 0x8000, RGB(30, 14, 26) | 0x0000, RGB(28,  9, 24) | 0x8000, RGB(24,  7, 22) | 0x0000, RGB(20,  6, 19) | 0x0000, RGB(16,  4, 17) | 0x0000, RGB(10,  2, 13) | 0x0000,
    RGB( 3,  0,  9) | 0x8000, RGB( 4,  2, 10) | 0x0000, RGB( 4,  3, 12) | 0x8000, RGB( 6,  5, 15) | 0x0000, RGB( 5,  6, 15) | 0x8000, RGB( 6,  8, 17) | 0x0000, RGB( 6,  9, 18) | 0x0000, RGB( 7, 11, 19) | 0x8000,
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

#define sDimensionMirror1Tileset (*(const u32 (*)[0xeb6])0x08AED7FC) /* INCBIN_U32("graphics/rooms/backgrounds/dimension_mirror_1/tileset.4bpp.lz"), 15064 bytes, read from the ROM */
#define sDimensionMirror1Tilemap (*(const u16 (*)[0x384])0x08AF12D4) /* INCBIN_U16("graphics/rooms/backgrounds/dimension_mirror_1/tilemap.bin"), 1800 bytes, read from the ROM */

extern const struct RoomTiledBG gDimensionMirror1RoomTiledBG = {
    .width = 30,
    .height = 30,
    .unk4 = 0,
    .unk6 = 0,
    .unk7 = 0,
    .tileset = sDimensionMirror1Tileset,
    .tilesetSize = 900,
    .palette = sDimensionMirror1Palette,
    .paletteOffset = 0x60,
    .paletteSize = ARRAY_COUNT(sDimensionMirror1Palette),
    .tilemap = sDimensionMirror1Tilemap,
    .unk1C = 0x38d0000
};

#ifdef __cplusplus
}
#endif
