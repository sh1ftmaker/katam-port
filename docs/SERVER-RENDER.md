# Server-side simulation: four Kirbys in one Durable Object, frames streamed back

Research, 2026-08-27. The question: instead of every browser running its own
copy of the game and exchanging inputs (Path B in [NETPLAY.md](NETPLAY.md)),
run all four seats *inside* the room's Durable Object and stream each seat's
rendered 240x160 frame to its client. Then a dropped packet can delay a
picture but can never desynchronise a world, because there is only one world.

Everything below is measured on this machine (x86-64, node v22, the
`build/katam-node.js` release build, real ROM) unless marked *estimate*.
The measurement scripts are throwaway; the numbers are what matter.

---

## 1. Bandwidth: the cheap part, with one trap

600 consecutive in-level frames, captured two ways: idling/walking
(`MASH=120:17:20`) and with Right held so the screen scrolls (`HOLD=0x10`).
Pixels converted to the GBA's native 15-bit RGB555 before compression, so
"raw" is 76,800 B/frame = 36.9 Mbit/s at 60 fps.

| encoding | walking, mean / p95 / max | scrolling, mean / p95 / max | mean Mbit/s (scrolling) |
|---|---|---|---|
| deflate(raw RGB555) | 11.2 / 11.4 / 11.5 KB | 10.9 / 11.2 / 11.4 KB | 5.2 |
| **XOR previous frame, then deflate** | **0.43 / 1.3 / 1.8 KB** | **1.97 / 5.2 / 15.5 KB** | **0.95** |
| QOI (RGB24, lossless) | 22.1 / 22.4 / 22.6 KB | 21.5 / 22.2 / 22.5 KB | 10.3 |
| QOI of the XOR delta | 1.05 / 2.2 / 3.0 KB | 3.9 / 9.8 / 30.1 KB | 1.9 |

Encode cost in plain JS on the server: deflate level 6 **0.8 ms/frame**,
QOI **0.09 ms/frame**. Level 9 buys 3% for much more CPU; not worth it.

Findings:

- **XOR-delta + deflate is the right primitive** and it is tiny: a median
  frame is 350 B walking, 1 KB scrolling. A four-seat room costs the DO
  well under 1 Mbit/s of egress most of the time. Cloudflare bills nothing
  for outgoing WebSocket bytes.
- **QOI is the wrong tool here.** It is a spatial coder for photographs; a
  GBA frame is 100–150 distinct colours in flat runs, and deflate on the raw
  16-bit frame already beats QOI 2:1. QOI on the XOR delta is better than
  plain QOI but still 2x worse than deflate on the same delta. Its only
  virtue is speed, and deflate at 0.8 ms is not the bottleneck.
- **The trap: a scroll defeats an XOR delta.** When the whole screen shifts
  by a pixel, every pixel changes and the delta (15.5 KB max) is *worse*
  than just deflating the raw frame (11.4 KB). Only 35 of the 599
  "scrolling" frames actually scrolled, because Kirby walked into a wall;
  a real player running through a level will scroll for seconds at a time,
  which is **~5–7 Mbit/s per seat, sustained**, on the naive scheme.
  Two fixes, in order of effort:
  1. Per frame, send whichever is smaller of `deflate(xor)` and
     `deflate(raw)`. Caps the worst case at 5.2 Mbit/s. One `if`.
  2. **Motion-compensate with the scroll registers.** The server has
     `REG_BG?HOFS/VOFS` for every layer; XOR against the previous frame
     *shifted by the main BG's scroll delta* and the delta collapses back to
     "sprites and the newly revealed column". This is the trick that makes
     it 1 KB/frame while running. The renderer already knows the offsets
     (`platform/ppu.c`); the encoder just needs them handed over. *Estimate*:
     scrolling frames drop to walking-frame sizes.

**Measured after building it** (`netplay/framecodec.mjs`, `make
framecodec-test`): the codec roundtrips every frame exactly, encodes in
0.3 ms and decodes in 0.08 ms per frame in plain JS, and a flying run
through the hub costs a mean of 1.66 KB/frame (p50 1.1 KB) = 0.8 Mbit/s.
But the scroll-compensation win is **2x, not 10x**: the game's BG1–3 are
parallax layers whose scroll registers are rewritten per scanline by the
HBlank handler, so one global shift can only compensate the main layer
(BG0) -- changed pixels on a scrolling frame fall from ~19k to ~10k, and a
compensated frame still deflates to ~8 KB (p95 12 KB). The raw fallback
holds the ceiling at ~15 KB. So: idle and walking are cheap, sustained
scrolling is ~4–6 Mbit/s per seat. Fine on wifi, marginal on cellular;
below that is the next rung.

