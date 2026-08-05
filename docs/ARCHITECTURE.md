# Architecture

## The decision everything else follows from

A GBA game is not written against an API. It is written against addresses. The
KATAM decompilation is full of lines like

```c
*(vu16 *)0x0600E002 = 0xF1B0;                       /* a tilemap entry */
gDispCnt = DISPCNT_FORCED_BLANK;                    /* an I/O register  */
DmaFill32(3, 0, (void *)VRAM, VRAM_SIZE);
```

and, more awkwardly, of arithmetic on region bases:

```c
#define TaskGetStructPtr(taskp)                            \
    ((taskp)->flags & TASK_USE_EWRAM                       \
    ? (void *)EWRAM_START + ((taskp)->structOffset << 2)   \
    : (void *)IWRAM_START + (taskp)->structOffset)
```

The usual porting answer is a shim: route every `REG_*`, every `Dma*`, every
literal address through an accessor. For this game that means touching hundreds
of sites, and it still does not fix `TaskGetStructPtr` — the game's own
allocator hands out offsets from a region base, and any answer other than "the
region is really there" makes the game and its allocator disagree about where
objects live.

So the port does the other thing. **The GBA memory map is reserved inside the
WebAssembly linear memory, at its real addresses.**

```
0x02000000  EWRAM      256K
0x03000000  IWRAM       32K
0x04000000  I/O          1K
0x05000000  palette      1K
0x06000000  VRAM        96K
0x07000000  OAM          1K
0x08000000  ROM         16M   <- the player's own ROM, loaded by the page
0x0E000000  save         64K
...
0x0F000000  everything the compiler owns: static data, stack, heap
```

`-sGLOBAL_BASE=0x0F000000` is what keeps the two apart: emscripten places its
own data above the map, so nothing the toolchain does can land in the middle of
VRAM. Total reservation is 272 MB of *address space*; WebAssembly memory is
virtual, and only the pages actually touched are ever committed.

The payoff is that the game's code compiles as written. There is no shim for
`REG_*`, none for literal addresses, and none for region arithmetic.

## What still needs adapting, and why

Four things do not survive the move, and each is handled at the narrowest point
that works.

**DMA** (`platform/dma.c`). `DmaSet` writes a control register and expects
hardware to notice. Every other `Dma*` macro in the decomp is built on top of
it, so redirecting that one macro catches them all. Channels armed for HBlank
are driven from the PPU's scanline loop — that is how the game's per-scanline
scroll effects work.

**BIOS calls** (`platform/bios.c`). `asm/libagbsyscall.s` is 15 one-instruction
`swi` wrappers around documented routines. They are reimplemented directly:
LZ77 and run-length decompression, `CpuSet`/`CpuFastSet`, the affine helpers,
`Div`, `Sqrt`, `ArcTan2`. `VBlankIntrWait` is the interesting one — see below.

**Save memory** (`platform/sram.c`). `src/agb_sram.c` cannot be ported at all.
Cartridge SRAM has to be read by code running out of RAM, so Nintendo's library
copies the machine code of its own inner loop into a stack buffer and calls it.
In WebAssembly code is not addressable as data and a function pointer is a table
index, not an address, so that call goes straight out of bounds. It was the
first thing to crash the port after boot. The replacement copies bytes.

**Sound** (`platform/audio.c`). `asm/m4a_asm.s` is 42 hand-written ARM functions
that were never a decompilation target. The port answers the whole m4a API with
no-ops. This is a deliberate omission, not a gap waiting to be filled in
passing.

## The frame loop

`GameLoop` never returns and blocks once a frame in `VBlankIntrWait`. A browser
cannot be blocked, so the port is built with Asyncify: the call unwinds the
WebAssembly stack, hands control back to the page, and resumes on the next
animation frame. The game's loop is left exactly as written.

`VBlankIntrWait` does, in order, what the hardware would have:

1. render the visible frame from whatever the game last wrote to VRAM/OAM,
   scanline by scanline, running HBlank DMA and the HBlank/VCount handlers as
   each line finishes;
2. enter VBlank — run VBlank-timed DMA, then call the game's own VBlank handler
   out of `gIntrTable`;
