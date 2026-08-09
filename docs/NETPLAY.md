# Netplay research: PartyKit, drop-in/drop-out, and the ceiling above four

Research, not implementation. Three questions were investigated on 2026-08-08:
which network stack to build on, what the transport contract at the port's seam
actually is at the code level, and how far the player count could be pushed.
Everything here cites either a source URL or a file:line; claims that could not
be verified are flagged as such. Nothing has been deployed.

This document extends [MULTIPLAYER.md](MULTIPLAYER.md), which describes the
machinery that already exists — the SIO seam, the lobby, the rollback engine,
the join/leave timeline. Read that first. §7 below also lists the places where
that document has drifted from the code.

---

## 1. The stack: partyserver on our own Cloudflare account, not partykit.dev

PartyKit the company was acquired by Cloudflare in April 2024
(https://blog.cloudflare.com/cloudflare-acquires-partykit). Two codebases now
exist and they are not equals:

- **`partykit/partykit`** — the original CLI and managed platform
  (`partykit deploy` → `*.partykit.dev`). Its README defers to the Cloudflare
  repo, and the managed platform is effectively dead:
  https://github.com/partykit/partykit/issues/971 (Oct 2025) reports deploys
  returning 504 and `api.partykit.io` failing DNS, with no maintainer response.
  No formal shutdown announcement was found — but do not build on it.
- **`cloudflare/partykit`** — the actively developed monorepo:
  **`partyserver`** (a `Server` class over Durable Objects) and
  **`partysocket`** (reconnecting WebSocket client). Deploys to **our own
  Cloudflare account with `wrangler deploy`** as a normal Worker + DO binding.

**The decision: `partyserver` + `partysocket`, deployed with wrangler.**
Caveat: `cloudflare/partykit` self-describes as work-in-progress (no 1.0).
The fallback that costs almost nothing is raw Durable Objects with
`partysocket` kept client-side — partyserver is ~100 lines of sugar
(routing, connection ids, broadcast) over native DO WebSocket APIs, so
dropping it later is cheap.

### The primitives, verified against source

- A room = one named Durable Object, addressed
  `/parties/game-room/:name` (kebab-case of the binding), or via
  `getServerByName`.
- Binary WebSocket messages both directions (`WSMessage = ArrayBuffer |
  ArrayBufferView | string`, verified in partyserver src). Received-message
  cap 32 MiB (https://developers.cloudflare.com/durable-objects/platform/limits/).
- **Connection id = the client's `_pk` query param** (partyserver src), and
  partysocket generates `_pk` once per instance — so a reconnecting or
  reloading client keeps its server-side identity if it passes a persistent
  `id`. This is the primitive that makes rejoin-after-blip trivial.
- Ordering: WebSocket over TCP is per-sender FIFO, and a DO is
  single-threaded, so every client observes the same interleaving — the DO's
  arrival order. (Not an explicit doc guarantee; structural.) This matters
  because the transport contract in §3 is exactly-once-in-order.
- Storage: DO storage, ArrayBuffer values ≤ 2 MB on SQLite-backed classes.
  The whole RLE input log of a ten-minute four-player session is ~13 KiB
  (MULTIPLAYER.md §9), so late-joiner history is one or two values, not a
  chunking problem.
- Hibernation (`static options = { hibernate: true }`): the DO is evicted
  when idle, sockets stay open, in-memory state is lost, `onStart` re-runs on
  wake. A 60 Hz room never hibernates mid-game; this is for idle lobbies and
  cost. Anything that matters must live in `conn.setState()` (≤ 2 KB) or
  storage.
- `onClose` is not timely for dirty disconnects (NAT drop, sleep). Presence
  needs an application heartbeat + alarm reaping;
  `ctx.setWebSocketAutoResponse` gives zero-cost pongs. Treat `onClose` as
  advisory — which the rollback design already does, since leaving is a
  scheduled timeline event, not an instant.

### Latency and cost

The room lives in one colo, chosen near the **first** connector
(https://developers.cloudflare.com/durable-objects/reference/data-location/),
overridable with `locationHint`. Versus a direct WebRTC datachannel, the relay
adds roughly one hop through the DO colo plus TCP-vs-UDP; for same-region
players an estimated +10–40 ms RTT (estimate, no published benchmark). The
rollback engine exists precisely to absorb this. If it ever isn't enough, the
upgrade path is a WebRTC datachannel mesh with the DO doing only signaling —
`stun.cloudflare.com` is free, Cloudflare TURN is $0.05/GB after 1 TB free.
Not worth the client complexity for v1.

Cost is a non-issue: incoming WS messages bill 20:1 as requests, outgoing are
free (https://developers.cloudflare.com/durable-objects/platform/pricing/).
A full-rate 4-player room ≈ 43,200 billable requests/hour ≈ **$0.0065/hour**
on the $5/mo Workers Paid plan; the free plan (SQLite-backed DOs) covers
~2.3 hours/day of full-rate play.

A verified-API server/client sketch (slot assignment, binary relay tagged with
sender slot, chunked append-only log, history replay to joiners) is in the
research transcript and transfers nearly verbatim when implementation starts;
it is deliberately not inlined here because the real server's wire format
falls out of §3.

---

## 2. Two architectures, and which one drop-in/drop-out forces

The port has **two parallel, mutually exclusive input paths**, and the biggest
finding of the code-level research is that they really are exclusive —
MULTIPLAYER.md hints at this but the mechanism is concrete:

**Path A — SIO-faithful.** The transport is a cable: relay halfwords, the
game's own MultiSio lockstep does the rest. The game is unmodified. But it is
lockstep — five frames of input delay, hard stalls when a peer is late, the
session formed once in the lobby, no joining after the fact, four units ever.

**Path B — rollback-direct.** The transport bypasses MultiSio and feeds the
rollback engine (`PortRbSetLocalInput` / `PortRbConfirmInput`), which writes
the game's per-slot input words directly (`platform/rollback.c:576-600`).
Joining and leaving are timeline events; a joiner replays the RLE log. This is
the only path that gives *seamless drop-in/drop-out* — the thing this branch
is for.

The conflict, precisely: with a live link session, `GameLoop` runs the game's
own input unpacker (`sub_08030FE0`) *after* `GetInput` and *before*
`TasksExec`, while `PortRbFrame` runs inside `VBlankIntrWait` — so the game
clobbers rollback's injected inputs every frame. **Path B therefore requires
suppressing `sub_08030FE0`** (a portify substitution), plus setting the two
things the lobby normally sets: `gUnk_0203AD10 |= 2` (selects the network
input branch, `src/kirby.c:6459-6463` in the decomp) and `gLocalPlayerId`
without reading SIOCNT. This is the one place the design stops being "the
game, unmodified" — MULTIPLAYER.md §8 predicted exactly this trade.

**Recommendation: build Path A first as a milestone, ship Path B.**
Path A between two real browser tabs exercises the transport's hardest
property (exactly-once ordered relay, §3) against the game's own lobby with
zero new game patches — it is the cheapest way to find transport bugs, and it
is also the first time two real instances will ever have talked. Then Path B
reuses the same socket and room for the actual product. The lobby UX for
Path B doesn't use the game's lobby at all: sessions form in the page
(room code → PartyKit room), and the game is put directly into
network-input mode.

---

## 3. The transport contract (what the code actually requires)

Everything a PartyKit transport must satisfy, read off `platform/mp.c`,
`platform/sio.c`, `platform/mp_js.c`, `web/shell.html`. File:line references
are in the research transcript; the load-bearing facts:

1. **The child free-runs.** `Armed()` (`platform/sio.c:125-130`) returns 1
   unconditionally for `selfId != 0`, so a child instance attempts 16
   transfers every frame *regardless of what the parent did*. The transport is
   the only pacing mechanism: buffer the parent's actual transfers as an
   **ordered queue of individual halfword-transfers**, serve one per
   `exchange`, and **return 0 when the queue is empty** — a stall ends the
   frame's transfers cleanly. This is the single biggest hazard for a relay
   and is documented nowhere but the code.
2. **Exactly-once, in-order, per-transfer.** Duplicating the parent's last
   word resets the child's lobby settle counter every transfer; padding with
   `0xFFFF` resets the parent's recognition counter. Both failure modes look
   like "the lobby never completes" with no error.
3. **Variable cadence.** The parent produces 1–2 transfers/frame in the lobby
   and 16 in play. The wire format must carry a per-frame transfer *count* —
   a fixed 16-halfword blob cannot carry the lobby. (MULTIPLAYER.md §7 item 1
   is wrong about this.)
4. **Slot assignment is authoritative and out-of-band.** The room assigns
   `selfId`: first joiner is slot 0 and clocks the cable; a second peer
   **must** be slot 1 (the game's classifier requires contiguous slots — a
   slot-2 child with no slot 1 never leaves recognition). If two peers both
   claim slot 0, recognition silently never happens on either side.
5. **`poll` runs once per frame from attach onward**, even before the game
   enters multi-play mode, at a fixed heap address a JS transport may cache.
   `exchange` is synchronous (`EM_JS`, not `EM_ASYNC_JS`) — it cannot await.
   Returning a Promise from JS would be truthy and silently corrupt the
   protocol. Drain the socket in `poll`; answer `exchange` from the buffer.
6. **Keep `link.error` at 0.** The error bit propagates into the lobby's
   `unk03` and restarts the lobby task every frame. Also: a transport silent
   for 180 frames mid-lobby causes a lobby restart (the game's own
   watchdog), not an error.
7. **Never write your own slot** — the caller overwrites `recv[selfId]` with
   `send`, so a transport needs no knowledge of its own slot to be correct.

## 3a. Milestone A: done, and what the wire taught

**Two real instances now link through the game's own lobby and play a
two-player session.**  `tools/netplay_test.mjs` runs two module instances in
one node process against `netplay/dev-relay.mjs`, drives both through
title → FILE 1 → START GAME → MULTIPLAYER, and passes on the game's own
state: `gUnk_03002558` set with two human players on both sides, sustained,
with the two framebuffers 97.7 % identical at sample time (the residue is
the one-frame parent/child skew).  MultiBoot recognition, the counter
handshake, `0xE4E4`, MultiSio validation (`0x8393`/`0x8303` steady), the
world-properties transfer and the in-play sub-lobby all run end to end.
`make netplay-test` runs it.

Building it revised §3.  The contract as measured, not merely derived:

1. **Lobby words are sampled registers, not queues.**  A transfer reads
   whatever each unit's SIOMLT_SEND holds; a late peer is read *again*, not
   waited for.  Stalling the parent instead leaves SIOCNT's busy bit set
   across the frame, and the lobby's counter phase (`sub_0803040C`) treats
   any of bits 2-7 beyond SD as a broken cable and restarts the handshake —
   on hardware busy clears ~228 cycles after the arm, so a transport that
   holds it for a round trip presents hardware that does not exist.  This
   was the parent's probe→counter→probe regression loop, found by
   `PortMpSetTrace`.
2. **Play words are strict streams, but a missing word is noise, never a
   stall and never a repeat.**  A repeat shifts MultiSio's packet framing
   into the checksum; a stall couples the parent's *intra-frame* interrupt
   chain to a network round trip and collapses it to a measured
   one-transfer-per-frame equilibrium.  A missing word is served as 0x0000:
   at worst one checksummed-away packet against eight frames of input
   redundancy.  The lobby/play switch is readable from the IO mirror in the
   module's own heap — serial-interrupt enable is set through the lobby and
   cleared by the MultiSio parent.
3. **The child is clock-gated and may catch up.**  Slot 0's stream is the
   cable clock; the child stalls without it, and drains backlog at up to
   32/frame through the transport's `pending()` hook.
4. **Delivery must beat the frame loop.**  A scheduler that lets game
   frames win the event-loop race against socket delivery starves the child
   into 0-then-32-word frames, which starves MultiSio's 8-frame validation
   windows, which put the sub-lobby state machine into a teardown loop that
   clears its own send buffer.  Real browsers order it correctly for free
   (16 ms frames, ~1 ms delivery); the harness had to stop using
   `setImmediate` for rAF.
5. **`sub_080324BC` reads `gWorldProps[7]` off the end of a seven-entry ROM
   table** on the last block of the world-properties transfer, before the
   bounds check — open bus on hardware, a wasm trap here, and unreachable
   until two real instances got this far.  `CpuSet`/`CpuFastSet` now refuse
   out-of-map endpoints the way DMA always has (`platform/bios.c`), which
   retires the whole class.
6. **The §6 menu script's timings only mean what they meant under the
   harness's latch semantics** — the mash window's last value stays held.  A
   naive release adds one extra A edge, and one extra press picks single
   player.

## 3b. Milestone B: done -- drop-in, drop-out, over the timeline

**Seamless drop-in/drop-out works.**  `make netplay-rb-test`: three
instances, one relay, no game lobby anywhere.  Two founders boot the same
scripted single-player world, activate at the same frame
(`PortRbInit` + `PortRbNetPlay` + `PortRbSetSelf`, all new exports), and
play different inputs with rollback absorbing the latency -- their desync
quantity (gRngVal plus every Kirby's x and y, the game's own measure) is
bit-equal at the checkpoint.  One founder disconnects; its Kirby goes to
the AI by a relayed timeline event, announced by the lowest surviving seat.
A third instance, held at the activation frame, fetches the room's input
history, replays ~1300 inputs to the live frame with the picture off in
well under a second, takes the vacated seat by the same event mechanism,
plays -- and is bit-equal with the survivor at the second checkpoint.

The driver is `web/rb_net.js` (one 7-byte MSG_INPUT per frame per player,
JSON `assign` events relayed to everyone including the sender); the relay
stores history per room and serves it in MSG_LOG batches.  On this path
`gUnk_03002558` stays 0 -- none of the game's link machinery runs, so the
`sub_08030FE0` clobber §4 worried about never happens and **no game patch
was needed at all**.

Three bugs found by the bit-equality bar, each now a rule:

1. **The engine is the only authority on frame numbers.**  The driver
   stamped outgoing inputs with a host-derived frame; it disagreed with the
   engine's own `sFrame` by one, so a sender applied its input at frame S
   while telling everyone S-1 -- every edge landed one frame apart on the
   two sides, measured as an AI Kirby reacting one frame later on one
   instance with end positions still equal.  `PortRbSetLocalInput` now
   returns the frame it recorded, and the wire carries exactly that.
2. **Snapshot before the frame's inputs are applied.**  The ring snapshot
   sat after `ApplySlotInputs`, so a re-simulated frame recomputed its
   pressed/released edges against its own already-applied words.  The
   self-test never caught it -- replaying identical inputs only trips this
   when an edge lands exactly on the snapshot frame.
3. **`PortRbInit`'s player count is part of the shared initial
   condition.**  A joiner that inits a different count than the founders
   maps the AI slots to empty peer columns -- its AI Kirbys stand still
   while everyone else's wander.  `join(depth, players)` takes it
   explicitly; a session-parameters message is the eventual home.

Known cosmetic gap: `sConfirmed` (and so `PortRbDescribeSession.logBytes`
freshness) lags while a seat is AI-driven, because confirmation waits on
every player column including unseated ones.  Harmless today; tighten when
the RLE log replaces raw history on the wire.

## 4. The gaps that are actual work items on this branch

Found by the code research; each is small, and together they are the real
to-do list in front of any transport:

- **Exports.** The rollback/join API is C-only today. The web build's
  `EXPORTED_FUNCTIONS` (all four link recipes in the Makefile) needs:
  `_PortRbConfirmInput`, `_PortRbSetLocalInput`, `_PortRbInputAt`,
  `_PortRbAssignSlot`, `_PortRbSuggestEventFrame`, `_PortRbDescribeSession`,
  `_PortRbVacantSlot`, `_PortRbJoin`, `_PortRbEncodeLog`, `_PortRbDecodeLog`,
  `_PortRbReplayTo`, `_PortRbCatchingUp`, `_PortRbGetStats`,
  `_PortFrameNumber` — plus **`_malloc`/`_free`**, without which JS has no
  scratch memory for the log blob or the out-param structs.
- **Catch-up pacing.** `PortRbCatchingUp()` has **zero callers**. The web
  host awaits one `requestAnimationFrame` per frame unconditionally
  (`platform/web/host_web.c:39-41`), so a ten-minute log replay would take
  ten minutes of wall clock today, not the measured 0.19 s. The fix is a
  batched loop (N sim frames per rAF while catching up) at `main.c:872` or
  in the host — small, but it must exist before joining works.
- **`PortRbSetLocalInput` has no caller**, and `PortRbInit` today freezes the
  local player's controls (the timeline override always fires, feeding back
  the last known input). A transport must call it every frame; the first
  integration test will hit this immediately.
- **`PortRbJoin` requires a prior `PortRbInit`** — a joiner creates a session
  before joining one. Fine, just undocumented.
- **Suppress `sub_08030FE0` under rollback** (Path B only) — a portify
  substitution gated on a port-side flag, plus setting `gUnk_0203AD10 |= 2`
  and `gLocalPlayerId` from the transport side.

## 5. Sixteen players: feasible, with two real ceilings

Asked because the game has 14 spray-paint colours plus 4 id-defaults — ~18
distinguishable Kirbys, so visual identity is *not* the constraint. The
constraints are elsewhere, and the network is not one of them: 16 forces
Path B (the SIO id field is 2 bits in hardware and the game reads its own
identity out of SIOCNT — `src/code_080332BC.c:38` — so the cable caps at 4),
and on Path B the engine's player count is one `#define`
(`PORT_RB_PLAYERS`) and a malloc'd timeline that already scales.

**Ceiling 1, the hard one: per-Kirby room streaming.** Every Kirby owns an
eagerly-decompressed copy of its room's tilemap and *collision maps* —
**21.3 KiB per Kirby** — drawn from a pool of exactly 4 buffer slots
(`FillLevelInfo`, `build/port-src/src/code.c:411-455`; the pool's free-slot
mask is a `u8` and the scan is hardcoded to 4). Kirbys in the same room share
a slot, but the worst case (16 Kirbys, 16 rooms) needs 16 slots = ~341 KiB of
live room state — in a 256 KiB EWRAM whose bottom 128 KiB is the task heap
and whose measured slack is 20.5 KiB, less than one extra player. Collision
reads go through these buffers, so remote Kirbys cannot be simulated without
their rooms resident: **the pool is a simulation resource, and its cost
scales with players.** The escape is port-specific: the address space above
EWRAM (`0x02040000..0x03000000`) is empty in the port's memory map, symbols
are generated address macros with an existing override hatch
(`tools/gen_ram_symbols.py`), and portify already relocates a whole region
(SRAM) by codemod — so growing and relocating the pool is genuinely
available, at the cost of a bigger (still cheap) snapshot.

**Ceiling 2: OBJ palettes and OBJ VRAM are statically partitioned by Kirby
id.** Each Kirby owns 2 of the 16 OBJ sub-palettes and 2 KiB of the 32 KiB
OBJ VRAM, chosen from `unk56` at object-creation time in simulation code
(53+ sites). Sixteen Kirbys want 32 sub-palettes and 100 % of OBJ VRAM — a
hardware ceiling the port's PPU emulates faithfully. The cheap mitigation is
recycling by `unk56 & 3` — deterministic on every client, so it cannot
desync; players 4..15 share the first four looks. Sixteen *visually distinct*
Kirbys needs a port-side PPU extension past the console's limits — possible,
but the first place the port's PPU would stop being the hardware.

**Bit-width traps are mostly absent.** The per-object owner tag `unk56` is a
plain `u8` (not a bitfield); the real traps are enumerated (T1–T9 in the
research transcript): the SIOCNT id read, a `u8` player bitmask whose bit 4
is already the "all players" sentinel (`Unk_03000510`), one hardcoded `< 4`
in the cell-phone path, difficulty tables in ROM sized `[27][4]` that index
off the end at count > 4 (67 read sites — clamp with a macro), ~23
player-indexed `i < 4` loops, and 63 unrolled `gKirbys[0..3]` references.

**The honest tiers:**

| tier | scope | size |
|---|---|---|
| 0 | four players, everything in MULTIPLAYER.md, no real far end yet | done |
| 1 | PartyKit transport, real 2–4 player drop-in/drop-out, **no game patches beyond `sub_08030FE0`** | ~1–2 weeks |
| 2 | 5–8 players: relocate/grow the per-player arrays, widen the room pool to 8, patch the traps, palette recycling (players 4–7 wear colours 0–3) | ~4–6 weeks |
| 3 | 16 players: room pool to 16 (+260 KiB relocated), all consumers regrown; distinct looks need a PPU extension | ~3–4 months |

Tier 1 is this branch. Tier 2 is real and bounded. Tier 3's load-bearing
third is the room pool, not the netcode.

---

## 6. Proposed implementation order

1. **Exports + JS glue.** The §4 export list, `_malloc`, and the catch-up
   pacing hook. Verifiable headless: drive `PortRbConfirmInput` from
   `tools/headless_test.js` and watch the state hash.
2. **The room server.** partyserver `GameRoom`: authoritative slot
   assignment (first = 0), binary per-transfer relay tagged with sender slot
   and per-frame count, append-only log in DO storage, history on request,
   heartbeat + alarm reaping. Local dev via `wrangler dev` — no deploy
   needed to develop against.
3. **Milestone A — two real tabs through the game's own lobby** (Path A).
   The first time two real instances talk. Validates §3's contract the hard
   way; every transport bug found here is one not found under rollback.
4. **Milestone B — drop-in/drop-out** (Path B): suppress `sub_08030FE0`,
   session-form in the page, feed the timeline both directions, join via log
   + catch-up, leave via scheduled AI handover. This is the deliverable.
5. **Later, if latency bites:** datachannel mesh, DO for signaling only.

Deployment of the Worker (a Cloudflare account action) is deliberately out of
scope for this document and needs a human decision first.

---

## 7. Corrections owed to MULTIPLAYER.md

Found while reading the code against it; left uncorrected there for now:

- §3's table still says MultiBoot is "not implemented / on the critical
  path"; §11–12 of the same document describe it implemented and the lobby
  completing. (`web/shell.html:3408-3412` repeats the stale claim.)
- §6 "what is not verified" still says nothing above the packet layer has
  run; §12 shows the lobby running to completion.
- §7 item 2 says a transport can "stall in `poll`" — `poll` is void and
  synchronous; the real mechanisms are returning 0 from `exchange` or
  reporting the link down.
- §7 item 1's "send the frame's sixteen halfwords as one message" does not
  survive the lobby's 1–2-transfer frames (§3.3 above).
- §8/§10's "0.19 s replay in a browser" is a property of the simulation, not
  of any code path that exists — the pacing hook is unwritten (§4 above).
