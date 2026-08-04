# katam-port

A WebAssembly port of **Kirby & The Amazing Mirror**, built on top of the
[jiangzhengwenjz/katam](https://github.com/jiangzhengwenjz/katam)
decompilation.

It boots, draws the title screen, navigates the menus with sprites, and loads
a level. It is not playable yet — it dies partway into gameplay — but the game
is running its own code the whole way.

**Live at https://katam-port.pages.dev** — bring your own ROM.

**This repository contains no game data.** You supply your own ROM. A local
file is read in your browser and never uploaded; a URL is fetched by your
browser directly from wherever you pointed it, not routed through the page or
any server of ours.

---

## What actually works

| | |
|---|---|
| Boots and runs the game's own `AgbMain` / `GameLoop`, unmodified | yes |
| Backgrounds, palettes, blending, windows, per-scanline effects | yes |
| Sprites | yes |
| Keyboard input (WASD/arrows and friends) | yes |
| Title screen, file-select menu, loading into a level | yes |
| **Surviving gameplay** | **no** — see [docs/STATUS.md](docs/STATUS.md) |
| Sound | no, deliberately |
| Saving between sessions | no, not yet |

What breaks it now is function pointers stored in ROM. A GBA game dispatches
through tables of them, and a ROM function pointer is an ARM code address — in
WebAssembly a function pointer is a table index, so calling one ends the
program. The build resolves most of them back to real functions by looking each
address up in the GBA build's link map (312 of 314 table entries, plus 193 of
the 219 constructors held inside `gUnk_08351648`), but the shapes it does not
cover yet are what stops the game partway into a level.

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

Then open the page and hand it your ROM — a local file, or a URL:

    http://localhost:8000/katam.html?rom=https://example.invalid/your-copy.gba

The URL form fetches in the browser, so the host has to allow cross-origin
reads (`Access-Control-Allow-Origin`); the page says so plainly when it does
not. `#rom=` works too, and keeps the address out of Referer headers.

`sync` copies the decompilation and rewrites what will not compile off ARM; it
never touches your checkout. Re-run all three whenever you pull new
decompilation work — the set of stubbed functions shrinks by itself as the
decompilation advances.

### Controls

| | |
|---|---|
| D-pad | arrow keys, or WASD |
| A | J or Z |
| B | K or X |
| L / R | Q / E |
| Start | Enter |
| Select | Backspace or right shift |

## How it works, in one paragraph

The GBA's memory map is reserved *inside* the WebAssembly linear memory, so
EWRAM really is at `0x02000000`, VRAM at `0x06000000`, and the ROM at
`0x08000000`. That one decision means the game's code needs almost no
changes: `*(vu16 *)0x0600E002 = 0xF1B0;` is a valid store, `EWRAM_START + (off
<< 2)` addresses the object the game's own allocator put there, and a pointer
read out of the ROM still points at the right place. A software PPU then reads
that memory exactly as the hardware would and produces the picture. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

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

`make dist` assembles the three build outputs plus cache headers into
`build/dist`. `make deploy` publishes that to Cloudflare Pages. What ships is
the page, the loader and the wasm — there is no ROM in the bundle and no way
for the site to supply one.

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
