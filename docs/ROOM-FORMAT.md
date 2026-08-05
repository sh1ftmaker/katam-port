# The room format

Decoded 2026-08-04 by reading the game's own loader and then checking the
result against the running game. `tools/room_export.py` is the executable form
of this document; if the two disagree, the tool is right, because the tool is
the one that gets checked.

**Status: the room container, its geometry and its collision grid are
decoded and verified. The tilemap and tileset blobs decompress but their
interpretation is not yet established. The door table is not decoded** — it is
being worked separately, see `docs/naming/doors.md`.

---

## 1. What a room is

A room is a 40-byte `struct RoomProps` record and nine indices into nine other
tables. There are 983 record slots at `0x089331AC`; **286 of them are real
rooms**, with ids from 101 to 982. The rest are `0xFFFF` filler — a room id is
an id, not a position in a packed list, and anything that stops at the first
filler entry sees no rooms at all.

```
gRoomProps[roomId]                              0x089331AC, 40 bytes each
  .mapDataIdx      -> gForegroundTilemaps[]     0x08D64520 -> RoomTiledBG
  .backgroundIdx   -> gRoomTiledBGs[]           0x08D64A24 -> RoomTiledBG
  .objectList2Idx  -> gLevelObjLists[]          0x08D637AC -> RoomTiledBG   (!)
  .objectListIdx   -> gLevelObjLists[]          0x08D637AC -> object records (!)
  .solidityMapIdx  -> gSolidityMaps[]           0x08D63330 -> collision
  .doorsIdx        -> gUnk_08D640A4[]           0x08D640A4 -> doors
  .pixelDataIdx    -> gForegroundInfo[]         0x08D6499C
  .paletteDataIdx  -> gForegroundPalettes[]     0x08D63288
  .songIdx            music
  .priorityFlags      per-room behaviour bits, not yet decoded
```

The two lines marked **(!)** are the same table read two different ways.
`gLevelObjLists` is declared as a transparent union of `RoomTiledBG *` and
`LevelInfo_1E4 *` (`include/data.h:853`): `objectListIdx` selects the object
reading, `objectList2Idx` selects the tilemap reading. Any tool that assumes
one meaning produces confident nonsense for the other half of its uses.

### The five indices are always the same number

Measured across all 286 rooms:

```
mapDataIdx == solidityMapIdx == objectListIdx == unk22 == doorsIdx   286 of 286
pixelDataIdx == paletteDataIdx                                       258 of 286
```

So in practice **a room has one content index**, and the five tables are five
parallel arrays keyed by it. The mapping from room id to content index is a
bijection: 286 rooms, 286 distinct indices, no sharing. Object list 4 is the
only one nothing references.

This is a measurement, not a guarantee — the fields are separate in the struct
and the game reads them separately, so a *new* room is free to break the
pattern. But it means the retail game has exactly one notion of "which room's
stuff is this", which is a much simpler thing to permute than five.

---

## 2. Geometry

`struct RoomTiledBG` (0x20 bytes) is fully named in the decompilation. The
**foreground** layer is the one that defines the room's size:

```c
levelInfo->roomWidth  = levelInfo->unk180[0].width  << 3;   /* wip/code.c:256 */
levelInfo->roomHeight = levelInfo->unk180[0].height << 3;
```

so `RoomTiledBG.width`/`.height` are in **8-pixel tiles** and `roomWidth`/
`roomHeight` are in pixels. Collision cells are 16 pixels, so the collision
stride is `roomWidth / 16` = `width / 2` — not `width`, and not `roomWidth / 8`.

Measured over all 286 rooms:

| | |
|---|---|
| widest room | 136 cells — 2176 px |
| tallest room | 103 cells — 1648 px |
| largest collision grid | 1768 bytes |
| **the buffer it must fit in** | **1950 bytes** (`gUnk_02024ED0[slot]`) |

**182 bytes of headroom in the whole game.** That is the hard ceiling on a
custom room: `(width/16) * (height/16) <= 1950` cells. It is not a soft budget
like the 512-tile object heap — the buffer is a fixed-size array and there are
four of them, one per room slot.

---

## 3. Collision — the decoded part

At `0x08D63330`, `gSolidityMaps[idx]` points at an 8-byte `struct SolidityMap`
whose first word is a **run-length compressed** blob. The loader does:

```c
RLUnCompWram(gSolidityMaps[gRoomProps[room].solidityMapIdx]->unk0,
             gUnk_02024ED0[levelInfo->unk65E]);          /* wip/code.c:431 */
```

Decompressed, it is **one `u8` per 16×16 pixel cell, row-major, stride
`roomWidth / 16`**. Reads go through a clamping accessor:

