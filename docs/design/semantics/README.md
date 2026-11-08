# The semantics registry

`assets/schema/semantics/*.json` says what the bytes of the game's level and
object records **mean**: a name, a unit, a range, an enum, per-bit meanings,
and how sure we are, with every claim pinned to the game code that makes it.
It is the source the authoring layer (schemas, `validate`/`render`, the level
editor) takes field names and documentation from. It contains no ROM bytes:
only names, numbers that are *properties of the format* (offsets, masks,
enum values) and value statistics measured over the retail rooms.

The registry documents; it never renames. The C names in `game/` stay as they
are (`unk0E`, `priorityFlags`, ...) and are the join key: a registry field is
found by `(record, field)` where `record` is the manifest kind or the C struct
and `field` is the C member name.

This file is the format. The human notes (the reasoning behind a reading,
open questions) are the other files in this directory and the older, longer
proposals in `docs/naming/`.

## Files

One file per record kind. The `record` key is the manifest `kind` from
`assets/manifest.json` when the table is a manifest kind, otherwise a stable
snake_case name.

| file | record(s) | C type |
|---|---|---|
| `room_props.json` | `room_props` | `struct RoomProps` (gRoomProps) |
| `metatile_attributes.json` | `metatile_attr` + the 256-entry value enum | `gUnk_082D88B8[]` words |
| `solidity_map.json` | `solidity_map`, `cell_record_patch16`, `cell_record_patch8`, `cell_record_door` | `struct SolidityMap`, `Unk_0888562C{,_2,_3}` |
| `progress_list.json` | `progress_list`, `progress_chest`, `progress_door` | `Unk_08930E5C`, `Unk_08930E00_{1,2}` |
| `object_template.json` | `object_template` (generic header of a room object record) | `struct ObjectTemplate` |
| `object_params.json` | per-object-type meaning of the parameter fields | `struct ObjectTemplate` by `type` |
| `obj_desc.json` | `obj_desc` | `struct Unk_08351648` |
| `tiled_bg.json` | `room_tiled_bg`, `foreground_info`, `foreground_anim`, `foreground_palette`, ... | `struct RoomTiledBG`, `ForegroundInfo`, ... |
| `nav_graph.json` | `nav_graph`, `nav_node` | `struct LevelInfo_1E8{,_14}` |
| `object_lists.json` | `level_obj_list` | `struct LevelInfo_1E4` |
| `tuning_inventory.json` | not records: where enemy/ability/boss numbers live | — |

`metatile_attributes.json` additionally has a top-level `values` object:
collision index (decimal string) -> `{name, word, flags, slope, forceDir,
level, cells, rooms, usage?}` for each of the 125 non-zero table entries.

## Shape of a record file

```json
{
  "format": "katam-semantics/1",
  "record": "room_props",
  "c_type": "struct RoomProps",
  "c_decl": "game/include/data.h:444",
  "size": 40,
  "table": {"symbol": "gRoomProps", "addr": "0x089331AC", "count": 983, "index": "room id"},
  "description": "…",
  "notes": ["…"],
  "fields": [ FIELD, … ]
}
```

`table` is absent for records that are only ever reached through pointers.
A file may hold several records: then the top level is
`{"format": …, "records": [RECORD, …]}` with the same RECORD shape.

### FIELD

```json
{
  "field": "unk0E",                 // C member name (the join key), exactly as in game/include
  "offset": 14,                     // bytes from the start of the record
  "type": "s16",                    // u8 s8 u16 s16 u32 s32 ptr, or "u8[2]" etc.
  "name": "bgParallaxX",            // proposed semantic name, lowerCamel; null if unknown
  "description": "…",               // what the game does with it, one paragraph
  "unit": "fx8.8 ratio",            // see Units; null if not a quantity
  "range": [-1, 255],               // inclusive, what the ENGINE accepts sensibly; null if unconstrained
  "default": 256,                   // the neutral/identity value, if any
  "sentinel": {"-1": "derive automatically from the background size"},
  "enum": {"0": {"name": "…", "description": "…"}},   // values -> meaning (keys are decimal strings)
  "bits": [ BITS, … ],              // for bitfields, instead of / alongside enum
  "packed": [ SUBFIELD, … ],        // for words that pack several quantities (e.g. hi/lo byte)
  "usage": "live",                  // live | code_only | unused | padding  (see below)
  "confidence": "confirmed",        // confirmed | likely | guess | unknown
  "evidence": [ EVIDENCE, … ],
  "stats": {"summary": "…", "values": {"-1": 87, "27": 4}},   // measured over the 286 retail rooms
  "editor": {"widget": "…", "hidden": false}                  // optional hints for the editor
}
```

