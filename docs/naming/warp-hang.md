# Why `PortWarp` hangs, and the one-line class of fix

**Status:** root cause found, proven by inverting the symptom, and a fix
measured. Nothing has been applied to `platform/warp.c` — the patch is at the
bottom of this document. All citations are `file:line` into
`/home/agent-tom/Desktop/katam` (read-only).

## Summary

`PortWarp` does not hang on a bad cell. It hangs on a **destination that the
room Kirby is currently in has no door to**. `sub_080562D0`'s door branch calls
`sub_08002C18(currentRoom, destRoom, destX, destY)`
(`src/kirby.c:12876-12877`), which walks the current room's progress list
looking for a tag-2 entry whose `(destRoomId, destX, destY)` triple matches —
`for (i = 0;; i++)` with **no terminator test** and the terminator's stride
byte equal to `0` (`src/code_080023A4.c:1641-1659`). A key that is not in the
list therefore does not miss; the walk reaches the terminator and spins on one
address forever, writing nothing. That is the hang: a tight infinite loop
inside a single frame, which is why the harness's progress line stops dead
rather than repeating.

Room 803's list has **exactly one** tag-2 entry: `(804, 2, 5)`. That is the
whole reason `WARP=1320:804:2:5` was the only warp anybody had seen work — not
because `(2,5)` is a good cell, and not because it is an arrival cell.
`(56, 3)` is a genuine arrival cell for room 804, but it is the arrival of a
door from room **805**, so it is absent from 803's list and hangs. The rule the
previous investigation was reaching for is not a property of the destination at
all; it is a property of the *edge*.

`docs/naming/doors.md:218-222` and `:598-604` already stated this failure mode
in the abstract ("a missing key is an infinite loop, not a miss"). What was
missing was the connection to `PortWarp`: a synthesised door descriptor is
exactly the "re-pointed door not reflected in the shadow table" case that
document warns about.

The fix is not to constrain the cell. It is to stop using the door branch.
`sub_080562D0` has three room-assigning paths (`docs/naming/doors.md:521-539`)
and only the first calls `sub_08002C18`. The third — `roomId = kirby->roomId`
at `src/kirby.c:12913-12916`, the respawn / soft-warp path that warp stars and
`sub_080551FC` use — reaches the identical room-load block at `_080564F8` with
no door-table lookup at all. Driving that path instead makes every cell and
every room work.

---

## What I measured

Clean run, for scale: **3.1 s** wall for 1400 frames on this machine. A hang is
therefore obvious at `timeout 12`; nothing here is CPU starvation. Other agents
were running the same binary concurrently, so individual timings drift by a few
seconds — which is why the hang classification does not rest on the 12 s cutoff
alone: `804:3:5` was separately held for 40 s and 60 s and never emitted the
next progress line. Nothing in the sweeps below timed out, so contention never
produced a false hang either.

### 1. The safe/hanging cell map is not a map of cells

```
for c in "1 5" "2 5" "3 5" "4 5" "5 5" "2 4" "2 6" "2 3" "0 5"; do
  timeout 12 env WARP=1320:804:${c/ /:} HOLD=0x10 MASH=600:1:6 \
    node tools/headless_test.js build/katam-node.js ~/Desktop/katam/baserom.gba 1330
done
```

| cell | result |
|---|---|
| (2,5) | finishes, 3.0 s |
| (1,5) (3,5) (4,5) (5,5) (0,5) (2,3) (2,4) (2,6) | all hang |

Every neighbour of the working cell hangs, including cells with identical
collision content. `(2,5)` and `(3,5)` put Kirby on tiles `(1,4)` and `(2,4)`
respectively — `x = (cellX << 12) - 0x800`, so the tile is `cellX - 1` — and
**both of those tiles are empty** in the collision grid
(`python3 tools/room_export.py --rom … --room 804 --map`, row 4 is `.` from
column 0 to 21). So terrain does not distinguish them, and the "spawned inside
solid rock" reading is dead.

### 2. It is not about room 804 either

```
timeout 12 env WARP=1320:802:13:3 …   # 802's only arrival cell -> hangs
timeout 12 env WARP=1320:805:4:7  …   # 805's only arrival cell -> hangs
```

