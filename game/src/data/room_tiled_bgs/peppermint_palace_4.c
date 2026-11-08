
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"

static const u16 sPeppermintPalace4Palette[] = {
    RGB( 0,  0,  0) | 0x0000, RGB(19, 24, 26) | 0x8000, RGB(21, 24, 26) | 0x8000, RGB(21, 26, 28) | 0x8000, RGB(17, 30, 31) | 0x8000, RGB(20, 30, 31) | 0x8000, RGB(25, 28, 29) | 0x8000, RGB(30, 26, 30) | 0x8000,
    RGB(22, 30, 31) | 0x8000, RGB(25, 30, 31) | 0x8000, RGB(28, 29, 30) | 0x8000, RGB(26, 30, 31) | 0x8000, RGB(30, 28, 31) | 0x8000, RGB(27, 30, 31) | 0x8000, RGB(30, 31, 31) | 0x8000, RGB(31, 31, 31) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB( 9, 15, 17) | 0x0000, RGB(12, 18, 20) | 0x8000, RGB(13, 19, 22) | 0x8000, RGB(14, 19, 23) | 0x8000, RGB(16, 20, 23) | 0x8000, RGB(17, 21, 24) | 0x8000, RGB(19, 22, 25) | 0x8000,
    RGB(19, 24, 26) | 0x8000, RGB(21, 24, 26) | 0x8000, RGB(21, 26, 28) | 0x8000, RGB(25, 28, 29) | 0x8000, RGB(28, 29, 30) | 0x8000, RGB(30, 28, 31) | 0x8000, RGB(30, 31, 31) | 0x8000, RGB(31, 31, 31) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB(14, 19, 23) | 0x8000, RGB(16, 20, 23) | 0x8000, RGB(16, 22, 23) | 0x8000, RGB(19, 22, 25) | 0x8000, RGB(19, 24, 26) | 0x8000, RGB(21, 24, 26) | 0x8000, RGB(21, 26, 28) | 0x8000,
    RGB(24, 28, 29) | 0x8000, RGB(25, 28, 29) | 0x8000, RGB(30, 26, 30) | 0x8000, RGB(27, 29, 30) | 0x8000, RGB(28, 29, 30) | 0x8000, RGB(27, 30, 31) | 0x8000, RGB(30, 31, 31) | 0x8000, RGB(31, 31, 31) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB( 9, 15, 17) | 0x0000, RGB(10, 16, 18) | 0x8000, RGB(11, 17, 19) | 0x8000, RGB(12, 18, 20) | 0x8000, RGB(13, 19, 22) | 0x8000, RGB(14, 19, 23) | 0x8000, RGB(16, 20, 23) | 0x8000,
    RGB(17, 21, 24) | 0x8000, RGB(16, 22, 23) | 0x8000, RGB(19, 22, 25) | 0x8000, RGB(21, 24, 26) | 0x8000, RGB(17, 30, 31) | 0x8000, RGB(20, 30, 31) | 0x8000, RGB(25, 30, 31) | 0x8000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB(14, 19, 23) | 0x8000, RGB(16, 20, 23) | 0x8000, RGB(17, 21, 24) | 0x8000, RGB(16, 22, 23) | 0x8000, RGB(19, 22, 25) | 0x8000, RGB(19, 24, 26) | 0x8000, RGB(18, 25, 27) | 0x8000,
    RGB(21, 24, 26) | 0x8000, RGB(21, 26, 28) | 0x8000, RGB(17, 30, 31) | 0x8000, RGB(20, 30, 31) | 0x8000, RGB(25, 28, 29) | 0x8000, RGB(25, 30, 31) | 0x8000, RGB(28, 29, 30) | 0x8000, RGB(31, 31, 31) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB( 9, 15, 17) | 0x0000, RGB(10, 16, 18) | 0x8000, RGB(12, 18, 20) | 0x8000, RGB(13, 19, 22) | 0x8000, RGB(14, 19, 23) | 0x8000, RGB(16, 20, 23) | 0x8000, RGB(16, 22, 23) | 0x8000,
    RGB(19, 22, 25) | 0x8000, RGB(19, 24, 26) | 0x8000, RGB(21, 26, 28) | 0x8000, RGB(25, 28, 29) | 0x8000, RGB(30, 26, 30) | 0x8000, RGB(28, 29, 30) | 0x8000, RGB(30, 28, 31) | 0x8000, RGB(31, 31, 31) | 0x8000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000,
    RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000, RGB( 0,  0,  0) | 0x0000
};

#define sPeppermintPalace4Tileset (*(const u32 (*)[0xe51])0x08A846B4) /* INCBIN_U32("graphics/rooms/backgrounds/peppermint_palace_4/tileset.4bpp.lz"), 14660 bytes, read from the ROM */
#define sPeppermintPalace4Tilemap (*(const u16 (*)[0x384])0x08A87FF8) /* INCBIN_U16("graphics/rooms/backgrounds/peppermint_palace_4/tilemap.bin"), 1800 bytes, read from the ROM */

extern const struct RoomTiledBG gPeppermintPalace4RoomTiledBG = {
    .width = 45,
    .height = 20,
    .unk4 = 0,
    .unk6 = 0,
    .unk7 = 0,
    .tileset = sPeppermintPalace4Tileset,
    .tilesetSize = 900,
    .palette = sPeppermintPalace4Palette,
    .paletteOffset = 0x60,
    .paletteSize = ARRAY_COUNT(sPeppermintPalace4Palette),
    .tilemap = sPeppermintPalace4Tilemap,
    .unk1C = 0xdc0000
};

#ifdef __cplusplus
}
#endif
