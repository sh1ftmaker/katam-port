# Naming proposals: room / level data tables

**Status:** review document. Nothing has been renamed. Every claim below carries a `file:line`
citation into `/home/agent-tom/Desktop/katam` (read-only), or a byte-level measurement taken from
`baserom.gba` with the offsets that `data/*.s` `.incbin`s from.

## Summary

The three big unknowns in this scope turned out to be decodable. **`gUnk_08D63C28` is the AI-Kirby
navigation graph**: `unk10` is a run-length-compressed map of one *node index* per 16x16 cell (I
decompressed room 803's blob — 988 bytes, exactly the collision map's dimensions, values 0..19 against
a declared node count of 20), `unk14` is the node array (x, y, arrival radius, plus a next-hop routing
matrix `unk0[dest] -> next node`), and `unk0[4]` is a per-player *bytecode script* whose interpreter is
`sub_08013804` (`src/code_0800ECAC_2.c:2149`). **`SolidityMap.unk4`** is a null-terminated,
variable-stride list of cell records keyed by *(collision value, cellX, cellY)*; I walked it for all
286 rooms and the key matches the decompressed collision map at that cell in every record — 9500
records of type 1 (2x2 tile + collision patch) and 724 of type 3 (door, carrying a destination room
id). **`ForegroundInfo`/`ForegroundPalette`** are cleanly a tile-animation and a palette-animation
descriptor respectively; every field has an unambiguous role in `sub_080021E4` / `sub_08003108`, and
the ROM data decodes into sane frame tables under that reading. **`RoomTiledBG.unk4/unk6/unk7`** are
the tileset-animation triple read by `src/bg.c:527-539`, and `unk1C` is really *two* `u16`s — the ROM
loads offset 0x1C with `ldrh`, and 0x1E is a room id, matching the `roomId` tail field that
`ForegroundInfo` and `ForegroundPalette` already have.

What I could **not** resolve: `priorityFlags` bit 5 (0x20) is read by nothing — not in `src/`, not in
`wip/`, not in the remaining `asm/`, yet it is deliberately cleared in 14 specific rooms, so it means
*something* to the level authors and nothing to the engine. `LevelInfo_1E4.unk8`/`unkD` is a second
list that is empty in all 287 entries and whose two accessors have zero callers anywhere. `RoomProps.unk26`
is two zero bytes never touched by any instruction. `LevelInfo_1E8_14`'s 14 trailing `filler26` bytes
contain structured, non-zero data (own node index followed by an 0xFF-terminated neighbour list) that
no decompiled or disassembled code reads. And a large fraction of this scope — `unk02`, `unk04`,
`unkXmod_06`, `unkXmod_08`, `priorityFlags` bit 1 — governs a *third* background layer that the retail
game never instantiates (`objectList2Idx` is 0xFFFF in all 983 `gRoomProps` slots), so those readings
rest on code alone and cannot be corroborated by data.

## Layer map (needed to read the rest of this document)

Established by `wip/code.c:67-130` (per-layer `Background` init), `src/code_080023A4.c:159-171`
(`gUnk_082D8D74`, which `Background.unk1C` indexes), and `src/code_080023A4.c:822-905` (which
`gBgScrollRegs` each layer writes):

| `LevelInfo.unk180[]` | source table | `LevelInfo.unkC0[]` | HW BG | scroll source |
|---|---|---|---|---|
| `unk180[0]` | `gForegroundTilemaps[mapDataIdx]` | `unkC0[1]`, `unk1C = i` | **BG3** | `viewportPosition` |
| `unk180[1]` | `gRoomTiledBGs[backgroundIdx]` | `unkC0[2]`, `unk1C = i+4` | **BG0** | `altViewport_34` |
| `unk180[2]` | `gLevelObjLists[objectList2Idx].unk` | `unkC0[0]`, `unk1C = i+8` | **BG2** | `altViewport_24` |

Throughout this document "playfield" = `unk180[0]`/BG3, "background" = `unk180[1]`/BG0, "extra layer"
= `unk180[2]`/BG2. The extra layer is never used: `objectList2Idx == 0xFFFF` in all 983 `gRoomProps`
entries (measured), and `wip/code.c:213-216` therefore always clears `DISPCNT` bit 10 (BG2 enable).

---

# struct SolidityMap

### `struct SolidityMap.unk0`
**Proposed:** `collisionMap`
**Confidence:** high
**Evidence:**
- `wip/code.c:431` — passed as the source of `RLUnCompWram(...)` into `gUnk_02024ED0[levelInfo->unk65E]`,
  so it is a run-length-compressed blob, not live data.
- `src/code_080023A4.c:1273-1279` (`sub_080023E4`) — the destination is indexed
  `gUnk_02024ED0[slot][roomWidth / 16 * y + x]`, i.e. one byte per 16x16-pixel cell, row-major, stride
  `roomWidth/16`.
- `src/code_080023A4.c:2044` and neighbours (`sub_080035F4`) — each byte read out of that array is used
  as `gUnk_082D88B8[value]`, i.e. it is an *index into a terrain-attribute table*, not a bitfield itself.
- Measured: decompressing `gSolidityMaps[1]->unk0` (0x08873678) yields exactly 988 bytes = 76x13 cells,
  which is `gForegroundTilemaps[1]` width/height (152x26 eight-pixel tiles) divided by 2 in each axis.

**Against / unresolved:** nothing. `compressedCollisionMap` would also be defensible; I prefer
`collisionMap` because `RoomTiledBG.tileset` is likewise a compressed pointer without a "compressed"
prefix.

### `struct SolidityMap.unk4`
**Proposed:** `cellRecords`
**Confidence:** high
**Evidence:**
- `wip/code.c:307` — copied verbatim to `levelInfo->unk1E0`.
- `src/code_080023A4.c:1348-1364` (`sub_080025AC`) — `unk1E0` is the head of a list walked with
  `var1.pat1 = (void *) var1.pat1 + var1.pat1->unk04;`, i.e. **`unk04` is the record stride**, and the
  match predicate is `unk01 == gUnk_02024ED0[y*(roomWidth/16)+x] && unk02 == x && unk03 == y` — so each
  record is keyed by *(collision value, cell x, cell y)*.
- `src/code_080023A4.c:589-624` (`sub_08001408`) — applying a matched record writes four `u16` tilemap
  entries (`var1->unk08[0..3]`) into the decompressed tilemap at tile coords `(2*unk02, 2*unk03)` and
  writes `var1->unk10` into the collision map cell. So type-1 records are "replace this 16x16 cell's
  four tiles and its collision value".