```c
u8 sub_080024F0(struct LevelInfo *li, u16 x, u16 y)      /* code_080023A4.c:1314 */
```

and the byte is then an **index into the metatile attribute table**
`gUnk_082D88B8` at `0x082D88B8`, whose `u32` entries are the terrain bitfields
the physics actually tests. `struct ObjectBase.unk58` caches that word for the
tile an object is standing on, rebuilt every frame.

The attribute bits are **now decoded** — see
`docs/naming/metatile-attributes.md` for the full specification. The word is:

```
 31        28 27      24 23      20 19      16 15                    0
| slope enum | surface   | magnitude | always 0 | property flags       |
   0..12       normal      0..4                   16 bits, 14 live
```

`gUnk_082D88B8` is not ROM-only data — it is **defined in C** at
`src/code_080023A4.c:71`, 256 literal `u32`s, so the readings are checkable
against the values themselves and not only against the test sites.

Note bit `0x1` means **empty**, not solid — it gates whether the per-tile
collision handler runs at all. Solid is `0x200`. An earlier draft of this
document had that backwards.

Measured, and reproduced independently here:

| | |
|---|---|
| table entries | 256, of which **125 non-zero** |
| distinct indices used by level data | **80** |
| non-zero entries **never placed in any room** | **45** |
| high-nibble values present | 0–12 — a 13-value slope enum |

Index 0 is empty space in every room, and its attribute word is exactly `0x1`.
That is three independent routes to the same conclusion: the sweep found index
0 empty everywhere, the table gives it `0x1`, and the test sites read `0x1` as
emptiness.

The 45 unplaced entries are not noise — they are whole features that shipped in
the table and were never used in a level, including all 20 water slopes, all 6
conveyors and all 4 diagonal currents.

Note the buffer is indexed by **room slot**, not player id. Up to four players
can be in different rooms; two players in the same room share a slot
(`wip/code.c:405-430`). Getting that wrong reads another player's level.

### A room, drawn

`tools/room_export.py --room 803 --map`:

```
room 803: 1216x208 px, 76x13 cells, 12 objects
..........................................#######..........................#
..........................................#######..........................#
............................................##.............................#
.................................#####......##......................##...@.#
.........................##.................##........#####.........########
.........................##.................##.............................#
.........................####...............@@..##.........................#
.........................####............#########.........................#
.................##......####..........#############..........##...........#
############################################################################
############################################################################
############################################################################
############################################################################
```

---

## 4. Objects

Already in production use — this is what the randomizer rewrites. See
`docs/RANDO.md`. In summary: `gLevelObjLists[objectListIdx]` read as
`LevelInfo_1E4 *`, whose first word points at a run of `struct Object` records.
Every record in the retail ROM is 36 bytes, but **termination is a zero stride
byte and the walk is `addr += record[1]`**, so the size is per-record data, not
a constant. Byte 12 is the type, and it indexes both `gSpawnFuncTable1`
(constructors) and `gUnk_08351648` (descriptors: tile count, ability,
constructor). 3,482 records across 287 lists; longest list is 30.

---

## 5. Verification

Three levels, in increasing order of what they prove.

**Every room resolves.** `--validate` walks all 286 and reports failures:

```
   286 resolved, collision size == width/16 * height/16
     0 resolved, but the sizes disagree
     0 failed to resolve at all
```

Each room's compressed collision blob declares its own decompressed size in its
header. That size equalling `(width/16) * (height/16)` — computed from a
completely different table, two pointer hops away — is 286 independent
confirmations that the geometry chain is right. A wrong table address does not
survive this.

**It looks like a level.** The ASCII map above has a four-cell floor, floating
platforms and a right-hand wall. Cheap, and it catches transposed axes and
off-by-one strides that size checks pass straight over.

**It matches the running game.** This is the one that counts:

```
DUMP_ROOM=build/room-live.json HOLD=0x10 MASH=600:1:6 \
    node tools/headless_test.js build/katam-node.js <rom> 1300
python3 tools/room_export.py --rom <rom> --check build/room-live.json

check: room 803, 76x13 cells -- all 988 collision bytes match the running game
```

`DUMP_ROOM` reads `gUnk_02024ED0[slot]` out of the live heap after the game has
played into a level, using the addresses from the game's own `linker.ld` and
the struct offsets `platform/port/gba_layout.h` asserts on every build. Reading
a compressed blob out of a ROM and calling the result a collision map is a
claim. 988 bytes that match what the game is actually indexing is evidence.

---

## 6. What is **not** decoded

Stated plainly, because the difference is the point:

- **The tilemap and tileset blobs.** They decompress (LZ77) and their sizes are
  consistent, but nothing here interprets them. That is what stands between
  this and *rendering* a room, as opposed to describing one.
