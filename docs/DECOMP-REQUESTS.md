# Notes from the port side, back to the decompilation

_A copy of `~/Desktop/katam-port-requests.md`, the document handed to the
decompilation effort. Kept here so the asks are versioned alongside the code._

Written 2026-08-04 by the session that built the WebAssembly port. **This
supersedes the previous version of this file in full** — enough changed that
appending would have left stale asks standing. In particular, the old §1
("`sub_08155128` is the one thing blocking everything") is gone because it is
done.

This is the return direction of `katam-port-notes.md`: what the port learned by
actually compiling and running the tree, what it needs next, and what it does
not need. Everything below was measured against the working tree or read out of
`baserom.gba`, not estimated. Nothing here asks the decompilation to change how
it works — the port adapts sources at build time and never edits your checkout.

Port repo: `~/Desktop/katam-port` → `sh1ftmaker/katam-port` (private).

---

## 0. Thank you — `sub_08155128` landed and it did exactly what was hoped

The #1 ask was delivered and it worked. `wip/sprite.c` now carries a C body for
`sub_08155128` (plus 14 more — 15 of 27 `sprite.s` functions have C), and the
effect on the port was immediate and total:

| Before | After |
|---|---|
| backgrounds only, every menu an empty frame | OAM populated, sprites draw |
| title screen: sky, rainbow, logo, copyright | **plus "PRESS START"** |
| file-select: blank | **entries, cursor, the four Kirbys, item sprites** |
| never left the menu | **reaches a level** |

Concretely, what it unblocked: with sprites the port can get *through* the
file-select menu, so `CreateLevelObjects` now runs, objects spawn
(`CreateSmallSwitch` among others), and Kirby's own state machine executes —
the current crash trap lands inside `kirbyFlyUp`, which is a sentence that was
not possible to write a day ago. It still dies partway into gameplay, on the
problem class in §3, not on a missing body.

The documented return protocol (`0` = animation stopped, skip OAM emission this
frame) was used as given and is correct.

### Where the port is now

| | |
|---|---|
| Game sources compiled | 190 |
| Stubbed symbols (`make stubs`) | **25** (was 28) |
| ROM function-table entries wired to real C | **312 of 314**, across 8 tables |
| Function pointers patched inside ROM structs | **193 of 219** |
| Linker-placed RAM symbols reconstructed | 183 |
| ROM data symbols resolved to addresses | 162 |
| Functions with no C body that the port reaches | **2** |

### Re-measuring port impact yourself

```bash
cd ~/Desktop/katam-port
make sync && make stubs   # prints exactly what is still ARM-only, re-derived
                          # from a real link -- the list shrinks by itself as
                          # you decompile
make test                 # boots under node with a ROM, writes build/frame.ppm
```

`make stubs` is still the honest scoreboard. Its output ends with
`... N stubbed symbols`; N is 25 today, and `build/undefined.txt` is the list.

---

## 1. The remaining port blockers, ranked

**Only two functions in the whole game still have no C body, and the port
reaches both.**

| Function | Why the port reaches it |
|---|---|
| `sub_08038010` | called twice from `sub_08037314` |
| `sub_08038B34` | Kirby update path |

Then four `sprite.s` functions the port still stubs, in no particular order
because none of them is currently the thing that kills it:

`sub_0815436C`, `sub_081548A8`, `sub_08154B14`, `sub_08155604`.

That is the entire "missing body" list. `build/generated/stubs.c` is the
ground truth for it; everything else in that file is m4a, MultiSio/MultiBoot
and two data symbols, all deliberate (§7).

### `sub_08037314` — warning received and respected

Understood that it is deliberately incomplete: the outer sweep and block 1 of
six collision blocks are written, blocks 2–6 are not. The port **does** reach
it, so it is running with partial collision resolution, and it is recorded on
the port side as a **known-incomplete site, not as working code** — exactly as
asked. If gameplay behaviour looks wrong near collisions, this is the first
place the port will look, and it will not be reported to you as a bug.

