# Naming proposals: level / room runtime state

**Status: review document. Nothing has been renamed. Every claim below carries a
`file:line` citation into `/home/agent-tom/Desktop/katam` (read-only).**

## Summary

The EWRAM block from `0x02022EB0` to `0x02038290` is a bank of **four parallel
room slots**, not four player slots. A slot is allocated in `FillLevelInfo`
(`wip/code.c:231`, slot logic at `:405-450`): if another player is already in the
room being entered, the entering player *adopts that player's slot and skips every
decompression and every reset* (`wip/code.c:411-414`); otherwise the lowest slot
index not claimed by any other player who is in a room is taken
(`wip/code.c:416-429`) and the room is materialised into it. The slot index lives
in `gCurLevelInfo[playerId].unk65E`, and **every array in this document is
subscripted by that value, never by `playerId`**. This is now settled at runtime,
not merely inferred: the port was booted into a level and the live heap read, with
`gCurLevelInfo[0].unk65E == 0` selecting the correct grid at `gUnk_02024ED0 + 0`,
and all 988 bytes of room 803's grid matching a reconstruction decompressed
independently from the ROM. The static picture agrees: `linker.ld:19-53` hard-places
every one of these symbols, and each array's four-slot size fits its address
window exactly (worked through per entry below).

The collision grid's `1950` is a **hard cap and a real ceiling**:
`(roomWidth/16) * (roomHeight/16) <= 1950`, enforced by nothing, and it is a
fixed-size array with four instances at a fixed address — not a heap allocation
that could be grown. Measured over all 286 real rooms, the largest grid in the
retail game is **1768 bytes**, the widest room is 136 cells (2176 px) and the
tallest 103 cells (1648 px). That leaves **182 bytes of headroom across the
entire game** (9.3% of the budget). A custom room may go up to 1950 cells and not
one further.

Unresolved and honestly unknown: `LevelInfo::unk65F` has **zero** references in
the entire tree; `gUnk_0203AD10 & 1` and `gUnk_0203AD20 & 1` are **read but never
set anywhere in `src/`, `wip/`, or `asm/`**; the exact per-bit meaning of
`gUnk_0203AD10` bits 2 and 4 is inferable only from the modes that assign them
wholesale; and `gUnk_02023508` vs `gUnk_02023510` are distinguished only by which
call sites bump them (defeats vs. pickups), which is suggestive but not proof of
the designer's intent. Details in each **Against / unresolved** section.

### A note on the address evidence

`linker.ld:19-84` assigns every EWRAM symbol an explicit absolute address
(`. = 0x00024ED0; gUnk_02024ED0 = .;`). This means the distance to the next symbol
is an **upper bound** on an array's size, not a measurement of it. Where the
computed four-slot size *equals* that bound exactly (`gUnk_02022EC0`,
`gUnk_02022F50`, `gUnk_02023358`, `gUnk_02023388`, `gUnk_02023508/10/18/20`,
`gCurLevelInfo`, `gUnk_02028EE0`) the confirmation is strong — the array cannot be
larger and nothing else fits. Where the bound exceeds the computed size
(`gUnk_02024ED0`, `gUnk_02026D60`, `gUnk_02026D50`, `gUnk_02022EB0`,
`gUnk_02022F40`) the linker slack is 8-12 bytes and the declared size stands on
the C declaration plus the fill sizes, not on the address. I say which applies in
each entry.

The port additionally asserts `struct LevelInfo`'s size and every field offset at
compile time in `platform/port/gba_layout.h:758-818`; those assertions are cited
below where they independently pin a field.

---

## Part 1 — the room-slot arrays

### `gUnk_02024ED0`
**Proposed:** `gRoomSolidityCells`
**Type as used:** `u8[4][1950]`, indexed
`[roomSlot][cellY * (roomWidth / 16) + cellX]`, **row-major, one byte per 16x16 px
cell**
**Confidence:** high — confirmed at runtime

**Evidence:**
- **Runtime**: the port was booted into a level and the live heap read.
  `gCurLevelInfo[0].unk65E == 0` and the grid at `gUnk_02024ED0 + 0 * 1950` was the
  correct one for the room; all 988 bytes of room 803's grid matched a
  reconstruction decompressed independently from the ROM. Room-slot indexing, the
  1950 stride, the one-byte-per-cell element and the `roomWidth/16` row stride are
  all measured, not inferred.
- `linker.ld:47` — `. = 0x00024ED0; gUnk_02024ED0 = .;` (absolute placement).
- `include/data.h:698` — declared `extern u8 gUnk_02024ED0[][1950];`
- `wip/code.c:431` — filled at room load by
  `RLUnCompWram(gSolidityMaps[gRoomProps[room].solidityMapIdx]->unk0, gUnk_02024ED0[levelInfo->unk65E])`.
  The source table is already named `gSolidityMaps` (`include/data.h:911`), which
  is where the proposed name comes from.
- `src/code_080023A4.c:1273-1279` (`sub_080023E4`) — read as
  `gUnk_02024ED0[var0->unk65E][var0->roomWidth / 16 * y + x]`. This is the
  indexing formula, stated literally.
- `src/code_080023A4.c:1288-1311` (`sub_08002470`) — same read, with `x` clamped
  to `[0, roomWidth/16 - 1]` and `y` to `[0, roomHeight/16 - 1]`. This is the
  clearest statement that the grid is exactly `roomWidth/16` by `roomHeight/16`.
- `src/code_080023A4.c:1338-1345` (`sub_0800255C`) — the only writer outside room
  load: writes one cell and returns the old value (block destruction).
- `src/code_080023A4.c:1352`, `:1371` — read by `sub_080025AC` / `sub_08002624`,
  which use the cell value as a *key* into `levelInfo->unk1E0` (the room's
  metatile-pattern list, sourced from `gSolidityMaps[...]->unk4`,
  `wip/code.c:308`). So a cell holds a metatile id, not a raw solidity bitmask.
- `wip/code.c:51` — cleared once at level start with `CpuFill32(0, gUnk_02024ED0, 1950 * 4)`,
  i.e. 7800 bytes = 4 x 1950. Confirms four slots at the declared stride.
- Address window: `linker.ld:47-48` puts `gUnk_02024ED0` at `0x02024ED0` and the
  next symbol `gUnk_02026D50` at `0x02026D50`, a span of `0x1E80` = 7808 bytes.
  4 x 1950 = 7800 fits with 8 bytes of linker slack. This is an **upper bound**,
  not a measurement — but note that `gUnk_02026D60` shows the *same* 8-byte slack
  before `gUnk_02028BF0`, and `gUnk_02026D50` the same 8 before `gUnk_02026D60`,
  so 8 bytes of slack is this region's house style and the declared 1950 is the
  right reading. The runtime measurement settles it either way.

**On `1950` — the level-editor ceiling:**
`(roomWidth / 16) * (roomHeight / 16) <= 1950` is a hard cap. This is a
**fixed-size array with four instances at a fixed address** (`linker.ld:47`), not
a heap allocation: it cannot be grown without moving every EWRAM symbol after it.
The corroboration that 1950 counts *cells* is `gUnk_02028EE0`, sized `1950 * 8`
(`include/data.h:706`): a 16x16 cell is 2x2 8x8 tiles, each a `u16`, so 8 bytes
per cell. `1950 * 8 = 15600` bytes per slot, and `sub_08001408`
(`src/code_080023A4.c:604-613`) indexes that buffer as
`(cellY * 2) * (roomWidth / 8) + cellX * 2` — the same grid at 2x resolution.
The two buffers are the same 1950 cells at two granularities.

Measured over all **286 real rooms** (`gRoomProps` at `0x089331AC`,
`data/data_8.s:44013`, 983 slots of which ids 101..982 non-contiguously are real
and the rest `0xFFFF` filler; grid dimensions from
`gForegroundTilemaps[mapDataIdx]`, `data/data_2.s:867`, whose first two `u16` are
width and height in 8x8 tiles, `include/data.h:71-73`):

| quantity | value |
|----------|-------|
| largest grid | **1768 bytes** (room id 224, 136 x 13 cells) |
| widest room | **136 cells** = 2176 px |
| tallest room | **103 cells** = 1648 px |
| headroom to the cap | **182 bytes**, 9.3% of the budget |

Every room's width and height in tiles is even, so `roomWidth/16` and
`roomHeight/16` are exact. The widest and tallest rooms are different rooms; no
room approaches the cap in both axes at once. Exceeding 1950 corrupts whatever
follows: slot 3 runs into `gUnk_02026D50` (the room state flags), slots 0-2 into
the next slot's grid.

**Against / unresolved:**
- Nothing bounds-checks the decompressed size. `RLUnCompWram` writes whatever the
  ROM blob says; a hand-authored room larger than 1950 cells overruns silently.
  The clamp in `sub_08002470` clamps the *lookup*, not the *fill*.
- The 8 bytes between the array's end (`0x02026D48`) and `gUnk_02026D50` are
  unaccounted for. They are almost certainly linker slack or an unnamed variable,
  but a stride of 1952 would also fit the address window. The two readings differ
  only in the base of slots 1-3 (by 2, 4 and 6 bytes), so **the runtime check
  should be repeated at `unk65E >= 1`** if anyone wants this closed beyond doubt;
  the slot-0 read cannot distinguish them.
- "Solidity" is the ROM table's word (`gSolidityMaps`, `include/data.h:911`), not
  necessarily the field's. The cell value is a metatile id consumed by the
  `unk1E0` pattern walk, and its solidity semantics are decided downstream — a
  name like `gRoomMetatileCells` is equally defensible and arguably more literal.

