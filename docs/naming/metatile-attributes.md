# `gUnk_082D88B8` — the metatile attribute table

**Declared:** `extern const u32 gUnk_082D88B8[];` at `include/data.h:806`
**Defined (matching C, not just asm):** `src/code_080023A4.c:71` — `const u32 gUnk_082D88B8[] = { ... }`, **256 entries**, ROM `0x082D88B8`.

The index is the u8 read from the room's per-16x16-cell solidity grid
(`sub_08002434` / `sub_08002470` / `sub_080024F0`, `src/code_080023A4.c:442/451/494`;
also `sub_080023E4`). The u32 result is a bitfield split into four independent
fields:

```
 31        28 27      24 23      20 19          16 15                        0
+------------+---------+----------+--------------+---------------------------+
| slope type | force   | level    | never set    | terrain property flags    |
| (enum 0-12)| dir mask| (0..4)   | (always 0)   |                           |
+------------+---------+----------+--------------+---------------------------+
   0xF0000000  0x0F000000  0x00F00000  0x000F0000        0x0000FFFF
```

Because the whole table is present as C source, every claim below is checked
against **both** the test sites *and* the 256 literal values. Where the two
agree the confidence is high; where a bit is only inferable from one, it is
called out.

A third, independent check — the spatial distribution of every metatile index
across all 286 real rooms, extracted with `tools/room_export.py` — is in
[Corroboration from level data](#corroboration-from-level-data) near the end.
It confirms 9 of the flag bits, both multi-bit enums, and shows that 45 of the
125 non-zero table entries are never placed in any room.

---

## Bit map

| mask | proposed name | confidence | what it gates |
|---|---|---|---|
| `0x00000001` | `TILE_EMPTY` | **high** | tile has no collision at all; suppresses the per-tile collision handler dispatch |
| `0x00000002` | `TILE_WATER` | **high** | swimming physics, water SFX/splash, buoyancy; selects the water wind-strength table |
| `0x00000004` | `TILE_TOP_SOLID` | **high** | one-way / jump-through platform: collides only from above |
| `0x00000008` | `TILE_DROP_THROUGH` | medium-high | on a `TILE_TOP_SOLID` tile, Kirby may duck-drop through it |
| `0x00000010` | `TILE_SLIPPERY` | **high** | ice: ground friction drops from 14/32/27 to 4/8/13 |
| `0x00000020` | `TILE_CHAIN_TRIGGER` | medium-high | breaking this block starts a chain explosion (`sub_0800E0E4`) instead of dropping debris |
| `0x00000040` | `TILE_CHAIN_MEMBER` | medium-high | a chain explosion propagates into / destroys this tile |
| `0x00000080` | *unknown* | — | **never tested anywhere**, and its one carrier (idx 92) is **never placed in any room** |
| `0x00000100` | `TILE_LADDER` | **high** | climbable rope/ladder |
| `0x00000200` | `TILE_SOLID` | **high** | full 16x16 block; walls, floors, ceilings |
| `0x00000400` | `TILE_MINI_ONLY` | **high** | passable only as Mini Kirby; instant death (hp = 0, crush) otherwise |
| `0x00000800` | `TILE_FORCE` | **high** | tile applies a directional push (wind / water current; conveyors exist in the table but are unused) — see `0x0F000000` |
| `0x00001000` | `TILE_BREAKABLE` | **high** | destructible block; strength required is in `0x00F00000` |
| `0x00002000` | `TILE_HAZARD` | **high** | damages Kirby on contact; AI marks the cell as "danger" |
| `0x00004000` | `TILE_DOOR` | **high** | door / mirror-warp anchor cell |
| `0x00008000` | *unknown* | — | **never tested anywhere**; set on exactly the 6 conveyor entries (idx 103-108), **none of which appear in any room** |
| `0x000F0000` | — | **high** | **never set in the table and never tested**; a 4-bit hole |
| `0x00F00000` | `TILE_LEVEL_MASK` | **high** | polysemous 0..4 magnitude: force strength, breakable hardness, hazard variant, door auto-warp flag |
| `0x0F000000` | `TILE_FORCE_DIR_MASK` | **high** | outward surface normal / push direction: `1`=up, `2`=down, `4`=left, `8`=right; OR-able for diagonals |
| `0xF0000000` | `TILE_SLOPE_MASK` | **high** | enum 0-12 selecting a slope-resolution handler (see below) |

Masks that appear literally in the source and are *composites* of the above:
`0xC` (= `TOP_SOLID|DROP_THROUGH`, 14 sites), `0x9` (= `EMPTY|DROP_THROUGH`, 3 sites),
`0xFFFFF` (the accumulate mask for `unk58`, 34 sites), `0xF0000004`, `0xF04000`,
`0x00F01000`, `0xF00000|0x1000`.

---

## Per-bit evidence

### `0x00000001` — `TILE_EMPTY`

**Confidence:** high (18 table sites + 3 `unk58` sites, plus the value layout)

**Tested at:** `src/code_080023A4.c:2074,2077,2080,2163,2188,3974,3989,4013` (+10 more:
`code_080023A4.c:4028,5712,5727,5780,5791`, `code_0800ECAC_2.c:818`,
`object.c:1676`, `boxin.c:140,142`); via `unk58` at `ufo.c:211`, `squishy.c:439`,
`twister.c:329`.

**What it does:** in the master Kirby-vs-terrain routine `sub_0800385C`
(`src/code_080023A4.c:2100`), the per-tile handler in `gUnk_082D8DA4` is
dispatched **only** when the bit is clear:

```c
// src/code_080023A4.c:2163
if ((gUnk_082D88B8[ugh] & 1) == 0)
    var0 = gUnk_082D8DA4[ugh](arg0, &stack0, arg1 | 0x420);
```

`sub_08003704` (`src/code_080023A4.c:2066`) samples three x-positions one pixel
below the feet and returns TRUE if *any* is empty — "not standing on anything".
`sub_0809D998` (`src/object.c:1670`) is the walking-enemy ledge check: tile below
the next step is empty → turn around. `sub_08011754_helper`
(`src/code_0800ECAC_2.c:812`) requires a run of empty cells for a clear path.

The value layout settles it: **`0x1` is set on exactly the entries that carry no
blocking geometry** — plain air (idx 0 = `0x1`), ladder body (33), wind cells
(28-31, 40-51, 81-88, 95-102, 109-112), doors (254, 255), water (72) — and is
**never** set on `SOLID`, `TOP_SOLID`, or any slope entry. `wip/flamer.c:840`
checks `collideFlags == 1` after OR-ing the four corner attributes, i.e. "all
four corners are exactly the empty tile".

**Against / unresolved:** `src/boxin.c:142` reads oddly — empty-below causes
`sub_080B5898()` + `yspeed = 0` and *skips* the gravity code, while non-empty
falls through to gravity. Every other site is consistent, so this is likely a
state transition I have not traced, not a counterexample to the bit's meaning.

### `0x00000002` — `TILE_WATER`

**Confidence:** high (307 `unk58` sites — by far the most-tested bit — plus 4 direct)

**Tested at:** `src/kirby.c:8749,8794,8811` and `code_0800ECAC_2.c:3483` directly;
via `unk58` at `kirby.c:6291,6387,6645,7081,7239,7371,7497,...` (307 total across
`kirby.c`, `blipper.c`, `haley.c`, `squishy.c`, `droppy.c`, `object.c`,
`code_0806F780.c`, `code_0800ECAC_2.c`, `dark_meta_knight.c`, `giant_rocky.c`,
`ability_objects.c`).

**What it does:** swimming. `kirby.c:20518` — `s16 r1 = unk58 & 2 ? 0x100 : 0x180;`
(slower in water). `kirby.c:6387` — `a = (unk58 & 2) ? 0x10 : 0x18;`.
`code_0806F780.c:13042-13048` — the landing-dust particle uses anim `0x2BD`
(splash) when the tile has bit 1, otherwise the normal dust anim.
`object.c:290` / `code_0806F780.c:7188` play SFX `0x296` on entering.
`code_080023A4.c:2415-2418` uses the bit to pick the water force table:

```c
if ((var2 & 0x800) != 0) {
    if ((var2 & 2) != 0) sub_080098C4(arg0, var2);   // water: gUnk_082D91BC
    else                 sub_080097C4(arg0, var2);   // air:   gUnk_082D91A4
}
```

Fish enemies (`blipper.c`, 8 sites) require it to keep moving.
It is also written/cleared manually: `kirby.c:10058` `unk58 |= 2`,
`kirby.c:15256` `unk58 &= ~2`, `code_0806F780.c:7246` `unk58 &= ~2` when off-map.

**Against / unresolved:** nothing. Note the survivor mask `kirby.c:8948`
`unk58 &= 0x4402` (state reset) deliberately keeps water, mini-gap and door.

### `0x00000004` — `TILE_TOP_SOLID`  (and `0x00000008` — `TILE_DROP_THROUGH`)

**Confidence:** `0x4` high; `0x8` medium-high.

**Tested at (0x4 / 0xC):** `src/code_080023A4.c:2610,2654,5274,5318,5447,5491`
(bare `& 4`); `& 0xC` at `code_0806F780.c:797,799,1319,1332,1345,1358,1371,1384`
(+6 more at 1397,1411,1425,1438,1451,1464), `code_0806F780.c:7249`,
`shooty.c:592`; `(x & 0xF0000004) == 4` in `Macro_08008484`
(`src/code_080023A4.c:462`) and `src/code_080023A4.c:4347`.

**What it does:** collides only from above.
`code_0806F780.c:7249` — `if (objBase->unk58 & 0xC && objBase->unk4C < objBase->y) objBase->unk62 |= 4;`
(`unk4C` is last frame's y; only register floor contact when moving downward).
`shooty.c:592` is identical. `Macro_08008484` snaps Kirby's feet onto a tile whose
attribute is `SOLID`, or `TOP_SOLID` with no slope. In `sub_0806FC70`
(`code_0806F780.c:796`) a `0xC` tile counts as a collision only when the slope
nibble is absent, and the slope handlers all begin
`if (!(attr & 0xC) || <moving into the surface>)`.

`0x8` never appears without `0x4`. Its one isolated read is `sub_080035F4`
(`src/code_080023A4.c:2029`):

```c
u32 ugh = gUnk_082D88B8[...];
if ((ugh & 9) == 0) return 0;   // neither empty nor drop-through -> can't
var0 |= ugh & 8;                // ...and something under me must have bit 3
```

The caller is `kirby.c:7277`:
`counter > 4 && sub_080035F4(...) && !sub_08009D70(...)` → set flag `0x1000`
and enter `sub_08044EA8`. `sub_08009D70` (`code_080023A4.c:5822`) excludes the
ladder-top tile. That is the "hold down to fall through the platform" check.

**Against / unresolved:** I have not traced `sub_08044EA8` to confirm it is the
fall-through state (hence medium-high, not high). The pairing in the data is
however exact: idx 14/16-19/24-25 (`0xC`) mirror idx 15/20-23/26-27 (`0x4`) tile
for tile, so `0x8` is a pure per-platform modifier, and `0x4` alone is the
non-droppable variant.

### `0x00000010` — `TILE_SLIPPERY` (ice)

**Confidence:** high

**Tested at:** `src/kirby.c:14406,14408,14410,14432,18956` (all via `unk58`).

**What it does:** `sub_0805B1B8` (`kirby.c:14395`) picks the per-frame x-friction:

```c
r4 = unk58 & 0x10 && ability != KIRBY_ABILITY_ICE ? 8  : 32;   // anim 12
r4 = unk58 & 0x10 && ability != KIRBY_ABILITY_ICE ? 13 : 27;   // anim 13
r4 = unk58 & 0x10 && ability != KIRBY_ABILITY_ICE ? 4  : r4;   // else (14)
```

`sub_0805B284` (`kirby.c:14427`) likewise uses 8 instead of 10. The Ice ability
cancels the effect (Kirby is already on ice skates).

Data agrees: `0x10` is set on idx 74-79 (six slope tiles: the four gentle floors
and the two 45° floors) plus idx 80 = `0x210` (a solid ice block). No ceiling
slope carries it, which is exactly right — you cannot slide on a ceiling.

**Against / unresolved:** none.

### `0x00000020` / `0x00000040` — chain-reaction block flags

**Confidence:** medium

**Tested at:** `0x20` — `wip/code_08032E98.c:2088` (the only site).
`0x40` — `src/code_0800E0E4.c:103,116,129,142`, and via `unk58` at
`src/code_0806F780.c:7135,7145`.

**What it does:** `wip/code_08032E98.c:2071-2100` is the block-smashing sweep:

```c
tile = gUnk_082D88B8[...];
if (!(tile & 0x1000)) continue;              // not breakable
switch (tile & 0x00F00000) { ... }           // strength gate, see below
if (tile & 0x20) {
    sub_08001408(id, sub_080025AC(id, t, k), NULL, NULL);   // clear the cell
    sub_0800E0E4((struct Object2 *)arg0, t, k);             // start chain task
    PlaySfx(arg0, 0x1FE);
} else {
    sub_08001408(...);                        // clear the cell
    obj = sub_0808AE30(arg0, 0, 0x28D, 0);    // ordinary debris
}
```

`sub_0800E0E4` (`src/code_0800E0E4.c:11`) is that chain task; its propagation
step tests the four neighbours for `& 0x40`, clears them and plays
`SE_BLOCK_BREAK` (`code_0800E0E4.c:103-150`). So `0x20` = "I detonate",
`0x40` = "a detonation eats me". `lever_wall.c:30,32` is the other caller of
`sub_0800E0E4`.

In `code_0806F780.c:7135/7145` a projectile survives only while the tile it is in
has `0x1000` **or** `0x40` — it tunnels through breakable/chain material.

Data: `0x20` on idx 241 (`0x101260`) and 246 (`0x301220`) — both `BREAKABLE|SOLID`;
`0x40` on idx 241, 242 (`0x240` = solid chain-member, itself *not* breakable) and
243 (`0x41` = empty chain-member, i.e. a gap the blast crosses).

**Against / unresolved:** the one `0x20` test lives in `wip/`, which is not yet a
matching decomp. I have not found a second, independent site. "Bomb block" is a
plausible name but I am not naming it that on one site.

### `0x00000080` — never tested

Set on exactly one entry, idx 92 = `0x00000081` (`EMPTY | bit7`). Zero reads of
bit 7 anywhere in `src/`, `wip/` or `include/`. **Finding: this bit is dead in the
shipped code**, or is read by a function still in `asm/`.

### `0x00000100` — `TILE_LADDER`

**Confidence:** high

**Tested at:** `src/kirby.c:13049`; `src/code_080023A4.c:5810` (`sub_08009D28`);
`src/kirby.c:13010` (in `sub_080566E0`, via the same expression).

**What it does:** `sub_080566E0` (`src/kirby.c:12994`) *is* the climbing state:
it sets `animationIndex` 45/46 and plays `SE_KIRBY_CLIMB_UP` / `SE_KIRBY_CLIMB_DOWN`.
Climbing down continues while the tile at Kirby's feet has `0x100`
(`kirby.c:13009-13012`); climbing up stops when the cell above lacks it
(`kirby.c:13049`). `sub_08009D28` (`code_080023A4.c:5806`) is the generic
"am I on a ladder" predicate.

Only two entries have it, and their split is the whole design:
* idx 33 = `0x101` — `EMPTY | LADDER`: the rope body, walk/fall straight through.
* idx 32 = `0x10C` — `TOP_SOLID | DROP_THROUGH | LADDER`: the ladder **top**, a
  platform you stand on and can drop through.

`kirby.c:13048` and `sub_08009D70` (`code_080023A4.c:5825`) both compare the
*whole word* against `gUnk_082D88B8[32]` — i.e. "is this specifically the
ladder-top tile" — which is how climbing up terminates onto the platform.

**Against / unresolved:** none.

### `0x00000200` — `TILE_SOLID`

**Confidence:** high (26 direct sites + 20 in `sub_0800F044` + 3 `unk58` sites)

**Tested at:** `src/sub_0800F044.c:80,102,123,146,168,188` (+14 more in the same
function); `src/code_080023A4.c:2603,2615,2647,2659,4002,4060,4107,5164`
(+more at 5267,5279,5311,5323,5440,5452,5484,5496,5745,5771),
`src/code_0800ECAC_2.c:3118,3120,4101,4126`, `src/code_080023A4.c:4340`
(as `var5 = 0x200`), `src/special_doors.c:142`, `src/droppy.c:110,158`,
`src/prank.c:156,188,275`, `src/bonkers.c:246`; via `unk58` at
`src/object.c:2233`, `src/batafire.c:894`, `src/dark_mind.c:5002`.

**What it does:** full blocking. `sub_0800F044.c:80-84` (whose header comment is
zalo's field-verified AI-grid documentation) maps it to grid value 2 = "solid".
`Macro_AAAAAAAA_0` (`src/code_080023A4.c:532`) is a bare `& 0x200` wall probe.
Walkers turn around on it (`prank.c:156`, `bonkers.c:246`, `droppy.c:110`).
Kirby's own step resolution treats it as an unconditional stop
(`code_080023A4.c:4340`: `var5 = 0x200;` then `if ((var5 & var4) != 0 || (var4 & 0xF0000004) == 4)`).

Data agrees: idx 13 and 34-39 are bare `0x200`; every conveyor (103-108),
solid-spike (90, 93), ice block (80) and breakable (240-247) carries it; no
`EMPTY` entry does.

**Against / unresolved:** none.

### `0x00000400` — `TILE_MINI_ONLY`

**Confidence:** high

**Tested at:** `src/kirby.c:6816,6931,9020,9117,9877,9949,10263,10334,10508`
(all `unk58`); `src/sub_0800F044.c:87,107,130,151,173,193` (+6 more);
`src/code_080023A4.c:4337`.

**What it does:** a gap only Mini Kirby fits through. The lethal half:

```c
// src/kirby.c:6816
if (kirby->ability != KIRBY_ABILITY_MINI && kirby->base.base.base.unk58 & 0x400) {
    sub_08088640(&kirby->base.base, 0x1C, 0x20);
    kirby->hp = 0;
}
```

`sub_0800F044.c:87` — `r5[1] = kirby->ability != KIRBY_ABILITY_MINI ? 2 : 0;`
(solid for the AI unless mini). `code_080023A4.c:4337` blocks entry unless
Mini, with a size escape hatch (`0x800 < arg1->unk14 - arg1->unkC`).
All the `kirby.c` sites of the form
`(ability != KIRBY_ABILITY_MINI || !(unk58 & 0x400)) && ability != KIRBY_ABILITY_SLEEP`
suppress ability changes while wedged in one.

Data: exactly one entry, idx 89 = `0x0C000401`.

**Against / unresolved:** idx 89 also has force-direction bits `0xC` (`LEFT|RIGHT`
simultaneously). Since both `sub_080097C4` and `sub_080098C4` switch on the exact
value of `arg1 & 0xF000000`, `0x0C000000` matches no case and produces no push —
and the entry lacks `0x800` anyway, so the force path is never entered. Those four
bits appear to be inert on this entry. I could not explain why they are set.

### `0x00000800` + `0x0F000000` + `0x00F00000` — directional force

**Confidence:** high

**Tested at:** `src/code_080023A4.c:2411` (the only `& 0x800` site — it is the
gate), then `sub_080097C4` (`src/code_080023A4.c:5609`) and `sub_080098C4`
(`src/code_080023A4.c:5648`) switch on `arg1 & 0xF000000` with cases
`0x1000000 / 0x2000000 / 0x4000000 / 0x8000000`. Also invoked from
`src/code_080023A4.c:3935` when Kirby *lands* on a tile.

**What it does:**

```c
u32 var0 = (arg1 & 0xF00000) >> 0x14;      // strength index 0..2
switch (arg1 & 0xF000000) {
case 0x1000000: movementOverride.y = max(movementOverride.y, T[1][var0]); break;
case 0x2000000: movementOverride.y = min(movementOverride.y, T[3][var0]); break;
case 0x4000000: movementOverride.x = min(movementOverride.x, T[0][var0]); break;
case 0x8000000: movementOverride.x = max(movementOverride.x, T[2][var0]); break;
}
```

with `T` = `gUnk_082D91A4` in air (`src/code_080023A4.c:433`) or `gUnk_082D91BC`
in water (`src/code_080023A4.c:440`):

```c
const s16 gUnk_082D91A4[4][3] = {
    { -0x100, -0x200, -0x280 },   // row 0 -> case 0x4000000
    {  0x180,  0x280,  0x400 },   // row 1 -> case 0x1000000
    {  0x100,  0x200,  0x280 },   // row 2 -> case 0x8000000
    { -0x100, -0x150, -0x300 },   // row 3 -> case 0x2000000
};
```

`movementOverride` is added to `yspeed` at `src/code_080023A4.c:2131`, and the
engine integrates `y -= yspeed` (`src/code_0806F780.c:2004`), so **positive y is
up**. Therefore:

| bit | direction |
|---|---|
| `0x01000000` | **up** |
| `0x02000000` | **down** |
| `0x04000000` | **left** |
| `0x08000000` | **right** |

Two independent confirmations:
1. The up case is the only one exempted for Stone/Smash Kirby
   (`code_080023A4.c:5615`, `5655`) and the only one with an
   `unk62 & 4` (grounded) guard at strength 0 — exactly the semantics of an
   updraft that cannot lift a heavy Kirby.
2. The table contains diagonals built from ORs: idx 48 `0x5000803` = up+left,
   49 `0x9000803` = up+right, 50 `0xA000803` = down+right, 51 `0x6000803` = down+left.

`0x00F00000` here is a **strength index 0..2**, matching the three columns
exactly: idx 40-51 = 0, idx 81-84 / 95-98 / 105-106 = 1, idx 85-88 / 99-102 /
107-108 = 2. No force entry ever exceeds 2.

Sub-classes visible in the data:
* **wind** (`FORCE|EMPTY`, dry): idx 40-43, 81-88.
* **current** (`FORCE|EMPTY|WATER`): idx 44-51, 95-102.
* **conveyor** (`FORCE|SOLID|0x8000`, horizontal only): idx 103-108 = left/right
  × strength 0/1/2. These push via the landing path (`code_080023A4.c:3935`)
  rather than the in-tile path.

**Against / unresolved:** `0x00008000` is set on all six conveyors and nowhere
else, and is **never read**. It looks like a "this is a belt, not wind" marker
that the shipped code does not consult (it may drive tile animation elsewhere).

### `0x00001000` — `TILE_BREAKABLE`, with `0x00F00000` as hardness

**Confidence:** high

**Tested at:** `src/code_0806F780.c:2014-2015,2170`; `wip/code_08032E98.c:1982,2077`;
`src/sub_0800F044.c:82,104,125,148,170,190` (+6 more); via `unk58` at
`src/code_0806F780.c:7135,7145,8408`. `src/code_0800ECAC_2.c` tests it ~40 times
(lines 1428, 1441, 1508, 1520, 1539, 1558, 1574, 1598, ... 2135) — that whole
block is the buddy-AI searching for breakable blocks to attack.

**What it does:** `sub_0800F044.c:82` maps `SOLID|BREAKABLE` to AI grid value 3 =
"destructible". The authoritative break routine is `wip/code_08032E98.c:2071-2100`
(quoted under `0x20` above): `0x1000` is the entry condition, and
`tile & 0x00F00000` gates on the attacker's capability bits:

| level | requirement |
|---|---|
| 0, 1 | none — any hit breaks it |
| 2 (`0x200000`) | `arg0->unk68 & 0x1000` |
| 3 (`0x300000`) | `arg0->unk68 & 0x4000` |
| 4 (`0x400000`) | `arg0->unk68 & 0x800` |

`wip/code_08032E98.c:1982` is a weaker attack that requires
`(attr & 0x00F01000) == 0x1000` — breakable **and** level 0 only.
`code_0806F780.c:2014` lets a thrown star survive a tile only if
`(attr & 0x1000) && (attr & 0xF00000) <= 0x100000` — it passes through the blocks
it can destroy and dies on anything harder.

Data: the six breakables are idx 240 (level 0), 241 and 244 (1), 245 (2),
246 (3), 247 (4) — a clean 0-4 ladder, and the only entries whose level field
exceeds 2.

**Against / unresolved:** the level→`unk68` mapping comes from `wip/`, which is
not yet matching. The *existence* of the gate and the level ordering are
corroborated by `code_0806F780.c:2015` in matched code.

### `0x00002000` — `TILE_HAZARD`

**Confidence:** high

**Tested at:** `src/kirby.c:6823`; `src/code_080023A4.c:2400`;
`src/ability_objects.c:243,280`; `src/sub_0800F044.c:78,100,121,144,166,186`
(+6 more).

**What it does:** damage.

```c
// src/kirby.c:6823
if (!(kirby->base.base.base.flags & 0x200) && kirby->base.base.base.unk58 & 0x2000) {
    if ((unk58 & 0xF00000) == 0x100000)      goto _0803FD44;
    else if ((unk58 & 0xF00000) <= 0x100000) { if (!(unk58 & 0xF00000)) goto _0803FD44; }
    else if ((unk58 & 0xF00000) == 0x200000) { _0803FD44: sub_0804D9D4(kirby); }
}
```

`sub_0800F044.c:78` maps it to AI grid value 1 = "hazard" — the cell the buddy
routes around. It has priority over `SOLID` in that classifier.

`0x00F00000` is a hazard *variant* here, values 0, 1, 2 — and the accumulator in
`sub_0800385C` treats it as sticky:

```c
// src/code_080023A4.c:2400
if ((unk58 & 0x2000) != 0)  unk58 |= var2 & ~0xF00000;   // keep the first hazard's level
else                        unk58 |= var2 & ~0xF00000;
...
else if ((var2 & 0x2000) != 0) { unk58 &= ~0xF00000; unk58 |= var2; }  // adopt this hazard's level
```

Data: idx 91 (`0x102001`, level 1) and 94 (`0x202001`, level 2) are passable
hazards; idx 90 (`0x102200`) and 93 (`0x202200`) are the solid spike versions;
idx 28-31 are hazards with a wind push; idx 109-112 are hazards in water with a
current. Levels observed: 0, 1, 2.

**Against / unresolved:** **all three branches of the `kirby.c:6823` chain call the
same `sub_0804D9D4(kirby)`.** As written the level field changes nothing at that
site — the branching is either compiler artefact of a `switch`, or the variant
matters only inside `sub_0804D9D4`, which I did not trace. So: `0x2000` = damaging
is certain; "the level selects the damage amount / kind" is *plausible but not
demonstrated*.

### `0x00004000` — `TILE_DOOR`

**Confidence:** high

**Tested at:** `src/mirror_door.c:46,142,176,198,230,246`;
`src/special_doors.c:78,308,383`; `src/special_hub_mirror.c:72`;
`src/code_0800ECAC_2.c:3007,3237,3456,3632,3688`; `src/kirby.c:6859,14894` (as
`(r6 & 0xF04000) == 0x104000`); via `unk58` at `src/kirby.c:12429,12867`.

**What it does:** marks the cell that a door/mirror occupies.
`mirror_door.c:46` decides at spawn whether a mirror door object actually has a
door tile under it. Kirby's warp trigger is:

```c
// src/kirby.c:6859
if ((r6 & 0xF04000) == 0x104000) {           // DOOR *and* level == 1
    kirby->base.base.base.unk58  |= 0x4000;
    kirby->base.base.base.unk62  |= 0x10;
    kirby->base.base.base.unkC   |= 0x80000;
    kirby->unk120 = x >> 12; kirby->unk122 = y >> 12;
    sub_0805545C(kirby);                     // enter the door
}
```

and `sub_080552A8` / `sub_080562D0` (`kirby.c:12429`, `12867`) branch on
`unk58 & 0x4000` to run the room transition. `code_0800ECAC_2.c` uses it so the
buddy follows Kirby into doors.

Data: exactly four entries — idx 254 `0x104001` and 255 `0x4001` (dry),
252 `0x104002` and 253 `0x4002` (submerged). The level field is 1 vs 0, and only
level 1 satisfies the `0xF04000 == 0x104000` auto-warp test — so the level-0 pair
is a door marker that Kirby does not walk into by himself (it is still visible to
`mirror_door.c`'s plain `& 0x4000`).

**Against / unresolved:** the door pairs use `0x1` (dry) vs `0x2` (water) in the
low bits, but the water pair does **not** also set `0x1`, unlike every other
submerged passable tile (idx 72 = `0x3`, idx 44-51 = `...803`). Either doors are
exempt from the empty-tile fast path, or it is an oversight in the data.

### `0x000F0000` — never set, never tested

Bits 16-19 are `0` in all 256 entries and are read by nothing. The `unk58`
accumulate mask `& 0xFFFFF` (`src/code_080023A4.c:511` and 33 other sites) spans
bits 0-19, so this hole exists purely so that mask can be written as five nibbles.

---

## The high nibble `0xF0000000`

**It is an enum, not a flag field.** Observed values: **0 through 12**, no others.
`src/code_0806F780.c:798` and `:833` use it as a raw index:

```c
if ((r4 = gUnk_082D88B8[a1->unk57] >> 0x1C))
    gUnk_08350C38[r4](a1);
```

`gUnk_08350C38` (`src/code_0806F780.c:334`) is a 20-entry function table whose
first 13 slots are the reachable ones (`nullsub_122` at index 0). Each handler is
a slope push-out routine (`src/code_0806F780.c:1315-1470`). Reading each handler's
penetration test gives the geometry exactly. With `px = (x & 0xF00) >> 8` and
`py = (y & 0xF00) >> 8` (sub-tile pixel coords, 0-15):

| value | handler | penetration test | surface | kind |
|---|---|---|---|---|
| 0 | `nullsub_122` | — | — | no slope |
| 1 | `sub_08070BA8` | `px/2 - (15-py) >= 0` | `py = 15 - px/2` | **floor**, 1:2, rising rightward, lower (left) tile |
| 2 | `sub_08070C24` | `px/2 - (8-py) >= 0` | `py = 8 - px/2` | **floor**, 1:2, rising rightward, upper (right) tile |
| 3 | `sub_08070FA8` | `px - (15-py) >= 0` | `py = 15 - px` | **floor**, 1:1 (45°), rising rightward |
| 4 | `sub_08071024` | `(15-px) - (15-py) >= 0` | `py = px` | **floor**, 1:1, rising leftward |
| 5 | `sub_08070CA0` | `(15-px)/2 - (15-py) >= 0` | mirror of 1 | **floor**, 1:2, rising leftward, lower (right) tile |
| 6 | `sub_08070D20` | `(15-px)/2 - (8-py) >= 0` | mirror of 2 | **floor**, 1:2, rising leftward, upper (left) tile |
| 7 | `sub_08070EA0` | `(15-px)/2 - py > 0` | — | **ceiling**, 1:2, descending leftward, right tile |
| 8 | `sub_08070F24` | `(15-px)/2 - (py-7) >= 0` | — | **ceiling**, 1:2, descending leftward, left tile |
| 9 | `sub_080710A4` | `px - (15-py) < 2` | `px+py = 15` | **ceiling**, 1:1, solid mass upper-left |
| 10 | `sub_08071124` | `(15-px) - (15-py) < 0` | `py = px` | **ceiling**, 1:1, solid mass upper-right |
| 11 | `sub_08070DA0` | `px/2 - py >= 0` | `py = px/2` | **ceiling**, 1:2, descending rightward, left tile |
| 12 | `sub_08070E20` | `px/2 - (py-7) >= 0` | `py = px/2 + 7` | **ceiling**, 1:2, descending rightward, right tile |

Floors push the object **up** (`a1->y -= r3 * 0x100`); ceilings push it **down**
(`a1->y += r3 * 0x100`). That is the primary floor/ceiling discriminator and it
splits the enum cleanly at **1-6 = floor, 7-12 = ceiling**.

Three independent checks confirm that split and the left/right grouping:

1. `src/code_0806F780.c:800-806` — after a slope, `if (a1->yspeed > 0 && r4 < 7) return;`
   (moving upward through a **floor** slope is ignored), and
   `if (a1->xspeed < 0) { if (r4 < 4 || r4 > 9) return; }`. The set
   `{1,2,3} ∪ {10,11,12}` is exactly the six slopes whose solid mass is on the
   **right** half of the tile — derived independently from the geometry above.
2. `src/kirby.c:14285-14330` (`sub_0805ABE0`, rolling) groups
   `{1,2,3}` → accelerate **left** and `{4,5,6}` → accelerate **right**, with the
   45° cases (3 and 4) clamped to `0x200` and the 1:2 cases to `0x180`.
   Rising-rightward floors ⇒ downhill is leftward. ✓ And the steeper slope gets
   the higher terminal speed. ✓
3. `src/sub_0800F044.c:61-68` sets `r6 = TRUE` for `{2,3,4,6}` — precisely the
   four floor slopes whose surface reaches the **top edge** of their tile — and
   then treats the neighbouring cells as air rather than solid.
   `src/code_080023A4.c:2256/2292/2330/2363` tests the same set as the two pairs
   `{4,6}` and `{2,3}` (left-reaching vs right-reaching).

Animation confirms the steepness classes: `sub_0805B8B8` (`src/kirby.c:14779`)
gives 45° slopes (3, 4) `animationIndex` 6/8 and 1:2 slopes (1, 2, 5, 6) 2/4,
with the pair swapped by facing; `sub_0805C0C8` (`src/kirby.c:15089`) special-cases
only 3 and 4.

Slope values never coexist with `SOLID`, `EMPTY`, `LADDER`, `FORCE`, `HAZARD`,
`BREAKABLE` or `DOOR` in the shipped data. They coexist only with `WATER` (idx
52-71), `TOP_SOLID`/`DROP_THROUGH` (idx 16-27), and `SLIPPERY` (idx 74-79) — and
the latter two combinations use **only** floor values 1-6, never a ceiling.

---

## Annotated dump of all 256 entries

`lvl` is `(v >> 20) & 0xF`, shown only where a field-owning flag
(`FORCE`/`BREAKABLE`/`HAZARD`/`DOOR`) is present.

| idx | value | slope | flags | force dir | lvl |
|---|---|---|---|---|---|
| 0 | `0x00000001` | — | EMPTY | — | — |
| 1 | `0xB0000000` | 11 ceil 1:2 desc-R (left) | — | — | — |
| 2 | `0xC0000000` | 12 ceil 1:2 desc-R (right) | — | — | — |
| 3 | `0x80000000` | 8 ceil 1:2 desc-L (left) | — | — | — |
| 4 | `0x70000000` | 7 ceil 1:2 desc-L (right) | — | — | — |
| 5 | `0x10000000` | 1 floor 1:2 rise-R (low) | — | — | — |
| 6 | `0x20000000` | 2 floor 1:2 rise-R (high) | — | — | — |
| 7 | `0x60000000` | 6 floor 1:2 rise-L (high) | — | — | — |
| 8 | `0x50000000` | 5 floor 1:2 rise-L (low) | — | — | — |
| 9 | `0xA0000000` | 10 ceil 1:1 mass-upper-R | — | — | — |
| 10 | `0x90000000` | 9 ceil 1:1 mass-upper-L | — | — | — |
| 11 | `0x30000000` | 3 floor 1:1 rise-R | — | — | — |
| 12 | `0x40000000` | 4 floor 1:1 rise-L | — | — | — |
| 13 | `0x00000200` | — | SOLID | — | — |
| 14 | `0x0000000C` | — | TOP_SOLID DROP_THROUGH | — | — |
| 15 | `0x00000004` | — | TOP_SOLID | — | — |
| 16-19 | `0x1/2/6/5000000C` | 1,2,6,5 | TOP_SOLID DROP_THROUGH | — | — |
| 20-23 | `0x1/2/6/50000004` | 1,2,6,5 | TOP_SOLID | — | — |
| 24-25 | `0x3/4000000C` | 3,4 | TOP_SOLID DROP_THROUGH | — | — |
| 26-27 | `0x3/40000004` | 3,4 | TOP_SOLID | — | — |
| 28-31 | `0x1/2/4/8002001` | — | EMPTY HAZARD | up/down/left/right | 0 |
| 32 | `0x0000010C` | — | TOP_SOLID DROP_THROUGH LADDER | — | — |
| 33 | `0x00000101` | — | EMPTY LADDER | — | — |
| 34-39 | `0x00000200` | — | SOLID | — | — |
| 40-43 | `0x1/2/4/8000801` | — | EMPTY FORCE | up/down/left/right | 0 |
| 44-47 | `0x1/2/4/8000803` | — | EMPTY WATER FORCE | up/down/left/right | 0 |
| 48-51 | `0x5/9/A/6000803` | — | EMPTY WATER FORCE | up+left / up+right / down+right / down+left | 0 |
| 52-53 | `0xA0000002` | 10 | WATER | — | — |
| 54-55 | `0x90000002` | 9 | WATER | — | — |
| 56-57 | `0x30000002` | 3 | WATER | — | — |
| 58-59 | `0x40000002` | 4 | WATER | — | — |
| 60-63 | `0xB/C/8/70000002` | 11,12,8,7 | WATER | — | — |
| 64-67 | `0x1/2/6/50000002` | 1,2,6,5 | WATER | — | — |
| 68-71 | `0xA/9/3/40000002` | 10,9,3,4 | WATER | — | — |
| 72 | `0x00000003` | — | EMPTY WATER | — | — |
| 73 | `0x00000006` | — | WATER TOP_SOLID | — | — |
| 74-79 | `0x1/2/6/5/3/40000010` | 1,2,6,5,3,4 | SLIPPERY | — | — |
| 80 | `0x00000210` | — | SLIPPERY SOLID | — | — |
| 81-84 | `0x1/2/4/8100801` | — | EMPTY FORCE | up/down/left/right | 1 |
| 85-88 | `0x1/2/4/8200801` | — | EMPTY FORCE | up/down/left/right | 2 |
| 89 | `0x0C000401` | — | EMPTY MINI_ONLY | (left+right, inert) | — |
| 90 | `0x00102200` | — | SOLID HAZARD | — | 1 |
| 91 | `0x00102001` | — | EMPTY HAZARD | — | 1 |
| 92 | `0x00000081` | — | EMPTY + **bit 7 (unknown)** | — | — |
| 93 | `0x00202200` | — | SOLID HAZARD | — | 2 |
| 94 | `0x00202001` | — | EMPTY HAZARD | — | 2 |
| 95-98 | `0x1/2/4/8100803` | — | EMPTY WATER FORCE | up/down/left/right | 1 |
| 99-102 | `0x1/2/4/8200803` | — | EMPTY WATER FORCE | up/down/left/right | 2 |
| 103-104 | `0x4/8008A00` | — | SOLID FORCE +bit15 | left / right | 0 |
| 105-106 | `0x4/8108A00` | — | SOLID FORCE +bit15 | left / right | 1 |
| 107-108 | `0x4/8208A00` | — | SOLID FORCE +bit15 | left / right | 2 |
| 109-112 | `0x1/2/4/8002003` | — | EMPTY WATER HAZARD | up/down/left/right | 0 |
| 113-239 | `0x00000000` | — | (unused — all zero) | — | — |
| 240 | `0x00001200` | — | SOLID BREAKABLE | — | 0 |
| 241 | `0x00101260` | — | CHAIN_TRIGGER CHAIN_MEMBER SOLID BREAKABLE | — | 1 |
| 242 | `0x00000240` | — | CHAIN_MEMBER SOLID | — | — |
| 243 | `0x00000041` | — | EMPTY CHAIN_MEMBER | — | — |
| 244 | `0x00101200` | — | SOLID BREAKABLE | — | 1 |
| 245 | `0x00201200` | — | SOLID BREAKABLE | — | 2 |
| 246 | `0x00301220` | — | CHAIN_TRIGGER SOLID BREAKABLE | — | 3 |
| 247 | `0x00401200` | — | SOLID BREAKABLE | — | 4 |
| 248-251 | `0x00000000` | — | (unused) | — | — |
| 252 | `0x00104002` | — | WATER DOOR | — | 1 |
| 253 | `0x00004002` | — | WATER DOOR | — | 0 |
| 254 | `0x00104001` | — | EMPTY DOOR | — | 1 |
| 255 | `0x00004001` | — | EMPTY DOOR | — | 0 |

Note that entry `0x00000000` (indices 113-239, 248-251) is **not** the empty
tile — index 0 is, with value `0x1`. An all-zero attribute word means "no
collision, and also not recognised as empty", which makes those 131 indices
reserved/unused rather than air.

---

## Two accumulate masks worth naming

Kirby's `unk58` is rebuilt from scratch each frame (`src/code_080023A4.c:2139`
`unk58 = 0`) and OR-accumulated from every tile his hitbox touches. Two different
masks are used and they are not the same:

* `attr & 0xFFFFF` — 34 sites, e.g. `Macro_0800549C` (`src/code_080023A4.c:511`),
  `src/code_080023A4.c:2637,2679,2865,2953,3036,...`. Keeps the property flags,
  drops the level, force-direction and slope fields.
* `attr & ~0xF00000` — `src/code_080023A4.c:2402,2404,2407`. Keeps the slope and
  force-direction fields, drops only the level — because the level of the *first*
  hazard encountered must win (`src/code_080023A4.c:2400`).
* Bare `attr` (no mask) — `src/code_080023A4.c:2692,3940,5180,5207,...`,
  `src/object.c:251`, `src/code_0806F780.c:795,7144,7265`, `src/kirby.c:12762`.

The state-reset survivor mask is `unk58 &= 0x4402` (`src/kirby.c:8948`):
`DOOR | MINI_ONLY | WATER` — the three properties that must persist across a
Kirby state change.

---

## Corroboration from level data

Method: `tools/room_export.py` (read, imported, not modified) was used to
decompress the collision grid of all **286 valid rooms** — 293,674 cells. For
every metatile index I computed occurrence count, number of rooms, mean
horizontal and vertical run length, and the distribution of the index directly
above and directly below. The readings above were derived purely from the C
source, so this is an independent test: a bit's proposed meaning implies a
spatial signature, and the signature either shows up or it does not.

**80 distinct indices are used.** All 80 have a non-zero attribute word, and
every index with a zero attribute word (113-239, 248-251) is unused — a clean
confirmation that those 131 slots really are reserved and that the zero word is
not a usable "air" value (air is index 0, word `0x1`).

### Confirmed — the distribution matches the reading

| bit / field | prediction | measurement | verdict |
|---|---|---|---|
| `0x200` `SOLID` (idx 13) | bulk terrain, clusters | 133,371 cells (45.4%), 275/286 rooms, mean run 7.6x5.0, 79% has `SOLID` below | ✔ |
| `0x1` `EMPTY` (idx 0) | bulk air | 119,770 cells (40.8%), 280 rooms, 78% has `EMPTY` below | ✔ |
| `0x2` `WATER` (idx 72) | contiguous regions, minority of rooms | 8,248 cells, **50 rooms only**, runs 6.4x4.7, 78% water below | ✔ |
| `0x10` `SLIPPERY` (idx 80) | themed regions, few rooms | 9,629 cells, **30 rooms only**, runs 5.3x3.4 | ✔ |
| `0x2000` `HAZARD` | rare and isolated | idx 90: 530 cells in **7 rooms**, mean row 0.76 (room floor); idx 93: 12 cells in **1 room**; idx 94: 8 cells in 1 room; idx 91: 40 cells in 3 rooms | ✔ |
| `0x4000` `DOOR` (idx 255) | one or two isolated cells per room | 453 cells across **236 rooms**, run 1.04x1.00, air above 99%, `SOLID` below 80% | ✔ |
| `0x400` `MINI_ONLY` (idx 89) | a gap only Mini fits | 33 cells in 4 rooms, run **3.67 wide x 1.00 tall**, `SOLID` **above 100% and below 100%** — a one-cell crawlspace inside rock | ✔✔ |
| `0x4` `TOP_SOLID` (idx 15, 14) | thin platforms with air under | idx 15: 2,028 cells, mean run **2.47 wide x 1.02 tall**, air above 92% and below 92%; idx 14 the same at 3.10x1.02 | ✔✔ |
| `0x100` `LADDER` (idx 32, 33) | 1-wide vertical column with a cap | idx 33 run **1.00 wide x 3.99 tall**; idx 32 run 1x1, air above 97%, and **idx 33 directly below it 100% of the time** | ✔✔✔ |

### Confirmed — the two readings I was least able to prove statically

**The slope enum's floor/ceiling split (`0xF0000000` values 1-6 vs 7-12).**
This came from reading push-out signs in `src/code_0806F780.c:1315-1470`. The
level data is an exact, independent 12/12 confirmation:

| idx | slope value | my reading | air above | solid above | air below | solid below |
|---|---|---|---|---|---|---|
| 5 | 1 | floor | 99% | — | — | 99% |
| 6 | 2 | floor | 100% | — | — | 99% |
| 11 | 3 | floor | 98% | — | — | 99% |
| 12 | 4 | floor | 98% | — | — | 99% |
| 8 | 5 | floor | 100% | — | — | 100% |
| 7 | 6 | floor | 100% | — | — | 99% |
| 4 | 7 | **ceiling** | — | 79% | 96% | — |
| 3 | 8 | **ceiling** | — | 77% | 100% | — |
| 10 | 9 | **ceiling** | — | 86% | 99% | — |
| 9 | 10 | **ceiling** | — | 85% | 99% | — |
| 1 | 11 | **ceiling** | — | 76% | 100% | — |
| 2 | 12 | **ceiling** | — | 75% | 100% | — |

Every value I called a floor rests on solid fill with air above it; every value
I called a ceiling has solid fill above and air below. No exceptions. (The
"missing" 12-25% on the ceilings is index 80, the ice block — also solid.)
Mean row position agrees too: ceilings average row 0.32-0.42 of room height,
floors 0.46-0.50.

Two further structural checks fall out:
* Ice slopes (idx 74-79, all floor values 1,2,3,4,5,6) have index **80** (the
  slippery solid block) below them 91-100% of the time, and air above 100%.
* Sloped one-way platforms (idx 20-23, 26, 27) have index **15** (the flat
  one-way platform) below them **100%** of the time, and air above 100%.

**The force-direction nibble `0x0F000000` (1=up, 2=down, 4=left, 8=right).**
Derived statically from the sign of `gUnk_082D91A4` combined with `y -= yspeed`.
Two entirely different geometric signatures confirm it.

*Wind and current shafts* — a vertical push should carve a tall column, a
horizontal push a wide band:

| idx | dir | mean run w x h | rooms |
|---|---|---|---|
| 85 | up (str 2) | 5.2 x **41.9** | 3 |
| 86 | down (str 2) | 5.9 x **25.3** | 4 |
| 99 | up (water, str 2) | 3.4 x **12.5** | 4 |
| 100 | down (water, str 2) | 3.0 x **11.7** | 10 |
| 81 | up (str 1) | 4.5 x 9.8 | 2 |
| 82 | down (str 1) | 2.0 x 7.0 | 1 |
| 45 | down (water, str 0) | 3.1 x 6.9 | 8 |
| 40 | up (str 0) | 1.3 x 2.8 | 1 |
| 47 | right (water, str 0) | **16.5** x 3.0 | 3 |
| 43 | right (str 0) | **14.3** x 6.3 | 1 |
| 46 | left (water, str 0) | **11.4** x 3.6 | 2 |
| 102 | right (water, str 2) | **10.0** x 1.9 | 5 |
| 101 | left (water, str 2) | **7.5** x 2.0 | 5 |

13 of the 17 used force tiles are unambiguous and every one of them agrees.
(A nice bonus: idx 40, an up-wind of strength 0, sits directly on top of idx 81,
the strength-1 up-wind, 27% of the time — an updraft that weakens as you rise,
which is exactly what a strength field should produce.)

*Hazard surfaces* — hazards also carry a direction, and there the signature is
the **opposite** aspect ratio, because a spike strip lies *along* the surface it
is attached to and pushes *away* from it:

| idx | dir | run w x h | above | below | reading |
|---|---|---|---|---|---|
| 28 | up | 3.19 x **1.00** | air 84% | **solid 90%** | floor spikes, knock upward |
| 29 | down | 3.16 x **1.00** | **solid 96%** | air 98% | ceiling spikes, knock downward |
| 30 | left | **1.00** x 6.80 | itself | itself | wall spikes, knock leftward |
| 31 | right | **1.00** x 7.60 | itself | itself | wall spikes, knock rightward |
| 109 | up (water) | 2.82 x **1.00** | water 74% | **solid 100%** | floor spikes underwater |
| 110 | down (water) | 2.30 x **1.00** | **solid 100%** | water 79% | ceiling spikes underwater |
| 111/112 | left/right (water) | **1.00** x 1.5/1.0 | water | water | wall spikes underwater |

Up-hazards are horizontal strips sitting on the ground; down-hazards are
horizontal strips hanging from the ceiling; left/right hazards are 1-wide
vertical columns. This is a second, fully independent derivation of the same
four direction bits — and it upgrades the `0x0F000000` field's description from
"push direction" to "**outward surface normal**", which covers both uses.

### Confirmed — chain blocks, the weakest static reading

`0x20` `CHAIN_TRIGGER` rested on a single `wip/` site. The level data is much
stronger than that:

* idx **246** (`0x00301220`, `CHAIN_TRIGGER|SOLID|BREAKABLE`, level 3) —
  14 cells across 13 rooms, always exactly 1x1, air above **100%**, and
  index **242** (`0x00000240`, `CHAIN_MEMBER|SOLID`) directly below it **100%**.
* idx **241** (`0x00101260`, both flags, level 1) — 403 cells in 125 rooms,
  run 1.07 x 1.01 (isolated single cells), with idx 242 above it 44% of the time
  and below it 26%.
* idx **242** itself — 4,074 cells in 131 rooms, runs 2.95 x 2.56, mostly
  adjacent to itself.
* idx **243** (`0x00000041`, `EMPTY|CHAIN_MEMBER`) — 694 cells in 42 rooms.

A single 1x1 detonator sitting on top of a mass of propagator blocks, with
"empty but blast-conducting" cells threading between them, is precisely the
layout the `0x20` / `0x40` split predicts. Raising `CHAIN_TRIGGER` and
`CHAIN_MEMBER` from medium to **medium-high**.

### Contradicted / corrected

**The door-level reading needed adjusting.** I wrote that the level field
selects a door variant and that only level 1 (`0xF04000 == 0x104000`) triggers
Kirby's auto-warp at `src/kirby.c:6859`. The data shows level **0** (idx 255) in
**236 of 286 rooms** and level **1** (idx 254) in only **8**. If level 1 were
the ordinary door, doors would barely exist. The resolution — which strengthens
both readings rather than breaking either — is that ordinary doors are *objects*
(`src/special_doors.c`, `src/mirror_door.c`) which find their anchor cell with a
plain `& 0x4000` test and handle the transition themselves; the `kirby.c:6859`
path is a rare **object-less automatic warp**, and 8 rooms is the right order of
magnitude for that. The level field is therefore "does Kirby warp on his own",
not "which kind of door".

**45 of the 125 non-zero table entries never appear in any room.** The unused
set is not random — it is whole features:

* **all 20 water-slope entries (52-71)** — no sloped ground exists underwater in
  the shipped game, so `TILE_WATER` and the slope enum never actually co-occur.
* **all 6 conveyor entries (103-108)** — the `SOLID|FORCE|0x8000` combination is
  authored nowhere. This *explains* the dead bit `0x8000`: the whole conveyor
  feature is cut, which is why nothing reads its marker bit.
* **all 6 droppable *sloped* platforms (16-19, 24-25)** — only the flat `0xC`
  tile (idx 14) and the non-droppable sloped platforms (20-23, 26, 27) are used.
* **all 4 diagonal water currents (48-51)**, and 6 further wind variants
  (41, 42, 44, 83, 84, 87, 95).
* **idx 92 (`0x81`)** — the sole carrier of the mystery bit `0x80`. It is
  neither read by any code nor placed in any room. Bit 7 is dead twice over.
* **idx 73 (`0x6`, `WATER|TOP_SOLID`)** — an underwater one-way platform, unused.

Caveat: "unused" here means "not authored in static room data". Cells are also
written at runtime by `sub_0800255C` (`src/code_080023A4.c:512`) and
`sub_08001408` (`src/code_080023A4.c:589`, called whenever a block breaks), so
an index absent from the maps could in principle still be produced in play. It
does mean no level *designer* placed one.

### Cannot speak to

* **`0x8` `DROP_THROUGH`.** idx 14 (`0xC`) and idx 15 (`0x4`) have statistically
  identical signatures — 1-cell-tall strips with air above and below, runs
  3.10x1.02 and 2.47x1.02 — differing only in frequency (52 rooms vs 159). No
  spatial test distinguishes "you can duck through this one". The reading stays
  at medium-high on the `sub_080035F4` evidence alone. (One weak hint: 6% of
  idx-14 cells have floor spikes, idx 28, directly beneath them, versus 0% for
  idx 15 — a droppable platform above a hazard is a plausible level-design
  motif, but 6% is not an argument.)
* **The hazard level field varying damage.** Levels 1 and 2 both occur
  (idx 90/91 vs 93/94) and level 2 is far rarer (1-3 rooms vs 3-7), which is
  consistent with a stronger hazard but equally consistent with an unused
  distinction.
* **The breakable hardness ladder.** Levels 0-4 all occur, with plausibly
  ability-gated populations (level 0: 130 rooms; level 1: 125 + 10; level 2:
  33 rooms; level 3: 13 rooms; level 4: 25 rooms). The spread supports "different
  requirements" but cannot identify *which* ability each level needs.

### One incidental finding

Indices **34-39** all carry the same attribute word `0x200` as index 13, yet the
level data uses them separately (5-33 cells each, 2-4 rooms) and they cluster
almost exclusively next to each other. So the *index* carries information the
attribute word does not — which is consistent with `sub_080025AC`
(`src/code_080023A4.c:527`) searching `LevelInfo.unk1E0` by
`(tileIndex, x, y)` triples. Anything reimplementing this table must not
canonicalise indices by their attribute value.

---

## What I could not determine

1. **`0x00000080`** (bit 7). Set on exactly one entry (idx 92, `0x81`), read by
   nothing in `src/`, `wip/` or `include/`, and — per the 286-room sweep — placed
   in no room. It is dead in the data as well as in the code, so there is nothing
   left to infer a meaning from. I cannot say what it means.
2. **`0x00008000`** (bit 15). Set on exactly the six conveyor entries (103-108),
   read by nothing, and none of those six indices is used by any room. The
   conveyor feature appears to have been cut, which explains the dead bit but
   leaves its intended meaning undetermined. (It correlates perfectly with
   `SOLID|FORCE`, so even a live consumer would have found it redundant.)
3. **Whether the hazard level actually varies the damage.** `src/kirby.c:6823-6835`
   branches on `0x00F00000` three ways and calls `sub_0804D9D4(kirby)` in all
   three. I did not trace `sub_0804D9D4` to see whether it re-reads `unk58`.
   Values 0, 1 and 2 all occur on hazard tiles, so the field is *populated*
   meaningfully; I just cannot show it changes behaviour.
4. **Why idx 89 (`TILE_MINI_ONLY`) carries force-direction bits `0xC`** with no
   `TILE_FORCE` flag. Both force routines switch on the exact nibble value, and
   `0xC` matches no case, so it is inert — but I cannot explain the intent.
5. **Why the water doors (idx 252, 253) lack `0x1`** while every other submerged
   passable tile sets `EMPTY|WATER` together.
6. **The precise trigger for `TILE_DROP_THROUGH`.** `sub_080035F4` is the only
   isolating read and its Kirby caller (`src/kirby.c:7277`) ends in
   `sub_08044EA8`, which I did not open. "Duck to fall through" fits, but the
   two `code_0800ECAC_2.c` callers (3271, 3666) are buddy-AI and I did not
   confirm they mean the same thing.
7. **`0x00000020` rests on a single `wip/` site** (`wip/code_08032E98.c:2088`).
   `wip/` is by convention not-yet-matching code in this repo, so treat
   `CHAIN_TRIGGER` as the weakest of the named bits.
8. **The `0x00F00000` level→`unk68` capability mapping** (levels 2/3/4 requiring
   `unk68 & 0x1000 / 0x4000 / 0x800`) likewise comes only from `wip/`.
   The corroborating matched-code site (`src/code_0806F780.c:2015`) proves only
   the ordering `<= 1` vs `> 1`.
9. **`sub_0800F044.c` is `NAKED`** — the C body I quoted is `#ifdef NONMATCHING`
   and is explicitly described in its own header comment as "believed
   functionally equivalent" rather than proven. I used it as corroboration, never
   as a sole source; every bit it supports (`0x200`, `0x400`, `0x1000`, `0x2000`,
   and the `{2,3,4,6}` slope set) has at least one matched-code witness.