`BITS` is `{"mask": "0x0040", "name": …, "description": …, "confidence": …,
"evidence": […], "stats": …, "usage": …}` — the same keys as a FIELD minus
`offset`/`type`. A multi-bit sub-field uses `"mask": "0x00F00000"` plus
`"shift": 20` and may carry its own `enum`.

`SUBFIELD` is `{"name": …, "mask": "0xFF00", "shift": 8, "unit": …,
"description": …, "confidence": …, "evidence": […]}` for a quantity packed
inside the field.

Names are unique within a record (and within one object type's params),
enum names are unique within a field, and collision value names are unique
across `metatile_attributes.json` `values`, because the authoring layer
uses them as vocabulary in editable files. When a name changes, keep the old
one in `"aliases": ["oldName"]` (on the field or the enum value) so existing
authoring files stay valid. The authoring layer (branch `authoring`,
`tools/katam/lib/registry.mjs`) uses a semantic name only when `confidence`
is `confirmed` or `likely` and `usage` is not `padding`; everything else
stays `raw.<cField>`, and `raw.<cField>` is always accepted.

### usage

- `live` — read by the engine and populated with varied values by the retail data.
- `code_only` — read by the engine, but every retail record holds the same
  value (often the neutral one), so the reading rests on code alone. Editing
  it reaches code no shipped level exercises.
- `unused` — no reader found in `game/src` (and the value is usually constant).
- `padding` — structure padding.

### confidence

- `confirmed` — the code site leaves no other reading **and** an independent
  check agrees: the value distribution over the retail data, a second code
  site, or the running game.
- `likely` — the code says so plainly, but there is no independent check, or
  there is a small unexplained residue.
- `guess` — inferred from the data or a name, not from a reader.
- `unknown` — we have no reading; the entry exists so the field is listed.

### EVIDENCE

```json
{"loc": "game/src/level.c:147", "code": "priorityFlags & 1", "note": "set: BG3 prio 2 / BG2 prio 1"}
{"data": "unk0C is 80 in rooms 910, 912, 914, 916 and 0 elsewhere", "tool": "tools/room_export.py"}
{"runtime": "--warp … with unk0C patched to 40 moved the sky down 40 px", "how": "…"}
{"doc": "docs/naming/room-tables.md", "note": "full argument"}
```

A `loc` citation must quote `code`: a short verbatim substring of the cited
line (whitespace-insensitive). `tools/semantics_check.py` checks that it is
within ±3 lines of the cited line, which keeps the registry honest as
`game/src` changes: when lines move it reports the drift, and `--fix`
re-points each citation at the nearest occurrence of its `code`.

### Units

| unit | meaning |
|---|---|
| `px` | whole pixels |
| `tile8` | 8-pixel tiles (BG tilemap units) |
| `cell16` | 16-pixel cells (collision / object-placement units) |
| `fx8.8 ratio` | 8.8 fixed-point multiplier, 0x100 = 1.0 |
| `fx8.8 px/frame` | 8.8 fixed-point pixels per frame |
| `frames` | game frames (60 Hz) |
| `room id` | `gRoomProps` index, 101..982 for real rooms |
| `index:<table>` | index into a named table |
| `bytes`, `hwords`, `colors` | as named |

## Object parameters (`object_params.json`)

Room object records share one 36-byte layout (`object_template.json`) but
the fields from `subtype1` on are per-type parameters: their meaning depends
on `type`. `object_params.json` is

```json
{
  "format": "katam-semantics/1",
  "record": "object_params",
  "base": "object_template",
  "types": {
    "0x77": {
      "const": "OBJ_SCROLL_LOCK",
      "ctor": {"symbol": "CreateScrollLock", "loc": "game/src/scroll_lock.c:…", "code": "…"},
      "category": "camera",          // enemy | miniboss | boss | item | door | gadget | camera | trigger | effect | other
      "description": "…",
      "placements": 52,               // records in retail rooms
      "params": [ FIELD-like entries keyed by the template field name ("unk12", "subtype1", "unk22", …) ]
    }
  }
}
```

A param entry is a FIELD without `offset`/`type` (those come from the base
record). A field not listed for a type is either ignored by that type or
not yet decoded: `"undecoded": ["unk16", …]` lists the fields the type's
code reads that the entry does not yet explain.

## Tooling

```
python3 tools/semantics_check.py              # validate shape + every loc citation
python3 tools/semantics_check.py --fix        # re-point drifted citations
python3 tools/semantics_check.py --coverage   # per-record field counts by confidence
```
