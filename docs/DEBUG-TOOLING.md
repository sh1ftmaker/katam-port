# Debug tooling

Everything below was tried against this build on 2026-08-04, emscripten 6.0.5,
201 objects, node v22.22.3, using `tools/headless_test.js` with a real ROM.
Sizes are the linked `.wasm`; timings are wall-clock for 5400 frames with the
scripted input that reaches a level.

## Recommendation

**Adopt two things.**

1. **DWARF plus `llvm-symbolizer`.** Compile with `-g`, link with
   `-gseparate-dwarf`, and every wasm frame in a trap — from node, from Chrome,
   from the crash report the shell already collects on a phone — turns into
   `file:line:column`. This is the single largest change to what a crash tells
   you, it costs 100 KB on the shipped wasm and ~7% run time, and it needs no
   browser extension. Verified end to end below: an `Out of bounds memory
   access` resolved to `task.c:122:31` and a synthetic bad function-pointer call
   resolved to the exact call site.

2. **UndefinedBehaviorSanitizer**, as an occasional sweep, not a shipped build.
   It works here, it needs no shadow memory and therefore does not care about
   the fixed-address map, and on its first run it reported five distinct classes
   of real bug with file, line and column — including the null dereference that
   is currently the first thing to go wrong at boot. It costs 2.1× run time and
   makes the wasm 25 MB, which is fine under node and not shippable.

**Do not spend time on AddressSanitizer.** It is not merely unsupported here, it
is geometrically incompatible with the memory map. Evidence in its section.

Also worth knowing right now: `-sSAFE_HEAP=1` **aborts at boot in this tree**
(`TaskCreate` → `task.c:122`, `gNextTask->prev` with `gNextTask == NULL`), and
`-sSAFE_HEAP=2` is a *downgrade* from `=1`, not an upgrade — it turns the
alignment check into a one-time warning, which is the only part of SAFE_HEAP
that has ever caught anything in this port.

| variant | wasm | 5400 frames | verdict |
|---|---|---|---|
| baseline `-O2 --profiling-funcs` | 2.45 MB | 28.9 s | — |
| `-g` + `-gseparate-dwarf` | 2.55 MB (+7.3 MB side file) | 30.9 s | **adopt** |
| `-fsanitize=undefined` | 25 MB | 62.3 s | **adopt, as a sweep** |
| `-sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2` | 2.50 MB | 28.9 s | free, keep on in debug builds |
| `-sSAFE_HEAP=1` | 2.70 MB | dies at boot | fix `task.c:122` first |
| `-sSAFE_HEAP=2` | 2.70 MB | dies at boot | weaker than `=1`; skip |
| `-fsanitize=undefined -fsanitize-minimal-runtime` | 5.0 MB | not measured | offsets only, no source lines |
| `-gsource-map` | 2.4 MB (+1.2 MB `.map`) | not measured | DevTools only; drops names |
| `-fsanitize=address` | — | — | **cannot link** |

## What a trap tells you today, and what is missing

A trap arrives as `RuntimeError: Out of bounds memory access` or `null function
or function signature mismatch`. WebAssembly does not carry a faulting address
in either, and nothing in emscripten adds one. What the engine *does* carry is a
frame list with a code offset per frame:

```
at safe1.wasm.TaskCreate  (wasm://wasm/safe1.wasm-00a7610a:wasm-function[3790]:0x24283b)
at safe1.wasm.CreateLogo  (wasm://wasm/safe1.wasm-00a7610a:wasm-function[2936]:0x1c1991)
```

`--profiling-funcs` (already on) is what supplies the names. The offsets are the
part nothing currently uses, and they are the part that carries the line number.

### Read the trap *kind* before anything else

WebAssembly has several distinct traps and they mean genuinely different
things. Reading the wrong one costs a day:

| message | what it means |
|---|---|
| `Out of bounds memory access` | a load or store past the end of linear memory |
| `null function or function signature mismatch`, `table index is out of bounds` | an **indirect** call whose target does not match the call site — reported at the *caller* |
| `unreachable` | the module executed an `unreachable` instruction |

The last one is the one to be careful with, because C does not have an
`unreachable` statement and there are only a few things that emit it:
`__builtin_trap`, a path the optimiser proved could not be taken, an
`abort()`-shaped stub — and **wasm-ld's answer to a signature disagreement on a
direct call.** That last one is not hypothetical here; it is what build
`8e8d234d6b1c` died of.

