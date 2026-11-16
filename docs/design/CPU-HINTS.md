# The game's per-room CPU hints, and what the CPU AI modes use

Status: survey done; door groups, block breaking and reachable targets in
use (branch `six-hints`).  Tool:
`tools/cpu_hints.py ROM` (reads the ROM at run time; write its `--json`
dumps to a scratch directory, never into the repository).  The brain that
reads these hints: `docs/naming/cpu-brain.md`; the modes: `CPU-AI.md`.

Every number below is measured on the retail ROM by `tools/cpu_hints.py`
unless it says otherwise.  286 rooms (the real `gRoomProps` entries).

## What the game ships per room

All per-room tables are indexed by the room's content index
(`gRoomProps[room].unk22`, equal to `solidityMapIdx` / `objectListIdx` /
`doorsIdx` for every real room, `docs/naming/doors.md`).

### 1. The nav graph and the scripts: `gUnk_08D63C28[idx]` (`struct LevelInfo_1E8`)

| offset | field | meaning |
|---|---|---|
| 0x00 | `unk0[4]` | one CPU script per ROM player row (`KATAM_ROM_ROW(slot)`) |
| 0x10 | `unk10` | RL-compressed node map: one node index per 16x16 cell, same size as the collision map |
| 0x14 | `unk14` | the nodes, `struct LevelInfo_1E8_14`, 0x34 bytes each |
| 0x18 | `unk18` | node count (2..32, mean 17.7) |

A node (`struct LevelInfo_1E8_14`):

| offset | field | meaning |
|---|---|---|
| 0x00 | `unk0[32]` | next hop: `unk0[dest]` is the node to head for to reach `dest`; 0xFF = no route |
| 0x20, 0x22 | `unk20`, `unk22` | the node's point, pixels |
| 0x24 | `unk24` | arrival radius, pixels (32 in 4393 of 5052 nodes; 8..64 in the rest) |
| 0x26 | (u16) | zero in every node |
| 0x28 | `filler26[2]` | the node's own index (5052/5052) |
| 0x29 | `filler26[3]` | neighbour count, 1..7 |
| 0x2A | `filler26[4..11]` | the neighbours, 0xFF padded (== the set of first hops in 5000/5052 nodes) |
| 0x32 | (u16) | zero in every node |

