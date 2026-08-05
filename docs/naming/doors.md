# Naming proposals: door / room-transition data

**Status:** review document. Nothing has been renamed. Every claim below carries a
`file:line` citation into `/home/agent-tom/Desktop/katam` (read-only) or a
byte-offset into `baserom.gba`, which I parsed directly to check the C against the data.

## Summary

`gUnk_08D640A4` is **not** a door table — it is a per-room *progress-tracking* table
that holds two kinds of entry in one flat, tag-discriminated, self-striding list:
chest entries (tag `1`) and door entries (tag `2`), chests always first. Its companion
`gVisitedDoors[i]` is a 16-bit word per room where bit *k* is "entry *k* of that room's
list has been collected/used" and bit 15 is a separate "this room has been entered"
flag. The union `Unk_08930E00` is **not** selected by any field of the outer struct: it
is a `transparent_union` of two views onto the same byte array, and the real
discriminator is the per-entry tag byte at offset 0, which each consumer tests itself
(`src/code_080023A4.c:1612`, `:1630`, `:1648`). The door variant carries
`(destRoomId, destX, destY)` — a complete outgoing-edge list — and I verified against
the ROM that for **all 286 real rooms**, the tag-2 entries are *exactly* the set of
tag-3 warp descriptors in that room's solidity map, i.e. this table is a faithful,
complete mirror of every door-tile transition in the game. It is saved to SRAM
(`src/save.c:23`), and the mysterious `- 2` there is explained by the table having 287
entries while the array is declared `[0x120]` = 288.

---

## Table-level facts established from the ROM

I parsed `gUnk_08D640A4` (ROM `0x08D640A4`, `0x47C` bytes → **287** pointers,
`data/data_2.s:863`) and `gRoomProps` (ROM `0x089331AC`, `0x9998` bytes / `0x28` →
**983** slots, `data/data_8.s:44013`). Results, all mechanical:

| Fact | Value |
| --- | --- |
| `gRoomProps` slots | 983 |
| slots with `doorsIdx == 0xFFFF` (whole 0x28 row is `FF`, i.e. unused) | 697 |
| real rooms (`doorsIdx != 0xFFFF`) | **286**, room ids 101..982, sparse |
| `doorsIdx` values in use | 0..286, each used by exactly one room; **index 4 is unused** (its list is empty) |
| total entries across all 287 lists | 643 (84 tag-1, 559 tag-2, 0 other) |
| entry stride (`unk01`) | always 8 |
| `unk02[2]` | always `FF FF` in all 643 entries → filler |
| longest list | 11 entries → bit 15 can never collide with an entry bit |
| lists where the count byte ≠ number of tag-1 entries | **0** |
| lists where a tag-2 entry precedes a tag-1 entry | **0** |
| rooms with a duplicate `(destRoomId, destX, destY)` key | **0** |

Every real room also has a valid `solidityMapIdx` and vice-versa (`0` exceptions), and
`sizeof(gVisitedDoors) - 2` = 574 bytes = 287 `u16` = exactly the table length.

### `doorsIdx` is not a door index — it is the room's content index

Independently confirmed over the ROM: for all **286 of 286** real rooms,

```
mapDataIdx (+0x18) == solidityMapIdx (+0x1A) == objectListIdx (+0x20)
                   == unk22 (+0x22) == doorsIdx (+0x24)
```

(offsets from `struct RoomProps`, `include/data.h:484-506`). `paletteDataIdx` (+0x16),
`objectList2Idx` (+0x1C), `backgroundIdx` (+0x1E) and `pixelDataIdx` (+0x14) do **not**
follow. So `doorsIdx` is one of five aliases for a single per-room content index, and
room id ↔ content index is a bijection over the 286 real rooms.

**Naming consequence:** `doorsIdx` (`include/data.h:503`) is misleading twice — it is
neither door-specific nor even table-specific. `contentIdx` (or `roomDataIdx`) is the
honest name, with the five fields collapsing to one. That is a bigger change than this
document proposes, but it should be decided *before* anyone renames `gUnk_08D640A4`,
because the two names have to agree.

### The room-id membership test, applied

The coordinator's discriminator — "a field is a room id iff every observed value lands in
the 286-element valid-room set" — separates the two union variants cleanly:

| Field | Values in the 286-room set |
| --- | --- |
| `Unk_08930E00_2.unk04` (tag 2) | **559 / 559** |
| `Unk_08930E00_1.unk04` (tag 1) | 22 / 84 |

559/559 across a 286-element subset of a 983-wide id space is not coincidence. 22/84 is
exactly what you would expect from arbitrary `s16` coordinates that happen to fall in
range. This is measurement, not inference, and it settles both fields at once.

---

