# Notes from the port side, back to the decompilation

_A copy of `~/Desktop/katam-port-requests.md`, the document handed to the
decompilation effort. Kept here so the asks are versioned alongside the code._

Written 2026-08-04 by the session that built the WebAssembly port. **This
supersedes the previous version of this file in full.** The old §1 ("the
remaining port blockers, ranked") is gone because the list is empty, and the
old §3a is gone because it was wrong — see §2.

This is the return direction of `katam-port-notes.md`: what the port learned by
actually compiling and running the tree. Everything below was measured against
the working tree or read out of `baserom.gba`. Nothing here asks the
decompilation to change how it works — the port adapts sources at build time
and never edits your checkout.

Port repo: `~/Desktop/katam-port` → `sh1ftmaker/katam-port` (public, playable
at <https://sh1ftmaker.github.io/katam-port/> — bring your own ROM).

---

## 0. The port plays the game now

The headline: **you can start a file and play.** Kirby spawns, the HUD draws
(ability, lives, health, battery), input moves him, the AI Kirbys wander off on
their own. A 6000-frame run — 100 seconds of gameplay — completes with no trap
and **zero diagnostics**, which is the number that matters most below.

The path from the last version of this file:

| Then | Now |
|---|---|
| reached a level, died partway in | **plays continuously, no trap in 6000 frames** |
| 2 functions with no C body that the port reaches | **0** |
| crash inside `kirbyFlyUp` | gone |
| crash on Start Game | fixed, and it was not a decomp bug (§3) |

### The scoreboard, re-measured today

| | |
|---|---|
| Game sources compiled | 190 |
| Undefined symbols at link (`make stubs`) | **15**, and **none of them are game logic** |
| ROM function-table entries wired to real C | 304 of 305, across 8 tables |
| Function pointers patched inside ROM structs | 193 of 219 |
| Linker-placed RAM symbols reconstructed | 183 of 193 |
| ROM data symbols resolved | 183, plus 14 more from `katam.map` |

All 15 remaining stubs are m4a, MultiBoot/MultiSio, `IntrMain` and two data
symbols — every one of them deliberate (§7). **There is no game function left
that the port needs and cannot get.**

### The strongest single measurement

Every stub calls `PortMissingFunction()`, which reports itself the first time it
runs. A full boot → title → file select → Start Game → 100 seconds of gameplay
logs **not one such report**. Nothing the port actually reaches is missing a
body any more. That is entirely your doing.

### Re-measuring it yourself

```bash
cd ~/Desktop/katam-port
make sync && make stubs   # re-derived from a real link; shrinks by itself
make test FRAMES=6000     # boots under node with a ROM, writes build/frame.ppm
MASH="120:17:20" make test FRAMES=1500   # taps A+Right — walks Kirby around
```

---

## 1. Everything the last version asked for is closed

For the record, since the previous list was six items:

| Ask | State |
|---|---|
| `sub_08038010` | **has C** (`code_08032E98.c`) |
| `sub_08038B34` | **has C** (`code_08032E98.c`) |
| `sub_08037314` blocks 2–6 | **complete**, warning withdrawn |
| `sub_0815436C`, `sub_081548A8`, `sub_08154B14`, `sub_08155604` | **all four have C** (`sprite.c`) |
| `gUnk_0834BD88` extent | **answered — and I was the one who was wrong** (§2) |
| the three uninitialised variables | **answered**, and the port's handling was already right |

On the last one: your analysis showed `dark_mind.c:8664` is a live bug if
zeroed, while the two `multi_08030C94.c` sites are register-allocator nudges
where zero is provably harmless. The port compiles all three under `PORTABLE`,
where `ASM_OUT_R` expands to nothing at all — so it never writes the zero, and
`dark_mind.c` keeps whatever the compiler had. That is the behaviour you said
was required, and it was reached by accident rather than by design; it is now
deliberate and commented.

---

## 2. Correction: `gUnk_0834BD88` — my "12 entries" was wrong

The previous version of this file reported that the table walks to at least 12
entries and asked whether the `0xC` label boundary was wrong. **It was not. The
table is exactly 3, as declared, and the reasoning that produced 12 was
unsound.** Your three independent lines of evidence are accepted in full.

The mistake is worth stating plainly because it was a *method* error, not a
data one. The port's generator used to walk forward from a table's label for as
long as the following words still resolved to known functions. That heuristic
cannot distinguish "this table continues" from "the next table begins", and
`0x34BD88 + 0xC == 0x34BD94` means these two labels abut exactly — so the
successor's entries resolved perfectly and looked like more of the same table.
The walk was guaranteed to overrun wherever two function-pointer tables are
adjacent, which is everywhere they are grouped.

It is removed. The generator now emits each label's own extent and nothing
more, and instead **verifies** every entry's resolved signature against the
table's declared element type, reporting a mismatch rather than casting over
it. That check is strictly better than the walk: it catches the thing the walk
was groping for (a wrong boundary shows up as a signature that does not fit)
without inventing entries.

Net effect on the numbers: the wired-entry count went from 312/314 to 304/305,
and the eight phantom entries are gone. No ask here — recorded so the wrong
number does not outlive the correction, and so nobody re-derives it.

---

## 3a. New problem class: reads through a pointer that is not valid yet

**This is the most useful thing in this document, and it needs nothing from
you.** It is reported because it is invisible from the ARM side by
construction, and because the port will keep hitting it.

### The case that was killing Start Game

`sub_080338B4` builds the HUD. At that moment `ObjectBase::roomId` is still
`0xFFFF` — `sub_0803EA90` sets it to `0xFFFF` deliberately, and nothing fills
it in until `sub_08055920`, which `sub_080332BC` calls *after* `sub_080338B4`
has returned. So `sub_08034FA8` runs this:

```c
gUnk_08D6CD0C[gKirbys[gUnk_0203AD3C].base.base.base.roomId]->unk46
```

with `roomId == 0xFFFF`. Following it through the ROM:

```
index address  0x08D6CD0C + 0xFFFF*4 = 0x08DACD08   <- still inside the ROM
value there                          = 0x100F0D0F   <- not a pointer
dereferenced at +0x46                = 0x100F0D55   <- the GBA does not decode this
```

On hardware that read returns open bus and the console carries on. **And the
result is discarded regardless** — `sub_080338B4` zeroes the very bytes it
feeds, on the next line:

```c
sub_08034FA8(NULL);
CPU_FILL(0, (void *)0x060077A0, 0x100, 16);
```

So the code is correct as decompiled, the hardware is fine with it, and nothing
is even lost. WebAssembly simply has no open bus: `0x100F0D55` is past the end
of linear memory and the load traps, taking the game down the instant you press
Start Game.

The port fixed it on its own side (`PortRoomTilesetIndex` checks the pointer
against the ROM that was actually loaded, and returns 0 otherwise). **No change
is wanted in the decomp** — patching this would make the C less faithful to
solve a problem the C does not have.

### Why you should know about it anyway

The general shape is: *the game forms an address from a field before that field
is initialised, dereferences it, and discards the result.* On ARM this is free.
In any port to a bounds-checked target it is fatal, and it is undetectable
statically because the pointer is perfectly well-typed.

Two things that would help, both cheap and neither urgent:

1. **When you notice one, leave a comment.** A one-liner like `/* roomId is
   0xFFFF here; this read is discarded */` at the site turns a multi-hour hunt
   into a five-minute fix. You spot these naturally while reading the assembly;
   the port only finds them by crashing.
2. **Sentinel values are worth naming.** `0xFFFF` as "no room yet" appears in
   `sub_0803EA90`, `code_0806F780.c:5258` and `:8458` at least. A named
   constant would make "is this initialised?" answerable by grep.

---

## 3b. The one that was hiding every level: an open-coded DMA register write

Same family — invisible from the ARM side, fatal off it — but this one had a
much larger blast radius, and it is the single most useful thing the port has
learned since the last version of this file.

`sub_08153184` in `src/bg.c` is the only routine that writes a level's BG
tilemap into VRAM. Its innermost row-copy loop does not call `DmaCopy16`; it
stores to the DMA3 registers by hand:

```c
vu32 *dmaRegs = (vu32 *)(0x4000000 + 0xd4);
dmaRegs[0] = (vu32)(r2);
dmaRegs[1] = (vu32)(r4);
dmaRegs[2] = (vu32)((0x8000 | ...) << 16 | (((r6->unk26 - (size)) * sp8)/(16/8)));
dmaRegs[2];
```

The port emulates DMA in software: `DmaSet` is redirected to `PortDmaSet`,
which performs the copy. A raw store to `0x040000D4` moves nothing — it lands
in emulated IO memory and returns. There is no way to trap it, because the GBA
map is ordinary WebAssembly linear memory with no write hook.

**This is the only raw DMA-register write in the entire game** (one grep hit
across all 190 files), which is why nothing else was affected — and why it took
so long to find. The symptom pointed everywhere except here:

- Tile *graphics* were always perfect: they arrive via `LZ77UnCompVram` and the
  VBlank queue, both of which use ordinary DMA.
- Every level *tilemap* was stale. Diffing VRAM across a level load showed
  screen blocks 29, 30 and 31 as the only regions in all of VRAM that never
  changed — still holding the file-select screen's fill of tile `0x1FF`.
- A room whose background is at most 32×32 takes a different branch that *does*
  use `DmaCopy16`, so some screens rendered correctly. Rainbow Route 2 is 45
  wide, took the broken path, and not one entry was written.

The port substitutes back the decompilation's own commented-out equivalent,
which sits one line above the poke:

```c
DmaCopy16(3, r2, r4, (r6->unk26 - (r8 - 1)) * sp8);
```

That is exactly equivalent — the hand-built control word `0x8000 << 16` is
`DMA_ENABLE | DMA_START_NOW | DMA_16BIT | DMA_SRC_INC | DMA_DEST_INC`, and the
count arithmetic is identical. With it, levels render.

**No ask, and specifically no request to change `bg.c`.** The open-coding is
there to reach a 95.2% match and it is doing its job; `-DNONMATCHING` is the
port's problem to solve and it has. Recorded because:

1. If any *future* function open-codes a hardware register write for match
   reasons, the port will silently do nothing and the failure will again look
   like something else entirely. A one-line comment saying "raw DMA3 write,
   equivalent to `DmaCopy16(...)`" costs nothing and would have saved a day.
2. It is worth knowing that this construct exists exactly once, so if the
   count ever becomes two, that is a thing to mention.

---

## 4. New sub-class: function pointers mis-cast in the C source

Distinct from §5, which is about pointers stored in ROM data. This one is about
casts written in the decompiled C itself, and it is also **not a defect** — the
C is faithful.

Seven call sites over four functions hand a function to a slot whose signature
differs:

```c
src/code.c:33          TaskCreate(sub_08001FF8, 0, 1, 0, (TaskDestructor)sub_08002E3C);
src/code_0802B4A8.c    gCurTask->main = (TaskMain)sub_0802D528;   /* and D53C, D550 */
```

`sub_08002E3C` takes **no arguments**; `TaskDestructor` is called with one. On
ARM a call just loads `r0`–`r3` and branches, so the unread argument costs
nothing and the cast is the honest way to write it. WebAssembly type-checks
every indirect call against the call site and traps —
`call_indirect to a signature that does not match` — naming neither the pointer
nor the caller. This is what killed the port whenever Kirby went through a
door.

The port routes all seven through adapters with the signature the call site
expects. It also has `tools/check_fnptrs.py`, which finds the class statically:
81 function-pointer slots, 3378 address-takes, exits non-zero on any mismatch.
Run against the pre-fix tree it finds exactly those seven sites — no more, no
fewer — so the class is closed and stays closed.

**No ask.** Recorded because if you ever wonder whether a cast like that is
load-bearing: it is, for anyone building this tree for a non-ARM target, and
the port has it handled.

One inference the port would like checked if it is ever cheap: the three
`(TaskMain)` adapters pass `TaskGetStructPtr(gCurTask)` as the argument, on the
grounds that it is the only value that makes those functions work and it is
what every neighbouring main fetches for itself. The reference assembly for
`TasksExec` would settle whether `r0` genuinely holds that at the call. It is
labelled as an inference in `platform/adapters.c`, not as fact.

---

## 5. ROM function-pointer tables — unchanged, still the long-term ask

A function pointer stored in ROM is an **ARM code address**. In WebAssembly a
function pointer is an **index into the module's function table**, so such a
value is not merely wrong, it is out of range.

The port resolves each address back through `katam.map` and emits a real
reference to the decompiled C function. Three shapes are covered automatically:

| Shape | Example | State |
|---|---|---|
| flat function-pointer arrays | `gSpawnFuncTable1` | 304 of 305 across 8 tables |
| arrays declared through a typedef | `gUnk_082EB7D0` | covered |
| function pointers *inside* ROM structs | `gUnk_08351648` — 219 object descriptors with a per-type setup routine at +0x10, patched at startup | 211 of 219 |

The one flat-table entry that still cannot be wired:

```
gSpawnFuncTable1[114]  CreateBossChallengeDoor   (boss_challenge_door.s, claimed)
```

### 5a. 193 of those 219 became 211, and the missing 18 were never missing

Recorded because the number in the last version of this file was misleading,
and the reason is worth knowing if you ever reason from `katam.map`.

`katam.map` lists **global** symbols. A `static` function in the tree is not in
it at all. Eighteen of the twenty-six unresolved `gUnk_08351648[].unk10`
entries pointed at ROM addresses that the map simply does not name — not
because the function was still ARM assembly, but because its perfectly good
decompiled C has internal linkage:

```
gUnk_08351648[0x08]  OBJ_CHIP              0x080AB720  sub_080AB720   chip.c
gUnk_08351648[0x12]  OBJ_WADDLE_DOO        0x080B6A54  sub_080B6A54   waddle_doo.c
gUnk_08351648[0x2E]  OBJ_FOLEY_2           0x080C0CBC  sub_080C0CBC   foley.c
gUnk_08351648[0x37]  OBJ_WADDLE_DEE_2      0x080A42B0  WaddleDee37ChooseXSpeed
gUnk_08351648[0x5E..0x64]  the pickups     0x08122F6C  BonusSetFunc   bonus.c
gUnk_08351648[0x66..0x6C]  OBJ_EMPTY_66..  0x08123780  sub_08123780   bonus.c
gUnk_08351648[0xA2]  OBJ_PARASOL           0x080C2B28  sub_080C2B28   parasol.c
```

`katam.elf`'s symbol table has every one of them, so the port now reads that as
well as the map, and gives those seven functions external linkage in its own
copy of the tree. `unk10` is the routine each object type runs *after* it is
constructed, so before this the pickups — small food, meat, tomato, the 1UP,
the invincibility candy — and Chip, Waddle Doo, Waddle Dee 37 and the parasol
were all being created and then not set up.

**No ask.** `static` is correct and the decompilation should keep it; this is
the port reading the wrong file. Recorded because "not in `katam.map`" reads
like "not decompiled yet" and is not the same statement.

### 5b. The two that really are still ARM

After the above, the whole remaining gap in `gUnk_08351648` is two functions:

```
sub_08106508   blocks OBJ_DARK_MIND_STAR_FIRE / _ICE / _SPARK / _MIX, OBJ_UNKNOWN_D4
sub_08118C18   blocks OBJ_BOSS_CHALLENGE_DOOR
```

and two entries whose pointer is genuinely `NULL` in the ROM
(`OBJ_ABILITY_STATUE_RANDOM`, `OBJ_EMPTY_9A`), which need nothing.

`sub_08118C18` pairs with `CreateBossChallengeDoor` above — the same object,
already claimed. `sub_08106508` is the Dark Mind fight, which the port cannot
reach yet anyway. Neither is urgent; they are listed because they are now the
complete list.

**Whenever data conversion is on the table, ROM function-pointer tables from
`.incbin` into typed C in `data/` is the highest-value target by a wide
margin.** It removes this whole class rather than an instance of it. No
urgency — the current mechanism works and is now signature-verified (§2).

---

## 6. Still true, still not requests

- **`inline` without `static`.** The tree relies on gnu89 semantics; C99
  inverted the rule, so a modern compiler turns every one into an undefined
  symbol. The port passes `-fgnu89-inline` and is fine. Recorded because it is
  a portability landmine that looks like nothing.
- `gLevelObjLists` is an array of an anonymous `transparent_union`
  (`include/data.h:847-849`), and `src/kirby.c:5339` declares `gUnk_08D60FB4`
  and `gUnk_08D60FDC` in one `extern` with two declarators. Both legal, both
  needed special handling in the port's header parser.
- The **"flag load-bearing return values"** convention keeps paying for itself.
  The port's side is `tools/stub_returns.conf`.

---

## 7. What the port does *not* need

- **The sound engine.** `m4a_asm.s` and friends are replaced wholesale by
  no-op stubs; `src/m4a.c` and `src/m4a_tables.c` are dropped from the build
  entirely. Silence is a deliberate choice, not a gap.
- **Link cable.** `multi_sio_asm.s`, `multi_boot.c`, `sio32_multi_load.c`
  stubbed. Single-player does not touch them.
- **`agb_sram.c`.** Replaced, and it cannot be otherwise: it copies the machine
  code of its own inner loop into a stack buffer and calls it, which is not
  expressible in WebAssembly.
- **Byte-matching, in general.** The port needs *correct C*, nothing more. A
  function sitting at DIFF is 100% usable, and the port builds `-DNONMATCHING`
  so it takes the readable branch wherever one exists.

**The port's gate is "has a C body", not "matches" — and that gate is now
open for everything it reaches.**

---

## 8. The ask, in order

Short list, because you closed the long one.

1. **Nothing is blocking the port.** There is no function it needs that it
   cannot get. This line has never been in this document before.
2. **`CreateBossChallengeDoor`** and **`sub_08118C18`** — the spawn function
   and the per-type setup routine for the same object,
   `gSpawnFuncTable1[114]` and `gUnk_08351648[0x72]`. Claimed by someone
   else; not urgent. **`sub_08106508`** (§5b) is the only other object-type
   routine in the game with no C body.
3. **A comment when you spot a discarded read through an uninitialised
   field** (§3a), or when a function open-codes a hardware register write for
   match reasons (§3b). Cheap for you, a day each for the port — these are the
   two shapes that are free on ARM and fatal off it, and neither is visible
   from your side.
4. **Whenever data conversion is on the table: ROM function-pointer tables
   into typed C** (§5). Not urgent, highest long-term value.

### Background rendering — solved, and it was not a decomp gap

The previous paragraph here said the levels drew as flat wallpaper with no
geometry, and guessed it was the port's own PPU or BG setup. It was neither:
it was §3b, and the levels now render — scenery, geometry, parallax and
scrolling. `DISPCNT = 0x1B40` with BG2 disabled turned out to be exactly
right for those rooms, because their `objectList2Idx` is `0xFFFF`.

Two things confirmed along the way, in case they are ever in doubt:

- The port's LZ77 output is **byte-exact** against an independent reference
  decoder for the level tilesets — 28800/28800 bytes.
- The layer assignment in `sub_08000460` reads correctly: `unkC0[1]` → BG3
  (geometry), `unkC0[2]` → BG0 (scenic backdrop), `unkC0[0]` → BG2 (second
  object layer, off when a room has none). The `unk2E & 3` convention for
  which hardware BG a `struct Background` drives is the key to reading that
  code, and it cost some time to find.
