# Status

Measured 2026-08-04 against the decompilation at `all-work`, not estimated.

## Where it gets to

Verified with `make test`, which boots the port under node with a real ROM and
runs frames headlessly, pressing buttons on a script:

| Frame | What happens |
|---|---|
| ~60 | first VRAM uploads; `DISPCNT` goes to mode 0, four backgrounds and objects on |
| ~1200 | title screen — sky, rainbow, logo, copyright line |
| ~1900 | **"PRESS START"** appears (sprites) |
| ~3000 | file-select menu: entries, cursor, four Kirbys, items — all sprites |
| ~3500+ | a level loads: `CreateLevelObjects` spawns objects, Kirby's state machine runs |

Sprites arrived when the decompilation landed `sub_08155128`. Before that the
port drew backgrounds only, and the menus were empty frames.

It does not survive long in gameplay yet — see below.

## What is missing, in the order it matters

### 1. Function pointers stored in ROM

This is now the main thing standing between the port and playing.

A function pointer in ROM is an ARM code address. In WebAssembly a function
pointer is an index into the module's function table, so such a value is not
merely wrong, it is out of range — calling one ends the program. The build
resolves these back to real functions by looking each address up in the GBA
build's `katam.map`, and covers three shapes:

| Shape | Example | State |
|---|---|---|
| flat function-pointer arrays | `gSpawnFuncTable1` | 312 of 314 entries wired across 8 tables |
| arrays declared via a function-pointer typedef | `gUnk_082EB7D0` | covered |
| function pointers *inside* ROM structs | `gUnk_08351648`, 219 object descriptors with a constructor at +0x10 | patched in place at startup, 193 resolve |

Shapes it does **not** cover yet still crash the port partway into gameplay.
The current failure is in `kirbyFlyUp`. Each one is individually fixable by the
same mechanism; the general fix is upstream, when ROM tables become typed C
data.

A related trap, worth knowing about: a label in `data/*.s` marks where the
decompilation assigned a name, not where a table ends. `gUnk_0834BD88` is
labelled as 3 entries and the game indexes it far past that — on hardware the
reads continue into the next label's words, which are more function pointers.
The generator now walks each table forward while the following words still
resolve to known functions, which turns that table into its real 12 entries.

### 2. The last two bodyless functions

`sub_08038010` and `sub_08038B34` are the only functions in the game with no C
body at all, and the port reaches both. `sub_08037314`, which calls
`sub_08038010` twice, is also documented upstream as deliberately incomplete —
five of its six collision blocks are unwritten — so the port is running with
partial collision resolution and treats it as a known-incomplete site.

### 3. The rest of `sprite.s`

Still stubbed: `sub_0815436C`, `sub_081548A8`, `sub_08154B14`, `sub_08155604`.

### 4. Sound

Deliberately absent. `asm/m4a_asm.s` is 42 hand-written ARM functions that were
never a decompilation target; the port answers the whole m4a API with no-ops.

### 5. Saving

`platform/sram.c` gives the game working save memory, but it lives in the wasm
heap and disappears when the tab closes. Persisting to IndexedDB is
straightforward and not yet done.

### 6. Link cable

`MultiSio*`, `MultiBoot*` and `sio32_multi_load` are stubbed.

## Build facts

| | |
|---|---|
| Game sources compiled | 190 |
| Stubbed symbols | 25 |
| Linker-placed RAM symbols reconstructed | 183 |
| ROM data symbols resolved to addresses | 162 |
| ROM function-table entries wired to real C | 312 of 314 |
| Function pointers patched inside ROM structs | 193 of 219 |
| INCBIN assets pasted in from the decomp | 143 files, 931 KiB |
| Output | 2.3 MB wasm |

## Bugs worth remembering

Five of these cost real time, and none looked like what it was.

**The GBA's compiler rounds a structure's size up to a multiple of 4; clang
does not.** gcc for ARM defaults to `-mstructure-size-boundary=32`, so
`struct Unk_08353510` -- four `s16` and two `u8` -- is 12 bytes on the console
and 10 in this port. Nothing notices until something walks an array of one.
That structure is the animation script the warp star hands Kirby at the end of
a level: `++kirby->unk114` stepped 10 bytes through a table with a 12-byte
stride, the per-entry frame counter picked up half of the next entry, went
negative, and `if (!--counter)` could never fire. The ride animation therefore
never ended, and the star's last state sat waiting on `animationIndex != 90` --
a condition that had already been made unreachable. No crash, no freeze, no
diagnostic; the level simply never changed.

Two things about it are worth keeping. First, `platform/port/gba_layout.h` did
not catch it and could not have: that header is *generated from the tree*, so
it had recorded `sizeof(struct Unk_08353510) == 10` and was defending the bug.
It catches drift from today's tree, which is worth having, but not a
reconstruction that was never right. `make size-check`
(`tools/check_doc_sizes.py`) is the other direction -- the tree against the
size the decompilation documents on each closing brace, which is console truth.
It found six such types. Second, the confirmation was free once the sizes were
fixed: the decomp names members after their offsets, and in
`struct Unk_0812DBB4` the member following `unk0[4]` is `unk20`, so that array
has to be 32 bytes -- 8 per element, not 6. Every moved offset landed exactly
on its own name.

**A stub returning 0 hung the game silently.** `main.c` drains the VRAM
transfer queue through four workers and reads `0` as "did not finish", then
stops running game tasks until the queue drains. A stub returning 0 says
"never finished", so the game ran its VBlank handler forever and drew nothing.
`tools/stub_returns.conf` records where 0 is the wrong answer.

**`inline` means the opposite thing in C99.** agbcc is gcc 2.95, where a plain
`inline` definition also emits an external one. Without `-fgnu89-inline` every
`inline` function in the game becomes an undefined symbol.

**VBlank is a duration, not an instant.** The game keeps working after
`VBlankIntrWait` returns, and its queue workers check `REG_DISPSTAT & VBLANK`
before each item — so returning with the flag already cleared meant the queue
never drained and no task ever ran again. The flag now stays set and is spent
by the transfers themselves (`PortVBlankConsume`), which is the same budget the
hardware imposes.

**An address-taken undefined function does not fail the link.** `wasm-ld`
resolves it to a null table entry, so the program dies at the dispatch with no
build-time warning. This happened because `src/flamer.c` and `wip/flamer.c`
define *different* halves of the same file and the build was keeping only one;
`nullsub_125` ended up declared, placed in a table, and never defined. The two
copies are now merged into one file, and `tools/refresh_stubs.sh` compares
`llvm-nm`'s undefined and defined sets rather than trusting the linker to
complain.
