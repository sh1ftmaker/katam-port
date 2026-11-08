
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"

static const u16 sCarrotCastle3Palette[] = {
    RGB( 0,  0,  0) | 0x0000, RGB(15,  9, 12) | 0x8000, RGB(17, 10, 12) | 0x0000, RGB(18, 10, 12) | 0x8000, RGB(20, 11, 12) | 0x0000, RGB(20, 11, 12) | 0x0000, RGB(21, 11, 12) | 0x8000, RGB(22, 11, 12) | 0x8000,
    RGB(22, 12, 12) | 0x0000, RGB(23, 12, 12) | 0x8000, RGB(23, 13, 12) | 0x0000, RGB(24, 14, 12) | 0x0000, RGB(24, 15, 13) | 0x0000, RGB(25, 15, 13) | 0x8000, RGB(26, 17, 13) | 0x8000, RGB(30, 23, 15) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB(17, 10, 11) | 0x0000, RGB(20, 11, 12) | 0x0000, RGB(21, 12, 12) | 0x8000, RGB(23, 13, 12) | 0x0000, RGB(24, 14, 12) | 0x0000, RGB(23, 15, 14) | 0x0000, RGB(25, 15, 12) | 0x8000,
    RGB(24, 16, 14) | 0x0000, RGB(26, 16, 14) | 0x8000, RGB(26, 18, 14) | 0x0000, RGB(27, 19, 14) | 0x8000, RGB(28, 21, 16) | 0x0000, RGB(30, 23, 15) | 0x8000, RGB(29, 24, 25) | 0x0000, RGB(31, 29, 30) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB(17, 10, 11) | 0x0000, RGB(17, 10, 12) | 0x0000, RGB(18, 10, 12) | 0x8000, RGB(20, 11, 12) | 0x0000, RGB(20, 11, 12) | 0x0000, RGB(22, 11, 12) | 0x8000, RGB(23, 11, 12) | 0x8000,
    RGB(23, 12, 12) | 0x8000, RGB(23, 13, 12) | 0x0000, RGB(24, 14, 12) | 0x0000, RGB(24, 14, 12) | 0x0000, RGB(24, 15, 13) | 0x0000, RGB(23, 15, 13) | 0x0000, RGB(24, 15, 13) | 0x8000, RGB(26, 18, 14) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB(15,  9, 12) | 0x0000, RGB(15,  9, 12) | 0x8000, RGB(16, 10, 12) | 0x0000, RGB(17, 10, 12) | 0x0000, RGB(17, 10, 12) | 0x0000, RGB(18, 10, 12) | 0x8000, RGB(18, 10, 12) | 0x8000,
    RGB(19, 10, 12) | 0x8000, RGB(19, 11, 12) | 0x0000, RGB(23, 10, 12) | 0x0000, RGB(21, 11, 12) | 0x8000, RGB(22, 12, 12) | 0x8000, RGB(24, 11, 12) | 0x8000, RGB(23, 13, 12) | 0x0000, RGB(24, 14, 12) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB(15,  9, 12) | 0x0000, RGB(15,  9, 12) | 0x8000, RGB(15,  9, 12) | 0x8000, RGB(15, 10, 12) | 0x0000, RGB(16, 10, 12) | 0x0000, RGB(17, 10, 11) | 0x0000, RGB(17, 10, 12) | 0x0000,
    RGB(18, 10, 12) | 0x8000, RGB(19, 11, 12) | 0x0000, RGB(20, 11, 12) | 0x0000, RGB(22, 12, 12) | 0x0000, RGB(23, 13, 12) | 0x8000, RGB(23, 15, 13) | 0x0000, RGB(26, 18, 14) | 0x0000, RGB(30, 23, 15) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB(15,  9, 12) | 0x8000, RGB(17, 10, 11) | 0x0000, RGB(17, 10, 12) | 0x0000, RGB(20, 11, 12) | 0x0000, RGB(21, 12, 12) | 0x0000, RGB(22, 12, 12) | 0x0000, RGB(23, 13, 12) | 0x0000,
    RGB(24, 14, 12) | 0x0000, RGB(24, 15, 13) | 0x0000, RGB(25, 15, 12) | 0x8000, RGB(25, 16, 13) | 0x0000, RGB(25, 17, 13) | 0x0000, RGB(26, 18, 14) | 0x0000, RGB(26, 18, 15) | 0x8000, RGB(28, 21, 16) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB(23, 15, 14) | 0x0000, RGB(24, 16, 14) | 0x0000, RGB(23, 16, 17) | 0x8000, RGB(25, 16, 14) | 0x8000, RGB(26, 18, 14) | 0x0000, RGB(26, 18, 18) | 0x8000, RGB(25, 19, 20) | 0x0000,
    RGB(27, 19, 14) | 0x8000, RGB(27, 20, 18) | 0x8000, RGB(27, 20, 20) | 0x8000, RGB(30, 19, 18) | 0x8000, RGB(28, 21, 20) | 0x8000, RGB(30, 22, 17) | 0x8000, RGB(29, 24, 25) | 0x0000, RGB(31, 29, 30) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB(15,  9, 12) | 0x8000, RGB(16, 10, 12) | 0x0000, RGB(17, 10, 11) | 0x0000, RGB(17, 10, 12) | 0x0000, RGB(19, 10, 12) | 0x8000, RGB(19, 11, 12) | 0x0000, RGB(21, 11, 12) | 0x8000,
    RGB(21, 12, 12) | 0x0000, RGB(22, 12, 12) | 0x8000, RGB(23, 13, 12) | 0x0000, RGB(23, 13, 12) | 0x8000, RGB(24, 14, 12) | 0x0000, RGB(23, 15, 13) | 0x0000, RGB(26, 18, 14) | 0x0000, RGB(30, 23, 15) | 0x8000
};

#define sCarrotCastle3Tileset (*(const u32 (*)[0x16ce])0x08AC74A0) /* INCBIN_U32("graphics/rooms/backgrounds/carrot_castle_3/tileset.4bpp.lz"), 23352 bytes, read from the ROM */
#define sCarrotCastle3Tilemap (*(const u16 (*)[0x384])0x08ACCFD8) /* INCBIN_U16("graphics/rooms/backgrounds/carrot_castle_3/tilemap.bin"), 1800 bytes, read from the ROM */

extern const struct RoomTiledBG gCarrotCastle3RoomTiledBG = {
    .width = 45,
    .height = 20,
    .unk4 = 0,
    .unk6 = 0,
    .unk7 = 0,
    .tileset = sCarrotCastle3Tileset,
    .tilesetSize = 900,
    .palette = sCarrotCastle3Palette,
    .paletteOffset = 0x60,
    .paletteSize = ARRAY_COUNT(sCarrotCastle3Palette),
    .tilemap = sCarrotCastle3Tilemap,
    .unk1C = 0x2bd0000
};

#ifdef __cplusplus
}
#endif
