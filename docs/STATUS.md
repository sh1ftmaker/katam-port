# Status

Measured 2026-08-05 against the decompilation at `all-work`, not estimated.
Every number below came from a run on that date; where a claim is not measured
it says so.

## Where it gets to

It plays. A level runs, Kirby is controllable, the AI Kirbys are on screen, and
a warp star at the end of a level changes to the next one.

Measured two ways. `tools/native_smoke.sh` drives 1400 frames tapping A and
Right (`--mash 120:17:20`) and is in a level by frame 600, still in one at
frame 3600, with **44 distinct pictures among 47 samples** -- so it is moving,
not a frozen picture that happens to be drawn. And a player got to a warp star
and through the level change on the published build, which is the first time
the port has been driven that far by hand.

| Measured | Value |
|---|---|
| frames to a running level | ~600 with A+Right tapped on a 20-frame period |
| motion over 1400 frames | 44 distinct pictures in 47 samples |
| audio | 3600 blocks, 60.00s of audio for 60.00s of game, RMS -22.2 dBFS |
| save | survives the tab closing (IndexedDB) and a native restart (a .sav file) |
| diagnostics in a 600-frame run | 0 |
| DMA transfers that leave the GBA map | 1497 in 1400 frames, both shapes understood -- see below |

## What is missing, in the order it matters

### 1. A second player

The link lobby's handshake is finished: recognition, the parent's sequence
counter, the `0x70AE` settle and the `0xE4E4` start all complete, and the game
leaves its own lobby into a session. What it leaves into is a *synthetic* peer
-- `platform/mp_peer_lobby.c` answers the protocol faithfully and has no game
in it -- so the session does not survive. The remaining work is a real far end,
which means choosing a transport; nothing in the port has to change to accept
one. docs/MULTIPLAYER.md has the transcript and the state machine.