### `gUnk_08D640A4`

**Proposed:** `gRoomProgressLists`
(`extern const struct RoomProgressList *const gRoomProgressLists[287];`)

**Confidence:** high (that it is per-room and holds both chests and doors);
medium (on the exact word "Progress" vs. something like `gRoomTrackedObjects`)

**Evidence:**
- `include/data.h:860` — declared `extern const struct Unk_08930E5C *const gUnk_08D640A4[];`.
- `src/code_080023A4.c:1587`, `:1607`, `:1625`, `:1643` — every access is
  `gUnk_08D640A4[gRoomProps[roomId].doorsIdx]`, so it is indexed by room, one list per room.
- `include/data.h:503` — the index field is `gRoomProps.doorsIdx` (offset `0x24`).
- ROM parse: 287 pointers, all distinct, 286 of them reachable via some room's `doorsIdx`.
- `src/chest.c:113` reaches this table through `sub_08002B30` to record a **chest**;
  `src/mirror_door.c:116`, `src/special_doors.c:100`, `src/special_doors.c:326`,
  `src/special_hub_mirror.c:37`, `src/kirby.c:12877` reach it to record a **door**. It is
  therefore not door-only, and the existing `doorsIdx` field name is a half-truth.

**Against / unresolved:**
- The name `doorsIdx` in `include/data.h:503` is already committed upstream. If this
  table is renamed to something chest-inclusive, `doorsIdx` should probably become
  `progressIdx` in the same pass, or the two names will contradict each other. That is a
  larger blast radius than this document's scope.
- 216 of the 287 lists have `chestCount == 0` and 34 have no entries at all, so the
  "progress" framing is only load-bearing for a minority of rooms. Someone who prefers
  `gRoomDoorLists` and treats chests as a secondary payload is not obviously wrong.
- There is a sibling table `gUnk_08D63C28` at ROM `0x08D63C28`, also `0x47C` bytes
  (`data/data_2.s:860`), indexed by `gRoomProps[room].unk22` at `wip/code.c:309`. It is a
  *different* structure (its pointers land in the `0x088D6xxx` region, not `0x0893xxxx`).
  Do not assume the two are parallel; I did not investigate it (out of scope).

---

### `struct Unk_08930E5C`

**Proposed:** `struct RoomProgressList`

**Confidence:** high

**Evidence:**
- `include/data.h:635-638` — `{ union Unk_08930E00 unk00; u8 unk04; }`, 8 bytes with padding.
- ROM: the three padding bytes at offsets 5..7 are `00 00 00` in all 287 instances.
- The two members are exactly "pointer to the entry array" and "how many of the leading
  entries are chests" — see the two field sections below.

**Against / unresolved:** none. This one is only a wrapper.

---

### `struct Unk_08930E5C.unk00`

**Proposed:** `entries`

**Confidence:** high

**Evidence:**
- `src/code_080023A4.c:1607` and `:1625` — dereferenced as `->unk00.pat1` / `->unk00.pat2`
  and immediately used as the head of a walk (`var0 = (void *)var0 + var0->unk01`,
  `:1619`, `:1637`, `:1657`).

**Against / unresolved:** none.

---

### `struct Unk_08930E5C.unk04`

**Proposed:** `chestCount`

**Confidence:** high

**Evidence:**
- `src/code_080023A4.c:1585-1588` (`sub_08002AAC`) returns it raw.
- `src/code_080023A4.c:1590-1602` (`sub_08002AD0`) uses it as the **loop bound** for a
  popcount over `gVisitedDoors[...]`: `for (i = 0; i < var1; i++) if ((var2 >> i) & 1) var0++;`
  So it bounds a *prefix* of the bit word, not the whole word.
- `src/pause_area_map.c:2922` — `if (sub_08002AAC(roomId) == sub_08002AD0(roomId))` sets
  `roomCompletion = AREAMAP_ROOM_COMPLETED`. So the prefix it bounds is "the collectibles
  that make a room show as complete on the area map".
- ROM: for all 287 lists, this byte equals the number of tag-1 (chest) entries, and tag-1
  entries always occupy the front of the list. Values observed: 0 (×216), 1 (×61),
  2 (×7), 3 (×3). Total = 84 = total tag-1 entries.
- `src/code_080023A4.c:1605-1620` (`sub_08002B30`) sets bit *i* only for tag-1 matches, and
  is called from `src/chest.c:113` right after `CollectChest(chest2->unkE2)`.

**Against / unresolved:**
- Strictly, the code only proves "number of leading list entries that count towards room
  completion". That those are exactly the chests is a fact about the *data*, not the code.
  If a future entry tag `3` were added and placed in the prefix, `chestCount` would become
  wrong while `completionCount` would stay right. `completionCount` is the safer name;
  `chestCount` is the more useful one. I lean `chestCount` but flag the tradeoff.

