# The authoring layer — design and status

Status: rooms, tilesets, backgrounds, palettes and object-type stats are implemented and gated
(byte-identical round trip of every vanilla resource of those kinds). Enemy behaviour, abilities
and bosses are designed here and in `docs/design/authoring/`, not implemented. Branch `authoring`.

The user's goal: *repackage the vanilla assets into a schema that is easy to edit or modify
(powers, enemies, tiles, levels, bosses), with every part AI-friendly for new content
generation, including the level editor.*

Per-kind documents:

| doc | covers |
|---|---|
| [authoring/rooms.md](authoring/rooms.md) | the room format, field by field: art, collision, patches, doors, objects, nav, properties |
| [authoring/graphics.md](authoring/graphics.md) | tilesets, backgrounds, palettes (PNG + JSON) |
| [authoring/enemies.md](authoring/enemies.md) | object types: what is data today, and the plan for behaviour |
| [authoring/abilities.md](authoring/abilities.md) | copy abilities ("powers"): design and migration path |
| [authoring/bosses.md](authoring/bosses.md) | bosses: data-driven parameters + Lua state machines |
| [authoring/AI-GUIDE.md](authoring/AI-GUIDE.md) | for AI models: generate a room, validate, render, iterate |

---

## 1. Two layers and one compiler

```
 player's ROM ──--res-extract──▶ vanilla pack (GBA-shaped JSON, per player, never committed)
      │                                   │
      └───────────── katam decompile ─────┴──▶ AUTHORING project (vanilla)      ◀── people, the editor, models
                                                     │  + mod projects (examples/authoring/…)
                                       katam validate │ katam render
                                                     ▼
                                              katam compile
                                                     │
                     ┌───────────────────────────────┴──────────────────────┐
                     ▼                                                      ▼
        OUT/pack   (room_props, fg_palette, obj_desc)          OUT/mod/rooms.katn  (katam-native/1)
        loaded by the game: --res-pack / --mod                 the room bundle: rooms (ids 983+),
        proven by --res-check and tests/regress.sh             tilesets, backgrounds, linked by the
                                                               runtime (--rooms, --mod, the web page;
                                                               platform/res_rooms.c, tests/playtest.sh)
                                                               OUT/native/: the same blobs unlinked
                                                               (one .bin each), for inspection
```

- **Authoring layer** — what humans, the web level editor and AI models edit. One small text file
  (or folder) per thing, stable string ids, no positional indexes, semantic field names with
  units, ranges and enums, a JSON Schema per kind with a description on every field.
- **Runtime layer** — what the game loads: GBA-native records (MODDING.md §2.5). Today the game
  can be served three kinds from a pack (`room_props`, `fg_palette`, `obj_desc`, MODDING.md M1);
  everything else still comes from the ROM tables.
- **The compiler is the only thing that knows GBA layouts.** `tools/katam/lib/native.mjs` holds
  every record layout, terminator and stride; `room.mjs`, `gfx.mjs`, `objects.mjs` map those to
  and from the authoring formats. Nothing in an authoring file is an address or a table index
  (the optional `bind` block is the one exception, see §4).

The compiler **derives** everything vanilla stores twice or that follows from other data, so an
author never writes it: the five per-room content indices, the playfield layer's size and room
tag, the order and keys of the collision cell records, the object-type set of a room, the
whole progress list (chests from chest objects, door shadows from doors — the pair that hangs
the game when it disagrees, docs/naming/doors.md), every list terminator and stride, and the
LZ77/RL streams. The gate (§6) proves every derivation reproduces vanilla exactly.

## 2. Where the compiler lives, and why JavaScript

`tools/katam/` is a zero-dependency ES-module library (`lib/`) plus a node CLI (`katam.mjs`).
Decision drivers:

1. **The level editor is web-first** (MODDING.md §5.4). It must read and write the same files
   and run the same validation and compilation. A JS library runs in the browser unchanged:
   `lib/` uses only `Uint8Array`, no Node APIs; file access goes through a 5-function `io`
   interface (`lib/io.mjs`: `nodeIO` for the CLI, `memoryIO` for an editor that loaded files
   from OPFS, a zip or fetch); zlib (PNG) and sha256 (pack.lock) are injected.