---

### `gUnk_02026D60`
**Proposed:** `gRoomNavNodeCells`
**Type as used:** `u8[4][1954]`, indexed
`[roomSlot][cellY * (roomWidth / 16) + cellX]`
**Confidence:** high

**Evidence:**
- `linker.ld:49` — `. = 0x00026D60; gUnk_02026D60 = .;`
- `include/data.h:700` — `extern u8 gUnk_02026D60[][1954];`
- `wip/code.c:432` — filled at room load from
  `gUnk_08D63C28[gRoomProps[room].unk22]->unk10`, i.e. field `unk10` of
  `struct LevelInfo_1E8` (`include/data.h:148-155`), RL-compressed.
- **`unk22` is not an independent index.** Measured over all 286 real rooms,
  `mapDataIdx == solidityMapIdx == objectListIdx == unk22 == doorsIdx` in 286 of
  286 cases, with values spanning 0..286. So one "room content index" selects the
  tilemap, the solidity grid, the object list, this navigation graph and the door
  list together. The nav graph is therefore 1:1 with the collision grid it
  overlays, which is what makes the shared `roomWidth/16 * y + x` indexing sound.
  (Verified independently against `baserom.gba` using the field offsets in
  `struct RoomProps`, `include/data.h:484-506`.)
- `src/code_080023A4.c:1387-1394` (`sub_080026A8`) — the **only** reader, with the
  identical `roomWidth / 16 * y + x` indexing as the solidity grid.
- `src/code_0800ECAC.c:76-89` (`sub_0800EEBC`) — wraps it: clamps an object's
  pixel position to cell coordinates (`x >> 12`, clamped against `roomWidth >> 4`)
  and returns the cell byte.
- `src/code_0800ECAC_2.c:2297-2303` — the returned byte is assigned to
  `a1->unk1A` and immediately used as the **first** subscript of the room's
  navigation graph: `unk3F = ...->unk14[a1->unk1A].unk0[a1->unk1B]`, i.e.
  `nextHop = adjacency[currentNode][targetNode]`.
- `src/code_0800ECAC_2.c:2313-2318`, `:2735-2740` — same pattern, guarded by
  `if (a1->unk1A == 0xFF)` ("no current node yet").
- `src/code_0800ECAC_2.c:450-474` (`sub_08010034`) — picks a target node by
  scanning `r4->unk14[i].unk20 / .unk22` (node world x/y,
  `struct LevelInfo_1E8_14`, `include/data.h:143-148`) against the camera bounds,
  bounded by `r4->unk18` (node count).
- Together: `gUnk_02026D60` maps every 16x16 cell of the room to the id of the
  navigation node containing it. The consumer is `gUnk_02038590[]`, the AI-Kirby /
  helper controller (`src/code_0800ECAC_2.c` throughout).
- `wip/code.c:52` — `CpuFill32(0, gUnk_02026D60, 1954 * 4)` = 7816 bytes = 4 x 1954.
- Address window: `linker.ld:49-50`, `0x02026D60` to `gUnk_02028BF0` at
  `0x02028BF0` is `0x1E90` = 7824. 4 x 1954 = 7816 fits with the same 8 bytes of
  slack as `gUnk_02024ED0`. Upper bound, not a measurement.

**Against / unresolved:**
- Why 1954 and not 1950? Both grids use the same `roomWidth/16 * y + x` formula
  over the same room, and — now that `unk22` is known to equal `solidityMapIdx`
  for every real room — over the same room *content*, so 1950 would suffice. The
  extra 4 bytes are unexplained by anything I can read; decompression slop for
  `RLUnCompWram`'s 4-byte write granularity is the obvious guess but I have no
  citation for it.
- This grid has **not** been checked at runtime. Unlike `gUnk_02024ED0` its
  stride and layout rest on the C declaration plus the fill size alone. Given the
  1954-vs-1950 oddity it is the better candidate of the two for a heap read.
- `struct LevelInfo_1E8` field names (`unk0`, `unk10`, `unk14`, `unk18`) are
  themselves unnamed, so "navigation graph" rests on the *use* at
  `code_0800ECAC_2.c:2303` rather than on any declared type.
- Node id `0xFF` is treated as "none" at `code_0800ECAC_2.c:2312`, so cell values
  presumably range `0..nodeCount-1` plus `0xFF`. I did not verify the ROM blobs
  actually stay in that range.

---

### `gUnk_02028EE0`
**Proposed:** `gRoomForegroundTilemap`
**Type as used:** `u16[4][7800]` (declared `u8[][1950 * 8]`), indexed
`[roomSlot][tileY * (roomWidth / 8) + tileX]`
**Confidence:** high

**Evidence:**
- `include/data.h:706` — `extern u8 gUnk_02028EE0[][1950 * 8];`
- `wip/code.c:433-434` — at room load,
  `LZ77UnCompWram(levelInfo->unk180[0].tilemap, gUnk_02028EE0[levelInfo->unk65E])`
  and then `levelInfo->unk180[0].tilemap` is **repointed at the EWRAM buffer**.
  So `unk180[0]` (a `struct RoomTiledBG`, `include/data.h:71-84`) starts pointing
  at ROM and ends pointing at this decompressed working copy.
- `wip/code.c:413` — a player adopting an existing slot repoints its
  `unk180[0].tilemap` at the same buffer without decompressing. Two players in one
  room share one tilemap.
- `src/code_080023A4.c:599-613` (`sub_08001408`) — writes a 2x2 block of `u16`
  tilemap entries at `(y*2) * (roomWidth >> 3) + x*2`, i.e. the four 8x8 tiles of
  one 16x16 cell, and in the same breath writes the matching
  `gUnk_02024ED0` cell (`:616-621`). This is the metatile-swap primitive used by
  breakable blocks and switch-doors (`src/lever_wall.c:28`).
- `src/code_080023A4.c:670-687` (`sub_080015A8`) — reads the same 2x2 block back,
  branching on `gForegroundTilemaps[...]->unk1C` to pick `u16` vs `u8` entries.
- `wip/code.c:50` — `CpuFill32(0, gUnk_02028EE0, 1950 * 8 * 4)` = 62400 bytes.
- Addresses: `linker.ld:53` puts it at `0x02028EE0` and `linker.ld:54`
  `gUnk_020382A0` at `0x020382A0` — `0xF3C0` = 62400 = **4 x 15600 = 4 x 1950 x 8**,
  exactly filling the window with no slack. Strong confirmation.

**Against / unresolved:**
- The `u8` vs `u16` branch at `sub_080015A8` (`src/code_080023A4.c:670` vs `:679`)
  means the element type is not fixed by the buffer — `gForegroundTilemaps[i]->unk1C`
  selects it per room. Naming the type `u16[]` is therefore slightly wrong for
  8-bpp rooms. `u8[4][15600]` with a comment is the honest declaration.
- `1950 * 8` in the declaration is a coincidence of arithmetic, not an intent
  signal: the buffer is `4 * (roomWidth/8) * (roomHeight/8)` bytes at maximum,
  which happens to equal `8 * maxCells`. Do not read `8` as a per-cell field
  count in some struct.

---

### `gUnk_02023358`
**Proposed:** `gRoomSpawnFlags`
**Type as used:** `u8[4][4]` — 32 bits per room slot, bit `n` = `[n >> 3] & (1 << (n & 7))`
**Confidence:** high

**Evidence:**
- `include/data.h:690` — `extern u8 gUnk_02023358[][4];`
- `wip/code.c:436` — `CpuFill32(0, gUnk_02023358[levelInfo->unk65E], 4)` — all 32
  bits cleared when a room is freshly loaded into a slot (and, critically, *not*
  cleared when a second player joins an already-loaded room, `wip/code.c:411`).
- `src/code_080023A4.c:688-701` (`sub_08001678`) — setter. `arg0` selects a
  storage tier, `arg1` is the bit index, `arg2` is the room slot, `arg3` the
  value. Tier 0 is this array; tier 1 is `gUnk_02023368` and tier 2
  `gUnk_02028BF0`, both un-slotted globals.
- `src/code_080023A4.c:1426-1440` (`sub_080027E8`) — matching getter, same tiers.
- `src/object.c:375` — on object destruction,
  `sub_08001678(obj->object->unk2, obj->object->unk3, gCurLevelInfo[obj->base.unk56].unk65E, sb)`.
  So `Object::unk2` is the *tier* and `Object::unk3` is the *flag index*, both
  authored per object in the ROM object list.
- `src/code_080023A4.c:5894-5895` and `:5949-5952` — in `CreateLevelObjects` and
  its streaming sibling, `if (sub_080027E8(obj->unk2, obj->unk3, levelInfo->unk65E) != 0) continue;`
  — a set flag suppresses respawning that object. This is the decision the array
  exists for.
- Other setters: `src/object_spawner.c:143`, `src/star_platform.c:99`,
  `src/lava_wall.c:94`, `src/scroll_lock.c:641`, `src/star_stone_block.c:387-389`,
  `src/destroyable_rock_block.c:41`, `src/object.c:2092`, `:2222`,
  `wip/cutscene_trigger.c:928`, `wip/large_star_stone_block.c:242` et al.
- Addresses: `linker.ld:38-39`, `0x02023358` to `gUnk_02023368` at `0x02023368`
  is `0x10` = **4 x 4 bytes**, exactly filling the window.

