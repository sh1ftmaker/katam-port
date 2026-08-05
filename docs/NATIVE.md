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

That builds for the machine you are on. Windows is the same sources through
`cmake/toolchain-windows-i686.cmake` — see [Windows](#windows) below, which also
says how far that build has and has not been verified.

There is no ROM here and no way for the program to obtain one. Supply your
own, as the argument above, by dropping the file on the window, or through the
file picker it offers when started with no argument.

---

## The two things the browser was hiding

### 1. The build has to be 32-bit

This is the second decision the port rests on, after the memory map, and it
decides which platforms are reachable at all.

The game is a 32-bit ARM program and the decompilation is its source. Its
headers define 246 named structures and unions, and **148 of them change size
or move a member under LP64** — measured, not estimated; see
[docs/SIXTYFOUR.md](SIXTYFOUR.md) for how. Those structures are not private to
the C code: they are read out of the ROM, and they are placed at addresses
`linker.ld` chose.

```
                     GBA / wasm32        x86-64
struct ToneData         12 bytes          24 bytes     read from the ROM
  ->wav at offset        4                 8           every instrument, wrong
struct Task             20 bytes          32 bytes     128 of them at 0x030019F0
struct Kirby           424 bytes         464 bytes     array at 0x02020EE0
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

That guard is coarse — it can say the ABI is wrong and nothing about what it
would break, and it cannot see a structure that changed shape for some other
reason. `platform/port/gba_layout.h` is the fine-grained half: the size and
every member offset of all 246 types, committed, and compiled as
`_Static_assert`s by every one of the four builds
(`platform/gba_layout_check.c`). A host whose ABI moves a member fails to
compile and names it. Regenerate with `make layout` when the decompilation
changes a structure on purpose, read the diff, and commit it; `make
layout-check` says whether the table still describes the tree.

On 64-bit x86 that means `-m32` and a 32-bit SDL:

```sh
sudo apt install gcc-multilib libsdl2-dev:i386     # Debian, Ubuntu
sudo dnf install glibc-devel.i686 libgcc.i686 SDL2-devel.i686   # Fedora
```

`make native` picks `cmake/toolchain-linux-i686.cmake` automatically on an
x86-64 host and `cmake/toolchain-linux-armhf.cmake` on an aarch64 one. A host
that is already ILP32 — i686, armv6l, armv7l — needs no toolchain file at all.

**What this means for the platforms** is set out under
[Adding a platform](#adding-a-platform) below. The short version: Windows is
done and is `i686-w64-mingw32`; 32-bit ARM is done and needed nothing written
beyond a toolchain file; **arm64 runs the 32-bit build rather than one of its
own**, which mostly works and has one sharp edge worth knowing before you buy a
Raspberry Pi 5; **macOS is blocked outright**, and the reason is worth reading
before starting.

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

Forcing `PortHostPageSize` to 16384 and then 65536 confirms it: the reservation
succeeds either way, the regions round outward and stay 16 MiB apart, and 400
frames of boot are unchanged. armhf itself is 4 KiB, and so is every arm64
kernel that can run it — see [arm64](#arm64--it-runs-the-armhf-build-with-one-sharp-edge)
for why those two facts are the same fact.

At 65536 something else breaks, and it is worth knowing about before Windows is
written. `PortHostAddrValid` rounds its query out to page boundaries because
`mincore` demands a page-aligned start, so with a 64 KiB page it rounds a stack
address *down* by up to 64 KiB — off the bottom of the stack's own mapping, on a
system whose real pages are 4 KiB. The probe then answers "not mapped" about a
pointer that was perfectly good, and the port refuses every DMA whose source is
a local variable, which during boot means the one that fills VRAM. On a real
64 KiB-page kernel this cannot happen, because mappings there are 64 KiB
granular too. On **Windows** it can: the allocation granularity is 64 KiB and
the page size is 4 KiB, and `PortHostPageSize` is documented above as the
former. So `mem_win32.c`'s `PortHostAddrValid` must round with `dwPageSize`,
not with the number `PortHostPageSize` returns. `native.h` says so at the
declaration.

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
void   PortHostReportAddressSpace(void);                 /* --verbose only */
```

`mem_posix.c` and `dialog_posix.c` implement them for Linux, macOS and the
BSDs; `CMakeLists.txt` swaps in `mem_win32.c` and `dialog_win32.c` on Windows.

Only the first three are load-bearing. `PortHostPickRomFile` may return 0 on a
platform with no file dialog, and the caller falls back to drag-and-drop.
`PortHostReportAddressSpace` prints nothing anybody depends on.

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
  that is the bug that once hid every level tilemap in the game. `mincore()`
  reports `ENOMEM` for an unmapped range on Linux and macOS; `VirtualQuery`
  answers it on Windows, where the test is `MEM_COMMIT` with a readable
  protection rather than merely "not `MEM_FREE`" -- the Windows section below
  says why the difference is not pedantry. `mem.c` memoises the last answer, so
  the syscall cost is a handful of calls a frame.

  This was `msync(MS_ASYNC)` until the ARM build, and the two are the same
  answer on a kernel. They are not the same answer under `qemu-user`, which
  reserves the whole guest address space up front: `msync` finds a `PROT_NONE`
  mapping at every address and reports success, so the check passes a pointer
  that faults on the next instruction. Asking the *wrong* question of the
  kernel is a category the file's own comment had not allowed for.
- **`PortHostReportAddressSpace` has to ask the kernel too**, for the same
  reason in a different place. `PortNativeReportMap` prints the port's account
  of itself, and a reservation that reported success and landed somewhere else
  would print exactly the same lines. On Linux the second witness is
  `/proc/<pid>/maps`; on Windows there is no such file, so the process walks
  itself with `VirtualQuery`.

---

## Adding a platform

Write a toolchain file in `cmake/`, and the two files above. That is the whole
of the *seam* — with one large exception, macOS.

It is not quite the whole of the job, and Windows is the evidence: `mem_win32.c`
and `dialog_win32.c` went in as designed and unsurprisingly, and then five other
things had to change, none of which anybody had predicted. A header the
force-included prelude had never had to defend against. Which subsystem the
executable is. Whether `SDL2main` is linked. Where the image is based. Which
character is a path separator. They are all written down below, because the next
platform will have its own five and the useful thing is the shape of them:
**the seam holds; the assumptions around it are what break.**

### Windows

Done, as `i686-w64-mingw32`. `cmake/toolchain-windows-i686.cmake` cross-compiles
it from Linux; the same file works in MSYS2's mingw32 shell, where the compiler
is already on `PATH`.

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-mingw.tar.gz
tar xf SDL2-devel-2.30.11-mingw.tar.gz

cmake -S . -B build/win32 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-i686.cmake \
      -DKATAM_SDL2_MINGW=$PWD/SDL2-2.30.11/i686-w64-mingw32
cmake --build build/win32 -j
cmake --build build/win32 --target package-windows
```

`package-windows` writes `build/win32/katam-port-windows-i686/`: `katam.exe`,
the `SDL2.dll` the build actually linked against, and a readme whose first line
is that there is no game in the folder. A Windows player has no package manager
to fetch SDL from, so the `.exe` on its own is not something anyone can run.

**How far this has been verified is set out at the end of this section.** The
short version: it is built and run, but under Wine, on Linux. It has never been
executed on Windows.

#### The memory map

`VirtualAlloc(addr, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)`, and unlike
`mmap` it already refuses to relocate: an explicit `lpAddress` that is not
available comes back `NULL`, never as a different address. So the whole
`MAP_FIXED` problem does not exist here.

What does exist is a question Linux does not have to ask. The argument in
`mem.c` for why nothing else wants `0x02000000`–`0x0A000000` is structural and
entirely Linux's: a PIE executable is loaded high, `brk` follows it, and `mmap`
grows down from the top. **None of that transfers.** A Windows process has no
`brk` — every heap, stack and mapping comes from `VirtualAlloc`, which hands out
whatever is free, and after the reservation the map is not free. The only things
that can be in the way are the ones already placed before `main` ran: this image,
and the DLLs the loader resolved from the import table. `SDL2.dll` is one of
those.

So `mem_win32.c` does not argue, it walks. `RangeIsFree` asks `VirtualQuery`
about every region a reservation covers and requires all of them to be
`MEM_FREE` before `VirtualAlloc` is called at all. `VirtualAlloc` failing would
also have caught a collision — but it would have said "failed", and the walk can
say *what* is in the way and which module it belongs to. On the one failure this
port cannot recover from, that is the difference between a bug report and a
shrug.

The image itself is taken out of the question by the link line, and it is worth
being clear that this is the **opposite** of the Linux answer for the same
reason. Linux needs PIE because a non-PIE binary at `0x400000` grows its `brk`
heap into EWRAM. Windows has no such heap, so the hazard is not where the image
starts but that ASLR can put it anywhere — including inside the window, which
would make the program fail to start on some boots and not others.
`CMakeLists.txt` therefore pins it:

```
-Wl,--image-base,0x10000000 -Wl,--disable-dynamicbase
```

0x10000000 is above the top of the map, so `mem.c`'s existing "this binary is
loaded below the top of the GBA map" check means exactly what it means on Linux.
This is a real loss of hardening, taken for the same reason every other one in
this port is: the addresses are the program. DLLs keep their own ASLR — only
this image is pinned — and if one of them ever does land in the window, the
walk above names it.

`PortHostPageSize` returns `dwAllocationGranularity` (64 KiB), not `dwPageSize`
(4 KiB). `VirtualAlloc` rounds a base down to the granularity and a size up to
the page, so rounding both to 64 KiB is what keeps `mem.c`'s spans and the
kernel's regions on the same edges. Every base in the GBA map is 64 KiB aligned
already, so nothing moves; only the seven sizes are rounded further than they
are on Linux.

One difference with a cost: `mem_posix.c` passes `MAP_NORESERVE`, because most
of the 32 MiB ROM window is address space the game never touches. Windows has
no equivalent that also lets a page be written on demand — `MEM_RESERVE` alone
means a read faults rather than materialising a zero page — so the map is
committed in full, about 34 MiB. That is charge against the commit limit, not
resident memory.

#### `PortHostAddrValid`, and why "not `MEM_FREE`" is the wrong test

`VirtualQuery` is finer-grained than `msync`, and the extra detail is load-bearing:

- `MEM_FREE` is the `msync` `ENOMEM` case. Reject.
- `MEM_RESERVE` **has no Linux equivalent.** Address space has been claimed but
  no pages stand behind it, and a read faults exactly as if it were free.
  Windows heaps and thread stacks both keep large reserved-but-uncommitted
  tails, so this is not a corner case; accepting it would hand `dma.c` a range
  that segfaults on touch.
- `PAGE_NOACCESS` and `PAGE_GUARD` are committed and still fault. The guard page
  under every thread stack is the common one.

And a range can span several regions, so it walks rather than asking once: a
transfer that starts in the last page of the heap and runs off the end has a
perfectly valid first region.

The test is readability, not writability, which is what `msync` amounts to on
the POSIX side. A DMA *into* read-only host memory would still fault — but
`dma.c` never has a host destination that is not one of the port's own writable
arrays, and diverging here would mean the two platforms accept different
transfers, which is a worse bug than the one it would prevent.

#### The console, and why this is not a GUI-subsystem program

A Windows `.exe` built with `-mwindows` has no `stdout`. Not "output goes
somewhere else" — `GetStdHandle` returns nothing and the CRT's `stdout` writes
into a hole.

Everything this port has to say goes to `stdout`: which ROM it loaded, where the
save file is, every refused DMA, the frame report `tools/native_smoke.sh` reads,
and the message explaining why the memory map could not be reserved and the game
is not going to start. That last one is the whole reason the walk in
`mem_win32.c` exists, and a GUI-subsystem build would throw it away.

So the build is console-subsystem and `PortConsole` is unchanged — the shared
`fputs`/`fflush` in `host_sdl.c` is correct as written, because the handles are
real. The cost is a console window next to the game window when it is launched
from Explorer, which the packaged readme explains. `AttachConsole(ATTACH_PARENT_PROCESS)`
from a GUI-subsystem binary would hide that window, at the price of a program
whose diagnostics exist only when it happens to have been started from a shell.
That trade is available and was not taken.

Two link-line details follow from it. SDL's CMake package puts `-mwindows` in
`SDL2::SDL2`'s interface and only removes it if `SDL2_NO_MWINDOWS` is set before
`find_package`, so `CMakeLists.txt` sets it. And `SDL2main` is deliberately not
linked: it supplies a `WinMain` that calls `SDL_main`, which needs
`platform/main.c`'s `main` to have been renamed by `SDL_main.h` — and `main.c`
does not include SDL at all, by design, because it is shared with the emscripten
build. The Windows build defines `SDL_MAIN_HANDLED` and calls
`SDL_SetMainReady()` in `host_sdl.c` instead, and keeps a plain
`main(int, char **)`.

#### The rest of what Windows broke

None of it was in the two files.

- **`abs` and `<intrin.h>`.** `platform/port/prelude.h` exists because the
  decompilation's `global.h` defines `abs` as a macro, and any system header
  parsed afterwards sees `int abs(int);` become a syntax error — so the prelude
  pulls the system headers in first, while the guards are still unset. MinGW's
  `<intrin.h>` is a member of that list that nobody had met, because
  `SDL_cpuinfo.h` includes it and `<SDL.h>` therefore fails to parse in all three
  of `platform/native/*sdl*.c`. One `#include <intrin.h>` under `#ifdef _WIN32`,
  in the file that already documents this exact failure.
- **SDL's `sdl2.pc` is not relocatable.** The `prefix=` inside the MinGW
  development tarball is a path on the machine that cut the release. Its
  `sdl2-config.cmake` works out the prefix from its own location and is correct
  anywhere. That is why `CMakeLists.txt` skips pkg-config on Windows, and why
  the toolchain file clears `PKG_CONFIG_LIBDIR` — left alone, pkg-config reads
  the *host's* `/usr/lib/pkgconfig` while cross-compiling and reports a 64-bit
  Linux SDL, and the first sign of trouble is several hundred lines from the
  linker.
- **Path separators.** The window title took `strrchr(path, '/')` as a basename,
  which on a path from the file picker is the whole path. Both separators now.
- **`SDL_GetPrefPath`** needed nothing: it returns
  `C:\Users\<you>\AppData\Roaming\katam-port\katam-port\` and the `%s%s.sav`
  join works unchanged. The save file written there is byte-identical in name
  and content to the Linux one, which is the point of the ROM key.
- **CRLF** matters in exactly one place: the packaged `README.txt`, which will
  be opened in Notepad by someone who has never heard of this program.
  `configure_file(... NEWLINE_STYLE CRLF)` converts it at build time, so the
  copy in the repository stays like every other file in the repository. Nothing
  else here is text: `png.c` and `save_file.c` already open `"wb"`, and a text
  mode fopen would have quietly corrupted every screenshot.
- **`-static-libgcc`**, so the folder that ships is two files. `libgcc_s_dw2-1.dll`
  exists to unwind exceptions there are none of here, and explaining a missing
  DLL to a player is a support cost for nothing.

`GetOpenFileNameW`, not `...A`: SDL's file functions take UTF-8 on Windows, the
`A` entry point returns the system ANSI code page, and the mismatch is a ROM
path that works for everyone whose name spells in Latin-1. `OFN_NOCHANGEDIR` is
not optional either — without it the dialog leaves the process's current
directory wherever the player was browsing, and every relative path the port
writes afterwards (`F12` screenshots, `--screenshot`) lands somewhere nobody
chose.

#### What was actually verified, and what was not

The machine this was built on has no Windows and no root. The toolchain is
Ubuntu's `gcc-mingw-w64-i686-posix` 13.2 and SDL2 2.30.11's MinGW development
tarball, both unpacked into a scratch directory (see "Building without root");
the binary was then run under **Wine 9.0**, also unpacked without root.

Wine is not Windows. It is a faithful enough implementation of `VirtualAlloc`,
`VirtualQuery`, the PE loader and the CRT that all of the above is exercised,
and it is not evidence about the Windows loader's DLL base addresses, which is
the one residual risk.

What ran:

```
     boot: 192 colours at best, 14 distinct pictures, final DISPCNT 0x1340
     play: 187 colours at best, 44 distinct pictures, final DISPCNT 0x1B40
     smoke test passed
```

Those are `tools/native_smoke.sh` unmodified, driving `katam.exe` through a
wrapper that translates the ROM path. They are the Linux build's numbers — the
same colour counts, the same two `DISPCNT`s, and 43 or 44 distinct pictures out
of 47 samples against Linux's 44. The play screenshot is Kirby in a level
with the HUD up, and `--window-shot` — the renderer readback, which is the only
thing that catches a correct picture drawn with red and blue swapped — comes
back with Kirby pink.

`--frames 600` with pacing on took 10.148 s against a target of 10.046 s, so
`SDL_GetPerformanceCounter` is doing what it does on Linux and the build has not
quietly gone back to 60 Hz.

The address space, from the kernel's own side (`--verbose`, abridged):

```
0x02000000 +0x00040000  committed private     EWRAM
0x03000000 +0x00010000  committed private     IWRAM
0x04000000 +0x00010000  committed private     I/O
0x05000000 +0x00010000  committed private     palette
0x06000000 +0x00020000  committed private     VRAM
0x07000000 +0x00010000  committed private     OAM
0x08000000 +0x02000000  committed private     ROM (and save memory inside it)
0x10000000 +0x00405000  committed image       katam.exe
...                     nothing at all between 0x01500000 and 0x02000000
```

Seven mappings, at a 64 KiB page size, exactly where the table says. Everything
the loader placed before `main` is below `0x01500000` or above `0x20000000`;
275 regions are above, the lowest at `0x20000000`. The save landed in
`%APPDATA%` and read back, under a filename byte-identical to the Linux one.

And from the file rather than from the running process, `objdump -p katam.exe`:

```
ImageBase              10000000
DllCharacteristics     00000100      NX_COMPAT       (not DYNAMIC_BASE)
Subsystem              3             console
DLL Name: SDL2.dll  COMDLG32.DLL  KERNEL32.dll  msvcrt.dll
```

`DYNAMIC_BASE` absent is the pinned image base; three of the four imports are
Windows itself, which is why `package-windows` ships two files.

**Not verified.** Real Windows, at all. The file dialog, which needs a display
Wine did not have here — it compiles and its failure mode is a return of 0 and a
fall back to drag-and-drop, which is the same path a cancel takes. Audio on a
real device, gamepads, fullscreen, drag-and-drop, and the `F12` screenshot path.
And the residual risk named above: if the Windows loader ever bases `SDL2.dll`
or a shim DLL inside `0x02000000`–`0x0A000000`, this program cannot start. It
will say so, and name the module, and that is all it can do.

### 32-bit ARM (armhf) — built and tested

Almost nothing to write, which was the prediction and turned out to be true:
`mem_posix.c` and `dialog_posix.c` apply unchanged and there is no `mem_arm.c`.
What the port needed was one toolchain file, one compile flag, and one fix to a
kernel probe that had been wrong all along and only ARM was in a position to
show it.

**On a Raspberry Pi running a 32-bit OS**, there is no cross-compilation and no
toolchain file:

```sh
sudo apt install build-essential cmake pkg-config libsdl2-dev
make sync  KATAM_DECOMP=~/katam
make stubs KATAM_DECOMP=~/katam
make native
./build/native/katam ~/roms/your-copy.gba
```

`uname -m` says `armv7l` (or `armv6l`), `make native` picks no toolchain file,
and CMake's ILP32 check passes because the host already is. Expect the compile
to take a long time — it is four thousand files of decompilation on an SD card
— which is the argument for the cross build below.

**Cross-compiling from a desktop:**

```sh
sudo apt install gcc-arm-linux-gnueabihf
sudo dpkg --add-architecture armhf && sudo apt update
sudo apt install libsdl2-dev:armhf              # see the caveat below

make native NATIVE_TOOLCHAIN=cmake/toolchain-linux-armhf.cmake \
            NATIVE_DIR=build/native-armhf
```

The caveat: on an x86-64 machine, `apt` will refuse `libsdl2-dev:armhf`.
Debian multiarch will co-install `i386` with `amd64` and `armhf` with `arm64`,
but the x86 and ARM archives are different archives, and `ports.ubuntu.com`
is not in a desktop's sources at all. Unpacking a sysroot by hand is the way
through, and it is how this was built and tested — see
[Building without root](#building-without-root).

#### What ARM actually changed

- **Plain `char` is unsigned.** x86 and wasm default to signed, ARM and
  PowerPC to unsigned, and until there was an ARM build nothing in the port
  cared. The GBA's answer settles it: agbcc compiles
  `char c = -1; return c < 0;` to `mov r0, #0` and warns that the comparison is
  always false, so the console's plain `char` is unsigned and the port's two
  existing builds had it backwards. `-funsigned-char` is now in `CMakeLists.txt`
  and in the `Makefile`, so every build says which it means. Both smoke tests —
  web and i686 — produce byte-identical output with and without it, so nothing
  the port has ever reached depends on the difference; the flag is there so that
  stays true on the next host rather than because a bug was found.

- **`PortHostAddrValid` was asking the wrong syscall.** It used
  `msync(MS_ASYNC)`, whose ENOMEM means "something in that range is not mapped".
  On a kernel that is exactly right; under `qemu-user` it is exactly wrong,
  because the emulator reserves the whole guest address space up front and
  `msync` finds a `PROT_NONE` mapping everywhere and reports success. The
  address that exposed it is not hypothetical:

  ```
  0x3fcbc034   msync=ok   mincore=ENOMEM   /proc/self/maps: absent   read: SIGSEGV
  ```

  That is a DMA source pointer the game presents during level load, which every
  other host reports and skips. Trusting `msync` turned it into a segfault four
  hundred frames in. `mincore()` answers correctly in both places and is what
  the file uses now, with `msync` kept as the fallback for a platform without
  it. Checked on x86-64 and i686 as well: on a real kernel the two agree.

- **Nothing else.** Alignment was the expected problem and did not appear.
  `platform/dma.c` already masks both endpoints to the transfer unit before
  moving anything (the hardware ignores those bits and the game relies on it),
  and `bios.c` does the same for `CpuSet` and `CpuFastSet`, so the bulk moves
  are aligned by construction rather than by luck. ARMv7 does unaligned `LDR`
  and `STR` in hardware anyway; what it cannot do unaligned is `LDRD` and `LDM`,
  which the compiler only emits where it can prove alignment from the type.
  Endianness is not a question — ARM Linux is little-endian, confirmed at run
  time, and the port would not survive being wrong about it for one frame.

- **`-mfloat-abi=hard` is what an `arm-linux-gnueabihf` compiler emits and
  there is nothing to choose.** This was written down as a risk — VFP computes
  at the declared precision while `-m32` on x86 defaults to x87 and its 80-bit
  intermediates — on the assumption that `platform/m4a_mixer.c` was scalar
  `float` and `double`. It is not: the mixer contains no floating point at all
  and neither does `trig.c`, and the i686 objects for both have no x87
  instruction in them.

  The port's only floating point on a game-visible path is `ArcTan2`,
  `ObjAffineSet` and `BgAffineSet` in `platform/bios.c`, which feed the affine
  registers. Those have now been measured across i686 with x87, i686 with SSE,
  x86-64, armhf and wasm32: **all five agree**, bit for bit, over a large fixed
  input set. emscripten's `cos` and `sin` differ from glibc's in their last
  bits, and the difference does not survive the truncation to `s16`.
  [docs/SIXTYFOUR.md](SIXTYFOUR.md) has the table.

#### Which ARM

The toolchain file sets no `-march`, deliberately, because the two ARM
distributions that matter disagree and picking one silently excludes the other:

| | baseline |
|---|---|
| Debian / Ubuntu armhf | ARMv7-A, VFPv3-D16, Thumb-2 |
| Raspberry Pi OS 32-bit | ARMv6, VFP2 — so it runs on a Pi 1 and Zero |

A binary built against Ubuntu's armhf runs on a Pi 2 and later and dies with an
illegal instruction on a Pi 1 or Zero. If you want those, say so:

```sh
make native NATIVE_TOOLCHAIN=cmake/toolchain-linux-armhf.cmake \
            NATIVE_DIR=build/native-armhf \
            KATAM_ARM_ABI_FLAGS="-march=armv6+fp -mfpu=vfp -marm"
```

Whether it would be *playable* on a 1 GHz ARM11 is a different question, and
the software PPU suggests not.

### arm64 — it runs the armhf build, with one sharp edge

There is no arm64 build and there will not be one until the port is 64-bit
clean. That project has now been measured rather than guessed at —
[docs/SIXTYFOUR.md](SIXTYFOUR.md) has the counts, the failure it produces, and
an estimate of 4–6 weeks. `-mabi=ilp32` exists on arm64 and no distribution
ships a userland for it.

What does work is running the **armhf** build under the arm64 kernel's 32-bit
support, and on most 64-bit ARM Linux that works out of the box:

```sh
sudo dpkg --add-architecture armhf     # already done on 64-bit Raspberry Pi OS
sudo apt update
sudo apt install libsdl2-2.0-0:armhf
./katam ~/roms/your-copy.gba
```

The kernel needs `CONFIG_COMPAT`, and **`CONFIG_COMPAT` on arm64 depends on
4 KiB pages** (`ARM64_4K_PAGES || EXPERT` in `arch/arm64/Kconfig`). That is the
sharp edge, and it has a very concrete form:

- **Raspberry Pi 2, 3, 4, Zero 2 W, CM3/CM4 running 64-bit Raspberry Pi OS**:
  4 KiB pages, armhf runs. Install the `:armhf` runtime libraries above and it
  is a normal program.
- **Raspberry Pi 5, CM5, Pi 500**: the firmware loads `kernel_2712.img` by
  default, and that kernel exists precisely because it uses **16 KiB pages**.
  A 32-bit binary will not execute on it. There is also no 32-bit Raspberry Pi
  OS for these boards at all. The fix is one line in
  `/boot/firmware/config.txt`:

  ```
  kernel=kernel8.img
  ```

  which is the same 64-bit kernel built with 4 KiB pages. Reboot and armhf
  binaries run. You give up whatever the 16 KiB page size was buying.
- **Distributions that use 64 KiB pages on arm64** — Fedora and RHEL do —
  cannot run this build, for the same reason and with no config-file fix.

`getconf PAGESIZE` on the machine you are aiming at answers the question in one
line. If it is not 4096, the armhf build will not start there.

### macOS

**macOS has had no 32-bit userland since Catalina**, and Apple silicon never
had one. There is no flag, and unlike arm64 there is no 32-bit compatibility
mode to fall back to.

So macOS needs the port to become 64-bit clean first, and that is a real
project rather than a platform port. It has been measured, and
[docs/SIXTYFOUR.md](SIXTYFOUR.md) is the whole of it. The short version:

- **148 of the 246 structures** change size or move a member under LP64, and
  the constrained set cannot be narrowed by analysis — the game's own allocator
  hands out `void *` and the type appears a line later, so the safe answer is
  all of them.
- Every pointer member becomes a 32-bit handle, and every host pointer the game
  stores in GBA memory — `gIntrTable`, `struct Task`'s `main` and `dtor`,
  `SoundInfo`'s `CgbSound`, the ROM function tables `gen_rom_data.py` rebuilds
  and the 219 function pointers it patches into ROM structures — has to fit in
  32 bits, which means linking the executable's code below 4 GiB (`-image_base`,
  with `-pagezero_size` shrunk).
- The rewrite is not textual: 65 of the 169 pointer-member names are also the
  names of non-pointer members elsewhere, so `tools/portify.py` cannot drive it
  and a type-aware pass is needed.
- Estimate: **4–6 weeks** for someone who knows this codebase.

An LP64 build can be configured today with `-DKATAM_ALLOW_LP64=ON`, which is an
experiment switch and not a step towards a working build. Without the layout
assertions it compiles and links with **no errors and no warnings at all**, and
then dies four calls into `AgbMain` on a jump table that ran over its
neighbour. Do not start this by accident.

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

**This is what a new platform should be measured against.** It is what the
Windows build was measured against: `tools/native_smoke.sh`, unmodified, driving
`katam.exe` through a wrapper that translates the ROM path, reported 192/14 and
187/44 with different `DISPCNT`s — the Linux build's numbers.

armhf, run under `qemu-arm-static` on an x86-64 machine, gives:

```
     boot: 192 colours at best, 14 distinct pictures, final DISPCNT 0x1340
     play: 187 colours at best, 44 distinct pictures, final DISPCNT 0x1B40
```

and the two PNGs it writes are **byte-identical** to the ones the i686 build
writes on the same machine — same `md5sum`, both runs. That is a stronger
result than the three numbers were designed to give: the software PPU, the DMA
and the game's own logic produce the same frame on two different instruction
sets, so any ARM-specific misbehaviour would have to be invisible in 1800 frames
of two runs and in every pixel of two screenshots.

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

### …and the Windows cross-compiler, the same way

The Windows build was produced on a machine with no root and no Windows. MinGW
unpacks and relocates cleanly — gcc works out its own prefix from `/proc/self/exe`,
so the whole toolchain runs from wherever it lands:

```sh
apt-get download binutils-mingw-w64-i686 mingw-w64-common mingw-w64-i686-dev \
                 gcc-mingw-w64-base gcc-mingw-w64-i686-posix \
                 gcc-mingw-w64-i686-posix-runtime
for d in *.deb; do dpkg-deb -x "$d" mingw; done
ln -s i686-w64-mingw32-gcc-posix mingw/usr/bin/i686-w64-mingw32-gcc
PATH=$PWD/mingw/usr/bin:$PATH cmake -S . -B build/win32 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-i686.cmake \
    -DKATAM_SDL2_MINGW=$PWD/SDL2-2.30.11/i686-w64-mingw32
```

Debian and Ubuntu ship the driver as `-posix` and `-win32` and let
update-alternatives make the plain name; unpacking by hand skips that, hence the
symlink. (The toolchain file looks for all three names, so the symlink is a
convenience rather than a requirement.) The threading model only matters for
libstdc++, and there is no C++ here.

SDL2 needs no unpacking trick at all — the MinGW development tarball from
libsdl.org is a plain tarball and its `sdl2-config.cmake` is relocatable.

**Running it** on the same machine takes Wine, which also unpacks:
`libwine:i386` and `wine32:i386`, plus `libz-mingw-w64` for the `zlib1.dll` that
Wine's own `user32` imports and that neither package contains. Point `WINEPREFIX`
at a scratch directory, fix the absolute paths in `usr/lib/wine/wineserver` (a
shell script), and `tools/native_smoke.sh` will drive `katam.exe` through a
three-line wrapper. Wine is not Windows and the Windows section above says
exactly what that does and does not prove.
### The same thing for armhf

The armhf case is not an escape hatch, it is the only route from an x86-64
desktop: `apt` will not install `libsdl2-dev:armhf` on `amd64` however much root
you have, and `ports.ubuntu.com` — where every armhf package lives — is not in a
desktop's sources at all.

The cross compiler itself is an ordinary `amd64` package:

```sh
apt-get download gcc-arm-linux-gnueabihf libc6-dev-armhf-cross qemu-user-static
```

The sysroot is not, and there are two differences from the i686 recipe. The
first is that it has to be **complete**: the i686 sysroot deliberately borrows
the host's headers, because glibc ships one set of x86 headers chosen between by
`__x86_64__`, but armhf is a different architecture whose whole userland has to
come from the sysroot. So it wants `libc6-dev`, `linux-libc-dev` and the
transitive runtime closure of `libsdl2-2.0-0` — about eighty packages, 250 MB
unpacked — fetched from `ports.ubuntu.com` and unpacked into one tree. The
second is that the tree needs `lib -> usr/lib` making by hand: Ubuntu is
merged-`/usr`, every package installs under `/usr`, and the `PT_INTERP` in the
binary still says `/lib/ld-linux-armhf.so.3`.

```sh
KATAM_SYSROOT_ARM=$PWD/sysrootarm \
  cmake -S . -B build/native-armhf \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-armhf.cmake
cmake --build build/native-armhf -j
```

Because the sysroot is complete, the toolchain file sets `CMAKE_SYSROOT` and
needs no `--allow-shlib-undefined`. What it does need is a `-L` and a `-B` at
the sysroot's `usr/lib/arm-linux-gnueabihf`, because Debian's *cross* packages
put the target libc somewhere a sysroot does not — `/usr/arm-linux-gnueabihf/lib`
— and its `libc.so` is a linker script naming that absolute path. Without those
two flags the link fails with `cannot find /usr/arm-linux-gnueabihf/lib/libc.so.6`
while an entirely good `libc.so.6` sits in the sysroot.

### Running it on the desktop that built it

`qemu-user-static` is what turns "it compiles" into "it boots":

```sh
qemu-arm-static -L $PWD/sysrootarm ./build/native-armhf/katam rom.gba
```

With `binfmt-support` installed and a handler registered — which needs root once
— the `qemu-arm-static` prefix disappears and the armhf binary runs by name, so
`make native-test` drives it unchanged. Without root, a two-line wrapper script
in place of the binary does the same job for the smoke test.

**What a qemu run does and does not prove.** It runs the real ARM instructions,
which is the part that matters for codegen, alignment and floating point. The
address space it runs in is qemu's: `guest_base` is 0, so the GBA window really
is at `0x02000000` in the host kernel's own `/proc/<pid>/maps`, but everything
*around* it is a `PROT_NONE` reservation qemu made, and that changes two answers.
One is `msync`, described [above](#the-operating-system-seam--platformnativenativeh)
— that one is a real bug qemu found. The other is where the executable lands:
qemu loads even a PIE image near `0x400000`, so the port's "this binary is loaded
below the top of the GBA map" warning fires on every qemu run. On a 32-bit ARM
kernel a PIE goes near `mmap_base`, around `0xb6000000`, well above the map; the
warning is qemu's and not the port's.

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

`-funsigned-char` is the one the ARM build added, and it is there for the same
kind of reason as `-fwrapv`: it is not a preference, it is what the compiler the
game was written for did. Whether plain `char` is signed is
implementation-defined, x86 and wasm say signed, ARM says unsigned, and agbcc —
asked directly — says unsigned. A build that does not name it is a slightly
different program on each host, and there is nothing in the port that would ever
report the difference.