When a call and its definition are typed differently — which an implicit
declaration guarantees, since it means `int f()` — `wasm-ld` cannot emit the
call, so it emits a stub whose entire body is `unreachable` and points the call
there. The link **succeeds**, with one warning:

```
wasm-ld: warning: function signature mismatch: PortTrace
>>> defined as (i32, i32, i32, i32) -> i32 in .../warp_star.o
>>> defined as (i32, i32, i32, i32) -> void in platform/main.o
```

Then `wasm-opt -O2` inlines the two-byte stub into every caller, so the trap is
attributed to the calling function and there is no frame naming the callee at
all. `sub_0800DC5C` in that build disassembles to exactly this: the three
argument loads, kept because they might fault, and then the trap.

```
  229cf9: 41 b4 9f 88 10   i32.const 33689524   ;; &gKirbys[0].animationIndex
  229cfe: 2f 01 00         i32.load16_u 0
  229d01: 00               unreachable
```

Two consequences worth internalising. Binaryen also folds identical functions,
and once a body is "evaluate the arguments, then trap" many bodies become
identical — all 32 warp-star state handlers collapsed into one function in that
build, so the name in the stack trace was the surviving representative and not
necessarily the state the game was in. And `unreachable` inside a function
whose source contains no call that could trap is a strong signal to go and read
the link output rather than the C.

The build now passes `-Werror=implicit-function-declaration`, which turns this
whole class into a compile error naming the file and line.

## 1. AddressSanitizer — does not work here, and cannot be made to

`emcc -fsanitize=address` refuses to link this build, twice over:

```
$ emcc -fsanitize=address -sGLOBAL_BASE=167772160 ... t.c -o a.js
emcc: error: ASan does not support custom GLOBAL_BASE

$ emcc -fsanitize=address -sSAFE_HEAP=1 t.c -o a.js
emcc: error: ASan does not work with SAFE_HEAP
```

Both are hard errors in `emsdk/upstream/emscripten/tools/link.py`
(`setup_sanitizers`). They are not conservatism. ASan's shadow memory **starts
at address zero** and occupies the bottom eighth of linear memory, and
emscripten then sets `GLOBAL_BASE` to the *end* of the shadow, so its static
data, stack and heap begin immediately above it. link.py computes:

```
user_mem  = INITIAL_MEMORY (+50 MiB, because ALLOW_MEMORY_GROWTH=0)
total_mem = user_mem * 8/7, rounded up to a wasm page
shadow    = total_mem / 8      ->  GLOBAL_BASE = shadow
```

For our `INITIAL_MEMORY=201326592` that is a 290,062,336-byte memory and a
shadow of `[0, 0x02294000)`. I built a toy with those settings to check the
arithmetic rather than trust it — `wasm-dis` reports `(memory $0 4426 4426)`,
which is exactly the predicted 290,062,336 bytes, and an initial stack pointer
of `0x027551C0`.

So under ASan:

- **EWRAM at 0x02000000 is inside ASan's shadow region.** The port writing a
  tilemap would be scribbling on ASan's bookkeeping for user address
  0x10000000.
- **`GLOBAL_BASE` would be 0x02294000**, i.e. emscripten's static data would
  start inside the reserved map, with the stack at 0x027551C0 and the heap
  growing up through IWRAM at 0x03000000.

There is no `INITIAL_MEMORY` that fixes this, because the two constraints are
opposite. The map must be above the shadow (`shadow ≤ 0x02000000`) *and* below
`GLOBAL_BASE` (`shadow ≥ 0x0A000000`), and `GLOBAL_BASE == shadow` by
construction. Pushing the shadow above the map needs `total_mem = 8 ×
0x0A000000 = 1.25 GiB` of linear memory — and at that size the shadow is
`[0, 0x0A000000)`, which contains the entire map including the 16 MB ROM at
0x08000000. Either way they overlap.

And it would not even pay off if it linked. ASan only reports an access whose
shadow byte is poisoned, and poison comes from `malloc`, from stack frames and
from globals it laid out itself. The game allocates out of its own EWRAM/IWRAM
heaps at fixed addresses; to ASan those are unremarkable unpoisoned bytes. A
`TaskGetStructPtr` result landing 4 KB into the wrong place — the failure this
port actually has — would be silent. Making it not silent means calling
`__asan_poison_memory_region` from `EwramMalloc`/`IwramMalloc`, which is a
project, not a flag.

`-fsanitize=leak` *does* link with our settings (verified). It has nothing to
find: the port's allocation lives in the game's own heaps, not in `malloc`.

## 2. UndefinedBehaviorSanitizer — works, and found real bugs immediately