Both are arrival cells produced by `tools/room_export.py --spawn`, and both
hang. At that point the only surviving common factor is *room 803*, the room
the scripted run is in at frame 1320 (`DUMP_ROOM` with no warp reports
`room 803` at every frame from 900 on).

### 3. Room 803's progress list, read out of the ROM

Walking `gUnk_08D640A4[gRoomProps[803].doorsIdx]` by the procedure in
`docs/naming/doors.md:436-452`:

```
room 803  doorsIdx=1  header=0x08930E70  entries=0x08930E64  chestCount=0
  +0x00  02 08 FF FF  24 03  02 05     tag 2, dest room 0x0324 = 804, cell (2,5)
  +0x08  00 00 FF FF  ...              tag 0, stride 0  <- terminator
room 804  doorsIdx=2 -> one tag-2 entry: room 805 at (4,7)
room 805  doorsIdx=3 -> tag-1 chest, then tag-2: room 804 at (56,3)
room 802  doorsIdx=213 -> one tag-2 entry: room 816 at (2,4)
```

The single working warp in the entire investigation is byte-for-byte the single
tag-2 entry in the source room's list. `(56,3)` lives in **805's** list, which
is why it is an arrival cell and still hangs.

### 4. Inverting the symptom by changing two ROM bytes

The decisive test. The port keeps the ROM image in the wasm heap at
`0x08000000`, so the record at `0x08930E64` is writable from the harness. I
copied `tools/headless_test.js` to a scratch file and added a `ROMPOKE=` byte
poke at frame 2 (nothing in the repo was modified). `destX`/`destY` are at
`+6`/`+7` of the record, i.e. `0x08930E6A`/`0x08930E6B`; `destRoomId` is the
`u16` at `0x08930E68`.

| door record rewritten to | warp asked for | before | after |
|---|---|---|---|
| `(804, 3, 5)` | `804:3:5` | hang | **finishes, 3.07 s** |
| `(804, 56, 3)` | `804:56:3` | hang | **finishes, 3.11 s** |
| `(805, 4, 7)` | `805:4:7` | hang | **finishes, 3.08 s** |
| `(804, 3, 5)` | `804:2:5` | works | **hangs** |

The safe/hanging map moves with the door record and with nothing else. The last
row is the important one: the previously-unique working cell becomes a hang the
moment the record stops naming it. Two bytes (four, for the row that also
changes the destination room) control the entire symptom, and they are bytes of
a save-progress lookup table that has nothing to do with geometry, physics, or
graphics. Note the poke changes *only* what `sub_08002C18` searches for — the
solidity map, the collision grid, the tilemaps and the object lists are all
untouched, so no other explanation survives it.

---

## Root cause

`src/code_080023A4.c:1641-1659`:

```c
void sub_08002C18(u16 roomId, u16 arg1, u8 arg2, u8 arg3)
{
    struct Unk_08930E00_2 *var0 = gUnk_08D640A4[gRoomProps[roomId].doorsIdx]->unk00.pat2;
    u16 i;

    for (i = 0;; i++) {
        if (var0->unk00 == 2 && var0->unk04 == arg1
            && var0->unk06 == arg2 && var0->unk07 == arg3) {
            gVisitedDoors[gRoomProps[roomId].doorsIdx] |= 1 << i;
            return;
        }
        var0 = (void *) var0 + var0->unk01;      /* stride; 0 at the terminator */
    }
}
```

**The condition that never becomes true** is
`var0->unk00 == 2 && var0->unk04 == destRoom && var0->unk06 == destX &&
var0->unk07 == destY`, evaluated against the progress list of the room Kirby is
*leaving*. There is no `i < count` bound and no `unk00 == 0` break. The
terminator entry carries `unk01 == 0` (`docs/naming/doors.md:207-208`,
`:218-222`), so once `var0` reaches it the pointer stops advancing and the loop
re-tests the same eight bytes forever. No memory is written, no task runs, the
frame never completes — which matches the observation exactly: the
`frame N: DISPCNT=…` line for the warp frame prints (the warp is delivered from
`portPresent`, after the frame) and the next one never does.

