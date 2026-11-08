# Level records: what the registry says, and what changed

Companion to `assets/schema/semantics/{room_props,metatile_attributes,
solidity_map,progress_list,tiled_bg,nav_graph,object_lists,object_template}.json`.
The JSON is the source of truth; this page is the reading guide and the list
of things that are new or corrected relative to the older proposals in
`docs/naming/` (whose line numbers predate the all-C tree and whose `wip/`
paths now live in `game/src/`).

## A room in one picture

```
gRoomProps[roomId]  (room_props)
  songIdx ............ m4a song number, 0 = silence (boss rooms start their own)
  unk0A/0C/0E/10 ..... background (BG0) offset + parallax / auto-scroll
  unk02/04/Xmod_06/08  extra layer (BG2) offset + parallax  -- never instantiated
  priorityFlags ...... room flags (BG priority, auto-scroll, phone, return-to-hub)
  pixelDataIdx ------> gForegroundInfo      playfield tileset + tile animations
  paletteDataIdx ----> gForegroundPalettes  playfield palette + palette animations
  mapDataIdx --------> gForegroundTilemaps  room size (tiles) + playfield tilemap
  backgroundIdx -----> gRoomTiledBGs        background tileset/palette/tilemap
  solidityMapIdx ----> gSolidityMaps        collision grid + cell records
  objectListIdx -----> gLevelObjLists       object placements
  unk22 -------------> gUnk_08D63C28        AI-helper nav graph + scripts
  doorsIdx ----------> gUnk_08D640A4        progress checklist (chests, doors)
                       gVisitedDoors[]      saved progress bits for the room
```

In retail data the five "content" indices are equal and unique per room, but
the engine reads each separately; an editor can keep one content index per
room but must write all five.

## Background scrolling, exactly

Per frame, for the background layer (camera-parallax mode, `priorityFlags`
bit 2 clear), in pixels:

```
bgScrollX = cameraX * ratioX / 256 + unk0A
bgScrollY = (bgHeight - 160) - (roomHeight - 160 - cameraY) * ratioY / 256 - unk0C
```

X is anchored at the room's left edge, Y at its bottom edge. `ratioX` is
`unk0E`, or when `unk0E == -1`, `(bgWidth - 240) * 256 / 2160`: the "magic"
0x870 is 9 x 240, i.e. the automatic ratio assumes nine screen-widths of
camera travel; 0x5A0 = 9 x 160 likewise for Y. A background no larger than
the screen gets ratio 0. With bit 2 set, `unk0E/unk10` are added to the
scroll every frame instead (only room 919 does it, with 0/0, to freeze the
layer for its scripted sky).

## Room flags (`priorityFlags`)

| bit | name | meaning | retail |
|---|---|---|---|
| 0x01 | playfieldBehindExtraLayer | BG3 prio 2 / BG2 prio 1 when set; BG3 1 / BG2 2 when clear | set everywhere |
| 0x02 | extraBgAutoScroll | extra layer: per-frame velocity instead of parallax | never |
| 0x04 | bgAutoScroll | background: per-frame velocity instead of parallax | room 919 |
| 0x08 | phoneCallAllowed | R calls the helper Kirbys (costs a battery); HUD phone icon | clear in 919, 980-982 |
| 0x10 | answerCallAllowed | R answers a pending call (warp to the caller); HUD call indicator | clear in 919, 980-982 |
| 0x20 | (unknown) | no reader anywhere; cleared in 14 hub/minigame/credit rooms | — |
| 0x40 | returnToHubAllowed | hold L 48 frames: return to room 801 | clear in 24 rooms |

**Corrected:** `docs/naming/room-tables.md` had bit 6 as "a second summon
action" and could not tell which buttons were involved. `unk11A` is the
newly-pressed key mask (`gHeldKeys & ~previous`, kirby.c), so 0x100 = R and
0x200 = L; the L path (`sub_08056E40` -> `sub_080573D0` -> `sub_0805BEE8` ->
`sub_08057774`) ends with `kirby->roomId = 0x321`: the return-to-hub warp.
That also explains the rooms where it is clear: the hub itself, the final
area, minigames and credits.

## Collision

A cell value is an index into `gCollisionAttributes` (256 u32 words, defined
in C). The word: bits 0-15 flags, 20-23 a level whose meaning depends on the
flag, 24-27 a direction, 28-31 a slope enum. `metatile_attributes.json`
`values` names all 125 non-zero entries; the commonly placed ones:

