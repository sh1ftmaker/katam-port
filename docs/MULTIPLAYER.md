# Multiplayer

Kirby & The Amazing Mirror's link play is already in this port. Almost all of
it: the driver, the lobby, the lockstep input exchange, the desync detector,
the error screens. What was missing was the cable.

This document is three things. First, what the game's link protocol actually
does, which nobody had written down — the decompilation has the code and the
SDK's own comments, but not the shape. Second, the seam the port puts under it,
and how to write a transport against that seam. Third, and most important, what
is verified, what is assumed, and what is still not there.

---

## 1. What the game does

### The hardware underneath

Multi-play SIO. Up to four consoles on one cable; one of them — the *parent* —
clocks it. Every transfer moves one halfword from each unit to all four at
once, so when a transfer lands all four consoles hold the same four halfwords
in `SIOMULTI0..3`, each in the slot of the unit that sent it. A unit reads its
own word back out of its own slot. That is the whole of the hardware, and every
protocol below is built by repeating it.

`SIOCNT` at `0x04000128` reports the rest:

| bit | meaning |
|---|---|
| 0-1 | baud rate; the game uses 3 = 115200 |
| 2 | SI. **Low** on the unit that clocks the cable |
| 3 | SD. High when every unit on the cable is ready |
| 4-5 | which slot this unit occupies, 0-3 |
| 6 | error |
| 7 | write to start a transfer; hardware clears it when the transfer lands |
| 12-13 | mode; 2 is multi-play |
| 14 | raise the serial interrupt when a transfer lands |

### The driver: Nintendo's MultiSio library

`src/multi_sio.c` and `asm/multi_sio_asm.s`. It turns a stream of halfword
transfers into a **28-byte packet per unit per frame**, of which 20 bytes are
the caller's:

```
halfword  0   frameCounter (byte 0) | flags (byte 1: recvErrorFlags:4,
                 loadRequest:1, downloadSuccessFlag:1, loadSuccessFlag:1, -:1)
halfword  1   checksum
halfwords 2-11   the caller's 20-byte block  (MULTI_SIO_BLOCK_SIZE)
halfwords 12-13  overrun catch -- not sent under normal timing, and not
                 covered by the checksum; somewhere for a late transfer to
                 land without corrupting the packet
```

A packet is streamed one halfword per transfer, preceded by the sync word
`0xFEFE`. The sync word is the resynchronisation mechanism: a receiver that
sees it *after* the payload should have ended resets its counter to -1 instead
of storing it, and takes the next packet from the top.

The checksum rule is worth stating exactly, because getting it wrong by one
halfword makes every packet silently vanish, which looks identical to no cable:

> the sender sums the first twelve halfwords with the checksum field zeroed and
> stores `~sum - 14`, so that the receiver's sum of the same twelve halfwords
> comes out at exactly **-15** as a signed 16-bit value.

Buffering: the send side is double-buffered (the frame writes one while the
interrupt streams the other) and the receive side is **triple**-buffered per
unit — the interrupt fills one, rotates it out when the packet completes, and
`MultiSioRecvBufChange` rotates it again for the frame to inspect. Three
buffers rather than two because the frame and the interrupt are not
synchronised at all.

### The cadence: sixteen transfers per frame

Not a guess. `MULTI_SIO_TIMER_COUNT` in `include/multi_sio.h` works out to

```
SYSTEM_CLOCK / 60 / ((2 + 4 + MULTI_SIO_BLOCK_SIZE + 6) / 2)
  = 16777216 / 60 / 16
  = 17476 cycles
```

so timer 3 on the parent is set to fire sixteen times a frame. One sync word
plus fourteen packet halfwords is fifteen, so a frame carries exactly one
packet with one slot of slack. This is where `PORT_SIO_SLOTS` comes from.

### Which interrupt

This one is easy to get wrong and the port did have it wrong.

`MultiSioMain`'s state 0 promotes a console to parent, and when it does it
**masks the serial interrupt off and the timer-3 interrupt on** — the parent is
clocked by its own timer, not by the cable. But `gIntrTable[7]`, the timer-3
entry, holds `Timer3Intr`, which belongs to a completely different protocol.

The resolution is in `asm/crt0.s`. `IntrMain` tests `INTR_FLAG_TIMER3` and
`INTR_FLAG_SERIAL` **together, before every other flag**, and dispatches both to
`gIntrTable[0]`:

```asm
	ands	r0, r1, #INTR_FLAG_TIMER3 | INTR_FLAG_SERIAL
	bne	_080001AC
	add	r2, r2, #4
	ands	r0, r1, #INTR_FLAG_VBLANK
	...
```

so the later per-timer entry for timer 3 is unreachable on hardware, and both
parent and child run the same handler. `PortDispatchInterrupt` reproduces this.

`gIntrTable[0]` also does not hold a function. `MultiSioInit` copies the machine
code of `MultiSioIntr` into an IWRAM buffer so a cartridge waitstate cannot
delay it, and the game installs the *buffer* — `gIntrTableTemplate[0]` is
literally `(void *)gMultiSioIntrFuncBuf`. See §4.

