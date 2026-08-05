# Making the port 64-bit clean

The port must be built ILP32 today. That rules out macOS entirely, and it means
arm64 Linux runs the armhf build under the kernel's 32-bit support rather than
a build of its own. This file is the measurement of what removing that
restriction would cost, so the decision can be made from numbers instead of
from an estimate.

**Summary.** The layout problem is 148 structures, and it is the whole problem.
The pointer-narrowing problem people expect turns out to be 244 source sites,
and almost all of them are harmless.

---

## Status: finished, and what the last two bugs were

**2026-08-05.** All four builds — wasm32, i686, x86-64, aarch64 — produce
**byte-identical DMA transfer streams over 63236 transfers and 1401 frames** of
the same scripted input, and pixel-identical frames. The three native builds
report identical smoke-test numbers (`boot: 192 colours, 14 pictures, DISPCNT
0x1340` / `play: 187 colours, 43 pictures, DISPCNT 0x1B40`). `make layout-check`
asserts 246 types and 2144 offsets. The ILP32 builds did not move: the wasm DMA
stream is unchanged, transfer for transfer, across every change described here.

Two bugs stood between "the layout assertions pass" and that result. Neither
was visible to any check the port had, and both are the same shape — *something
outside a struct member whose size the console fixed at four bytes*.

### 1. A file-scope array of pointers, read through another type

```c
extern struct RoomTiledBG *const gUnk_082D8D74[];         /* stride 8 under LP64 */
gUnk_03002E60 = (const union Unk_03002E60 *)gUnk_082D8D74; /* stride 4 */
```

`union Unk_03002E60` is a transparent union of `PTR32` members and is four
bytes, so every `gUnk_03002E60[i]` read element `i/2` of the real array. On the
console both strides are four and the cast is exact.

`narrow32.py` narrows pointer members of *structures*, on the argument that a
structure's layout is decided by the ROM, by `linker.ld` or by the game's own
allocator. A file-scope array has none of those constraints — until something
casts it. This is the only place in the tree that does, which
`tools/check_ptr_arrays.py` is what establishes and what keeps establishing.

The symptom was a corrupt background layer: every level's BG descriptor came
from the wrong room, 20999 of 38400 pixels. Nothing faulted, nothing asserted,
and both source addresses were valid mapped memory — which is what made it look
like a graphics bug for three rounds of investigation.

### 2. Twenty-five types defined in .c files rather than headers

`portify.py` ran the narrowing pass over `include/*.h` only. `struct
CutsceneTrigger`, `struct AreaMap`, `struct WorldMap`, `struct Unk_080880AC`
and twenty-one others are declared in the .c file that uses them, and they grew
under LP64 — `CutsceneTrigger` by 48 bytes.

Nothing about a structure's *location* decides whether its pointer members have
to be four bytes. What decides it is whether anything outside the compiler
cares about the layout, and the game's own allocator does:

```c
task = TaskCreate(ObjectMain, sizeof(struct CutsceneTrigger), ...);
```

A bigger struct does not corrupt anything. It makes the allocation bigger, so
the fixed 0x2604-byte IWRAM heap fills sooner, so a later object misses IWRAM
and falls back to EWRAM — which `TaskCreate` does silently and by design. The
two builds were still computing the same thing; they had stopped keeping it in
the same place. The first frame that looked different was **73 frames** later,
and the first *DMA* that looked different was 73 frames after that.

One of the twenty-five was an anonymous union whose whole body was on one line,
which the line-oriented narrowing pass cannot see. Exactly one such member
exists in the tree, so it is a site-table entry rather than a parser.

### What now checks for both

| | |
|---|---|
| `tools/abi_size_diff.py` | every DWARF struct/union size, ILP32 build against LP64 build. This is the check `make layout-check` cannot make: a type only enters `gba_layout.h` if it has a known console address to be asserted against, and neither of these types did. |
| `tools/check_ptr_arrays.py` | file-scope pointer arrays that something reads through a cast |
| `PORT_DMA_STACK` + `tools/symbolize_stack.py` | the call stack that issued a given DMA transfer, as decompiled C names, from either host |
| `PORT_WINDOW=addr:len` | one window hashed every frame, for finding *when* a known address started to differ |

### And an ILP32 native control now exists