The call site is `src/kirby.c:12876-12877`, inside the door branch of
`sub_080562D0`:

```c
if (kirby->base.base.base.unk58 & 0x4000) {
    if (kirby->base.base.base.unk62 & 0x10) {
        ...
        if (gUnk_0203AD30 > *fake)                      /* *fake = playerId */
            sub_08002C18(kirby->base.base.base.roomId,  /* the SOURCE room */
                         r4.pat3->unk08, r4.pat3->unk0A, r4.pat3->unk0B);
```

`platform/warp.c:113-114` sets `unk58 |= 0x4000` and `unk62 |= 0x10` precisely
to select this branch, and `platform/warp.c:107` hands it a descriptor the
shadow table has never heard of. The gate `gUnk_0203AD30 > playerId` is true
for player 0 in a single-player run, so the call always happens.

`gVisitedDoors` bookkeeping is the *only* thing `sub_08002C18` does. Its
purpose is to light up a door on the pause map. For a warp there is no door to
light up, so the call is not merely dangerous, it is semantically wrong.

### The rule, stated exactly

> `PortWarp(p, room, x, y)` as currently written terminates **iff** the room
> Kirby is in at delivery time has a tag-2 entry in
> `gUnk_08D640A4[gRoomProps[currentRoom].doorsIdx]` whose
> `(destRoomId, destX, destY)` equals `(room, x, y)`.

That is derived from the loop, not fitted to the data, and it predicted all
four rows of the inversion table before I ran them. It is also useless as a
warp rule: the destinations it permits are exactly the current room's outgoing
door edges, which is "walking", the thing `PortWarp` exists to avoid. 33 rooms
have no outgoing edges at all (`docs/naming/doors.md:541`).

---

## Fix

Stop taking the door branch. `sub_080562D0` assigns `roomId` from three places
(`docs/naming/doors.md:521-539`) and **only the door branch calls
`sub_08002C18`**. The third path, `src/kirby.c:12913-12916`:

```c
sb = 1;
kirby->base.base.base.roomId = kirby->roomId;
kirby->base.base.base.x = (kirby->spawnLocation.x * 0x1000) - 0x800;
kirby->base.base.base.y = (kirby->spawnLocation.y * 0x1000) - 0x701;
```

reaches the same `_080564F8` label (`:12937`) and therefore the same
`FillLevelInfo` / `sub_0800EE04` / `sub_08033674` / `sub_0803CD40` room load —
the identical work, minus the shadow-table lookup. It computes Kirby's landing
position with the *same* arithmetic as the door branch (`:12879-12880`), so the
destination cell means exactly what it meant before.

The *second* path (`:12901-12911`, the object-carried transition) also skips
`sub_08002C18`, but it is not usable: it takes the room id from
`objBase->unk63`, which is declared `s8` (`include/data.h:365`) and so cannot
express room 804 at all.

This is the game's own soft-warp. `sub_080551FC` (`src/kirby.c:12406-12422`)
is a two-line wrapper over it, and `src/warp_star.c:1801-1803` is the actor
that uses it — a warp star is a room transition with no door record, which is
the same shape as what `PortWarp` wants to be. (`sub_080551FC` itself is not
callable here: it refuses unless `unkC & 0x1000` is set and `hp > 0` and
`animationIndex` is in range, state a scripted warp cannot guarantee.)

### Proposed patch