### Above the driver: what the game sends

Two different games' worth, and they share the 20-byte block by tagging its
first byte.

**The lobby** (`src/multi_08030C94.c`, `struct MultiSioData_0_2`, tag `2`) is a
four-state handshake. The parent advertises `0x20`, children answer `0x40`, the
parent moves to `0x22` when every child it can see has answered, children
answer `0x41` then `0x42`, and when every child has reached `0x42` both sides
are agreed on the player count and each other's identity. A magic `0x123` in
the packet guards against a different game on the cable. The result is
`gUnk_0203AD30` (player count) and `gUnk_0203AD1C[]` (per-player identity).

**In play** (`sub_08030E44` / `sub_08030FE0`, `struct MultiSioData_0_0`, tag
`0x20`) is deterministic lockstep, and it is the interesting part:

- Each player's input for a frame is **12 bits**: the ten button bits from
  `gHeldKeys`, plus two bits of a desync check.
- One packet carries **eight consecutive frames** of that — the current frame
  and the seven before it — packed into six halfwords, plus a one-byte frame
  number. So a packet that is lost entirely is recovered from the next one, and
  the cable can drop up to seven frames in a row without the game noticing.
- The receiver reconstructs each peer's ring of sixteen frames of input, and
  the game **runs five frames behind**: `sub_08030D4C` lines every player up on
  frame `current - 5`. That is the input delay.
- When a peer's ring runs dry, `gUnk_020382D0.unk4 |= 4` and `GameLoop` skips
  `TasksExec` entirely — the whole game stalls for a frame rather than
  advancing without that player. That is the lockstep.
- The two spare bits per frame carry a checksum of `gRngVal` plus every Kirby's
  x and y. If the players disagree, `gUnk_02038580 = 8` and the session ends.
  That is the desync detector, and it means **the port must be
  bit-deterministic against the other units** for link play to survive, which
  is a much stronger requirement than looking right.

The error codes in `gUnk_02038580` are worth having written down, because they
are what a broken transport will produce:

| | |
|---|---|
| 3 | a peer sent something that was not an in-play packet |
| 4 | a peer's sub-session id disagreed |
| 5 | the input ring overran — a peer got too far ahead |
| 6 | a frame of a peer's input was never received |
| 7 | stalled waiting for a peer for more than 16 frames |
| 8 | desync: the players disagree about game state |

### Where multiplayer is entered from

Two doors, and they are different modes.

**Multi-cart, four-player main game.** Main menu → START GAME → MULTIPLAYER.
`sub_08032B0C` (`src/multi_08030C94.c:1311`). Every console needs its own
cartridge. The parent sends its save file's world properties to the children
over MultiSio before play starts, so all four run the same world.

**Single-cart, download play, for the sub-games.** Main menu → SUB-GAMES → pick
one → MULTIPLAYER. `sub_08019F28` (`src/multi_08019F28.c:303`). Three sub-games
have a multiboot image in ROM — Speed Eaters, Crackity Hack and Kirby Wave Ride
— which the host downloads to the other consoles, then pushes graphics to them
over 32-bit normal-mode SIO (`sio32_multi_load.c`), then plays over MultiSio.
The fourth sub-game entry needs 100 % completion and is multi-cart, not
download play: `sMultiBootPrograms` has no image for it.

Neither door is gated on a save flag. Both are reachable from a fresh file.

### A third SIO protocol, unrelated to any of this

`src/multi_sio_08158934.c` puts `SIOCNT` into **32-bit normal** mode and runs a
challenge/response that exchanges the string in `gAgbSramLibVer` and then three
rounds of nibble-checksummed words. It is not link play, it takes over
`gIntrTable[0]` while it runs, and the port must not clock multi-play transfers
underneath it — which is why `platform/sio.c` checks the mode bits before doing
anything.

---

## 2. The seam

`platform/port/mp.h`. A transport answers one question: *what did the other
units put on the bus for this transfer?*

```c
struct PortMpLink {
    u8 up;        /* 1 once units are connected                    */
    u8 selfId;    /* 0..3.  0 means this unit clocks the cable     */
    u8 players;   /* units in the session, including this one      */
    u8 error;     /* raises SIOCNT's error bit                     */
};

struct PortMpTransport {
    const char *name;
    int  (*open)(struct PortMpTransport *t, int players);
    void (*close)(struct PortMpTransport *t);
    void (*poll)(struct PortMpTransport *t, struct PortMpLink *link);
    int  (*exchange)(struct PortMpTransport *t, u16 send,
                     u16 recv[PORT_MP_PLAYERS]);
    void *user;
};

int PortMpAttach(struct PortMpTransport *t, int players);
```

Three properties it is shaped by:

**The game does not change.** The seam is under the game, at the SIO boundary.
`platform/sio.c` presents `SIOCNT`, `SIOMLT_SEND` and `SIOMULTI0..3` the way
the hardware would; the game's own driver drives the transport without knowing
it exists. No file in `src/` is aware of any of this.

