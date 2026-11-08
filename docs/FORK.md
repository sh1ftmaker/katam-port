# The fork

Until the fork, the port did not contain the game. Every build began with
`make sync`: `tools/portify.py` copied a checkout of the decompilation
([jiangzhengwenjz/katam](https://github.com/jiangzhengwenjz/katam)) into
`build/port-src`, rewrote what would not compile off ARM with a few hundred
regex transforms, and two generators read the GBA build's `katam.map` and
`katam.elf` to produce address tables. That pipeline is retired. The port no
longer aims at GBA fidelity -- a modern renderer and game-logic changes are
coming -- and a codemod that has to keep re-deriving the source from upstream
is the wrong tool for a codebase that will diverge from it on purpose.

**The game source now lives in this repository, in `game/`, and is edited
directly.**

## Layout

    game/src/          the game: the decompilation's src/, as portify.py
                       last rewrote it (202 files)
    game/include/      its headers (179)
    game/generated/    the address tables the sync derived from katam.map,
                       katam.elf and a link, frozen:
        port/ram_symbols.h    RAM globals at their linker.ld addresses
        port/rom_data.h       ROM data labels as address macros
        rom_copies.c          ROM arrays that need real storage
        rom_fn_tables.c       ROM function-pointer tables, rebuilt as C
        stubs.c               the six functions with no C body
    platform/          unchanged: the console (PPU, DMA, BIOS, mixer) and the
                       two hosts
    tools/legacy-sync/ the retired pipeline, off the build path

Why `game/` and not `src/` at the top: the repository already has three
peers -- `platform/` (the emulated console and the hosts), `web/` and
`netplay/` -- and the game is a fourth, not the whole. Keeping
`game/src` + `game/include` the same shape as the decompilation keeps every
`#include "foo.h"` and every path in the docs' line references meaningful, and
the include line is the one the build always had (`-Igame/include
-Igame/generated -Iplatform`). `generated/` sits inside `game/` because its
headers are force-included into every game translation unit and describe the
game's own symbols; they are no longer generated, but the name says where they
came from and that hand-editing them is now normal.

## Base

| | |
|---|---|
| upstream decompilation | `1197474` "Match sub_08153184" |
| plus the port's series | `tools/legacy-sync/patches-decomp/0001..0006` (below) |
| decomp branch synced  | `port-patches` at `3f92d3a` |
| port before the fork  | `b59e25e` (main) |
| sync output check     | a fresh `make sync` of `3f92d3a` was byte-identical to the tree committed |

The patch series, which upstream does not have:

1. `0001` Decompile `code_0802B4A8.c` (105 functions) -- the post-Dark Mind cutscene.
2. `0002` Decompile the rest of `flamer`.
3. `0003` Decompile the rest of `prank` (43 functions).
4. `0004` Decompile `code_0802E57C.c` (26 of 28 matching; two `NAKED` + `NONMATCHING`).
5. `0005` Follow upstream's rename `gUnk_03002558` -> `gMultiSioEnabled` in it.
6. `0006` Bring `shotzo.c`, `code_0806F734.c`, `sub_0800F044.c` over from the old
   all-work branch, ported to upstream's names.

All six built `make compare: OK` against the ROM.

## What was frozen, moved, or removed

- **Frozen** (`game/generated/`): everything the build used to read out of
  `katam.map`, `katam.elf`, `linker.ld` and a trial link. They contain
  addresses, sizes and declarations -- no ROM bytes; ROM data is read from the
  player's ROM at run time, as before.
- **Taken out of the tree before it was committed**: portify.py expanded every
  `INCBIN_U8/16/32("graphics/...")` into a C initialiser holding the asset's
  bytes, read from the decomp checkout -- 143 arrays, 931 KiB of graphics that
  the decomp extracts from a ROM. Harmless in `build/`, not committable.
  `tools/legacy-sync/unbake_incbin.py` replaced each with a macro naming the
  same bytes in the player's ROM, keeping the array type (so `sizeof` and
  `ARRAY_COUNT` still work):

      #define sFoo (*(const u32 (*)[0x1c0])0x08ABCDEF) /* INCBIN_U32("..."), 1792 bytes, read from the ROM */

  Statics stay in their file; the 34 globals moved to `port/rom_data.h` and
  their `extern`s were commented out the way `gen_rom_data.py` always did it.
  Every address came from `katam.elf` and was checked byte-for-byte against
  the ROM before it was written. What remains in `game/` is source text that
  upstream itself tracks (including its C palette/tilemap tables); a check of
  every numeric-heavy line against upstream's tracked files found one
  reformatted line and nothing else.
- **Moved to `tools/legacy-sync/`**: `portify.py`, `cxxify.py`, `narrow32.py`,
  `gen_ram_symbols.py`, `gen_rom_data.py`, `gen_stubs.py`, `refresh_stubs.sh`,
  `stub_returns.conf`, the decomp patch series (`patches-decomp/`) and the last
  `PORTIFY_REPORT.txt`. Nothing on the build path imports them. Comments in
  `platform/` that say "tools/legacy-sync/cxxify.py does X" describe how the
  code in `game/` got its shape.
- **Retired make targets**: `sync`, `stubs` (they now say so and fail).
- **Kept**, because they work from the tree alone: `layout`/`layout-check`
  (needs a 32-bit gcc, not a decomp), `relayout`/`relayout-check`,
  `size-check`, `ptr-array-check`, `abi-size-check`, `p32-test`,
  `shell-check`. Their default tree is now `game/`.

Building: `make` (web), `make build/katam-node.js` / `make test` (node),
`make native` (ILP32), the LP64 CMake build (`cmake -S . -B build/lp64
-DKATAM_ALLOW_LP64=ON`), `make arm64`, `make windows` -- all of them compile
`game/` through the same two variables (`PORT_SRC`, `GENERATED`) and none needs
anything outside the repository.

## What is allowed now

Anything. `game/` is the port's code:

- **Non-matching code is fine.** Nothing compares against the ROM's machine
  code any more. `NONMATCHING` branches, `NAKED` stubs and `// TODO(match)`
  comments are history; rewrite freely.
- **Non-GBA code is fine.** C++ features the LP64 build already compiles under,
  host calls, larger structures, new fields -- with two constraints that are
  about the *port*, not about the GBA:
  - Structures the ROM's data is read through (anything in
    `platform/port/gba_layout.h`, anything placed at a fixed address by
    `ram_symbols.h`) still have to keep their console layout while the game
    reads ROM data through them. `make layout-check` and the compiled
    `_Static_assert`s say when one moves. Change a layout on purpose by
    re-running `make layout` and committing the diff.
  - The simulation has to stay deterministic and identical across hosts --
    netplay depends on it. `make regress` / `make regress-wasm`
    ([DETERMINISM.md](DETERMINISM.md)) are the check.