Filling blocks 2–6 is therefore a real port ask, not just a tidiness one, but
it ranks below the two bodyless functions above.

---

## 2. The general problem class, stated plainly

Because it is now the main thing between the port and playing, and because it
is invisible from your side — it costs nothing on hardware.

A function pointer stored in ROM is an **ARM code address**. In WebAssembly a
function pointer is an **index into the module's function table**, so such a
value is not merely wrong, it is out of range: calling one ends the program
instantly.

The port handles this by resolving each address back through the GBA build's
`katam.map` and emitting a real reference to the decompiled C function. Three
shapes are covered automatically:

| Shape | Example | State |
|---|---|---|
| flat function-pointer arrays | `gSpawnFuncTable1` | 312 of 314 entries wired across 8 tables |
| arrays declared through a function-pointer typedef | `gUnk_082EB7D0` | covered |
| function pointers *inside* ROM structs | `gUnk_08351648` — 219 object descriptors with a constructor at +0x10, patched in place at startup | 193 of 219 resolve |

**The shapes it does NOT yet cover are what still crashes it partway into
gameplay.** Each new one is individually fixable by the same mechanism, and
each costs a session to find, because the symptom is always the same: a trap at
an indirect call with no build-time warning.

**The single most useful thing the decomp could do here**, whenever data
conversion is on the table, is convert ROM function-pointer tables from
`.incbin` into typed C data in `data/`. That removes this entire class — not
one table at a time, the class. No urgency; the current mechanism works. But if
data conversion is ever on the menu, function-pointer tables are the
highest-value target by a wide margin.

The two entries that still cannot be wired, by name:

```
gSpawnFuncTable1[114]  CreateBossChallengeDoor   (boss_challenge_door.s, claimed)
gUnk_0834BD94[9]       sub_0801DFE8              (see §3b -- it is static)
```

---

## 3. Two table-boundary findings

### 3a. `gUnk_0834BD88` — the label may be wrong, and this one is open

`data/data_6.s:1188` declares it `.incbin "baserom.gba", 0x34BD88, 0x000000C`
— **3 entries**. The game indexes it well past 3: `wip/code_08032E98.c:2224-25`
does

```c
res = gUnk_0834BD88[obj->unk0](other);
if ((u16)gUnk_0834BD88[other->unk0](obj) != 0)
```

On hardware the reads simply continue into the words of the next label and keep
finding valid function pointers, so nothing is wrong at runtime. In C those
became two separate arrays and the index ran off the end of a 3-element one
into unrelated memory — which is what was crashing the port here.

The port now walks each table forward past its label for as long as the
following words still resolve to known functions. `gUnk_0834BD88` comes out at
**12 entries**, all 12 resolving to decompiled C:

```
 0 sub_080364E4   1 sub_080365C8   2 sub_0803699C   3 CreatePauseMenu
 4 sub_0801D618   5 sub_0801D624   6 sub_0801D630   7 sub_0801D63C
 8 sub_0801D648   9 sub_0801D654  10 sub_0801D660  11 sub_0801D66C
```

**Two things you should know before treating 12 as the answer.**

The labels are contiguous: `0x34BD88 + 0xC == 0x34BD94`, so words 3 onward of
`gUnk_0834BD88` *are* `gUnk_0834BD94`, word for word. And the walk stopped at
12 for a reason that is not "the table ended" — word 12 is `0x0801DFE9`, i.e.
`sub_0801DFE8`, which is `static` in `src/code_0801DA58.c:204` and therefore
never appears in `katam.map`. The port cannot see it, so the walk halts.

So **12 is a lower bound, not a measurement of the table's extent.** If the
table genuinely continues it runs to at most 30 entries (3 + the 27 of
`gUnk_0834BD94`), because word 30 is where the function pointers stop (§3b).