```diff
--- a/platform/warp.c
+++ b/platform/warp.c
@@
-static struct Unk_0888562C_3 sDoor;
-
 u32 gPortWarpCount;
@@ static void Deliver(u8 playerId, u16 room, u8 cellX, u8 cellY)
 {
     struct Kirby *kirby = &gKirbys[playerId];
 
-    sDoor.unk00 = 3;            /* tag: this cell is a door */
-    sDoor.unk01 = 0;
-    sDoor.unk02 = 0;
-    sDoor.unk03 = 0;
-    sDoor.unk04 = sizeof(sDoor);
-    sDoor.unk08 = room;         /* destination room */
-    sDoor.unk0A = cellX;        /* destination cell, 16px units */
-    sDoor.unk0B = cellY;
-    sDoor.unk0C = 0;            /* bit 7 would suppress the respawn update */
-
-    kirby->unk10C.pat3 = &sDoor;
-
-    kirby->base.base.base.unk58 |= 0x4000;
-    kirby->base.base.base.unk62 |= 0x10;
-
     sub_08055920(kirby);
 
+    /* Take the RESPAWN branch of sub_080562D0 (src/kirby.c:12913), not the
+     * door branch (:12867).  The door branch calls sub_08002C18 to light the
+     * door up on the pause map, and that function walks the source room's
+     * progress list with `for (i = 0;; i++)` and no terminator test
+     * (src/code_080023A4.c:1646).  The terminator's stride byte is 0, so a
+     * destination that room has no door to is not a miss -- the pointer stops
+     * advancing and the game spins forever inside one frame.  A synthesised
+     * door is by definition not in that table, so the door branch can only
+     * ever deliver Kirby to the current room's real door destinations, which
+     * is the thing PortWarp exists to avoid.  See docs/naming/warp-hang.md.
+     *
+     * Both bits must be actively CLEARED, not merely left unset: Kirby can be
+     * standing on a real door tile when the warp is asked for, and then the
+     * collision sweep has already set unk58 & 0x4000 for him.  sub_08055920
+     * sets flags bit 8, which makes the sweep skip this object, so what is
+     * written here survives to the frame sub_080562D0 runs on. */
+    kirby->base.base.base.unk58 &= ~0x4000;
+    kirby->base.base.base.unk62 &= ~0x10;
+
+    /* The destination, in the fields that branch reads. */
+    kirby->roomId = room;
+    kirby->spawnLocation.x = cellX;
+    kirby->spawnLocation.y = cellY;
+
+    /* sub_08055920 sets flyTimer to 5 for two animation states (:12593), and
+     * a non-zero flyTimer makes the respawn branch overwrite the respawn
+     * record from unkF6/unkF8/unkFA (:12921-12925).  The current warp still
+     * lands correctly -- that write happens after the position is taken -- but
+     * the *next* one would inherit a respawn point the port never chose. */
+    kirby->flyTimer = 0;
+
     gPortWarpCount++;
     PortLog("[katam-port] warp: player %u -> room %u at cell (%u,%u)",
             (unsigned)playerId, (unsigned)room,
             (unsigned)cellX, (unsigned)cellY);
 }
```

`struct Unk_0888562C_3` and the `sDoor` static go away entirely, and with them
the only place the port fabricated a ROM data structure.

The file's header comment (`platform/warp.c:11-52`) and the corresponding
section of `docs/OBSERVING.md:36-59` both explain the door mechanism and stop
being true with this patch. The replacement claim is smaller and stronger:

> A room change is a Kirby *state*, not three field writes. `sub_08055920`
> installs the transition state; `sub_080562D0` then reads the destination from
> **`kirby->roomId` and `kirby->spawnLocation`** — the same two fields a warp
> star writes (`src/warp_star.c:1801-1803`) and the same ones the game reads
> when Kirby respawns after a death. The port fills them in and calls the
> game's own entry point; everything after that is the game's code. The door
> descriptor route was tried first and abandoned: it works only for
> destinations the current room already has a door to, because the transition
> also updates the pause map's door bits through a table walk that spins
> forever on a key it cannot find. See `docs/naming/warp-hang.md`.

Note the field-name collision that makes this easy to misread. `roomId`
appears twice on a Kirby: `base.base.base.roomId` at `+0x60` is the live
object's room, and writing it from outside really does nothing —
`docs/OBSERVING.md:38-42` is still correct about that. `kirby->roomId` at
`+0x106` (`include/kirby.h:80`) is the *respawn record*, an input the
transition state reads. The patch writes the second, never the first.

### Evidence the patch works, without rebuilding

The patch writes its fields *after* `sub_08055920` returns, and
`sub_080562D0` does not run until the **next** frame (`unkD9 == 0` → case 0 of
`sub_08055D9C`, `src/kirby.c:12695-12705`). So calling the existing
`_PortWarp` and then rewriting exactly those fields from JavaScript, in the
same `portPresent` callback, leaves the Kirby in bit-for-bit the state the
patched `Deliver` would — same fields, same values, same point in the frame.
I did that (offsets from `include/kirby.h`:
`roomId` `0x106`, `spawnLocation` `0x108`, `flyTimer` `0x0D8`; `gKirbys` at
`0x02020EE0`, `katam.map:3275`):