3. present the frame and wait for `requestAnimationFrame`;
4. clear the VBlank flag, because `GameLoop` spins on
   `while (REG_DISPSTAT & DISPSTAT_VBLANK);` the moment it gets control back.

## The PPU

`platform/ppu.c` is a scanline renderer over the same memory the hardware would
have read: text and affine backgrounds, regular and affine sprites in both
mapping modes, all four priorities, both windows plus the object window, alpha
blending, brightness fade, and mosaic. It is per-scanline rather than per-frame
because the game arms DMA channel 0 to feed a register a fresh value every
HBlank, and a frame-at-a-time renderer would miss that entirely.

## Symbols that are not in any C file

Two generators reconstruct things the GBA build got from its linker.

`tools/gen_ram_symbols.py` — 189 of the game's globals are not defined
anywhere. They are addresses assigned by `linker.ld` (`. = 0x00020EE0; gKirbys
= .;`) and the C side only ever sees `extern struct Kirby gKirbys[];`.
`wasm-ld` has no `--defsym`, so each one becomes a macro over its address and
the matching `extern` is commented out of the copied headers. Array extents are
preserved, because `sizeof(gBgScrollRegs)` is used as a DMA length.

`tools/gen_rom_data.py` — `data/*.s` is 28,591 labels over `.incbin` blocks.
Only ~160 are ever named by C code; the rest are reached by following pointers
that already live inside the ROM image, which works because the ROM is mapped
where those pointers expect it. The named ones become address macros too.

## ROM function tables

This one is subtle and it is what stood between the title screen and the file
select menu.

The game dispatches through tables of function pointers that live in ROM —
`gSpawnFuncTable1` is 219 entries of object constructors, and a handful of
others cover animation commands and task behaviour. Those tables hold *ARM code
addresses*. In WebAssembly a function pointer is an index into the module's
function table, so an address like `0x0802FE84` is not merely wrong, it is out
of bounds: calling one takes the program down immediately.

So the generator rebuilds them. Each entry is looked up in the GBA build's
`katam.map`, turned back into the name of the function at that address, and
emitted as a real reference to the decompiled C function — provided the port
defines it. 284 of 310 entries currently resolve. The rest get a stub with the
correct signature, so a call reports itself instead of trapping.

This needs `katam.map` and a ROM at build time. Without them the tables are
still generated, entirely stubbed, and the game will not get far.

## Keeping up with a moving decompilation

The decompilation is being worked on continuously; functions gain C bodies while
the port is being built. Nothing here hard-codes what is missing.

`tools/refresh_stubs.sh` re-derives it from a real link every time: generate
stubs for what is believed missing, compile, drop anything the game now defines
for itself, link, and collect whatever is still undefined — repeating, because
stubbing one function makes more code reachable and can expose more gaps. A
shrinking count between runs is the decompilation making progress. The list is
never carried between runs; a stale entry that got dropped once would otherwise
stay missing forever.

It refuses to run after a failed compile, which sounds obvious and is not: a
source file that did not build looks exactly like a pile of missing functions,
and an earlier version of this script cheerfully stubbed half the game.

## Things that will bite later

- **Struct layout.** agbcc pads a 2-byte union to 4. Where the game reads ROM
  bytes through a struct, a layout difference is silent and produces garbage
  rather than a crash. `platform/port/gba_layout.h` is the answer to this: the
  size and every member offset of all 246 types the decompilation defines,
  committed and asserted at compile time in every build. A layout that moves is
  now a compile error naming the structure and the member, not a silent
  corruption. `make layout` regenerates it; [docs/SIXTYFOUR.md](SIXTYFOUR.md)
  says what it is for and what it still cannot see.
- **Function pointers inside ROM structs.** The table rebuild above handles
  arrays the headers declare. A function pointer stored in a struct that is read
  from ROM has no declaration to key off, and will trap the same way.
- **Three variables are genuinely uninitialised.** The decomp has three sites of
  the form `asm("" : "=r"(v))` — "this variable is whatever the register
  happened to hold". Under `PORTABLE` they expand to nothing. The codemod zeroes
  them, which is deterministic but not necessarily right.