- `src/mirror_door.c:112-116`, `src/special_doors.c:100` — type-3 records are read as `.pat3` and their
  `unk08` is handed to `sub_08002BA8(roomId, ...)` as a room id, i.e. type 3 is a door/warp descriptor.
- Measured across all 286 rooms: the list terminates on a record whose `unk00 == 0` and stride `0`;
  record types present are only **1 (9500 records)** and **3 (724 records)**; and for *every* record the
  decompressed collision map at `(unk02, unk03)` equals `unk01`. Record counts per room run 0..~11+.
  For `gSolidityMaps[1]` the list begins at 0x08873708, immediately after the compressed blob, as the
  coordinator measured.

**Against / unresolved:** type 2 (`struct Unk_0888562C_2`, the 8-bit-tilemap variant handled by
`sub_08001408`'s else branch, `src/code_080023A4.c:624-652`) occurs **zero** times in retail data. That
dead branch is the mirror image of `RoomTiledBG.unk1C` being 0 everywhere (below) — the two together
say the engine supports an 8-bit tilemap format that no shipped room uses.

---

# struct ForegroundInfo (and friends)

`gForegroundInfo[pixelDataIdx]` supplies the playfield's *tileset* plus a set of looping tile
animations that overwrite parts of char block 2 each frame. `sub_08003438`
(`src/code_080023A4.c:1980-2003`) does the initial blit; `sub_080021E4`
(`src/code_080023A4.c:1187-1240`) advances it per frame. `levelInfo->unk5FC[i]` is the runtime state
pair `{frameIndex, frameTimer}` for animation `i`.

### `struct ForegroundInfo.unk4`
**Proposed:** `animGfx`
**Confidence:** high
**Evidence:**
- `src/code_080023A4.c:1214` — `void *var4 = fgInfo->unk4 + var3->unkC[var2[0]].unk0;` — it is the base
  address that every animation frame's offset is added to. Byte-granular (`void *` arithmetic).
- `src/code_080023A4.c:1994` — same expression in the initial-load path.
- Measured: for `gForegroundInfo[1]`, `tileset = 0x089C6F98` (LZ77-compressed, decompressed to
  0x06008000 by `wip/code.c:190`) while `unk4 = 0x089CAA68` — a *separate, later, uncompressed* blob.
  Frame offsets are 0x0/0x100/0x200/0x300 into it.

**Against / unresolved:** the declared type is `void *`; `const u8 *` would be more honest but changing
the type is a separate decision.

### `struct ForegroundInfo.unk8`
**Proposed:** `anims`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1210` / `:1992` — `fgInfo->unk8 + i` for `i` in `[0, unkC)`; it is
already typed `const struct ForegroundInfo_8p *`.

### `struct ForegroundInfo.unkC`
**Proposed:** `numAnims`
**Confidence:** high
**Evidence:**
- `src/code_080023A4.c:1206` (`why = fgInfo->unkC;`) and `:1987` — the loop bound over `unk8`.
- Measured across the 34 entries: values 0..9; entry 0 has 0 (no animation) and its `unk8` pointer is
  degenerate, consistent with a count.

### `struct ForegroundInfo_8p.unk0`
**Proposed:** `vramOffset`
**Confidence:** high
**Evidence:**
- `src/code_080023A4.c:1215` / `:1995` — `void *var5 = (void *) 0x6008000 + var3->unk0;` — a byte offset
  from the start of char block 2, which is exactly where `wip/code.c:190` decompresses the tileset.
- Measured: values like 0x5C00, 0x1D60, 0x2160 — all inside a 0x8000-byte char block, all 32-byte
  aligned (i.e. tile-aligned in 4bpp).

### `struct ForegroundInfo_8p.unk4`
**Proposed:** `copySize`
**Confidence:** high
**Evidence:**
- `src/code_080023A4.c:1219` / `:1998` — the third argument of `CpuCopy32(src, dst, size)`, so a byte
  count.
- Measured: 32, 64, 96, 128, ... 1024 — all multiples of 32 (whole 4bpp tiles).

### `struct ForegroundInfo_8p.unk8`
**Proposed:** `stride`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1221-1222` / `:2000-2001` — added to *both* the source and the
destination pointer after each chunk copy. Measured: always 0x400 (= 32 tiles = one row of a 32-tile-wide
char block).

### `struct ForegroundInfo_8p.unkC`
**Proposed:** `frames`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1214` — indexed by the runtime frame counter `var2[0]`; already typed
`const struct ForegroundInfo_8p_Cp *`.

### `struct ForegroundInfo_8p.unk10`
**Proposed:** `numFrames`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1234` — `if (++var2[0] < var3->unk10) continue;` then `var2[0] = 0;`
— a wrap bound on the frame index. Measured: 2, 3, 4 or 8.

### `struct ForegroundInfo_8p.unk12`
**Proposed:** `numChunks`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1218` / `:1997` — the `j` loop bound over the `CpuCopy32` chunks.
Measured: 1 (70 animations) or 2 (7 animations).

### `struct ForegroundInfo_8p_Cp.unk0`
**Proposed:** `gfxOffset`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1214` — added to `ForegroundInfo.unk4`. Measured: monotonically
increasing per frame by exactly `copySize` (e.g. 0x0, 0x100, 0x200, 0x300 for a 0x100 animation).

### `struct ForegroundInfo_8p_Cp.unk4`
**Proposed:** `duration`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1229` — `if ((var2[1] = r0) < var3->unkC[var2[0]].unk4) continue;` —
the per-frame timer is compared against it and reset when it reaches it, so it is a hold time in game
frames. Measured: 1..10.

---

# struct ForegroundPalette (and friends)

`gForegroundPalettes[paletteDataIdx]` supplies the playfield's palette plus looping palette animations.
`sub_08003108` (`src/code_080023A4.c:1847-1904`) drives them. `levelInfo->unk63C[i]` is the runtime
`{frameIndex, frameTimer}` pair.

### `struct ForegroundPalette.unk4`
**Proposed:** `anims`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1858` — `struct ForegroundPalette_4pp *fgPal4p = fgPal->unk4[i];`
for `i` in `[0, unk8)`. It is an array of pointers (declared `struct ForegroundPalette_4pp **`).

### `struct ForegroundPalette.unk8`
**Proposed:** `numAnims`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1854` — `u16 i, why = fgPal->unk8;` is the loop bound. Measured over
the 42 entries: 0 (23 entries), 1 (15), 2 (3), 3 (1).

### `struct ForegroundPalette_4pp.unk0`
**Proposed:** `frames` — an array of pointers to `{ const u16 *palette; u16 duration; }`
**Confidence:** high
**Evidence:**
- `src/code_080023A4.c:1866` / `:1874` / `:1883` — `fgPal4p->unk0.u16ppp[frameIdx][0]` is the source
  palette handed to `LoadBgPaletteWithTransformation` / `DmaCopy16`.
- `src/code_080023A4.c:1895` — `fgPal4p->unk0.u16pp[frameIdx][2]` is the frame's duration.
- Both spellings load the same word `*(u32 *)(unk0 + 4*frameIdx)`; the first then dereferences it as a
  pointer, the second reads a `u16` at +4 of it. So each element points at a two-field record that is
  byte-for-byte `struct Unk_08D6DCAC` (`include/data.h:935`).
- Measured: for `gForegroundPalettes[1]`, `unk0 = 0x08D62DEC` holding pointers 0x0886E300, 0x0886E310,
  0x0886E320 (16 bytes apart), each of which is `{u32 palettePtr, u16 duration}` with durations 2,2,2.
  Durations across all 42 entries: 1, 2, 3, 4, 5, 20, 30 — and **999 three times**, which is exactly the
  sentinel `src/code_080023A4.c:1860` tests for.

**Against / unresolved:** the double-union is only needed because the frame record type is not declared.
Declaring `const struct Unk_08D6DCAC *const *frames;` would remove the union entirely — but that is a
type change, not a rename, so I am flagging it rather than proposing it.

### `struct ForegroundPalette_4pp.unk4[0]`
**Proposed:** `numFrames`
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1900` — `if (++var0[0] < fgPal4p->unk4[0]) continue;` then reset —
a wrap bound on the frame index. Also used as a "disabled" guard at `:1860` (`== 0` skips the animation).
Measured: 4, 5, 6, 7, 33, 49, 65.

### `struct ForegroundPalette_4pp.unk4[1]`
**Proposed:** `paletteOffset` (in colours, into `gBgPalette`)
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1875` — `gBgPalette + fgPal4p->unk4[1]` as the DMA destination
(`gBgPalette` is `u16 *`), and `:1867`/`:1884` pass it as the offset argument to
`LoadBgPaletteWithTransformation` / `sub_0803D21C`. Measured: 65, 38, 65, ... — plain colour indices.
Note this is the same meaning and the same name as the existing `RoomTiledBG.paletteOffset`.

### `struct ForegroundPalette_4pp.unk8`
**Proposed:** `paletteSize` (in colours / hwords)
**Confidence:** high
**Evidence:** `src/code_080023A4.c:1876` — `DmaCopy16(3, src, dst, fgPal4p->unk8 * 2)`, i.e. the byte
count is twice this, so it counts `u16`s. Same convention and same name as the existing
`RoomTiledBG.paletteSize` (`include/data.h:81`, "in hwords"). Measured: 4.

**Against / unresolved:** `src/code_080023A4.c:1860` guards with `fgPal4p->unk0.u16pp[i][2] == 999`
where `i` is the *animation* index, not the frame index used everywhere else in the same loop. Either
this is an original-source indexing slip that happens to be harmless (all 42 entries have
`numFrames >= numAnims`, so it never reads out of bounds — measured), or the sentinel is deliberately
placed on frame `i`. I could not distinguish these. Whatever name is chosen for `unk0`, this line
should keep a comment.

---

# struct RoomTiledBG

### `struct RoomTiledBG.unk4`
**Proposed:** `animFrameSize` (bytes of tile data per animation frame)
**Confidence:** medium
**Evidence:**
- `src/bg.c:531` — `r5 = r3->unk4;` and then `:536` / `:538` use it as the per-frame stride
  `base + tilesetSize + (frame - 1) * r5`, and `:543` passes it as the transfer size
  `gUnk_03002EC0[...].unk8 = r5`.
- `src/bg.c:523-525` — the struct being read is `gUnk_03002E60[ip->unk1C].x`, and
  `src/code_080023A4.c:159-171` shows `gUnk_082D8D74` (one of the two things `gUnk_03002E60` points at,
  `wip/code.c:48` and `:161`) is exactly `&gCurLevelInfo[player].unk180[layer]` — so this code really
  does read `RoomTiledBG`.

**Against / unresolved:** **`unk4` is 0 in every entry of `gRoomTiledBGs` (36) and every entry of
`gForegroundTilemaps` (287)** — measured. So this path never executes for room layers and the reading is
code-only. Separately, `src/bg.c:536` adds `tilesetSize` to a byte address while `include/data.h:78`
comments it "in tiles"; the data files say `.tilesetSize = 900` (`src/data/room_tiled_bgs/cabbage_cavern_1.c`),
which is not a multiple of 32, so the comment is probably wrong — but since the animation path is dead I
cannot settle it, and `tilesetSize` is outside this scope anyway.

### `struct RoomTiledBG.unk6`
**Proposed:** `numAnimFrames`
**Confidence:** medium
**Evidence:**
- `src/bg.c:527` — `if (r3->unk6 && ...)` — zero disables the animation entirely.
- `src/bg.c:529-530` — `if (r3->unk6 <= ++ip->animFrameCounter) ip->animFrameCounter = 0;` — a wrap
  bound on the already-named `Background.animFrameCounter` (`include/bg.h:26`).

**Against / unresolved:** same as `unk4` — 0 in all 323 room-layer entries. Code-only reading.

### `struct RoomTiledBG.unk7`
**Proposed:** `animDelay` (game frames between animation steps)
**Confidence:** medium
**Evidence:** `src/bg.c:527-528` — `if (... r3->unk7 <= ++ip->animDelayCounter) { ip->animDelayCounter = 0; ... }`
— a threshold on the already-named `Background.animDelayCounter` (`include/bg.h:27`).

**Against / unresolved:** same as above — 0 everywhere in room data. Note the comparison is `<=` against
a *pre-incremented* counter, so `unk7 == 0` and `unk7 == 1` both mean "every frame"; only `unk6` actually
gates the feature.

### `struct RoomTiledBG.unk1C`
**Proposed:** split the `u32` into `u16 useByteTilemap; u16 roomId;`
**Confidence:** high for the split and for `roomId`; medium for `useByteTilemap`
**Evidence:**
- The split is forced by the ROM: disassembling `sub_080015A8` gives `ldrh r0, [r0, #28]` at file offset
  0x15E6 — offset 0x1C is loaded as a **halfword**, so 0x1E is a separate field. (`src/code_080023A4.c:666`
  reads it as `u16 unk1C = ...->unk1C;`, which happens to compile to the same load.)
- `roomId` at 0x1E: measured, `gForegroundTilemaps[mapDataIdx][0x1E] == roomId` for **286 of 286** real
  rooms. For `gRoomTiledBGs` the values are 100, 101, 102, 103, 130, 151, ... 997, 998, 999 — plausible
  room ids, one per background. `src/data/room_tiled_bgs/cabbage_cavern_1.c` writes
  `.unk1C = 0x1fe0000`, i.e. high half = 0x1FE = 510, and that background is `gRoomTiledBGs[19]`, whose
  only user is room 510. This mirrors the existing `roomId` tail field on `struct ForegroundInfo`
  (`include/data.h:867`) and `struct ForegroundPalette` (`include/data.h:890`) — an authoring annotation
  the engine never reads.
- `useByteTilemap` at 0x1C: `src/code_080023A4.c:663-687` (`sub_080015A8`) reads a 2x2 block of tilemap
  entries out of `gUnk_02028EE0` as `u16 *` when it is 0 and as `u8 *` when it is non-zero, from the same
  element index. That is a tilemap element-width switch and nothing else.

**Against / unresolved:** the field is **0 in all 287 `gForegroundTilemaps` entries and all 36
`gRoomTiledBGs` entries** (measured), so the `u8` path is dead. `sub_080015A8` reads the *table* entry,
not `levelInfo->unk180[0]`, so no runtime patch can turn it on. The name is therefore a reading of dead
code — but it is corroborated by the identically-dead `Unk_0888562C_2` record type (see
`SolidityMap.unk4` above), which is the write-side counterpart of the same 8-bit tilemap format. If you
would rather not name dead code, `unk1C` -> `tilemapFormat` is the weaker, safer alternative.

---

# struct RoomProps

Measured facts that frame this whole section: 983 slots, of which **286 are real rooms** and 697 are
all-0xFFFF filler. `mapDataIdx == solidityMapIdx == objectListIdx == unk22 == doorsIdx` in 286/286.
`objectList2Idx == 0xFFFF` in 983/983. `unk26` is `{0, 0}` in 983/983. Only five distinct
`priorityFlags` values occur: 0x79 (262 rooms), 0x19 (11), 0x39 (9), 0x01 (3), 0x25 (1, room 919).

### `struct RoomProps.unk02` / `struct RoomProps.unk04`
**Proposed:** `extraBgScrollOffsetX` / `extraBgScrollOffsetY` (pixels)
**Confidence:** medium
**Evidence:**
- `src/code_080023A4.c:881-882` — `altViewport_24.x = (viewportPosition.x >> 4) * (unk_S16Vec2_B4.x >> 4) + unk02 * 0x100;`
  `altViewport_24` is 8.8 fixed point (`:895-896` shifts it right by 8 to get `Background.scrollX`), so
  `* 0x100` makes `unk02` a whole-pixel constant added to the extra layer's horizontal scroll.
- `src/code_080023A4.c:889` — `altViewport_24.y -= unk04 * 0x100;` — same, vertical, *subtracted*
  (that layer's y is measured up from the bottom, see `:885-886`).
- `altViewport_24` drives `gBgScrollRegs[2]` (`src/code_080023A4.c:898-899`), i.e. HW BG2, i.e. the extra
  layer — see the layer map above.

**Against / unresolved:** both are **0 in all 286 real rooms** (measured), and the extra layer never
exists, so this is a code-only reading of a feature the shipped game does not use. The `* 0x100`
identifies the unit unambiguously, but the *sign convention* on `unk04` (subtract) is only meaningful if
you accept the bottom-anchored y reading.

### `struct RoomProps.unkXmod_06` / `struct RoomProps.unkXmod_08`
**Proposed:** `extraBgParallaxX` / `extraBgParallaxY` (8.8 ratio; 0x100 = moves 1:1 with the camera)
**Confidence:** medium
**Evidence:**
- `wip/code.c:380-381` — copied into `levelInfo->unk_S16Vec2_B4.{x,y}` at room load.
- `src/code_080023A4.c:881` — used as `(viewportPosition.x >> 4) * (unk_S16Vec2_B4.x >> 4)`, which is
  `viewportPosition.x * ratio / 256`. Since `viewportPosition` and `altViewport_24` are both 8.8, the
  value is a 8.8 multiplier: 0x100 = 1.0.
- **This is why every room has 0x100** (measured, 286/286): 1.0 is the identity, and no shipped room
  parallaxes this layer. That reconciles the coordinator's measurement with the reading rather than
  contradicting it — the reading *predicts* 0x100 as the neutral value.
- `src/code_080023A4.c:876-878` — when `priorityFlags & 2` is set the same two fields are instead *added
  to* `altViewport_24` every frame, i.e. they become a per-frame scroll velocity. Bit 1 is never set in
  retail data, so this second meaning is unexercised.

**Against / unresolved:** the field is dual-purpose (ratio vs. per-frame delta) selected by
`priorityFlags` bit 1, so *no single name is correct*. If you want one name that covers both,
`extraBgScrollParam{X,Y}` is honest and ugly; I would rather name it for the branch that is 286/286
live. Also the existing name `unkXmod_06` is misleading — `unkXmod_08` is the *Y* component, not a
second X.

### `struct RoomProps.unk0A` / `struct RoomProps.unk0C`
**Proposed:** `bgScrollOffsetX` / `bgScrollOffsetY` (pixels)
**Confidence:** medium-high for `unk0C`, medium for `unk0A`
**Evidence:**
- `src/code_080023A4.c:847-848` — `altViewport_34.x = (viewportPosition.x >> 4) * (unkB8 >> 4) + unk0A * 0x100;`
- `src/code_080023A4.c:855` — `altViewport_34.y -= unk0C * 0x100;`
- `altViewport_34` drives `gBgScrollRegs[0]` (`src/code_080023A4.c:865-866`), i.e. HW BG0, i.e. the
  background layer from `gRoomTiledBGs[backgroundIdx]`.
- `wip/code.c:299-301` — the load-time seed is
  `altViewport_34.y = ((unk180[1].height << 3) - (unk0C + 0xA0)) << 8`, i.e. "background height in
  pixels, minus one screen (0xA0 = 160), minus `unk0C` pixels" — the same pixel unit and the same
  subtract-from-bottom convention as the per-frame formula. Two independent sites agreeing on the unit.
- Measured: `unk0C` is 0 in 282 real rooms and **80 in exactly rooms 910, 912, 914 and 916** — a small
  set of related rooms nudging their background up by 80 px. That is precisely what a per-room constant
  offset looks like.

**Against / unresolved:** `unk0A` is **0 in all 286 real rooms**, so only `unk0C` is corroborated by
data. Worse, the load-time seed for x disagrees with the per-frame formula: `wip/code.c:298` does
`altViewport_34.x = gRoomProps[room].unk0A;` with **no `<< 8`**, while `src/code_080023A4.c:848` uses
`unk0A * 0x100`. I verified this against the ROM — `asm/code.s:1005-1006` is
`ldrh r0, [r2, #0xa]` / `str r0, [r7, #0x34]`, a raw halfword store — so `wip/code.c` is faithful and the
inconsistency is in the original code. It is invisible because `unk0A` is always 0 and the value is
recomputed on the next frame anyway. If you port this, keep the bug.

### `struct RoomProps.unk0E` / `struct RoomProps.unk10`
**Proposed:** `bgParallaxX` / `bgParallaxY` (8.8 ratio; `-1` = derive automatically)
**Confidence:** high
**Evidence:**
- `wip/code.c:333-357` — at room load, when `priorityFlags & 4` is clear, `levelInfo->unkB8` takes
  `unk0E` unless `unk0E == -1`, in which case it is computed as
  `((bgWidthTiles << 11) - 0xF000) / 0x870` = `((bgWidthPx - 240) << 8) / 0x870`. The `<< 8` in the
  numerator is what makes the result 8.8. `unkBA`/`unk10` is the same with `0xA000` (= 160 << 8) and
  `0x5A0`; note `0x870 : 0x5A0 = 240 : 160`, i.e. the two divisors are the screen dimensions times 9.
- `src/code_080023A4.c:847` / `:851-852` — `unkB8`/`unkBA` are then used as
  `(viewportPosition >> 4) * (unkB8 >> 4)`, i.e. `camera * unkB8 / 256`. 0x100 = 1:1.
- Measured: `unk0E` is `-1` in 87 real rooms and a small positive number (17, 21, 32, 42, 46, 64, 128,
  147, ...) in 199; `unk10` is `-1` in 264 rooms and 0, 17, 30, 58, 97, 98 or 106 in 22. Values well
  under 0x100 mean "background moves slower than the camera" — textbook parallax.
- `src/code_080023A4.c:842-844` — when `priorityFlags & 4` *is* set, the same two fields are added to
  `altViewport_34` every frame instead (per-frame velocity).

**Against / unresolved:** dual-purpose, same as `unkXmod_06/08`. Only room 919 sets bit 2, and there
`unk0E == unk10 == 0` (measured), so the "velocity" mode is really being used to *freeze* the background
scroll so that `sub_08003108`'s room-919 special case can drive `altViewport_34.y` directly
(`src/code_080023A4.c:1965`). Also `wip/code.c:334-335` still copies them into `unkB8`/`unkBA` in that
mode, where they are never read — harmless dead assignment. `0x870` and `0x5A0` are magic and I cannot
explain the factor 9.

### `struct RoomProps.priorityFlags` — bit meanings
**Proposed:** rename the field to `roomFlags`; only bit 0 has anything to do with priority.
**Confidence:** per-bit, below.
**Method note:** I enumerated every reference to `gRoomProps` in `src/`, `wip/`, `include/` *and* the
remaining `asm/*.s`, and inspected the mask at each load of offset 0x12. The list below is exhaustive.

| bit | mask | meaning | conf. | clear in |
|---|---|---|---|---|
| 0 | 0x01 | BG priority swap between playfield (HW BG3) and extra layer (HW BG2) | high | never (286/286 set) |
| 1 | 0x02 | extra layer scrolls by accumulating `unkXmod_06/08` instead of camera parallax | high | always (0/286 set) |
| 2 | 0x04 | background layer scrolls by accumulating `unk0E/unk10` instead of camera parallax | high | all but room 919 |
| 3 | 0x08 | Kirby-call (cell phone) allowed here; also drives the HUD phone indicator | high | 919, 980, 981, 982 |
| 4 | 0x10 | Kirby-call allowed here, second code path | high | 919, 980, 981, 982 |
| 5 | 0x20 | **never read by any code** | — | 801, 803, 804, 805, 950, 951, 955, 956, 957, 969, 970, 980, 981, 982 |
| 6 | 0x40 | gates the second summon action `sub_08056E40` | high | 801, 803-805, 909-916, 918, 919, 950, 951, 955-957, 969, 970, 980-982 |
| 7-15 | | never set in a real room, never read | — | always |

**Evidence:**
- bit 0: `wip/code.c:221-228` — set: `gBgCntRegs[3] |= 2; gBgCntRegs[2] |= 1;` clear: `gBgCntRegs[3] |= 1; gBgCntRegs[2] |= 2;`
  after both were masked with `&= 0xFFFC` at `:218-219`. `BGxCNT` bits 0-1 are the priority field, lower =
  drawn in front. So set means the extra layer draws *in front of* the playfield.
- bit 1: `src/code_080023A4.c:876-878`.
- bit 2: `src/code_080023A4.c:842-844` and `wip/code.c:333-335`.
- bit 3: `src/kirby.c:7059` — `!(gUnk_0203AD20 & 2) && gRoomProps[roomId].priorityFlags & 8 && ...` guarding
  `sub_08056C2C(kirby)` on the branch reached when `kirby->battery` is set; `src/kirby.c:13076-13122`
  shows `sub_08056C2C` ends in `PlaySfx(..., SE_KIRBY_CALL)`. HUD side: `wip/code_08032E98.c:2829-2832`
  (`priorityFlags & 8 ? sub_08036194() : sub_080361B0()`), `:2879` (cached into `p->unkB`), `:3062-3068`
  (re-checked and the HUD toggled when it changes). Also `asm/code_08032E98.s:197`, `:852`, `:960`,
  `:1110`, `:1133`, `:1856`, `:2434`, `:4326` — all `movs r0, #8`.
- bit 4: `src/kirby.c:7038` and `:7049` — the two `gUnk_02021580 < gUnk_0203AD44` branches, both leading to
  `sub_08056C2C`; `wip/code_08032E98.c:2966`; `asm/code_08032E98.s:493`, `:4479` (`movs r0, #0x10`).
- bit 5: no site. `asm` masks at offset 0x12 are only 1, 4, 8 and 0x10; C sites are only 1, 2, 4, 8, 0x10
  and 0x40.
- bit 6: `src/kirby.c:7119-7123` (and identical guards at `:7256`, `:7389`, `:7510`, `:7630`, `:7904`,
  `:7968`, `:8031`, `:8135`, `:8269`, `:8405`, `:8498`, `:8582`, `:8650`, `:8708`) —
  `kirby->unk11A & 0x200 && ... && priorityFlags & 0x40 && ability != MINI` -> `sub_08056E40(kirby)`.
  `src/kirby.c:13125-13163` shows `sub_08056E40` is a near-copy of `sub_08056C2C`'s non-paused branch,
  also ending in `PlaySfx(..., SE_KIRBY_CALL)`, but additionally setting `unkC |= 0x40000` and not
  consuming battery.

**Against / unresolved:**
- Bit 5 is the honest failure here. It is *deliberately* cleared in 14 rooms — 801/803/804/805 (hub) and
  950-982 (minigames/credits) — which is exactly the pattern of a real gameplay restriction, and it
  co-varies with bit 6 but is not identical to it (bit 6 is additionally clear in 909-919). Either the
  reader was removed late in development, or it lives in code that references `gRoomProps` through a
  pointer I could not trace. Static reading cannot settle it; a save-state watchpoint on the field could.
- Bits 3 and 4 both gate `sub_08056C2C` and are cleared in exactly the same four rooms, so the *data*
  cannot separate them; only the two distinct call sites can. `kirby->unk11A` is commented "newly pressed
  this frame" (`include/kirby.h:88`) but is actually assigned from `movementState`
  (`src/kirby.c:6485`), so I could not identify which physical button 0x100 vs 0x200 is, and therefore
  cannot say what distinguishes the bit-3/4 summon from the bit-6 summon in gameplay terms. Do not guess
  in the final names.
- `priorityFlags` is a bad name for a field where five of the seven live bits are not about priority. If
  you rename it, `roomFlags` with a set of `ROOM_FLAG_*` constants is the natural shape.

### `struct RoomProps.unk22`
**Proposed:** `navGraphIdx`
**Confidence:** high
**Evidence:**
- `wip/code.c:309` — `levelInfo->unk1E8 = gUnk_08D63C28[gRoomProps[...].unk22];`
- `wip/code.c:432` — `RLUnCompWram(gUnk_08D63C28[gRoomProps[room].unk22]->unk10, gUnk_02026D60[slot]);`
- `src/code_0800ECAC.c:39` and `:65` — `r5->unkB4 = gUnk_08D63C28[rp->unk22]->unk0[playerId];`
- See the `gUnk_08D63C28` section below for what that table is. The field naming convention in this
  struct is `<what the table holds>Idx`, so it should be named after the table.

**Against / unresolved:** the *value* carries no independent information — it equals `mapDataIdx`,
`solidityMapIdx`, `objectListIdx` and `doorsIdx` in 286/286 rooms (measured). Five fields, one number.
That is worth a comment on the struct, but it does not change what each field selects, and a port that
collapses them will break if any romhack ever separates them.

### `struct RoomProps.unk26`
**Proposed:** *nothing.* This is structure padding.
**Confidence:** high (as a negative result)
**Evidence:**
- Measured: both bytes are 0 in all 983 entries, real and filler alike.
- No instruction anywhere reads offset 0x26 or 0x27 of a `gRoomProps` element: I scanned every
  `=gRoomProps` literal pool reference in `asm/*.s` and its following 25-30 instructions, and every
  `gRoomProps[...]` expression in `src/` and `wip/`.
- The struct is 0x28 bytes and the last real field ends at 0x26, so two bytes of trailing padding is the
  expected shape.

**Against / unresolved:** `padding26` or `filler26` (matching this codebase's existing `filler` naming,
e.g. `include/data.h:139`) would be the mechanical choice. It is *conceivable* that these are real fields
zeroed in the shipped data, but nothing distinguishes that from padding, and I would not name them.

---

# Tables

### `gForegroundTilemaps`
**Proposed:** keep the name. It is accurate; the useful change is a comment, not a rename.
**Confidence:** high (for the facts, not for a rename)
**Evidence:**
- 287 entries (`data/data_2.s:867` onward, counted). Indexed by `RoomProps.mapDataIdx`.
- `wip/code.c:248` — copied 0x20 bytes into `levelInfo->unk180[0]`, which is the **playfield** layer:
  `wip/code.c:256-257` derives `roomWidth = unk180[0].width << 3` and `roomHeight = unk180[0].height << 3`
  from it, so `width`/`height` are in 8-pixel tiles and this struct defines the room's size.
- `wip/code.c:433-434` — `unk180[0].tilemap` is LZ77-decompressed into `gUnk_02028EE0[slot]` and the
  pointer is repointed at the RAM copy, so the tilemap is the only payload.
- Measured: in all 287 entries `tileset`, `tilesetSize`, `palette`, `paletteOffset` and `paletteSize` are
  **zero** — the playfield's tiles come from `gForegroundInfo[pixelDataIdx]` (`wip/code.c:190`) and its
  palette from `gForegroundPalettes[paletteDataIdx]` (`wip/code.c:193`). This confirms the existing
  header comment at `include/data.h:94`, and it is a fact about *this table*, not about `struct RoomTiledBG`
  — `gRoomTiledBGs` populates all of them normally.
- Measured: `[0x1E]` is the room id, 286/286 (see `RoomTiledBG.unk1C`).

**Against / unresolved:** "Foreground" is a slightly odd word for the layer Kirby stands on, but it is
consistent with `gForegroundInfo` / `gForegroundPalettes`, which supply the same layer's tiles and
palette. Renaming one without the other would be worse than leaving all three.

### `gUnk_08D63C28`
**Proposed:** `gRoomNavGraphs`, and `struct LevelInfo_1E8` -> `struct NavGraph`
**Confidence:** high
**Evidence:** see the field entries below. In one line: it is the per-room navigation graph that the
AI-controlled Kirbys path on, plus their per-player behaviour script.
- 287 entries, 0x47C bytes (`data/data_2.s:860-861`). Indexed by `RoomProps.unk22`.

### `struct LevelInfo_1E8.unk0`
**Proposed:** `scripts[4]` — one AI bytecode script per player id
**Confidence:** high
**Evidence:**
- `src/code_0800ECAC.c:39` / `:65` — `unkB4 = gUnk_08D63C28[rp->unk22]->unk0[a1];` where `a1` is the
  player id (`sub_0800ECAC(u8 a1, ...)` at `:14`, `gUnk_02038590[a1]`).
- `src/code_0800ECAC_2.c:2155` — `switch (a1->unkB4[a1->unkB8])` with cases 0..0xA and 0xFFFF: a
  dispatch table over an opcode stream, `unkB8` being the program counter.
- `src/code_0800ECAC_2.c:4435-4441`, `:4461-4467` etc. — each handler does `++a1->unkB8; operand = a1->unkB4[a1->unkB8]; ++a1->unkB8;`
  i.e. variable-length instructions with inline `u16` operands.
- `src/code_0800ECAC_2.c:4492` — `a1->unkB8 = ++r1 + a1->unkB4[a1->unkB8];` — opcode 8 is a relative jump.
- `src/code_0800ECAC_2.c:4431-4433` — opcode 0xFFFF sets `flags |= 0xC` (halt).
- Measured: for `gUnk_08D63C28[0]` the four pointers are 0x088D6494/64A4/64B4/64C4 — 16 bytes apart,
  i.e. four short, distinct scripts.

**Against / unresolved:** `Unk_02038590.unkB4` is declared `const u16 *` with a `// TODO: type` comment;
whether the stream is genuinely `u16`-wide or `u8` with `u16` operands I did not verify against the ROM
loads. The 0xFFFF terminator argues for `u16`.

### `struct LevelInfo_1E8.unk10`
**Proposed:** `nodeMap` — RL-compressed, one node index per 16x16 cell
**Confidence:** high
**Evidence:**
- `wip/code.c:432` — `RLUnCompWram(gUnk_08D63C28[...]->unk10, gUnk_02026D60[levelInfo->unk65E]);` —
  decompressed alongside the collision map into a parallel array.
- `src/code_080023A4.c:1387-1393` (`sub_080026A8`) — read as
  `gUnk_02026D60[slot][roomWidth / 16 * y + x]`: identical indexing to the collision map, so identical
  geometry.
- `src/code_0800ECAC.c:76-89` (`sub_0800EEBC`) — converts an object's position to cell coords and returns
  that byte.
- `src/code_0800ECAC_2.c:2297-2303` — `a1->unk1A = sub_0800EEBC(&kirby->...);` and then
  `a1->unk3F = ...->unk14[a1->unk1A].unk0[a1->unk1B];` — the byte is used directly as an index into the
  node array. That is what makes it a node map and not a second collision map.
- **Measured, room 803:** decompressing `gUnk_08D63C28[1]->unk10` yields 988 bytes — exactly the same
  size as the collision map for that room (76x13 cells) — containing values 0..19, against a declared
  node count (`unk18`) of **20**. No value is out of range. Spot-checking rows shows large contiguous
  runs, i.e. the room is partitioned into ~20 regions.

**Against / unresolved:** nothing. This settles the coordinator's open item.

### `struct LevelInfo_1E8.unk14`
**Proposed:** `nodes`
**Confidence:** high
**Evidence:** `src/code_0800ECAC_2.c:452` (`const struct LevelInfo_1E8_14 *r8 = r4->unk14, *r9 = &r8[a2];`),
`:2301`, `:2303`, `:3072` — indexed by node ids that come out of the node map.

### `struct LevelInfo_1E8.unk18`
**Proposed:** `numNodes`
**Confidence:** high
**Evidence:**
- `src/code_0800ECAC_2.c:463` — `for (++r5; r5 < r4->unk18; ++r5)` and `:470` — the loop bound when
  picking a random reachable destination node.
- Measured over all 287 entries: values 2..32, and **the maximum is exactly 32 = 0x20**, matching the
  declared `unk0[0x20]` adjacency row width in `struct LevelInfo_1E8_14`. Independent confirmation that
  both the count and the array size are right.

### `struct LevelInfo_1E8_14.unk0`
**Proposed:** `nextHop[0x20]` — `nextHop[dest]` is the neighbouring node to move to in order to reach
`dest` from this node; 0xFF means unreachable
**Confidence:** high
**Evidence:**
- `src/code_0800ECAC_2.c:2303` / `:2318` / `:2740` / `:4570` / `:5423` —
  `a1->unk3F = ...->unk14[a1->unk1A].unk0[a1->unk1B];` where `unk1A` is the node Kirby is *standing in*
  (from the node map, `src/code_0800ECAC_2.c:2297`) and `unk1B` is the chosen *destination* node (`:2315`). `unk3F` is then
  used as the immediate target: `src/code_0800ECAC_2.c:488-491` reads `unk14[a1->unk3F].unk20/.unk22` as
  the position to steer toward. That is a next-hop routing matrix.
- `src/code_0800ECAC_2.c:464` — `if (r9->unk0[r5] != 0xFF ...)` — 0xFF is the "no route" sentinel, tested
  when filtering candidate destinations.
- Measured, `gUnk_08D63C28[0]` (20 nodes... 11 in this entry): node 0's row is
  `00 01 02 01 01 02 0a 0a 0a 0a 0a` followed by 0xFF padding to 32. `nextHop[0] == 0` (self),
  `nextHop[10] == 10` (direct neighbour), `nextHop[3] == 1` (route via node 1). Exactly the shape of a
  precomputed all-pairs next-hop table.

### `struct LevelInfo_1E8_14.unk20` / `.unk22`
**Proposed:** `x` / `y` (pixels)
**Confidence:** high
**Evidence:**
- `src/code_0800ECAC_2.c:2301-2302` — `a1->unk7C = r5->unk14[a1->unk1B].unk20 << 8;` and
  `a1->unk80 = ... .unk22 << 8;` — shifted left 8 to become an 8.8 world coordinate target.
- `src/code_0800ECAC_2.c:490` — `a1->unk94 = r2->unk20 - (kirby->x >> 8);` — differenced directly
  against a pixel-space position.
- `src/code_0800ECAC_2.c:465-467` — bounded against `li->unk_S32Vec2_6C`/`_74` shifted right by 8, i.e.
  the level's pixel-space limits.
- Measured, `gUnk_08D63C28[0]`: (248, 224), (184, 224), (312, 224), (248, 288), (88, 272), (408, 272),
  (40, 176), (456, 176), (56, 64), (440, 64), (248, 64) — a plausible scatter of waypoints in a room.

### `struct LevelInfo_1E8_14.unk24`
**Proposed:** `arriveRadius` (pixels)
**Confidence:** medium
**Evidence:**
- `src/code_0800ECAC_2.c:3072-3078` — `v22 = ...->unk14[a1->unk3F].unk24;` then
  `else if (a1->unk1A == 0xFF || v22 >= a1->unk98) return;`, where `a1->unk98` is
  `max(|dx|, |dy|)` in pixels to the current target node (`src/code_0800ECAC_2.c:495`). So it is a distance threshold in the
  same units, and being *inside* it suppresses the following action.
- Measured, `gUnk_08D63C28[0]`: 32 for every node.

**Against / unresolved:** it is 32 in every node of the entry I dumped; I did not scan all 287 entries for
variation, so I cannot rule out that it is a global constant stored per-node. And it is read at exactly
one site, which suppresses a state transition rather than declaring "arrived" — `actionSuppressRadius`
would be more literal and less readable. Medium, not high.

### `struct LevelInfo_1E8_14.filler26`
**Proposed:** *nothing*, but it is **not** filler.
**Confidence:** high that it is structured data; no confidence at all in what it means.
**Evidence:**
- Measured, `gUnk_08D63C28[0]`, bytes 0x26-0x33 of each node: byte 0x28 is always the node's **own
  index** (node 0 -> 0x00, node 1 -> 0x01, ... node 10 -> 0x0A, checked for all 11 nodes), followed by a
  short 0xFF-terminated list of other node indices (node 0: `03 01 02 0A`; node 10: `04 08 09 06 07`),
  then zeros. Bytes 0x26, 0x27, 0x32 and 0x33 are zero.
- No decompiled C reads past `unk24`, and I found no `asm/` access at those offsets.

**Against / unresolved:** the neighbour-list reading does not quite work — node 0's list contains 3,
whose next-hop from node 0 is 1, so it is not the set of *direct* neighbours. It may be an authoring-time
adjacency list that the next-hop matrix was generated from, left in the ROM. Since nothing reads it, this
is unknowable from static reading; recording the layout is more useful than naming it. If you want a
name, `unk26` (not `filler26`) at minimum, so nobody assumes it is safe to drop in a port.

### `gUnk_08D640A4`
**Proposed:** `gRoomDoorLists`, and `struct Unk_08930E5C` -> `struct RoomDoorList`
**Confidence:** medium
**Evidence:**
- 287 entries, 0x47C bytes (`data/data_2.s:863-864`). Indexed by `RoomProps.doorsIdx`, the same index
  that keys `gVisitedDoors` (`src/code_080023A4.c:1577`, `:1582`).
- `src/code_080023A4.c:1587` (`sub_08002AAC`) — returns `gUnk_08D640A4[...]->unk04` as a plain count.
- `src/code_080023A4.c:1593-1602` (`sub_08002AD0`) — popcounts the low `unk04` bits of
  `gVisitedDoors[doorsIdx]`. So `unk04` is the number of entries in the list and the bitmask has one bit
  per entry.
- `src/code_080023A4.c:1607-1622` (`sub_08002B30`) — walks `unk00.pat1` records (`unk01` is the stride
  here, unlike `SolidityMap.unk4`), matching `unk00 == 1 && unk04 == x && unk06 == y`, and sets bit `i`.
  Its only caller is `src/chest.c:113`, `sub_08002B30(chest->roomId, chest->object->x, chest->object->y)`
  — so type-1 entries are **treasure chests**, keyed by position.
- `src/code_080023A4.c:1625-1660` (`sub_08002BA8` / `sub_08002C18`) — walks `unk00.pat2` records matching
  `unk00 == 2 && unk04 == arg1 && unk06 == arg2 && unk07 == arg3`; callers are `src/mirror_door.c:116`,
  `src/special_doors.c:100` and `:326`, `src/special_hub_mirror.c:37`, passing a `pat3` door record's
  `unk08/unk0A/unk0B`. So type-2 entries are **doors**, keyed by destination.
- `src/pause_area_map.c:2920-2925` — a room shows as `AREAMAP_ROOM_COMPLETED` when
  `sub_08002AAC(roomId) == sub_08002AD0(roomId)`, i.e. when every entry in this list has been collected
  or used. `src/code_080023A4.c:1577` reads bit 15 of the same word as "room visited".

**Against / unresolved:** the list is not only doors — chests are in it too, and the area map treats the
whole thing as a per-room completion checklist. `gRoomDoorLists` follows the existing `doorsIdx` field
name and `gVisitedDoors` global, which is why I propose it, but `gRoomProgressLists` describes the data
better. Pick one and make `doorsIdx` and `gVisitedDoors` agree with it. I did not decode `pat2`'s
`unk04`/`unk06`/`unk07` semantics beyond "they are matched against a door record's fields", so the record
structs themselves stay unnamed.

---

# struct LevelInfo_1E4 (`gLevelObjLists`)

287 entries (`data/data_2.s:571` onward, counted). Indexed by `RoomProps.objectListIdx`.

### `struct LevelInfo_1E4.unk0`
**Proposed:** `objects`
**Confidence:** high
**Evidence:** `wip/code.c:308` copies it to `levelInfo->objlistPtr`; `src/code_080023A4.c:5880`,
`:5942`, `:6002` walk it as `for (; obj->unk1 != 0; obj = (void *) obj + obj->unk1)` and hand each entry
to `CreateObject` / `gSpawnFuncTable1[obj->type](obj, playerId)`. Already typed `const struct Object *`
and already described as the object list.

### `struct LevelInfo_1E4.unk4` and `.unkC`
**Proposed:** `objectTypes` and `numObjectTypes`
**Confidence:** high
**Evidence:**
- `src/code_080023A4.c:1396-1404` — `sub_080026F8(roomId)` returns `unkC`; `sub_0800271C(roomId, i)`
  returns `unk4[i]`. Classic count + array pair.
- **Measured:** for all **287 of 287** entries, the `unkC` bytes at `unk4` are exactly the sorted,
  deduplicated set of `Object.type` values (`include/data.h:261`) appearing in that entry's object list.
  Not a partial match, not a superset — an exact match in every case. Examples: entry 0 -> `{101, 140,
  142, 152}` with `unkC == 4`; entry 3 -> `{5, 12, 17, 19, 26, 111, 127, 129, 145, 151}` with `unkC == 10`.
  `unkC` ranges 0..13.

**Against / unresolved:** the *purpose* is not established — the two accessors have **no callers
anywhere** (grepped `src/`, `wip/`, `include/`, `asm/`; only the definitions at
`src/code_080023A4.c:1396`/`:1401` and the declarations at `include/functions.h:41-42`). The obvious
guess is a graphics-preload manifest, but that is a guess and I am not putting it in the name. The
*content* is measured beyond doubt; the *use* is not.

### `struct LevelInfo_1E4.unk8` and `.unkD`
**Proposed:** *nothing.* Unknown, and unknowable from static reading.
**Confidence:** high (as a negative result)
**Evidence:**
- `src/code_080023A4.c:1406-1414` — `sub_08002750(roomId)` returns `unkD`, `sub_08002774(roomId, i)`
  returns `unk8[i]`: structurally identical to the `unk4`/`unkC` pair.
- **Measured: `unkD` is 0 in all 287 entries**, so the array is always empty. `unk8` points immediately
  after the `unk4` array in every entry, so it is a real second array that was simply never populated.
- Both accessors have **no callers anywhere** (same grep as above).

**Against / unresolved:** with an empty array and no readers, nothing constrains the meaning. A plausible
story — that `unk4` lists types for `spawnTable == 1` objects and `unk8` for `spawnTable == 2` — is ruled
out by measurement: **every one of the 3482 objects across all 287 lists has `spawnTable == 1`**, and
`unk4` covers them all. Leave `unk8`/`unkD` as-is, with a comment saying they are empty in retail data.
