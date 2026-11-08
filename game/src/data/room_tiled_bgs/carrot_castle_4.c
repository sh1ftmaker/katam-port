
#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"

static const u16 sCarrotCastle4Palette[] = {
    RGB(21,  3, 21) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 11, 11) | 0x8000, RGB( 1, 12, 12) | 0x8000, RGB( 2, 12, 15) | 0x8000, RGB( 2, 13, 14) | 0x8000, RGB( 2, 13, 15) | 0x8000, RGB( 2, 14, 14) | 0x0000,
    RGB( 3, 15, 15) | 0x0000, RGB( 4, 14, 14) | 0x8000, RGB( 4, 16, 16) | 0x0000, RGB( 5, 15, 15) | 0x8000, RGB( 8, 18, 16) | 0x8000, RGB(10, 20, 17) | 0x0000, RGB(13, 20, 18) | 0x8000, RGB(21, 24, 22) | 0x0000,
    RGB(21,  3, 21) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 12, 14) | 0x0000, RGB( 3,  8, 10) | 0x8000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 12, 15) | 0x8000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 13, 14) | 0x8000,
    RGB( 3, 11, 13) | 0x0000, RGB( 4, 12, 14) | 0x0000, RGB( 3, 13, 13) | 0x0000, RGB( 4, 14, 16) | 0x0000, RGB( 4, 14, 14) | 0x8000, RGB( 4, 16, 16) | 0x0000, RGB( 5, 18, 17) | 0x8000, RGB( 5, 21, 20) | 0x8000,
    RGB(21,  3, 21) | 0x0000, RGB( 2, 10, 11) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 12, 14) | 0x0000, RGB( 3,  8, 10) | 0x8000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 11, 13) | 0x8000, RGB( 2, 10, 12) | 0x8000,
    RGB( 2, 13, 14) | 0x8000, RGB( 2, 12, 14) | 0x8000, RGB( 4, 12, 14) | 0x0000, RGB( 3, 15, 15) | 0x0000, RGB( 4, 14, 14) | 0x8000, RGB( 4, 16, 16) | 0x0000, RGB( 5, 18, 17) | 0x8000, RGB( 5, 21, 20) | 0x8000,
    RGB(21,  3, 21) | 0x0000, RGB( 2, 10, 11) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 12, 14) | 0x0000, RGB( 1, 12, 12) | 0x8000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 11, 13) | 0x8000, RGB( 2, 12, 15) | 0x8000,
    RGB( 2, 10, 12) | 0x8000, RGB( 2, 13, 14) | 0x8000, RGB( 3, 10, 11) | 0x0000, RGB( 3, 11, 13) | 0x0000, RGB( 4, 12, 14) | 0x0000, RGB( 4, 12, 14) | 0x0000, RGB( 4, 14, 16) | 0x0000, RGB( 4, 16, 16) | 0x0000,
    RGB(21,  3, 21) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 12, 12) | 0x8000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 13, 14) | 0x8000, RGB( 3, 10, 11) | 0x0000, RGB( 3, 11, 13) | 0x0000, RGB( 2, 12, 14) | 0x8000,
    RGB( 2, 14, 14) | 0x0000, RGB( 4, 12, 14) | 0x0000, RGB( 3, 15, 15) | 0x0000, RGB( 4, 12, 14) | 0x8000, RGB( 4, 14, 14) | 0x8000, RGB( 6, 15, 14) | 0x0000, RGB( 4, 16, 15) | 0x8000, RGB( 5, 21, 20) | 0x8000,
    RGB(21,  3, 21) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 12, 14) | 0x0000, RGB( 1, 12, 12) | 0x8000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 11, 13) | 0x8000, RGB( 2, 12, 15) | 0x8000, RGB( 2, 13, 14) | 0x8000,
    RGB( 2, 13, 15) | 0x8000, RGB( 2, 14, 14) | 0x0000, RGB( 3, 15, 15) | 0x0000, RGB( 2, 16, 16) | 0x0000, RGB( 4, 14, 14) | 0x8000, RGB( 3, 15, 15) | 0x8000, RGB( 4, 16, 16) | 0x0000, RGB( 4, 16, 15) | 0x8000,
    RGB(21,  3, 21) | 0x0000, RGB( 2, 10, 11) | 0x0000, RGB( 2,  9, 11) | 0x0000, RGB( 1, 11, 13) | 0x0000, RGB( 1, 12, 14) | 0x0000, RGB( 2, 10, 12) | 0x8000, RGB( 2, 11, 13) | 0x8000, RGB( 2, 10, 12) | 0x8000,
    RGB( 2, 13, 14) | 0x8000, RGB( 3, 11, 13) | 0x0000, RGB( 4, 12, 14) | 0x0000, RGB( 4, 12, 14) | 0x0000, RGB( 4, 14, 16) | 0x0000, RGB( 4, 16, 16) | 0x0000, RGB( 5, 18, 17) | 0x8000, RGB( 5, 21, 20) | 0x8000,
    RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000,
    RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000, RGB(21,  3, 21) | 0x0000
};

#define sCarrotCastle4Tileset (*(const u32 (*)[0x11fc])0x08AD377C) /* INCBIN_U32("graphics/rooms/backgrounds/carrot_castle_4/tileset.4bpp.lz"), 18416 bytes, read from the ROM */
#define sCarrotCastle4Tilemap (*(const u16 (*)[0x384])0x08AD7F6C) /* INCBIN_U16("graphics/rooms/backgrounds/carrot_castle_4/tilemap.bin"), 1800 bytes, read from the ROM */

extern const struct RoomTiledBG gCarrotCastle4RoomTiledBG = {
    .width = 45,
    .height = 20,
    .unk4 = 0,
    .unk6 = 0,
    .unk7 = 0,
    .tileset = sCarrotCastle4Tileset,
    .tilesetSize = 900,
    .palette = sCarrotCastle4Palette,
    .paletteOffset = 0x60,
    .paletteSize = ARRAY_COUNT(sCarrotCastle4Palette),
    .tilemap = sCarrotCastle4Tilemap,
    .unk1C = 0x2da0000
};

#ifdef __cplusplus
}
#endif
