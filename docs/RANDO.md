# The randomizer

An enemy and boss shuffle on the `quality-of-life` branch. Off by default;
the stock game is still what the page loads with nothing switched on.

The short version:

| | verdict |
|---|---|
| **Enemy shuffle** | works, verified in play, and costs one byte per level record. 1,618 of the game's 3,482 placements change on a typical seed |
| **Mid-boss and boss shuffle** | implemented, and verified only as far as the substitution. Nobody has fought a randomized boss yet — see "What is not verified" |
| **Matching enemy sizes** | on by default, and it is the only thing standing between a shuffle and a corrupted tile heap |
| **Keeping abilities where they were** | on by default, and it is a hedge rather than a guarantee |

---

## 1. The mechanism, which is one byte

Every enemy, item, door and block in a KATAM level is a 36-byte record in the
room's object list. Byte 12 is its type. The game turns that byte into a
constructor by indexing `gSpawnFuncTable1` — 219 entries, one per type — and
hands the constructor the whole record.

So a randomizer is: change byte 12.

What makes that enough, rather than the first step of a long job, is that
KATAM allocates an object's graphics **when the object is created**, not when
the room loads. `sub_0809A630` calls `VramMalloc(gUnk_08351648[type].numTiles)`
for a new object and `sub_0803DFAC` loads its palette on demand. There is no
per-room tileset to keep in step, no sprite sheet to rebuild, no palette slot
to reassign. A substituted enemy brings its own everything.

This was the deciding measurement. A port where graphics were loaded per room
would have needed the room's tile budget recomputed and re-uploaded, which is
a different and much larger piece of work.

### Where the substitution happens

Not at the constructor call. There are three of those — the room-entry rescan
in `CreateObject` and the two scroll-in spawns in `sub_0800A2B4` and
`sub_0800A460` — and all three take a `const struct Object *`, and the
constructor **keeps that pointer**: `struct Object2` has an `object`
back-pointer that `sub_0809A630` dereferences every frame the object is alive.
A hook at the call site would have to hand out rewritten records that outlive
the call, own a pool of them, and know when each one died.

The room's list has exactly that lifetime already. `FillLevelInfo` does

```c
levelInfo->objlistPtr = gLevelObjLists[gRoomProps[room].objectListIdx].obj;
```

once per room load, and all three walks start from that pointer. So
`tools/portify.py` routes that one line through `PortRandoObjList`, which
copies the list, rewrites the type bytes, and returns the copy. Every consumer
downstream agrees about what is in the room, because there is only one list.

Two buffers per player, used alternately. One would do if a room load were
guaranteed to have destroyed every object from the previous room first — it
appears to be — but objects hold pointers into these bytes and the cost of
being wrong is a read through a pointer to a record that has become a
different record. The second buffer makes a stale read return the previous
room's data, which is inert. The largest object list in the ROM is 30 records;
the buffer holds 2 KB, and a list that would not fit is passed through
untouched and reported.

---

## 2. What it will and will not swap

The destination pool is **the set of types the retail game actually places**,
measured by walking all 287 object lists in the ROM. A type that ships in a
level is known to work as a level-placed object. A type that does not is an
unknown, and several of the unplaced ones in the enemy range turn out to be
aliases: `OBJ_EMPTY_2C` builds a Droppy, `OBJ_EMPTY_31` a Cookin.

- **Enemies** — 49 types, from Minny at 4 tiles to Sparky at 90.
- **Mid-bosses** — the eight in `0x38..0x3F`, Mr Frosty through Bombar.
- **Bosses** — the seven in `0x45..0x4B`, Kracko through Mega Titan.

Everything else in a room — doors, chests, buttons, blocks, items, ability
statues, cannons, the object spawner — is outside every pool and is left
exactly as the level data has it. That is 1,640 of the game's 3,482
placements.

Excluded deliberately, with reasons:

| | |
|---|---|
| `OBJ_SHADOW_KIRBY` | six scripted story encounters, not enemy placements. Removing one removes the scene; putting one in a corridor starts the scene there |
| `OBJ_DARK_META_KNIGHT`, `OBJ_DARK_MIND_FORM_*` | the endgame sequence. `0x51` is a *trigger*, not a boss |
| `OBJ_DARK_MIND_BOMB` | boss ordnance |
| `OBJ_TITAN_HEAD` | a component of Mega Titan, placed by the fight |
| `OBJ_CRAZY_HAND_2` | a variant spawned by its own boss |

---

## 3. The two things that can go wrong

### Video memory, which is the real one

The object tile heap in a level is `0x200` tiles — 512, set by
`sub_080332BC` — and it is shared with Kirby, the HUD and every effect.
`VramMalloc`'s answer to a full heap is **not** a failure. It returns
`ewram_end`, a real address, so the game writes tiles past the end of EWRAM
and carries on looking healthy.

Enemy tile counts run from 4 to 90. A blind shuffle can put four Sparkys in a
room built for four Waddle Dees.

So the default swaps within a size class, cut at the gaps in the pool's own
distribution: `<=12`, `<=25`, `<=36`, and above. Four classes rather than
three because three was measured and was not good enough — a single "28 and
up" class holds both Sword Knight at 28 and Sparky at 90, and
`tools/rando_preview.py` found the worst room in the game going from 40 tiles
to 102 under it. Splitting at 36 caps the worst single swap at +34 tiles and
the worst room at +51.

Measured over the whole ROM at seed 1:

| | default (`0x11`) | `PORT_RANDO_ANY_SIZE` (`0x19`) |
|---|---|---|
| placements replaced | 1,618 | 1,761 |
| rooms asking for more tiles than they shipped with | 117 | 133 |
| worst room | 352 → 403 (+51) | 214 → 401 (+187) |