**It is plain C.** The web build's JavaScript binding is in `platform/mp.c`, not
in the header. A native build registers a C transport through the same
`PortMpAttach` and includes nothing from emscripten.

**It does not assume a wire.** `exchange` is the hardware's operation, so a
same-process loopback implements it directly. A networked transport cannot
answer synchronously and is not asked to: `poll` runs once a frame and is where
a transport collects whatever arrived; `exchange` then answers from that
buffer. A frame is sixteen transfers, so one frame of buffering is one network
message. Returning 0 from `exchange` stalls the cable for that slot, which is
what a real cable does when a unit is late, and which the game's driver already
handles.

### Writing one

Four calls. In C:

```c
static int MyExchange(struct PortMpTransport *t, u16 send, u16 recv[4])
{
    struct MyState *s = t->user;

    if (s->consumed >= s->haveThisFrame)
        return 0;                      /* stall: nothing buffered yet */
    recv[1] = s->peerWords[s->consumed++];
    /* recv[selfId] is overwritten by the caller with `send`, so a transport
     * never has to know which slot it is in to be correct. */
    return 1;
}

static struct PortMpTransport sMine = { "mine", MyOpen, MyClose, MyPoll,
                                        MyExchange, &sState };

PortMpAttach(&sMine, 2);
```

In JavaScript, through `window.katamMultiplayer` (see `web/shell.html`):

```js
katamMultiplayer.attach({
    open:  (players) => 1,
    close: () => {},
    poll:  (ptr)       => katamMultiplayer.writeLink(ptr, 1, 0, 2, 0),
    exchange: (word, ptr) => {
        katamMultiplayer.writeSlots(ptr, [word, peerWord(), 0xFFFF, 0xFFFF]);
        return 1;
    },
}, 2);
```

`poll` is handed a four-byte address to fill; `exchange` a four-halfword one,
because both return more than one value. `exchange` is called up to sixteen
times a frame and has to answer at once, so a networked page transport buffers
a frame in `poll` and serves `exchange` out of that buffer.

### The reference transport

`platform/mp_loopback.c` puts a *unit* on the other end of the cable in the
same process: a small, complete MultiSio endpoint that streams properly framed
packets and reads and checksums the ones the game sends back. It needs no
network and nothing to configure.

There cannot be a second copy of the game instead. The port reserves the GBA
memory map at its true addresses inside a single wasm linear memory, so a
second instance would want EWRAM at `0x02000000` as well. That is a real
consequence of the decision in `docs/ARCHITECTURE.md` and it is not going away
cheaply; two-player local play in one tab would need either two modules with
different memory maps, or a memory-map rebase, and neither is a small change.

---

## 3. What is implemented

| | |
|---|---|
| `platform/port/mp.h` — the transport interface | done |
| `platform/sio.c` — SIOCNT status bits, transfers, the sixteen-slot cadence | done |
| `platform/multi_sio_intr.c` — `MultiSioIntr`, `MultiSioRecvBufChange` | done |
| `PortDispatchInterrupt` — IntrMain's SERIAL/TIMER3 routing, and the `gMultiSioIntrFuncBuf` interception | done |
| `platform/mp_loopback.c` — the loopback transport and its synthetic peer | done |
| `platform/mp.c` — registry, JavaScript transport, diagnostics | done |
| `web/shell.html` — `window.katamMultiplayer` | done |
| MultiBoot | **not implemented, and it is on the critical path — see §5** |
| A network transport | not written |

With nothing attached, `PortSioFrame` returns immediately, `SIOCNT` reads as an
unplugged cable, and the port behaves exactly as it did before any of this
existed.

---

## 4. Two things that only bite a wasm port

Both are the same shape as the bug that stopped this port at boot in
`agb_sram.c`: **code copied into a buffer and called as data**.

`MultiSioInit` does it twice.

```c
CpuCopy32(MultiSioRecvBufChange, gMultiSioRecvFuncBuf, sizeof(gMultiSioRecvFuncBuf));
CpuCopy32(MultiSioIntr,          gMultiSioIntrFuncBuf, sizeof(gMultiSioIntrFuncBuf));
```

In WebAssembly a function pointer is a table index, not an address, so both
copies read from low memory and copy nothing useful. What matters is the two
*calls* through the buffers:

- `gIntrTable[0] = (IntrFunc)gMultiSioIntrFuncBuf`, in
  `gIntrTableTemplate[0]` and again by hand in `multi_08019F28.c` and
  `multi_sio_08158934.c`. Caught in `PortDispatchInterrupt`, which recognises
  the buffer's address and calls the real `MultiSioIntr`. It says so once, so
  that "did the link driver's interrupt actually run" is answerable from
  outside.
- `MultiSioRecvDataCheck` calls `gMultiSioRecvFuncBuf` directly. That one is
  inside the game's own C, so there is nowhere for the platform layer to
  intervene; `tools/portify.py` rewrites it to name the function
  (`CODE_IN_RAM`).