| warp | stock | with the respawn path |
|---|---|---|
| `804:3:5` | hang | 3.26 s, lands in 804 |
| `804:56:3` | hang | 3.27 s, lands in 804 |
| `805:4:7` | hang | 3.28 s, lands in 805 |
| `802:13:3` | hang | 3.33 s, lands in 802 |
| `804:2:5` | 3.14 s | 3.26 s, lands in 804 |

Zero port diagnostics in all five. `tools/room_export.py --check` against each
`DUMP_ROOM` dump: **all 793 / 988 / 1643 collision bytes match** the static
reconstruction, for rooms 804, 805 and 802 respectively — the same check the
286-room sweep runs, passing on rooms that could not be reached at all before.
The room slot (`unk65E`) is 1 in every case, including the stock door-path
control, so the respawn path does not land Kirby in a different slot.

Then the breadth check, run serially, one game process at a time,
`timeout 12` against a 3 s baseline:

**Room 804, coarse cell grid** — all 143 of `cellX ∈ {0, 6, 12, …, 60}` ×
`cellY ∈ 0..12` (the room is 61×13), plus the corners `(0,0)` and `(60,12)` and
three deliberately out-of-range cells `(70,3)`, `(200,200)`, `(255,255)`:

```
148 runs   148 finished   0 hangs   148 landed in room 804   0 port diagnostics
```

Cells all over the room — solid, empty, floor, ceiling, open air — and cells
outside the room entirely. None of the 148 is `(2,5)`, so by the rule above the
stock door path hangs on every one of them; the nine stock runs in the table
further up are the measured instance of that (one worked, eight hung, and the
one that worked was `(2,5)`).

**30 rooms spread across the whole id space**, each at the cell
`tools/room_export.py --spawn` picks for it — 101, 114, 127, 137, 146, 175,
193, 208, 218, 227, 304, 321, 402, 412, 421, 501, 512, 526, 535, 544, 557, 604,
614, 690, 710, 731, 740, 802, 819, 829:

```
30 runs   30 finished   0 hangs   30 landed in the requested room   0 port diagnostics
```

**Zero** of those 30 destinations is in room 803's outgoing key set (which
contains the single triple `(804, 2, 5)`), so under the stock door path all 30
are hangs. This is the measurement that matters for the 286-room sweep: it is
the sweep, at 1/10 scale, working.

**Six of the 18 rooms no door in the game ever arrives at** — 415, 910, 913,
916, 950, 956 — each at the `--spawn` `floor` fallback cell (an empty cell with
a solid one under it), which is a *guess*, not an arrival:

```
6 runs   6 finished   0 hangs   6 landed in the requested room   0 port diagnostics
```

These are the rooms that cannot be reached by playing at all, at cells no door
record anywhere in the ROM names. They are the hardest case for the old rule —
under it they are unreachable by construction — and they are indistinguishable
from any other room under the new one. Two rooms in the 30-room sweep (412 and
710) are also in the separate set of 33 rooms with no *outgoing* door edge, and
those loaded too.

**Landing position is exact.** `WARP=1320:804:56:3` with the respawn path puts
`gKirbys[0].base.base.base.x` at `0x37800` and `.y` at `0x28FF`, which is
`(56 << 12) - 0x800` and `(3 << 12) - 0x701` — the door branch's own arithmetic
(`src/kirby.c:12879-12880`), to the bit. The cell argument keeps exactly the
meaning it had.

**It stays healthy.** The same warp run out to 3000 frames — 1680 frames, 28
seconds of game time, after the transition — finishes in 7.0 s with 0 port
diagnostics and audio still tracking the frame count 1:1.

### Why this generalises past room 804

The argument is about which function is called, not about which room. The hang
is `sub_08002C18` and nothing else, and the respawn branch does not call it —
there is no room-specific data on that path that could reintroduce it. The
other two `for (;;)` walkers over the same lists, `sub_08002B30` (chests) and
`sub_08002BA8` (door-used query), are not reached from a transition at all.

