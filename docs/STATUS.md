# Status

Measured 2026-08-04 against the decompilation at `all-work`, not estimated.

## Where it gets to

Verified with `make test`, which boots the port under node with a real ROM and
runs frames headlessly:

| Frame | What happens |
|---|---|
| 0 | `AgbMain` starts; RAM cleared, ROM mapped |
| ~60 | first VRAM uploads; `DISPCNT` goes to mode 0 with all four backgrounds and objects enabled |
| ~400 | title logo drawn |
| ~1200 | full title screen — sky, rainbow, logo, copyright line |
| 1250 | Start pressed |
| ~1900 | file-select menu, "CHOOSE A FILE TO PLAY" |

No crash across 2000 frames. The game's own `gFrameCount` advances in step, and
`TasksExec` runs every frame.

## What is missing, in the order it matters

### 1. Sprites — `sub_08155128`

**Nothing that is not a background appears.** Kirby, enemies, menu cursors, the
file boxes on the select screen: none of them draw.

Everything in the game reaches OAM through `sub_08155128` — 506 call sites, the
single most-referenced function in the codebase. It is still ARM assembly
(`asm/sprite.s`, ~1,950 instructions) and has no C body, so the port stubs it
and OAM is never populated. The PPU's sprite renderer is written and waiting.

`asm/sprite.s` is claimed by another contributor upstream. Two ways forward:
wait for that PR, or write a port-local C reimplementation, which does not have
to match byte-for-byte and so is a much smaller job than the decompilation of
it.

Also stubbed from the same file: `sub_0815436C`, `sub_081548A8`,
`sub_08154B14`, `sub_08154FE8`, `sub_0815521C`, `sub_08155604`.

### 2. The last bodyless functions

12 functions in `wip/code_08032E98.c` still have no C body at all (down from 31
earlier the same day — this is moving fast). Three of them are reachable and
stubbed here: `sub_08035788`, `sub_08037314`, `sub_08038B34`.

### 3. Sound

Deliberately absent. `asm/m4a_asm.s` is 42 hand-written ARM functions that were
never a decompilation target; the port answers the whole m4a API with no-ops.
Restoring audio means either reimplementing MP2K in C or routing the sequencer
at a host backend — a self-contained project.

### 4. Saving

`platform/sram.c` gives the game working save memory, but it lives in the wasm
heap and disappears when the tab closes. Persisting it to `localStorage` or
IndexedDB is straightforward and not yet done.

### 5. Link cable

`MultiSio*`, `MultiBoot*` and `sio32_multi_load` are stubbed. Multiplayer and
the download-play subgames will not work.

## The full stub list

28 symbols, regenerated on every `make stubs`:

```
gAreaMapRoomsTilemapOffsets  gAreaMapRoomsTilemaps  gHBlankIntrs  gSongTable
gUnk_03003678  IntrMain  MPlayStart  MPlayStop  MultiBootCheckComplete
MultiBootInit  MultiBootMain  MultiBootStartMaster  MultiSioIntr
MultiSioRecvBufChange  RomHeaderGameCode  RomHeaderMagic  sub_08035788
sub_08037314  sub_08038B34  sub_080B9048  sub_080BA5A4  sub_0815436C
sub_081548A8  sub_08154B14  sub_08154FE8  sub_08155128  sub_0815521C
sub_08155604
```

Each one reports itself the first time it is called, in the page's log.

## Build facts

| | |
|---|---|
| Game sources compiled | 189 of 189 |
| Codemod: register pins / asm barriers rewritten | 2 (the decomp's own `PORTABLE` guard handles the other 392) |
| Codemod: asm-wrapper functions stubbed | 19 |
| Codemod: INCBIN assets pasted in from the decomp | 143 files, 931 KiB |
| Files replaced by the platform layer | 4 (`m4a.c`, `m4a_tables.c`, `multi_boot.c`, `agb_sram.c`) |
| Linker-placed RAM symbols reconstructed | 183 |
| ROM data symbols resolved to addresses | 162 |
| ROM function-table entries wired to real C | 284 of 310 |
| Output | 2.0 MB wasm |

## Two bugs worth remembering

Both cost real time and neither looks like what it is.

**A stub returning 0 hung the game silently.** `main.c` drains the VRAM
transfer queue through a table of four workers, and reads `0` as "this one did
not finish" — it then stops calling `TasksExec()` until the queue drains. A
stub that always returns 0 says "never finished", so the game ran its VBlank
handler forever, drew nothing, and looked for all the world like a rendering
bug. `tools/stub_returns.conf` records the cases where 0 is the wrong answer.

**`inline` means the opposite thing in C99.** agbcc is gcc 2.95, where a plain
`inline` definition also emits an external one; C99 inverted that. Without
`-fgnu89-inline` every `inline void Foo(...)` in the game becomes an undefined
symbol, and the stub generator will obligingly stub a function whose body is
right there.
