// `katam reach` and `katam playtest`: designed, not implemented yet.  The plan
// is printed by the CLI (exit status 3 = "not implemented") so a script or a
// model calling them learns what they will do and what to use meanwhile.
const PLANS = {
  reach: `katam reach ROOM... -- not implemented yet (docs/design/AUTHORING.md section "reach").

Will answer: from each door / arrival point of a room, which other doors and
arrivals can Kirby get to?  Model: a graph over standable cells (an empty cell
with a floor -- solid, top_solid or slope -- below).  Edges: walk left/right
along floors and slopes; fall (any distance, drifting up to 4 cells
sideways); jump (up to 3 cells high, 4 across: KATAM's jump apex, measured
from the native build's physics at 60 Hz); float (Kirby flies indefinitely,
so with "float" on, any empty cell connected to the arrival through air is
reachable -- the default, matching the game; --no-float models a mini/stone
ability or water ceilings); ladders climb; water swims; hazards are
traversable but reported; breakable blocks are passable with --abilities.
Doors are edges between rooms (the world graph), checked both ways.
Output: per room, the reachable door set and unreachable doors with the
blocking cells; --json for tools.  Until then: katam validate checks
references, arrivals and one-way doors, and katam render --overlay doors
draws doors and arrivals.`,
  playtest: `katam playtest ROOM -- not implemented yet (docs/design/AUTHORING.md section "playtest").

Will do: compile the project stack, start the native binary headless with the
compiled pack (--res-pack OUT/pack, or --mod for a mod) and --warp to the
room's numeric id and an arrival cell, run a scripted input recording through
the determinism harness (tests/regress.sh conventions: fresh XDG dirs,
setarch -R, SDL dummy drivers), and report: did the room load, did Kirby
survive N seconds, the per-frame state hash, and screenshots at fixed frames.
Blocked on the runtime: the game loads room props, palettes and object
descriptors from a pack today, but a room's tilemap, collision, objects,
doors and nav still come from the ROM tables.  A new room needs the native
room-bundle loader (MODDING.md R2 for the room bundle, consuming OUT/native,
katam-native/0).  Until then: edits to vanilla rooms' RoomProps, palettes
and object stats can be playtested by hand with
  build/lp64/katam --res-pack OUT/pack --warp <roomId> baserom.gba`,
};
export const plan = (cmd) => PLANS[cmd];
