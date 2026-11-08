# Rooms (`katam-room/1`)

A room is one file, `rooms/<area>/<name>.room.json`, id `room:<area>/<name>`. Schema:
`assets/schema/authoring/room.schema.json` (every field described there; `katam describe room`).
Codec: `tools/katam/lib/room.mjs`. Runtime layouts: `tools/katam/lib/native.mjs`,
docs/ROOM-FORMAT.md, docs/naming/room-tables.md, docs/naming/doors.md.

## Coordinates and units

- **Cells** are 16×16 px, `(0, 0)` top-left, x right, y down. Grids, doors, patches and nav
  are in cells.
- **Objects** are in **pixels** (`x = cell * 16 + 8` is a cell's centre).
- `size` is in cells. One screen is 15×10 cells (240×160). The game keeps one collision byte
  per cell in a 1,950-byte buffer per room slot, so `width * height <= 1950` (vanilla's largest
  is 1,768). The playfield tilemap is `2*width` × `2*height` 8-px tiles.

## Example

`examples/authoring/sky_garden/rooms/sky_garden/meadow.room.json` (original content):

```json
{
  "format": "katam-room/1",
  "id": "room:sky_garden/meadow",
  "size": {"width": 30, "height": 12},
  "music": "mus_rainbow_route",
  "tileset": "tileset:vanilla/29", "palette": "palette:vanilla/29", "background": "background:vanilla/28",
  "art": {
    "legend": {".": "000.0 000.0 000.0 000.0", "#": "001.0 001.0 001.0 001.0",
               "-": "000.0 000.0 00a.0 00b.0", "=": "0ba.0 0bb.0 0da.0 0db.0"},
    "rows": ["##############################", "#............................#", "…"]
  },
  "collision": {
    "legend": {".": 0, "#": 13, "=": 15, "D": 255},
    "rows": ["##############################", "#............................#", "…",
             "#...........#####..........D.#", "##############################"]
  },
  "doors": [{"to": "room:sky_garden/tower", "arrive": {"x": 3, "y": 18}, "cells": [[27, 9]]}],
  "objects": [
    {"id": "door_to_tower", "type": "door", "x": 440, "y": 152, "raw": {"unk3": 1}},
    {"id": "dee", "type": "waddle_dee_1", "x": 136, "y": 152, "raw": {"unk3": 2}}
  ]
}
```

With the semantics registry loaded the collision legend can (and decompiled files do) use names:
`{".": "air", "#": "solid", "=": "platform", "D": "door"}`, and `raw.unk3` reads `despawnBit`.

## Sections

### Identity and references

| field | meaning |
|---|---|
| `id` | must match the path |
| `bind` | `{roomId, content}`: the numbers in the game. Vanilla files carry them; new rooms omit them and the compiler allocates (AUTHORING.md §4). |
| `music` | song name (`vocab.json` songs) or number → `RoomProps.songIdx` |
| `tileset` | → `RoomProps.pixelDataIdx` (`gForegroundInfo`: the playfield tiles + tile animations) |
| `palette` | → `RoomProps.paletteDataIdx` (`gForegroundPalettes`: banks 0-5). Equal to the tileset number in 258/286 vanilla rooms. |
| `background` | → `RoomProps.backgroundIdx` (`gRoomTiledBGs`, HW BG0, shared between rooms) |

### Properties: `props` and `raw`

The other `RoomProps` fields (camera offsets, parallax ratios, `priorityFlags`) by registry name
under `props` (`bgScrollX`, `bgOffsetY`, `roomFlags: {phoneCallAllowed: …}`) or by C name under
`raw`. Omitted fields take frozen defaults (`PROP_DEFAULTS`): `unkXmod_06/08 = 256` (1.0 in 8.8),
`unk0E/unk10 = -1` (derive parallax from the background size), `priorityFlags = 0x79`, others 0.
Structural fields (`mapDataIdx`, `solidityMapIdx`, `objectListIdx`, `unk22`, `doorsIdx`,
`objectList2Idx`, `unk26`) are never written: the compiler sets them.

### `art` — what you see

The playfield (HW BG3) at cell resolution: each legend value is a metatile, four 8×8 tile tokens
`"TL TR BL BR"`, each `TTTfP` (tile hex, flip `.hvx`, palette bank hex). Tile 0 / colour 0 is
transparent; the background shows through. Foreground tiles use palette banks 0-5 (the room
palette) or 15; tile indices 000-2ff (768 tiles in VRAM char blocks 2-3). Art does not affect
physics.

### `collision` — what is solid

One value 0-255 per cell, an index into `gCollisionAttributes` (C source,
`game/src/code_080023A4.c`), whose word encodes empty/solid/top-solid/water/ladder/hazard/
breakable/door bits and a slope enum (docs/naming/metatile-attributes.md). `katam describe
collision` lists every value with its bits and (with the registry) name and vanilla usage.
Common: 0 air, 13 solid, 15 one-way platform, 255 door, slopes 1-12, water 52-71.

### `patches` — cells that change

`{x, y, tiles[4], collision}`: when the game triggers the cell (breaking a block, a switch), it
writes these four tiles into the tilemap and this collision value into the map
(`Unk_0888562C`, tag 1). Several patches on one cell chain in file order. The record's lookup
key (the collision value the cell has at that moment) is derived: the map's value, or the
previous patch's result.

### `doors` — transitions

`{to, arrive: {x, y}, cells: [[x, y], …], flags}`: standing on any of `cells` and pressing up
takes Kirby to room `to`. **`arrive` is the cell Kirby lands on** — in vanilla the floor cell
right under the destination door — so the cell above it must be passable (the validator checks;
23 of 559 vanilla doors break the rule, entering through floors/ceilings). `flags`
(`Unk_0888562C_3.unk0C`): 0x80 keep the respawn point; 0x01/0x02 arrival facing.

The compiler writes each door twice, as the runtime needs: one tag-3 cell record per cell
(keyed by the cell's collision value, sorted with the patches by (y, x)), and one door entry in
the room's progress list keyed by `(to, arrive)`. Those two must agree or the game hangs
(docs/naming/doors.md); in the authoring format they cannot disagree. **Order matters**: the
progress list order is the per-room save-bit order (chests first, then doors), so append doors
rather than reorder them in a room players have saves for. 15 chests + doors max per room.

A door usually also needs a door collision value under its cells (vanilla uses 255, `door`) and
a `door` object (or `area_door`, `door_to_hub`, …) at the cell for its graphics and the
enter-door behaviour.

### `objects` — what is placed

`{id, type, x, y, params, raw, …}` in spawn order. `type` is an object type name
(`vocab.json`, `katam describe objects`). `params` are the type's registry-named parameters
(`object_params.json`: e.g. a Waddle Dee's `movePattern`, `speed`); top-level keys are generic
template fields with registry names (`despawnBit`, `variant`, …); `raw` holds any template
field by C name. Omitted fields default to 0 (`spawnTable` 1, `unkA/unkB/unkD` 255). `id` is
the editor's and mod patches' handle; the game does not see it.

Derived: the room's object-type set (`LevelInfo_1E4.unk4`), and the progress list's chest
entries, one per `small_chest` / `big_chest` object at its position (84/84 vanilla chests).

### `nav` — helper-Kirby navigation (optional)

`legend/rows` map each cell to a node, `nodes` give each node's position, arrival radius and a
32-entry next-hop row, `scripts` the four per-player AI bytecode programs
(`gUnk_08D63C28`, docs/naming/room-tables.md). Vanilla rooms carry theirs verbatim. Omitted, the
compiler emits one node at the room centre with halting scripts: helpers idle, nothing breaks
for the player. Generating real graphs is future work (AUTHORING.md §9).

## Limits the validator enforces

| limit | value | source |
|---|---|---|
| cells per room | `width*height <= 1950` | `gUnk_02024ED0[slot]` |
| min size | 15×10 cells | one screen |
| playfield tiles | 000-2ff | char blocks 2-3 |
| palette banks (fg) | 0-5, 15 | fg palette = banks 0-5, bg = 6-13 |
| chests + doors | 15 | `gVisitedDoors` u16 per room, bit 15 = visited |
| nav nodes | 32 | `LevelInfo_1E8_14.unk0[0x20]` |
| objects | no hard cap found; vanilla max 30 per room | |

## Derivation table (what the compiler writes that you do not)

| runtime field | from |
|---|---|
| `RoomProps.mapDataIdx/solidityMapIdx/objectListIdx/unk22/doorsIdx` | the room's content index |
| `RoomProps.objectList2Idx` = 0xFFFF, `unk26` = 0 | constant (no vanilla room has an extra layer) |
| `RoomTiledBG` (playfield) width/height, `0x1E` room tag | `size`, the room's number |
| cell record keys, order, strides, padding, terminator | collision grid, `(y, x)` sort, constants |
| object stride, terminator, type set, header counts | records |
| progress list (chests, door shadows, chest count) | chest objects, `doors` |
| LZ77 / RL streams | an encoder that reproduces vanilla's bytes exactly (928/928) |
