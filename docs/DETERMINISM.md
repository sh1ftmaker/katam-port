# Determinism and the regression harness

Every later change to the port -- the modern renderer, game-logic changes,
netplay work -- is checked against one question: *did the simulation do the
same thing, frame for frame?*  This is the instrument that answers it.

    make regress            # sim mode: the game's state must not move
    make regress-pixels     # pixels mode: where the picture changed
    make regress-wasm       # the node build against the same goldens
    make lint-determinism   # no unsequenced RNG use (run by both regress targets)

`tests/regress.sh` builds nothing; build the native binary (`make native`, or
the LP64 CMake build in `build/lp64`) or `build/katam-node.js` first. It needs
your ROM: `ROM=/path/to/rom.gba`. The five recordings take about 30 s natively
and 40 s under node.

## The pieces

**Recording** (`platform/inputlog.c`, shared by both builds). Every key mask
the game latches -- at the single latch, `UpdateKeyInput` in
`platform/main.c`, after rollback and seats have had their say -- and every
warp the host asks for, by frame, as a text file:

    katam-input 1
    frames 7300
    option rando 4127 0x11         (only if set)
    option view 1 40 0 0 0 2       (only if set)
    warp 1300 0 101 3 7            (frame player room cellX cellY)
    k 0 000                        (frame mask: the mask from this frame on)
    k 120 011
    end

Inputs only. Nothing in a recording comes from the ROM.

**Replay** overrides the latch from the file, so a replay depends on nothing
the host polls: not the keyboard, not `--mash`, not the harness. Recorded
options are re-applied at load.

**The hash log.** One line per frame:

    frame keys sim      vid      sram     pix
    1398  000  E07F8F69 F7F14549 A59C9F65 7B3A1FC5

taken at the top of `PortPresentFrame` -- the point where rollback snapshots
-- except `pix`, which is the frame drawn later in the same call, and `keys`,
the mask latched at the end of it.

**Dumps.** The bytes behind `sim`/`vid`/`sram` at chosen frames, for
`tools/state_diff.py`, which says which words differ and classifies them.

Native flags: `--record FILE`, `--replay FILE` (runs exactly the recording's
frames unless `--frames`), `--hash-log FILE`, `--dump-state F:PATH`
(repeatable), `--warp F:ROOM[:X:Y]` (recorded). Node harness
(`tools/headless_test.js`): `REPLAY=`, `HASH_LOG=`, `DUMP_STATE=F:path,...`.

## What "simulation state" is

