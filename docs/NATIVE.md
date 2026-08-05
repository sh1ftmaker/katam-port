# The native build

A desktop build of the same port: same PPU, same DMA, same mixer, same game
sources. What changes is the host — SDL2 instead of a browser page — and two
things the browser was hiding.

```sh
make sync  KATAM_DECOMP=~/katam
make stubs KATAM_DECOMP=~/katam
make native
./build/native/katam ~/roms/your-copy.gba
```

There is no ROM here and no way for the program to obtain one. Supply your
own, as the argument above, by dropping the file on the window, or through the
file picker it offers when started with no argument.

---

## The two things the browser was hiding

### 1. The build has to be 32-bit

This is the second decision the port rests on, after the memory map, and it
decides which platforms are reachable at all.

The game is a 32-bit ARM program and the decompilation is its source. **111 of
its structures have a pointer member**, and those structures are not private to
the C code — they are read out of the ROM, and they are placed at addresses
`linker.ld` chose:

```
                     GBA / wasm32        x86-64
struct ToneData         12 bytes          24 bytes     read from the ROM
  ->wav at offset        4                 8           every instrument, wrong
struct Kirby's head     12 bytes          16 bytes     array at 0x00020EE0
```

`struct ToneData` is the sound bank: built 64-bit, every instrument in the game
is read from the wrong offset. `struct Kirby` is an array the linker script
placed at a fixed address with a fixed extent: built 64-bit, it runs over its
neighbour, and the game's own allocator and the port disagree about where
objects live — which is exactly the failure that
[docs/ARCHITECTURE.md](ARCHITECTURE.md) says the whole address-map decision
exists to avoid.

The web build gets ILP32 free, because wasm32 is ILP32. A native build has to
ask for it. `CMakeLists.txt` refuses to configure with 8-byte pointers, because
there is no run-time symptom that would point back here: the game would boot,
render, and be subtly and permanently wrong.

On 64-bit x86 that means `-m32` and a 32-bit SDL:

```sh
sudo apt install gcc-multilib libsdl2-dev:i386     # Debian, Ubuntu
sudo dnf install glibc-devel.i686 libgcc.i686 SDL2-devel.i686   # Fedora
```

`make native` picks `cmake/toolchain-linux-i686.cmake` automatically on an
x86-64 host. A host that is already ILP32 — i686, armv7 — needs no toolchain
file at all.