```sh
# compile: every object, same CFLAGS as the Makefile plus
-fsanitize=undefined
# link: same LDFLAGS plus
-fsanitize=undefined
```

Nothing else changes. UBSan needs no shadow memory and no address-space
reservation — every check is a branch emitted next to the operation, with the
source location baked into a static record — so `GLOBAL_BASE`, the fixed map
and `ALLOW_MEMORY_GROWTH=0` are all irrelevant to it. That is precisely why it
works where ASan cannot.

Compiling 201 objects: 23 s versus 9 s (`-P 8`). Linking: 14 s versus 4 s.
Result: 25 MB wasm, 62.3 s for 5400 frames against 28.9 s. It reached the same
frame as the baseline — the instrumentation does not change what the game does.

What one 5400-frame run reported, deduplicated:

| site | what UBSan says |
|---|---|
| `task.c:122:42` | member access within null pointer of type `struct Task` — `gNextTask->prev`, `gNextTask` is NULL |
| `code_080023A4.c:1182:12` | member access within null pointer of type `struct Task` |
| `platform/dma.c:68:13` | store to null pointer of type `u16` |
| `sprite_2.c:228–278` (14 sites) | `OamData` accessed at a misaligned address; `load/store of misaligned address … requires 4 byte alignment` |
| `main.c:161:25` | index 14 out of bounds for `const IntrFunc[14]` — one past the end of `gIntrTable` |
| `intro.c:3309:30` | index **-1** out of bounds for `struct Sprite[32]` |
| `intro.c:1280:36` | index 56 out of bounds for `const struct Unk_08387814[56]` |
| `code_08026044.c:324,332` | index 4 out of bounds for `s16[4]` |
| `demo.c:27:15` | index 2 out of bounds for `u16[2]` |
| `bg.c:372:51` | pointer index expression overflowed |

None of these are visible in the baseline build. Note what the array-bounds
checks are doing: `gIntrTable` is at a fixed address in IWRAM
(`#define gIntrTable ((IntrFunc *)0x03000560)`), and UBSan still bounds-checks
it, because the check comes from the declared type, not from any knowledge of
the heap. The whole reserved map is checkable this way.

The misaligned `OamData` cluster is the same class of bug `SAFE_HEAP` caught
once, found at 14 more sites and with the line number attached.

Caveats, all measured:

- **25 MB.** Nearly all of it is UBSan's location records. Not something to put
  on a phone. `-fsanitize-minimal-runtime` brings it to 5.0 MB but the messages
  degrade to `ubsan: out-of-bounds by 0x004b032d` — a code offset, which is
  recoverable only if that same build also carries `-g` and you symbolize it.
- **`-fno-sanitize-recover=all` is not usable here.** Built and run: the game
  stalls at frame 0 and the port's own DMA guard starts firing. The first UB it
  hits is the benign one-past-the-end read of `gIntrTable`, so halting there
  buys nothing anyway. Stay with the default recovering mode, which reports
  every site and keeps running — one session gives you the whole list.
- Objects do not record the flag, exactly as `CHECK_POINTERS` warns. Build into
  a separate object directory or `rm -rf build/obj` when switching.

Narrower subsets link fine if 25 MB is a problem
(`-fsanitize=alignment,pointer-overflow,shift,signed-integer-overflow,integer-divide-by-zero`,
verified to link with the full `LDFLAGS`).

## 3. `SAFE_HEAP`, `ASSERTIONS`, `STACK_OVERFLOW_CHECK`

It is worth knowing exactly what SAFE_HEAP checks, because for this port it is
much less than it sounds. Binaryen rewrites every load and store into a call to
a checker; disassembling `safe1.wasm` gives, for each access:

```wat
(if (i32.or (i32.or
      (i32.lt_u (i32.load (i32.const 167979204))   ;; the sbrk pointer
                (i32.add (local.tee $1 (i32.add (local.get $0) (local.get $1)))
                         (i32.const 2)))
      (i32.gt_u (local.get $0) (local.get $1)))    ;; offset overflow
      (i32.lt_u (local.get $1) (i32.const 1024)))  ;; near-null
  (then (call $segfault) (unreachable)))
(if (i32.and (local.get $1) (i32.const 1))
  (then (call $alignfault)))
```

That is the whole spatial check: **`1024 ≤ addr` and `addr + size ≤ sbrk(0)`**.
Because the port puts its map *below* `GLOBAL_BASE`, every address from 1024 up
to the top of the heap passes — the entire GBA map, every hole in it, and every
ARM code address like `0x0802FE84` that leaks out of a ROM table. SAFE_HEAP
cannot see a wild pointer that stays inside 192 MB, which is nearly all of them.

