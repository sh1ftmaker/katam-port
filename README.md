# katam-port

A port of **Kirby & The Amazing Mirror**, built on top of the
[jiangzhengwenjz/katam](https://github.com/jiangzhengwenjz/katam)
decompilation. It runs in a browser, and it runs as a native desktop program.

It boots, plays its music, gets you through the menus into a save file, and
plays a level — backgrounds, sprites, parallax, scrolling and sound, driven by
the game's own code the whole way.

**Live at https://sh1ftmaker.github.io/katam-port/**
(also https://katam-port.pages.dev) — bring your own ROM.

**This repository contains no game data.** You supply your own ROM. A local
file is read in your browser and never uploaded; a URL is fetched by your
browser directly from wherever you pointed it, not routed through the page or
any server of ours.

---

## What works

| | |
|---|---|
| Boots and runs the game's own `AgbMain` / `GameLoop`, unmodified | yes |
| Backgrounds, palettes, blending, windows, per-scanline effects | yes |
| Sprites | yes |
| Level tilemaps, parallax and scrolling | yes |
| Title screen, file select, starting a file, playing a level | yes |
| **Music and sound effects** | yes |
| Keyboard input | yes |
| Touch controls, built for phones | yes |
| Saving between sessions | yes, in browser storage |
| Native desktop build (SDL2) | yes, on Linux |

Nothing the port reaches is missing a body any more. A full run — boot, title,
file select, Start Game, a hundred seconds of gameplay — reports not one call
into an undecompiled function. Of the 13 symbols still stubbed at link time,
**none are game logic**: they are the link-cable driver, multiboot, and
`IntrMain`, all deliberately out of scope.

### Known problems

- **The warp star at the end of a level does not launch.** Kirby boards it and
  the sequence stalls. Under investigation; the shipped build traces the star's
  state machine to the browser console.
- **Some minigame sound effects do not play.**
- **The 4 PSG channels are not implemented.** The sequencer keeps their
  registers current, but nothing turns that into sound yet, so the chip
  channels are missing from the mix.
- **120 Hz displays run at double speed** *in the browser build.* The frame
  loop takes one game frame per `requestAnimationFrame`. Audio makes this
  obvious. The native build paces to the GBA's own 59.7275 Hz and does not
  have this problem.

## Running it

You need [emscripten](https://emscripten.org/) and a checkout of the
decompilation.

```sh
git clone https://github.com/jiangzhengwenjz/katam.git ~/katam
make KATAM_DECOMP=~/katam sync    # copy and adapt the decomp sources
make KATAM_DECOMP=~/katam stubs   # work out what is still ARM-only
make KATAM_DECOMP=~/katam         # build web/katam.{html,js,wasm}
make serve                        # http://localhost:8000/katam.html
```

Then open the page and hand it your ROM — a local file, drag-and-drop, or a
URL:

    http://localhost:8000/katam.html?rom=https://example.invalid/your-copy.gba

The URL form fetches in the browser, so the host has to allow cross-origin
reads (`Access-Control-Allow-Origin`); the page says so plainly when it does
not. `#rom=` works too, and keeps the address out of Referer headers. The last
ROM you loaded is remembered in IndexedDB so you do not have to supply it
again.

### Saves

Save files persist in the browser. The game's save memory is written to
IndexedDB whenever it changes -- debounced, with a periodic re-check and a
`visibilitychange` backstop, because mobile browsers kill tabs without warning
and `beforeunload` is not reliable there -- and restored the first time the
game reaches into it.

Storage is keyed per ROM, by the cartridge header plus a hash of the image, so
loading a different copy cannot inherit or clobber another game's save. The
page can export your save as a plain 64 KiB `.sav` (the format an emulator
reads), import one back, and delete what it has stored.

`sync` copies the decompilation and rewrites what will not compile off ARM; it
never touches your checkout. Re-run all three whenever you pull new
decompilation work — the set of stubbed functions shrinks by itself as the
decompilation advances.

## The native build

The same port in a window, on SDL2. Same PPU, same DMA, same mixer, same game
sources — what changes is the host.

```sh
sudo apt install gcc-multilib libsdl2-dev:i386   # or your distro's equivalent
make sync KATAM_DECOMP=~/katam                   # if you have not already
make stubs KATAM_DECOMP=~/katam
make native
./build/native/katam ~/roms/your-copy.gba
```

Same controls as the page, plus a gamepad. F11 fullscreen, 1–6 window scale,
F12 screenshot, Ctrl+Q quit. No ROM argument and it offers a file picker, or
you can drop one on the window. Saves go to a plain 64 KiB `.sav` in your
config directory — the format an emulator reads, and the same one the page
exports — keyed per ROM exactly as browser storage is.

It paces to the GBA's real 59.7275 Hz rather than to your monitor, so it does
not have the browser build's double-speed problem on a 120 Hz panel.

Two things are worth knowing before you build it elsewhere:

- **It is a 32-bit program, and has to be.** 111 of the decompilation's
  structures contain a pointer, and those structures are read out of the ROM
  and placed at addresses the linker script chose. Built 64-bit, `struct
  ToneData` grows from 12 bytes to 24 and every instrument in the sound bank is
  read from the wrong offset. The build refuses to configure with 8-byte
  pointers rather than produce something that boots and is quietly wrong.
- **macOS is blocked by that**, because it has had no 32-bit userland since
  Catalina. Windows and 32-bit ARM are fine.

`make native-test ROM=...` boots it headless with a real ROM, drives it through
the menus into a level, and checks that what came out is a picture.

[docs/NATIVE.md](docs/NATIVE.md) is the full account: how the GBA memory map is
reserved at its true addresses in a process, how that is verified rather than
assumed, and exactly what a new platform has to implement — five functions.

### Controls

| | |
|---|---|
| D-pad | arrow keys, or WASD |
| A | J or Z |
| B | K or X |
| L / R | Q / E |
| Start | Enter |
| Select | Backspace or right shift |

On a phone the page switches to on-screen controls: thumb clusters in
landscape, a reserved control band in portrait, safe-area aware, with the page
chrome tucked into a bottom sheet so nothing sits under your thumbs.

## How it works, in one paragraph

The GBA's memory map is reserved *inside* the WebAssembly linear memory, so
EWRAM really is at `0x02000000`, VRAM at `0x06000000`, and the ROM at
`0x08000000`. That one decision means the game's code needs almost no
changes: `*(vu16 *)0x0600E002 = 0xF1B0;` is a valid store, `EWRAM_START + (off
<< 2)` addresses the object the game's own allocator put there, and a pointer
read out of the ROM still points at the right place. A software PPU then reads
that memory exactly as the hardware would and produces the picture. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Sound

The decompilation has no C for the half of the sound driver that actually
plays anything — `asm/m4a_asm.s` holds 44 symbols including the sequencer, not
just the mixer. `platform/m4a_mixer.c` is that file rewritten in C, from the
game's own assembly. A C reimplementation of the same library exists elsewhere,
but nothing in its chain carries a licence, so none of it is used here.

Output goes through a hand-written `AudioWorklet` fed one block per VBlank.
Emscripten's own audio worklet support needs `SharedArrayBuffer`, which needs
cross-origin isolation headers that GitHub Pages does not send.

### What the port has to fix, and why

Three classes of thing are free on ARM and fatal in WebAssembly. They are
invisible from the decompilation's side, which is what makes them expensive to
find; [docs/DECOMP-REQUESTS.md](docs/DECOMP-REQUESTS.md) is the running list
sent back upstream.

- **Function pointers stored in ROM** are ARM code addresses, and a wasm
  function pointer is a table index — calling one ends the program. The build
  resolves each address back to a real function through the GBA build's link
  map: 304 of 305 flat table entries, plus 193 of the 219 constructors held
  inside ROM structs.
- **Function pointers mis-cast in the C source** — handing a no-argument
  function to a slot that calls it with one. ARM does not care; wasm
  type-checks every indirect call. `tools/check_fnptrs.py` finds these
  statically.
- **Reads through a pointer that is not valid yet**, where the game forms an
  address from an uninitialised field, dereferences it and discards the
  result. The console returns open bus; wasm traps.

There is a fourth that only bites a software port: a transfer written by
poking the DMA registers directly instead of going through the `DmaSet` macro.
The port emulates DMA in software, so a raw store to `0x040000D4` moves
nothing. There is exactly one in the game, and until it was found no level
tilemap had ever been written.

## Debugging tools

Built because each one was needed, and kept because the next bug will need
them:

- `tools/headless_test.js` boots the port under node with a real ROM and no
  browser. `MASH=`/`HOLD=` drive input, `LAYERS=`/`FORCE=` render one
  background at a time, `VRAM_AT=` dumps VRAM and palettes, `WATCH=` names
  every block move that touches an address, `AUDIO_RATE=`/`WAV=` capture the
  sound to a file, and `BG=`/`QUEUE=`/`LIVE=` report hardware state as it runs.
- `tools/native_smoke.sh` is the same idea for the native build: no window, no
  audio device, scripted input, a PNG of what it drew. It is what a new
  platform should be measured against.
- `tools/resolve_fnptr.py` turns a raw function-pointer value, or the index in
  a wasm stack trace, into a name and a signature.
- `make debug` builds with DWARF into a separate object tree, so
  `llvm-symbolizer` turns a trap into `file:line:column`.
- `make safe` bounds-checks every load and store.
- See [docs/DEBUG-TOOLING.md](docs/DEBUG-TOOLING.md) for what works here and
  what does not — AddressSanitizer, for instance, cannot link at all against a
  custom `GLOBAL_BASE`.

## Relationship to the decompilation

This repository does not vendor the decompilation. It copies it at build time
and applies a codemod (`tools/portify.py`) for the constructs that exist only
to make `agbcc` emit byte-identical ARM code — register pins, inline-asm
barriers, and the handful of functions whose "body" is included reference
assembly. The decompilation keeps its own build byte-exact; nothing here asks
it to change.

Where the decomp has grown a `PORTABLE` guard for those constructs, the port
uses it rather than rewriting anything.

## Deploying

`make dist` assembles the build outputs plus cache headers into `build/dist`,
and `make check-dist` refuses to publish anything ROM-shaped. `make deploy`
publishes to Cloudflare Pages; `scripts/publish-pages.sh` publishes to GitHub
Pages. What ships is the page, the loader and the wasm — there is no ROM in the
bundle and no way for the site to supply one.

## Legal

You need your own copy of the game; none is distributed here, and the page
cannot supply one.

To be precise about what the build does contain: the decompilation converts
some of the game's graphics into files it commits to its own public
repository, and `INCBIN` pastes 931 KiB of those into the wasm at build time,
exactly as the GBA build does. That is the only game-derived content in the
output. Everything else — level data, maps, music, the rest of the graphics —
is read from the ROM you supply, at run time.

Kirby & The Amazing Mirror is © 2004 HAL Laboratory, Inc. / Nintendo.