**What this means for the three platforms that follow** is set out under
[Adding a platform](#adding-a-platform) below. The short version: Windows and
32-bit ARM are fine; **macOS is not**, and the reason is worth reading before
starting.

### 2. Page zero

`TaskCreate` does this, and has always done it:

```c
if (slow->next == gNextTask->prev)      /* gNextTask is null at boot */
```

On the GBA, address `0x00000002` is the BIOS ROM. The read returns open bus,
the comparison cannot match, and the branch is not taken. In WebAssembly
address 2 is ordinary low linear memory and reads as zero — so the port has
been running this every boot without noticing. A native process is the first
host that genuinely cannot do it: every operating system reserves page zero and
refuses to map anything there (Linux via `vm.mmap_min_addr`, 65536 by default;
macOS via `__PAGEZERO`; Windows reserves the first 64 KiB outright).

So the native build segfaulted in `CreateLogo`, four frames in, on the first
run. The fix is a guard in `NULL_DEREFS` in `tools/portify.py`, written to be
what the hardware does rather than what is tidier, and
[docs/DECOMP-REQUESTS.md](DECOMP-REQUESTS.md) asks upstream for it.

The same reasoning removes the BIOS from the DMA range check natively —
`PORT_BIOS_REGION_SIZE` in `platform/port/backend.h` is `0x4000` on the web and
`0` here. A transfer that names the BIOS is reported and skipped rather than
faulting, which is what the hardware's open bus amounts to. There is exactly
one such transfer during boot (`dest=0x00000000, count=96`); in the web build it
writes 192 bytes into wasm's unused low memory and nobody has ever seen it.

---

## Reserving the memory map

The port's whole premise is that the GBA's regions are at their real addresses:
EWRAM at `0x02000000`, VRAM at `0x06000000`, the ROM at `0x08000000`. In wasm
that is a linker flag (`-sGLOBAL_BASE=0x0A000000` puts everything the compiler
owns above the map). Natively the addresses are taken at run time, in
`PortNativeReserveMap` (`platform/native/mem.c`), before SDL has allocated a
byte or started a thread.

### Why nothing else wants those addresses

The executable is built **PIE**, so the loader places it at a randomised high
address and its `.data`, `.bss` and `brk` heap go with it. Large allocations
and shared libraries come from `mmap`, which the kernel hands out near the stack
and grows *downward*. The stack is at the top. None of them can reach
`0x02000000` from above without exhausting the address space first.

A non-PIE binary would load at `0x400000` — below EWRAM, and therefore harmless
in itself — but its `brk` heap would then grow upward straight into it.
`mem.c` checks its own load address at startup and says so if this went wrong.
`CMakeLists.txt` sets `POSITION_INDEPENDENT_CODE`; on a platform where that is
not the default, it is not a hardening preference, it is the reason the
reservation works.

### Why an argument is not evidence

The reservation is made with `MAP_FIXED_NOREPLACE`, never bare `MAP_FIXED`,
so a collision is an error rather than a silent unmapping of whatever was
there. It is then checked three ways:

- `mmap` is required to return the address that was asked for. On a kernel
  older than 4.17 `MAP_FIXED_NOREPLACE` is *ignored* rather than rejected and
  the mapping quietly relocates, which is the one outcome the whole file exists
  to prevent.
- every region is written and read back at both ends, because a size rounded
  the wrong way fails only at the far end;
- `--verbose` prints what landed, and the kernel's own view can be read from
  `/proc/<pid>/maps`. On this machine, for a running game:

```
02000000-02040000 rw-p    EWRAM
03000000-03008000 rw-p    IWRAM
04000000-04001000 rw-p    I/O
05000000-05001000 rw-p    palette
06000000-06018000 rw-p    VRAM
07000000-07001000 rw-p    OAM
08000000-0a000000 rw-p    ROM (and, inside it, save memory)
...                       nothing else below 0x20000000
57c2c000-...              the executable, 1.3 GiB above the top of the window
```

### The ROM and save memory are one mapping

`GBA_ROM_MAX` is the whole 32 MiB the cartridge bus decodes, not the 16 MiB
image, because the game reads past the end of its own data in a few places and
the hardware returns open bus. `tools/portify.py` relocated save memory from
the hardware's `0x0E000000` down to `0x09000000`, to save the wasm build 80 MiB
of reservation — which puts it *inside* the ROM's window.

They do not fight (the image stops at `0x09000000`), but they are one mapping
and not two: asking for them separately gets `EEXIST` from the second request.
`Coalesce()` in `mem.c` merges the region table into the fewest non-overlapping
page-aligned spans, which is also what makes a coarse page size harmless.

### Page size

Asked for, never assumed. Apple silicon and many arm64 Linux kernels use
16 KiB pages, some configurations 64 KiB, and Windows reserves at a 64 KiB
granularity. Every base address in the map is 64 KiB aligned already, so the
only thing a larger page changes is how far each size is rounded up — and since
the regions are 16 MiB apart, rounding cannot make two of them collide.

---

## The seam

There are two, one inside the other.

### The host seam — `platform/port/backend.h`

Fourteen functions, implemented twice: `platform/web/*.c` (emscripten, EM_JS
bodies that reach into the page) and `platform/native/*.c` (SDL2). Nothing
outside those two directories knows which one is present.

```c
void PortHostInit(int argc, char **argv);
void PortConsole(const char *text, int isErr);
void PortBlitFramebuffer(const u32 *pixels, int w, int h);
void PortAwaitAnimationFrame(void);
void PortAwaitRom(void);
int  PortHostRangeOk(uintptr_t addr, u32 len);
void PortAudioOpen(int ringFrames);
int  PortAudioRate(void);
int  PortAudioQueued(void);
int  PortAudioUnderruns(void);
void PortAudioSubmit(const float *samples, int frames);
int  PortSramLoad(u8 *dest, u32 size);
void PortSramMarkDirty(void);
/* plus PORT_BIOS_REGION_SIZE */
```

`platform/*.c` — the PPU, DMA, the BIOS calls, the m4a mixer, the frame loop,
3900 lines of it — is below this line and is shared unchanged.

### The operating-system seam — `platform/native/native.h`

**This is the one the Windows, macOS and ARM builds plug into.** Everything
else in `platform/native/` is SDL2 and portable C: the window, the renderer,
input, gamepads, audio, frame pacing, the save file, the command line, the
screenshot writer. Adding an operating system means writing these five and
nothing more.

```c
size_t PortHostPageSize(void);
int    PortHostReserve(uintptr_t addr, size_t size, const char **why);
int    PortHostAddrValid(uintptr_t addr, size_t len);
int    PortHostPickRomFile(char *out, size_t outSize);   /* may return 0 */
```

`mem_posix.c` and `dialog_posix.c` implement them for Linux, macOS and the
BSDs; `CMakeLists.txt` swaps in `mem_win32.c` and `dialog_win32.c` on Windows,
which do not exist yet.

The contracts that matter:

- **`PortHostReserve` must never relocate.** Failure has to mean "these
  addresses were not available", not "here is somewhere else" — `mmap` without
  `MAP_FIXED` silently relocates, and a relocated GBA map is a game whose every
  pointer is wrong in a way nothing detects.
- **`PortHostAddrValid` must ask the kernel, not guess.** `platform/dma.c`
  uses it (through `PortHostRangeOk` in `mem.c`) to tell one of the port's own C
  pointers — the address of the local that `DmaFill` is filling from, or the
  storage `build/generated/rom_copies.c` gives a few ROM arrays — from a stale
  GBA pointer that would fault. Permissive by a guess lets a wild pointer
  through to a segfault; strict by a guess silently drops real transfers, and
  that is the bug that once hid every level tilemap in the game. `msync()` and
  `mincore()` both report `ENOMEM` for an unmapped range on Linux and macOS;
  `VirtualQuery` reports `MEM_FREE` on Windows. `mem.c` memoises the last
  answer, so the syscall cost is a handful of calls a frame.

---

## Adding a platform

Write a toolchain file in `cmake/`, and the two files above. That is the whole
job — with one large exception.

### Windows

- **ABI**: build for x86, not x64. MSVC cannot compile the decompilation at all
  (it is GNU C: statement expressions, `__attribute__`, gnu89 inline
  semantics), so `CMakeLists.txt` stops with a message saying so. Use
  `i686-w64-mingw32` or clang-cl with a 32-bit target.
- **`PortHostReserve`**: `VirtualAlloc(addr, size, MEM_RESERVE|MEM_COMMIT,
  PAGE_READWRITE)`. It already refuses to relocate — it returns NULL rather
  than moving — so there is no `MAP_FIXED` trap to avoid. Reservation
  granularity is 64 KiB, which is what `PortHostPageSize` should return; every
  base in the map is already aligned to it.
- **`PortHostAddrValid`**: `VirtualQuery`, and accept only `MEM_COMMIT` with a
  readable protection.
- **`PortHostPickRomFile`**: `GetOpenFileNameW`, or return 0 and rely on
  drag-and-drop.
- SDL wants `SDL_main`; link `SDL2main` and keep `main(int, char **)`.

### 32-bit ARM (armv7 / armhf)

Nothing to write. It is ILP32 already, `mem_posix.c` applies unchanged, and
the only thing that differs is the page size — which is why `PortHostPageSize`
exists and why nothing calls `getpagesize()` directly. Check that
`MAP_FIXED_NOREPLACE` is present (Linux 4.17+); the fallback path in
`mem_posix.c` covers older kernels with a `mincore()` probe.

### arm64 and macOS — read this first

Both are blocked on the ILP32 requirement, and not in a way a toolchain file
fixes.

- **arm64 Linux** has an ILP32 ABI (`-mabi=ilp32`) but almost nothing ships a
  userland for it. The practical answer is to build armhf and run it under the
  arm64 kernel's 32-bit support, where that is compiled in.
- **macOS has had no 32-bit userland since Catalina**, and Apple silicon never
  had one. There is no flag.

So macOS needs the port to become 64-bit clean first, and that is a real
project rather than a platform port. What it involves: every pointer member of
a game structure becomes a 32-bit handle (there are 111 structures), and every
host pointer that the game stores in GBA memory — `gIntrTable`, `struct Task`'s
`main` and `dtor`, `SoundInfo`'s `CgbSound`, and the ROM function tables
`gen_rom_data.py` rebuilds — has to fit in 32 bits, which means linking the
executable's code below 4 GiB (`-image_base`, with `-pagezero_size` shrunk).
It is doable and it is not small. Do not start it by accident.

If macOS is attempted anyway, the `__PAGEZERO` part is already handled:
`CMakeLists.txt` passes `-Wl,-pagezero_size,0x1000`, because the default 4 GiB
`__PAGEZERO` covers the entire GBA map and nothing can be mapped inside it.
`MAP_FIXED_NOREPLACE` does not exist there, and `mem_posix.c` falls back to a
`mincore()` probe followed by `MAP_FIXED` — which has a race in a threaded
program, and is safe here only because the reservation happens before SDL
starts a thread.

---

## The frame loop

The web build takes one game frame per `requestAnimationFrame`, which is why it
runs at double speed on a 120 Hz display. The native build does not inherit
that. The GBA's LCD runs at **59.7275 Hz** — 280896 cycles of a 16.777216 MHz
clock — and that, not the monitor, is what a frame is worth.

So `PortAwaitAnimationFrame` paces from `SDL_GetPerformanceCounter`, and the
presentation is deliberately **not** vsynced: a 60 Hz panel would drag the game
0.46% fast, a 144 Hz panel would be a disaster, and the audio clock would fight
it either way. `--vsync` is there for anyone who would rather have no tearing.

The sleep is a coarse `SDL_Delay` down to the last two milliseconds and a yield
loop after that, because `SDL_Delay` rounds up to a scheduler tick and landing
3 ms late every frame is audible before it is visible. Falling behind is normal
— a room transition decompresses tilesets and takes longer than a frame — and
the response is to give up the time rather than earn it back; more than four
frames behind and the schedule restarts, which is what a suspended machine or a
debugger looks like.

There is no Asyncify here and none is needed. `VBlankIntrWait` blocks, and
blocking is what a process is allowed to do.

**Measuring it:** `--frames N` deliberately does not imply `--turbo`, so

```sh
time ./build/native/katam rom.gba --frames 600 --no-audio
```

should take 600 / 59.7275 = 10.046 s. A build where that comes out at 10.00 has
quietly gone back to 60 Hz.

---

## Audio

`platform/audio_out.c` is shared: it converts the mixer's `s16` to float and
pushes one block per VBlank, and reads the queue depth back so `audio.c` can
widen or narrow the next block to track the device clock. Only the transport
differs. Natively it is `SDL_OpenAudioDevice` with a callback and a lock-free
single-producer/single-consumer ring (`platform/native/audio_sdl.c`).

`SDL_QueueAudio` would have been less code and is the wrong shape: it cannot
report running dry, and the block-size feedback needs that. The ring is
lock-free rather than wrapped in `SDL_LockAudioDevice` because taking the
device lock stalls the audio thread behind whatever the game is doing, which is
what makes ports like this crackle under load.

The device picks the rate; do not assume 48000. Rate 0 — no audio at all — is a
configuration the rest of the port supports rather than an error path.

---

## Saves

A 64 KiB `.sav` in `SDL_GetPrefPath("katam-port", "katam-port")`, which is
`~/.local/share/katam-port/katam-port/` on Linux, `~/Library/Application
Support/` on macOS and `%APPDATA%` on Windows. It is the plain image an
emulator reads and writes — no header, no length prefix — so a save made in the
browser can be exported and dropped next to the native binary, and the other
way round.

The filename is the same ROM key `web/shell.html` computes, byte for byte: the
four-letter game code at `0xAC`, the version byte at `0xBC`, the image length,
and two FNV-1a hashes of the whole file. The cartridge header alone is not
enough — every dump of this game shares it — and a translation patch or a ROM
hack is emphatically a different game as far as a save file is concerned.

```
B8KE-0-1000000-229921bbfc110555.sav
```

Writes are debounced (`WriteSramEx` writes, verifies and retries, so one in-game
save is many calls) and go to a temporary file that is renamed over the real
one, so an interrupted write loses the new save rather than the old one. A
fatal signal flushes first, which is not something to be proud of in a signal
handler and is better than losing a save the player already made.

---

## Running it

```
katam [options] [rom.gba]

  --scale N          window is N times 240x160 (default 3)
  --fullscreen       start fullscreen; F11 toggles, 1-6 set the scale
  --vsync            present on the display's refresh as well as pacing
  --no-audio         open no audio device
  --frames N         run N frames and exit
  --screenshot PATH  write a PNG of the last frame; F12 writes one any time
  --window-shot PATH write a PNG of the scaled window, read back from the
                     renderer
  --hold MASK        hold these buttons for the whole run
  --mash S:MASK:P    from frame S, tap MASK with a period of P frames
  --turbo            do not pace
  --verbose          report the memory map and the frame timing
```

Controls match the web build: arrows or WASD, A = J/Z, B = K/X, L/R = Q/E,
Start = Enter, Select = Backspace or right shift. F11 fullscreen, 1-6 scale,
F12 screenshot, Ctrl+Q quit.

A gamepad works through SDL's game-controller database. The face cluster is
split the way a Nintendo layout maps onto an Xbox one — bottom and right are
both A, left and top are both B — so a two-button game does not need anyone to
look up a mapping. The left stick doubles the d-pad with a half-deflection
deadzone.

`--hold` and `--mash` take the GBA button mask directly: A=1, B=2, Select=4,
Start=8, Right=16, Left=32, Up=64, Down=128, R=256, L=512. They are the same
knobs `tools/headless_test.js` has, so a native run and a web run can be driven
identically and compared.

---

## Testing it without a person

```sh
make native-test ROM=~/roms/your-copy.gba
```

`tools/native_smoke.sh` runs the binary twice under `SDL_VIDEODRIVER=dummy` and
`SDL_AUDIODRIVER=dummy`: once with no input at all, and once tapping A and
Right, which is what gets it through the title screen and the file-select menu
and into a level. Each run writes a PNG of its last frame and three numbers:

```
     boot: 192 colours at best, 14 distinct pictures, final DISPCNT 0x1340
     play: 187 colours at best, 43 distinct pictures, final DISPCNT 0x1B40
```

Getting to those three took two wrong answers, both worth knowing about before
you write the same check for another platform.

- **Measure the best frame, not the last one.** The game fades the screen
  constantly — between the logo and the title, on every room transition — and a
  brightness fade collapses the palette to a handful of greys. A run that
  walked Kirby across a level and stopped mid-transition scored 47 colours and
  failed a check it should have passed.
- **A level does not have more colours than a menu.** The obvious threshold —
  "gameplay must be much richer than a title screen" — is false, and measuring
  said so: the title peaks at 192 distinct colours and a level frame has about
  150. It is a 4bpp console with a 512-entry palette; there is no headroom for
  the intuition. So the colour count only answers *is anything being drawn*
  (a forced blank is 1, a fading logo a few dozen).
- **Whether the game got anywhere is a separate question**, and a port that
  wedges after the title screen draws a perfectly good picture forever. Two
  things it will not do: keep producing *different* pictures, and reconfigure
  the display. So the play run has to end with a different `DISPCNT` from the
  boot run — which is what leaving the title screen for a level means in
  hardware terms — and both runs have to show the picture changing.

**This is what a new platform should be measured against.** If
`make native-test` passes on Windows, the Windows build works.

`--screenshot` writes the framebuffer the PPU produced. `--window-shot` writes
what the *renderer* produced, read back with `SDL_RenderReadPixels` before the
present — scaled, letterboxed, in whatever pixel format the texture actually
holds. They answer different questions, and the second one is the only way to
catch a picture that is correct and drawn with red and blue swapped.

The screenshot writer (`platform/native/png.c`) is ninety lines and has no
dependency, deliberately: libpng is a dependency, SDL_image is usually not
installed, and nothing in a CI log can look at a BMP. A zlib stream is allowed
to consist entirely of *stored* deflate blocks, so the only real work is the
two checksums. The files are about 40% larger than a compressed PNG and are
otherwise completely ordinary.

---

## Building without root

If you cannot install `libsdl2-dev:i386`, the toolchain file takes a
`KATAM_SYSROOT32` pointing at a directory holding the unpacked 32-bit packages:

```sh
apt-get download libc6-dev-i386 lib32gcc-13-dev libsdl2-dev:i386 \
                 libsdl2-2.0-0:i386 libxcursor1:i386 libxi6:i386 \
                 libxrandr2:i386 libwayland-egl1:i386 \
                 libwayland-cursor0:i386 libxkbcommon0:i386 libdecor-0-0:i386
for d in *.deb; do dpkg-deb -x "$d" sysroot32; done
ln -s /usr/lib/i386-linux-gnu sysroot32/lib32
ln -s /usr/lib/i386-linux-gnu/libm.so.6 sysroot32/usr/lib32/libm.so.6

KATAM_SYSROOT32=$PWD/sysroot32 cmake -S . -B build/native \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-i686.cmake
cmake --build build/native -j
LD_LIBRARY_PATH=$PWD/sysroot32/usr/lib/i386-linux-gnu ./build/native/katam rom.gba
```

The toolchain file adds `-Wl,--allow-shlib-undefined` on this path, because a
hand-assembled sysroot has SDL but not the twenty libraries SDL is linked
against, and the linker cannot resolve *their* symbols. The runtime loader can,
from the host's own i386 libraries. A properly installed machine does not use
that flag.

---

## Why CMake and not another Makefile target

The web build is one compiler with one set of flags, and a Makefile is the right
shape for it. The native build is not: four platforms are coming and they differ
in exactly the things a Makefile is worst at — finding SDL (pkg-config here, a
framework on macOS, vcpkg or a bundled SDK on Windows), what "PIE" is called,
which compiler is even in use, and how to ask for a 32-bit ABI. CMake knows all
of it, and keeps the per-platform knowledge in one toolchain file each instead
of spread through a variable soup.

What is shared with the Makefile is the source list and the compile flags, which
is what should be shared. The objects are not: `build/native` is a separate
tree from `build/obj`, because mixing objects across toolchains is exactly the
failure the Makefile's `DEBUG_INFO` comment warns about.

The compile flags are the Makefile's, for the Makefile's reasons.
`-fgnu89-inline` is the one to keep an eye on: the decompilation is compiled by
agbcc (gcc 2.95), where a plain `inline` definition also emits an external one,
and C99 inverted that rule. Without the flag every `inline void Foo(...)` in the
game becomes an undefined symbol at link time.