**Against / unresolved:**
- 32 flags is a per-room budget shared by every object in the room, so flag index
  collisions between objects are prevented only by the ROM data. A level editor
  must allocate `Object::unk3` carefully; nothing at runtime detects a clash.
- `src/star_stone_block.c:389` calls `sub_08001678(..., 0)` — clearing a flag.
  So this is not strictly write-once. I did not enumerate every clear site.
- The tier-1 (`gUnk_02023368`) and tier-2 (`gUnk_02028BF0`) arrays are *not* slot
  indexed (`sub_08001678` ignores `arg2` for them, `src/code_080023A4.c:697-706`).
  A reader who names this one `gRoomSpawnFlags` should name those two something
  clearly wider-scoped or the tier distinction is lost.

---

### `gUnk_02023388`
**Proposed:** `gRoomVars`
**Type as used:** `u32[4][16]`, indexed `[roomSlot][varIndex]`, `varIndex` 0-15
**Confidence:** medium-high

**Evidence:**
- `include/data.h:685` — `extern u32 gUnk_02023388[][16];`
- `wip/code.c:437` — `CpuFill32(0, gUnk_02023388[levelInfo->unk65E], 0x40)` — 64
  bytes = 16 `u32`, cleared on fresh room load.
- `src/code_080023A4.c:1445-1457` (`sub_08002888`) — returns
  `gUnk_02023388[subindex] + index` for tier `SUB_08002888_ENUM_UNK_1`; the caller
  dereferences the returned `u32 *`. Tiers 2 and 3 return the un-slotted
  `gUnk_02023488` and `gUnk_02028C10`.
- Read/write sites all pass `gCurLevelInfo[obj->base.unk56].unk65E` as `subindex`
  and an authored `Object` field as `index`:
  `src/jack.c:55` (`= 1`), `:61`, `:91`, `:241`, `:268`, `:329` (`= 0`) — index
  from `object->unk14` / `object->unk4`;
  `src/big_small_switch.c:302`, `:305`, `:313`, `:376` (index `0xF`);
  `src/kirby_cannon.c:86`, `:154`;
  `src/lava_wall.c:58`; `src/code_0800ECAC_2.c:2220` (index `0xF`).
- Addresses: `linker.ld:40-41`, `0x02023388` to `gUnk_02023488` at `0x02023488`
  is `0x100` = **4 x 16 x 4 bytes**, exactly filling the window.

**Against / unresolved:**
- These really are general-purpose per-room words with no fixed meaning; each
  object type picks an index and defines the contents itself (`jack.c` uses it as
  a boolean, `big_small_switch.c:313` casts the `u32 *` to `u8 *`). A more
  specific name would be a lie. `gRoomVars` may be *too* vague to be an
  improvement over the address.
- Index `0xF` is used by at least three unrelated systems
  (`big_small_switch.c:376`, `code_0800ECAC_2.c:2220`) — so either it is a
  reserved slot with a shared meaning I did not chase down, or those systems
  genuinely never coexist in one room. I could not settle this statically.

---

### `gUnk_02023508`
**Proposed:** `gRoomDefeatedCount`
**Type as used:** `u16[4]`, indexed `[roomSlot]`
**Confidence:** medium

**Evidence:**
- `include/data.h:693` — `extern u16 gUnk_02023508[];`
- `wip/code.c:439` — zeroed on fresh room load.
- `src/code_080023A4.c:1536-1543` — accessors `sub_080029BC` (get) and
  `sub_080029CC` (add).
- `src/object.c:392-397` — in `ObjectDestroy`, for objects passing
  `ObjType0To37 || ObjType38To52 || ObjType53To64`, if `obj->object->unk22 & 1`
  and `obj->unk80 <= 0`, `sub_080029CC(gCurLevelInfo[obj->base.unk56].unk65E, 1)`.
  `unk80 <= 0` is the "was defeated, not merely despawned" condition also used at
  `src/object.c:361-372`.
- `src/lever_wall.c:14-18` — a lever wall with `subtype1 == 0` opens when
  `sub_080029BC(slot) == arg0->object->unkF`, i.e. "break the wall after N
  defeats in this room".
- `src/object_spawner.c:59` and the reference asm it mirrors
  (`asm/nonmatching/sub_0811BAAC.inc`) — a spawner reads this counter into
  `unkDC` and gates spawning on `unkDC >= object->unk18 >> 8`.
- `src/object.c:336` — `if (sub_080029BC(...) > 1)` inside a `gUnk_0203AD10 & 4`
  (sub-game) branch for rooms `0x3BB..0x3BD`.
- Addresses: `linker.ld:42-43`, `0x02023508` to `gUnk_02023510` at `0x02023510`
  is `8` = **4 x u16**, exact.

**Against / unresolved:**
- "Defeated" is inferred from `unk80 <= 0` plus the object-type ranges, not
  stated. `Object::unk22 & 1` is an authored opt-in bit whose name is unknown, so
  the counter is really "objects that opted in and died with `unk80 <= 0`". If
  `unk22 & 1` is set on non-enemies anywhere in the ROM data, the name overreaches.
  I did not audit the ROM object lists for that.
- `sub_080029CC` *adds* `arg1` rather than incrementing, so a non-1 delta
  somewhere would break the "count" reading. Every call site I found passes 1
  (`src/object.c:396`).
- There is no saturation and no clamp; it is a `u16` reset only at fresh room load.

---

### `gUnk_02023510`
**Proposed:** `gRoomCollectedCount`
**Type as used:** `u16[4]`, indexed `[roomSlot]`
**Confidence:** medium

**Evidence:**
- `include/data.h:694`; zeroed at `wip/code.c:440`.
- `src/code_080023A4.c:1546-1553` — accessors `sub_080029E4` (get),
  `sub_080029F4` (add).
- Increment sites are all *pickups*, never defeats:
  `src/bonus.c:166-168` — on collecting an HP/battery/1UP/candy bonus, gated on
  `object->unk2 != 0 || object->unk3 != 31` (a real authored object, not a
  dynamically created one);
  `src/chest.c:342`, `:345`, `:349` — on resolving a chest's contents;
  `src/chest.c:359` — in `sub_0800BD4C` when `HasChest(...)` is already true.
- `src/lever_wall.c:19-23` — the `subtype1 == 1` lever wall opens when this
  counter equals `object->unkF`; the `subtype1 == 0` variant uses
  `gUnk_02023508`. The two counters are deliberate alternatives.
- `src/object_spawner.c:57` — a spawner with `object->unk22 & 2` gates on this
  counter instead of `gUnk_02023508`.
- Addresses: `linker.ld:43-44`, `0x02023510` to `gUnk_02023518` at `0x02023518`
  is `8` = **4 x u16**, exact.

**Against / unresolved:**
- Same `+= arg1` caveat as above.
- `src/chest.c:359` bumps it in `sub_0800BD4C` on a chest that is *already*
  collected (per `HasChest`), i.e. at room-entry re-sync rather than at the moment
  of collection. So the counter is "collectibles resolved this room visit",
  which includes already-owned ones. `gRoomCollectedCount` slightly oversells
  that; `gRoomPickupCount` may be safer.
- The `object->unk22 & 1` / `& 2` split (defeats vs pickups) is my reading of two
  independent call sites, not of a named constant.

---

### `gUnk_02023518`
**Proposed:** `gRoomSongId`
**Type as used:** `u16[4]`, indexed `[roomSlot]`
**Confidence:** high

**Evidence:**
- `include/data.h:695`.
- `wip/code.c:446` — `gUnk_02023518[levelInfo->unk65E] = gRoomProps[room].songIdx;`
  on fresh room load. `songIdx` is already a named field (`include/data.h:485`).
- `src/code_080023A4.c:1556-1563` — accessors `sub_08002A0C` (get) /
  `sub_08002A1C` (set).
- `src/code_080023A4.c:578-586` (`sub_08001358`) — `levelInfo->unk65C = gUnk_02023518[levelInfo->unk65E];`
  then, for the local player only, `m4aSongNumStartOrChange(levelInfo->unk65C)`.
- `include/object.h:164-168` (`Macro_080E7D74`) and `include/object.h:184-190`
  (`Macro_08100F18`) — push/replace the current song then start it.
- `src/code_0806F780.c:9363`, `:9385` — read as the song to (re)start after a
  boss/miniboss sequence.

**Against / unresolved:**
- None material. The one thing worth flagging: because it is per *room slot*, two
  players in the same room share one BGM state (by construction,
  `wip/code.c:411-414`), while two players in different rooms genuinely have two
  independent BGM states even though only one is audible. That is why the sound
  code checks `playerId == gUnk_0203AD3C` before touching the mixer
  (`src/code_080023A4.c:571`).

---

### `gUnk_02023520`
**Proposed:** `gRoomSongStack`
**Type as used:** `u16[4][2]`, indexed `[roomSlot][depth]`, `0xFFFF` = empty
**Confidence:** high

**Evidence:**
- `include/data.h:696` — `extern u16 gUnk_02023520[][2];`
- `wip/code.c:448-449` — `for (j = 0; j < 2; j++) gUnk_02023520[slot][j] |= 0xFFFF;`
  on fresh room load. (`|= 0xFFFF` on a `u16` is an unconditional set to `0xFFFF`.)
- `src/code_080023A4.c:1566-1573` — accessors `sub_08002A2C(slot, depth)` (get)
  and `sub_08002A44(slot, value, depth)` (set).