The question for you: is the `0xC` label boundary wrong, or is the real table
12 (or 30) entries with the label merely marking where a name got assigned? The
answer needs the range of `ObjectBase::unk0`, which the port cannot bound.
Whatever the answer, 12 is what the port runs today and it is stable.

### 3b. `gUnk_0834BD94` — resolved, and the fix is confirmed from the ROM

The previous version of this file asked for this label to be split. It has
been, in `aa70e1e`, and the split is **verified correct against the ROM
bytes**, not just accepted:

- `data/data_6.s:1191` is now `.incbin ..., 0x34BD94, 0x000006C` = 27 words.
- All 27 of those words have the Thumb bit set and 26 resolve through
  `katam.map`; the 27th is the `static` `sub_0801DFE8` above.
- The next word is at `0x34BE00` and is `0x04200400` — not a pointer. The full
  eight words now under the new `gUnk_0834BE00` label are
  `04200400 0C210421 0C630C61 1CE31C63 3CE71CE7 3DDF3DD7 7FDF7DDF FFFF7FFF`.

The boundary is exactly 27. The port stopped generating the eight nonsense
dispatch entries. Nothing further needed here.

One small residual ask: `sub_0801DFE8` being `static` makes
`gUnk_0834BD94[9]` unreachable for the port — it is one of the two entries in
the whole build that gets a reporting stub instead of a real function. It is
also the thing blocking a longer answer in §3a. If it turns out `static` is not
load-bearing for the match there, dropping it would close both.

---

## 4. The anim-table attribution question — settled from the ROM bytes

You asked, correctly, that this be settled from the bytes rather than from
either side's prose. It was: 48 bytes read at each address out of
`baserom.gba`, every word resolved through `katam.map`.

**`gUnk_08D5FDE4`** (`0x08D5FDE4`, `0x30` bytes, 12 entries):

```
 0 AnimCmd_GetTiles_2    1 AnimCmd_GetPalette_2   2 sub_08154E18
 3 sub_08154E24          4 sub_08154E34           5 AnimCmd_6_2
 6 sub_08154E48          7 sub_08154E64           8 sub_08154E70
 9 sub_08154E88         10 sub_08154E90          11 sub_08154E9C
```

**`gUnk_08D6081C`** (`0x08D6081C`, `0x30` bytes, 12 entries):

```
 0 sub_08155370          1 sub_08155400           2 AnimCmd_JumpBack
 3 AnimCmd_4             4 AnimCmd_PlaySoundEffect 5 sub_08155494
 6 AnimCmd_TranslateSprite 7 AnimCmd_8            8 AnimCmd_SetIdAndVariant
 9 AnimCmd_10           10 AnimCmd_SetPriority   11 AnimCmd_12
```

Findings:

- **The two sets are completely disjoint.** No handler appears in both.
- All 24 words had the Thumb bit set; all 24 resolved to exactly one map
  symbol each. No ambiguity anywhere in the two tables.
- Both dispatch sites you cited do drive `gUnk_08D6081C`: `asm/sprite.s:1912`
  (`ldr r6, _08155218 @ =gUnk_08D6081C`, inside `sub_08155128`) and
  `asm/sprite.s:2033` (`ldr r0, _081552D8 @ =gUnk_08D6081C`, inside
  `sub_0815521C`).
- The string `08D5FDE4` **does not occur in `asm/sprite.s` at all**.
  `gUnk_08D5FDE4` is driven by the already-decompiled `sub_08153D78` and
  `sub_08153E6C` in `src/sprite_1.c` (lines 31 and 71).

So the port's original attribution was right, and `wip/sprite.c`'s own
annotations (`gUnk_08D6081C[0..12]`) are right. **Both tables are now 12/12
wired in the port.** Nothing to change on either side; recorded so nobody
re-derives it.

---

## 5. `gUnk_0834C120` — five entries, all wired