`docs/DEBUG-TOOLING.md` used to say the thing that would settle an ABI question
completely was an ILP32 *native* build — same host layer, same audio driver,
only the ABI different — and that it did not exist. It does now, and it was one
line of CMake: `project(katam-port C CXX)` makes CMake compile *and link* a C++
test program at configure time, so every 32-bit target needed a 32-bit
libstdc++ to configure, for a language none of its sources use. Declaring `C`
and calling `enable_language(CXX)` only when `CMAKE_SIZEOF_VOID_P` says so
fixes it.

It earned its keep immediately. i686 against wasm32 — different hosts, same ABI
— was identical over all 63212 transfers, which retired the standing worry that
the SDL and web hosts drive the sound engine differently enough to matter. i686
against x86-64 — same host, different ABI — diverged at frame 482. That is what
turned "something differs" into "this is an ABI bug", with no host variable
left in the comparison.

### One measurement that was contaminated

The native runs in this investigation were reading a **save file left on the
developer's machine** by earlier runs, while the node harness starts with blank
SRAM every time. So the two builds were not being given the same starting
state, and a real-looking divergence at frame 409 was the game resuming a saved
tutorial in one build and not the other. `XDG_DATA_HOME` pointed at a scratch
directory is the fix, and any cross-build comparison needs it. The bug in §1 was
found before this was noticed and is unaffected — it was proved structurally,
by the two strides, not by the frame counts.

---

## Status, and a correction to this document