What it does catch here is (a) near-null dereferences and (b) misalignment. And:

- **`SAFE_HEAP=2` downgrades the alignment check to `warnOnce`**
  (`src/runtime_safe_heap.js`: `alignfault()` aborts under `=1`, warns under
  `=2`). Since the alignment check is the part that has earned its keep in this
  port, `=2` is strictly worse. Use `=1`.
- **`segfault()` takes no arguments.** The abort message is
  `Aborted(segmentation fault)` with no address, in either mode, with or
  without `ASSERTIONS`. SAFE_HEAP gives you a stack, not an address.
- **In this tree, `SAFE_HEAP=1` aborts during boot**, 0.15 s in:
  `segfault ← TaskCreate ← CreateLogo ← sub_080001CC ← AgbMain`. UBSan names the
  same line independently: `task.c:122` is `if (slow->next == gNextTask->prev)`,
  and `TasksInit` leaves `gNextTask` NULL, so this reads address 2. On hardware
  that reads BIOS ROM and is harmless; in the port it reads emscripten's null
  page and is equally harmless — which is why the normal build never notices.
  Guarding that one line is what it would take to get SAFE_HEAP back.

`-sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2` together: 2.50 MB (+50 KB), 28.9 s
(no measurable cost), nothing reported over 5400 frames. Per emscripten's
`settings.js`, `STACK_OVERFLOW_CHECK=1` puts a cookie at the top of the stack
and checks it at each tick; `=2` adds a binaryen pass that checks every stack
pointer assignment. Both are cheap enough to leave on in any non-shipping
build. Note that neither watches the **Asyncify** stack
(`ASYNCIFY_STACK_SIZE=32768`), which is a separate allocation; an Asyncify stack
overflow is its own failure mode and is reported by Asyncify's own assertion
when `ASSERTIONS` is on.

`ASSERTIONS` also renames the Asyncify export wrapper from `wrapper` to
`__asyncify_wrapper_3990` in stack traces, which is a small readability win.

## 4. DWARF — the recommendation, with the evidence

The current `build/katam-dbg.js` rule does not do what it looks like it does.
It passes `-O0 -g2` at *link* time over objects compiled with `-O2 -g0`. Debug
information comes from the compile step; there is none in the objects, so there
is none in the binary. Confirmed by section headers: linking the existing
objects with `-g` produces a `.debug_info` of 57 KB — all of it from
emscripten's own libraries — while recompiling the game with `-g` and linking
the same way produces 1.5 MB of `.debug_info` and 2.2 MB of `.debug_line`.

With real DWARF, the offsets in a trap become source locations:

```sh
$ node tools/headless_test.js build/katam-dbg.js "$ROM" 60
TRAP: Aborted(segmentation fault)
    at dwarf-safe.wasm            (…:wasm-function[4598]:0x2761e1)
    at dwarf-safe.wasm.TaskCreate (…:wasm-function[4293]:0x25b07d)
    at dwarf-safe.wasm.CreateLogo (…:wasm-function[3332]:0x1d2415)
    at dwarf-safe.wasm.sub_080001CC (…:wasm-function[2710]:0x14ceae)

$ ~/emsdk/upstream/bin/llvm-symbolizer --obj=build/katam-dbg.wasm 0x25b07d
TaskCreate
/home/agent-tom/Desktop/katam-port/build/port-src/src/task.c:122:31
$ … 0x1d2415  ->  CreateLogo   src/logo.c:36:10
$ … 0x14ceae  ->  sub_080001CC src/init.c:57:5
```

Column 31 of line 122 is `gNextTask->prev`. That is the faulting expression,
recovered from a trap that reported no address.

It works for the other trap message too. A synthetic module calling a function
pointer holding an ARM code address:

```
RuntimeError: table index is out of bounds
    at fp.wasm.callThrough (…:wasm-function[4]:0x299)
$ llvm-symbolizer --obj=fp.wasm 0x299  ->  callThrough  fp.c:10:5
```

`fp.c:10` is the indirect call. The trapping frame is the *caller*, so
`null function or function signature mismatch` symbolizes to the call site — the
question `docs/ARCHITECTURE.md` describes as unanswerable for function pointers
inside ROM structs.