---

### `union Unk_08930E00` — what selects `pat1` vs `pat2`

**Proposed:** `union RoomProgressEntry` with members `chest` (was `pat1`) and `door` (was `pat2`).

**Confidence:** high

**Answer: nothing in the outer struct selects it. The per-entry tag byte at offset 0 does,
and each consumer tests that byte itself.**

**Evidence:**
- `include/data.h:630-633` — it is a `__attribute__((transparent_union))` of two *pointer*
  types. A transparent union of pointers is a decomp idiom for "the same address is read
  through two different struct views"; it carries no discriminator of its own.
- Both variants agree on their first four bytes (`include/data.h:613-616` vs `:621-624`):
  `u8 unk00; u8 unk01; u8 unk02[2];`. Only offsets 4..7 diverge:
  - `Unk_08930E00_1`: `s16 unk04; s16 unk06;`
  - `Unk_08930E00_2`: `u16 unk04; u8 unk06; u8 unk07;`
  Both are 8 bytes.
- `src/code_080023A4.c:1605-1620` takes `.pat1`, then **filters on `var0->unk00 == 1`**
  (`:1612`) and skips anything else by adding `var0->unk01` (`:1619`).
- `src/code_080023A4.c:1623-1638` and `:1641-1658` take `.pat2`, then **filter on
  `var0->unk00 == 2`** (`:1630`, `:1648`), skipping by the same stride.
- Both walks share the *same* index `i` as the bit position into `gVisitedDoors`
  (`:1616`, `:1635`, `:1653`) and neither resets or skips `i` on a tag mismatch. This is
  only coherent if the list is a single mixed sequence and `i` is a global entry ordinal.
  That is the decisive structural argument: the union cannot be a per-list mode flag,
  because one list is walked by both views over one shared index space.
- ROM confirms: tag values present are `1` and `2` only, plus a 4-byte terminator whose
  first byte is `0`; stride is 8 for every real entry.

**Proposed constants:** `ROOM_PROGRESS_CHEST = 1`, `ROOM_PROGRESS_DOOR = 2`,
`ROOM_PROGRESS_END = 0`.

**Against / unresolved:**
- No code ever reads the terminator's fields; I infer "terminator" from the fact that
  every list's bytes end with `00 00 FF FF` before the next list's data, and from
  `sub_080025AC` in `src/code_080023A4.c:1348-1362`, which walks a structurally identical
  tag-first/stride-carrying list. This is strong but is data-inference, not code-forced.
- The terminator is dangerous: its stride byte is `0`, so if any of `sub_08002B30` /
  `sub_08002BA8` / `sub_08002C18` ever reaches it, the `for (;;)` at `:1610` / `:1628` /
  `:1646` spins forever (pointer never advances). The vanilla data never triggers this
  because every query key is present (verified below), but **any tool that re-points a
  door without updating this table will hang the game**, not misbehave gracefully.

---

### `struct Unk_08930E00_1` (tag 1)

**Proposed:** `struct RoomChestEntry`

- `.unk00` → **`kind`** — confidence high. `src/code_080023A4.c:1612` compares it to `1`.
- `.unk01` → **`size`** — confidence high. `src/code_080023A4.c:1619` uses it as the byte
  stride to the next entry. Always 8 in ROM, but the code treats it as variable, so name it
  for what it is.
- `.unk02[2]` → **`filler02`** — confidence high. Never read anywhere; `FF FF` in all 643
  ROM entries.

### `struct Unk_08930E00_1.unk04` / `.unk06`

**Proposed:** `objX` / `objY`

**Confidence:** high

**Evidence:**
- `src/code_080023A4.c:1613-1614` — matched against `arg1` / `arg2` of `sub_08002B30`.
- `src/chest.c:113` — the only caller:
  `sub_08002B30(chest->obj2.base.roomId, chest->obj2.object->x, chest->obj2.object->y);`
  `obj2.object` is the `struct Object` **spawn-template** entry, whose `x`/`y` are `s16`
  (`include/data.h:257-258`) — matching the `s16` typing of `unk04`/`unk06` exactly.
- So the key is the chest's coordinates *as written in the room's object list*, not its
  live runtime position (which `src/chest.c:44` mutates during init).
- ROM sample: `gRoomProps` index with `doorsIdx == 3` → entry
  `01 08 FF FF 38 04 60 00` → `objX = 0x0438`, `objY = 0x0060`.
- Membership test: only 22 / 84 of these values fall in the valid-room-id set, so they are
  emphatically **not** room ids — which is the point worth making, since `unk04` at the
  same offset in the sibling variant is.

