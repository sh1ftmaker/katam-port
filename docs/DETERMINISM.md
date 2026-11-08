# Determinism and the regression harness

Every later change to the port -- the modern renderer, game-logic changes,
netplay work -- is checked against one question: *did the simulation do the
same thing, frame for frame?*  This is the instrument that answers it.

    make regress            # sim mode: the game's state must not move
    make regress-pixels     # pixels mode: where the picture changed
    make regress-wasm       # the node build against the same goldens

`tests/regress.sh` builds nothing; build the native binary (`make native`, or
the LP64 CMake build in `build/lp64`) or `build/katam-node.js` first. It needs
your ROM: `ROM=/path/to/rom.gba`. The four recordings take about 20 s natively
and 30 s under node.

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
`0xC0000000|hash(name)`, and a pointer to or into one of its data objects
(start, interior, or one past the end) with `0x80000000|hash(name, offset)`.
Identical-code-folded aliases share one token. Linux only (`/proc/self/exe`);
elsewhere the hook is the identity and the hash is still deterministic, just
layout-sensitive. `PORT_HASH_RAW=1` turns it off.

**Why the image is pinned at 0xB5000000.** Recognising a host address *by
value* is only sound if no game value can be mistaken for one. With the image
at 0x10000000 that failed in both directions, and the first renderer commit,
which changed no game code at all, "diverged" every recording:

- *Constants taken for function pointers.* 0x1000 is 1.0 in the game's 4.12
  fixed point, so packed u16 pairs in the 0x1000xxxx range are common; the old
  goldens themselves canonicalised the constant 0x10030000 at 0x03003EE4 as
  `sub_08005DE0` (boot_to_level, frame 580) because that function happened to
  be linked there, and the intro's 0x10010000 at 0x03004E38 became
  `sub_080CA0A4` the moment a 128-byte change to the PLT moved that function
  onto it.
- *Interior pointers left raw.* The attract demo keeps a pointer into one of
  the port's static tables (frame 1551); any change that grew the code moved
  the table and the raw pointer with it. Canonicalising interior pointers was
  tried before and dropped because at 0x10000000 palette colour pairs and
  shadow-OAM words fall inside objects by coincidence.

Above 0x80000000 no BGR555 colour pair can reach, and 0xB5xx is not a
fixed-point value the game has reason to hold, so both rules now hold:
exact function entries, and start-or-interior data pointers. The goldens were
re-recorded with this change alone (no game or renderer code): `keys`, `vid`,
`sram` and `pix` are identical to the previous goldens on every frame of all
four recordings; `sim` moved only on frames holding one of the two kinds of
word above, and `tools/state_diff.py` between the old and new builds finds
nothing else (boot_to_level 580/1398, attract 1551/6598, play_rooms
4000/7298, warp_sweep 3000/5648).

**What it still cannot see.** The game reuses heap memory without clearing
it and sometimes initialises a struct only partly, so for a frame or a few it
can hold a *fragment* of a stale host function pointer -- e.g. `unk1D` of the
world map's Kirby sprite at 0x03004275 is the middle byte of the task callback
that last owned that memory (warp_sweep frame 4988; attract 5715-5720). No
by-value rule can recognise a fragment, so moving a game function moves those
frames' hashes. Changes to game code should expect it and confirm with
`state_diff.py`; changes outside game code do not move game functions and do
not hit it (the renderer's hooks are linker wraps for that reason,
docs/RENDERER.md). The real fix is netplay-v2's R5: no host pointers in
simulation memory.

Two copies of host machine code into IWRAM (`IntrMain`, `MultiSioIntr`) made
IWRAM differ between any two builds; they were never executed and are gone
(`game/src/main.c`, `multi_sio.c`, `sio32_multi_load.c`, `PORT-FORK:`).

## The recordings

`tests/recordings/`, generated by `tools/make_recordings.py` (regenerate, then
`tests/regress.sh --update`, and say why in the commit):

| recording | frames | what it covers |
|---|---|---|
| `boot_to_level` | 1400 | power-on, logo, title, file select (new file), the first level -- the `native_smoke.sh` input |
| `attract` | 6600 | no input: the title screen times out into the game's own attract-mode demo, the game playing room 804 with its recorded input |
| `play_rooms` | 7300 | boot, then a warp into each of ten level rooms (101, 104, 106, 202, 205, 310, 402, 502, 601, 704) and 600 frames of play in each: float right, Up for doors, exhale |
| `warp_sweep` | 5650 | boot, then a warp every 150 frames through 29 rooms spread over every area (every tenth room with an arrival cell) |