- **Push**, `include/object.h:184-188` (`Macro_08100F18`):
  `for (_i = 1; _i < 2; ++_i) sub_08002A44(_r6, sub_08002A2C(_r6, _i - 1), _i);`
  shifts `[0]` into `[1]`, then `sub_08002A44(_r6, sub_08002A0C(_r6), 0)` puts the
  current song into `[0]`, then `sub_08002A1C(_r6, gUnk_08352D80[_r3])` installs
  the new one. Textbook two-deep push.
- **Pop**, `src/code_0806F780.c:9362-9368`:
  `v6 = sub_08002A2C(slot, 0);` `if (v6) { sub_08002A1C(slot, v6); for (i = 1; i < 2; ++i) sub_08002A44(slot, sub_08002A2C(slot, i), i - 1); }`
  — restore `[0]` as current, then shift `[1]` down into `[0]`.
- `include/object.h:164-165` (`Macro_080E7D74`) — a degenerate push that saves the
  current song to `[0]` and sets the current to `0` (silence).
- Addresses: `linker.ld:45-46` puts `gUnk_02023520` at `0x02023520` and
  `gCurLevelInfo` at `0x02023530` — a window of exactly `0x10` = **4 x 2 x u16**.
  The array is boxed in on both sides with no slack, so `[4][2]` is exact.

**Against / unresolved:**
- The empty sentinel is `0xFFFF` at init but the pop tests `if (v6)` — i.e. it
  treats **0**, not `0xFFFF`, as "nothing to restore". `Macro_080E7D74` sets the
  *current* song to 0, so 0 is meaningful as "silence". Whether `0xFFFF` ever
  reaches the pop path (and what `m4aSongNumStart(0xFFFF)` would do) I cannot
  determine statically. This is a real inconsistency, not a naming artefact.
- Depth is hard-coded 2 everywhere via `for (i = 1; i < 2; ...)` loops that a
  compiler would unroll; if the array is ever re-declared with a different depth
  those loops do not follow.

---

### `gUnk_02026D50`
**Proposed:** `gRoomStateFlags`
**Type as used:** `u16[4]`, indexed `[roomSlot]`. Only bits 0-3 are ever touched.
**Confidence:** high for the mechanism, medium for the names below

**Bits:**

| bit | mask | set by | cleared by | gates |
|-----|------|--------|-----------|-------|
| 0 | `0x1` | `wip/code.c:441` — fresh room load (unconditional) | `src/code_080023A4.c:1144` | `src/code_080023A4.c:1799` — room 919 one-shot reset |
| 1 | `0x2` | `src/code_080023A4.c:1146` — the frame after bit 0 | `src/code_080023A4.c:1144` | nothing reads it; it is the phase counter for bit 0 |
| 2 | `0x4` | `wip/code.c:443-444` — room load **only when `prevRoom == room`** | `src/code_080023A4.c:1149` | nothing reads it; phase counter for bit 3 |
| 3 | `0x8` | `src/code_080023A4.c:1151` — the frame after bit 2 | `src/code_080023A4.c:1149` | object despawn + "is this room occupied" (below) |

**Evidence:**
- `include/data.h:699` — `extern u16 gUnk_02026D50[];`; `linker.ld:48` places it at
  `0x02026D50`.
- `wip/code.c:53` — `CpuFill16(0, gUnk_02026D50, 8)` — 8 bytes = **4 x u16**.
- `wip/code.c:441` — `gUnk_02026D50[levelInfo->unk65E] = 1;` (assignment, not OR:
  a fresh load wipes any leftover state).
- `wip/code.c:443-444` — `if (prevRoom == room) gUnk_02026D50[...] |= 4;` — bit 2
  distinguishes *re-entering the room you are already in* from a normal transition.
- `src/code_080023A4.c:1140-1152` (`sub_08001FF8`, the per-frame level tick) is the
  whole state machine:
  ```
  if ((gUnk_02026D50[i] & 2) != 0)  gUnk_02026D50[i] &= ~3;
  else if ((gUnk_02026D50[i] & 1) != 0) gUnk_02026D50[i] |=  2;
  if ((gUnk_02026D50[i] & 8) != 0)  gUnk_02026D50[i] &= ~0xC;
  else if ((gUnk_02026D50[i] & 4) != 0) gUnk_02026D50[i] |= 8;
  ```
  So each pair is a two-frame pulse: `1 -> 3 -> 0` and `4 -> 12 -> 0`.
- **Bit 3 readers** — all of the form "is any Kirby in this room, and is that
  room not mid-reload":
  `include/data.h:36-39` (`Macro_081135A8`), `include/object.h:196-206`
  (`Macro_0809E55C`, which sets `flags |= 0x1000` — despawn — when *no* qualifying
  Kirby is found), `src/code_0806F734.c:156`, `src/code_0806F780.c:976`,
  `:4250`, `:9581`, `:9601`, `:9656`, `:9675`, `:9715`.
- **Bit 3, the decisive one** — `src/code_0806F780.c:739-742`:
  ```
  if (gUnk_02026D50[gCurLevelInfo[a1->unk56].unk65E] & 8) {
      a1->flags |= 0x1000;   /* despawn */
      return TRUE;
  }
  ```
  i.e. on the frame after a same-room re-entry, **every object in that room is
  force-despawned**, so `CreateLevelObjects` repopulates it from scratch.
- Bit 0's single reader, `src/code_080023A4.c:1799-1803`: in room 919 only, clears
  `gUnk_0203AD10 & 0x20` / `& 0x40` and resets `gUnk_03000000` — a one-shot
  "first frame in this room" hook.
- Note the tick loop bound is `i < gUnk_0203AD44` (`src/code_080023A4.c:1142`) —
  `gUnk_0203AD44` is the *Kirby* count being used as the *slot* count. See its
  entry below.

**Against / unresolved:**
- Bits 1 and 2 have **no readers at all**; they exist purely so the state machine
  can time bits 0 and 3. Any name for them will be about mechanism, not meaning
  (`ROOM_LOADED_PENDING` / `ROOM_RELOAD_PENDING` at best).
- Bits 4-15 are never touched anywhere. The `u16` is therefore over-wide; whether
  the original was `u16` or the decomp inferred it from `CpuFill16` I cannot tell.
- The bit-3 despawn is genuinely surprising and worth a comment wherever it lands:
  a *same-room* door transition wipes the room's objects, a *different-room*
  transition does not (bit 2 is conditional at `wip/code.c:443`).

---

## Part 2 — the per-frame object lists

All four of these are cleared every frame by `sub_08036470`
(`wip/code_08032E98.c:871-897`), which is a task created at priority `0xFFFD`
(`wip/code_08032E98.c:715`) — so they are rebuilt from scratch each frame by
objects registering themselves, and consumed by the collision sweep
`sub_08037314` (`wip/code_08032E98.c:2196` onward). They are indexed by **room
slot**, taken from `gCurLevelInfo[objBase->unk56].unk65E` at every registration
site, with `0xFF` meaning "no owning player, do not register".

### `gUnk_02022F50`
**Proposed:** `gRoomCollisionList`
**Type as used:** `struct ObjectBase *[4][2][32]` (declared flat), indexed
`[roomSlot * 64 + (objBase->unk0 - 1) * 32 + n]`
**Confidence:** high

**Evidence:**
- `include/data.h:683` — `extern struct ObjectBase *gUnk_02022F50[];`
- `include/object.h:73-91` (`SetPointerSomething`) — the registration macro:
  ```
  _temp = (objBase)->unk56 != 0xFF ? gCurLevelInfo[(objBase)->unk56].unk65E : 0xFF;
  if (_temp != 0xFF) {
      u32 _temp2 = _temp * 64 + ((objBase)->unk0 - 1) * 32;
      _idx = gUnk_02022EB0[_temp][(objBase)->unk0 - 1]++ + _temp2;
      gUnk_02022F50[_idx] = (objBase);
      gUnk_02022F50[_idx + 1] = NULL;
  }
  ```
  This one macro establishes: room-slot subscript, two sub-lists selected by
  `ObjectBase::unk0` (values 1 and 2), 32 entries each, `gUnk_02022EB0` as the
  count, and a NULL terminator written one past the tail.
- Registration is opt-in and gated: skipped when `flags & 0x1000` (despawning) or
  `flags & 0x200` (`include/object.h:74`). Used by ~100 object files
  (`grep -rl SetPointerSomething src` — box_boxer, wiz, kracko, sword_knight, ...).
- `wip/code_08032E98.c:71-73` (`sub_08039490`) — returns `&gUnk_02022F50[temp * 64]`
  for an object, i.e. "the head of my room's list A".
- Consumption, `wip/code_08032E98.c:2241-2380` — pass 2 walks list B
  (`&gUnk_02022F50[i * 64 + 32]`, count `gUnk_02022EB0[i][1]`) against list A,
  the Kirbys, and itself; pass 3 walks list A against itself and the Kirbys.
  Entries are NULLed in place when a handler reports the object consumed
  (`wip/code_08032E98.c:2262`, `:2268`).
- `wip/code_08032E98.c:888-891` — reset writes `NULL` to indices 0, 64, 128, 192:
  exactly the four list heads at stride 64.
- Addresses: `linker.ld:35-36`, `0x02022F50` to `gUnk_02023350` at `0x02023350`
  is `0x400` = 1024 = **4 x 64 x 4 bytes**, exactly filling the window. And what
  it would overrun into is a `struct Task *` (`include/data.h:687`) — see below.
- `ObjectBase::unk0` is bounded to `{0,1,2}` — see the analysis comment at
  `wip/code_08032E98.c:2114-2124`, which reasons from `gUnk_02022EB0[][2]` and
  the 3-entry handler table `gUnk_0834BD88`.