**Against / unresolved:**
- Nothing pins the *unit*. `struct Object.x` is `s16` and `src/chest.c:24` feeds it through
  `InitObject`, after which `obj2.base.x` is in 1/256-px. Whether the template value is
  raw pixels or something else is not settled by anything I read. `objX`/`objY` is
  deliberately unit-agnostic for that reason. Do not name these `pixelX`/`tileX`.
- Also note this is a *chest* identity, not a door one. If the reviewer wants the names to
  read as "treasure", `chestX`/`chestY` is equally defensible; I chose `objX`/`objY`
  because the code literally compares against object-template coordinates.

---

### `struct Unk_08930E00_2` (tag 2) — the door entry

**Proposed:** `struct RoomDoorEntry`

- `.unk00` → **`kind`** — high. `src/code_080023A4.c:1630`, `:1648` compare it to `2`.
- `.unk01` → **`size`** — high. `src/code_080023A4.c:1637`, `:1657`.
- `.unk02[2]` → **`filler02`** — high. Never read; `FF FF` throughout ROM.

### `struct Unk_08930E00_2.unk04`

**Proposed:** `destRoomId`

**Confidence:** high

**Evidence:**
- `src/code_080023A4.c:1631` / `:1649` — matched against `arg1`, typed `u16`.
- `src/kirby.c:12877-12878` — the call site is
  `sub_08002C18(kirby->...roomId, r4.pat3->unk08, r4.pat3->unk0A, r4.pat3->unk0B);`
  immediately followed by `kirby->base.base.base.roomId = r4.pat3->unk08;`. The value
  passed as `arg1` *is* the room Kirby is about to be in.
- `include/data.h:132` already annotates the source field: `u16 unk08; // roomId` on
  `struct Unk_0888562C_3`.
- `src/code_080023A4.c:1352` (`sub_080025AC`) shows `Unk_0888562C_3` is looked up from the
  room's solidity map by the tile Kirby is standing on, so it is the door-tile descriptor.
- ROM membership test: **559 / 559** tag-2 `unk04` values land inside the 286-element set
  of valid room ids. Zero out-of-range, zero dangling. By contrast only 22 / 84 tag-1
  `unk04` values do, which is the null-hypothesis rate for coordinates.

**Against / unresolved:** none. This one cannot mean anything else.

### `struct Unk_08930E00_2.unk06` / `.unk07`

**Proposed:** `destX` / `destY` (units: 16-px tiles in the destination room)

**Confidence:** high

**Evidence:**
- `src/code_080023A4.c:1632-1633` / `:1650-1651` — matched against `arg2` / `arg3`, both `u8`.
- `src/kirby.c:12877` passes `r4.pat3->unk0A` and `r4.pat3->unk0B` as those args, and the
  next lines consume the *same* two fields as Kirby's landing position:
  ```
  kirby->base.base.base.x = (r4.pat3->unk0A << 12) - 0x800;
  kirby->base.base.base.y = (r4.pat3->unk0B << 12) - 0x701;
  ```
  (`src/kirby.c:12879-12880`), and as the respawn point at `:12883-12884`
  (`kirby->spawnLocation.x/.y`).
- `<< 12` against a position in 1/256-px units = ×16 px, so these are 16-px tile
  coordinates. The same unit appears on the read side: `sub_080025AC` is called with
  `base.x >> 12` (`src/special_doors.c:98`; `src/mirror_door.c:112`, whose `unkBC`/`unkBE`
  are set from `x >> 12` / `y >> 12` at `src/mirror_door.c:32-33`).
- `src/kirby.c:12406-12418` (`sub_080551FC`) takes the same triple as
  `(u16 a2, u8 a3, u8 a4)` and assigns `kirby->roomId = a2; spawnLocation.x = a3; .y = a4;`.

**Against / unresolved:**
- These are the coordinates of the **arrival point in the destination room**, i.e. the
  door's *far* side. They are not the position of the door tile in the room that owns this
  entry. A naive editor that treats `destX/destY` as "where the door is drawn" will be
  wrong. The owning room's door tile position is `Unk_0888562C_3.unk02/.unk03`
  (`src/code_080023A4.c:1357-1358`), which lives in the solidity-map list, not here.
- Consequently the `(destRoomId, destX, destY)` triple is the *door identity key*, and it
  identifies the door by its exit, not its entrance. ROM check: no room has two door
  entries sharing a triple, so the key is unique per room and safe to use as an identity.

---

### `gVisitedDoors`

**Proposed:** `gRoomProgressBits` (or keep `gVisitedDoors` only if the reviewer also keeps
`doorsIdx`; the two should agree)

**Confidence:** high on the layout; medium on the name

**Layout (all citations below):**