The copies themselves are left alone. They are harmless and faithful, and
removing them would make the port disagree with the decompilation for no gain.

---

## 5. MultiBoot: needed, and not for the reason you would expect

MultiBoot downloads a program to a cartridge-less GBA over the cable. Whether
this port needs it looked like a question about download play — and download
play, the single-cart sub-game mode, is genuinely out of scope: it boots a
*different program* on the other console, which is not something a
WebAssembly port can be on the receiving end of.

But the multi-cart four-player mode needs the library too, and not for
downloading anything. `src/multi_08030C94.c:804` calls

```c
MultiBootInitWithParams((void *)0, (void *)0x100);   /* zero-length program */
```

— a probe, not a transfer. The game uses MultiBoot's client-recognition phase
as its **link detector**. In the lobby's state 0 (`sub_080302EC`), the only
thing that starts a transfer on the unit that clocks the cable is
`MultiBootMain`:

```c
if (!(REG_SIOCNT & SIO_MULTI_CONNECT))      /* SI low: we clock the cable */
    MultiBootMain(&gMultiBootParam);
```

Nothing else in the lobby writes `SIOCNT`'s start bit until the link has
already been classified. So with `MultiBootMain` stubbed, the cable never
clocks, the thirty frames of stable client replies never accumulate, and the
lobby sits on "Please connect the Game Boy Advance Game Link cable" forever.

**This is measured, not inferred** — see §6.

What a working link-up would need, in order:

1. `MultiBootInit` and the probe phase of `MultiBootMain`: send
   `0x6200 | client_mask` and track which slots reply `0x720X`. Roughly sixty
   lines, from GBATEK. This is what the multi-cart path needs and nothing more:
   once `gUnk_0300050C` is classified, `sub_0803040C` clocks the cable itself.
2. The transport's peer must then also implement the game's own lobby
   handshake — the `0x8F51` / `0x70AE` / `0xE4E4` words in `sub_08030898`, then
   the `0x20`/`0x40`/`0x41`/`0x42` exchange in `multi_08030C94.c`. That is
   game logic, not hardware, and it is the second half of a synthetic peer.
3. `MultiBootStartMaster`, `MultiBootCheckComplete` and the actual program
   transfer are needed only for download play, and can stay stubbed.

They are deliberately left as stubs rather than half-written. A `MultiBootMain`
that clocks the cable but does not track clients would move the failure
somewhere less obvious than the screen that currently says what is wrong.

---

## 6. Verification

Everything below was run against the user's own ROM with
`tools/headless_test.js`. The harness knobs are `MP=`, `MP_AT=`, `MP_ID=` and
`MP_SELF=`.

### The link comes up, both ends, 2 to 4 players

```
MP=loopback:2 MP_AT=60 MP_SELF=1 make test FRAMES=200
```

```
[katam-port] mp: transport 'loopback' attached, 2 player(s)
[katam-port] mp: MultiSioMain -> 0x0000 (recv=0 connected=0 child)
[katam-port] link up: 2 player(s), this unit is slot 0 (parent)
[katam-port] mp: MultiSioMain -> 0x0080 (recv=0 connected=0 parent)
[katam-port] mp: MultiSioMain -> 0x0393 (recv=3 connected=3 parent)
[katam-port] mp: MultiSioMain -> 0x8393 (recv=3 connected=3 parent flags-valid)
[katam-port] mp: transport='loopback' up=1 self=0 players=2 transfers=2208 stalls=0
[katam-port] mp:   gMultiSioRecv[0] = 4B 94 C2 C3 C4 C5 C6 C7
[katam-port] mp:   gMultiSioRecv[1] = A1 8B 12 13 14 15 16 17
[katam-port] mp:   peer 1 took 137 packet(s) from us, last payload 4B 94 C2 C3 C4 C5 C6 C7
```

Reading that line by line:

- `0x8393` is the game's own `MultiSioMain` return value, in the game's own
  `gMultiSioStatusFlags` at `0x03002554`. `recv=3` and `connected=3` are its
  receive-success and connection-history bitmasks: slots 0 and 1, both live.
  `0x0080` is `MULTI_SIO_TYPE`, so the game's driver promoted itself to parent;
  `0x8000` is `MULTI_SIO_RECV_FLAGS_AVAILABLE`, which the library only sets
  after eight frames of stable communication.
- `gMultiSioRecv[0]` is `4B 94 C2 C3…` — what this unit sent, read back out of
  its own slot, which is what the hardware does.
- `gMultiSioRecv[1]` is `A1 8B 12 13 14 15 16 17` — the peer's payload,
  `0xA0 | slot` then a packet counter then `slot * 0x10 + i`, exactly what
  `mp_loopback.c` builds.
- **The peer validated 137 packets from the game.** That is the other
  direction, checksummed by an independent implementation of the same rule.
- 2208 transfers over 138 frames is 16.0 per frame, and no stalls.