**DWARF survives Asyncify.** This was the thing most likely to break: binaryen
rewrites every function for Asyncify, and the line numbers above are still
correct afterwards. emcc does warn
`running limited binaryen optimizations because DWARF info requested`, and the
measured price is 30.9 s against 28.9 s, about 7%.

### Ship it without shipping 7 MB

`-gseparate-dwarf=<file>` writes the debug sections to a side file and leaves an
`external_debug_info` section in the wasm pointing at it. Measured:

| | wasm | side file |
|---|---|---|
| `-O2 --profiling-funcs` | 2,452,962 | — |
| `-O2 -g` | 7.1 MB | — |
| `-O2 --profiling-funcs -gseparate-dwarf` | 2,551,716 | 7,345,950 |

**The code offsets are identical between the two files**, so a stack captured
from a shipped build symbolizes against the side file kept locally — checked by
symbolizing offsets taken from a run of the stripped module against the
separate `.debug.wasm` and getting `task.c:122:31` back. That makes it viable to
put a symbolizable build on the deployed page for the price of 100 KB, which is
the only way the tester's crashes on a phone become readable.

### Chrome DevTools C/C++ extension

The [C/C++ DevTools Support (DWARF) extension](https://developer.chrome.com/docs/devtools/wasm)
consumes exactly the DWARF produced above and gives source-level stepping,
breakpoints and variable inspection over `build/port-src/src/*.c`. Two things to
know before trying it:

- Chrome's documentation recommends `-gseparate-dwarf` for load time; the
  extension follows `external_debug_info`.
- The DWARF here records `DW_AT_comp_dir = /home/agent-tom/Desktop/katam-port`
  with *relative* names (`build/port-src/src/task.c`), so `make serve` from the
  repo root, or the extension's path substitution, is what makes sources resolve.

Not tested in a browser from this machine; the DWARF itself was validated with
`llvm-dwarfdump`, which is the input the extension reads.

### Source maps, as the lighter option

`-gsource-map` produces a 1.2 MB `.wasm.map` and leaves the wasm at 2.4 MB.
Two catches, both measured: on its own it **drops the `name` section** — the
`-gsource-map` build has no names at all, so pass `--profiling-funcs` as well —
and a source map carries line numbers only, no columns, no variables, and no
`llvm-symbolizer` support. It is strictly less than DWARF for a build this size,
and the size argument for it disappears once `-gseparate-dwarf` exists.

## 5. Safari and iOS

Cannot be tested from this machine; what follows separates what was measured
from what is documented.

The `original(...args)` in the tester's report is emscripten's Asyncify import
wrapper (`instrumentWasmImports`, `src/lib/libasync.js`). Checked: it is present
in **every** build, including `-O2` with `ASSERTIONS` off, so there is no build
setting that removes it. Safari is naming the innermost *JavaScript* frame it
has and nothing below it, and this matches a known JavaScriptCore limitation —
[wasm frames missing from stack traces in Safari 17.2 and mobile Safari](https://github.com/getsentry/sentry-javascript/issues/9968),
where the same error yields a full frame list in Chrome and Firefox.

Three things can be done about it, in order of cost:

1. **Symbolize whatever offsets do arrive.** `web/shell.html` already installs a
   `window.onerror` crash reporter that prints `err.stack`. If any wasm frame
   with an offset survives on the tester's device, the previous section turns it
   into a line number offline. This costs nothing beyond building with `-g`, so
   do it first and find out.

2. **Capture the stack from C, before the trap.** `emscripten_get_callstack()`
   asks the *engine* for a stack at a moment of your choosing, so it does not
   depend on the trap being catchable. Verified to work through Asyncify — it
   returns frames both before and after an `emscripten_sleep`, i.e. across a
   rewind:

   ```c
   char buf[4096];
   emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_JS_STACK, buf, sizeof buf);
   PortLog("%s", buf);
   ```

   `platform/checks.c` already knows the two moments worth capturing: a task
   pointer that leaves the map, and a destructor the game never installed. It
   currently prints the task; adding the callstack there gives the caller too.
   The quality of the result is still whatever Safari supplies — but combined
   with `-g` a single offset is enough.

3. **Keep instrumenting the C side.** `PortTaskStruct` is the model, and the
   reason it exists is sound: a check at the point where a pointer is *born*
   does not need a stack at all. Nothing off the shelf replaces it on a platform
   whose engine will not produce frames.

`Asyncify.exportCallStack` is maintained by the runtime in every build and holds
the exported wasm functions currently on the stack. It is coarse — exports only,
which for this port is roughly `main` — so it is not worth wiring into the crash
report.

## 6. Everything else

**`--emit-symbol-map`** writes `katam.js.symbols`, `index:name` per line
(`3790:TitleScreenMain`). It is the offline form of what `--profiling-funcs`
already embeds, and its use is to *drop* `--profiling-funcs` from the shipped
wasm and translate `wasm-function[3790]` afterwards. Since names cost little and
`-gseparate-dwarf` gives strictly more, this is only interesting if wasm size
becomes critical.

**Disassembling one function.** `llvm-objdump` reads the name section directly:

```sh
~/emsdk/upstream/bin/llvm-objdump -d --disassemble-symbols=TaskCreate build/katam.wasm
```

which prints `001fee69 <TaskCreate>:` and the body with byte offsets in the same
space as the offsets in a trap stack — so you can find the trapping instruction
even without DWARF. `wasm-dis` produces readable `.wat` for the whole module but
numbers its functions rather than naming them; prefer `llvm-objdump` for this.

**Wasmtime / wasmer.** Neither is installed, and neither can load this module:
it is an emscripten module with JS imports (the PPU present callback, Asyncify's
`js_sleep`), not a WASI one. Getting there means `-sSTANDALONE_WASM` and a
WASI-side replacement for `platform/main.c`, which is the same work as the
native build below without the mature tooling as a payoff.

**A native x86-64 build — evaluated, and blocked on pointer size.** The
attraction is real: run the game's C under a native debugger, get `SIGSEGV` with
a faulting address, and leave the holes in the GBA map unmapped so that a wild
`TaskGetStructPtr` result faults instead of quietly landing in the wrong
structure. The address space cooperates — `mmap(MAP_FIXED_NOREPLACE)` at
0x02000000 for 128 MB succeeds in an ordinary 64-bit PIE process on this machine
and the write goes through (`vm.mmap_min_addr` is 65536, well below).

What does not cooperate is `sizeof(void *)`. The port's fixed addresses come with
fixed *extents*: `gTasks` is at 0x030019F0 and `gTaskPtrs` at 0x03002560, 0xB70
bytes apart, and `MAX_TASK_NUM` is 0x80. `struct Task` holds two function
pointers, so it is 0x14 bytes with 4-byte pointers — 0x80 × 0x14 = 0xA00, fits —
and 0x20 bytes with 8-byte pointers — 0x80 × 0x20 = 0x1000, which runs 0x490
bytes into `gTaskPtrs`, whose own elements have also doubled. Repeat for 189
generated symbols and for every ROM structure holding a pointer. A 64-bit native
build corrupts the RAM map by construction.

So it has to be `-m32`, and this machine cannot do that: `gcc -m32` and
`clang -m32` both fail with `bits/libc-header-start.h: No such file or
directory` (no 32-bit libc headers), `valgrind` is not installed, and `sudo`
needs a password. Verdict: **not blocked in principle, blocked in practice.** If
it is ever wanted, the shape is a 32-bit build with an `mmap`-ed map, a native
`platform/main.c` (much simpler than the wasm one — nothing needs Asyncify when
blocking in `VBlankIntrWait` is allowed), and `-fsanitize=address` at i386, whose
shadow lives at 0x20000000 and leaves `[0x02000000, 0x0A000000)` alone. Weigh
that against the fact that UBSan already runs today and finds this class of bug.

## Next steps

1. **Add `-g` to `CFLAGS` for a debug object tree** and a target that links it
   with `-gseparate-dwarf`. It should not share `build/obj` with the release
   build — objects do not record their flags, which the `CHECK_POINTERS` comment
   already warns about:

   ```make
   build/obj-dbg/%.o: %.c
   	@mkdir -p $(dir $@)
   	@$(CC) $(CFLAGS) -g -c $< -o $@

   build/katam-dbg.js: $(DBG_OBJS)
   	$(CC) -O2 --profiling-funcs -gseparate-dwarf=build/katam-dbg.debug.wasm \
   	    -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2 \
   	    $(NODE_LDFLAGS) $(DBG_OBJS) -o $@
   ```

   and drop the `-O0 -g2` from the current rule, which does nothing.

2. **Add a `symbolize` helper**, because the offsets arrive as text:

   ```sh
   # tools/symbolize.sh — feed it a stack, get file:line back
   grep -o '0x[0-9a-f]*' | xargs -n1 \
       $(EMSDK)/upstream/bin/llvm-symbolizer --obj=build/katam-dbg.debug.wasm
   ```

3. **Add a `ubsan` target** building into `build/obj-ubsan` and running the
   headless test, and run it after each `make sync`. The list in section 2 is
   what one run produces; it is a to-do list.

4. **Fix `task.c:122`** (guard `gNextTask`) so `-sSAFE_HEAP=1` boots again, and
   keep `SAFE_HEAP=1` rather than `=2` wherever it is used.

5. **Consider shipping `-gseparate-dwarf`** on the deployed page. 100 KB buys
   the ability to read the tester's crash reports.


---

## 7. Comparing one build of the port against another

Added for the 64-bit work, and the only instrument that could answer the
question it was built for.

`PORT_STATE_TRACE=1` makes the port emit one line per frame holding the input
and an FNV-1a hash of each region of the emulated console:

    [trace] f=185 keys=011 ewram=E43334E7 iwram=... vram=D502AEBB pltt=... oam=... io=...

The point is that this port can be diffed against itself across ABIs, which an
emulator comparison cannot: **every build reserves the GBA map at the same true
addresses**, so EWRAM at 0x02000000 in the wasm build and EWRAM at 0x02000000
in the aarch64 build hold the same bytes for the same reasons. Run two builds
with the same input, diff the logs, and the first differing line is the frame
they stopped agreeing — and its columns say whether the *input* diverged (a
harness difference) or the *state* did, and which region.

Comparing screenshots cannot do this. It says the pictures differ, which is
where a divergence ended up rather than where it began, and it cannot tell
"the two harnesses pressed different buttons" from "the two builds computed
different things". That distinction was exactly what was missing when the
64-bit play path could not be attributed.

`PORT_STATE_DETAIL=<frame>` adds one line per 1 KiB block of EWRAM at that
frame, which turns "EWRAM differs" into an address, and an address into a
symbol via `katam.map`. That is how the divergence below was pinned to
`gMPlayTrack_0` in two steps.

### Two regions are not comparable, by design

- **`io`** holds the DMA register mirrors, and `platform/dma.c` writes the
  transfer's *host* source address into them. `DMA_FILL` passes `&tmp`, a stack
  address, so this region differs between two runs of the same binary — stack
  ASLR moves it. Nothing reads the mirrors back, so it is harmless, but it is
  also the one place an LP64 truncation is still live.
- **`iwram`** holds `gIntrTable`, whose entries are host function addresses: a
  wasm table index in one build and a code address in another.

Both are worth hashing anyway — knowing *which* region moved is the diagnosis —
but a difference in either is expected across builds and means nothing.

### What it found

Comparing wasm32 (ILP32) against x86-64 (LP64), same input, over 1400 frames:

| region | first divergence |
|---|---|
| ewram | frame 185 |
| vram | frame 407 |
| pltt, oam | frame 410 |

`PORT_STATE_DETAIL` put the frame-185 difference in one 1 KiB block, which
`katam.map` names `gMPlayTrack_0` — the sound engine's track array. So the
divergence begins in sound state and reaches the picture 222 frames later.

It is **not** an ABI difference, and the instrument shows why: within a single
binary, turning audio off alone moves EWRAM at frame 63. The music player's
state is a function of how the host drives the mixer, and the web and SDL hosts
drive it differently. Both builds are otherwise deterministic — two runs of the
native build agree in every game region across 1400 frames.

### Two corrections, and a limit of the method

**The sound divergence is not causal.** Hashing EWRAM per block at frames 200,
300, 380 and 400 shows *no* block differing at all: the frame-185 difference in
`gMPlayTrack_0` is transient and has healed by frame 200. The blocks that
diverge at 406 are a different set. So there are at least two independent
phenomena here and the first does not lead to the second -- which is what the
single-chain story assumed, on no evidence beyond having found it first.

**The frame-406 difference is a false positive**, and it exposes the method's
real limit. One word differs, at 0x02020F58: 0x0000041A in the wasm build and
0x1011D350 in the 64-bit one. The image is pinned at 0x10000000, so that is a
host code address; the wasm value is a function table index. 0x02020F58 is
offset 0x78 into `gKirbys`, which `gba_layout.h` names `struct Object2::unk78`
-- the void* the game stores *functions* in, the field
[SIXTYFOUR.md](SIXTYFOUR.md) flags as invisible to type analysis. Both builds
are correct. They simply spell a function pointer differently, because a
function pointer *is* a table index in WebAssembly and an address natively.

So: **any GBA memory holding a host function pointer is incomparable between
builds, in EWRAM as much as in IWRAM.** The regions listed above as
"not comparable" were incomplete -- it is not a property of a region, it is a
property of what the game happened to store there, and the game stores function
pointers in ordinary object fields. Every "first divergence" frame this
instrument reports is therefore an *upper bound* that may be contaminated.

### Looking at the picture, which should have come first

The symbol-normalising comparator turned out not to be the next thing needed.
Putting the two 1400-frame screenshots side by side answers the question in one
glance: **a whole background layer is wrong in the 64-bit build.** The sky and
mountains are replaced by an orange dotted fill and a band of garbled tiles,
while the clouds, the pillars, every sprite and the entire HUD are pixel
correct.

That is not a subtle numerical divergence, and it is not the sound engine. It
is one BG layer whose tiles never arrived -- which is exactly what the port's
own diagnostic had been saying all along, once and non-fatally, on this path
and no other:

    [katam-port] DMA leaves the map: src=0x0877A4EC dest=0x00000000  <-- bad
                 count=96 unit=2 flags=0x8000

A refused tile upload with a null destination. The port declines the transfer
rather than following it, the tiles stay whatever they were, and the layer
renders as garbage. The 32-bit build reports no diagnostics on the same path.

The trail from there: `sub_081525DC` drains the tile-upload queue at
`gUnk_03002EC0` and copies `current->unk0` to `current->unk4`, and the entry it
picked up has a length but a null destination. `bg.c` has the only two sites
that write that field, and a breakpoint on each of them conditioned on
`tilesVram == 0` never fires across the whole 1400-frame run. So the entry is
not being enqueued with a null destination -- it is being *read* when it should
not be, or read from the wrong index. That is where this stops.

**Look at the output before building the next instrument.** Three rounds of
hashing memory produced a transient sound difference that healed, and a
function-pointer spelling difference that was never a difference at all.
Fifteen seconds with the two images and a pixel diff named the subsystem.

### The DMA transaction log, and where the trail is now

`PORT_DMA_TRACE=1` logs every transfer in order -- channel, source,
destination, count, unit, control.  It watches the data movement instead of the
memory afterwards, so a tile upload that never happened is a *missing line*
rather than a hash that stopped matching four hundred frames later.  Normalise
host addresses out first (DmaFill's source is `&tmp`), then diff.

It found two real bugs immediately, both the same shape -- a `sizeof` over a
function-pointer typedef that the narrowing had not reached, so the count
doubled on a 64-bit host:

    dest=gHBlankIntrs   count 4 -> 8    read 16 bytes past gHBlankCallbacks,
                                        which is where gCurTask lives
    dest=gUnk_030068C0  count 1 -> 2    wrote past it into gInputPlaybackData

Fixing them removed the port's refused-DMA diagnostic entirely.  It did **not**
fix the corrupt background layer -- the same 20999 pixels, byte for byte.  A
genuine bug sitting next to the symptom was not the cause of it.

**Where the trail is now.** From transfer 27263 the two builds fill the same
VRAM addresses with the same counts from completely different places:

    wasm32   src=08AE7FE4  ROM,   stride 0x5A  (90 bytes)
    LP64     src=02028EE0  EWRAM, stride 0x130 (304 bytes)

Same destination (0x0600F800, +0x40 per row), same count (32 halfwords).  One
build is reading tile data straight out of the ROM and the other out of a
buffer in EWRAM, stepping a different distance between rows.  That is not a
corrupted pointer -- both sources are valid, mapped, plausible addresses.  It
is the game taking a *different branch*, which means the divergence that
matters happened before this and is upstream of the graphics code entirely.

The next step is to bisect backwards from transfer 27263 with the state trace,
looking for the last frame at which every comparable region still agrees, and
then to find the decision between there and here.  The instruments for both
halves exist; the work is running them.: instead of
hashing raw words, resolve any word that falls inside the host image to a
symbol name and hash the name instead of the address. Both builds would then
agree that a field holds `sub_0800C124` rather than disagreeing about how it is
spelled. The port already has most of the machinery -- `tools/gen_rom_data.py`
resolves ARM addresses to decompiled C names, and `nm` gives the native side.
That is the next instrument and it is not built.

**What would settle it completely** is an ILP32 *native* build: same host layer,
same audio driver, only the ABI different. That control does not exist on this
machine. A 32-bit toolchain is easy (the multilib packages unpack like any
other sysroot — see NATIVE.md), but SDL2 has to be built from source for i686
and its CMake thread detection does not survive a hand-assembled 32-bit
sysroot. Until that exists, the attribution above rests on the audio-on/off
experiment rather than on a direct comparison.
