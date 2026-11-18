# CPU AI modes

Status: first draft (branch `six-cpuai`); door groups (`six-hints`); gate
hints and exit objects (`six-gates`).  A slow planner (a Haiku agent)
directing these brains one directive at a time: `CPU-HAIKU.md`.  Code:
`game/src/cpu_ai_modes.c`
(the brains), `platform/cpu_ai_host.c` (the setting, the input-log/online
plumbing), `platform/port/cpu_ai.h` (the contract), a three-line hook in
`CpuBrainTask` (`game/src/code_0800ECAC.c`), rows in
`platform/menu_poc.c`.  The stock brain is described in
`docs/naming/cpu-brain.md`.

## What the host can choose

SETTINGS > EXPERIMENTAL has four rows: **CPU AI** (every CPU Kirby at once:
MIXED / CLASSIC / BODYGUARD / EXPLORER / ADVENTURER) and **KIRBY 2**,
**KIRBY 3**, **KIRBY 4** (one each).  The default is MIXED: Kirby 2
BODYGUARD, Kirby 3 EXPLORER, Kirby 4 ADVENTURER; the 16-Kirby build's
Kirbys 5..16 take the same three in turn (5 BODYGUARD, 6 EXPLORER,
7 ADVENTURER, ...).  A setting other than MIXED or a single mode for all
shows as CUSTOM on the CPU AI row.  In an online game only the host can
change them (guests see the rows dimmed: THE HOST CHOOSES) and every device
shows the session's modes.  No cap: the rows work whenever the game has CPU
Kirbys (single player, local and online sessions); a slot a person plays
ignores its mode (the brain waits while a person drives it).