**Against / unresolved:**
- There is **no overflow check**. A room with more than 32 registered objects of
  one class writes past its sub-list into the next one, and past slot 3's list
  into `gUnk_02023350` (a `struct Task *`). The 32-entry limit is a hard cap for a
  level editor, exactly like the 1950-cell one, and it is enforced by nothing.
- What distinguishes list A (`unk0 == 1`) from list B (`unk0 == 2`) is decided by
  the handler table `gUnk_0834BD88[unk0]` and by which flag bits gate each pass
  (`flags & 0x20000000` for pass 2 block 1, `wip/code_08032E98.c:2247`). I did not
  chase this to a clean "attacker vs. target" reading and I do not think the code
  supports one cleanly. A name like `gRoomCollisionList` deliberately does not
  commit.

### `gUnk_02022EB0`
**Proposed:** `gRoomCollisionListCounts`
**Type as used:** `u8[4][2]`, indexed `[roomSlot][objBase->unk0 - 1]`
**Confidence:** high

**Evidence:**
- `include/data.h:682` — `extern u8 gUnk_02022EB0[][2];`
- `include/object.h:86` — post-incremented as the insertion cursor for
  `gUnk_02022F50` (quoted above).
- `wip/code_08032E98.c:718-725` and `:876-883` — reset, all eight elements
  written individually as `[0..3][0..1]`. That enumeration is itself the proof of
  the `[4][2]` shape.
- Read as the loop count in every collision pass:
  `wip/code_08032E98.c:2228` (`gUnk_02022EB0[i][0]`), `:2241` (`[i][1]`),
  `:2250`, `:2308`, `:2343`, `:2352`.

**Against / unresolved:**
- `u8` counts against 32-entry lists: an overflowing room silently wraps the
  cursor at 256, not at 32.

### `gUnk_02022EC0`
**Proposed:** `gRoomMovingSolids`
**Type as used:** `struct Object2 *[4][8]`, indexed `[roomSlot][n]`
**Confidence:** medium-high

**Evidence:**
- `include/data.h:711` — `extern struct Object2* gUnk_02022EC0[][8];`
- `include/object.h:59-71` (`Macro_080A4728`) — registration:
  `_idx2 = _idx * 8 + gUnk_02022F40[_idx]++; gUnk_02022EC0[0][_idx2] = (obj2);`
  (written through row 0 as a flat array; the effective address is identical).
- `wip/code_08032E98.c:78-80` (`sub_080394C8`) — returns `gUnk_02022EC0[temp]` for
  an object, `temp` being its room slot.
- `wip/code_08032E98.c:892-895` — reset writes `NULL` to `[0][0]`..`[3][0]`.
- Consumption, `wip/code_08032E98.c:2214-2237` — pass 1: each entry is tested
  against **every Kirby in the same room** and every list-A object in the same
  room with `unkC & 0x1000`, via `sub_08038010(other, &o2->base)`. No bounding-box
  work happens in the pass itself.
- `sub_08038010` (`wip/code_08032E98.c:4337-4400`) computes overlap at both the
  **previous** position (`unk48`/`unk4C`) and the **current** position (`x`/`y`) —
  a swept test, which is what you need to push a body out of a moving solid.
- Registrants are exactly the moving-solid object types:
  `src/flame_column.c:104`, `src/lava_wall.c:63`, `:65`, `:70`, `:72`,
  `src/star_stone_block.c`, `src/blockin.c:54`, `src/vert_sliding_door.c`.
- Addresses: `linker.ld:33-34`, `0x02022EC0` to `gUnk_02022F40` at `0x02022F40`
  is `0x80` = **4 x 8 x 4 bytes**, exactly filling the window.

**Against / unresolved:**
- "Moving solids" is an inference from the five registrant files plus the swept
  form of `sub_08038010`; there is no flag or type predicate that says so. If a
  sixth registrant elsewhere is not a moving solid, the name is wrong. I checked
  `grep -rl Macro_080A4728 src wip` and found exactly those five.
- 8 entries per room, again unchecked. `blockin` and `star_stone_block` can be
  numerous; whether any shipped room exceeds 8 simultaneously-active instances I
  did not verify.

### `gUnk_02022F40`
**Proposed:** `gRoomMovingSolidCount`
**Type as used:** `u8[4]`, indexed `[roomSlot]`
**Confidence:** high

**Evidence:**
- `include/data.h:712` — `extern u8 gUnk_02022F40[];`
- `include/object.h:67` — post-incremented insertion cursor for `gUnk_02022EC0`.
- `wip/code_08032E98.c:726-729`, `:884-887` — reset, four elements enumerated.
- `wip/code_08032E98.c:2216` — read as the pass-1 loop count.

**Against / unresolved:**
- Same unchecked-overflow caveat; `u8` cursor against an 8-entry list.

---

## Part 3 — the globals that select a player

### `gUnk_0203AD3C`
**Proposed:** `gLocalPlayerId`
**Type as used:** `u8`, value 0-3, a **playerId** (index into `gKirbys` and
`gCurLevelInfo`), never a room slot
**Confidence:** high

**Evidence:**
- `include/data.h:730` — already carries `// SUGGESTION: gCurrentPlayerId`.
- `src/code_080332BC.c:36-42` — the definition:
  ```
  if (gUnk_0203AD10 & 2) gUnk_0203AD3C = (*(vu32*)REG_ADDR_SIOCNT << 0x1a) >> 0x1e;
  else                   gUnk_0203AD3C = arg1;
  ```
  In link play it is read from the SIO multiplayer ID bits — that is *this
  console's* seat, which is what makes "local" the right word rather than
  "current".
- `src/subgame_menu.c:891` and `src/multi_08019F28.c:362` — same derivation.
- Every use is "should this console render/hear it":
  `include/object.h:334-335` — sprite screen position is computed against
  `gCurLevelInfo[gUnk_0203AD3C].viewportPosition`;
  `include/data.h:16`, `:27`, `include/object.h:256`, `:272` — SFX only play when
  the object's room matches `gKirbys[gUnk_0203AD3C]`'s room;
  `src/code_080023A4.c:571` — only the local player's `LevelInfo` drives the mixer.

**Against / unresolved:**
- In single-player it also happens to be the human-controlled Kirby, so
  `gCurrentPlayerId` reads fine there; the two names diverge only in link play,
  where `gLocalPlayerId` is the accurate one. This is a judgement call, not a
  factual dispute.

### `gUnk_0203AD30`
**Proposed:** `gNumPlayers` (endorsing the existing suggestion)
**Type as used:** `u8`, count of *human* players, 1-4
**Confidence:** high

**Evidence:**
- `include/data.h:727` — already `// SUGGESTION: gNumPlayers`.
- `src/multi_08019F28.c:316`, `:685` — set to `1`; `:363` and
  `src/multi_08030C94.c:830` — set from `gMultiBootStruct.unk01`;
  `src/subgame_menu.c:890` — from `gUnk_020382A0.unk28`;
  `src/code_080332BC.c:43` — from the caller's `arg0`.
- `src/code_080332BC.c:52-59` — the discriminating use: `CreateKirby` runs for all
  4 ids, then `for (otherKirbyId = arg0; otherKirbyId < 4; otherKirbyId++) sub_0800ECAC(...)`
  parks ids `>= gUnk_0203AD30` in the start room. So `< gUnk_0203AD30` = human,
  `>= gUnk_0203AD30` = AI helper.
- `wip/code_08032E98.c:1678-1683`, `:4531-4532`, `:4549-4550`,
  `wip/large_star_stone_block.c:313-315` — all branch on
  `unk56 < gUnk_0203AD30` to distinguish human from AI Kirby.
- `include/data.h:348-349` — the existing comment on `ObjectBase::unk56` already
  says "player id ... compared against the player count `gUnk_0203AD30`".
- Difficulty scaling reads it as an index: `wip/code_08032E98.c:3411`, `:3414`,
  `:3548`, `:3551` — `gUnk_08351628[subtype][gUnk_0203AD30 - 1]`.

**Against / unresolved:**
- None found. `gHumanPlayerCount` would be more explicit given `gUnk_0203AD44`
  also counts players in a different sense (below), and I would mildly prefer it.

### `gUnk_0203AD44`
**Proposed:** `gKirbyCount`
**Type as used:** `u8`, always `4` during gameplay
**Confidence:** medium — the value is certain, the *name* is the problem

**Evidence:**
- `include/data.h:732` — `extern u8 gUnk_0203AD44;`
- `src/kirby.c:6145` — `++gUnk_0203AD44;` at the end of `CreateKirby`. That is the
  only increment in the tree.
- `src/code_080332BC.c:35` — zeroed, then `src/code_080332BC.c:52-55` calls
  `CreateKirby` exactly 4 times, so it is `4` by the time
  `src/code_080332BC.c:69` loops on it. `src/multi_08019F28.c:315`, `:684` and
  `src/subgame_menu.c:892` assign `4` directly.
- ~137 uses, all as the bound of a 0-3 loop.

**Against / unresolved — this is the important one:**
- The array it bounds is *not always* `gKirbys` / `gCurLevelInfo`. It is used as
  the **room-slot** count at `src/code_080023A4.c:1142` (the `gUnk_02026D50` tick)
  and at `wip/code_08032E98.c:2212` (the `gUnk_02022EC0` / `gUnk_02022F50`
  collision sweep, whose `i` indexes room slots, not players). It works only
  because both counts are 4. Naming it `gKirbyCount` makes those two loops read
  as a type error; naming it `gSlotCount` makes the other 135 read wrong. **There
  is no name that is correct at every site**, and a reviewer should decide whether
  to (a) pick `gKirbyCount` and add a comment at the two slot-loops, or (b) leave
  it unnamed. I recommend (a) with the comment; I do not think (b) is better.