`SIOCNT` at `0x04000128` read `0x6003` before and `0x208B` after: multi-play
mode, 115200, SD set, SI clear, id 0, start bit set — and interrupt-enable
cleared, which is `MultiSioMain`'s `ifEnable = 0` on the parent.

Four players (`MP=loopback:4`) gives `recv=F connected=F` and three peers each
taking packets. The game as a child (`MP_ID=1`) gives `0x8303` — the same
flags with `MULTI_SIO_TYPE` clear — with the loopback clocking the cable
instead.

### The game's own interrupt handler runs

```
[katam-port] gIntrTable[0] holds gMultiSioIntrFuncBuf (the IWRAM copy of
             the link driver's interrupt); calling MultiSioIntr
```

That line is printed from `PortDispatchInterrupt` the first time it takes the
buffer branch. It is also implied by everything else: `MultiSioIntr` is the
only thing that writes `SIOMLT_SEND` per transfer, so no packet could have been
framed without it.

### The JavaScript transport works

```
MP=js:3 MP_AT=60 MP_SELF=1 → 0x8797 (recv=7 connected=7 parent flags-valid)
```

The harness's `MP=js` installs an echo peer written in JavaScript. Three slots
come up and validate. This is the same code path a page uses.

### The game reaches its own link lobby, and stops where §5 says

This is the one that matters, because it is the game driving, not the harness.

```
MASH=300:1:8:440 \
PRESS_AT="480:1,500:0,560:1,580:0,720:1,740:0,810:0x80,825:0,850:1,870:0" \
MP=loopback:2 MP_AT=200 \
node tools/headless_test.js build/katam-node.js <rom> 1100
```

drives title → FILE SELECT → FILE 1 → GAME SELECT → START GAME → MULTIPLAYER,
and the game arrives at its own link-waiting screen — "TRANSMITTING… Please
connect the Game Boy Advance Game Link cable" — with:

```
[katam-port] called MultiBootInit(), which has no C body yet
[katam-port] called MultiBootMain(), which has no C body yet
[katam-port] mp: transport='loopback' up=1 self=0 players=2 transfers=0 stalls=0
```

`transfers=0` with a transport attached and the link up is the measurement. The
game is in multi-play mode, the port is ready to clock, and nothing arms a
transfer — exactly the prediction in §5, from the game's own code rather than
from a reading of it.

### No regression

A 300-frame boot with no transport attached produces a byte-identical summary
to the build before this work: 182 distinct colours, `DISPCNT=0x1f40`, first
non-blank frame 3, same audio RMS and peak, **zero diagnostics**. A 1800-frame
run with `MASH=600:1:6` reaches gameplay (17 tasks, 67709 non-zero VRAM bytes)
with zero diagnostics. `make` links and `make check-dist` passes.

### What is *not* verified

Stated plainly, because the difference is the whole point:

- **The game has never driven `MultiSioMain` itself.** `MP_SELF=1` has the
  platform layer make that call once a frame, the way `GameLoop` does under
  `if (gUnk_03002558 != 0)`. Everything below `MultiSioMain` is exercised by
  the game's own code; the call itself is not. It cannot be until §5 is done.
- **Nothing above the packet layer has run at all.** The lobby handshake, the
  input ring, the five-frame delay, the desync check: all still only read, not
  executed.
- **Two real consoles have never been on the other end.** The peer is this
  repository's own reading of the protocol, so a shared misreading would not
  show up. The strongest evidence against that is that the peer's checksum
  implementation and the game's are independent and agree, and that the game's
  library reports `RECV_FLAGS_AVAILABLE` — which it only does after eight
  consecutive frames of well-formed traffic.
- ~~**Determinism has not been tested and is the real risk.**~~ **Measured on
  2026-08-05; see below.** It is no longer the open question this section said
  it was, though the *multiplayer* code path is still unexercised.

### Determinism: measured

The desync check hashes exactly this (`multi_08030C94.c:114`):

```c
r5 = gRngVal;
for (i = 0; i < 4; ++i)
    r5 += gKirbys[i].base.base.base.x + gKirbys[i].base.base.base.y;
```

— two bits of that sum, once a frame. So "will two instances of this port stay
in sync" is a question about `gRngVal` and eight `s32`s, and it is directly
checkable with the instruments in [DEBUG-TOOLING.md](DEBUG-TOOLING.md) §7.

Against 1401 frames of identical scripted input:

| | |
|---|---|
| `gRngVal`, hashed **every frame**, LP64/SDL against wasm32/node | identical on all 900 frames tested |
| all four Kirbys' x and y at frame 600, same two builds | identical, sum 204692 |
| the whole DMA transfer stream, wasm32 / i686 / x86-64 / aarch64 | byte-identical, 63236 transfers |
| frame 1400 pixels, same four builds | 0 of 38400 differ |
| audio on against audio off, **same binary**, independent save profiles | byte-identical, 35208 transfers |

The last row is the one that had been assumed to fail. An earlier note in
DEBUG-TOOLING.md said turning audio off alone moved EWRAM at frame 63; that is
true of the *sound engine's own* state and does not reach game state. The
mixer's output does not feed anything the desync check hashes.

