# Tilesets, backgrounds, palettes

Codecs: `tools/katam/lib/gfx.mjs`. Schemas: `tileset`, `background`, `palette` in
`assets/schema/authoring/`. All three round-trip byte-identically (34/34 tilesets, 36/36
backgrounds, 42/42 palettes; the LZ77 tile streams re-encode to the same bytes).

## Tile pixels: indexed PNG

Every tile sheet is an **indexed PNG, bit depth 4, 256 px (32 tiles) wide**, tile *n* at
column `n % 32`, row `n / 32` — the decomp/gbagfx convention. Pixel values 0-15 are the data
(4bpp GBA tiles; colour 0 is transparent in the playfield). The PNG's palette is only a preview:
a tileset is previewed with bank 0 of its matching palette, everything else in greys. Edit in any
paint program that keeps indexed mode (Aseprite, GIMP "Indexed", Krita with care); `katam
validate` rejects an RGB PNG or an index above 15 with the pixel position.

## Foreground tilesets (`tilesets/<ns>/<name>/tileset.json`)

`gForegroundInfo[i]`, chosen by a room's `tileset`. Decompressed into VRAM char block 2 at
room load (`LoadLevelGfx`, `game/src/level.c`).

```json
{"format": "katam-tileset/1", "id": "tileset:vanilla/29", "bind": {"index": 29},
 "image": "tiles.png", "tileCount": 768, "roomTag": 803, "previewPalette": "palette:vanilla/29",
 "animations": [{"tile": 736, "tiles": 4, "chunks": 1, "chunkStride": 32,
                 "frames": [{"gfxTile": 0, "duration": 8}, {"gfxTile": 4, "duration": 8}]}],
 "animImage": "anim.png", "animTileCount": 8}
```

(Illustrative values.) `animations` are the looping tile animations (`ForegroundInfo_8p`): every
`duration` frames, `tiles` tiles from `animImage` at `gfxTile` are copied over the tileset at
`tile`, `chunks` times `chunkStride` tiles apart (water surfaces, conveyor-looking edges).
`roomTag` is an authoring annotation left in the data (a room id) that the game never reads.

The playfield can address 768 tiles (char blocks 2-3, tile tokens 000-2ff).

## Backgrounds (`backgrounds/<ns>/<name>/background.json`)

`gRoomTiledBGs[i]`, the parallax layer behind the playfield (HW BG0), shared by every room that
names it. Its own tiles (char block 0), palette (written at `palette.offset`, colour 96 = bank 6,
8 banks) and an uncompressed tilemap of `size.width × size.height` 8-px tiles, one row per
line of `TTTfP` tokens. How it scrolls is the room's business (`props.bgScrollX/Y` = 8.8
parallax ratios, `-1` = derive from the background size; `bgOffsetY`; MODDING/room-tables).

## Foreground palettes (`palettes/<ns>/<name>.palette.json`)

`gForegroundPalettes[i]` (pack kind `fg_palette`): six banks of 16 `#RRGGBB` colours = BG
palette banks 0-5. `bit15` lists entries whose unused top bit is set (kept for exactness).
`animations` (palette cycling: frames of colour ranges with durations, docs/naming/room-tables.md
"ForegroundPalette") are not decoded into the file yet: `vanillaRef` keeps the original's. A new
palette has `animations` omitted (none).

This kind compiles to the pack the game already loads, so a palette edit is playable today:
`katam compile <mod> --base <vanilla> -o OUT && build/lp64/katam --res-pack OUT/pack rom.gba`.

## Not covered yet

Sprites (`gSpriteTables`: OBJ tiles, palettes, animation command streams), UI/HUD/title
graphics and the area map. MODDING.md §2.2 sketches their pack layout; the authoring side will
follow the same PNG + JSON pattern, with animations as symbolic command lists.