| mode | GOAL layer | SENSE layer | ACTION layer |
|---|---|---|---|
| CLASSIC | stock (script node / roam / follow / off-screen warps) | stock | stock |
| BODYGUARD | goal = the nearest person in its room (24 px to one side); no person in its room: walks to that person's room door by door (route over the game's door graph); after 20 s stuck in one room, the stock off-screen warp to the next room of the route | stock enemy scan (so it fights what is near the person, because it is near the person); low HP: scan held off while it walks to food | stock |
| EXPLORER | route (breadth first over the door graph) to the nearest room nobody has entered, or with an unopened chest; walks to the door tile and takes it; walks to a chest in a room that has one unopened (20 s at most); marks rooms it enters as entered (the pause map shows them) | stock; low HP as above | stock, plus a nudge along the ground for the last few pixels to a door |
| ADVENTURER | as EXPLORER, but the route's target is the nearest boss room whose boss is not beaten (the stock brain's own `gCpuLinkedRooms` pairs and session flags); in a room with a live boss or mid-boss it stays on it; a mirror shard in the room (not the hub's) it goes for; no boss route: explores | stock (the enemy scan attacks the boss); low HP as above | stock |

Common to the three new modes:

- **Routes over door groups** (`CPU-HINTS.md`): the route is a breadth
  first search over (room, door group) states, not over rooms.  A room's
  doors fall into groups that reach one another (the collision map flooded
  from each door, blocks the Kirby's ability breaks counted open); entering
  by a door puts the Kirby in the group of its arrival cell, and only that
  group's doors lead on.  In its own room the Kirby's live map is flooded
  from where it is, so it never chooses a door it cannot get to (room 709,
  three sealed chambers: before, all three Kirbys aimed at the upper-left
  door; now each takes its own chamber's door).  Rooms joined by their edges
  or by objects rather than by door tiles are not in the graph: a room with
  no door it can reach (a dead end for the graph) ends after 20 s with the
  stock warp to a neighbour from the brain's own room table.
- **Blocks in the way**: a block (or chain) on the cell path to the door
  within three steps is broken first: stand beside it, stop flying, face
  it, the stock break check for the ability (CPU-HINTS.md, B).
- **Gates** (CPU-HINTS.md, D; `docs/naming/gates.md`): the room object lists
  give sliding doors and the switches that share their room-state slot,
  crumble blocks, triggerable walls.  Objects spawn only where people are,
  so: in a room with a person, a shut sliding door is a wall in the floods,
  a door behind it is still a target when one of its switches is reachable,
  and the Kirby goes and stands on the switch (until the door is fully open
  for a hold-only switch), then hurries through along the flood's cell path;
  a wall waiting for the room's chest sends it to the chest.  In a room with
  no person there are no sliding doors to stop it, and a crumble block it
  stands on crumbles (the CPU does what the unspawned object would: its tile
  record and its destroyed flag).
- **Exit objects**: warp stars and cannons are edges of the route (their
  room and arrival from the template); the Kirby walks to the object and
  goes where it would send it, as it takes doors.  Room 142 (a warp star,
  no door tiles) and the rooms before it are no longer dead ends.
- **Only reachable targets**: chests, shards, bosses and food the Kirby
  can get to from where it is; rooms wanted for a chest only if the chest
  is reachable from the door it would enter by.
- **Door use**: the Kirby walks (nav graph first, then 5 s turns of steering
  straight at the door when the graph stops getting it nearer) to the door
  tile it chose, and within one tile of it takes the door with
  `sub_080551FC` -- the same call the stock brain makes when a CPU happens to
  stand on a door.  So doors are "walked into", not opened with Up, and the
  call does not check locks: a CPU can take a door a person could not yet
  (see "Cheats" below).
- **Giving up**: a door not reached in 30 s is blocked (the last three
  failed room->door pairs are remembered) and the route is recomputed; a
  room that has held the Kirby for 60 s ends with the stock warp to the room
  behind the chosen door.  The route only starts through doors the room
  shows now (door tiles present), so locked hub mirrors are not tried.
- **HP**: below a third of its maximum, if there is food in the room
  (small food, soda, meat, Maxim tomato) it walks to it with the enemy scan
  held off.  There is no "retreat to a safe room" yet.
- **Room scripts**: a room's CPU script (the stock SCRIPT layer) runs first;
  after 10 s in the room the mode takes over anyway.
- **The opening** (rooms 0x323-0x325) is left to the stock brain when no
  person is in it (it takes CPUs to the hub).

What a mode does not change: actions (walking, jumping, flying, swimming,
attacks, swallowing) are the stock ACTION states; the enemy scan and the
mouthful decision are stock.

## Determinism and where the state lives

- The modes are two bits per Kirby slot in the sim block's spare word
  `gPortSim->game.reserved[0]` (`port/simblock.h`): snapshotted, hashed,
  carried in images.  Zero (power-on) = everyone CLASSIC = the stock brain;
  the hook in `CpuBrainTask` is `if (!word || !CpuAiGoal(brain)) stock goal`.
- Each brain's own state (chosen door, frames in room, blocked doors,
  progress toward the door) is in `struct CpuBrain`'s filler bytes
  (`fillerC0`, `fillerCE`, `fillerF0`), which the game zeroes at creation
  and never otherwise touches: zero while CLASSIC.  The blocked pairs are
  packed (20 bits each) to make room for a planner directive in the same
  filler (`CPU-HAIKU.md`, "Where the state lives").
- No new function pointers are stored (the hook steers the stock layers),
  so no new code IDs.
- The word changes only at a latch: offline a recorded input-log line
  `cpuai FRAME WORD` (hex), online the host's sealed `PORT_LN_EV_CPUAI`
  (a = word).

### Why recordings and tests stay CLASSIC

The default is no longer the stock brain, so who asks for the mix matters.
`PortCpuAiPoll` (`platform/cpu_ai_host.c`, called once a frame before the
latch) asks for this device's setting only when

1. not replaying a recording (a replay applies only its own `cpuai`
   lines; recordings made before this have none, so they stay CLASSIC), and
2. not an online guest (the host decides) and not the overlay fork /
   canonical shadow, and
3. the in-game menus are on (`PortMenuPoc()`: the web page; natively
   `KATAM_MENU_POC=1`) -- or `KATAM_CPU_AI=WORD` is set for a headless run.

So native/node runs without menus (the regress recordings, the netproof
rigs that do not turn menus on, `tests/*.sh`) never ask: their word stays
0.  The attract demo (`gUnk_0203AD10 & 0x10`) is CLASSIC whatever the word.
A live web game asks at its first frame and the request is recorded, so a
recording made on the page replays with the same modes.

## Beating the game: what it needs vs what the brain does

The goal chain of the game: leave the opening, reach the hub (room 801),
open the areas (big switches unlock hub mirrors), reach each area's boss,
beat it, take its mirror shard (8 in all), then the final area and bosses.

| needed | today |
|---|---|
| walk room to room toward a goal | yes (door graph routing + nav graph) -- draft quality |
| know where the bosses are | partly: the stock brain's eight `gCpuLinkedRooms` pairs (boss-door room -> boss room) |
| know which doors are open | partly: door tiles present in the room now, door groups (which doors a Kirby can reach from where it is or enters, with the blocks its ability breaks), sliding doors and their switches, crumble blocks, chest walls, warp stars and cannons (gate hints); ability gates it does not have are found by failing (30 s) |
| press switches | small switches that open sliding doors: yes (with a person in the room: elsewhere there are no doors); a hold-only switch for another Kirby: no |
| press big switches | no (the game lets only people; it starts a world-map cutscene and writes the save) |
| use the right ability for an obstacle (hammer stakes, fuse lines, bomb blocks) | no (stock: block breaking only if it already has a fitting ability) |
| fight a boss with a plan | no boss routine: stock attack scan, goal on the boss |
| collect a shard | walks to a shard object if it sees one (untested end to end) |
| survive | food when below a third of HP; no item use, no avoiding hazards |
| final boss sequence | no |

Honest expectation: EXPLORER and ADVENTURER wander far through Rainbow
Route and into areas whose hub mirrors are open, take many doors, and get
stuck at ability gates and switch puzzles; beating a boss and returning a
shard is not expected from this draft.  See "Evidence" for numbers.

## Cheats the draft inherits from the stock brain

- Door use by `sub_080551FC` does not check whether the door is locked.
- The stock off-screen warp (used after 60 s stuck) teleports a Kirby.
- The stock off-screen ability gift still applies when a CPU is away from
  every person (`CpuActDecide`: a random ability now and then).
- EXPLORER/ADVENTURER mark rooms they enter as entered in the save (the
  pause map and completion count show rooms the CPUs found).
- Warp stars and cannons take only people; a CPU in these modes reaching one
  goes where it leads anyway (by `sub_080551FC`, as with doors) -- also a
  cannon whose fuse nobody lit (room 126 -> 570).
- A crumble block a CPU stands on in a room no person is in crumbles without
  its object (which only spawns in a person's view), and stays crumbled in
  that room's state as if a person had stood on it.

## Evidence

Native, headless, the player's pad idle: a recording that boots a new
game, sets the story past the opening, warps player 1 to the hub (room 801)
at frame 1400 and then presses nothing; `cpuai 1300 WORD` picks the modes.
10 minutes of game time (37400 frames); counts from `KATAM_CPUAI_LOG=1`
(the `[cpuai-obs]` lines watch every Kirby whatever the mode).  First draft,
one seed each, so treat as indicative:

| run (Kirbys 2/3/4) | distinct rooms (K2/K3/K4) | doors walked | stock warps | deaths | shards |
|---|---|---|---|---|---|
| CLASSIC/CLASSIC/CLASSIC | 6 / 6 / 9 (off-screen hops) | 0 | (stock hops) | 1 / 1 / 0 | 0 |
| MIXED (BODYGUARD/EXPLORER/ADVENTURER) | 2 / 13 / 13 | 0 / 15 / 19 | 0 / 2 / 2 | 0 / 0 / 1 | 0 |
| EXPLORER x3 | 13 / 13 / 14 | 15 / 14 / 14 | 2 / 2 / 2 | 0 | 0 |
| ADVENTURER x3 | 14 / 16 / 10 | 29 / 21 / 13 | 2 / 3 / 3 | 1 / 1 / 0 | 0 |

They leave the hub through Rainbow Route (101 -> 106 -> 107 -> 108 ...),
reach rooms of area 5xx through Rainbow Route's links, and stall most at
Rainbow Route 108 / 110 / 140 (doors given up after 30 s: likely ability or
switch gates).  No boss reached, no shard.  The BODYGUARD stays with the
idle player in the hub all along.

### With door groups, block breaking and reachable targets (`six-hints`)

The same four recordings, sums over Kirbys 2-4 (time stuck = time in room
stays longer than 30 s, past the 30 s; give-ups = doors given up after
30 s; warps = 60 s-stuck warps + dead-end warps):

| run | distinct rooms | doors walked | warps | give-ups | time stuck |
|---|---|---|---|---|---|
| MIXED before / after | 25 / 38 | 34 / 34 | 4 / 14 | 14 / 8 | 977 s / 850 s (566 s of it the BODYGUARD idling with the idle player, both) |
| EXPLORER x3 | 37 / 59 | 43 / 52 | 6 / 23 | 21 / 17 | 616 s / 391 s |
| ADVENTURER x3 | 37 / 49 | 63 / 65 | 8 / 17 | 27 / 7 | 442 s / 404 s |
| CLASSIC | identical | | | | |

They now reach Moonlight Mansion (7xx) and Olive Ocean (8xx) rooms through
the one-way Rainbow Route rooms 103/142 (dead-end warp).  Most warps are
the dead-end fallback (49 of 54 after; 18 stuck warps before).  Remaining
stalls: 831/814 (8xx), 704/706 (7xx), 740 -- doors behind switches or
objects the map cannot see.  One seed each.

### With gate hints and exit objects (`six-gates`)

The same recordings (summed over Kirbys 2-4 as above; warps = all warps,
in brackets the dead-end fallback ones), before -> after:

| run | distinct rooms | doors walked | warps (fallback) | give-ups | time stuck |
|---|---|---|---|---|---|
| MIXED | 38 / 34 | 34 / 59 | 14 (12) / 2 (1) | 8 / 4 | 848 s / 722 s (564 s of it the BODYGUARD idling with the idle player, both) |
| EXPLORER x3 | 59 / 61 | 52 / 75 | 23 (20) / 13 (12) | 17 / 13 | 391 s / 352 s |
| ADVENTURER x3 | 49 / 60 | 65 / 77 | 17 (17) / 13 (11) | 7 / 10 | 388 s / 292 s |
| all three | 146 / 155 | 151 / 211 | 54 (49) / 28 (24) | 32 / 27 | 1627 s / 1366 s |
| CLASSIC | identical (rooms of every Kirby; layout-neutral check) | | | | |

One seed each is noisy (a small change sends a Kirby another way), so the
same three were also run with the player's hub warp at frames 1437 and 1471
(nine runs, sums): distinct rooms 457 -> 473, doors 427 -> 586, warps 161
(142 fallback) -> 99 (88), give-ups 112 -> 93, time stuck 5091 s -> 4067 s.
The person idles in the hub in all of them, so the CPUs are always alone:
no objects spawn, the switch logic never runs here, and the gains are the
warp star of room 142 (30 of the 142 fallback warps before, and the dead
ends 104 -> 103 -> 142 behind it) and crumble blocks (814 -> 829: 13
give-ups before, 1 after).  `tests/cpu_ai.sh` covers the switch case with
a person in the room (712).

Remaining stalls (nine runs): 706 -> 707 (17 give-ups: a row of pits the
Kirby must fly over), the Moonlight Mansion cycle 790 / 740 / 708 / 743 /
824 (each about 13-15 dead-end warps: everything reachable is explored, the
rest is behind the chest wall of 743 -- no chest object without a person --
and the hub door of 790, which opens with a big switch only people press),
814 -> 832 (11: a long chain of level-1 blocks and a chain fuse), 829 -> 811
(8), 704 -> 711 (6: the nav graph sends it round the wrong side).  Movement,
not knowledge, is now the larger share.

## What would be needed to finish the game (prioritised)

1. Obstacle knowledge: done for breakable blocks, switches and sliding
   doors, crumble blocks, chest walls, warp stars and cannons (gate hints);
   still to do: have ADVENTURER fetch an ability (inhale a matching enemy;
   `wantAbility` already exists) before a gate, and star stone blocks.
2. Big switches: the game lets only people press them (`PortIsHuman` in
   `SwitchIsPressed`), and each starts a world-map interlude and sets a
   saved world flag; a CPU press would need a rule change there.
3. A boss routine per boss (keep distance, attack windows, inhale the
   projectiles stars) instead of the generic scan.
4. Door locks: respect the game's own lock state so routes match what a
   person can do (and drop the stock warp fallback in a "fair" variant).
5. Survival: track hazards (spikes, pits) per room; leave a fight at low
   HP; use the phone/food items.
6. Coordination: share one route/target between CPUs (sim memory for a
   team plan), BODYGUARD shields the ADVENTURER, etc.
7. The final sequence (Dark Mind) needs all of the above plus its own
   scripted handling.