2. **A Python compiler would need a port** for the editor (no Python in the browser without
   Pyodide's 10 MB). Python stays for the existing analysis tools.
3. **C compiled to wasm** (MODDING.md §2.5 originally put the pack compiler in C) remains the
   runtime's job: the game binary compiles the *pack* into the arena at boot. Doing the
   authoring compiler in C would put JSON-schema validation, PNG and grid parsing into the game
   binary, change its code layout (and so the native `sim` hash, docs/DETERMINISM.md) for no
   gameplay reason, and make tooling iteration need a rebuild. The split is: **authoring ↔
   runtime records in JS; runtime records → arena in C** (the existing `platform/res_pack.c`).
4. Node ≥ 18 is already a project requirement (`tools/*.mjs`, the wasm tests).

The game binary is unchanged by this work: no C was touched.

## 3. Formats on disk, and why

| what | format | why |
|---|---|---|
| records (rooms, metadata) | **JSON**, canonical layout (`lib/json.mjs`): 2-space indent, LF, keys in schema order, short values on one line | JSON Schema validates it natively; every model and every browser reads/writes it with no library; the game-side pack is already JSON (`platform/res_json.c`). TOML was rejected: no schema language with editor support, nested arrays of tables are awkward for objects/doors, and a second syntax buys nothing. `mod.toml` stays TOML (it is MODDING.md's mod manifest, not content). |
| grids (art, collision, nav) | **row strings + legend** in the room JSON: `"rows": ["....##"]`, `"legend": {"#": 13}` | One line per row: a one-cell edit is a one-line diff. A model writes it exactly like ASCII art and can *see* the shape; a mistake (wrong row length, unknown key) is caught with row/column. Keys are 1 character, or 2 when a layer has more than 92 distinct values (big vanilla rooms' art); never escaped in JSON. RLE arrays were rejected: compact but neither readable nor locally editable by a model. CSV (MODDING.md §2.2's first idea) was rejected for the same reason plus a second file per layer. |
| 8×8 tilemap entries | 5-character token `TTTfP` (`"012.0"`, `"3a1h2"`) | tile index (3 hex), flip (`.` `h` `v` `x`), palette bank (hex): fixed width keeps columns aligned, and the fields are readable without bit arithmetic. A 16×16 art cell is four tokens `"TL TR BL BR"`. |
| tile pixels | **indexed PNG, bit depth 4**, 32 tiles per row | the decomp/gbagfx convention; any paint program edits it; the indices are the data, the PNG palette is only a preview, so the round trip is exact (34/34 tilesets, 36/36 backgrounds). |
| palettes | `"#RRGGBB"` strings, BGR555-exact (`c5<<3 \| c5>>2`, inverse `>>3`), plus the indices whose unused bit 15 is set | same convention as the M1 pack; humans and models think in hex colours. |

Sizes: the decompiled vanilla project is 617 resource files + project.json and 91 PNGs, 8.4 MB;
the largest room file (room 827) is 45 KB. Decompile takes 0.4 s, compile 0.5 s, validating all 617 files 0.5 s.

### Project layout and ids

```
<project>/project.json                               katam-project/1: id, base ("vanilla" for a mod)
<project>/rooms/<area>/<name>.room.json              room:<area>/<name>
<project>/tilesets/<ns>/<name>/tileset.json + tiles.png [+ anim.png]   tileset:<ns>/<name>
<project>/backgrounds/<ns>/<name>/background.json + tiles.png          background:<ns>/<name>
<project>/palettes/<ns>/<name>.palette.json          palette:<ns>/<name>
<project>/objects/<name>.object.json                 object:<name>
```

The path is a function of the id and back (`lib/project.mjs` `pathOf`/`idOfPath`), so any
reference can be opened without an index, and the validator rejects a file whose `id` does not
match its path. Ids are lowercase `kind:segment/segment`.

**Vanilla ids are frozen by rule** (format version 1): `room:<area>/<roomId>` with the area from
the game's own area-map table (`sAreaMapRoomInfos`, `game/src/pause_area_map.c`; rooms not on
the map are `other`), `tileset|palette|background:vanilla/<table index>`, `object:<OBJ_ name
lowercased>`. `room:vanilla/<roomId>` always resolves as an alias. A mod refers to vanilla rooms
by these ids and they never change.

A **project stack** is a base (the player's decompiled vanilla project) plus mod projects; later
layers add resources or replace them whole by id (MODDING.md §1.3 "file replace"). `--base DIR`
on every command builds the stack.

## 4. Bindings: string ids to the numbers the game uses

Numbers exist only at compile time (MODDING.md §3.4). A resource may carry `bind` (vanilla files
do, so they compile back to the same slots); otherwise the compiler allocates, deterministically
by sorted id:

- **room ids**: **after the original game's ids**: 983 and up (gRoomProps has 983 entries,
  0-982, of which 286 are real rooms). The vanilla filler slots 1-100 are never used, so a new
  room cannot collide with an id the game or a save gives a meaning to;
- **content index** (the one number a room's tilemap, collision, objects, progress and nav share,
  docs/ROOM-FORMAT.md §1): 287 and up;
- tilesets/backgrounds/palettes: after the vanilla tables (34, 36, 42).

`katam compile` reports the allocation and writes the **room bundle**, `OUT/mod/rooms.katn`
(plus `OUT/pack/rooms.katn` when it writes a full pack): every room, tileset and background the
stack defines above the decompiled vanilla project, in `katam-native/1` (§8). The runtime grows
its tables to the ids the bundle names (`platform/res_rooms.c`), so rooms ≥ 983 play natively
(`--rooms OUT/mod/rooms.katn`, or `--mod OUT/mod`) and on the web playtest page.
Without `--base`, references into vanilla resolve by the number their id carries (unchecked):
enough for the bundle, not for the pack.

A new room also gets, in the bundle, the two room-id tables beside `gRoomProps` that the
loader grows: `gUnk_08D6CD0C` (the AI helpers' room graph and the room's **area**: the room's
optional `area` field, else the area of the vanilla room its first door leads to, else
rainbow_route; no neighbours, flag 0x80 "not a routing target") and `gUnk_0893CB44` (a default
cell: its first door's cell).

## 5. Schemas and names: written for models

- `assets/schema/authoring/*.schema.json` — draft 2020-12, one per kind (`room`, `tileset`,
  `background`, `palette`, `object`, `project`) plus `common` ($defs) and `vocab.json` (enums from
  committed headers: object types, songs, abilities, areas, room→area, the 256 collision attribute
  words). Generated by `katam vocab` from the headers and the semantics registry, committed;
  `katam vocab --check` (in `make authoring-check`) fails when they are stale.
- **Every field has a `description`** written for a model: what the game does with it, the unit
  (16-px cells, pixels, frames, 8.8 ratio), the range, the default, an example where the format
  is not obvious. The schema is the prompt and the editor's form definition at once.
- **Enums by name**: object types (`"waddle_dee_1"`), music (`"mus_rainbow_route"`), abilities
  (`"fire"`), collision values (registry names: `"air"`, `"solid"`, `"platform"`, `"door"`,
  `"floor_1_2_riseR_low"`…). Numbers are always accepted too.
- **Defaults are frozen** per format version (`PROP_DEFAULTS`, `TEMPLATE_DEFAULTS` in
  `lib/room.mjs`): an omitted field means the same value forever, whatever a registry later says.
  Decompiled files omit defaults, so a vanilla room shows only what is special about it.
- `katam describe room|collision|objects|music [filter]` prints the same knowledge as compact
  text, for a model's context or a person's terminal.

### Consuming the semantics registry

The `semantics` branch owns `assets/schema/semantics/*.json` (format `katam-semantics/1`, spec in
its `docs/design/semantics/README.md`). The authoring layer reads it through
`lib/registry.mjs` with this contract:

1. a field is found by `(record, C field name)`; its semantic `name` is used in authoring files
   **only when the registry is sure** (`confidence` confirmed or likely, `usage` not padding);
2. **`raw.<cField>` is always accepted** on input, whatever the registry says — so improving the
   registry never invalidates a file, and a field without a name is still editable;
3. bitfields with named single bits (e.g. `RoomProps.priorityFlags` → `roomFlags`) are written as
   objects of booleans (`{"phoneCallAllowed": true, …}`); an integer is accepted too;
4. collision legend values use the metatile record's `values` names; per-object-type parameters
   (`object_params.json`) go under `params` with their names; generic template fields
   (`object_template.json`) become top-level object keys (`despawnBit`, `variant`, …);
5. an optional `aliases: [...]` on a field or enum value keeps old names valid after a rename
   (requested of the registry; nothing renames today). Without aliases, a renamed field in a
   hand-written file is reported by `validate` with a "did you mean" hint.

Measured with the registry's 11 files (semantics `f66990a`, merged into main): the round
trip is still 286/286 rooms identical, and a decompiled room reads
`"props": {"bgScrollX": 32, "roomFlags": {...}}`, collision legends `"air"`, `"solid"`,
`"platform"`, objects `{"type": "cpu_kirby_trigger", "despawnBit": 1, "params": {"rectX": -24,
…}}`, and `object:waddle_dee_1` has `"stats": {"score": 200, "hp": 2, …}`. Without a registry
every one of those is `raw.<cField>` and every tool still works. The registry is merged (main
da2165d) and the committed schemas are generated from it; whenever the registry changes, run
`node tools/katam/katam.mjs vocab` and commit (`vocab --check` in `make authoring-check`
enforces it).

## 6. Gates

| gate | command | result (2026-11-08, ROM sha1 274b102b…) |
|---|---|---|
| ROM-free | `make authoring-check` | library self-test (compression, grids, tokens, colours, PNG, scaffold round trip), schemas current, the example mod validates |
| round trip | `make authoring-roundtrip ROM=…` (`tests/authoring_roundtrip.sh`) | decompile → compile → compare, blob by blob, with the ROM: **286/286 rooms, 34/34 tilesets, 36/36 backgrounds, 42/42 palettes, 219/219 object types identical**; **928/928 LZ77/RL streams re-encode byte-identically** (the encoders reproduce Nintendo's, so even compressed bytes match); the compiled pack is **file-for-file identical to `--res-extract`** (1027 files) and **`--res-check` passes** (53,312 bytes) |
| game unchanged | same script | `tests/regress.sh --mode sim`, all six recordings green with the game served from the **compiled** vanilla pack, and again from the pack **with the example rooms added** |

What "blob by blob" covers per room: RoomProps (40 B), playfield header, tilemap (decompressed
and compressed), collision map (both), every cell record (patches and door cells, keys, order,
padding), the 8-byte terminator, every object template, its terminator, the object-type set,
the progress list (chests + door shadows) and its header, the nav header, node map (both), nodes
and four scripts. Tilesets: header, pixels (both), every animation record and frame table, the
animation graphics. Backgrounds: header, pixels (both), palette, tilemap.

Not covered yet (no authoring kind): sprites/animations, text/UI graphics, audio, the area map,
palette-cycling animations (kept as an opaque reference, `animations.vanillaRef`).

## 7. The CLI

```
katam vocab [--check]                              schemas + vocab from headers and the registry
katam decompile --rom R --pack P -o DIR            vanilla authoring project (ROM-derived: never commit)
katam compile PROJECT [--base DIR]... -o OUT       OUT/pack + OUT/native, allocation report
katam roundtrip --rom R --pack P --work DIR [--bin build/lp64/katam]
katam validate PATH [--base DIR]... [--json]       PATH = project folder or one file
katam render ROOM -o out.png [--project DIR] [--base DIR]... [--overlay collision,objects,doors,nav,grid] [--scale N]
katam new-room ID -o FILE [--width W --height H]   a valid starting room
katam describe room|collision|objects|music [filter]
katam reach ROOM...      designed, exit 3 with the plan (§8)
katam playtest PROJECT --rom R -o OUT   boot the native game into a room, screenshots + hash log (§8)
```

All commands take `--registry DIR` (default `assets/schema/semantics`).

**validate** findings are `{severity, code, file, where, message, hint}`; `where` is a JSON
pointer into the file (`/doors/0/arrive`), `code` is stable (`schema`, `id-path`, `ref`,
`ref-unchecked`, `grid`, `door-terrain`, `door-object`, `arrive-bounds`, `arrive-blocked`, `one-way`, `object-bounds`, `object-id`,
`limit-cells`, `limit-tile`, `limit-bank`, `tile-range`, `nav-node`, `nav-default`,
`unreachable`, `compile`, …), `hint` says how to fix it. It runs, in order: JSON syntax, the
schema (with "did you mean" on enum misses), id ↔ path, references (doors, tileset, palette,
background; into the base when `--base` is given, else reported as unchecked), door arrivals
(inside the destination, not into a wall — `arrive` is the cell Kirby lands *on*), back-links
(one-way doors are warnings: vanilla has 159), objects inside the room, the GBA limits the
compiler enforces (1,950 collision cells, 768 playfield tiles, foreground palette banks, 15
progress bits, 32 nav nodes), and finally the compiler itself on the room. Vanilla rooms (those
with `bind`) get warnings where a new room gets errors, since the game ships them.

**render** draws the background (tiled, bottom-aligned), the art, and overlays; without the
tileset/palette loaded it draws a collision schematic so the output still means something. It
reads the player's extracted art; output goes wherever `-o` says (scratch), never into the repo.

## 8. Designed, not implemented

### reach (`katam reach`)

Per room: from each arrival, which doors can Kirby get to. A graph over standable cells (empty
with a floor below: solid, top-solid or slope), with edges for walking along floors and slopes,
falling, jumping inside an envelope derived from Kirby's jump impulse/gravity/air speed (the
`kirby.physics.*` entries of the semantics tuning inventory, checked against recorded jumps on
the native build), climbing ladders, swimming, and floating (Kirby flies indefinitely, so with
float on — the default — any empty cell connected through air is reachable; `--no-float` models
abilities and rooms that forbid it). Breakable blocks need an ability (`--abilities`); hazards are
passable but reported. Across rooms: doors are edges, checked both ways — the world graph that
docs/naming/doors.md started (559 door edges), plus warp stars and other object-borne exits from
the registry's object params. Output: reachable / unreachable doors with the blocking cells,
`--json`.

### playtest (`katam playtest`) — implemented

```
katam playtest PROJECT --rom ROM -o OUT [--base VANILLA]... [--room ID|N] [--door N | --at X:Y]
               [--frames N] [--input SCRIPT] [--shots F,F,...] [--pack VANILLA_PACK] [--bin build/lp64/katam]
```

Compiles the stack (`OUT/build`), writes a recording (`OUT/playtest.txt`,
`tools/katam/lib/playtest.mjs`): the regression boot (A+Right taps from frame 120, the
recordings' own — `--warp` alone never leaves the title), a warp into the room at frame 1300
(arrival: `--at`, else the N-th door leading into the room, else under its own first door),
then the input script from arrival (`k FRAME HEXMASK` / `hold MASK FROM TO`, frames counted
from arrival, which is warp + 90). Runs the native binary headless with the bundle (`--rooms`,
or `--res-pack P --mod OUT/build/mod`), fresh XDG dirs, `setarch -R`, dummy SDL drivers; writes
`OUT/hash.log`, `OUT/game.log`, PPU screenshots `OUT/shot-<frame>-ppu.png` (default: arrival,
middle, end) and prints JSON: warp delivered, the room and hp at exit, the bundle's sha256 and
`levelSetHash`. Exit 0 when the warp landed and Kirby is alive. The room is the first one the
project adds unless `--room`.

The web page `/next/playtest/` (`web/playtest/`, `make dist-playtest`) does the same in the
browser against the player's own ROM; see docs/design/authoring/rooms.md "Playtesting".

**Arrival coordinates**: the game places Kirby's centre at `arrive.x*16 - 8`,
`arrive.y*16 - 7` px — feet on top of row `arrive.y`, horizontally on the left edge of column
`arrive.x`. Vanilla arrivals sit beside the destination door (door at `(x, y-1)` or
`(x-2, y-1)` in 280 of 326 cases), so Kirby does not stand *on* it; to arrive on a one-cell door
at column `c`, use `x = c + 1`. **Camera**: the view never shows the bottom 40 px of a room, so
floors need ≥ 3 rows under them (vanilla 3-4); the validator warns (`camera-bottom`).

### Art from collision ("brushes")

Hand-placing 8×8 tiles is the least AI-friendly part of a room. The next format step: a tileset
file gains named **brushes** (a metatile, or an autotile rule set: top edge, fill, left/right
edge, corners, keyed by the collision neighbourhood), and a room's art legend may say
`"#": "brush:cloud"`; `art` becomes optional and is generated from `collision` when absent. The
compiler expands brushes, so the runtime format does not change. The sky_garden example already
follows the pattern by hand (fill, rim-in-the-air-cell-above-floor, platform).

## 9. Open questions

1. **Room id space for new rooms** — settled: 983 and up (the runtime grows its tables; see
   MODDING.md "R2: room bundles" for what else is indexed by room id and how it was handled).
2. **Nav graphs for new rooms.** A room without `nav` gets a one-node graph and halting
   scripts: helper (CPU) Kirbys idle there. Generating a real one (regions from standable areas,
   next-hop matrix by BFS — the unread `extra` bytes look like the adjacency list the matrix was
   built from) belongs with `reach`.
3. **Door/object coupling.** A door needs a door-typed collision value under its cells and,
   for graphics, usually a `door` object at the same cell; the validator does not yet check the
   pairing. The registry's object params will say which object types are doors.
4. **`despawnBit` (`ObjectTemplate.unk3`)** should probably be unique per room; vanilla mostly
   numbers objects in order. Not validated until the registry's reading is confirmed at run
   time.
5. **Palette animations** are kept as an opaque reference to the original's ROM data. Decoding
   them into the palette file (frames of colour ranges with durations) is straightforward from
   docs/naming/room-tables.md and the next step for palettes.
