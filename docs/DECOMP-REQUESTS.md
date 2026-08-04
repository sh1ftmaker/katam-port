# Notes from the port side, back to the decompilation

_A copy of `~/Desktop/katam-port-requests.md`, the document handed to the
decompilation effort. Kept here so the asks are versioned alongside the code._

Written 2026-08-04 by the session that built the WebAssembly port. This is the
return direction of `katam-port-notes.md`: what the port learned by actually
compiling and running the tree, what it needs next, and what it does not need.

Everything below was measured against the working tree, not estimated. Nothing
here asks the decompilation to change how it works — the port adapts sources at
build time and never edits your checkout.

Port repo: `~/Desktop/katam-port` → `sh1ftmaker/katam-port` (private).

---

## 0. The port exists and runs

It boots the game's own `AgbMain` / `GameLoop` unmodified and gets to the
file-select menu. Verified headlessly against a real ROM, not by inspection:

| Frame | State |
|---|---|
| ~60 | first VRAM uploads, `DISPCNT` = mode 0, 4 BGs + OBJ enabled |
| ~1200 | full title screen: sky, rainbow, logo, copyright line |
| 1250 | Start pressed |
| ~1900 | file-select menu, "CHOOSE A FILE TO PLAY" |

2000 frames, no crash. `gFrameCount` advances in step and `TasksExec` runs
every frame. 189 of 189 game `.c` files compile.

**One thing is missing and it is the whole visible game: sprites.** More below.

### Re-measuring port impact yourself

```bash
cd ~/Desktop/katam-port
make sync && make stubs   # prints exactly what is still ARM-only, re-derived
                          # from a real link -- the list shrinks by itself as
                          # you decompile
make test                 # boots under node with a ROM, writes build/frame.ppm
```

`make stubs` is the honest scoreboard: 28 symbols today.

---

## 1. The one thing that matters most

### `sub_08155128` — 521 references, no C body

Every object in the game reaches OAM through it. While it is stubbed, **OAM is
never populated and nothing but backgrounds draws** — no Kirby, no enemies, no
menu cursors, no file boxes. The port's sprite renderer is written, tested and
idle, waiting for this one function.

It is the single most-referenced function in the codebase and it is still ARM
assembly in `asm/sprite.s` (~1,950 instructions).

I know `sprite.s` is claimed by another contributor and I have not touched it.
Two things worth saying anyway:

- **The port does not need a match.** A body in a `#else` / NONMATCHING branch
  is fully usable. If the matching effort on that file is slow, correct C that
  nobody claims is a match would unblock the entire visual game.
- The rest of that file matters much less. Ranked by references:
  `sub_0815521C` (10), `sub_08155604` (7), `sub_08154FE8` (7), `sub_081548A8`
  (4), `sub_08154B14` (2), `sub_0815436C` (2).

---

## 2. Bodyless functions the port actually reaches

Of the 12 remaining bodyless functions, these are the ones the running port hits
or would hit. The other five are not reachable from anything the port exercises
yet, so they are lower value *for the port* — matching priorities may differ.

| Function | Why it matters | Refs |
|---|---|---|
| `sub_08035788` | called from the Kirby update path in `code_08032E98.c` | 3 |
| `sub_08037314` | same file, called next to the above | 2 |
| `sub_08038B34` | same | 2 |
| `sub_0803A450` | **`gUnk_0834C120[0]`** | 0 direct |
| `sub_0803AFE8` | **`gUnk_0834C120[1]`** | 0 direct |
| `sub_0803B788` | **`gUnk_0834C120[2]`** | 0 direct |
| `sub_0803BF68` | **`gUnk_0834C120[3]`** | 0 direct |

The bottom four look like nothing by reference count and are not: they are four
of the five entries of the ROM dispatch table `gUnk_0834C120`, so **the entire
table is dead** in the port. A grep for callers will not show this, which is
exactly why it is worth flagging. If you want one cluster with outsized effect
on the port after sprites, that is it.