Goldens are in `tests/golden/*.hash`, produced by the fork's LP64 build. Every
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
| attract | identical, all 6599 frames | identical but for pointer spelling |
| play_rooms | identical, all 7299 frames | identical but for pointer spelling |
| warp_sweep | **diverge at frame 3283** (`sim` at 3132) | -- |

"Pointer spelling": a stored function pointer is a code address natively and a
table index in wasm, a pointer to the game's const data is in the executable
natively and above `GLOBAL_BASE` in wasm. `tools/state_diff.py --a-bin
build/lp64/katam --b-bin build/katam-node.wasm` maps both back to names
(including identical-code-folded ones) and reports what is left; for the first
three recordings that is nothing. A per-frame cross-platform `sim` hash would
need the game to store host-independent function references, which is the
netplay-v2 work (the netplay-v2 design, `design/NETPLAY-V2.md` beside this repository).

### The warp_sweep divergence: `Rand16()` twice in one expression

Frame 3132, in room 415, `sub_0801617C` (`game/src/code_0800ECAC_2.c:3363`):

    a1->unkE8 = (Rand16() & 7) + 0xF + (Rand16() & 0xF);

`Rand16()` is `(Rand32(), gRngVal >> 16)`, a comma expression, so the two
calls and the two reads of `gRngVal` are unsequenced against each other. g++
(the LP64 build compiles the game as C++) runs both `Rand32()`s and then reads
`gRngVal` twice; clang (wasm) interleaves them. Native gets 15, wasm 18; the
timer differs, the object acts on a different frame, and the picture follows
151 frames later. There are 42 such lines (netplay-v2 §6); the fix is a codemod
sequencing them plus one compiler for every host, and it is its own milestone.
The recording is kept as it is so that `make regress-wasm` shows the moment it
is fixed.

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

## Floating point and libm

Where floats reach the simulation:

- **`ArcTan2`** (`platform/bios.c`) is `atan2()` in double, scaled and
  truncated to the BIOS's 16-bit angle. The game uses it in `kracko.c`,
  `shotzo.c`, `bang_bang.c` and `code_0806F780.c`, bucketing the angle into 8
  or 16 directions -- so a one-ulp difference between two libms matters only
  where an angle lands exactly on a bucket edge, and then it changes an enemy's
  aim.
- **`ObjAffineSet`/`BgAffineSet`** use `sin`/`cos`; the game never calls them.
- **`sub_08158654`** (`game/src/palette.c`) uses `powf`; nothing calls it.
  `stubs.c` lists `powf`, `sin` and `cos` only because they are referenced
  somewhere in the tree.
- The audio mixer is float, and does not feed game state.

Measured: glibc (native) and emscripten's musl-derived libm (wasm) agree on
`ArcTan2` for all 4.2 M inputs with |x|,|y| < 1024 and on a 676×737 lattice
over the full s16 range, and on every affine value for all 256 angles. So no
divergence today on x86-64 Linux vs wasm. Not guaranteed elsewhere: macOS's
libm, MSVC's and an ARM glibc can each round differently. The robust fix is an
integer `ArcTan2` matching the BIOS's own fixed-point routine, which also makes
the port more faithful; it is part of the netplay-v2 milestone.

### Audio

With an audio device the mixer's block size follows the device queue
(`platform/audio.c`, `ChooseBlockSize`), and `pcmSamplesPerVBlank` -- a field
the game can read -- comes from the device rate. Every run here uses
`--no-audio` / `AUDIO=0`; a run with sound is not comparable to one without
(docs/DEBUG-TOOLING.md §7 measured it moving EWRAM at frame 63). For netplay the
mixer will have to be driven by frames, not the device.

## Known issues the harness tripped over

Not fixed here; the recordings were steered round them, and each is
reproducible from the note:

- LP64: Mega Titan's arm setup (`sub_080EF1A8`, via `CreateTitanArm1`) faults on
  arrival in room 738. Swapped out of `warp_sweep`.
- LP64: the attract demo faults at frame 6744 in `sub_08002374`
  (`TaskGetStructPtr(gUnk_02023354)` of a stale task). `attract` stops at 6600.
- wasm: a 36-room sweep with 160-frame dwell traps at frame 5781 in
  `sub_080788AC` -> `sub_08155128` (Kirby's effect sprite) whatever room is
  current; an earlier variant with walking input trapped at 2924 the same way,
  with the bomb ability. The native build runs through. `warp_sweep` is sized
  to avoid it.

## Using it

- **Renderer work:** `make regress` must stay green. `make regress-pixels`
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