| Bit | Meaning |
| --- | --- |
| 0 .. `chestCount-1` | chest *k* of this room's list has been collected |
| `chestCount` .. `n-1` | door *k* of this room's list has been used |
| 15 | this room has been entered at least once |

**Evidence:**
- `include/data.h:704` — `extern u16 gVisitedDoors[0x120];`.
- Indexed identically to the table: `gVisitedDoors[gRoomProps[roomId].doorsIdx]`
  (`src/code_080023A4.c:1577`, `:1582`, `:1594`, `:1616`, `:1635`, `:1653`).
- **Bit 15 is separate.** `src/code_080023A4.c:1577` returns `>> 0xF`;
  `:1582` sets `|= 0x8000`. `src/kirby.c:12952` calls the setter (`sub_08002A80`) *after*
  `kirby->...roomId` has been updated to the destination, guarded by `if (sl != 0xFFFF)`
  where `sl` is the previous room — so it marks the room just *entered*.
- Bit 15 is read as "visited" by three independent consumers:
  `src/pause_area_map.c:2920` (`AREAMAP_ROOM_UNVISITED` → `AREAMAP_ROOM_VISITED`),
  `src/pause_area_map.c:3106` (area is `AREAMAP_NO_MAP` if any of its rooms is visited),
  `src/pause_world_map.c:1185` and `:1274` (`WorldMapSetTileDoorUnvisited`),
  `src/code_08138D64.c:756`.
- **Low bits are per-entry.** `src/code_080023A4.c:1616` sets `1 << i` where `i` is the
  index of the matching tag-1 entry; `:1653` sets `1 << i` for tag-2; `:1635` reads
  `>> i & 1` for tag-2. Same `i` space for both — see the union section.
- **Bit 15 can never be an entry bit:** longest list in ROM is 11 entries.
- **It is save data — confirmed.** `src/save.c:23`:
  `{ (void *)gVisitedDoors, sizeof(gVisitedDoors) - 2 }` inside `gWorldProps[]`, which is
  the `SAVE_BUFFER_TYPE_WORLD_PROPS` buffer written by
  `UpdateSaveBufferByOffset(SAVE_BUFFER_TYPE_WORLD_PROPS, ...)` at `src/kirby.c:12958`
  and `:12960` — i.e. the game flushes it on every room transition.

**The `- 2` TODO at `src/save.c:23` is answered:**
`sizeof(gVisitedDoors)` = `0x120 * 2` = 576 bytes. Minus 2 = **574 bytes = 287 `u16`**,
which is exactly the length of `gUnk_08D640A4` (`0x47C / 4 = 287`, `data/data_2.s:863`).
The array is over-declared by one element and the last element is never a valid
`doorsIdx`, so it is deliberately excluded from the save image. The array should arguably
be declared `[287]`; the `[0x120]` is a round-up. **Save-format fact:** offset of
`gVisitedDoors` within the world-props record is
`sizeof(gUnk_02028BF0) + sizeof(gUnk_02028C10)` = `0x20 + 0x80` = 160 bytes, length 574
bytes, little-endian `u16` per room-with-doors, indexed by `doorsIdx` not by room id.

**Against / unresolved:**
- The name `gVisitedDoors` is wrong twice over: the array is indexed by *room*, not by
  door, and most of its semantic weight (bit 15, the chest bits) is not about doors at all.
  But renaming it is a wide diff and it is a save-format symbol, so I am flagging rather
  than pushing.
- I did not verify what clears it on a new file. `sbWorldPropsClear` is declared at
  `src/save.c:9` but I did not read its body, so "starts at zero" is assumed, not proven.
- Nothing proves the chest bits and door bits cannot overlap *by construction* — it is
  guaranteed only by the data invariant "tag-1 entries come first", which holds in all 287
  ROM lists but is not enforced by code.

---

### The seven consuming functions

Proposed names, all derived from the evidence already cited above:

| Current | Proposed | Confidence |
| --- | --- | --- |
| `sub_08002A5C` (`src/code_080023A4.c:1575`) | `IsRoomVisited` | high |
| `sub_08002A80` (`:1580`) | `SetRoomVisited` | high |
| `sub_08002AAC` (`:1585`) | `GetRoomChestCount` | high |
| `sub_08002AD0` (`:1590`) | `GetRoomChestsCollected` | high |
| `sub_08002B30` (`:1605`) | `SetChestCollected` | high |
| `sub_08002BA8` (`:1623`) | `IsDoorUsed` | medium |
| `sub_08002C18` (`:1641`) | `SetDoorUsed` | medium |