- Mark changes made for the port in `game/` with `PORT-FORK:` in a comment when
  the reason is not obvious from the code; `PORT:` marks are portify's.
- `game/generated/` is ordinary source now. `stubs.c` holds the six functions
  that had no C; give them bodies here.

## Bringing an upstream change across by hand

Upstream keeps decompiling and renaming. Nothing pulls that in automatically
any more; when a change is worth having:

1. **Build both upstream revisions** -- the one the fork is based on
   (`1197474` + the series) and the new one -- with `make compare` passing,
   so both produce byte-identical ROMs and `katam.elf`s.
2. **Build the rename map from the two ELF symbol tables, keyed by address.**
   Because both builds produce the same ROM, every function and data label is
   at the same address in both; only names (and types) change. `nm` each
   `katam.elf` (it has file-local symbols too; `katam.map` does not), join on
   address, and every address whose name differs is an old -> new rename.
   This is the method the last sync used to carry `0006` from old names to
   upstream's (it found `struct Object` -> `ObjectTemplate`, `Object2` ->
   `Object`, `Object4` -> `EffectObject` and the field renames that way, then
   confirmed with `make compare`). Struct and field renames do not appear in
   `nm`; diff the headers for those, guided by the function renames.
3. **Apply the map to `game/`** with whole-word replacement, then build. The
   compiler finds what the map missed.
4. **For new C (functions upstream decompiled since)**, diff upstream
   `src/foo.c` between the two revisions and port the new bodies by hand. What
   portify used to do to such code, and what you now do yourself:
   - remove register pins and `asm` barriers (`PORTABLE` covers most);
   - turn `INCBIN` into a ROM-address macro as above -- never paste bytes;
   - a pointer member of a structure the ROM is read through must be
     `PTR32(T)` / `PTR32_FN(T)` (`platform/port/p32.h`) or the LP64 build
     reads it at the wrong width (`docs/SIXTYFOUR.md`);
   - a function pointer that ends up in a ROM table needs an entry in
     `game/generated/rom_fn_tables.c`;
   - a new RAM global at a fixed address goes in `port/ram_symbols.h`, a new
     ROM data label in `port/rom_data.h`.
5. **Prove it with the harness.** A pure rename or a newly-C function that
   replaces a stub must leave `make regress` green; a real behaviour fix will
   move it, and the first divergent frame says where.

The old pipeline still runs, if a full re-derivation is ever wanted as a
reference to diff against: `tools/legacy-sync/` with the commands from the
pre-fork Makefile (`git show b59e25e:Makefile`). Its output goes to `build/`
and must not be committed as it comes out -- run `unbake_incbin.py` over it
first.