What the respawn path still does, correctly:

- `sub_08002A80(destRoom)` at `src/kirby.c:12952` is **outside** the branch, so
  bit 15 of `gVisitedDoors` — "this room has been entered" — is still set for
  the destination, and the pause map still shows the room as visited.
- The world-props save buffer is still flushed (`:12958`, `:12963`).
- `kirby->roomId` / `spawnLocation` end up equal to the destination, which is
  the same end state as a door with `unk0C & 0x80` clear (`:12883-12885`). A
  death after a warp respawns in the warped-to room, which is what a player
  would expect.

What it does not do, correctly: it does not set the per-door "this door has
been used" bit, because no door was used.

The two behavioural differences from the door branch are both small and both
visible: Kirby's facing comes from `unkC & 0x800` (`:12917-12920`) instead of
from the descriptor's `unk0C & 1` / `& 2` (`:12887-12895`), and `sb` becomes 1,
which is only read by `sub_0800EE04` on the AI-Kirby path
(`gUnk_0203AD30 <= playerId`, `:12946`) and so is dead for player 0.

---

## What I could not determine

- **Why `sub_08002C18` has no bound.** `sub_08002AD0` right above it
  (`src/code_080023A4.c:1597`) loops to `chestCount`, so the author had a bound
  available and did not use one. Vanilla data makes the search total, so this
  is not reachable in the retail game; whether that is deliberate or an
  oversight is not decidable from the code.
- **Whether the respawn path is right for boss and hub rooms.** I measured 38
  distinct rooms and all of them loaded, but I did not check that a warped-into
  boss room arms its fight, or that room `0x321` — the hub, special-cased at
  `src/kirby.c:12714` and `:12725` — behaves. The sweep proves rooms *load*;
  `docs/OBSERVING.md:180` already says it does not prove they are playable, and
  that limit is unchanged by this fix.
- **Whether the `--spawn` `floor` fallback cell is a good place to stand.** The
  six no-arrival rooms loaded from it and ran, but "the room loaded and the
  process exited" is weaker than "Kirby is somewhere a person would want to
  start". I did not look at a frame.
- **A factual slip in the current `platform/warp.c:8`,** which says 33 rooms
  have no *incoming* door edge. Reading the tables: **33** rooms have no
  *outgoing* edge and **18** have no incoming one (415, 803, 909-916, 918, 919,
  950, 951, 955-957, 969 — `tools/room_export.py`'s own `spawn_cell` docstring
  says 18 as well). The header is being rewritten anyway; this should be fixed
  in the same pass. Note room **803** is on that list, which is a small irony:
  the room the scripted run parks in is one nothing arrives at.
- **What `gUnk_0203AD30` is exactly.** I read it as "number of human players"
  from the `> playerId` / `<= playerId` pairs at `src/kirby.c:12876`, `:12946`,
  `:12948` and `include/data.h:348-349`, and did not verify it. Nothing in the
  fix depends on it, because the respawn branch is taken regardless of its
  value — but a reader who wanted to suppress `sub_08002C18` by moving that
  gate instead would need to settle it first, and that approach also flips
  `sub_0800EE04` and the save flush, which is why I did not take it.
- **Whether a warped-to cell can still be bad in a non-hanging way.** Cells
  outside the room, and cells inside solid terrain, no longer hang — but I only
  checked that the run completes and the room loads, not that Kirby ends up
  somewhere sensible. `sub_08055D9C` case 2 guards its collision read with a
  `levelMinPosition`/`levelMaxPosition` bounds test (`src/kirby.c:12754-12757`),
  so an out-of-range cell is skipped rather than read out of bounds; that is a
  code reading, not a measurement of what the player would see.
- **Whether `tools/room_export.py --spawn` should change.** Its docstring
  (`:297-308`) encodes the refuted hypothesis — it says the difference is that
  `(2,5)` is an arrival cell. With the patch, arrival cells stop being special
  and `--spawn` can pick any reasonable floor cell. I did not touch the file:
  other agents depend on it, and the docstring is only wrong once the patch
  lands.