- `src/code_0802B4A8.c:1803` — `for (i = gUnk_0203AD30; i < gUnk_0203AD44; i++)`
  iterates precisely "the AI Kirbys", which is the cleanest demonstration that
  `AD30 <= AD44` and that they mean different things.

### `gUnk_0203AD10`
**Proposed:** `gGameModeFlags`
**Type as used:** `u32` bitfield
**Confidence:** medium for the word, low-to-medium per bit

**Bits, with the evidence for each:**

| mask | reading | evidence |
|------|---------|----------|
| `0x1` | **never set** — suppresses object creation when set | read at `src/code_080023A4.c:1268` (`CreateLevelObjects` skipped) and `wip/code.c:134`; **no write site exists** (see below) |
| `0x2` | link / multiplayer session | `src/code_080332BC.c:36` selects the SIO-derived player id; `src/multi_08030C94.c:1286` sets the word to `2`; `src/multi_08032E98.c:142` clears it |
| `0x4` | sub-game (not the main adventure) | set as part of `= 6` (`src/multi_08019F28.c:313`) and `= 4` (`:682`); read at `src/object.c:331` guarding sub-game-only behaviour in rooms `0x3BB..0x3BD` |
| `0x8` | help menu shows a Kirby drawing instead of button glyphs | `include/data.h:721` (pre-existing comment); set/cleared at `src/multi_08019F28.c:375` / `:366` |
| `0x10` | demo / attract playback | `src/demo.c:74` sets the word to `0x10`; ~20 reads of the form `if (!(gUnk_0203AD10 & 0x10))` guard HUD and sound (`wip/code_08032E98.c:163`, `:176`, `:706`, `:3626`, `:3696`, ...) |
| `0x20` | room 919 vertical auto-scroll active | set `src/dark_mind.c:12056`; cleared `src/code_080023A4.c:1800`; drives the `gUnk_03000000` scroll at `src/code_080023A4.c:1956-1973` |
| `0x40` | that auto-scroll has reached its end | set `src/code_080023A4.c:1970` and `src/dark_mind.c:12052`; cleared `src/code_080023A4.c:1801` |
| `0x80` | Dark Meta Knight encounter in progress | set `src/dark_meta_knight.c:251`; cleared `:768` and `src/kirby.c:8989` |
| `0x100` | suppress SFX on music-slot 0 | set `src/kirby.c:10616`, cleared `:10652`; read in `PlaySfxInternal`, `include/data.h:11` |

**Against / unresolved:**
- **Bit 0 is read twice and written zero times.** I searched all of `src/`,
  `wip/`, and `asm/` for writes: the only assignments are whole-word `= 0`, `= 2`,
  `= 4`, `= 6`, `= 0x10` and read-modify-writes on bits 3, 5, 6, 7 and 8 (full
  list: `src/demo.c:74`, `src/multi_08019F28.c:313`, `:366`, `:375`, `:682`,
  `src/code_08123950.c:232`, `src/dark_meta_knight.c:251`, `:768`,
  `src/code_080332BC.c:82`, `src/code_0814EBE4.c:263`, `src/multi_08032E98.c:83`,
  `:142`, `src/multi_08030C94.c:1286`, `src/code_080023A4.c:1800`, `:1801`,
  `:1970`, `src/kirby.c:8989`, `:10616`, `:10652`, `src/dark_mind.c:12052`,
  `:12056`). All 43 literal-pool references in `asm/` are `ldr`+`ands` reads. So
  the `CreateLevelObjects` suppression path is dead in the shipped game. Caveat:
  I matched the `=gUnk_0203AD10` literal-pool form; an access reached by adding an
  offset to a different symbol's address would not show up.
- Bits 2 and 4 are only ever set as part of whole-word assignments, so their
  meaning is inferred from the mode that assigns them, not from an isolated
  set-site. `0x4` = "sub-game" is the weakest claim here.
- The word mixes true modes (`0x2`, `0x10`) with transient per-encounter state
  (`0x20`, `0x40`, `0x80`), so `gGameModeFlags` is a compromise. Splitting it is
  not an option — it is one word in EWRAM.

### `gUnk_0203AD20`
**Proposed:** *no confident proposal* — `gGameplayFlags` if a name is required
**Type as used:** `u32` bitfield, 4 bits used
**Confidence:** low

**Bits:**

| mask | reading | evidence |
|------|---------|----------|
| `0x1` | **never set**; cleared when a bonus-item pickup finishes | cleared `src/bonus.c:200`, `:211`; no set site anywhere (same search as above; only 4 `asm/` references, all reads) |
| `0x2` | Kirby idle / sleeping animation state | set `src/kirby.c:13116`, cleared `:13246`, both inside the idle-timer logic (`kirby->idleTimer > gUnk_0203AD44`) |
| `0x4` | invincibility-candy music is playing — suppress normal BGM | set `src/code_0806F780.c:7839-7841` immediately after `m4aSongNumStart(MUS_INVINCIBLE_CANDY)`; cleared `:7873`, `:7882`, `:7887`, `:7952`; read as a guard before every `m4aSongNumStartOrChange` in `include/object.h:167`, `:189`, `src/code_080023A4.c:582`, `src/code_0806F780.c:9372`, `:9387` |
| `0x8` | Dark Mind final-phase HUD/behaviour | set `src/dark_mind.c:9787`, cleared `:9695`, `:10400`; read `wip/code_08032E98.c:3020`, `:3191`, `:3229`, `:3765` |
| — | reset | `src/code_080332BC.c:81`, `:94`, `src/code_080332BC.c:78-88` (`sub_08033478`) |

**Against / unresolved:**
- Bit 2 is the only one I would name with confidence (`SUPPRESS_ROOM_BGM` or
  similar). Bit 0 is unreachable-as-set, bit 1's meaning rests on one `kirby.c`
  site whose surrounding logic I did not fully read, and bit 3 is boss-specific.
- The four bits have nothing in common, so any single name for the word is
  arbitrary. My recommendation is to **name the bit constants and leave the word
  as `gUnk_0203AD20`** until someone reads `src/kirby.c:13100-13250` properly.

---

## Part 4 — the `struct LevelInfo` fields

`struct LevelInfo gCurLevelInfo[4]` (`include/data.h:719`) is indexed by
**playerId**, always. It lives at `0x02023530` (`linker.ld:46`), is
`sizeof == 1640` (`platform/port/gba_layout.h:759`, asserted every build), and
`4 * 1640 = 6560 = 0x19A0` places its end exactly at `gUnk_02024ED0`
(`linker.ld:47`) — so the array is boxed in on both sides and both its element
size and its length of 4 are pinned by the address map, not inferred. Its
`unk65E` is the bridge to the room-slot arrays above.

### `unk65E`
**Proposed:** `roomSlot`
**Type as used:** `u8`, 0-3
**Confidence:** high — confirmed at runtime

**Evidence:**
- **Runtime**: reading the live heap of the running port, `gCurLevelInfo[0].unk65E == 0`
  and the grid at `gUnk_02024ED0 + 0` was the correct one for the room. The
  room-slot-vs-playerId question this document was commissioned to answer is
  settled by measurement.
- `include/data.h:242` — declared `u8 unk65E;`; offset 1630 (`0x65E`) asserted at
  `platform/port/gba_layout.h:813`.
- `wip/code.c:406-429` — allocation, quoted in the summary. Note the two branches:
  **adopt** (`:411-413`, when another player is already in `room`) versus
  **claim + materialise** (`:415-449`).
- `wip/code.c:419-422` — the occupancy mask is built from *other* players whose
  `currentRoom != 0xFFFF`, so a player who is nowhere does not hold a slot.
- It is the subscript of every array in Parts 1 and 2, at every site: e.g.
  `src/code_080023A4.c:616`, `:1277`, `include/object.h:63`, `:80`, `:86`,
  `src/jack.c:55`, `src/chest.c:342`, `src/code_0806F780.c:739`.
- The `[4]`-ness is confirmed by the address arithmetic listed per array above,
  not just by the `for (i = 0; i < 4; i++)` at `wip/code.c:424`.

**Against / unresolved:**
- It is **never reset** when a player leaves a room. `wip/code.c:243-244` sets
  `currentRoom = 0xFFFF` without touching `unk65E`, so a stale slot index persists.
  This is harmless only because the mask loop (`wip/code.c:420`) skips players
  with `currentRoom == 0xFFFF`. Any refactor that drops that guard corrupts slot
  allocation.
- Consequence worth documenting alongside the name: because two players in one
  room share a slot, **all** of `gRoomSolidityCells`, `gRoomForegroundTilemap`,
  `gRoomSpawnFlags`, `gRoomVars`, both counters, the song and song stack, and the
  state flags are *shared* between them — one player breaking a block changes the
  other's collision grid. That is the intended behaviour, and it is the single
  most important fact this document records.

### `unk65F`
**Proposed:** *none — do not name*
**Type as used:** `u8`
**Confidence:** n/a

**Evidence:**
- `include/data.h:243` — declared, between `unk65E` and `unk660`; offset 1631
  (`0x65F`) asserted at `platform/port/gba_layout.h:814`. It is a real, distinct
  byte in the layout — `unk65E` is 1630 and `unk660` is 1632
  (`gba_layout.h:813`, `:815`), so it is not alignment padding.