| idx | name | idx | name |
|---|---|---|---|
| 0 | air | 72 | water |
| 13 | solid | 80 | ice_block |
| 14 / 15 | platform_drop / platform (one-way) | 74-79 | ice slopes |
| 1-12 | slopes (5,6,7,8,11,12 floors; 1-4,9,10 ceilings) | 81-88, 40-47 | wind / currents (dir, strength 0-2) |
| 32 / 33 | ladder_top / ladder | 89 | mini_gap (non-Mini Kirby dies) |
| 28-31 | spikes floor/ceiling/wall | 90 / 91 | fire hazard solid / passable |
| 240-247 | breakable blocks, hardness 0-4 (241/246 chain triggers) | 242 / 243 | chain-blast solid / gap |
| 255 / 254 | door / auto-warp door | 253 / 252 | underwater door / auto-warp |

New in this pass:

- **Hazard level = damage element.** All three levels reach `sub_0804D9D4`,
  which re-reads the level: 0 plain damage, 1 burn (`sub_0804A1A0`, anim 123,
  `SE_KIRBY_FIRE_DMG`), 2 a second element (`sub_0804A328`, anim 135, generic
  damage SFX; probably electric, unconfirmed). Levels above 2 do no damage.
- **Breakable hardness** gates on the attacker's `unk68` bits (0x1000 /
  0x4000 / 0x800 for levels 2 / 3 / 4) in `object_collision.c`. Level 4
  (0x800) is set by the giant star/stone block objects and one Kirby attack
  (`sub_08083FC0`); which *ability* each Kirby attack function belongs to is
  not mapped yet.
- Indices with the same word are not interchangeable (34-39 = 13 by value):
  cell records key on the index.

## Cell records and doors

`SolidityMap.unk4` is a list of records keyed by (current cell value, x, y):

- **tag 1, patch16** — "what this cell becomes": four tilemap entries and a
  new collision value. Every breakable block cell, every switch-revealed door
  (40 records turn air into door 255) and every chained change has one.
- **tag 3, door** — destination room, arrival cell, arrival flags
  (bit 0 face right, bit 1 face left, bit 7 keep the old respawn point; new).
- tag 2 (8-bit tilemap patch) is dead.

The lookup `sub_080025AC` has no terminator test: a breakable or door cell
without its record hangs the game. The progress list (`progress_list.json`)
holds one tag-2 entry per logical door, keyed by its exit triple, and one
tag-1 entry per chest, chests first; its walkers likewise loop until found.
So a door is three things an editor must keep in step: the door collision
cells, one tag-3 record per cell, and one progress entry.

## Smaller corrections

- `RoomTiledBG.tilesetSize` is 900 in every room background (including 30x320
  and 32x22 ones); the room loader never reads it. Whether it means tiles is
  a guess.
- Nav nodes: `arriveRadius` is not constant (32 on 87% of nodes, also 8-64).
- `songIdx` 0 is `MUS_DUMMY` (silence) in the eight boss rooms x00 and 909-918;
  rooms 102, 113, 114 play sound effect 0x20B as their music.

## Still unknown

- `priorityFlags` 0x20: no reader; needs a watchpoint or a diff of behaviour.
- The AI script opcodes (nav_graph `scripts`) and the nav node tail.
- `LevelInfo_1E4.unk8/unkD` (always empty, no callers).
- Which ability each Kirby attack function is, for the hardness table.
- The second hazard element (2) by name.

## Objects

`object_template.json` is the shared 36-byte header; `object_params.json`
documents the per-type fields for all 121 placed types (enemies 0x00-0x37,
bosses 0x38-0x52, items/doors/gadgets 0x53+). Highlights:

- Doors (0x6F, 0x99, 0x72, 0x97, 0x8C) carry no destination: it is the tag-3
  cell record at the object's cell. Warp stars (0x84) and the Dimension Mirror
  (0x65 subtype1 8) carry it in the object: unk1E = room, unk1A/unk1C = cell.
  Cannons (0x8A): unk12 = room, unk18 high byte = cell x.
- `unk22` low bits are generic for types 0x00-0x64 (kill counter, pushed by
  solid objects, no music change); other bits and other types are per-type.
- Items: unk22 bit 3 = floating (no gravity, no tile collision).
- Bosses: unk1A..unk20 = wake-up rectangle; subtype2 = hard/rematch mode.
- Enemies: subtype1 = movement pattern (enumerated per type), subtype2 mostly
  a speed/aggression tier.

Nothing in object_params was confirmed in the running game; `confirmed`
there means unambiguous code plus agreeing value statistics.
Tuning numbers (HP, damage, speeds, physics): `tuning-inventory.md`.
