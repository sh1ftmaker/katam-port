# Observing the game

Three instruments, and a workflow that uses them together to answer the
question that gates most of the remaining work: **what does this undocumented
global actually do?**

83.7% of this game's symbols are still `sub_XXXXXXXX` or `gUnk_XXXXXXXX`.
Static reading can tell you what a value is *tested* for. It cannot tell you
what makes it move. These close that gap.

| | |
|---|---|
| **`PortWarp`** (`platform/warp.c`) | put Kirby in any of the 286 rooms, including the 33 that cannot be reached by playing |
| **`DIFF=`** (`tools/headless_test.js`) | snapshot EWRAM and IWRAM at two frames and report what changed |
| **`tools/symbolize_diff.py`** | turn changed addresses into named globals, and subtract a control run |

---

## 1. The warp

```
WARP="frame:room[:cellX:cellY]"
```

```
WARP=1320:804:2:5 HOLD=0x10 MASH=600:1:6 \
    node tools/headless_test.js build/katam-node.js <rom> 1500
```

Cell coordinates are in 16-pixel units and default to (3,5). Requesting a warp
before the game is in a level is fine — the port holds the request and
delivers it on the first frame that can take it, rather than dropping it.
A dropped warp and a warp that worked and did nothing look identical from
outside, which is why it queues.

### Why it is a port call and not a heap poke

Taking a door writes `roomId`, `x` and `y` on the Kirby. Writing those three
fields from outside does nothing — and it fails in the least helpful way
available: **the write sticks and nothing acts on it.** `gKirbys[0].roomId`
reads back as the new room while `gCurLevelInfo` stays on the old one, so the
game is now internally inconsistent and still in the wrong place.

A room change is a Kirby *state*. `sub_08055920` installs `sub_08055D9C` as the
state function, and on the next frame that calls `sub_080562D0`, which assigns
`roomId` from three different branches. Which branch runs decides everything.

`PortWarp` takes the **respawn** branch: it writes `kirby->roomId` and
`kirby->spawnLocation` — the same two fields a warp star writes
(`src/warp_star.c:1801`) and the same ones the game reads when Kirby respawns
after dying — and calls the game's own entry point. Everything after that is
the game's code, so the state it lands in is the state a player would have
arrived in. Same shape as `platform/rando.c`: the port supplies data, the game
does the work.

Mind the field-name collision. `base.base.base.roomId` at `+0x60` is the live
object's room, and writing it from outside really does nothing.
`kirby->roomId` at `+0x106` is the *respawn record*, an input the transition
state reads. `PortWarp` writes the second.

Installing a state means writing a function pointer, and in WebAssembly a
function pointer is a table index. That is the reason this cannot live in the
harness: JavaScript has no way to name a function the module never exported.

### The door route, tried first and abandoned

The obvious approach is the **door** branch: synthesise a door descriptor,
point `kirby->unk10C` at it, set `unk58 & 0x4000` (the DOOR metatile bit) and
`unk62 & 0x10`, and let the game take the door. That is what this did first,
and it hangs.

The door branch also marks the door used on the pause map, through
`sub_08002C18`, which walks the **source** room's progress list looking for the
destination with `for (i = 0;; i++)` and **no terminator test**
(`src/code_080023A4.c:1646`). The terminator's stride byte is zero, so a
destination the source room has no door to does not miss — the pointer stops
advancing and the game spins forever inside one frame, writing nothing.

A synthesised door is never in that table. So the door route can only deliver
Kirby to destinations the current room *already has a real door to*, which is
precisely the limitation a warp exists to remove.

It looked like it worked for one reason: the room a scripted run ends up in
(803) has exactly **one** door, to room 804 cell (2,5), and that was the only
warp anyone had tried. Two plausible theories died on this — "the cell must be
walkable" and "the cell must be one a real door arrives at" — and the second
was refuted by a cell that *is* an arrival cell and still hung, because it was
an arrival cell of a *different room's* door.

`docs/naming/warp-hang.md` is the full account, including the experiment that
proved it: changing two bytes of room 803's door record makes the previously
hanging destinations work and makes the previously working one hang, with
nothing about geometry or collision touched.

---

## 2. The differ

```
DIFF="frameA:frameB"   DIFF_OUT=build/ram-diff.json
```