**Against / unresolved on `IsDoorUsed` / `SetDoorUsed`:** the bit's *display* meaning is
"this door's other side is known". `src/mirror_door.c:116` picks an open-mirror animation
variant when set and a closed one when clear; `src/special_doors.c:100` and `:326` and
`src/special_hub_mirror.c:37` create a `0x2CF` "?" marker sprite when **clear** and destroy
it when set. So `IsDoorDiscovered` / `IsDoorExplored` reads better against those four call
sites, while `Used` reads better against the writer at `src/kirby.c:12877`, which fires the
instant Kirby transits. Both are defensible; I did not find a call site that discriminates.

---

## Can a room graph be reconstructed statically?

**Yes — for door-tile transitions, completely and exactly. No — for the full traversal
graph, and I can name the 33 rooms where it visibly breaks.**

### Procedure (verified end-to-end against `baserom.gba`)

No code execution, no decompression, no object-list parsing. Four tables, all
little-endian, all at fixed ROM addresses.

1. **Enumerate rooms.** `gRoomProps` at ROM `0x089331AC`, stride `0x28`, 983 slots
   (`data/data_8.s:44013`; struct `include/data.h:484-506`). Slot *r* is a real room iff
   `u16 @ +0x24` (`doorsIdx`, `include/data.h:503`) != `0xFFFF`. → **286** rooms, ids
   101..982 sparse; the other 697 slots are all-`0xFF` filler.
2. **Get the content index.** `contentIdx = u16 @ (0x089331AC + 0x28*r + 0x24)`.
   Range 0..286; the map room-id → contentIdx is a bijection, so the inverse is a plain
   dict. (Offsets `+0x18`, `+0x1A`, `+0x20`, `+0x22` hold the same value — see
   "`doorsIdx` is not a door index" above — so any of them will do.)
3. **Find the room's entry list.**
   `listPtr = u32 @ (0x08D640A4 + 4*contentIdx)` (`data/data_2.s:863`,
   `include/data.h:860`; 287 entries). Subtract `0x08000000` for a file offset.
   At `listPtr`: `entriesPtr = u32 @ +0`, `chestCount = u8 @ +4`, `+5..+7` are zero pad
   (`struct Unk_08930E5C`, `include/data.h:635-638`).
4. **Walk the entries.** Start `off = entriesPtr`, ordinal `i = 0`. Loop:
   - `kind = u8 @ off`. If `kind == 0` → end of list, stop.
   - `size = u8 @ (off+1)` (always 8 in vanilla; honour it anyway — the code does,
     at `src/code_080023A4.c:1619`, `:1637`, `:1657`).
   - `u8 @ (off+2)`, `u8 @ (off+3)` are filler (`FF FF` in all 643 entries).
   - `kind == 1` → **chest**, at object-template coords
     `objX = s16 @ (off+4)`, `objY = s16 @ (off+6)` (`src/chest.c:113`).
   - `kind == 2` → **door**, emit directed edge
     `r -> (destRoomId = u16 @ (off+4))`, arriving at
     `destX = u8 @ (off+6)`, `destY = u8 @ (off+7)`, both in 16-px tiles
     (`src/kirby.c:12877-12884`).
   - `off += size; i += 1`.
5. **Bit index.** The ordinal `i` from step 4 is the bit position in
   `gVisitedDoors[contentIdx]` (`src/code_080023A4.c:1616`, `:1635`, `:1653`). It runs
   across *both* kinds, so door *d* sits at bit `chestCount + d`. Bit 15 is the
   room-visited flag and is never an entry bit (longest list is 11).

Reference implementation, ~25 lines:

```python
ROM   = open("baserom.gba","rb").read()
u8    = lambda a: ROM[a]
u16   = lambda a: int.from_bytes(ROM[a:a+2], "little")
s16   = lambda a: int.from_bytes(ROM[a:a+2], "little", signed=True)
u32   = lambda a: int.from_bytes(ROM[a:a+4], "little")
GBA   = lambda p: p - 0x08000000

ROOMPROPS, DOORTAB = 0x9331AC, GBA(0x08D640A4)

edges, chests = [], []
for r in range(983):
    ci = u16(ROOMPROPS + 0x28*r + 0x24)
    if ci == 0xFFFF:                       # unused slot
        continue
    lst  = GBA(u32(DOORTAB + 4*ci))
    off  = GBA(u32(lst))                   # entriesPtr
    nch  = u8(lst + 4)                     # chestCount
    i = 0
    while (kind := u8(off)) != 0:          # kind 0 == terminator
        if   kind == 1: chests.append((r, i, s16(off+4), s16(off+6)))
        elif kind == 2: edges .append((r, i, u16(off+4), u8(off+6), u8(off+7)))
        off += u8(off + 1)                 # stride
        i   += 1
```

Yields 286 rooms, 559 edges, 84 chests.

