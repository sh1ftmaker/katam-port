# Gates and exit objects: switches, sliding doors, walls, crumble blocks, warp stars, cannons

Status: decoded for the CPU AI gate hints (branch `six-gates`,
`docs/design/CPU-HINTS.md` "Gates").  Functions called directly were renamed
in the tree (no code ID changes: none of them is stored as a code ID); the
ones the game stores as code IDs (object `unk78` states, init functions) keep
their `sub_` names here and are only named below.  Every claim is from the C
source; counts are from the retail ROM at run time (`tools/cpu_hints.py ROM
--gates`), never stored in the repository.

## Where the state lives

| state | storage | accessors |
|---|---|---|
| room-state slots: 16 `u32` per loaded room slot (`levelInfo->unk65E`) | `gUnk_02023388[roomSlot][16]` | `GetStateSlot(STATE_SLOT_ROOM, id, roomSlot)` |
| session flags (boss beaten, mirror doors) | `gUnk_02023488[]` | `GetStateSlot(STATE_SLOT_SESSION, id, _)` |
| world flags (saved: big switches, shortcuts) | `gUnk_02028C10[]` | `GetStateSlot(STATE_SLOT_WORLD, id, _)` |
| beaten marked enemies per room slot (template `unk22 & 1`, `object.c` on death) | `gUnk_02023508[roomSlot]` | `GetRoomKillCount` / `AddRoomKillCount` (were `sub_080029BC` / `sub_080029CC`) |
| collected chests / marked items per room slot (`chest.c`, `bonus.c`) | `gUnk_02023510[roomSlot]` | `GetRoomItemCount` / `AddRoomItemCount` (were `sub_080029E4` / `sub_080029F4`) |
| an object template's spawn / destroyed flag (template `unk2` kind 0 room slot, 1 session, 2 world; `unk3` bit) | `gUnk_02023358[roomSlot]`, `gUnk_02023368`, `gUnk_02028BF0` | `sub_080027E8` (get), `sub_08001678` (set/clear); `CreateLevelObjects` skips a template whose flag is set |
| the objects spawned in a room | `gCurLevelInfo[p].unk1F0`: a list of `struct Unk_02023720` nodes `{next, prev, &obj->base.unk56, inUse}` on the list of the player whose view spawned them | `CreateObject` inserts (`sub_08002958` a free node, `sub_08002918` insert), `sub_080028CC` removes, `sub_08002984` finds by `&unk56`; `level.c` hands a room's list to another player in it when its owner leaves |
| a room's default spawn cell (+1) | `gUnk_0893CB44[room][2]` | `sub_08002D40` (the stock CPU warp's arrival, `CpuWarpToRoom`) |

**Objects spawn only in the views of people.**  `CreateLevelObjects` and the
scroll-in walks (`sub_0800A2B4`, `sub_0800A460`) are run per player and test
the template against that player's view and every person's view
(`PortHumanEnd`/`PortHumanNext`); a room only CPU Kirbys are in has no
objects at all -- no sliding door, no switch, no rock block object, no chest,
no warp star.  So in such a room a sliding door is no obstacle, and nothing
that needs an object can be opened.

## Switches: `big_small_switch.c` (`OBJ_SMALL_BUTTON` 0x6D, `OBJ_BIG_BUTTON` 0x86)

`struct Switch`: `unkB4` hold timer (frames left), `unkB6` state bits,
`unkB8` is big.

| `unkB6` bit | meaning |
|---|---|
| 0x001 | down (pressed this frame or still held by the timer) |
| 0x002 | went down this frame (press edge) |
| 0x004 | came up this frame (release edge) |
| 0x010 / 0x020 | blinking while the hold timer runs |
| 0x100 | pressed this frame |
| 0x200 | has been pressed |
| 0x400 | pressed by standing on it (platform contact, object flag 0x1000000) rather than by overlap: set when template `unk18 != 0` or `unk22 & 4` |
| 0x1000 / 0x2000 | big switch: world unlock pending / done |