- `grep -rn '\.unk65F\|->unk65F' src include wip` returns **zero results**.
- No `asm/` reference either: searching for the offsets `0x65f` / `#1631` across
  `asm/` returns nothing, though see the caveat.

**Against / unresolved:**
- A negative asm result here is weak evidence. `LevelInfo` is `0x668` bytes, far
  past Thumb's `ldrb [rN, #imm5]` reach, so every field in this region is accessed
  as `base + register` or via an `adds`-built pointer. The offset never appears as
  a literal. **Static reading cannot settle whether `unk65F` is a live field or
  dead space**, and the only way to settle it is to watch the byte at
  `gCurLevelInfo[p] + 0x65F` in the running game — which is now cheap to do, since
  the heap is already being read for `unk65E` two bytes earlier. That is the
  single highest-value follow-up measurement in this document.
- Leave it as `unk65F` until then.

### `unk660`
**Proposed:** `cameraTargetKirbyId`
**Type as used:** `u8`, a playerId
**Confidence:** high

**Evidence:**
- `include/data.h:244`.
- `src/code_080332BC.c:71` — initialised `gCurLevelInfo[idx].unk660 = idx;`
  (each camera starts following its own Kirby).
- `src/code_080023A4.c:913` — `struct Kirby *kirby = gKirbys + levelInfo->unk660;`
  then lines 915-916 derive the camera delta from that Kirby's position.
- `src/code_080023A4.c:1694` — same, in the instant-follow camera mode.

**Against / unresolved:**
- Only ever written once, at init. Nothing in the decompiled C ever points one
  player's camera at another Kirby via this field, so the "target" generality is
  unexercised. If it is genuinely always `== playerId`, a reviewer might argue the
  field is vestigial — but it is read, so the name should describe the read.

### `unk661`
**Proposed:** `cameraMirrorPlayerId`
**Type as used:** `u8`, a playerId
**Confidence:** high

**Evidence:**
- `include/data.h:245`.
- `src/code_080023A4.c:1728-1736` (`sub_08002DFC`, camera mode 4):
  ```
  struct LevelInfo *var0 = gCurLevelInfo + arg1->unk661;
  if (arg1->currentRoom == var0->currentRoom) {
      arg1->viewportPosition.x = var0->viewportPosition.x;
      arg1->viewportPosition.y = var0->viewportPosition.y;
      arg1->unk1C = 0; arg1->unk20 = 0;
  }
  ```
- Written at `src/kirby.c:11000`, `:11095`, `:13587`, each time as
  `gCurLevelInfo[kirby->...unk56].unk661 = kirby2->...unk56;` and immediately
  paired with `unk1EC = 4` (`:10999`, `:11094`) — i.e. "adopt this other player's
  camera", set when one Kirby is being carried by / merged with another.
- `src/code_080023A4.c:1159-1163` — the level tick deliberately runs all
  `unk1EC != 4` players **first**, then the `unk1EC == 4` players, so a mirrored
  camera reads an already-updated source. This ordering is only explicable if
  `unk661` is a cross-player reference.

**Against / unresolved:**
- None material.

### `unk1EC`
**Proposed:** `cameraMode`
**Type as used:** `u32`, 0-4, an index into `gUnk_082D8D60`
**Confidence:** high

**Evidence:**
- `include/data.h:235`.
- `src/code_080023A4.c:1128` — `gUnk_082D8D60[levelInfo->unk1EC](var1, levelInfo);`
- `src/code_080023A4.c:151-156` — the table, in order:

  | value | handler | behaviour |
  |-------|---------|-----------|
  | 0 | `sub_08001C40` | catch-up follow: delta to the target Kirby clamped to `unk662`/`unk664`, which ramp `+4` per frame; on catch-up it switches itself to mode 1 and resets the limits to `0x180` (`src/code_080023A4.c:928-937`) |
  | 1 | `sub_08002D60` | instant follow: unclamped delta to the target Kirby (`src/code_080023A4.c:1694-1699`) |
  | 2 | `sub_08002DA0` | bounds clamp only — no follow; the camera is driven externally (`src/code_080023A4.c:1702-1722`) |
  | 3 | `nullsub_10` | frozen (`src/code_080023A4.c:1724`) |
  | 4 | `sub_08002DFC` | mirror `gCurLevelInfo[unk661]` (above) |

- Assignment sites match: `src/code_080332BC.c:70` (`= 1` at level start),
  `src/kirby.c:9391`, `:10679`, `:10750`, `:10907` (`= 1`),
  `src/kirby.c:10180`, `:10595`, `src/warp_star.c:1928` (`= 2`),
  `src/dark_meta_knight.c:246`, `src/warp_star.c:2199` (`= 3`),
  `src/kirby.c:10999`, `:11094` (`= 4`),
  `src/scroll_lock.c:286-301`, `:463`, `:635`, `:672`, `src/warp_star.c:1800`,
  `:2627`, `src/kirby.c:11193`, `:11399` (`= 0`).

**Against / unresolved:**
- Mode 0 vs mode 1 could equally be read as "damped" vs "rigid" rather than
  "catching up" vs "locked on"; the self-transition `0 -> 1` at
  `src/code_080023A4.c:934-937` is what makes "catch-up" the better reading, but
  a reviewer may prefer neutral names (`CAMERA_MODE_0`..`_4`).
- Mode 2 is `sub_08002DA0`, which is also called as a *subroutine* by modes 0 and
  1 (`src/code_080023A4.c:926`, `:1699`). So entry 2 in the table is "the shared
  clamp, used standalone" — a slightly unusual table entry worth a comment.

### `unk662` / `unk664`
**Proposed:** `cameraMaxStepX` / `cameraMaxStepY`
**Type as used:** `s16`, Q(8) pixels per frame
**Confidence:** high

**Evidence:**
- `include/data.h:246-247`.
- `src/code_080023A4.c:918-923` — the camera delta for the frame is clamped to
  `[-unk662, +unk662]` in x and `[-unk664, +unk664]` in y. That *is* the
  definition: a maximum step.
- `src/code_080023A4.c:931-932` — `unk662 += 4; unk664 += 4;` each frame while
  catching up (acceleration).
- `src/code_080023A4.c:936-937` — reset to `0x180` once caught up.
- `wip/code.c:475-476` — initialised to `0x180` at room load.
- `src/kirby.c:10086-10087`, `:10158-10159`, `:10175-10176`, `:11270-11271`,
  `:11408-11409`, `src/code_0806F780.c:8820`, `:8844`, `:8850` — bumped to
  `0x300` (a faster camera) for specific Kirby transitions.
- `0x180` >> 8 = 1.5 px/frame; `0x300` >> 8 = 3 px/frame — plausible camera speeds.

**Against / unresolved:**
- `src/dark_meta_knight.c:340-342`, `:430-435`, `:480-485` use the same two fields
  as a **screen-shake amplitude**: `li->unk662 += 0x18; if (li->unk662 > 0x180) ...;
  li->viewportPosition.x += li->unk662;` — added directly to the viewport, not
  used as a clamp, and reset to `0` at `:529-530`. So during that boss the field
  means something else entirely. `cameraMaxStepX` is right for the 95% case and
  wrong for Dark Meta Knight. A neutral `cameraShakeX`/`cameraSpeedLimitX` choice
  has to be made deliberately; I lean toward `cameraMaxStepX` plus a comment
  pointing at `dark_meta_knight.c:340`.

### `unk65C`
**Proposed:** `currentSongId`
**Type as used:** `u16`, a song table index; `0xFFFF` at init
**Confidence:** high

**Evidence:**
- `include/data.h:241`.
- `wip/code.c:65` — `levelInfo->unk65C |= 0xFFFF;` at level start (unconditional
  set to `0xFFFF`, same idiom as `currentRoom` on the line above).
- `src/code_080023A4.c:578` and `:585` — `levelInfo->unk65C = gUnk_02023518[levelInfo->unk65E];`
  in both branches of `sub_08001358`, i.e. it is a **per-player cache of the room
  slot's song id**.
- `src/code_080023A4.c:581` — `m4aSongNumStartOrChange(levelInfo->unk65C)` for the
  local player.

**Against / unresolved:**
- It is written and immediately read on the same lines; nothing else ever reads
  it. So it may be a scratch variable the original author happened to put in the
  struct rather than a meaningful cached field. `currentSongId` is accurate but
  possibly grander than the field deserves.

### `unk5FA`
**Proposed:** `prevRoom`
**Type as used:** `u16`, a roomId; `0xFFFF` = none
**Confidence:** medium-high

**Evidence:**
- `include/data.h:238` — declared immediately after `u16 currentRoom;` (`:237`);
  offsets 1528 and 1530 asserted at `platform/port/gba_layout.h:808-809`, i.e.
  adjacent `u16`s with no gap.
- `wip/code.c:243-244` — `levelInfo->unk5FA = levelInfo->currentRoom;` immediately
  followed by `levelInfo->currentRoom = room;`, at the top of `FillLevelInfo`
  (`wip/code.c:231`). That is the whole story: it is the previous value of
  `currentRoom`.

**Against / unresolved:**
- **It has no readers.** `grep` finds exactly one reference in the entire tree,
  the write at `wip/code.c:243`. The room-load logic that needs the previous room
  uses a *local* `u32 prevRoom = levelInfo->currentRoom;` captured at
  `wip/code.c:237` instead (used at `:384`, `:443`). So either `unk5FA` is read
  from assembly not yet decompiled, or it is genuinely write-only. Given
  `wip/code.c` is a reimplementation of `asm/code.s`, and the rest of that file is
  covered, write-only is the likelier reading — but I cannot prove a negative
  here for the same offset-invisibility reason as `unk65F`.