So: **the port is deterministic given identical input, across two ABIs, three
architectures and two hosts, in both audio configurations.** That is the
property lockstep needs, and it is a stronger result than the one this section
asked for -- it survives a change of pointer size, not just a change of
machine.

Two caveats, both real:

- **This is the single-player path.** Nothing above the packet layer has run,
  so a divergence that only exists in link mode -- `gUnk_0203AD3C` selecting a
  different player, the world-properties transfer, the five-frame input delay
  -- would not appear here.
- **Identical input is the premise, not a finding.** It says the port computes
  the same thing from the same input; delivering the same input to four
  instances is what the transport is for.

When comparing two runs, give each one its own `XDG_DATA_HOME`. The native
builds keep a save file and the node harness does not, so two runs that share a
profile are not being asked the same question -- and the first attempt at the
audio row above was contaminated exactly that way, reporting a divergence at
frame 409 that was one run resuming the other's saved tutorial.

---

## 7. What a netplay transport would still need

Beyond §5, which is the blocker:

1. **A frame of buffering, and the discipline to keep it.** `exchange` must
   answer synchronously. Send the frame's sixteen halfwords as one message and
   serve the next frame's `exchange` calls from what arrived. That is one round
   trip of added latency on top of the game's own five frames of input delay,
   which is already generous.
2. **Something to do when a peer is late.** Return 0 from `exchange` and the
   cable stalls for that slot; the game's driver treats it as a dropped
   transfer and the sync word re-aligns it on the next packet. Do that for too
   long and the game reports code 7 and ends the session. A transport that
   would rather stall the frame than desync should stall in `poll`.
3. **Slot assignment out of band.** `selfId` and `players` come from the
   transport, not from the game. Slot 0 clocks the cable, so whoever is slot 0
   sets the pace for everyone.
4. ~~**Determinism, first.**~~ **Done** — see "Determinism: measured" in §6.
   `gRngVal` and every Kirby's x and y, the two things the desync check hashes,
   are identical frame for frame between the LP64 native build and the wasm32
   one, and all four builds agree transfer for transfer and pixel for pixel.
   Audio on and audio off are identical too. What remains untested is the link
   path itself, which cannot run until §5 is done.

   Keep the check honest as netplay is built: hash the desync word itself, not
   a proxy. `PORT_WINDOW=30068D8:4` watches `gRngVal` every frame, and the
   Kirby positions are at `0x02020EE0 + 424*i + 64` and `+68`.

---

## 8. Rollback: what it would cost, measured

The lockstep in §1 is what a *cable* wants -- five frames of input delay and a
hard stall when a peer is late. Over the internet a stall is a visible freeze,
so the question is whether this port could predict inputs, run ahead, and
re-simulate when the truth arrives. Two numbers decide it, and both are
measured rather than estimated. Machine: x86-64, `build/lp64`, `--turbo`.

### A snapshot is a memcpy

Rollback needs to save and restore complete game state. In most emulators that
is a bespoke serialiser; here it is six `memcpy`s, because the port reserves the
GBA map at its true addresses and *all* console state lives in it:

| region | size |
|---|---|
| EWRAM `0x02000000` | 256 KiB |
| VRAM `0x06000000` | 96 KiB |
| IWRAM `0x03000000` | 32 KiB |
| IO / PLTT / OAM | 1 KiB each |
| **total** | **387 KiB** |

**save 5.0 µs, restore 4.7 µs.** A twelve-frame ring costs 4.5 MiB. The fixed
memory map was chosen for [ARCHITECTURE.md](ARCHITECTURE.md)'s reasons and it
happens to make state capture free.

### Re-simulation is dominated by drawing, which rollback skips

Marginal cost per frame, startup subtracted by differencing two run lengths:

| | native x86-64 | wasm32 / node |
|---|---|---|
| compose + blit (what a player sees) | 0.945 ms | 1.22 ms |
| blit only, no pixel composition | 0.253 ms | — |
| **simulation only** | **0.0025 ms** | **0.0062 ms** |

**Pixel composition is ~99.7 % of a native frame.** A re-simulated frame is
never displayed, so it pays none of it. `PortSetRenderEnabled(0)` skips exactly
the composition and nothing else — verified state-neutral: the DMA transfer
stream over 900 frames is byte-identical with it on and off.

So an eight-frame rollback natively costs `4.7 + 8 × 2.5 ≈ 25 µs`, which is
**0.15 % of a 16.7 ms frame budget**. Sixty frames -- a full second of
mispredicted input -- costs about 155 µs. The depth is not the constraint.

### Two measurement traps, both fallen into

- **Skipping the whole scanline loop is not "no rendering".** `PortRenderFrame`
  drives VCOUNT, the DISPSTAT flags, the affine reference points, the HBlank
  DMAs and the HBlank/VCOUNT interrupts; the game arms per-scanline effects
  every frame and its own handlers run from in there. Skipping the loop looked
  like an 86× speedup and was a different program — one distinct picture in
  3600 frames, and new DMA transfers leaving the map. Only `RenderScanline` is
  skippable.