The rollback engine underneath it (snapshots, timeline, join/leave,
renumbering, RLE'd input logs) is written and self-tests clean, but nothing has
ever driven it over a wire -- predict, mispredict and restore are exercised
only by the self-test's own replay.

### 2. Function pointers stored in ROM

A function pointer in ROM is an ARM code address; in WebAssembly it is a table
index, so such a value is not merely wrong but out of range. The build resolves
them against the GBA build's `katam.map`.

| Shape | State |
|---|---|
| flat function-pointer arrays and typedef'd ones | 304 of 305 entries wired across 8 tables, 1 stubbed |
| function pointers inside ROM structs (`gUnk_08351648`) | 8 object descriptors still spawn without their per-type setup |

Two of those eight point at address 0 -- there is no function there at all --
and the rest are still ARM assembly upstream.

### 3. Functions with no C body

19 of them, listed by `make sync`. None is reached in the smoke run; each is
reachable in principle. `sub_08038010` and `sub_08038B34` are the two the port
has actually reached in the past, and `sub_08037314`, which calls the first
twice, is documented upstream as deliberately incomplete -- five of its six
collision blocks are unwritten -- so collision resolution is partial by
upstream's own account, not by the port's.

### 4. One structure whose documented size the tree does not have

`AnimCmd_SetIdAndVariant`: documented `0xC`, built 8. Not a size-boundary case,
so `make size-check` lists it as a known exception with its reasoning rather
than skipping it. Its own offset comments put `variant` at `0x08` where the
member list puts it at `0x06` -- a missing pad. Fixing it moves a member and
changes how every animation command is read, so it wants establishing against
the ROM first.

### 5. Sound, saving, the rest of sprite.s

Done, and listed here only because earlier revisions of this document said they
were not. Audio is produced by `platform/m4a_mixer.c`; saves persist on both
hosts; the four `sprite.s` functions this document used to list as stubbed all
have bodies.

## Build facts

| | |
|---|---|
| Game sources compiled | 191 |
| Functions with no C body anywhere | 19 |
| Linker-placed RAM symbols reconstructed | 183 |
| ROM data labels seen / referenced and resolved | 28592 / 183 (+14 from `katam.map`) |
| ROM function-table entries wired to real C | 304 of 305, across 8 tables |
| Function pointers patched inside ROM structs | 211 of 219 |
| INCBIN assets pasted in from the decomp | 143 files, 931 KiB |
| Output | 2.80 MB wasm |

## The "DMA leaves the map" reports

Both shapes were tracked to their source under a debugger, and **neither is a
port bug**. Recorded here so the next person does not spend the afternoon
again.

**Bad source, `dest` in VRAM — the tilemap blitter.** `sub_08153184` walks a
`struct Background` whose descriptor was filled by
`CpuFill16(0xFFFF, &levelInfo->unk180[2], ...)` — a room with no second object
layer. Read out of the live process at the moment of the transfer:
`unk10 = 0xFFFFFFFF`, `unk14 = 65535`. The address arithmetic then wanders off
the map, and the reported sources differ by exactly `unk14 * 2` — the blitter's
own row stride, which is what confirms it rather than merely fitting it. On
hardware this reads open bus into a screen block for a disabled layer.

**Destination 0 — a sprite with no VRAM.** `sub_08155370`, the sprite `GetTiles`
animation command, enqueues `sprite->tilesVram` and that is 0. The game sets it
to 0 deliberately in five places (`intro.c`, `title_screen.c`). Address 0 is
BIOS ROM, so the hardware discards the write. Caught with a hardware watchpoint
on the queue entry, which named the writer directly — the queue is drained a
frame later, so the stack at the transfer only ever names the drain.

**What was a real defect is the reporting.** The cap was five transfers total,
with a comment saying it existed "so a genuinely new one is still visible". It
did the opposite: those two shapes account for exactly five in an ordinary run,
so a novel bad transfer arriving sixth was suppressed in silence. The budget is
now per shape, and an end-of-run line gives the totals — which revealed the
other thing the old report hid: a 1400-frame run has **1497** of these, not
five. A count in the "both ends bad" shape is the one that has never been seen
and would be worth chasing.

## Bugs worth remembering

Five of these cost real time, and none looked like what it was.

**The GBA's compiler rounds a structure's size up to a multiple of 4; clang
does not.** gcc for ARM defaults to `-mstructure-size-boundary=32`, so
`struct Unk_08353510` -- four `s16` and two `u8` -- is 12 bytes on the console
and 10 in this port. Nothing notices until something walks an array of one.
That structure is the animation script the warp star hands Kirby at the end of
a level: `++kirby->unk114` stepped 10 bytes through a table with a 12-byte
stride, the per-entry frame counter picked up half of the next entry, went
negative, and `if (!--counter)` could never fire. The ride animation therefore
never ended, and the star's last state sat waiting on `animationIndex != 90` --
a condition that had already been made unreachable. No crash, no freeze, no
diagnostic; the level simply never changed.

Two things about it are worth keeping. First, `platform/port/gba_layout.h` did
not catch it and could not have: that header is *generated from the tree*, so
it had recorded `sizeof(struct Unk_08353510) == 10` and was defending the bug.
It catches drift from today's tree, which is worth having, but not a
reconstruction that was never right. `make size-check`
(`tools/check_doc_sizes.py`) is the other direction -- the tree against the
size the decompilation documents on each closing brace, which is console truth.
It found six such types. Second, the confirmation was free once the sizes were
fixed: the decomp names members after their offsets, and in
`struct Unk_0812DBB4` the member following `unk0[4]` is `unk20`, so that array
has to be 32 bytes -- 8 per element, not 6. Every moved offset landed exactly
on its own name.

**A stub returning 0 hung the game silently.** `main.c` drains the VRAM
transfer queue through four workers and reads `0` as "did not finish", then
stops running game tasks until the queue drains. A stub returning 0 says
"never finished", so the game ran its VBlank handler forever and drew nothing.
`tools/stub_returns.conf` records where 0 is the wrong answer.

**`inline` means the opposite thing in C99.** agbcc is gcc 2.95, where a plain
`inline` definition also emits an external one. Without `-fgnu89-inline` every
`inline` function in the game becomes an undefined symbol.

**VBlank is a duration, not an instant.** The game keeps working after
`VBlankIntrWait` returns, and its queue workers check `REG_DISPSTAT & VBLANK`
before each item — so returning with the flag already cleared meant the queue
never drained and no task ever ran again. The flag now stays set and is spent
by the transfers themselves (`PortVBlankConsume`), which is the same budget the
hardware imposes.

**An address-taken undefined function does not fail the link.** `wasm-ld`
resolves it to a null table entry, so the program dies at the dispatch with no
build-time warning. This happened because `src/flamer.c` and `wip/flamer.c`
define *different* halves of the same file and the build was keeping only one;
`nullsub_125` ended up declared, placed in a table, and never defined. The two
copies are now merged into one file, and `tools/refresh_stubs.sh` compares
`llvm-nm`'s undefined and defined sets rather than trusting the linker to
complain.
