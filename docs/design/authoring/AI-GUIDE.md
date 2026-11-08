# Making rooms: a guide for AI models (and people in a hurry)

You write JSON files; three commands tell you whether they are right and what they look like.
Everything below works from the repository root with node ≥ 18.

## 0. One-time setup (needs the player's ROM)

```sh
build/lp64/katam --res-extract /tmp/work/pack baserom.gba          # the game's own extractor
node tools/katam/katam.mjs decompile --rom baserom.gba --pack /tmp/work/pack -o /tmp/work/vanilla
```

`/tmp/work/vanilla` is every vanilla room, tileset, background, palette and object type as
authoring files. It is derived from the ROM: read it, learn from it, **never commit or share it**.
Without it you can still write and validate rooms; references to vanilla resources are then
reported as unchecked and renders show a collision schematic instead of art.

## 1. Learn the vocabulary (cheap, do it first)

```sh
node tools/katam/katam.mjs describe room                 # every room field, what it does
node tools/katam/katam.mjs describe collision            # collision values, names, terrain bits
node tools/katam/katam.mjs describe objects waddle       # object types + their parameters
node tools/katam/katam.mjs describe music                # song names
```

The schema `assets/schema/authoring/room.schema.json` says the same with exact types and ranges.

## 2. Start a project and a room

```
my_mod/project.json                       {"format": "katam-project/1", "id": "my_mod", "base": "vanilla"}
my_mod/rooms/my_mod/<name>.room.json      id "room:my_mod/<name>"  (the path follows the id)
```

```sh
node tools/katam/katam.mjs new-room room:my_mod/cave -o my_mod/rooms/my_mod/cave.room.json --width 40 --height 14
```

gives a valid room (floor, walls, one Waddle Dee) to edit. `examples/authoring/sky_garden/` is a
complete two-room example with doors both ways.

## 3. Rules that matter (the validator enforces them, this saves round trips)

**Grids.** `art`, `collision` (and `nav`) are `{"legend": {...}, "rows": [...]}`. One row string
per cell row, top to bottom; exactly `size.height` rows of exactly `size.width` keys. All legend
keys of a layer have the same length (1 character, or 2 for big legends). Use the same picture in
`art` and `collision` so what you see is what is solid.

**Size.** Cells are 16 px. At least 15×10 (one screen); `width * height <= 1950`.

**Collision values.** `0` air, `13` solid, `15` one-way platform (jump through from below),
`255` door cell; slopes, water, ladders, spikes, breakables: see `describe collision`. With the
semantics registry loaded you may write names (`"air"`, `"solid"`, `"platform"`, `"door"`).

**Art.** Each art legend value is four 8×8 tile tokens `"TL TR BL BR"`, token = `TTTfP` (tile
hex, flip `.`/`h`/`v`/`x`, palette bank hex): `"000.0 000.0 000.0 000.0"` is empty. Get real
metatiles for a tileset from a vanilla room that uses it: open its `art.legend` and pick values
(render it with `--overlay grid` to see which key is which). Banks 0-5 (and 15), tiles 000-2ff.

**Objects.** Positions in **pixels**: cell centre = `cell * 16 + 8`. A walker stands in the
empty cell right above a floor (`y = (floorRow - 1) * 16 + 8`). Type names from `describe
objects`. Give each object a distinct `despawnBit` (`raw.unk3`): it is the bit the game uses to
remember the object is gone.

**Doors.** `{"to": "room:…", "arrive": {"x": X, "y": Y}, "cells": [[cx, cy]]}`:
- `cells` are door cells in *this* room; give them collision `255` (door) and put a `door`
  object at the same cell centre so the door is drawn and enterable;
- `arrive` is the cell Kirby lands **on** in the destination: the floor cell right under the
  destination's door (so the cell above it must be empty);
- link both ways (the meadow → tower door has a tower → meadow partner), unless a one-way exit
  is intended;
- to leave into a vanilla room use its id (`room:other/803`, `room:rainbow_route/126`…). To make
  a vanilla room lead into yours, copy that vanilla room file into your project (same id, same
  path) and **append** a door: never reorder existing doors (save bits).

**Chests and doors**: at most 15 per room in total.

**Leave out** what you don't need: `props`, `raw`, `patches`, `nav` are optional (no `nav` means
helper Kirbys idle in the room).

## 4. Validate → fix → render → repeat

```sh
node tools/katam/katam.mjs validate my_mod --base /tmp/work/vanilla --json
```

Each finding: `{"severity", "code", "file", "where", "message", "hint"}`. `where` is a JSON
pointer into the file (`/doors/0/arrive`, `/collision/rows/7`), `hint` says what to change.
Fix every `error`; read every `warning` (one-way doors and `nav-default` are often fine).

```sh
node tools/katam/katam.mjs render room:my_mod/cave --project my_mod --base /tmp/work/vanilla \
    -o /tmp/work/cave.png --overlay collision,objects,doors,grid --scale 2
```

Look at the picture: grid numbers every 5 cells, collision tinted (grey solid, blue platform, cyan
water, red hazard, green door), objects boxed and labelled `o<i>` in list order, doors `d<i>`,
arrival markers `a`. Check: floors under every walker, the doors are reachable (Kirby can float,
but keep the main path to short jumps), nothing is sealed off.

## 5. Compile

```sh
node tools/katam/katam.mjs compile my_mod --base /tmp/work/vanilla -o /tmp/work/out
```

`out/pack` is what the game loads (`--res-pack`); `out/mod/rooms.katn` is the room bundle with
every room the project adds (ids 983 and up, listed under `inBundle`). Play it:

```sh
node tools/katam/katam.mjs playtest my_mod --base /tmp/work/vanilla --rom baserom.gba -o /tmp/work/pt \
    [--room room:my_area/my_room] [--frames 600] [--input script.txt]
```

It boots the game into the room, plays the frames (the script's `k FRAME HEXMASK` lines count
from arrival; Right=0x10, Left=0x20, Up=0x40, A=0x01), and prints JSON with the room and hp at
the end; look at the `shot-*-ppu.png` screenshots. Or use the web page `/next/playtest/`.
Keep floors at least 3 rows above the bottom of the room: the camera never shows the last
40 px (`camera-bottom` warning).

## Checklist before you hand a room back

- [ ] `validate` reports 0 errors.
- [ ] Every door has a partner door (or is meant to be one-way) and an `arrive` on a floor.
- [ ] Every door cell has collision 255 and a `door` object.
- [ ] The render shows what you intended; nothing important is outside the walls.
- [ ] `katam playtest` lands in the room and Kirby is alive at the end.
- [ ] `name` and `description` say what the room is for.