- **Skipping the blit breaks the node harness.** `Module.portPresent` is where
  `tools/headless_test.js` counts frames, applies `MASH` and calls
  `_PortSetKeys`. With no blit the game gets *no input at all*, so the run is
  not the same workload — it reported 40 ms/frame, which was neither pacing nor
  the port being slow. The blit is therefore left alone by
  `PortSetRenderEnabled`; a caller that wants no output for a re-simulated
  frame suppresses it on its own side, where it knows what the host is.

- **The harness itself was two thirds of a wasm frame.** `portPresent` copied
  153,600 bytes out of the heap every frame, and `requestAnimationFrame` was
  shimmed to `setTimeout(cb, 0)`, which node clamps to about a millisecond --
  so the shim, not the port, set the frame rate. `setImmediate` runs the
  callback in the same loop iteration's check phase and removes the floor;
  under `PORT_NO_RENDER` the harness now skips the copy as well. Both changes
  are pacing only: the DMA stream is identical across each, 63236 transfers,
  0 differences.

**wasm simulates at 6.2 µs a frame -- only 2.5x the native cost.** That is the
number that decides whether a browser client can do this at all, and it says
yes with room to spare.

### What is genuinely hard

Not the snapshot and not the speed.

1. **State outside the map.** Console state is all inside it; the *port's* is
   not. `platform/dma.c`'s `sChannels[4]` holds armed VBlank and HBlank
   transfers that persist across frames, `main.c` holds `sVBlankBudget` and
   `sKeysDown`, and `platform/m4a_mixer.c` and `ppu.c` hold their own. A
   snapshot has to include the ones that feed simulation. That is a bounded
   audit — a few dozen statics — but it must be done deliberately, and
   `tools/abi_size_diff.py`-style rigour is the right standard for it.
2. **The execution point.** The game's loop never returns: it blocks in
   `VBlankIntrWait` deep inside `GameLoop`. Restoring memory does not restore
   the C stack. The workable shape is to snapshot and restore only at the frame
   boundary, where the stack is the same by construction — but that has to be
   established, not assumed, and anything live in a local across
   `VBlankIntrWait` breaks it.
3. **Host pointers in the map are fine, and only same-process.** `Object2::unk78`
   and `struct Task::main` hold host function pointers. Within one process a
   snapshot restores them correctly; between two clients or two builds they are
   meaningless. Rollback is same-process, so this costs nothing — but it means
   snapshots can never be shipped over the wire as a resync mechanism.
4. **The game's own five-frame delay would stack on top.** Rollback wants the
   transport to answer `exchange` immediately with a *predicted* input, so the
   cable never stalls and the game never knows. That fits the existing seam
   exactly — a rollback engine is a `PortMpTransport`. But `sub_08030D4C` still
   lines everyone up on `current - 5`, so you would be adding prediction
   underneath a delay that already exists. Removing it is a patch to game code,
   and it is the one place this design would stop being "the game, unmodified".
5. **Audio during catch-up.** Re-simulated frames must not re-emit samples.
6. **§5 is still the blocker.** None of this is reachable until `MultiBootMain`
   clocks the cable.

---

## 9. Joining, leaving, and how well input compresses

### The game already has AI Kirbys, and the split is by index

`gUnk_0203AD30` is the number of *human* players and `gUnk_0203AD44` the number
of Kirbys. `sub_080332BC` creates all four and then attaches an AI controller to
every index from the human count up:

```c
for (curKirbyId = 0; curKirbyId < 4; curKirbyId++)
    CreateKirby(curKirbyId, &gKirbys[curKirbyId], ...);
for (otherKirbyId = arg0; otherKirbyId < 4; otherKirbyId++)   /* arg0 = humans */
    sub_0800ECAC(otherKirbyId, ...);
```

So single-player is one human and three AI Kirbys, and a four-player session is
four humans and no AI. **Handing a departed player's Kirby to the AI is
therefore a thing the game already knows how to do** — it is not a transport
trick and it does not need new game code.

Two consequences that decide the design:

- **`gUnk_0203AD30` is read all over the game**, including enemy difficulty
  (`gUnk_08351530[type][gUnk_0203AD30 - 1]`), chest contents and switch
  behaviour. Changing it changes the simulation, so every client has to change
  it on the *same frame*.
- **The split is purely by index**, so only the highest-numbered player can
  leave without renumbering. If player 1 of 3 drops, making Kirby 1 AI-driven
  means the humans are no longer a prefix of 0..3, which is the one thing this
  arrangement cannot express. Either the last player is migrated down into the
  vacated slot — which changes which Kirby a human is holding, mid-game — or
  the slot keeps receiving synthetic input.

### Determinism does not forbid this; rewriting history does

A player count that changes mid-session is only a desync risk if clients
disagree about *when* it changed. Do not edit the input history — **append the
change to it**: "player 2 becomes AI at frame F" is an event on the same
timeline as the inputs, applied at F by every client and by every replayer.
The history stays append-only and a replay from frame 0 reproduces the change
at the same point.