- ~~The door table.~~ **Now decoded** — see `docs/naming/doors.md` and §7
  below. The short version: `gUnk_08D640A4` is not a door table, it is a
  per-room progress list holding chests and doors in one tag-discriminated,
  self-striding array, and it yields a 559-edge room graph. Two limits on that
  graph are recorded in §7.
- **`priorityFlags`** — mostly read now (`docs/naming/room-tables.md`), and it
  is a misnomer: only bit 0 is about priority. **Bit 5 is read by nothing** in
  `src/`, `wip/` or `asm/`, yet is deliberately cleared in 14 specific rooms.
  That one needs a watchpoint, not more reading.
- **`unk02`, `unk04`, `unkXmod_06`, `unkXmod_08`.** Read, but they govern a
  third BG layer the retail game **never instantiates** — `objectList2Idx` is
  0xFFFF in all 983 slots — so those readings are code-only and untestable
  against data. `unkXmod_06/08` being 0x100 everywhere turns out to *support*
  the reading rather than undercut it: they are 8.8 parallax ratios and 0x100
  is the 1:1 identity.
- **The metatile attribute bits.** The table's address and its role are
  established; the meaning of individual bits is not yet.
- **Writing.** Everything here reads. Nothing yet builds a room, and the
  injection side — the thing that would make this a level editor rather than a
  level viewer — does not exist. The seam it would use does:
  `PortRandoObjList` already substitutes one of the room's tables at load.

---

## 7. Where a room randomizer would attach

Worth writing down now that the shape is visible, because §1 makes it simpler
than expected.

There are two distinct things "room randomizer" could mean:

**Shuffle room contents.** Permute the content index. Because all five indices
are the same number, one permutation moves geometry, collision, objects and
doors together as a unit — so rooms stay internally consistent. What breaks is
the graph: the doors inside a moved room still name absolute destination room
ids, and those are now wrong. So this needs the door table decoded, to rewrite
destinations to match the permutation.

**Rewire the connections.** Leave rooms alone and permute what the doors point
at. This is the version that produces a genuinely different game, and it is
the one that needs reachability logic — an ability-gated door graph where a
bad seed is unwinnable rather than merely hard.

Both used to terminate on the same blocker, the door table. **That is now
decoded** (`docs/naming/doors.md`), and the graph it yields was reproduced here
by an independent parse:

| | |
|---|---|
| door edges | **559**, every destination a valid room id (559/559) |
| chest entries | 84 |
| rooms with **no** outgoing door edge | **33** |

The 559/559 result is what settles the destination field: a `u16` that lands
inside a 286-element subset of 0..982 on all 559 samples is not a coincidence.
The same test on the chest variant's field scores 22/84, which is the rate you
would expect from coordinates — so the test also confirms the two entry kinds
are genuinely different structures.

Three things a randomizer built on this must handle, all of them measured:

**Doors are not the only way to change rooms.** 33 real rooms have zero
outgoing door edges, including room 126 — the first room of Rainbow Route.
Warp stars carry their destination in the object template
(`src/warp_star.c:1730`), invisible to this table, and boss rooms are exited by
a scripted transition rather than a door tile. Room 219 (Wiz) and room 417
(Crazy Hand) are both in the 33, which is exactly the shape you would expect.
A reachability checker built on this table alone will report dead ends that
are not dead ends.

**The graph is topology only.** There is no gating information in it — nothing
records that a door needs an ability, a key or a switch. A completability
check needs that from somewhere else.

**The table is a shadow copy, and getting it wrong hangs the game.** The
runtime reads the door descriptors out of the *solidity map* side
(`SolidityMap.unk4`); this table mirrors them. Both walks are `for (;;)`
terminated by a zero stride, so a door present on one side and missing from the
other is an infinite loop, not a soft failure. Anything that rewrites doors
must rewrite both sides and preserve the pairing — and the solidity side is a
different struct whose stride is a `u16` at +4, so the 8-byte walker used for
the shadow table does not transfer.

The exact invariant to preserve, measured on both sides independently:

| | |
|---|---|
| door **cells** on the solidity side (tag 3) | **724**, all 724 destinations valid room ids |
| distinct `(destination, x, y)` triples among them | **559** |
| door entries in the shadow table (tag 2) | **559** |

So a door occupies several adjacent cells but is one logical door, and the
shadow table holds exactly one entry per logical door. Those two counts were
produced by separate passes over different tables and agree exactly — which is
what makes the pairing an invariant rather than an assumption.

The injection point on the other side is already known and already proven:
`FillLevelInfo` resolves every one of a room's indices in one place
(`wip/code.c:245-252`), and `tools/portify.py` already routes one of those
lines through the port. Widening that from one table to all of them is a small
change to a mechanism that works.