The existing definition is the rollback snapshot (`platform/rollback.c`,
`kRegions` plus the port's own out-of-map state): if restoring those bytes
re-runs a frame identically, they are the state. The hash splits it, and takes
out what belongs to the host rather than the game:

| column | covers | not covered, and why |
|---|---|---|
| `sim` | EWRAM, IWRAM, the I/O block, the DMA channel table, the frame state (frame number, VBlank DMA budget, the keys the next frame latches) | the four DMA source/destination registers in I/O: `platform/dma.c` mirrors the *host* address into them, a stack address for `DmaFill`. The channel table's own pointers are reduced to the GBA address they name, or a marker for port memory. |
| `vid` | palette, VRAM, OAM, the PPU's affine reference latches | the framebuffer -- that is `pix` |
| `sram` | save memory | -- (not in a rollback snapshot, so a rollback never un-writes a save; it is game-written and decides the next boot, so it has a column) |
| `pix` | the framebuffer the frame drew | -- |

Never hashed: the audio mixer and ring (measured not to feed game state with
audio off; see "Audio" below), diagnostics counters, and the view/rando
*settings*, which are inputs and live in the recording.

`vid` is separate from `sim` because the game writes it and the renderer reads
it; a renderer change may legitimately change how the PPU latches move, and the
game never reads the picture back. `sim` mode fails on `keys` or `sim` and
reports `vid`/`sram`/`pix` without failing.

### Host addresses in game memory

The game stores function pointers in its own memory -- task callbacks,
`Object::unk78` state functions, `gIntrTable`, the sound engine's jump table --
and pointers to its C-defined const tables. Natively those are addresses in
this executable, which move whenever code linked ahead of them changes size, so
a raw hash of EWRAM would report a divergence for a change that altered
nothing. `platform/native/symtab.c` reads the executable's own ELF symbol table
and the hash replaces a word that is *exactly* a function's entry point with
`0xC0000000|hash(name)`, and exactly a data object's start with
`0x80000000|hash(name)`. Exact starts only: the game's data is full of aligned
words that fall inside some object by coincidence (the shadow OAM, for one),
and the first version, which canonicalised interior pointers, turned every one
of them into a false divergence. Identical-code-folded aliases share one token.
Verified: inserting code and data into the first game file and into
`platform/ppu.c` leaves every hash of the 7299-frame play recording unchanged.
Linux only (`/proc/self/exe`); elsewhere the hook is the identity and the hash
is still deterministic, just layout-sensitive. `PORT_HASH_RAW=1` turns it off.

Two copies of host machine code into IWRAM (`IntrMain`, `MultiSioIntr`) made
IWRAM differ between any two builds; they were never executed and are gone
(`game/src/main.c`, `multi_sio.c`, `sio32_multi_load.c`, `PORT-FORK:`).

### What still makes the native `sim` column layout-sensitive

The `sim` column is a *same-build-family* check: a change that moves code or
the game's data can move it with the game's state unchanged.  Found and
removed here:

- **Pooled constants moved the game's tables.**  The game keeps pointers
  *into* its const tables (`&gUnk_082EB2B8[1]`), which canonicalisation cannot
  name, and gcc's mergeable float/string constants from every object were
  pooled ahead of them, so a platform file losing a `double` moved every game
  table.  `-fno-merge-constants` (CMakeLists.txt): each object's constants
  stay in its own `.rodata`, game first, platform after.
- **Shadow OAM words that equal a function.**  OAM attribute pairs are
  arbitrary 32-bit words; `0x10190050` was `sub_0800E0AC` in one layout and
  mid-function in the next.  `gUnk_030031C0` and `gOamBuffer` are hashed and
  dumped raw (`kNoPointers`, platform/inputlog.c).

Left: any game word that coincides with a symbol's start elsewhere in memory,
and *stale bytes* -- a destroyed task's host function pointer partly
overwritten by the next struct (attract's last frames leave `0x10000000`
natively where wasm has `0` at iwram 0x03004880; the game does not read the
byte, or `vid`/`pix` would follow).  So a change that moves code re-records the
goldens, and the commit has to show the move is layout-only: `keys`, `vid`,
`sram` and `pix` identical on every frame, and raw dumps
(`PORT_HASH_RAW=1 --dump-state`) differing only in words that are the same
host pointer at two layouts -- `tools/layout_diff.py` and
`tests/layout_neutral.sh` on the resources branch do exactly that.  The real
fix is host-independent function references (netplay-v2 R5).

## The recordings

`tests/recordings/`, generated by `tools/make_recordings.py` (regenerate, then
`tests/regress.sh --update`, and say why in the commit):

| recording | frames | what it covers |
|---|---|---|
| `boot_to_level` | 1400 | power-on, logo, title, file select (new file), the first level -- the `native_smoke.sh` input |
| `attract` | 9000 | no input: the title screen times out into the game's own attract-mode demo, the game playing room 804 with its recorded input, through the demo's end and back to the title |
| `play_rooms` | 7300 | boot, then a warp into each of ten level rooms (101, 104, 106, 202, 205, 310, 402, 502, 601, 704) and 600 frames of play in each: float right, Up for doors, exhale |
| `warp_sweep` | 7060 | boot, then a warp every 160 frames through 36 rooms spread over every area (every eighth room with an arrival cell) |
| `aim` | 6700 | boot, then 600 hands-off frames in rooms with the code the determinism work changed: Shotzo and Bang-Bang (aim with `ArcTan2`), Kracko, Gobbler (RNG in argument lists), Mega Titan (arm setup through a NULL parent) |

Goldens are in `tests/golden/*.hash`, produced by the LP64 build. Every
run uses a fresh `XDG_DATA_HOME`/`XDG_CONFIG_HOME` -- the native host loads and
writes a save there, and two runs sharing one diverge -- and `setarch -R`.
Repeat runs are byte-identical.

Kirby does not take a door on his own in any of them -- the door positions are
not decoded (`docs/naming/doors.md`) and blind input did not find one -- so
room changes come from `PortWarp`. The scripted play is real play inside each
room: movement, enemies, collisions, abilities.

## Native vs wasm

Run `make regress-wasm`, or by hand: replay the same file through both, then
compare `keys`/`vid`/`sram`/`pix` per frame and `sim` through dumps.

| recording | keys, vid, sram, pix | sim (state_diff at the last frame) |
|---|---|---|
| boot_to_level | identical, all 1399 frames | identical but for pointer spelling |
| attract | identical, all 8999 frames | identical but for pointer spelling and one stale host byte (above) |
| play_rooms | identical, all 7299 frames | identical but for pointer spelling |
| warp_sweep | identical, all 7059 frames | identical but for pointer spelling |
| aim | identical, all 6699 frames | identical but for pointer spelling |

"Pointer spelling": a stored function pointer is a code address natively and a
table index in wasm, a pointer to the game's const data is in the executable
natively and above `GLOBAL_BASE` in wasm. `tools/state_diff.py --a-bin
build/lp64/katam --b-bin build/katam-node.wasm` maps both back to names
(including identical-code-folded ones) and reports what is left. A per-frame
cross-platform `sim` hash would need the game to store host-independent
function references, which is the netplay-v2 work (`design/NETPLAY-V2.md`).

### Unsequenced RNG: `make lint-determinism`

`warp_sweep` used to diverge at frame 3132, in room 415, `sub_0801617C`:

    a1->unkE8 = (Rand16() & 7) + 0xF + (Rand16() & 0xF);

`Rand16()` is `(Rand32(), gRngVal >> 16)`, so the two calls and the two reads
of `gRngVal` are unsequenced against each other, and every compiler picks its
own order.  `tools/lint_determinism.py` finds them all: libclang parses each
game file, pass 1 summarises which functions advance or read `gRngVal` and
closes that over the call graph, pass 2 applies C's sequencing rules to every
full expression (`,` `&&` `||` `?:` sequence; operator operands, call
arguments and initialiser lists do not).  The same check covers the
allocators (`TaskCreate`, the heaps, `VramMalloc`).  It found **39 sites**
(netplay-v2's 42 regex lines less four that are sequenced, plus one call over
four lines the regex missed); there are none now, and `make regress` /
`regress-wasm` run it first.  `tests/lint/determinism_selftest.c` is its test.

The order each site now spells out is agbcc's, read from the ROM's code
(the decomp's `katam.elf`), because that is what the game was:

| shape | sites | agbcc | g++ (LP64) | clang (wasm) |
|---|---|---|---|---|
| two `Rand16()` as operands of one arithmetic operator | 28 | advance twice, **one** read feeds both (gcc folds the commas out) -> `Rand16Twice()` | same as agbcc | interleaved: two values |
| one RNG call per function argument | 11 | left to right | right to left | left to right |

So the first shape was a wasm-only divergence and the second a native-only
one.  clang `-Wunsequenced` and gcc `-Wsequence-point` find nothing else in
the game but `r5 = r5 = x` (kirby.c, harmless, removed).

### Divergences found and fixed here

Each found by this harness; each made the two builds disagree, or one of them
crash, where the other did not.

- **`gCurLevelInfo[0xFF]`.** Objects with no owning player (`unk56 == 0xFF`)
  index 255 entries past the table, ~300 KiB past EWRAM. The console reads an
  EWRAM mirror, wasm its own (zero) linear memory, the LP64 build segfaulted --
  five different object destructors, every time a run warped out of a room
  holding one. `.unk65E` reads now go through `EWRAM_MIRROR_U8` (what the
  console reads); natively the pages past EWRAM are mapped and zero, as in wasm,
  for the other reads of that shape.
- **`ForegroundPalette_4pp::u16ppp`** had a host-width innermost pointer, so the
  LP64 build skipped animated-palette loads: first divergence of `play_rooms`,
  frame 1900.
- **A NULL `Object` read** in `sub_080720F8` (BIOS on the console, segfault
  natively).
- **Uninitialised locals** read stack leftovers that differed by build and even
  by whether the hash was on. `-ftrivial-auto-var-init=zero` in both builds.
  With it, the `sim` hash of three recordings changed and nothing else did.
- **`-ffp-contract=off`** natively, so an FMA-capable target (arm64) cannot
  round `ArcTan2`'s arithmetic differently from wasm, which has no FMA.
  (Moot since `ArcTan2` is integer; kept for the mixer.)
- **Table over-reads** that read the linker's layout instead of the ROM's
  (UBSan): `gUnk_083507F8` read up to index 117 of 15 (now one array with the
  tables that follow it in ROM; this was the wasm trap below), `sub_08070164`
  reading one row past three tables (`ROM_NEXT_ROW`), the intro script loop
  one past its 56 entries, `gIntrTable[14]` from a 14-entry template.
- **`1 << unk56` with `unk56 == 0xFF`** (`Macro_0810B1F4`): x86/wasm mask the
  count, the ARM7 gives 0; now 0.
- **Reads through NULL** the console answers from BIOS open bus: Mega Titan's
  arm setup reading its not-yet-set parent (answered as the console's non-zero
  byte), `sub_08002374` after the display task is destroyed (player 0).

## Floating point and libm

Where floats reach the simulation:

- **`ArcTan2`** (`platform/bios.c`) is now the BIOS's own integer routine
  (ArcTan at 0x474, ArcTan2 at 0x4FC; the GBATEK polynomial), no floating
  point.  Checked against the real BIOS run under an emulator on 7.75 M inputs
  (all |x|,|y| <= 1024, a full-range lattice, both diagonals, axes, extremes,
  3 M random): identical.  The atan2() version it replaced disagreed with the
  BIOS on 57% of inputs, by 1 or 2 units; over all 2^32 pairs the 8-direction
  bucket changed for 272,455 and the 16-direction one for 592,939.
- **`ObjAffineSet`/`BgAffineSet`** use `sin`/`cos`; the game never calls them.
- **`sub_08158654`** (`game/src/palette.c`) uses `powf`; nothing calls it.
  `stubs.c` lists `powf`, `sin` and `cos` only because they are referenced
  somewhere in the tree.
- The audio mixer is float, and does not feed game state.

So nothing float reaches the simulation through `ArcTan2` any more.  The
affine helpers and `powf` are uncalled; a no-libm lint for sim code is still
to do (netplay-v2 M0).

### Audio

With an audio device the mixer's block size follows the device queue
(`platform/audio.c`, `ChooseBlockSize`), and `pcmSamplesPerVBlank` -- a field
the game can read -- comes from the device rate. Every run here uses
`--no-audio` / `AUDIO=0`; a run with sound is not comparable to one without
(docs/DEBUG-TOOLING.md §7 measured it moving EWRAM at frame 63). For netplay the
mixer will have to be driven by frames, not the device.

## Known issues the harness tripped over

All three that were here are fixed, and the recordings now cover them:

- LP64: Mega Titan's arm setup faulted on arrival in room 738 -- a read
  through the arm's NULL parent (`mega_titan.c`, `TitanArmParentUnkC4`).
  Room 738 is in `aim`.
- LP64: the attract demo faulted at frame 6744 in `sub_08002374` -- the
  display task was gone (`code_080023A4.c`).  `attract` runs 9000 frames.
- wasm: the 36-room sweep trapped at frame 5781 in `sub_080788AC` ->
  `sub_08155128` -- `gUnk_083507F8` read past its end into whatever the
  linker put next (`kirby.c`).  `warp_sweep` is that sweep.

UBSan (`-fsanitize=undefined`, LP64, all five recordings) reports nothing else
that differs by host but three reads left as they are: `gCurLevelInfo[255]`
in `object_collision.c:490` (the `unk56 == 0xFF` shape; zero pages in both
port builds, an EWRAM mirror on the console), `unk48[-1]` inside a struct
(`intro.c:3313`, layout-stable), and an out-of-enum load in `save.c:214`.
Signed overflow is defined by `-fwrapv` in every build.

## One toolchain (netplay-v2 R2): not yet

The LP64 build compiles the game as C++ with g++ `-fpermissive`; the wasm
build is clang C.  The sequencing fixes above make the two agree today, but
"one compiler, one language mode" is still the goal.  Measured: the LP64 build
under clang++ stops on about 3,700 errors, almost all from four C-isms
g++'s `-fpermissive` accepts and clang++ has no switch for -- implicit
`void *` -> `T *` (~1,170, mostly `TaskGetStructPtr` results), function
pointer -> `void *` (~860, `ObjectSetFunc`), `void *` arithmetic (~2,000,
mostly the `TaskGetStructPtr` macro), and `(u32)ptr` (~90) -- plus an
ambiguous surrogate call through function-typed `P32` (~200).  A header-level
pass (a `void *` proxy for `TaskGetStructPtr`, an `ObjectSetFunc` template,
`P32::operator()`, a `uintptr_t` macro body) removes most of the first and
last; the rest is a few hundred mechanical source edits.  Not done here.

## Using it

- **Renderer work:** `make regress` must stay green (or, where code moved,
  show it is layout-only -- see "What still makes the native `sim` column
  layout-sensitive"). `make regress-pixels`
  shows the first frame each recording's picture changed; that is expected.
- **Game-logic work:** `make regress` will go red where the change takes
  effect; the first divergent frame is the first frame the change mattered.
  Read it, then `tests/regress.sh --update` and commit the new goldens with the
  change, saying why they moved.
- **Finding what differs:** dump the frame before and the frame of the
  divergence from both runs (`--dump-state`, `DUMP_STATE=`) and
  `tools/state_diff.py A B --a-bin ... --b-bin ...`. `game/generated/port/
  ram_symbols.h` names the fixed addresses; `gdb` with
  `watch -l *(u16*)ADDR` on the native build finds the writer.