Under rollback, schedule F beyond the rollback window (say `current + 30`) so
the event is confirmed before it is applied and can never be mispredicted.

### How well the inputs compress: measured, on Nintendo's own recordings

The attract-mode demos are four players of real recorded input per demo, six
demos, in ROM at `gUnk_082EAB98`. And the format is already run-length encoded
— `sub_080204EC` packs each entry as a `u16` of **10 button bits plus a 6-bit
repeat count**, bumping the count by `0x400` while the input is unchanged and
capping a run at 64 frames.

Decoding all 24 tapes:

| | |
|---|---|
| runs | 2172 |
| player-frames covered | 49431 |
| **mean run length** | **22.8 frames** |
| RLE | 4344 bytes |
| the game's wire encoding, 12 bits/frame | 74146 bytes |
| **compression** | **17.1x** |

Per tape it ranges from **8.9x** for the busiest player to **47x** for a nearly
idle one; actively-played tapes cluster around 9–22x. The 47x cases are hitting
the 64-frame cap, so a catch-up log with a wider count field would do better
still on idle players.

What that means for a rejoin: ten minutes of four-player input is 216 KiB in the
game's own wire format and **about 13 KiB run-length encoded**. The entire
history of a session fits in one message, and §8's numbers say replaying it
costs 0.19 s in a browser.

RLE is for the *stored* log, not the wire — a live stream cannot batch runs
without adding the latency the whole exercise is meant to avoid.

---

## 10. The rollback machinery, and what it is verified to do

`platform/port/rollback.h` and `platform/rollback.c`. No network in it and none
intended: a transport calls `PortRbConfirmInput` when a peer's real input
arrives, and this decides when to roll back and re-simulates.

| | |
|---|---|
| snapshot / restore | the six GBA regions plus the port's own out-of-map state |
| input timeline | per-frame, per-player, with `known` marking real against predicted |
| prediction | "the same again" — right ~96% of the time at the measured 22.8-frame mean run |
| rollback | requested from `PortRbConfirmInput`, performed at the top of `PortPresentFrame` |
| join / leave | events on the timeline, applied at a scheduled frame |
| the log | the game's own RLE, encode and decode, with the events in it |

### Where it hooks in, and why that is the whole trick

`VBlankIntrWait()` is `PortPresentFrame()`, called from inside `GameLoop`, and
`GameLoop` keeps nothing in locals across it. The C stack is
`main → AgbMain → GameLoop → VBlankIntrWait → PortPresentFrame` on every frame,
identical in shape. So restoring the GBA map at the top of `PortPresentFrame`
and returning normally puts the game back at that frame with no stack surgery:
**the loop simply runs the frames again.** That is a property of the port's
architecture, not of this code.

Input is driven at `UpdateKeyInput` — the one point where the host's buttons
become the game's. Forcing it earlier does not work, because the host polls
between the top of the frame and the latch.

### Verified, by doing it

`PORT_RB_SELFTEST=at:span` snapshots at a frame, runs a span, restores, runs the
span again with the recorded input, and compares. It records and replays the
input itself — the native host counts frames with its own counter, so `--mash`
would hand the second pass *different* buttons and the test would fail for a
reason unrelated to the snapshot.

| span | result |
|---|---|
| 30, 60, 120, 240, 600 frames | **PASSED** — bit-identical state both times |
| on wasm32, i686 and x86-64 | PASSED, same log bytes on each |

Six hundred frames is ten seconds, far past any rollback window.

### The negative control, and the finding it produced

A self-test that cannot fail proves nothing, so `PORT_RB_BREAK` damages the
restore on purpose.

- **`=1`, skip the port's out-of-map state** (armed DMA channels, affine
  reference points): **passes.** That is a measured fact rather than a broken
  test — the game re-arms its DMA every frame and the affine points are
  re-latched from the IO registers, which are in the map. So on this path the
  port's own statics do not reach the simulation. They are still saved, because
  "does not, here, today" is not "cannot".
- **`=2`, skip restoring IWRAM** (which holds `gTasks`): **fails**, which is
  what demonstrates the test can detect a bad snapshot at all.

The frame state is restored under both, because `sFrameCount` lives there and
without it the restored run never arrives back at the frame the test waits for.
The first version of the control skipped all three and produced no output —
breaking the harness rather than demonstrating a divergence.

### What is not done

- **No transport drives it.** `PortRbConfirmInput` has never been called by
  anything but a test, so the rollback *path* — predict, mispredict, restore,
  re-simulate — is exercised only by the self-test's restore-and-replay, not by
  a real disagreement. §5 is still the blocker for reaching link play at all.
- **`PortRbReplayTo` sets a target and returns**; the frames happen because the
  game keeps running, and the host has to stop pacing for it to be fast. The
  host side of that is not written.
- **Leaving is still only clean for the highest-numbered player** (§9), and the
  engine does not renumber.