Your correction was right and the fade family landed. All five entries are live
in the port: `sub_0803A450`, `sub_0803AFE8`, `sub_0803B788`, `sub_0803BF68`,
`sub_0803C748`. The table that was entirely dead in the previous version of
this file is now entirely alive. Nothing outstanding.

---

## 6. Defects reported last time — three fixed, two still open

`aa70e1e` closed three of the six, byte-identically, and the port dropped its
build-time workarounds for all three. Thank you; the ones that remain:

1. **The three uninitialised variables.** `src/dark_mind.c:8664`,
   `src/multi_08030C94.c:241` and `:250` — still `ASM_OUT_R(v)`. The port
   zeroes them, which is deterministic but is a guess. Still needs someone who
   knows the intended behaviour. This is the only open item from the old list
   that is a genuine correctness risk.

2. **`inline` without `static`.** The tree relies on gnu89 semantics, where a
   plain `inline` definition also emits an external one; C99 inverted that, so
   a modern compiler turns every one into an undefined symbol. The port passes
   `-fgnu89-inline` and is fine. **Not asking for a change** — recorded because
   it is a portability landmine that looks like nothing, and the next consumer
   of this tree will lose an hour to it.

Also still true, also **not** a request: `gLevelObjLists` is an array of an
anonymous `transparent_union` (`include/data.h:847-849`), and `src/kirby.c:5339`
declares `gUnk_08D60FB4` and `gUnk_08D60FDC` in one `extern` with two
declarators. Both are legal and both needed special handling in the port's
header parser. Noted only so the sharp edges are known if the headers are ever
tidied.

On the "flag load-bearing return values" convention you adopted: it is already
paying for itself — `sub_08155128`'s documented return is why sprites worked
first try rather than after a hunt. The port keeps its side in
`tools/stub_returns.conf`.

---

## 7. What the port does *not* need

Unchanged and still true, so no effort goes into things that will not help:

- **The sound engine.** `m4a_asm.s` and friends are replaced wholesale by
  no-op stubs. The port drops `src/m4a.c` and `src/m4a_tables.c` from its build
  entirely. Silence is a deliberate choice, not a gap.
- **Link cable.** `multi_sio_asm.s`, `multi_boot.c`, `sio32_multi_load.c` are
  stubbed. Single-player does not touch them.
- **`agb_sram.c`.** Replaced, and it cannot be otherwise: it copies the machine
  code of its own inner loop into a stack buffer and calls it, which is not
  expressible in WebAssembly. Not worth any decomp effort on the port's
  account.
- **Byte-matching, in general.** The port needs *correct C*, nothing more. A
  function sitting at DIFF is 100% usable. The 3 asm wrappers still in
  `wip/large_star_stone_block.c` are already fine, because their real C is in
  the `#else` branch and the port builds with `-DNONMATCHING`.

**The port's gate is "has a C body", not "matches".** Of the two numbers you
track, the one that predicts port progress is the bodyless count — and it is
now 2.

---

## 8. Summary: the ask, in order

1. **`sub_08038010`** and **`sub_08038B34`** — the last two functions in the
   game with no C body, and the port reaches both. Correct C is enough.
2. **`sub_08037314` blocks 2–6.** The port runs this function today with
   partial collision resolution, knowingly.
3. **`gUnk_0834BD88`'s real extent** (§3a) — is the `0xC` label wrong, or is
   the table 12, or 30? The port runs 12 and is stable either way; the answer
   would let it stop guessing.
4. **The four remaining `sprite.s` stubs**: `sub_0815436C`, `sub_081548A8`,
   `sub_08154B14`, `sub_08155604`.
5. **The three uninitialised variables** (§6.1) — the only open correctness
   risk from the previous list.
6. **Whenever data conversion is on the table: ROM function-pointer tables
   into typed C** (§2). Not urgent, highest long-term value, removes a whole
   class of port failure rather than an instance of it.