### Why I trust it: the completeness check

The authoritative per-room door data at runtime is *not* this table — it is
`LevelInfo.unk1E0`, assigned at `wip/code.c:307` as
`gSolidityMaps[gRoomProps[room].solidityMapIdx]->unk4` (`include/data.h:906-911`,
`gSolidityMaps` at `data/data_2.s:568`, ROM `0x08D63330`, also 287 pointers).
Note `solidityMapIdx` (`+0x1A`) equals `doorsIdx`, so it is the same content index.
`sub_080025AC`
(`src/code_080023A4.c:1348-1362`) walks that list to find the descriptor for the tile
Kirby is standing on, and `struct Unk_0888562C_3` (`include/data.h:125-135`) is the tag-3
variant carrying `unk08 = destRoomId`, `unk0A/unk0B = destX/destY`.

I extracted, for each of the 286 real rooms, the set of `(unk08, unk0A, unk0B)` triples
from every tag-3 descriptor in its solidity-map list, and compared it to the set of
`(destRoomId, destX, destY)` triples in its `gUnk_08D640A4` list:

```
rooms matching exactly: 286    mismatching: 0
rooms with doorsIdx == 0xFFFF but having warp descriptors: 0
```

So `gUnk_08D640A4` is a **complete and exact** static index of the game's door-tile
transitions. That is the strongest statement available and it is checkable in one script.

### The resulting graph

- 286 nodes, **559** directed edges.
- Out-degree histogram: 0 doors → 33 rooms, 1 → 93, 2 → 75, 3 → 54, 4 → 23, 5 → 3,
  6 → 1, 8 → 1, 11 → 3.
- 253 rooms have at least one outgoing door; 268 are the destination of at least one.
- 159 distinct `(src, dst)` pairs have no reverse pair — one-way links are common and real.
- Forward BFS from room `0x65` reaches 259 of the 286 rooms.

### The blocker: door tiles are not the only way to change rooms

`src/kirby.c:12856-12915` (`sub_080562D0`) has **three** paths that assign
`base.base.base.roomId`, and only the first is the door path:

1. `:12878` — `roomId = r4.pat3->unk08`, the door-tile path. This is the one indexed by
   `gUnk_08D640A4`, and it is the only one that calls `sub_08002C18` (`:12877`).
2. `:12905` — `roomId = objBase->unk63`, with position from `objBase->unk64/unk66`, where
   `objBase = kirby->base.base.base.unk6C` is *whatever object is currently carrying
   Kirby*. Nothing in `gUnk_08D640A4` describes these.
3. `:12913` — `roomId = kirby->roomId`, the respawn/soft-warp path. `kirby->roomId` is
   itself written from outside the door system, e.g.
   `src/warp_star.c:1801-1803`: `kirby->roomId = ws->unkBE; spawnLocation.x = ws->unkC0;`
   where `src/warp_star.c:1730-1731` sourced those from the object template
   (`ws->unkBE = template->unk1E; ws->unkC0 = template->unk1A;`, `struct Object` fields at
   `include/data.h:271-273`). **Warp stars are room transitions encoded in the object
   list, entirely invisible to this table.** The same is true of
   `sub_080551FC` (`src/kirby.c:12406`), which any actor can call with an arbitrary
   `(roomId, x, y)`.

The concrete symptom: **33 real rooms have zero outgoing door edges** and yet 15 of them
are reachable as destinations, so they must be exitable some other way. Room ids
(decimal → hex): `126`(0x7e), `129`(0x81), `142`(0x8e), `219`(0xdb), `230`(0xe6),
`318`(0x13e), `324`(0x144), `412`(0x19c), `417`(0x1a1), `516`(0x204), `705`(0x2c1),
`710`(0x2c6), `738`(0x2e2), `822`(0x336), `909-919`(0x38d-0x397), `950,951`(0x3b6,0x3b7),
`955-957`(0x3bb-0x3bd), `969`(0x3c9), `980-982`(0x3d4-0x3d6). Note `0x7e` is
`sAreaMapRoomInfos[AREAMAP_ROOMINFO_OFFSET_RAINBOW_ROUTE].roomId`
(`src/pause_area_map.c:268`) — the first room of Rainbow Route. You leave it, and the
static graph cannot say how.

### Second blocker: the graph is topology only, with no gating

Nothing in a `RoomDoorEntry` records a requirement. Whether a door is *usable* is decided
at spawn time by the actor:

- `src/mirror_door.c:46` and `src/special_doors.c:78`, `:308` — the door only becomes
  interactive if `gUnk_082D88B8[sub_080023E4(...)] & 0x4000`, a property of the room's
  decompressed solidity map, not of this table.
