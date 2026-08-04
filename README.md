# katam-port

A WebAssembly port of **Kirby & The Amazing Mirror**, built on top of the
[jiangzhengwenjz/katam](https://github.com/jiangzhengwenjz/katam)
decompilation.

It boots. As of the first commit it runs the intro, draws the title screen,
and reaches the file-select menu under keyboard control.

**This repository contains no game data.** You supply your own ROM, which is
read locally in your browser and never uploaded. Nothing about the game is
distributed here — only code that was already open source, plus the platform
layer that lets it run somewhere other than a Game Boy Advance.

---

## What actually works

| | |
|---|---|
| Boots and runs the game's own `AgbMain` / `GameLoop`, unmodified | yes |
| Backgrounds, palettes, blending, windows, per-scanline effects | yes |
| Keyboard input (WASD/arrows and friends) | yes |
| Reaches the title screen and the file-select menu | yes |
| **Sprites** | **no** — see [docs/STATUS.md](docs/STATUS.md) |
| Sound | no, deliberately |
| Saving between sessions | no, not yet |

Sprites are the one thing standing between this and something you can play.
Every object in the game is drawn through `sub_08155128`, which is still ARM
assembly in the decompilation — 506 call sites, no C body. Until it has one,
OAM is never populated and nothing but the backgrounds appears. That is a
decompilation task, not a porting one, and it is being worked on upstream.

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

Then open the page and hand it your ROM.

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

## Legal

The decompilation, and this port, contain no Nintendo code or assets. You need
your own copy of the game. Kirby & The Amazing Mirror is © 2004 HAL
Laboratory, Inc. / Nintendo.