| template field | meaning |
|---|---|
| `unk4` | room-state slot it drives (small) / world flag it sets (big) |
| `unk18` | hold time: frames it stays down after the Kirby leaves (+2) |
| `unk22 & 1` | value mode: while down, writes the 3-byte value (`unk14 >> 8`, `unk14`, `unk12 >> 8`) to the slot |
| `unk22 & 2` | big switch: attack it (people only, `PortIsHuman`) to set world flag `unk4` and run world-map unlock `unk14` (`gUnk_08357B8C`) |
| `unk22 & 4` | stays down for good |
| `unk22 & 0x8000` | counting, and raises room-state slot 0xF to `unk11` (the CPU scripts' `if_state`) |
| default | counting: +1 on the slot when it goes down, -1 when it comes up |

| old | new | what |
|---|---|---|
| `sub_0811942C` | `SwitchStillInRoom` | FALSE once it fell to the room's bottom (freezes it) |
| `sub_081194DC` | `SwitchIsPressed` | a star stone block on it, a Kirby on it, or (big) a person's attack |
| `sub_081197D4` | `SwitchUpdatePress` | the `unkB6` edges and the hold timer |
| `sub_08119A20` | `SwitchCountOnSlot` | the counting mode |
| `sub_08119AA8` | `SwitchWriteSlotValue` | the value mode |
| `sub_08119B3C` | `BigSwitchUnlockWorld` | the big switch's world flag + map unlock interlude |
| `sub_08119BB8` | `SwitchCountAndRaiseRoomState` | the 0x8000 mode |
| `sub_0811938C` | (kept: a code ID) switch init: a big switch whose world flag is set is spent |
| `sub_08119980` | (kept) the switch's every-frame state |
| `sub_08119C0C` | (kept) gone |

The link from a switch to what it opens is only the slot number: a sliding
door (or cannon) whose template `unk4` is the same room-state slot.  Big
switches link to `OBJ_DOOR_TO_HUB` (0x97) doors with the same world flag in
`unk4`, in any room: the door's tile is put in (`sub_08001408`) when the flag
is set (`special_doors.c` `sub_0802AD00`).

## Sliding doors: `vert_sliding_door.c` (`OBJ_VERTICAL_SLIDING_DOOR` 0x71)

Its template `y >> 4` is the floor cell; it fills the two cells above it (a
2-cell gap: 35/35 doors in the ROM have solid floor and ceiling around it).
It opens while room-state slot `unk4` is non-zero.  `struct VertSlidingDoor`:
`unkB4` bit 0 moving, bit 1 open; `unkB6` frames per step (a step every 7);
`unkB7` steps open (0..8, bounds bottom at `-4 * unkB7` px); `obj.unk83` 1
opening, 2 closing.  States `VertSlidingDoorIdle/Open/Close` (already named).
Fully opening or closing takes about 56 frames.

## Triggerable walls: `lever_wall.c` (`OBJ_TRIGGERABLE_WALL` 0x91)

A chain-blast source: when `GetRoomKillCount` (template `subtype1` 0) or
`GetRoomItemCount` (`subtype1` 1) of its room slot equals template `unkF`,
`LeverWallAction` replaces its cell (`sub_08001408`) and starts a chain
explosion there (`sub_0800E0E4`), which clears the chain cells (collision
0x40) joined to it.  Its cell is collision 0x240 (solid + chain).  5 in the
ROM, all `subtype1` 1 with `unkF` 1: "open the chest in this room".

## Crumble blocks: `destroyable_rock_block.c` (`OBJ_DESTROYABLE_ROCK_BLOCK` 0x74)

A solid cell (mostly collision 0xF7 = breakable level 4, which no ability
breaks) with an object on it.  `sub_0811BF24` (stand check, a code ID):
a Kirby inside the rectangle 8..24 px above it (template `unk22 & 1`: its own
rectangle `unk1A..unk20`), not flying (flag 0x40) and not in the air (0x20),
sets `unkB6 |= 1`; `sub_0811C07C` (crumble) after `unkB4` (10) frames puts its
cell's tile record in (`sub_08001408`) and destroys it;
`DestroyDestroyableRockBlock` sets its template's destroyed flag
(`sub_08001678`) so it does not spawn again.  137 in 34 rooms.

## Warp stars: `warp_star.c` (`OBJ_WARP_STAR` 0x84)

Template `unk1E` the destination room, `unk1A`, `unk1C` the arrival cell + 1
(copied to `ws->unkBE/unkC0/unkC2`); negative values in 3 of the 6 stars
(they fly in from off the map).  `sub_0800C084` -> `WarpStarTryBoard`: a
Kirby touching it boards (`STAR_ADD`) -- only a person when the star is
waiting (`a2` TRUE), anyone while it is leaving with people.  6 in the ROM.

## Cannons: `kirby_cannon.c` (`OBJ_CANNON` 0x8A), `cannon_fuse.c` (0x78)

Template `unk12` the destination room, `unk18` the arrival (`>> 8` x; the
decompiled `unkBD = unk18 != 0` is the y), `unk4` the room-state slot that
fires it (a burning fuse adds one, `cannon_fuse.c`), `unk14` a slot that,
while set, keeps Kirbys out (best guess).  Only people get in (`PortIsHuman`).  `sub_08121AF8` init,
`sub_08121B70` waiting (who is in), `sub_08121FCC` firing (calls
`sub_080515D4`, the launch: room, arrival, arc), `sub_08122248` recoil (all
code IDs, kept).  4 in the ROM.  `OBJ_8_DIR_CANNON` (0x8B) launches inside
its room (no destination): not an exit.

## The CPU AI's own names (`game/src/cpu_ai_modes.c`)

| name | what |
|---|---|
| `struct HintDoor`, `EXIT_DOOR/STAR/CANNON` | a way out of a room: door tile record or exit object, with its arrival |
| `struct HintGate`, `GATE_SLIDE/SWITCH/CRUMBLE/WALL` | a gate or opener of a room |
| `sGate`, `sGateFirst`, `sGateCount` | the gates per room (ROM cache) |
| `sGateOv`, `OV_SHUT/DOWN/OPEN` | what the gates make of each cell for the next flood |
| `GateOverlay`, `FloodGated`, `GateOpeners` | the gate overlay; the flood with openers opened until nothing more opens |
| `RoomObjectList`, `RoomObjectAt` | the live objects of a room (the per-player object lists) |
| `SlideOpenNow`, `LiveOpen` | a sliding door passable now |
| `GateWay` | act on the gate in the way: switch, wall's chest / enemy, hurry through |
| `CrumbleUnseen` | stand on a crumble block whose object is not spawned: do what it would |
| `PathWaypoint`, `PathTo` | steer along the live flood's cell path (pits left out) |