This file was first written as a measurement, and it concluded that the
conversion needed a type-aware rewriter over ~40,000 member accesses, which
meant `libclang`, which meant a new dependency and 4–6 weeks. **That conclusion
was wrong**, and the section it lives in ([What a conversion would
involve](#what-a-conversion-would-involve)) is kept below as written so the
reasoning can be checked rather than quietly replaced.

What it got right: C cannot express a four-byte thing that still behaves like a
pointer, and a textual codemod cannot tell which `->unk0` is which. What it
missed: **C++ can express it**, in a class with `operator->`. That changes what
has to be rewritten from ~40,000 *uses* to ~285 *declarations*, and the uses do
not change at all.

So the work is under way rather than hypothetical. Done so far:

| | |
|---|---|
| `platform/port/gba_layout.h` — the assertion table | done |
| `tools/cxxify.py` — the decompilation's C through a C++ front end | done, 156/156 |
| `platform/port/p32.h` — `PTR32`, the 4-byte pointer member | done, tested both ABIs |
| C++ linkage, so the 64-bit build links by the ILP32 build's rules | done |
| `cmake/toolchain-linux-arm64.cmake` — an aarch64 target, `make arm64` | **renders**; frame-identical to the ILP32 reference |
| code and game-visible host storage below 4 GiB | done, `-no-pie -Wl,-Ttext-segment=0x10000000` |
| narrowing the pointer members to `PTR32` | done, 277 members |
| **the assertion table holding at LP64 — the definition of done for the layout** | **met** |
| the run-time tail the assertions cannot cover | in progress |

The layout half of this project is finished and the finishing is checkable:
`platform/gba_layout_check.c` compiles in the LP64 build with nothing to say,
all 246 types and 2144 offsets, on x86-64 and on aarch64 both. What is left is
the tail the table was never able to see — the linker-placed arrays with fixed
extents, and everything that is not a structure member.

The aarch64 binary boots, plays the intro and reaches the title screen. Its
frame at 1200 is **pixel-for-pixel identical** to the wasm32 ILP32 reference —
0 of 38400 pixels differ, 167 distinct colours, DISPCNT `0x1740`, and `0
distinct gaps reported, 0 calls into missing functions`. So does the x86-64
LP64 build. A 64-bit host now produces the same picture as the 32-bit one, from
structures laid out the way a 32-bit console laid them out.

What did *not* work at the time of writing was the play path: mashing into a
level died in `sub_08010590` on an object-list walk. That was traced to the ROM
function-pointer patcher having been silently disabled by the narrowing (commit
`6448212`), and the two bugs in the section at the top of this file. All of it
now runs; the paragraph is kept because the sentence after it — "this is a
genuinely 64-bit-only failure and the first one that has been" — was correct
and is how the remaining work got scoped.

**The ILP32 builds are the control and they have not moved.** The wasm build
over 1200 frames is byte-identical through all of it: frame md5
`de1b1dfefeb50eeba2791e08f332942c`, 167 colours, DISPCNT `0x1740`, peak 0.6527,
0 diagnostics.

### What the C++ route actually cost

Measured, not estimated: all 156 game translation units through `g++
-fpermissive`. 107 compiled unchanged. The other 49 produced 698 errors from
**six causes**, not a long tail — a parameter named `template` (41 files, and
it cascades), nested struct tags that have file scope in C and class scope in
C++, GNU range designators, the flexible-array idiom, transparent unions, and
one unprototyped declaration. All six are mechanical and all six are handled by
`tools/cxxify.py`, whose transforms are valid C as well, so the ILP32 builds
compile the identical tree.

Linking took four more findings, and those are the ones that matter, because
none of them produces a diagnostic anywhere:

- **`const` at file scope has internal linkage in C++ and external linkage in
  C.** Every ROM data table in the decompilation is a const array, so under C++
  they all silently became static. 420 definitions now carry `extern`.
- **`extern "C"` does not fix that.** It sets *language* linkage, not storage
  linkage. Learning this cost several rounds of "the symbol is right there".
- **The game is given C linkage throughout**, so the 64-bit build links by the
  same rules the ILP32 builds do. Anything else would be a second port.
- The stub set is derived from the wasm link, and the two optimisers do not
  fold the same references, so the 64-bit link asked for symbols `make stubs`
  had never heard of. Every one was an artefact of the above rather than a
  missing symbol.

### There is no toolchain shortcut

Checked rather than assumed, because it would have made all of this
unnecessary. x86-64 LLVM does have 32-bit address spaces in its data layout
(`p270:32:32`), but clang rejects an address-space qualifier on a struct field:
`field may not be qualified with an address space`. aarch64's data layout has
no 32-bit address space at all. `-mabi=ilp32` on aarch64 has no libc anybody
ships and no upstream kernel support, and clang's `aarch64_32` target is
Apple's watchOS ABI.

### The revised estimate

`libclang` is not needed and neither is the rewriter it was for. What remains:

| | |
|---|---|
| link-line and host-storage work (code and heap below 4 GiB) | 3–5 days — **spent** |
| rewriting 285 member declarations to `PTR32` | 2–4 days — **spent** (277 members) |
| the function-pointer patcher and the ROM tables | 2–3 days |
| making it run, and the long tail the assertions do not cover | 1–2 weeks |
| **total** | **2–3 weeks** |

The first two rows are done, and half of the third: the 34 ROM tables whose
elements are pointers are emitted with `PTR32` elements by
`tools/gen_ram_symbols.py`, which is the generator that names them. The other
half is not. `PortPatchRomFunctionPointers` in `build/generated/rom_fn_tables.c`
still writes `*(void **)sRomStructFns[i].at` — an eight-byte store into a field
the ROM gave four bytes — exactly as
[Function pointers stored in GBA memory](#2-function-pointers-stored-in-gba-memory)
below says it does.

The last row is still the one to distrust, for the reason given at the end of
the original estimate: the assertions cover struct layout and nothing else, and
the port has around 190 linker-placed symbols with extents that are not
asserted anywhere. Two of the three things that broke first were of exactly
that kind — `gMPlayJumpTable`'s extent and `INTR_VECTOR`'s address — and neither
is a structure.

---

---

## How the numbers were obtained

Not from `katam.elf`. It carries `.debug_info` and modern tools refuse it:
agbcc is gcc 2.95 and gdb stops at `unexpected tag 'DW_TAG_imported_declaration'`
before reading a single type. The working 32-bit builds are the evidence
instead.

`tools/gen_gba_layout.py` compiles one translation unit that includes every
header in `build/port-src/include` — 157 of them — with the port's own flags,
and reads the layout back out of the DWARF with `tools/dwarf_layout.py`, a
pure-python reader written for this (no dependency, the same rule
`gen_ram_symbols.py` and `gen_rom_data.py` follow). Doing it twice, at `-m32`
and `-m64`, gives the difference directly.

Everything below was cross-checked a second way: the same numbers, turned into
`_Static_assert`s and put in front of gcc and clang. The two methods agree on
the count of 148 exactly, and they share no code.

One instrument had to be thrown away first, and it is worth recording. The
first attempt to count pointer-narrowing sites passed `-w` (which the port
does) followed by `-Wpointer-to-int-cast`, and reported **zero** sites at
LP64. gcc's `-w` defeats a later `-W`, and defeats a later `-Werror=` as well.
The measurement below drops `-w` entirely and filters the output by message,
with the ILP32 build as a control.

---

## What is in the tree now

### `platform/port/gba_layout.h` — the committed table

246 named struct and union types, 2144 member offsets, generated from the
ILP32 build and written out as assertions:

```c
/* struct ToneData -- gba/m4a.h:58 */
_Static_assert(sizeof(struct ToneData) == 12, "struct ToneData changed size -- it is read at a fixed address");
_Static_assert(offsetof(struct ToneData, wav) == 4, "struct ToneData::wav moved");
```

89 members are bitfields and carry no `offsetof`; they are listed in comments
with their bit position, and the containing structure's size is asserted, which
is what a bitfield layout change would move.

Every structure is asserted, not just the ones a pointer would move. Working
out which structures are genuinely constrained is the hard direction of this
question — see [The true constrained set](#the-true-constrained-set) — and
getting it wrong is silent. Asserting all of them costs nothing at run time and
cannot be wrong.

### `platform/gba_layout_check.c` — the thing that compiles them

One translation unit with no code in it. It is picked up by
`platform/*.c` in both the Makefile and `CMakeLists.txt`, so **all four builds
— wasm32, i686, armhf and mingw32 — check the same table** with nothing wired
up per platform.

### `make layout` and `make layout-check`

`make layout` re-measures and rewrites the table; run it when the
decompilation changes a structure on purpose, read the diff, commit it.
`make layout-check` fails if the committed table no longer describes the tree.
The build already fails on a *changed* number, because the assertions are
compiled; the check catches the other direction, a structure that stopped
being declared anywhere and dropped out of the table quietly.

Both need a 32-bit compiler, because the table records what the GBA layout
*is*. `LAYOUT_CC` and `LAYOUT_ABI` override it; `KATAM_SYSROOT32` is honoured,
so the no-root recipe in [NATIVE.md](NATIVE.md#building-without-root) works.
The generator refuses to run against a compiler with 8-byte pointers rather
than writing a table of the wrong numbers.

### Proving the net catches anything

A check that passes is not evidence until it has been made to fail. One wrong
number was put in the table — `offsetof(struct ToneData, wav) == 0xDEADBEE` —
and every build was run:

```
web      error: static assertion failed ... 'offsetof(struct ToneData, wav) == 233495534': struct ToneData::wav moved
i686     error: static assertion failed: "struct ToneData::wav moved"
windows  error: static assertion failed: "struct ToneData::wav moved"
armhf    error: static assertion failed: "struct ToneData::wav moved"
make layout-check   platform/port/gba_layout.h is out of date.
```

Five for five, naming the structure and the member. `sizeof(struct Kirby)` was
falsified the same way with the same result.

This is worth having whether or not the 64-bit work ever happens. The
decompilation is moving; a structure that gains a member upstream changes the
memory the ROM is read through, and until now nothing in the port would have
said so.

---

## The three problems, with counts

### 1. Layout — 148 structures

The decompilation's headers define **246** named types (236 structs,
10 unions). Under LP64:

| | |
|---|---|
| types whose size or member offsets change | **148** |
| …with a pointer member of their own | 121 |
| …that only *embed* something with one | 27 |
| types that do not change | 98 |
| member offsets that move | 1110, over 139 types |
| aggregate size of the 148 | 42,120 → 51,200 bytes (+21.6%) |

**The set that changes is exactly the set that carries a pointer.** Nothing
moves for any other reason — no alignment ripple, no enum width, no padding
surprise. That is a cleaner result than expected and it means the problem has
one cause.

Some individual figures, because the aggregate hides what matters:

```
struct Task          20 ->   32   +60%   128 of them at 0x030019F0
struct ToneData      12 ->   24  +100%   every instrument, read from the ROM
struct SongHeader    12 ->   24  +100%   every song, read from the ROM
struct EwramNode      8 ->   16  +100%   the game's own allocator's node
struct Object2      180 ->  224   +24%   every object in the game
struct Kirby        424 ->  464    +9%   the array at 0x02020EE0
struct LevelInfo   1640 -> 2784   +70%
```

`gTasks` is `MAX_TASK_NUM` = 128 entries at `0x030019F0`. At 20 bytes each that
is 2560 bytes and ends below its neighbour; at 32 bytes each it is 4096 and
runs over `gVramHeapMaxTileSlots` at `0x03002488` and everything else IWRAM
holds. That is the failure `docs/ARCHITECTURE.md` says the address-map decision
exists to avoid, and it happens 148 times.

### 2. Function pointers stored in GBA memory

Two shapes, and they are counted separately because only one of them is solved.

**Solved, for the tables the headers declare.** `tools/gen_rom_data.py`
rebuilds 8 ROM function tables — 304 entries wired to decompiled C, 1 stubbed —
and patches **219 function pointers that live inside ROM structs**, 211 of
which resolve to a decompiled function. The mechanism generalises to LP64
without changing its shape: it already maps ARM address → host function.

What does not survive is where it writes them:

```c
*(void **)sRomStructFns[i].at = sRomStructFns[i].fn;
```

At LP64 that is an 8-byte store into a 4-byte field of a structure whose stride
the ROM fixed at 0x18. It clobbers the next field and the value it writes is
above 4 GiB. Both halves need the conversion's answer, not a new mechanism.

**Not solved, and not visible to any type analysis**: 52 members across 31
types are declared as function pointers, and more are not. The example that
started this — `ws->unk0.obj2.unk78 = sub_0800C124;` — assigns a function to a
`void *` member of `struct Object2`, which lives in IWRAM at an address the
game's own allocator chose. `-Wint-conversion` reports none of these, because
they are not integer conversions; they are perfectly legal C that happens to
put a host code address in a field the console gave four bytes.

The port already answers this once in WebAssembly, where a function pointer
*is* a 32-bit table index. Natively it needs the executable's code below
4 GiB — `-Wl,-Ttext-segment` on Linux, `-image_base` with `-pagezero_size` on
macOS, which is what `CMakeLists.txt` already passes for a different reason.

### 3. Host memory the game can observe — smaller than expected

Where does the port hand the game an address of its own?

- `build/generated/rom_copies.c` — one array, `gLevelObjLists`, 1148 bytes of
  ROM data given real storage.
- `build/generated/rom_fn_tables.c` — 8 tables and the 219 patched pointers
  above.
- `platform/dma.c`'s `DmaFill` source, which is the address of a local.
- `PortHostRangeOk` / `PORT_GLOBAL_BASE`, which exist precisely to tell those
  from a stale GBA pointer.
- `platform/dma.c` writes `(u32)src` and `(u32)dest` into the emulated DMA
  registers at `REG_ADDR_DMA0`. Nothing reads them back, but at LP64 they
  truncate.

The addressing itself is not a problem, and this was checked rather than
assumed: the LP64 binary reserves the whole GBA window at its true addresses on
the first try, and `/proc/self/maps` agrees.

```
[katam-port] page size 4096, GBA window 0x02000000..0x0A000000 (7 mappings)
[katam-port] this binary at 0x62b5b74a33d0, a stack address at 0x7ffe46b9314c,
             a heap address at 0x62b5c763f370 -- all outside the window
```

A 32-bit GBA address is a valid 64-bit host address; the map lives entirely
below 4 GiB and nothing about a wider pointer changes that. What changes is
storage width, and only where the game supplies the storage.

**The narrowing sites, measured.** Every source the port builds, compiled at
`-m64` and at `-m32` with `-Wpointer-to-int-cast -Wint-to-pointer-cast
-Wint-conversion` and the results diffed site by site:

| | ILP32 | LP64 |
|---|---|---|
| `cast to pointer from integer of different size` | 0 | 154 |
| `cast from pointer to integer of different size` | 0 | 90 |
| sites present in both | 4 | 4 |

244 LP64-only sites in 35 files; `task.c` has 28, `pause_area_map.c` 21,
`code_08032E98.c` and `code_0802B4A8.c` 20 each. The 154 int→pointer casts are
`(void *)0x02000000` and its relatives and are **harmless** — the constant is a
valid host address. The 90 pointer→int casts are the ones that need reading.

The four sites that warn in both builds are not an LP64 problem and are worth
knowing about: `task.c` does `u32 curOffset = (u16)cur;` — a pointer truncated
to *sixteen* bits — because that is what the ARM code did.

---

## The true constrained set

This is where the estimate is most likely to be wrong, so it is worth being
precise about what could and could not be established.

A structure is constrained if the game observes its bytes at an address the
port did not choose. Three seeds were used, then closed transitively over
embedded and pointed-to types:

| seed | types |
|---|---|
| A — named by a `linker.ld` RAM symbol (`port/ram_symbols.h`) | 27 |
| B — named by a ROM symbol (`port/rom_data.h`, `rom_copies.c`, `rom_fn_tables.c`) | 29 |
| C — cast out of one of the game's allocators | 87 |
| union of the three | 133 |
| transitive closure | 201 |

Of the 148 types that LP64 moves, **129 are in the closure and 19 are not**:

```
CgbChannel  Chest  ChestItemPopup  EightDirCannon  EwramNode  FlashSetupInfo
GoalGameBonus  LargeStarStoneBlock7D  LogoStruct  MultiSioArea  RockBlock
SaveBuffer  Sio32MultiLoadArea  SoundInfo  SpecialHubMirror  TitleStruct
Unk_08387348  Unk_0888562C_4  VertSlidingDoor
```

Every one of those is constrained. `SoundInfo` is 4016 bytes of sound engine
reached through `SOUND_INFO_PTR`, a hardware address macro. `EwramNode` is the
game's own allocator's node. `SaveBuffer` is save memory. `Chest` is this:

```c
struct Task *task = TaskCreate(ObjectMain, sizeof(struct Chest), 0x1000, TASK_USE_IWRAM, ObjectDestroy);
void *ptr = TaskGetStructPtr(task);
chest2 = ptr;
```

The allocator returns `void *` into an untyped local, and the type only appears
on the next line. No seed sees it, and no reasonable widening of the seeds
would: the pattern is "assign a `void *` to something", which is most of the
codebase.

**So the honest answer is that the constrained set cannot be established by
analysis, and the safe answer is all 246.** That is why the assertion table
covers everything rather than a list. It is also why the earlier
grep-and-closure estimates (111, 103, 65) were all low: they are lower bounds
on a set whose upper bound is the whole file.

There is a second reason "all of them" is right. `TaskCreate` is handed
`sizeof(struct Chest)` and hands back an offset into a heap of fixed extent —
`gEwramHeap` is 0x20080 bytes at `0x02000000`, `gIwramHeap` is 0x2604 bytes at
`0x03003A20`, both placed by `linker.ld`. Growing the structures by 21.6% does
not merely move members; it runs the game's own allocators out of space in
regions that cannot grow, and `struct Task::structOffset` is a `u16`.

---

## What the LP64 build actually does

*This section is the state before the narrowing, and it is kept because it is
the evidence the whole approach was chosen from. All three of its numbers have
since moved: the assertions pass, the jump table is four bytes an entry again,
and the build gets as far as the task scheduler. What has not moved is the
finding in the middle — that an LP64 build produces no diagnostic of any kind —
which is why the table is the only thing that can say when this is done.*

`-DKATAM_ALLOW_LP64=ON` turns `CMakeLists.txt`'s pointer-size guard from an
error into a loud warning. It is an experiment switch and says so at configure
time. `-DKATAM_SKIP_LAYOUT_CHECK=ON` additionally drops
`platform/gba_layout_check.c`, which is the only way to get past the
assertions; it prints a warning of its own.

With the assertions in, before any of the members were narrowed, one translation
unit failed and produced the enumeration:

```
1258 static assertion failures
 148 types changed size
1110 member offsets moved
```

With them out, **208 of the 209 translation units compile clean and the
binary links with no errors and no warnings**. That is the finding that
matters most in this file. The port compiles with `-w`; there is no diagnostic,
no link error and no run-time check anywhere in the tree that would have
noticed. Before this table existed, an LP64 build was a build that worked.

It does not run for long. It reserves the memory map correctly, loads the ROM,
and dies before the first frame:

```
Program received signal SIGSEGV
#0  0x0000000000000000 in ?? ()
#1  0x00005555556d6a91 in m4aSoundInit ()
#2  GameInit () -> AgbMain () -> main ()

   0x5555556d6a8a <m4aSoundInit+362>:  call   *0x3001628
```

An indirect call through IWRAM. `gMPlayJumpTable` is at `0x03001510` and its
extent is 0x90 bytes — 36 entries of 4. At 8 bytes an entry it needs 288, so
index 35 lands at `0x03001628`, past the end of the table and inside
`gCgbChans` at `0x030015A0`, which nothing has written. The call goes to zero.

That is the predicted failure, at the predicted place, for the predicted
reason — a fixed-extent array overrunning its neighbour — arriving four calls
into the program with no other symptom.

---

## What a conversion would involve

### The shape

Pointer members of constrained structures become 32-bit handles. The GBA map
is below 4 GiB, so a handle is just the address: `(void *)(uintptr_t)h` and
`(u32)(uintptr_t)p`, with no table and no allocator. Everything the game
points at is either in the map or is host storage that the link line has to
place below 4 GiB.

That last part is the piece with no shortcut. `-Wl,-Ttext-segment=0x10000000`
on Linux and `-image_base` on macOS put the code low, and `CMakeLists.txt`
already pins the image base on Windows for a different reason, so the
precedent is there. The heap is the harder half: `malloc` above 4 GiB is fine
for the port's own storage, and is not fine for anything the game will hold a
pointer to. `rom_copies.c`'s storage and the DMA fill locals are the known
cases and there will be more.

### What is mechanical

- The layout table. It already exists, it is generated, and it is what tells
  you when you are done.
- The member declarations. 285 pointer members over 121 types, in headers, in
  a shape a generator can rewrite.
- The ROM function-pointer patcher. It is generated; the change is to the
  emitted store, not to the 219 entries.
- Anything the compiler names: the 244 narrowing sites, whatever survives.

### What is not

`tools/portify.py` is a textual codemod, which is what makes it survive a
moving decompilation. This change needs types, and the member names do not
carry them:

| | |
|---|---|
| distinct pointer-member names | 169 |
| …never used for a non-pointer anywhere | 104 |
| …also the name of a non-pointer member elsewhere | **65** |
| member accesses naming an unambiguous one | 3,400 in 167 files |
| member accesses naming an ambiguous one | 37,494 in 186 files |

`unk0` is a pointer in 22 types and an integer in others; `x` and `y` are
pointers in one structure and coordinates everywhere else. A textual codemod
can convert 3,400 accesses safely and cannot touch the other 37,494 without
knowing the type of the expression on the left of the arrow.

So the conversion is either:

- **a type-aware rewriter**, which means a C front end — `libclang` is the
  realistic answer and it is a new dependency of a kind this repo has so far
  avoided; or
- **accessor macros that do not need the member name to be unique**, which
  means the *declarations* change and the uses do not, which C cannot express
  for a struct member; or
- **hand conversion of 148 structures and up to 40,894 accesses**, which is not
  something to start.

There is a fourth option that should be checked before any of them: **x32**.
x86-64 has an ILP32 ABI (`-mx32`) and aarch64 has `-mabi=ilp32`. Neither has a
distribution userland, which is why `docs/NATIVE.md` dismisses the aarch64 one
— but the question for macOS is different, because there the blocker is a
missing 32-bit userland and not a missing ABI, and Apple has neither.

### Risks

- **A conversion that is 95% done is indistinguishable from one that is
  finished.** The table is what makes that false, and it is the reason to do
  this in the order it is written here.
- **The decompilation is moving.** Every structure it changes moves numbers in
  a committed table, and a conversion in flight makes that diff unreadable.
- **The save file.** `SaveBuffer` is in the constrained set. A save written by
  a wrong build is a corrupt save, and the port shares its `.sav` format with
  emulators.
- **The two builds diverge.** wasm32 stays ILP32 forever. A handle conversion
  that only the 64-bit build compiles is two ports; one that both compile is
  one port with an indirection the wasm build does not need.

### Size

With the table in place, and assuming `libclang`:

| | |
|---|---|
| the rewriter, and getting it right | 1–2 weeks |
| link-line and host-storage work (code and heap below 4 GiB) | 3–5 days |
| the function-pointer patcher and the ROM tables | 2–3 days |
| making it run, and the long tail the assertions do not cover | 1–2 weeks |
| **total** | **4–6 weeks** for one person who knows this codebase |

The last row is the one to distrust. The assertions cover struct layout and
nothing else; the LP64 build's first crash was a fixed-extent array, and the
port has around 190 linker-placed symbols with extents that are not asserted
anywhere.

---

## Two things measured on the way

### Floating point does not differ between hosts

`docs/NATIVE.md` said `platform/m4a_mixer.c` was scalar `float` and `double`
and that nobody had measured whether the hosts agree. Both halves were wrong.

The mixer contains no `float` and no `double` — it is entirely integer, and the
i686 object has no x87 instruction in it. Neither does `trig.c`. The port's
only floating point on a game-visible path is in `platform/bios.c`:
`ArcTan2` (`atan2`) and `ObjAffineSet`/`BgAffineSet` (`cos`, `sin`, then a
double multiply truncated to `s16`), which feed the affine registers and
therefore pixels.

Those three were lifted verbatim into a probe, run over a large fixed input
set and hashed:

| | ArcTan2 | ObjAffine | raw cos/sin bits |
|---|---|---|---|
| i686, x87 (what the build uses) | a122cf8b | bda9dc11 | c4b690e4 |
| i686, `-mfpmath=sse` | a122cf8b | bda9dc11 | c4b690e4 |
| i686, `-ffloat-store` | a122cf8b | bda9dc11 | c4b690e4 |
| x86-64 | a122cf8b | bda9dc11 | c4b690e4 |
| armhf (VFP), under qemu | a122cf8b | bda9dc11 | c4b690e4 |
| wasm32, under node | a122cf8b | bda9dc11 | **fc0a9740** |

x87's 80-bit intermediates change nothing here. emscripten's `cos` and `sin`
do differ from glibc's in their last bits — that is the one disagreement in
the table — and the difference does not survive the truncation to `s16` for
any input sampled. It is not proof that it never would.

### Implementation-defined behaviour, asked of five compilers

The armhf build found that plain `char` is unsigned on the console and the
port had it backwards. The same question was put to agbcc and to all four of
the port's compilers, as compile-time constants so agbcc could answer from its
assembly:

| | agbcc | i686 | armhf | wasm32 | x86-64 |
|---|---|---|---|---|---|
| plain `char` signed | no | no | no | no | no |
| `(-8) >> 1` | −4 | −4 | −4 | −4 | −4 |
| `sizeof(enum)`, incl. negative and >16-bit | 4 | 4 | 4 | 4 | 4 |
| bitfields allocated low bits first | yes | yes | yes | yes | yes |
| `sizeof{char; long long}` | **12** | **12** | 16 | 16 | 16 |
| `sizeof{char; double}` | **12** | **12** | 16 | 16 | 16 |
| `sizeof{u8:3; u16:9}` | **4** | 2 | 2 | 2 | 2 |

Two real disagreements, both latent today:

- **`long long` and `double` align to 4 on agbcc and on i386, and to 8 on ARM,
  wasm and x86-64.** No structure the decompilation defines has a member of
  either type — checked directly in the DWARF — so nothing depends on it. The
  layout table would catch it if one appeared, because i686 and armhf would
  then disagree with each other and one of them would fail to compile.
- **A bitfield run whose members have different declared base widths starts a
  fresh storage unit on agbcc and packs on every modern compiler.** 11 types
  contain bitfields and none of them mixes widths, so nothing depends on this
  either. **The layout table cannot catch this one**, because all four hosts
  agree with each other and disagree with the console. If the decompilation
  ever writes `u8 a : 3; u16 b : 9;`, every build the port has will be wrong
  together and silently. That is worth a line in
  [DECOMP-REQUESTS.md](DECOMP-REQUESTS.md) rather than a check here.

---

## Recommendation

*Superseded — see [Status](#status-and-a-correction-to-this-document). The
original text is kept below because the reasoning was sound given the numbers
it had, and the numbers changed rather than the reasoning.*

The remaining order is: link-line and host-storage work, then the 285
declarations, then the ROM function-pointer patcher. Keep every step behind
`KATAM_ALLOW_LP64` until the assertions pass at LP64. **The assertions passing
is the definition of finished, and there is no other one** — that part of the
original conclusion stands unchanged and is the reason the table was built
first.

### As originally written

Keep the table; it earns its place on the decompilation alone.

Do the conversion only if macOS or a native arm64 build is actually wanted.
The web build is the Mac answer today and it is a good one, arm64 Linux runs
the armhf build on any 4 KiB-page kernel, and 4–6 weeks buys a second way to
run the game on hardware that already has one — against a permanent tax on
every future decompilation import.