Snapshots EWRAM (256K) and IWRAM (32K) at each frame and reports the runs of
bytes that differ. The game's entire mutable state is 288K, so a snapshot is a
`memcpy` and a diff is a scan — no watchpoints, no single-stepping, no
instrumented build. The frame loop is already ours and the memory is a flat
array.

I/O, palette, VRAM and OAM are excluded on purpose: they change every frame in
ways that say nothing about game state, and including them buries the signal.

Adjacent changes are coalesced across gaps of up to 3 identical bytes, so an
`s32` whose middle bytes happen to match is reported as one fact rather than
three.

---

## 3. Naming, and the control run

A single diff is mostly noise: timers tick, RNG advances, sprites animate. The
measurement that means something is **the difference between two runs that
differ in exactly one thing.**

```
# the thing happens
DIFF=1310:1400 DIFF_OUT=build/diff-warp.json WARP=1320:804:2:5 \
    HOLD=0x10 MASH=600:1:6 node tools/headless_test.js ... 1420

# identical run, thing does not happen
DIFF=1310:1400 DIFF_OUT=build/diff-control.json \
    HOLD=0x10 MASH=600:1:6 node tools/headless_test.js ... 1420

python3 tools/symbolize_diff.py build/diff-warp.json \
        --minus build/diff-control.json --min 4
```

`--minus` drops every run whose address also moved in the control. Matching on
address alone is deliberately strict: a counter that ticks in both runs is
noise even when it ticks to *different* values, and keeping it because the
bytes differ would defeat the point of having a control.

### The method validates itself on a known answer

Run against a room transition, the top of the list is:

```
where                            bytes  before -> after
gUnk_02028EE0+19714               2229  00000000... -> 8e048d04...
gUnk_02024ED0+2491                 252  00000000... -> 0d0d0d0d...
gUnk_03003A04+1218                 181  40010000... -> 00000000...
```

`gUnk_02028EE0` is the decompressed tilemap buffer and `gUnk_02024ED0` is the
collision grid — both of which we already know, independently, are exactly what
a room load writes (`wip/code.c:431-434`). The tool rediscovered two symbols
whose meaning was established by a different method entirely.

That is the reason to trust it on the symbols we *haven't* decoded. A tool that
gets the known answers right on a question where the answer was obtained
another way has earned a hearing on the unknown ones. `gUnk_03003A04` is not
in `docs/naming/`; it is now a candidate with evidence attached.

### Naming discipline

The output is a **candidate with evidence**, not a name. The rules from
`docs/naming/` still apply: cite the sites, say what contradicts you, and
distinguish "this is the only thing it can mean" from "this is the best of
several readings". A confidently wrong name is worse than `gUnk_03003A04`,
because the next person builds on it and nobody re-checks.

---

## 4. The sweep

```
tools/room_sweep.sh <rom.gba>            # every room in the ROM
tools/room_sweep.sh <rom.gba> 126 219    # named rooms
```

Visits each room and records the live room state, the last frame as a PPM, and
the port's diagnostics. Then it checks each room's live collision grid against
the static reconstruction from `tools/room_export.py`.

That check is why the sweep is worth running rather than just looking at
screenshots: it is **286 independent comparisons** between what the ROM says a
room is and what the game builds when it goes there. A disagreement is a real
finding about the format or the port, and it is the kind of thing no amount of
reading finds.

Room ids come from the ROM's own table rather than a range, because they are
not contiguous and do not start at zero.

---

## 5. What this does not do

- **It does not tell you which function wrote a byte.** The diff says *what*
  changed between two frames, not *who* changed it. `PortSetWatch` reports the
  writer, but only for block transfers (DMA/`CpuSet`) — it is wired into those
  macros and sees nothing else. For a plain store there is currently no
  instrument.
- **It cannot separate two things that always happen together.** If a global
  only ever moves during a room load, the diff cannot tell you whether it is
  about rooms, about the fade, or about the object teardown.
- **`--minus` matches on address.** Two different events that touch the same
  address are indistinguishable, and a control run that happens to touch an
  address once will mask it forever.
- **The sweep proves a room *loads*, not that it is playable.** It checks
  geometry and collision, and it counts diagnostics. It does not know whether
  the room is winnable, whether its objects behave, or whether a boss fight
  completes.