---

## 3. Function-pointer tables in ROM — a class of thing worth knowing about

This was the difference between "draws the title screen" and "reaches the menu",
and it is invisible from the decompilation side because it costs nothing on
hardware.

The game dispatches through tables of function pointers that live in ROM. Those
tables hold **ARM code addresses**. In WebAssembly a function pointer is an
index into the module's function table, so `0x0802FE84` is not merely wrong, it
is out of bounds — calling one takes the program down instantly. That is what
killed the port the first time Start was pressed.

The port now rebuilds each table: every entry is looked up in `katam.map`,
turned back into the name of the function at that address, and emitted as a real
reference to the decompiled C function. **284 of 310 entries resolve.** The
rest get a signature-correct stub that reports itself.

Two consequences for you:

**a. Entries that cannot be wired, by name.** These are the functions that
appear in a ROM dispatch table and have no C the port can point at:

```
gSpawnFuncTable1[114]  CreateBossChallengeDoor        (boss_challenge_door.s)
gUnk_0834C120[0..3]    sub_0803A450 sub_0803AFE8 sub_0803B788 sub_0803BF68
gUnk_08D5FDE4[2..11]   sub_08154E18 sub_08154E24 sub_08154E34 sub_08154E48
                       sub_08154E64 sub_08154E70 sub_08154E88 sub_08154E90
                       sub_08154E9C                 (sprite.s, anim commands)
gUnk_08D6081C[0,1,5]   sub_08155370 sub_08155400 sub_08155494   (sprite.s)
```

`gSpawnFuncTable1` is otherwise complete — 218 of 219 object constructors
resolve, which is why objects spawn at all.

**b. A data-typing note.** `gUnk_0834BD94` is declared as one label but is not
one thing: the first 27 words are function pointers and the remaining 32 bytes
(`0x04200400`, `0x0C210420`, `0x0C630C60`, … `0xFFFF7FFE`) are clearly some
other data that the `.incbin` happens to cover. If that boundary is ever worth
splitting into two labels, the port would stop generating eight nonsense table
entries — and the decomp would be more accurate.

**The long-term fix is yours, not mine.** Every one of these tables that gets
converted from `.incbin` into typed C data in `data/` removes the port's need
to reconstruct it from `katam.map` plus a ROM at build time. No urgency — the
current mechanism works — but if data conversion is ever on the menu, function
pointer tables are the highest-value target.

---

## 4. Things the port found in the tree that look like real defects

These are not port problems. They are things that are harmless under agbcc and
would bite anyone else, so they may be worth fixing upstream regardless of the
port.

1. **Prototypes that disagree with their definitions.**
   `src/code_0802E57C.c:27-28` declares
   ```c
   void sub_0802F8D8(struct Unk_0802E57C *, u16, u16, u32, s32, s32, s16, s16, u16);
   void sub_0802FA40(struct Unk_0802E57C *, u16, u16, u32, s32, s32, s16, s16, u16);
   ```
   but both are defined in `src/code_0802F8D8.c` returning
   `struct Unk_0802F8D8 *`. On ARM the return value sits in `r0` and the caller
   ignores it, so nothing breaks. WebAssembly validates signatures, and a
   mismatch links to a stub that traps when called. The port patches this at
   build time; fixing the declaration would be strictly better.

2. **`static` definitions of symbols the headers export.**
   `src/sparky.c` defines `gUnk_08355578` and `gUnk_08355584` as `static` while
   `include/data.h` declares them `extern`. gcc 2.95 accepts it; clang rejects
   it outright.

3. **A NONMATCHING branch that does not compile.**
   `src/cookin.c`'s `#else` branch calls `ObjectSetFunc(obj, ...)` where `obj`
   is declared only in the matching branch. Nothing in the decomp workflow ever
   builds those branches, so they rot silently. Worth knowing, since the port
   builds with `-DNONMATCHING` and so is the only consumer of that code.