There are **no edge kinds**: no jump / fly / ladder / water / one-way flag
anywhere in the node or the graph record (the only spare bytes are zero in
all 286 rooms).  Direction is implicit in the next-hop table: 143 rooms have
pairs reachable one way only (drops).  159 rooms have node pairs with no
route at all.  The stock brain uses the graph three ways:
`CpuPickRandomNode` (roam: a random node whose `unk0[node] != 0xFF`, inside
the camera bounds), `CpuGoalScriptNodeBegin` (a script's `goto`), and the
SENSE layer's waypoint (`unk14[nextNode]` point, `unk24` radius).  Nothing
reads the neighbour list.

### 2. The scripts (`CPU_OP_*`)

A u16 stream; the pc is a u16 (`CPU_OP_SKIP` with a negative operand
loops).  Operand counts: `wait`/`goto`/`ability`/`face`/`inhale`/`skip` 1,
`if_state` 3 (cmp, value, skip), the rest 0.

- 1100 of the 1144 scripts are `roam; end` (every row of 275 rooms).
- 11 rooms have real scripts, the same for all four rows:
  - the opening (803, 804, 805): walk node to node as the player trips
    people-only triggers (room state 10, 20, ... 60) -- "follow the player
    through the tutorial";
  - the hub (801): `wait 3; roam`;
  - seven puzzle rooms (146, 206, 310, 426, 617, 733, 815): loop "go to a
    node, face right, slide" (206, 426, 617, 733, 815) or "drop the
    ability, go to a node, inhale 3 s" (146, 815) or "go to a node, wait"
    (310) until a trigger raises the room state, then `roam`.
- `ability` occurs 8 times, always with operand 0xFFFF, which the u8
  `wantAbility` stores as 0xFF = **drop the ability**.  No script ever asks
  for a particular ability.
- No script encodes a route between doors.

### 3. The CPU triggers (`OBJ_CPU_KIRBY_TRIGGER`, `cpu_kirby_trigger.c`)

22 triggers in 10 rooms.  A trigger is a rectangle (template `unk1A..unk20`
relative to its position) that raises the room-state slot 0xF
(`STATE_SLOT_ROOM`, the room's slot) to `unk11` (10..60) when

- `unk22` bit 0 clear and bit 1 or 2 set: a Kirby stands in it -- bit 1
  people, bit 2 CPUs (16 people-only, 4 CPU-only, 1 both);
- otherwise: a star stone block (`OBJ_STAR_STONE_BLOCK` or the large one)
  is in it (1 trigger, room 146: the CPUs inhale a block into place).

`CPU_OP_IF_ROOM_STATE` compares that slot; the 10 rooms with branching
scripts are exactly the 10 rooms with triggers.

### 4. The rest the brain reads

- `gUnk_08D6CD0C[room]` (`struct Unk_08D6CD0C`): the room's **area**
  (`unk46`), **flags** (`unk47`), its neighbour rooms (`unk4[]`, count
  `unk49`), and `unk0`: `unk48` pointers to `{dest room, length, path...}`
  records -- an all-pairs room-level shortest path inside the area
  (`[i][1]` the path length, 0 = none; `[i][3]` the next room).  The
  off-screen room hops (`CpuNextRoomToward`) use it.  Flags: 0x08 on the
  eight area entrance rooms, 0x01 on the eight boss rooms, 0x02 on 15
  rooms the CPU leaves by warping home, 0x80 / 0x81 / 0x82 on special rooms
  excluded from the random neighbour pick.
- `gUnk_08D6DCAC[area]`: the area's exits as (room inside, room outside)
  pairs (`CpuPickAreaExit`).
- `gCpuLinkedRooms` (in the C source): eight (area entrance room, boss
  room) pairs -- confirmed: every first room has flag 0x08, every second
  flag 0x01, both in the same area.  `GetStateSlot(STATE_SLOT_SESSION,
  i + 1)` is that boss beaten.
- `homeRoom`: confirmed as the room a CPU falls back to: `CpuBrainCreate`'s
  third argument, set to the hub (0x321) when a CPU leaves the opening,
  the warp target from rooms with flag 0x02 and from `CpuNextRoomToward`'s
  dead ends.
- `gCpuBreakBlockChecks[ability]`: the block a CPU breaks, and how, when it
  is right in front of / above / below it.  The breakable level a CPU
  handles per ability (collision attribute `0x1000`, level in `0x00F00000`):
  levels 0-1 almost every ability (Normal: attack/inhale, or slide for level
  1); level 2 Burning, Wheel, Stone, Missile, Hammer, Master; level 3 Hammer
  and Master only; level 4 (331 cells, 25 rooms) none; Mini none.
  Chain blocks (attribute `0x240`, destroyed by a chain explosion, 4074
  cells in 131 rooms) are opened by hitting a chain trigger (`0x20`, level 1
  or 3).

## Were they useful?  (the three failures the user saw)

### A. Door groups (which doors of a room reach one another)

**The hints cannot be trusted for this on their own.**  The nav graph's
next-hop table does encode reachability, and in room 709 (the Moonlight
Mansion room of the bug report: three sealed chambers, one door each) it
gets the upper chambers right, but it links the lower chamber to the upper
right one (node 6 -> node 3) through two rows of solid floor.  The room
table (`gUnk_08D6CD0C`) has no notion of doors inside a room at all: 709's
neighbours are 702, 704 and 707, and 1166 of its 3912 multi-room paths cross
some room from an arrival to a door the collision map says it cannot reach
at the "normal ability" tier below (1009 even breaking up to level 3; 873
more cross rooms by edges or objects, not judged).  Some of those are
switch or object passages the map cannot see, but the off-screen hops the
stock brain uses this table for are warps, so it never noticed.

**The collision map can.**  Flood the cells a floating Kirby can pass from
a cell: not solid, not a Mini-only gap, a breakable block only if its
ability breaks that level, and a chain block (0x240) only once the blast
gets there -- from a chain trigger (0x1020) it can reach and break, through
chain cells (0x40).  That last rule makes reachability directed (a chain
set off from one side opens a way the other side cannot open), so the
modes use, for every door into a room, the set of the room's doors its
arrival cell reaches.  Over the 253 rooms with doors:

| | breaking nothing | levels 0-1 and chains (most abilities) | to level 3 (Hammer) |
|---|---|---|---|
| rooms whose doors are not all mutually reachable | 95 | 53 | 48 |
| entrances that do not reach every door of their room | 423 (140 rooms) | 234 (55 rooms) | 213 (47 rooms) |

The nav graph (mutual next hops) splits 73 rooms.

Door pairs (one door to another): the nav graph and the breaking-nothing
flood agree on 1310 pairs; the nav graph says "reachable" where the flood
needs blocks broken (up to level 3) in 415 pairs (the nav graph assumes
CPUs break blocks) and where nothing passes in 884 pairs -- 864 of those in
the two looping shaft rooms 180/181 (their doors lead into the same room),
the other 20 in 132, 312, 605, 706, 709, 736, 743 (graph errors like 709's);
the other way round, the graph has no route where the flood passes without
breaking anything in 122 pairs (one-way drops in the graph, or flying the
graph does not plan).  So the game's graph is the designers' "CPU paths"
and treats breakables as open; the flood is the physical answer and matches
the hand-checked rooms (709, 545, 108, 110).

What the modes do now (`game/src/cpu_ai_modes.c`): the route is a breadth
first search over door records (the solidity map's tag-3 cell records: door
cell -> room, arrival cell `unk0A - 1, unk0B - 1`): taking a door leads to
the doors its arrival cell reaches in the next room.  The Kirby's own room
uses its live collision map flooded from its cell (blocks already broken are
open) and only doors that are on the map now; a route never passes back
through the room it starts in.  The tier is the Kirby's current ability's.
The reach sets are computed once per tier from the ROM maps and cached (a
pure function of the loaded data, rebuilt if the room tables change);
`KATAM_CPUAI_LOG=3` prints them, and they match `tools/cpu_hints.py` for all
724 doors at tiers 0, 1 and 3.

### B. Breakable blocks in the way

**The hints say nothing useful here** beyond `gCpuBreakBlockChecks` (which
level each ability breaks, used for the tiers): the nav graph routes
straight through breakable blocks, and no script or trigger marks them.  So
the modes use the live collision map: while walking to the chosen door
(`BreakWay`), a breadth-first distance field from the door over the live
map (at the Kirby's tier) gives the cell path; when a cell that is not open
without breaking is within three steps, the Kirby goes to stand beside it
(same row, or on top of it), lets the air out if it is flying, turns to it
and runs the stock break check for its ability (`CpuBreakBlockCheck`, the
call `CpuActDecide` makes).  A chain block is attacked through its trigger
(the reachable trigger of the same chain).  The stock ACTION layer stands
still near a point straight above or below it (its distance is |dx - dy|),
so near the block it flaps up or lets the air out itself.

### C. Unreachable targets

Hints: none (no script asks for an ability; no switch data).  Map-based:

- doors: the route only uses doors reachable as above, at the tier of the
  ability it has (so a door behind level-2 blocks is not chosen by a
  normal Kirby);
- chests, shards, bosses, food: only targets whose cell the live flood
  reaches (a sealed chamber's chest is not chased);
- rooms wanted for an unopened chest: only if a chest is reachable from
  the door it would enter by (ROM object list, same flood);
- dead ends for door tiles (one-way rooms like 103, exits that are objects
  -- cannons, warp stars -- or room edges): after 20 s without a route, the
  stock warp to a neighbour from the brain's own room table (a wanted one
  first), and that way back in is blocked for the brain;
- a route whose only way on is straight back through the door just taken
  blocks that door for the brain (two rooms no longer send it to and fro).

Not done: fetching a needed ability (the data never names one; the map
names only the breakable level, so "inhale an enemy that gives Hammer" would
need an enemy-to-ability table and a plan), switches, objects that block
or open passages (sliding doors, triggerable walls, star stone blocks),
exits that are objects or room edges (only the warp fallback above), and
BODYGUARD in the same room as its person but cut off from it.

## Coverage per area (rooms; all have a nav graph; real scripts)

| area (`unk46`) | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| rooms | 51 | 26 | 21 | 25 | 23 | 29 | 31 | 29 | 28 | 3 | 20 |
| rooms with a real script | 1 | 1 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 3 | 0 |