A further rung, if ever wanted: ship PPU *state* deltas (VRAM/OAM/PLTT/IO
writes, ~a few KB/frame since the game streams tiles incrementally) and let
the client's own `ppu.c` compose. Pixel-exact, and cheaper still — but the
per-scanline register effects (`ARCHITECTURE.md`, "per-scanline rather than
per-frame") would need an HBlank register log per frame, so it is real work.
Frames are simpler and the numbers above say frames are cheap enough.

## 2. Compute: nearly free

Measured with four booted instances in one node process, each running its
own `AgbMain` from power-on:

| | per frame per instance |
|---|---|
| simulation + pixel composition (render on) | **1.09 ms** |
| simulation only (`PortSetRenderEnabled(0)`) | **0.01 ms** |

Four seats rendered = ~4.4 ms, plus four deflates = ~3.2 ms, ~7.6 ms of a
16.7 ms frame on one thread. Fine. (This agrees with MULTIPLAYER.md §8:
1.22 ms compose, 6 µs sim.)

## 3. Memory: the real constraint, and where it actually goes

A Durable Object gets **128 MB**. Four naive instances measured **+40–50 MB
each** on top of a node baseline — over budget. Where it goes (one instance,
private dirty pages, THP as shipped on this host vs disabled):

| stage | THP `always` (this host) | THP disabled (`prctl`) |
|---|---|---|
| instantiate (V8 compiles the 2.45 MB wasm) | +9 MB | +7 MB |
| ROM copied to `0x08000000` | +16 MB | +16 MB |
| boot + first frame | **+17 MB** | **+3.5 MB** |
| 600 frames (JIT tier-up) | +2 MB | +8 MB |
| **total per instance** | **~44 MB** | **~35 MB** |
| four instances (measured) | 248 MB RSS | 187 MB RSS |

What the game itself touches is small. A census of resident pages in the
192 MB linear memory after boot: only **~1.2 MB** of non-ROM memory is
non-zero (EWRAM 256 KB, IWRAM 32 KB, VRAM 96 KB, palettes, OAM, IO, SRAM,
~0.6 MB of the port's own static data and stack).

The "+17 MB at boot" is **transparent huge pages**, not the game: this host
runs `/sys/kernel/mm/transparent_hugepage/enabled = [always]`, and every
GBA region the game touches — nine of them, each at its own hardware address
megabytes from the next — faults in a full 2 MB huge page. Nine regions x
2 MB = 18 MB of zero-filled padding around 1.2 MB of real state. Disabling
THP for the process (`prctl(PR_SET_THP_DISABLE)`) takes the four-instance
figure from 248 MB to 187 MB. Whether Cloudflare's hosts do this is unknown
and cannot be controlled from inside a Worker; assume the worse number.

So per instance: **16 MB ROM + ~1.2 MB state**, with ~10 MB of V8 code
shared across instances of the same module, and the rest is host artefact.
The ROM is the immovable part: every wasm instance owns its linear memory
and the game addresses ROM at absolute `0x08xxxxxx`, so four instances mean
four copies. (0.8 MB of the ROM is `0xFF` trailing padding that need not be
copied; it saves nothing that matters.)

### The way out: four game states through one instance

Rollback already made state capture free: a full snapshot is six `memcpy`s
of the GBA regions plus the port's out-of-map state, **387 KB, 5 µs to save
and 4.7 µs to restore** (MULTIPLAYER.md §8, `platform/rollback.c`
`SaveTo`/`MapBytes`/`HostBytes`). All four seats are always at the same
frame boundary, which is the one place a snapshot is valid.

So run **one** wasm instance holding one ROM, and keep four 387 KB seat
snapshots. Each frame, for each seat: restore its snapshot, set the local
player id, run the frame with rendering on, deflate the picture, save the
snapshot. Cost per frame: 4 x (4.7 + 5 µs + 1.09 ms + 0.8 ms) ≈ **7.6 ms**,
same as four instances. Memory: **one instance (~35–44 MB) + 1.5 MB of
snapshots**, comfortably inside 128 MB with room for the room's input
history.

Why four seat states are needed at all rather than one sim and four
cameras: the local seat (`gUnk_0203AD3C`) is sim-neutral (NETPLAY.md §3c) but
not VRAM-neutral — the game streams the local Kirby's room into VRAM
incrementally, so each seat's VRAM/OAM/scroll is the product of its own
history. The four snapshots are what makes that history per-seat while the
ROM and the code are shared.

Audio is the same sim four times, so mix it **once** (seat 0's PCM) and
send the same stream to everyone.

## 4. What this buys and what it costs, versus Path B

**Buys:**
- No desync is possible. Path B's whole apparatus — bit-equality bars,
  seals, reconfirmation from history, the boot-script replay for joiners,
  the "never simulate past a peer's window" stall — goes away. A joiner is
  a new WebSocket that starts receiving frames; a rejoin is the same.
- No cross-client nondeterminism risk (different browsers, different
  builds): one build runs the world.
- Clients become thin: a WebSocket, an inflate, a canvas, an audio queue.
  Phones stop paying 1.2 ms/frame to compose and stop needing 192 MB of
  linear memory.

**Costs:**
- **Input latency is unhidden.** Rollback exists to hide RTT; this design
  cannot. Every input pays client→DO→sim→encode→DO→client before the player
  sees it: *estimate* 40–100 ms same-region over the DO relay, versus the
  1-frame local feel Path B gives. For a platformer with 1-frame jumps that
  is the difference the user will feel first. Loss on TCP WebSockets shows
  as a picture freeze (head-of-line blocking), never a desync — which is
  what was asked for, but it is a freeze all the same.
- **Audio has to be streamed too.** 32 kHz stereo s16 is 1 Mbit/s raw per
  client; deflate does poorly on PCM. Realistically an Opus encoder in wasm
  on the server (~64 kbit/s) and a jitter buffer on the client, adding
  ~60–100 ms audio latency. This is the biggest unmentioned work item.
- **The ROM lives on the server.** The project's line today is "your ROM is
  read in your browser and never uploaded". Here a client must upload it to
  the DO, and the DO keeps it in memory for the room's lifetime and serves
  derivative frames to other players who may not own the game. That is a
  deliberate change to the project's stance, not a technical detail.
- **Durable Object limits to verify before designing further**, none of
  which could be measured from here:
  - the 128 MB budget is *actual* memory, but whether workerd accounts a
    192 MB `INITIAL_MEMORY` reservation as 192 MB is unknown — if it does,
    the flat GBA map is dead on arrival and the port would need a
    remapped `GLOBAL_BASE`/`INITIAL_MEMORY` build for the server;
  - a continuously ticking loop (`setInterval` at 60 Hz inside a DO with
    open WebSockets) and how CPU time is accounted against the per-request
    limit (`limits.cpu_ms`); the alternative of driving the tick from client
    input messages sidesteps it;
  - a DO is one thread in one colo: everyone in the room plays at the
    first connector's colo latency, and there is no scaling a room across
    cores.

## 4a. Built so far (branch `server-render`)

| step | state |
|---|---|
| 2. seat-multiplexed loop | **done, proven.** `platform/seats.c`; `make seat-test` runs four real instances against one instance with four seats over a scripted four-player session: every framebuffer byte-identical on all 670 frames, desync quantities equal. |
| 3. frame codec | **done, measured** (§1 above). `netplay/framecodec.mjs`, `make framecodec-test`. |
| 1. DO spike, local half | **done.** `make build/katam-server.mjs` is the Workers build (ENVIRONMENT=web, ES module, wasm handed in through `instantiateWasm`). `netplay/world/spike.mjs` under `wrangler dev` (workerd, this machine): boot to the world **100 ms**, **5.8 ms per 4-seat round** including render, encode and `CompressionStream` deflate, a `setInterval` loop paced at **60.28 Hz with 0 late ticks over 10 s**. |
| 1. DO spike, deployed half | **run, 2026-08-28, and it is a wall.** `https://katam-world-spike.sh1ftmaker.workers.dev`: `WebAssembly.Memory` of **128 MB allocates, 160 MB does not**, and instantiating the port's module fails with `WebAssembly.Instance(): Out of memory: Cannot allocate Wasm memory for new instance`. Cloudflare accounts the **reservation** (192 MB), not the ~17 MB touched. Timers work (100 `setTimeout(0)` in a row; `Date.now` is frozen inside a request, so nothing can be timed from inside). The account is on the free plan: `limits.cpu_ms` is refused, so a long-lived tick could not be tested and its CPU accounting stays unknown. See §4b. |
| 4. audio | not started |
| 5. world server + thin client | not started; `netplay/world/katam_host.mjs` is the host the server will drive |

## 4b. The Durable Object cannot hold the memory map, and why remapping is not a fix

The flat map (ARCHITECTURE.md) puts EWRAM at `0x02000000` ... OAM at
`0x07000000` and the ROM at `0x08000000`–`0x09000000`: the linear memory must
reach 144 MB before the port's own data and stack, and a Durable Object stops
at 128 MB, counted at reservation. Two ways around it were considered:

- **Fold the ROM below the RAM regions** (e.g. `0x00100000`) so the map ends
  under 128 MB. The RAM regions cannot move (the ROM holds ~96k words that
  look like RAM pointers -- `gMPlayTable` and friends really are), and the ROM
  cannot move either without rewriting every ROM-internal pointer: `gen_rom_data.py`
  says it plainly -- of 28,590 data labels only 162 are named by C, the rest
  are reached by following absolute `0x08xxxxxx` pointers inside the image.
  A scan finds **55,770 aligned words in the ROM that look like ROM pointers**,
  with no way to tell a pointer from a graphics word that happens to be in
  range. A heuristic rewrite would be a different, unverifiable ROM. **No.**
- **A smaller world in a bigger box.** The seats and the codec are fine; the
  box is wrong. Cloudflare Containers (Workers Paid, 256 MiB–4 GiB, a real
  process with node or the native build) or any ordinary host can run
  `netplay/world/katam_host.mjs` unchanged, with a Durable Object left to do
  what it is good at -- the room, the WebSockets, the input log -- and the
  world process behind it. The native build (`docs/NATIVE.md`) would even
  drop the 192 MB question entirely.

So the honest state of step 1: **the Durable Object is out as the place the
world runs; it stays as the room.** Where the world runs is a platform choice
the numbers do not make for us: a Cloudflare Container keeps everything in
one colo and one bill, a plain host is cheaper and simpler.

### QOI, re-measured on the flying run (asked for; it was 2:1 before)

| | mean | p50 | p95 | max |
|---|---|---|---|---|
| QOI of the raw picture | 52.9 KB | 52.4 | 56.5 | 58.8 |
| deflate of the raw picture | ~15 KB | | | 15.2 |
| QOI of the codec's delta words | 4.0 KB | 2.3 | 15.3 | 56.1 |
| **deflate of the delta (the codec)** | **1.66 KB** | 1.10 | 5.9 | 15.2 |
| QOI of the delta, then deflate | 1.56 KB | 0.93 | 6.1 | 17.7 |

The hub is busier than the level measured in §1 (150 colours, gradients), and
QOI alone is 3.5x worse than deflate on it. QOI-then-deflate on the delta is
6 % better on the mean and 15 % on the median, and *worse* at p95 and max --
it trades the frames that matter (scrolling) for the ones that were already
cheap. Not worth a second pass. `QOI=1 make framecodec-test` reproduces it.

## 5. Recommendation

Do not replace Path B with this; Path B's latency properties are what make
the game feel right, and its desync defences are already built and
measured. But the bandwidth and compute numbers say server-side simulation
is **cheap enough to exist as a second mode** — a "spectate / thin client"
transport for phones and for rooms whose peers keep desyncing — and the
snapshot-multiplexed single instance in §3 is the only shape that fits in a
Durable Object.

If it is pursued, the order that retires risk first:

1. **Spike the two DO unknowns** (§4): deploy a Worker that instantiates the
   real module once and reports `WebAssembly.Memory` allocation success and
   RSS; and a 60 Hz tick with an open WebSocket for ten minutes.
2. Build the seat-multiplexed loop natively first (`platform/rollback.c`
   already has save/restore; a `PortSeatRun(seat)` is small) and prove four
   seats' pictures match four real instances bit-for-bit.
3. The frame encoder with scroll-compensated XOR + deflate, min'd against
   raw deflate per frame (§1).
4. Audio: Opus in the DO, or accept the shared-mix stream and measure.
5. The thin client page.