- `src/mirror_door.c:70` — `subtype1 == 2` mirror doors additionally require
  `*sub_08002888(SUB_08002888_ENUM_UNK_2, template->unk4, 0)`, i.e. a flag in
  `gUnk_02023488` (`src/code_080023A4.c:1459`), which is a *different* save array
  (`src/save.c:22` saves `gUnk_02028C10`, its persistent sibling).
- `src/vert_sliding_door.c:32` — gated on `*sub_08002888(0, door->obj2.object->unk4, ...)`,
  a switch-state flag; this actor is an in-room obstacle, not a room transition, and does
  not touch `gUnk_08D640A4` at all.

### If you want to *rewire* doors, not just read them

Reading needs one table. Writing needs two, kept in lockstep.

**Authoritative side — the solidity-map descriptor list.** Reached as
`gSolidityMaps[contentIdx]->unk4` (`wip/code.c:307`), walked by `sub_080025AC`
(`src/code_080023A4.c:1348-1363`). It is a tag-first, self-striding list like the other
one, but with a **different layout and a `u16` stride** — do not reuse the 8-byte walker.
`struct Unk_0888562C_3` (`include/data.h:125-135`, `packed, aligned(2)`, size `0xE`):

| Offset | Field | Meaning |
| --- | --- | --- |
| `+0x00` | `unk00` | tag, `3` for a warp descriptor |
| `+0x01` | `unk01` | solidity-tile value this entry is keyed to (`src/code_080023A4.c:1357`) |
| `+0x02` | `unk02` | door tile **x** in this room (`:1358`) |
| `+0x03` | `unk03` | door tile **y** in this room (`:1359`) |
| `+0x04` | `unk04` | `u16` stride to next entry (`src/code_080023A4.c:1363`) |
| `+0x06` | `filler06` | — |
| `+0x08` | `unk08` | **destRoomId** (`include/data.h:132`, `src/kirby.c:12878`) |
| `+0x0A` | `unk0A` | **destX**, 16-px tiles (`src/kirby.c:12879`) |
| `+0x0B` | `unk0B` | **destY**, 16-px tiles (`src/kirby.c:12880`) |
| `+0x0C` | `unk0C` | flags: `0x80` = do **not** move the respawn point (`src/kirby.c:12881`); `0x01` = face one way, `0x02` = face the other (`:12887`, `:12892`) |

This is the *only* side the game reads to decide where Kirby goes. Note this list gives
you the door's position **in the source room** (`+0x02`/`+0x03`), which
`gUnk_08D640A4` does not — so a graph with both endpoints located requires joining the
two tables.

**Shadow side — `gUnk_08D640A4`.** Only consulted for the save-bit lookup, keyed on the
triple `(destRoomId, destX, destY)`.

**The invariant you must preserve:** for every room, the set of
`(unk08, unk0A, unk0B)` triples in the solidity list must equal the set of
`(unk04, unk06, unk07)` triples in the `gUnk_08D640A4` list. Vanilla satisfies this for
286/286 rooms. **If you break it, the game hangs** — `sub_08002BA8`/`sub_08002C18` walk
`for (;;)` (`src/code_080023A4.c:1628`, `:1646`) and the terminator has stride `0`, so a
missing key is an infinite loop, not a miss. The same applies to `sub_08002B30` for
chests (`:1610`).

**Cheap safety property:** if a rewiring only permutes `destRoomId` *within* one room's
door set and applies the same permutation to both tables, the triples change but stay
matched, list lengths are unchanged, bit indices are unchanged, and existing saves stay
meaningful. Changing the *number* of doors in a room shifts every bit index above it and
invalidates `gVisitedDoors` for that room — plus the chest prefix invariant (all tag-1
before all tag-2) must be re-established or `chestCount` stops bounding the chests.

### Verdict for a randomizer / editor

- **Use it.** The table is the cleanest static room-graph source in the ROM, and it is
  provably identical to the runtime warp descriptors for all 286 rooms.
- **Do not treat it as the source of truth for editing.** The runtime reads
  `Unk_0888562C_3` out of the solidity map (`wip/code.c:307`, `src/code_080023A4.c:1348`).
  `gUnk_08D640A4` is a *shadow copy* used only for the save-bit lookup. Editing one without
  the other desynchronises them, and because the walk loops `for (;;)` with a zero-stride
  terminator (`src/code_080023A4.c:1646-1657`), a door whose key is missing from the shadow
  copy **hangs the game** on transit rather than failing softly.
- **Supplement it with the object lists.** Warp stars (`src/warp_star.c:1730`) and any
  actor calling `sub_080551FC` add edges you will otherwise miss, and 33 rooms have no
  door edges at all. A completability checker built on this table alone will report those
  33 rooms as dead ends and be wrong.