- Name it `prevRoom` anyway: the assignment is unambiguous about what the value
  *is*, regardless of whether anyone uses it.

### `unk5FC`
**Proposed:** `fgAnimState`
**Type as used:** `u16[16][2]` — `[strip][0]` = frame index, `[strip][1]` = tick
counter
**Confidence:** high

**Evidence:**
- `include/data.h:239` — `u16 unk5FC[0x10][2]; /* TODO: determine real size */`
- `wip/code.c:311` — `CpuFill32(0, levelInfo->unk5FC, 0x40);` = 64 bytes =
  16 x 2 x `u16`. **The declared `[0x10][2]` is correct**; the TODO can be closed.
- Independently: `platform/port/gba_layout.h:810-811` asserts `unk5FC` at 1532
  and `unk63C` at 1596, a gap of exactly 64 bytes. The field is boxed in by its
  neighbours, so the size is pinned, not inferred.
- `src/code_080023A4.c:1208-1240` — the animation tick:
  ```
  u16 *var2 = levelInfo->unk5FC[i];                       // i < fgInfo->unkC
  const struct ForegroundInfo_8p *var3 = fgInfo->unk8 + i;
  if (var2[1] == 0) { ...CpuCopy32 the frame's tiles to 0x6008000 + var3->unk0... }
  { u32 r0 = var2[1] + 1;
    if ((var2[1] = r0) < var3->unkC[var2[0]].unk4) continue;   // per-frame duration
    var2[1] = 0;
    if (++var2[0] < var3->unk10) continue;                     // frame count
    ... wrap ... }
  ```
  `var2[0]` indexes `var3->unkC[]` (the frame list) and is bounded by `var3->unk10`;
  `var2[1]` counts up to `var3->unkC[frame].unk4`. Frame index and timer,
  unambiguously.
- `src/code_080023A4.c:1991-2003` — a re-upload path that reads `var1[0]` as the
  current frame without ticking (used on room re-entry / VRAM restore).
- The loop bound is `fgInfo->unkC` (`src/code_080023A4.c:1206`), the room's
  animated-strip count, from `gForegroundInfo[gRoomProps[room].pixelDataIdx]`.

**Against / unresolved:**
- 16 is a hard cap on animated foreground strips per room, unchecked: nothing
  compares `fgInfo->unkC` against 16 before indexing `unk5FC[i]`. Overrunning it
  walks into `unk63C`. Another editor-relevant limit.
- "fg" here means the room's *foreground layer* tiles (`gForegroundInfo`,
  `gForegroundTilemaps`), the layer Kirby walks on — not a UI foreground.

### `unk63C`
**Proposed:** `bgPalAnimState`
**Type as used:** `u16[8][2]` — `[entry][0]` = frame index, `[entry][1]` = tick
counter
**Confidence:** high

**Evidence:**
- `include/data.h:240` — `u16 unk63C[8][2]; /* TODO: determine real size */`
- `wip/code.c:312` — `CpuFill32(0, levelInfo->unk63C, 0x20);` = 32 bytes =
  8 x 2 x `u16`. **The declared `[8][2]` is correct**; TODO can be closed.
- Independently: `platform/port/gba_layout.h:811-812` asserts `unk63C` at 1596
  and `unk65C` at 1628, a gap of exactly 32 bytes.
- `src/code_080023A4.c:1857-1880` — same two-word idiom as `unk5FC`, but the
  payload is a palette upload:
  ```
  u16 *var0 = levelInfo0->unk63C[i];                 // i < fgPal->unk8
  struct ForegroundPalette_4pp *fgPal4p = fgPal->unk4[i];
  if (var0[1] == 0) {
      LoadBgPaletteWithTransformation(fgPal4p->unk0.u16ppp[var0[0]][0], fgPal4p->unk4[1], fgPal4p->unk8)
      /* or DmaCopy16 into gBgPalette + fgPal4p->unk4[1] */
  }
  ```
  driven by `gForegroundPalettes[gRoomProps[currentRoom].paletteDataIdx]`
  (`src/code_080023A4.c:1854`).
- Runs only for `arg1` (the on-screen player) and only when the room is not
  transitioning (`src/code_080023A4.c:1852`).

**Against / unresolved:**
- Same unchecked cap: `fgPal->unk8` is not compared against 8.
- I did not determine what `fgPal4p->unk0.u16pp[i][2] == 999` means at
  `src/code_080023A4.c:1859` (a sentinel that skips the entry). `999` is a
  suspiciously decimal constant in a hex codebase and may be a data-authoring
  marker; it does not affect the field's meaning.

---

## Cross-cutting notes for a reviewer

1. **Room slot vs playerId.** Everything in Parts 1 and 2 is `[roomSlot]`.
   Everything reached through `gCurLevelInfo[...]` / `gKirbys[...]` is
   `[playerId]`. The conversion is always `gCurLevelInfo[playerId].unk65E` and it
   is never the identity in multiplayer. Confirmed at runtime for
   `gUnk_02024ED0`; the rest follow the same textual pattern at every call site.
   Two loops break the pattern by using `gUnk_0203AD44` (a Kirby count) to iterate
   slots: `src/code_080023A4.c:1142` and `wip/code_08032E98.c:2212`.
2. **Hard caps a level editor must respect**, none of them checked at runtime:
   - **1950 cells per room**, `(w/16) * (h/16)`. Largest of the 286 real rooms is
     1768 (136 x 13); widest is 136 cells, tallest 103. **182 bytes of headroom in
     the whole game.** Fixed-size array x4 at a fixed address (`linker.ld:47`) —
     not growable.
   - 32 collision-list entries per room per class (`gUnk_02022F50`); overflow runs
     into `gUnk_02023350`, a live `struct Task *`.
   - 8 moving solids per room (`gUnk_02022EC0`).
   - 32 spawn flags per room (`gUnk_02023358`), 16 vars (`gUnk_02023388`).
   - 16 animated foreground strips (`unk5FC`), 8 animated palettes (`unk63C`).
3. **One content index, five tables.** For all 286 real rooms,
   `mapDataIdx == solidityMapIdx == objectListIdx == unk22 == doorsIdx`
   (`struct RoomProps`, `include/data.h:484-506`; verified against `baserom.gba`).
   Authoring a new room means adding one entry to each of five parallel tables at
   the same index, not five independent ids. `gRoomProps` has 983 slots of which
   ids 101..982 are real non-contiguously; the rest is `0xFFFF` filler.
4. **Two dead bits.** `gUnk_0203AD10 & 1` and `gUnk_0203AD20 & 1` are read but
   never set. If a port reimplements these, the corresponding branches are
   unreachable in the original and should be preserved as such rather than
   "fixed".
5. **Two closable TODOs.** `include/data.h:239` and `:240` both say
   "determine real size"; `wip/code.c:311-312` and the offset assertions at
   `platform/port/gba_layout.h:810-812` determine both exactly (64 and 32 bytes).
6. **Follow-up measurements worth taking**, in priority order, now that heap
   reads are cheap:
   - `gCurLevelInfo[p] + 0x65F` (`unk65F`) — the only field in scope with zero
     references anywhere; static reading cannot settle it.
   - `gUnk_02024ED0` at `unk65E >= 1` — distinguishes stride 1950 from 1952;
     the slot-0 read already taken cannot.
   - `gUnk_02026D60` — stride and layout are still declaration-only, and the
     1954-vs-1950 discrepancy is unexplained.
   - `gCurLevelInfo[p] + 0x5FA` (`unk5FA`) — written every room load, read by
     nothing; confirm it really is write-only.

---

## Addendum: the follow-up measurements, taken

Added after the document above was written, by the coordinator. The
`DUMP_ROOM=` hook in `tools/headless_test.js` now carries the tail of all four
`gCurLevelInfo` entries, so item 6's first question could be answered rather
than filed. Three runs into room 803 at frames 1300, 1600 and 2000, single
player:

```
  p0 room=803  unk65E=0  unk65F=0  unk660=0  unk661=0
  p1 room=803  unk65E=0  unk65F=0  unk660=1  unk661=0
  p2 room=803  unk65E=0  unk65F=0  unk660=2  unk661=0
  p3 room=803  unk65E=0  unk65F=0  unk660=3  unk661=0
```

**`unk660` is the entry's own player index.** It holds 0/1/2/3 in
`gCurLevelInfo[0..3]` and is stable across all three samples. That is a new
determination, and it means a `LevelInfo` knows which player it belongs to
without being told.

**All four entries carry the same room and the same slot.** This is the
adoption logic at `wip/code.c:411-414` observed directly rather than inferred:
`FillLevelInfo` runs per player entry even in a single-player game, all four
land in room 803, and all four adopt slot 0. Slot sharing is therefore not an
edge case reachable only in multiplayer — it is what happens all the time.

**`unk65F` is still not determined, but it is now bounded.** It was zero in
every one of twelve samples. That is consistent with "always zero in single
player" and with "holds something only a second occupied room populates", and
this experiment cannot separate those. Settling it needs two players in
*different* rooms, which needs a working link session — currently blocked on
the MultiBoot probe phase (`docs/MULTIPLAYER.md`). Filed, not answered.

The remaining three items in §6 are unaffected and still worth taking: reading
`gUnk_02024ED0` at a slot other than 0 is the one that would settle stride
1950 versus 1952, and it has the same multiplayer prerequisite.