4. **Declarations that no tool can parse.**
   `gLevelObjLists` is an array of an anonymous `transparent_union` spread over
   four lines; `gUnk_08D60FB4` and `gUnk_08D60FDC` share one `extern` with two
   declarators. Both are legal and both required special handling. Not asking
   for a change — just noting where the sharp edges are if the headers are ever
   tidied.

5. **`inline` without `static`.** The tree relies on gnu89 semantics, where a
   plain `inline` definition also emits an external one. C99 inverted that rule,
   so a modern compiler turns every one of these into an undefined symbol. The
   port passes `-fgnu89-inline`. Mentioning it because it is a genuine
   portability landmine that looks like nothing.

6. **The three uninitialised variables are still open.** `src/dark_mind.c:8664`,
   `src/multi_08030C94.c:241` and `:250` — the `ASM_OUT_R(v)` sites. The port
   currently zeroes them, which is deterministic but is a guess. This still
   needs someone who knows the intended behaviour, exactly as your notes say.

---

## 5. What the port does *not* need

So no effort goes into things that will not help it:

- **The sound engine.** `m4a_asm.s` and friends are replaced wholesale by
  no-op stubs. The port drops `src/m4a.c` and `src/m4a_tables.c` from its build
  entirely. Silence is a deliberate choice, not a gap.
- **Link cable.** `multi_sio_asm.s`, `multi_boot.c`, `sio32_multi_load.c` are
  stubbed. Single-player does not touch them.
- **`agb_sram.c`.** Replaced, and it cannot be otherwise: it copies the machine
  code of its own inner loop into a stack buffer and calls it, which is not
  expressible in WebAssembly. Not worth any decomp effort on the port's account.
- **Byte-matching, in general.** The port needs *correct C*, nothing more. A
  function sitting at DIFF is 100% usable. The 3 asm wrappers left in
  `large_star_stone_block.c` are already fine for the port, because their real C
  is in the `#else` branch.

The framing in §1 of your notes is exactly right and worth restating from this
side: **the port's gate is "has a C body", not "matches"**. Of the two numbers
you track, the one that predicts port progress is the 12 — and, within it, the
seven functions listed in §2.

---

## 6. One trap the port hit that says something about the tree

A stub that returned `0` hung the game silently, and it took a while to find
because it looks exactly like a rendering bug.

`src/main.c` drains the VRAM transfer queue through a table of four workers:

```c
gUnk_030035D4 = 0xff;
for (; j <= 3; j++) {
    if (gUnk_08D5FDD4[j]() == 0) {
        gUnk_030035D4 = j;
        break;
    }
}
```

A `0` means "this worker did not finish". `GameLoop` then stops calling
`TasksExec()` until the queue drains. Two of those four workers live in
`sprite.s`, so the port's stubs returned `0` forever, the game ran its VBlank
handler for eternity, drew nothing, and looked completely healthy from the
outside — `gFrameCount` climbing, no crash, blank screen.

Worth knowing because the same shape recurs: any stubbed function whose return
value is read as a state can wedge the game rather than degrade it. The port
records these in `tools/stub_returns.conf`. If you decompile a function and
discover its return value is load-bearing in this way, that is useful to say so
in a comment — it is invisible at the call site.

---

## 7. Summary: the ask, in order

1. **`sub_08155128`** — nothing visible works without it. Correct C is enough;
   it does not need to match.
2. **`sub_0803A450`, `sub_0803AFE8`, `sub_0803B788`, `sub_0803BF68`** — four
   entries of `gUnk_0834C120`; the whole table is dead without them, and no
   reference count reveals that.
3. **`sub_08035788`, `sub_08037314`, `sub_08038B34`** — reached from the Kirby
   update path.
4. The rest of `sprite.s`, in the reference order given in §1.
5. The defects in §4, whenever convenient — they cost the decomp nothing and
   they cost every other consumer of the tree something.