Both numbers overstate the risk, because they total a room's whole object list
and the game only allocates for objects inside the spawn window. They are
still the right relative signal, and the right-hand column is why the setting
that produces it says so on the page.

### Abilities, which is a hedge

Some rooms want an ability that one enemy carries. A shuffle that replaces the
only Sword Knight with a Waddle Dee can leave a player without a tool a later
block needs.

`gUnk_08351648[type].kirbyAbility` says which enemies carry one, so by default
an ability-carrying enemy is replaced by another ability-carrying enemy. This
does **not** preserve which ability — preserving that would pin most enemies
to themselves, since most abilities have exactly one source. It is the
difference between "a different ability is here" and "no ability is here",
which is where the soft locks live. It is not a logic guarantee and nothing
here does route logic.

---

## 4. Determinism, and why the seed is a number you can read out

The mapping is a pure function of `(seed, room id, index in the room's list)`.
It reads nothing that changes while the game runs — not `gRngVal`, not the
frame counter, not which player triggered the load. So:

- the same seed is the same game, this session and the next;
- four players in a shared room compute the same list;
- two units on a link cable would agree, given the same seed. Nothing here
  sends the seed anywhere; agreeing on it is a transport's problem, and the
  desync detector in `docs/MULTIPLAYER.md` would end a session where the two
  ends disagreed.

The page shows the seed rather than hiding it, and remembers it in
`localStorage`. That is a deliberate difference from the view controls, which
are remembered nowhere: a run under a seed is something a player is partway
through, the save that goes with it is already persisted in IndexedDB, and
coming back to a stock game with a randomized save half-finished is the page
losing state rather than staying out of the way.

---

## 5. Using it

**On the page** — the **Enemies** panel. *As it shipped* / *Shuffled* /
*Shuffled, bosses too*, a seed with a Reroll button, and the two rules above.
A change **takes effect at the next door**: the room you are standing in
already has its list.

**Native** — `--rando SEED[:FLAGS]`, e.g. `katam rom.gba --rando 4127` or
`--rando 4127:0x17` for bosses as well.

**Headless** — `RANDO="flags:seed"`, applied at frame 0:

```
RANDO=0x11:1 HOLD=0x10 MASH=600:1:6 \
    node tools/headless_test.js build/katam-node.js <rom> 1300
```

The flag word, from `platform/port/port.h`:

| bit | |
|---|---|
| `0x01` | enemies |
| `0x02` | mid-bosses |
| `0x04` | bosses |
| `0x08` | ignore tile sizes — the risky one |
| `0x10` | keep ability enemies carrying abilities |

**Previewing a seed without playing it** — `tools/rando_preview.py` is a
second implementation of the same roll, in Python, over the player's own ROM:

```
python3 tools/rando_preview.py --rom <rom> --seed 4127            # whole game
python3 tools/rando_preview.py --rom <rom> --seed 4127 --room 803 # one room
python3 tools/rando_preview.py --rom <rom> --seed 1 --check       # see below
```

---

## 6. Verification

### The substitution reaches the screen

`RANDO=0x11:1 HOLD=0x10 MASH=600:1:6 ... 1300` and the same run with no
`RANDO` and with `RANDO=0x11:7` produce three different frames of the same
place in the same level: a Waddle Dee where the stock game has one, a Rolypoly
at seed 1, a Boxin at seed 7. Correct sprites, correct palettes, no corruption
in any of the three, and **zero port diagnostics** in all of them.

The port's own report for that run:

```
rando: flags=0x11 seed=1 (enemies size-matched keep-ability)
rando: flags=0x11 seed=1, 4 room load(s), 16 object(s) swapped
```

Four room loads for one room is right: `FillLevelInfo` runs per player slot.

### The Python model agrees with the C

`--check` asserts that the model reproduces a recorded run of
`platform/rando.c` — room 803, seed 1, flags `0x11`, indices 7/9/10/11 going
to `0x0B`/`0x32`/`0x10`/`0x23`:

```
check: room 803 at flags=0x11 seed=1 matches platform/rando.c (4 swaps)
```

That assertion is the whole reason the whole-ROM numbers in §3 are worth
printing. If the C changes and the model does not, it fails, and the numbers
stop being claimed rather than quietly becoming wrong.

### No regression with it switched off

A 300-frame boot and an 1,800-frame run to gameplay with no `RANDO` produce
the same summary as the build before this work: same first non-blank frame,
same `DISPCNT`, same colour count, same audio RMS and peak, zero diagnostics.
`PortRandoObjList` returns its argument unchanged when no pool bit is set, so
that is what should happen.

### What is **not** verified

Stated plainly, because the difference is the point:

- **No randomized boss has been fought.** The substitution is verified — the
  preview tool shows Wiz becoming Mega Titan in room 219 and Crazy Hand
  becoming Wiz in room 417 — but reaching a boss room takes a scripted run
  nobody has written. A boss room is a door that opens when the boss dies, and
  the trigger watches the object rather than the species, so it should hold.
  *Should* is the operative word, and it is why bosses are off by default.
- **Arena fit is unexamined.** Kracko flies and King Golem does not. A
  mid-boss dropped into a corridor built for a different one may be able to
  hit the player from somewhere the player cannot answer, or not be able to
  reach them at all.
- **Nothing checks route logic.** The ability hedge in §3 is a hedge. A seed
  that makes some part of the game unreachable is possible and there is no
  detector for it. The fix, if one is wanted, is a completability check in
  the preview tool, which would need the game's own room graph.
- **VRAM exhaustion has not been *observed*, only bounded.** The classing
  keeps the demand near stock and no run has produced a diagnostic, but no run
  has deliberately parked four large enemies on one screen either.
