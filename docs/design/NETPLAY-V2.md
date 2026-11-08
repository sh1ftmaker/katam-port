# Netplay v2: deterministic simulation, input-only sync

Design, 2026-11-08. Nothing here is implemented yet. It targets the hard fork
(branch `fork`, game source in `game/`) and the planned GPU renderer, where
rendering becomes a read-only view of simulation state.

Conventions:
- `port-src/…:N` means `build/port-src/src/…` as generated at `main` b59e25e.
  The fork's `game/src/` has identical line numbers for every file cited here
  except `init.c`, `pause_*.c` and `subgames.c`.
- `main:platform/…:N` is the port layer at b59e25e.
- **Measured** means I ran it, in the scratchpad, against an lp64 build of
  `main` and the real ROM. **Cited** means a measurement from the existing
  docs. **Unverified** means read from the code but not run.

---

## 0. Summary

1. **One model: rollback netcode, with only inputs and session events on the wire.** Every peer runs the whole
   simulation (sim). All four Kirby slots always exist. Each slot is driven
   either by a human's input column or by the game's own AI, and that choice
   changes only through an event stamped with an agreed frame.
2. **The Cloudflare Durable Object (DO) is the session authority, not a game host.** It assigns slots,
   orders events, keeps the input archive, settles disconnects, compares
   state hashes, and relays snapshots and signalling. No peer is special, so
   when the "host" leaves nothing has to migrate.
3. **Inputs travel peer to peer where possible.** They go over unreliable,
   unordered WebRTC data channels, which work in browsers (wasm) and in native
   builds through libdatachannel. Each packet re-sends every input the
   receiver has not yet acknowledged. If a pair cannot connect directly, the
   fallback is Cloudflare TURN, then the DO's WebSocket.
4. **Joining is snapshot plus tail, not full replay.** A joiner receives a
   compressed snapshot of a *confirmed* frame (about 10 KB measured, once the
   sim and presentation are split) and the inputs since that frame. It
   catches up, then takes a seat through an event. The same mechanism repairs
   a desynced peer.
5. **Portable snapshots need three things the codebase lacks today.**
   (a) Function pointers stored in game memory must be canonical rather than
   host addresses. (b) Simulation state that lives outside the GBA memory map
   must move into it. (c) Per-peer presentation state must be separated out.
   Until all three land, joining falls back to the existing input-log replay,
   which works across platforms because it carries only numbers.
6. **Determinism is the gating risk, and one bug is proven.** The native build
   (GCC) and the web build (clang) evaluate unsequenced `Rand16()` calls in
   different orders. I measured this, and it will desync browser and native
   players (§6, R1). Every sim build must use the same compiler and the same
   language mode, the 42 affected sites need fixing, and libm must come out
   of the sim.

---

## 1. Verdict on existing pieces

| piece | verdict | reason |
|---|---|---|
| **Path A**: emulated link cable plus the game's MultiSio lockstep (`sio.c`, `multi_sio_intr.c`, `mp*.c`, the link takeover in `mp_loopback.c`) | **Retire from online play.** Freeze it on `main` as a reference. | The session forms once, so players cannot drop in. The cable caps play at 4 units and the game reads its own identity from SIOCNT (`port-src/code_080332BC.c:43-44`). Latency is hidden by lockstep stalls, not prediction. The bus has intra-frame causality that cannot be paused (NETPLAY.md §3d). The link takeover proved the useful idea, which is that the game's link play is input-lockstep co-simulation. v2 keeps the idea and drops the bus. |
| **Server-side render** (`seats.c` as a play mode, `framecodec.mjs`, `netplay/world/`) | **Retire.** Keep `seats.c` only as the census and harness tool (§3.4, §9). | The input latency is unhidden. The DO cannot reserve the memory map (128 MB limit counted at reservation; SERVER-RENDER.md §4b). It requires uploading the ROM to a server. Audio would have to be streamed. Input-only sync makes spectating nearly free (§8), which removes its last use. |
| **Seats** (four game states in one instance) | **Keep as a tool, not product.** | The renderer split makes several viewpoints a presentation concern over one sim. Swapping game states by memcpy is exactly what the in-process multi-peer harness needs (§9.1), and running four seats with different `gLocalPlayerId` is the divergence census (§3.4). |
| **DO relay** (`netplay/server.mjs`, `protocol.mjs` RoomCore, partyserver) | **Keep the platform, rewrite the protocol.** | Durable Objects are the right home for a single-threaded sequencer, a persistent identity per connection (`_pk`), cheap storage and signalling. The SIO word-stream protocol (MSG_WORDS, MSG_PAYLOAD) goes. `RoomCore`'s split (logic testable without a socket, shared by the DO and `dev-relay.mjs`) stays. |
| **`rollback.c`** (ring, timeline with known bits, events, seal, stall gate) | **Keep the concepts, rework the code** (§2, M3). | The good parts stay: prediction by repeating the last known input, the stall gate, events applied at the top of the frame, the seal, and snapshotting before inputs are applied (the bug fixed in NETPLAY.md §3b). What changes: the slot map, story floor and focus are timeline-derived state held *outside* the snapshot (`main:platform/rollback.c:258-288`); the local column is fed into the global key latch (`:858`, R8); events on the same frame are ordered by arrival (`:609-630`, R12); and the player count is pinned at 4 (`:744`), which also pins enemy difficulty at 4 players (`gUnk_08351530[..][gNumHumanPlayers-1]`, e.g. `port-src/bombar.c:668`). |
| **RLE input log** (`PortRbEncodeLog`, the game's own 10+6-bit runs) | **Keep it as the archive and replay format, not as the join path.** | Its 17x compression is measured, and it remains the cross-build fallback join until snapshots are portable. Rework it to carry session parameters, the content hash, events with their sequence numbers, and seals; widen the run count beyond 64. Unify it with the fork's `--record/--replay`. |
| **Boot-to-world script** (`web/rb_boot.js`, F0=2200) | **Retire after M5.** | Snapshot join removes the need to replay from power-on. Session start becomes "the creator's state at frame S" (§4.1). |
| **`PortRbShouldStall` plus the host pump** | **Keep.** | Freezing at the edge of the rollback window is the correct failure mode (§2.5). |

---

## 2. The model

### 2.1 Simulation step

One sim frame is one iteration of `GameLoop` (`port-src/main.c:220-282`),
from the input latch through `TasksExec` to the VBlank handler, at a fixed
59.7275 Hz. Its contract is
`step(simState, inputs[4], events) -> simState` (plus presentation outputs).
The fork should turn `GameLoop` into a `SimStep()` that returns each frame, so
that neither Asyncify nor stack-shape arguments are needed. The only other
places that block on VBlank are `WaitForInput` (`main.c:578-603`, sleep mode,
disabled in sessions, R8) and the boot health screen (`init.c`, before any
session exists).

### 2.2 What goes on the wire per frame

The input word is a `u16`. Bits 0-9 are the GBA buttons
(`PORT_RB_KEYS_MASK 0x3FF`). Bits 10-15 are reserved for virtual inputs, such
as mod-defined actions (§11); they are zero in vanilla.

The **INPUT packet** is unreliable and unordered. It goes peer to peer, or
through the relay with an identical layout, and one is sent per sim frame
(two frames may be coalesced on bad links):

```
u8   type = 0x20
u8   fromPeer
u16  pktSeq                 // per-destination, for RTT and loss stats
u16  ackPktSeq              // newest pktSeq heard from the destination
u8   ackDelay               // ms between receiving ackPktSeq and this send (RTT)
u32  ackFrame               // "I hold all of your inputs for frames < ackFrame"
u32  localFrame             // frame I will simulate next (time sync, §2.4)
u32  firstFrame             // = destination's ackFrame of my stream
u8   n                      // frames carried, <= 32
     n x 10-bit inputs, packed   // ceil(10n/8) bytes
[TLV...]                    // optional: HASH (§5), PING
```

The header is 21 bytes. With 3 to 8 unacknowledged frames, which is typical,
the payload is 25-31 bytes. Every packet re-sends all unacknowledged inputs,
so losing *k* consecutive packets costs nothing as long as one packet arrives
inside the rollback window. Duplicate and reordered packets are harmless,
because inputs are keyed by frame and are immutable.

Sizes per link, per direction, at 60 Hz:
- application payload: about 1.8 KB/s;
- on the wire, including UDP/IP, DTLS and SCTP overhead (about 60-70 B per
  packet): about 6 KB/s;
- a 4-player mesh: about 18 KB/s up and 18 KB/s down per peer;
- a spectator, fed by the DO in batches every 6 frames: about 0.5 KB/s.

**Events** are reliable and ordered, and come only from the DO over the
WebSocket. Each is `{seq u32, frame u32, type u8, args}`. Types:
- SLOT (slot ← peer or AI);
- SEAL (a column's inputs end at frame L, with the authoritative tail);
- PARAMS (session parameters);
- PAUSE / RESUME (session-level);
- MOD (a sim-relevant mod toggle), if ever allowed mid-session.

They are applied at the top of `frame`, in `(frame, seq)` order.

### 2.2a The simulation view

The game's spawn and despawn windows are sized for the world view a player is
shown (docs/VIEW.md, "Spawn margin"). That makes the view size simulation
input. The fork already carries it as one:

- `PortSimViewRequest(w, h)` is the host's request, rounded to whole cells.
- `view FRAME W H` is a frame-stamped event in the input log.
- `PortInputLogViewAt(frame)` is a pure lookup, applied at the latch, so a
  rollback that re-latches a frame gets the same value.

A session must give every peer the same value. Until it can, the engine holds
the view at 240x160 whenever a rollback session or the game's link is active,
or after a page has called `PortSimViewPinNative` (the `web/` drivers do this
on creation).

The v2 design:

- Each peer's request travels as an event: `VIEW {seq, frame, peer, wCells,
  hCells}`, scheduled past the rollback window
  (`PortRbSuggestEventFrame`).
- The session view at frame F is the per-axis max of each seated peer's
  latest request at or before F. It is computed by every peer from the same
  event stream, and replaces `PortInputLogViewAt` as the source for
  `PortSimViewLatch`.
- A peer that leaves stops counting from the frame its seat is sealed.
- A spectator's request never counts.
- A joiner gets the event history with the input history, so its replay
  reaches the same value.

The netsim branch plans to keep the latched value (`struct PortSimView` in
`platform/view.c`: `viewW, viewH, prevW, prevH, stock`) in the port sim block
(`platform/port/simblock.h`, 0x02040000). Hashing and snapshotting it there
would cost nothing.

### 2.3 Input delay and prediction

- **Prediction:** repeat the last known input. This is right about 96% of the
  time (MULTIPLAYER.md §9, mean run length 22.8 frames). Do not try anything
  cleverer: every peer must make the same prediction for a misprediction to
  stay cheap.
- **Local input delay *d*:** the input sampled at local frame *f* is stamped
  *f + d*. The default is 2. Adapt it per peer, at most one step per second,
  with hysteresis: `d = clamp(ceil(oneWay95 / 16.74ms) - 5, 1, 4)`, where
  `oneWay95` is the 95th-percentile one-way latency to the slowest seated
  peer. This keeps the expected rollback depth at 5 frames or less.
- **Changing *d* never leaves a hole.** Raising it stamps one sample twice;
  lowering it drops one sample. Each peer's stream is authoritative for its
  own column, so this is deterministic for everyone. The delay is local
  policy and never needs agreement.

### 2.4 Time sync and frame-advantage balancing

Each peer estimates every other peer's current frame as
`remote.localFrame + RTT/2 / 16.74ms`, and its advantage `a = localFrame - est`.
Following GGPO, both sides exchange their advantages and use the symmetric
`(a_local - a_remote)/2`, averaged over 1 second. If that exceeds 0.5 frame,
the peer that is ahead *slows its tick rate*, by up to 2% (about 1 frame per
50 frames). This replaces GGPO's whole-frame stalls. It works because the
renderer is decoupled from the sim, so a 1-2% change in rate is invisible.
None of this touches sim state.

### 2.5 Rollback window and what happens past it

- **Window:** `W = 10` frames past the newest confirmed input of the slowest
  seated column. The ring holds 12 snapshots. Delay (up to 4) plus window
  covers about 233 ms one way before any stall.
- **Cost (cited):** a 10-frame rollback is one restore plus 10 sim frames.
  Natively that is 4.7 µs + 10 × 2.5 µs ≈ 30 µs. In wasm it is about 60 µs
  for the sim plus an estimated 20-40 µs for the memcpy. Either way it is
  under 1% of a frame. The real cost of rollback is the visible correction,
  not the CPU. Lua mods change this (§11).
- **Past the window, freeze; never diverge** (as `PortRbShouldStall` does
  today). The present shows the last frame. After 0.5 s the UI says "waiting
  for <name>". After 1.5 s of silence, which the DO's heartbeat detects or
  peers report, the DO starts the SEAL procedure (§4.4) and the slot goes to
  the AI.
- **The DO's event stream is a column too.** The DO sends
  `WATERMARK{frame}`, meaning "no events before *frame*", every 100 ms, about
  60 frames ahead of its estimate of the session frame. Peers never simulate
  past the watermark. Events are scheduled at least 30 frames ahead, so they
  are confirmed before they apply and are never mispredicted. A DO outage
  shorter than about 1 s is invisible; a longer one freezes play until the
  connection comes back.

---

## 3. Simulation vs presentation

### 3.1 Memory classes

- **S (sim):** snapshotted every frame, hashed, transferred to joiners.
- **P (presentation):** never snapshotted, may differ per peer, recomputed
  from S.
- **S\* (sim until the split):** memory that sim code writes today and may
  read back, so it has to stay in the snapshot. It is excluded from hashes by
  a mask (§5.2) and becomes P once the renderer split lands.

| region / symbol | class | notes |
|---|---|---|
| EWRAM task heap `gEwramHeap` 0x02000000+0x20080 | S | Game objects. They embed S\* fields: `sprite.tilesVram`, screen-space `sprite.x/y`, palette slots. |
| `gKirbys` 0x02020EE0, `gCurLevelInfo[4]` 0x02023530 (including room buffers, 21.3 KiB per slot), AI `gUnk_02038590` (244 B per slot), input block `gUnk_020382D0`, mode flags 0x0203AD10.., `gRngVal` 0x030068D8, `gFrameCount`, `gTasks` 0x030019F0, `gIwramHeap` 0x03003A20 | S | Holds host function pointers (R5). `LevelInfo` holds per-peer viewport fields (`viewportModX_44`, R-census). |
| `gLocalPlayerId` 0x0203AD3C, display task `gUnk_02023354` | P | Differ per peer by design; re-pinned every frame today (`GAME_REPIN_FOCUS`). |
| `gHeldKeys/gPressedKeys…` 0x03002E90.. | S | Must hold the *menu owner's* input, not the local peer's (R8). |
| `gIntrTable` 0x030017B0, `gSoundInfo`, `gMPlayInfo_*`, `gMPlayTrack_*` (EWRAM), `SoundMainRAM_Buffer` | P | Audio/platform. Host-pacing-dependent (cited, DEBUG-TOOLING §7). Two sim reads must be removed (R10). |
| `gBgPalette`, `gObjPalette`, `gOamBuffer`, `gBgScrollRegs`, `gRgbMap`, BG/OBJ/VRAM upload queues (`gUnk_03002EC0`, `gUnk_03002400`) | S\* | Shadow registers and upload queues, drained against the modelled VBlank budget (`main.c:535`, `bg.c:69`, `sprite.c:276`). |
| `gVramHeapState`, `gVramHeapStartAddr`, `gVramHeapMaxTileSlots` | S\* | The OBJ VRAM allocator. Its results are stored in sim objects (`include/object.h:246`). |
| IO 0x04000000 (1 KiB) | S\* | DMA mirrors hold *host* addresses (R6). DISPSTAT/VCOUNT are modelled. |
| PLTT, VRAM, OAM (98 KiB) | S\* | In the snapshot today, P after the split. |
| SRAM 0x0E000000 (64 KiB) | **S (new)** | The session's save. The game reads it during play (world props, progress). Snapshot it on write only. |
| Out-of-map game and port state: `gSaveFileInfo`, `gWorldProps` (save.c, host .bss), `gInputRecorder`/`sTapeBuffer`, rando `sListBuf/sListHdr/sListParity` (`main:platform/rando.c:354-356,439`), rollback's `sSlotPeer/sSlotsInPlay/sStoryFloor` (`rollback.c:258-264`), the warp queue, `sKeysDown/sVBlankBudget` | **S, but missing from snapshots today** (R7) | Move all of it into a **port sim block at 0x02040000** (the empty EWRAM extension; see NETPLAY.md §5), reserved on every platform. |
| Mod sim store (§11) | S | It belongs in the port sim block. |

The writable symbols outside the map came from `nm` over the game objects of
an lp64 `main` build (measured).

### 3.2 Snapshot size and cost

| | raw | compressed (deflate-6 / xz) |
|---|---|---|
| today: whole map + host state (cited, MULTIPLAYER.md §8) | 387 KiB | - |
| **measured** EWRAM + IWRAM, hub world, 4 Kirbys, frame 2600 | 288 KiB | **9.3 KB / 7.4 KB** |
| measured VRAM, same frame | 96 KiB | 37 KB / 32 KB |
| frame-to-frame EWRAM change, idle (measured) | 46 bytes changed | 0.36 KB |
| **after split**: EWRAM + IWRAM + port block (+ SRAM when dirty) | ~292 KiB | ~10 KB (estimate: 15-40 KB in a busy level) |

Only 96 of the 1024 256-byte blocks in EWRAM were non-zero at the measured
point. A busy level will be larger; how much is unmeasured.

Per-frame cost after the split:
- **Save:** a 292 KiB memcpy, about 4 µs natively (scaled from the cited
  5.0 µs for 387 KiB) and an estimated 20-40 µs in wasm.
- **Restore:** the same as save.
- **Hash:** xxh3-64 over 292 KiB, done only at checkpoints (§5): about 20 µs
  natively and an estimated 60 µs in wasm with SIMD.
- **Dirty-page tracking:** not worth it at these sizes.

### 3.3 What blocks dropping VRAM, OAM, palettes and audio from the state today

None of these blocks the netcode. They block *shrinking* the state and
*hashing it unmasked*, and they disappear with the renderer split.

1. **VRAM allocation is coupled to sim objects.** The sim calls `VramMalloc`
   and stores the result in object structs (`include/object.h:246`,
   `malloc_vram.c`). On failure it returns `ewram_end`. The split replaces
   this with presentation-side tables keyed by object.
2. **Upload queues are drained inside the sim frame against a VBlank budget**
   (`port-src/main.c:528-539`, `bg.c:60-72`, `sprite.c:276`). The drains
   advance IWRAM queue indices. This is deterministic today, but it is
   presentation work mutating sim memory.
3. **Palette state round-trips through sim objects**
   (`code_08026044.c:239,920,1516` copy `gRgbMap` into an object and back).
4. **The sim reads audio state** at `collection_room.c:1748`
   (`gMPlayInfo_0.status` decides `unk30`) and at `code_0806F780.c:9382` (a
   compare against `songHeader`). The `PlaySfx` gates (`include/data.h:15-35`)
   read music-player state, but only to decide whether a sound plays.
5. **`gLocalPlayerId` has 502 reads in sim code.** Most are HUD, camera and
   sound gating. Some write sim structs for the local slot only: camera shake
   at `cutscene_trigger.c:965-976` (presentation-only: `viewportModX_44`
   reaches only `scrollX`, `code_080023A4.c:837`), `dark_mind.c:6263`
   (`gCurLevelInfo[local].unk0 = 1`), and
   `FillLevelInfo → sub_08003028(playerId, playerId == local)` (`level.c:167`).
6. **No sim read-back of VRAM, OAM or palette *contents* was found** by
   grepping literal-address loads. Reads through computed pointers are
   **unverified**. Section 3.4 settles it.

### 3.4 The divergence census (milestone M1)

The shipping game survived per-console `gLocalPlayerId` in link play, and the
cited `seat_test` showed four seats agreeing on RNG and positions over 670
frames. That is weak evidence. The census makes it strong.

**Method.** Run *k* = 4 seats (or 4 processes) with identical inputs and
`gLocalPlayerId = 0..3`, across recorded sessions that cover hub, levels,
bosses, cutscenes, the pause menu and subgames. Every frame, diff S and S\*
per 64-byte block. Then map each differing block to a symbol through
`ram_symbols.h`, `katam.map` and the heap allocator's block headers.

**Outputs.**
- (a) Blocks that legitimately differ (masks).
- (b) Divergence in true S, which is a game bug to fix in the fork.

**Two more axes.**
- Audio on vs off: this must not change S.
- wasm vs lp64 vs aarch64: this must not change S once R1-R7 are fixed.

---

## 4. Drop-in and drop-out

### 4.1 Session creation

The creator's peer boots locally: its own menu, or the fork's direct "start
co-op world" entry with a blank session save. At frame *S0* it registers with
the DO, which starts the event column. There is no boot script and no fixed
F0.

### 4.2 Join (spectate first, then sit)

1. **Hello.** The joiner opens the DO WebSocket with
   `HELLO{room, identity, simVersion, contentHash, platform}`. On a mismatch
   it is refused or sent a missing-content manifest (§8, §11).
2. **Connect.** The DO replies with `SESSION{params, peers, iceServers,
   estFrame}`. The joiner starts WebRTC with each peer, using the DO for
   signalling.
3. **Snapshot.** The DO picks a donor: the healthy seated peer with the
   lowest RTT to the DO. The donor serialises its snapshot at its newest
   *confirmed* frame C, which is still in its ring because confirmation lags
   the present by no more than W. It compresses with zstd and sends
   `SNAP{C, rawSize, hash64, simVersion}` plus 16 KiB chunks through the DO.
   The DO also has the inputs and events for C onward.
4. **Catch up.** The joiner loads C and checks `hash64`, then checks it again
   against the session's agreed checkpoint hash for C when one exists. It
   replays C up to the confirmed frontier as fast as it can (wasm: about 6 µs
   per frame, cited) and then runs live with prediction. At this point it is
   a **spectator**: it receives inputs and sends none.
5. **Sit.** The joiner sends `SIT{preferredSlot}`. The DO chooses a slot in
   this order: the slot this identity held before (a reconnect), then the
   lowest AI-driven slot. It emits `SLOT{slot, peer, F}` with
   `F = estFrame + 30`. The joiner's input column starts at F. Columns are
   per peer, not per slot.

Because the snapshot is of a confirmed frame, **a rollback that is in flight
on any peer cannot affect it**. Several joiners are independent of each
other. Joining mid-cutscene, mid-menu or while paused is the same operation,
because the snapshot contains whatever the game was doing. The seat event
only switches which input drives the slot.

**Before snapshots are portable** (milestones M2-M3), step 3 is replaced by
the existing RLE log replay from S0, which is portable. Before the split,
the snapshot also carries the S\* regions, and the joiner re-pins its focus
on load, as `GAME_REPIN_FOCUS` does now. The game streams the new viewport's
graphics into VRAM over the following frames. The repinning itself is
measured in `tools/netplay_boot_test.mjs`; the visual convergence time is
unverified.

### 4.3 Handing a slot between human and AI deterministically

The fork changes the game so that this needs no tricks. The details are
unverified until the fork changes are made.

- **Make the human/AI choice a per-slot mask.** `sub_0803F324`
  (`kirby.c:6462-6484`) selects network input versus AI by
  `unk56 < gNumHumanPlayers`. Replace that with a per-slot mask
  `gSlotHuman` in the port block, set by SLOT events. This retires the pin of
  `GAME_PLAYER_COUNT` at 4 (`rollback.c:744`).
- **Make difficulty follow the human count.** Today `gNumHumanPlayers` also
  scales difficulty (`gUnk_08351530[type][n-1]`). Let it equal
  `popcount(gSlotHuman)`, changing only on SLOT events. Pinning it at 4
  instead is a product choice; either way it is an event, never local.
- **Give every slot an AI controller.** Create one for all four slots at
  world creation (`code_080332BC.c:64` currently starts at `arg0`, so slot 0
  has none and its Kirby would freeze if its player left). The AI runs every
  frame for every slot, so the handover is just which word is read. The AI
  then takes over the Kirby in place, keeping its position and ability.
- **Remove the SIOCNT read and the network flag.** Drop the read at
  `code_080332BC.c:43-44` and the `gUnk_0203AD10 & 2` branch. The session
  sets these instead, so the frame at which AI inputs are sampled never flips
  (NETPLAY.md §3c).

### 4.4 Leaving, timeouts and the seal

- **Clean leave:** `LEAVE` → the DO emits `SLOT{slot, AI, F}`. Before F, the
  leaver keeps sending inputs (it stays connected until F is confirmed).
- **Dirty drop:** survivors freeze at the window edge, which is correct.
  After 1.5 s the DO (a) asks every survivor for the dropped column's tail
  (`TAIL_REQ`; each survivor replies with the frames it holds beyond the DO's
  archive), (b) takes the union, which is consistent because it is one
  stream, and sets *L* to the last frame held, then (c) emits
  `SEAL{peer, L, tail}` and `SLOT{slot, AI, L+1}`.

  No survivor can have *confirmed* a frame beyond L+1 for that column, so the
  seal never contradicts confirmed state. Survivors roll back at most W
  frames. This is the existing seal (NETPLAY.md §3d), with the DO as the one
  announcer and an explicit tail union.
- **Reconnect** with the same identity inside a 60 s grace period: the peer
  goes through the join flow (spectate, then SIT), and its old slot is
  preferred. After the grace period the slot is simply free.

### 4.5 Host leaves

There is no host. The DO is the sequencer and archive, and any seated peer
can donate a snapshot. Room ownership (title, kicking, whose save the world
started from) is DO metadata and passes to the longest-seated peer. A session
save is downloadable by any player at any time (`SAVE_REQ` → a donor snapshot
of the SRAM region).

**Offline / LAN mode (later):** `RoomCore` already runs outside a DO
(`dev-relay.mjs`). A native peer can embed it as the sequencer.

---

## 5. Desync detection and recovery

### 5.1 Checkpoints

Every *K* = 30 frames, when frame *F* (with `F % K == 0`) becomes confirmed,
each peer hashes the ring snapshot taken at the start of F. It is final by
then: no input before F can change. The hash is sent as a HASH TLV to the
DO, and peers may also send it to each other:

```
HASH { u32 frame; u64 full; u32 region[16]; }   // 76 bytes every 0.5 s
```

Regions are coarse and stable:
- 0 core globals and RNG;
- 1 `gKirbys`;
- 2 `gCurLevelInfo`;
- 3 AI;
- 4 the EWRAM task heap;
- 5 IWRAM tasks and heap;
- 6 other EWRAM globals;
- 7 other IWRAM globals;
- 8 the port sim block;
- 9 the session save;
- 10 the mod store;
- 11-15 spare.

S\* bytes are excluded through the census masks (§3.4) until the split. In
debug sessions, K = 1.

### 5.2 Localising a desync

The DO compares hashes per frame. On a mismatch it requests the 4 KiB block
hashes for the regions that differ from one peer on each side, finds the
differing blocks, and asks both peers to upload a **desync report** to DO
storage or R2:

- both build IDs and platforms;
- the last agreed checkpoint frame A and its compressed snapshot (about 10 KB);
- the input and event log from A to the mismatch;
- the block diffs, mapped to symbols.

**Offline repro** then becomes mechanical: load A on wasm and on native,
replay with `--replay --hash-log` at K = 1, find the first differing frame,
and walk the per-block diff to a symbol.

### 5.3 Automatic resync

The DO picks the authority by majority of `full`. A tie, which includes the
two-player case, goes to the peer with the longest clean hash history, and
then to the lowest slot. Minority peers go through §4.2 steps 3-4 using the
authority's latest confirmed frame. Their rings and timelines reset at C.
Players see a pop, not a crash.

**Rate limit:** at most one resync per peer per 30 s. A second desync within
that period marks the session "unstable" in the report and the DO log, and
play continues with resyncs.

---

## 6. Determinism: requirements and audit

**Requirement:** the same `simVersion` must produce bit-identical S on wasm32,
x86-64, aarch64 and Windows, given the same snapshot, inputs and events.
Risks are listed in priority order.

| # | risk | evidence (file:line) | mitigation |
|---|---|---|---|
| **R1** | **Unsequenced `Rand16()`/`Rand32()` in one expression** (function arguments, the operands of `+`). The order is unspecified, and GCC and clang disagree. | 42 lines, e.g. `kirby.c:17882` (4 `Rand16()` as arguments), `gobbler.c:1466`, `code_0800ECAC_2.c:3363`, `code_0806F780.c:12389`. **Measured:** the same expression gives `0 2 -451 8 / 20` under gcc and g++, and `0 3 -498 84 / 23` under clang and emcc. Native builds use GCC (`CMakeCache: /usr/bin/cc`; lp64 compiles the game as C++); the web build uses emcc. | Codemod into sequenced temporaries, left to right, which matches what the web build does today. Add a lint: a full expression with two or more side-effecting calls or macro expansions touching the same global. clang `-Wunsequenced` does *not* fire, because `gRngVal` is `*(u32*)0x030068D8` (checked). Plus R2. |
| **R2** | **Different compilers and language modes per target:** wasm is clang C (gnu99); lp64 native is GCC C++17 `-fpermissive` (CMake `flags.make`); i686 is GCC C; Windows is MinGW GCC i686. | Unspecified evaluation order (R1), different UB exploitation, C vs C++17 sequencing rules, x87 on i686. | **One toolchain for sim code:** clang everywhere (llvm-mingw for Windows, Apple clang, emcc), the same `-O2`, and **one language mode** (compile the game as C++ with `PTR32` on all targets, wasm included). Drop the i686 x87 builds or force `-msse2 -mfpmath=sse`. CI cross-check (§9.3). |
| **R3** | **libm in sim:** `ArcTan2` calls `atan2` on doubles (`main:platform/bios.c:71-76`) and is used by enemy aim and branching (`bang_bang.c:100`, `shotzo.c:307`, `kracko.c:1205`, `code_0806F780.c:15416`). libm `atan2` is not correctly rounded and differs between glibc, musl/emscripten, macOS and msvcrt; truncation magnifies a one-ulp difference near a boundary. | `ObjAffineSet`/`BgAffineSet` use `sin`/`cos` (`bios.c:229-262`; no callers in game source). `palette.c:87-150` has the float/`powf` functions `sub_081584E8`/`sub_08158654`, also with no callers. `trig.c`'s `Q_2_14(…)` values are compile-time constants and fine. | Reimplement the BIOS `ArcTan2` in integers, using the documented BIOS polynomial (GBATEK). This is exact and also restores fidelity. Delete the dead float functions and use `gSineTable` for affine. Lint: no FP instructions or libm imports in sim objects. |
| **R4** | **Uninitialised reads** | `scroll_lock.c:312,375` (`i` is uninitialised on the else path, so `gKirbys[garbage]`), `star_stone_block.c:274-280` (`unk65E`), `cutscene_trigger.c:119` (`size`/`dtor`), `m4a.c:1363`, and the padding of stack structs copied into the map. | `-ftrivial-auto-var-init=zero` on all sim translation units (clang 16+, gcc 12+). Fix the listed sites. Run a valgrind memcheck soak on native. |
| **R5** | **Host function pointers stored in sim memory** | `Task::main`, `Object2::unk78` (measured in DEBUG-TOOLING §7: 0x41A in wasm vs 0x1011D350 natively at 0x02020F58), the AI's `unk0[4]` (`code_0800ECAC.c:33-36`), `gIntrTable`. That is 52 typed members across 31 types plus untyped `void*` sites (SIXTYFOUR.md §2). Deterministic within one build, but it makes snapshots and hashes non-portable. | **Canonical code IDs.** The sim stores each function's *original GBA ROM address* (from `katam.map`, as `gen_rom_data.py` already resolves), and calls go through a generated perfect-hash table from address to host function. Codemod every store site (`x = FN(sub_…)`) and call site (`CALL(x)(…)`). A debug assertion catches unknown values. ROM function tables then stop being patched. Equality compares keep working. |
| **R6** | **Host data addresses stored in sim memory** | `levelInfo->objlistPtr` points at `gLevelObjLists` (host .bss, `rom_copies.c`) or at rando buffers (`level.c:230`, `rando.c:354-356`). DMA mirrors receive `(u32)src`, including `DmaFill`'s stack `&tmp` (`main:platform/dma.c:310`): different on every run because of ASLR (cited, MULTIPLAYER.md §11). | Move port-owned sim data into the port block at a fixed address. Write the GBA-equivalent value, or 0, into the DMA mirrors. |
| **R7** | **Sim state outside the snapshot** | `gSaveFileInfo`, `gWorldProps`, `gInputRecorder`/`sTapeBuffer`, rollback's slot map and story floor, and rando's `sListParity ^= 1` (`rando.c:439`). That last one runs inside `FillLevelInfo`, so every re-simulation flips it again: **rando plus rollback desyncs today.** | Relocate it all to the port block. CI rule: `nm` of sim objects must show no writable symbol outside the map, apart from an allowlist. |
| **R8** | **This peer's local input leaks into the sim** | `gHeldKeys` latches `REG_KEYINPUT` ← `sKeysDown` ← this peer's own column (`rollback.c:858` → `main.c:510`, `port-src/main.c:546`). There are 78 read sites outside `main.c` (menus, subgames, pause). Soft reset reads `REG_KEYINPUT` directly (`port-src/main.c:491-504`). Sleep (`WaitForInput`, `:578`) and warp (`main:platform/main.c:952`) are local too. | In a session, the global latch takes the **menu owner's** column (the DO-designated owner, by default the lowest human slot). The fork migrates sim reads to an explicit `SlotInput(slot)` or `MenuInput()`. Soft reset, sleep and warp become session events or are disabled. |
| **R9** | **Identity read from hardware** | `gLocalPlayerId` comes from SIOCNT (`code_080332BC.c:43-44`). | Set by the session (§4.3). |
| **R10** | **The sim reads audio state** | `collection_room.c:1748`, `code_0806F780.c:9382`. Sound engine state depends on host pacing and is gated per peer (`data.h:15-35`). | The sim tracks its own "current song" and "playing" flags, and audio becomes P (§3). |
| **R11** | **Integer UB** | Shift counts at or above the width (x86, arm64 and wasm take them mod 32 at runtime; the GBA gives 0; compile-time folding differs). Out-of-bounds reads of host-placed arrays (UBSan list, DEBUG-TOOLING §2: `Sprite[32]` index -1 in `intro.c`, `s16[4]` index 4 in `code_08026044.c`, `u16[2]` in `demo.c`; those line numbers are from an older tree). `INT_MIN/-1` in `Div` (`bios.c:39`). `-fwrapv -funsigned-char` are already set (`Makefile:107-108`, `CMakeLists.txt:404`): keep them. Out-of-bounds reads *inside* the map are deterministic. | UBSan (`bounds,shift,integer-divide-by-zero,signed-integer-overflow`) over the recorded corpus in CI, then fix what it finds. Guard `Div`/`Mod` against -1. |
| **R12** | **Same-frame event order follows arrival order** | `rollback.c:609-630` | Order by `(frame, DO seq)`. |
| **R13** | **Lua and mod runtime** | §11 | §11 |
| **R14** | **Host time and host iteration order:** none found in game source (no `time`/`clock`/`rand`). `performance.now` appears only in pacing (`host_web.c:111-158`). | Forbid it, with a lint. JavaScript drivers must never order sim inputs by `Map` or object iteration. |

Not a risk: data pointers into the map are real GBA addresses and therefore
identical everywhere, and layouts inside the map are asserted
(`make layout-check`, 246 types).

---

## 7. Transport

| option | browser | NAT | latency | cost / ops | verdict |
|---|---|---|---|---|---|
| (a) WebRTC data channels P2P (`ordered:false, maxRetransmits:0`), DO for signalling, TURN fallback | yes (Safari included) | ICE/STUN; TURN when that fails (`stun.cloudflare.com` is free; Cloudflare TURN is $0.05/GB after 1 TB, cited NETPLAY.md §1) | best: a direct path over UDP, no head-of-line blocking | natives need libdatachannel (C/C++, MPL-2.0, Windows/macOS/Linux) | **Primary input plane.** |
| (b) Pure DO relay over WebSocket (today) | yes | none needed | +1 hop through one colo near the *first* connector, TCP head-of-line blocking on loss (estimated +10-40 ms RTT for same-region players, cited) | trivial; already deployed; about $0.0065/h per 4-player room (cited) | **Control plane plus last-resort input relay.** |
| (c) Dedicated UDP relay for natives, WebSocket/WebTransport for browsers | WebTransport support across browsers (Safari) **unverified**; there is no P2P path | relay only | good | needs a VPS or a container fleet outside Workers (Workers have no raw UDP); a second protocol stack | Rejected: it costs more to run, gives browsers no gain over (a), and splits the stack. |

**Recommendation: (a) plus (b).**
- Every peer always holds one WebSocket to the room DO. That carries
  membership, events and watermarks, the archive (batched inputs, 10 messages
  per second per peer), hashes, snapshots, and signalling.
- Inputs flow over a full mesh of unreliable data channels. Path selection
  per pair: direct, then TURN, then relayed through the DO WebSocket with the
  same packet layout. The DO fans relayed packets out to the other peers.
- Spectators get inputs from the DO only. They run confirmed-only behind a
  2-frame buffer and never roll back.

**Worth a spike:** Cloudflare Realtime's SFU now offers data channels. An
unreliable channel through Cloudflare's anycast edge would give UDP-like
relaying with no TURN. Its pricing and semantics are **unverified**.

---

## 8. Session, lobby and versioning (protocol level)

- **Room code:** 6 characters of Crockford base32 (about 10⁹ codes), which is
  also the DO name. Idle rooms persist the archive, the latest snapshot and
  the session save for 7 days, so a world can be resumed.
- **Identity:** a 128-bit random token kept on the client (localStorage, or
  the native config file), sent as the partysocket `id`, plus a display name.
  The DO keeps `identity → last slot`. It is not an account; when abuse
  matters, add an HMAC-signed invite.
- **Version gate:** checked in HELLO, and the DO refuses on any mismatch.
  - `protoVersion`.
  - `simVersion`: a hash of the sim source tree, the sim compiler and flags,
    and determinism-relevant generated data. It is stamped at build time
    (`stamp_build.py`) and is the same for web and native builds of one
    commit. It is *not* a binary hash.
  - `romHash`: SHA-1 of the player's ROM, which must be the same dump.
  - `contentHash`: §11.
  - Release CI blocks if the cross-platform hash corpus (§9.3) disagrees.
- **Spectators** are free in the way that matters: one snapshot (about
  10 KB), then about 0.5 KB/s. Cap them at 16 per room (DO fan-out). A
  spectator can become a player through SIT.
- **Messages:**
  - control: HELLO, SESSION, SIGNAL{to, sdp/ice}, SIT, LEAVE, EVENT,
    WATERMARK, TAIL_REQ/TAIL, SNAP_REQ/SNAP/CHUNK, HASH, BLOCKHASH_REQ/…,
    REPORT, SAVE_REQ, PING;
  - input plane: INPUT (§2.2).
  - JSON for rare control messages, binary for everything per-frame.

---

## 9. Testing

### 9.1 In-process multi-peer harness

N virtual peers live in one process. Each has its own sim arena, ring,
timeline and network endpoint. The harness context-switches between them with
the snapshot memcpy at the frame boundary, which is the `seats.c` technique
(about 5 µs per switch natively), because the fixed map allows only one live
game state per process.

- **Network simulator:** links driven by a seeded PRNG and a virtual clock,
  with configurable latency, jitter, loss, duplication, reordering,
  partitions, and a DO that goes away. The whole harness is deterministic, so
  a failing seed reproduces exactly.
- **The DO in the same process:** `RoomCore` in JavaScript, or a C port for
  the native harness. The web harness is node with N module instances, which
  `tools/netplay_*_test.mjs` already does.
- **Pass criterion:** every peer's checkpoint hashes are identical on every
  checkpoint, there are zero `lateDrops`, and every resync caused by
  injected corruption converges.
- **Scenarios:**
  - 2-4 players on a scripted route;
  - join and leave at random frames, including mid-rollback and
    mid-transition;
  - a dirty drop with partial tails;
  - reconnect within the grace period;
  - a peer whose RAM is deliberately corrupted (byte flips), testing desync
    detection, localisation and resync;
  - a 10% loss storm;
  - 300 ms of jitter;
  - a DO outage longer than the watermark.

### 9.2 Building on the fork's `--record/--replay/--hash-log`

The same files are reused for everything:
- the session archive (the RLE log plus events);
- the content of desync reports;
- the corpus for the census (M1) and for UBSan/valgrind runs.

`--hash-log` should log the §5.1 region vector per frame, so a cross-check
names the region.

### 9.3 Cross-platform checks (release gate)

A corpus of recordings is replayed with `--hash-log` on wasm (node), x86-64,
aarch64 and Windows (under Wine in CI). Each recording starts either from
power-on or from a snapshot. All hash logs must be identical.

The corpus must exercise R1 and R3:
- the enemies that use ArcTan2 (Bang-Bang, Shotzo, Kracko);
- the `kirby.c:17882` path;
- Gobbler;
- a rando seed.

Also cross-load snapshots: save one on platform X, load it on Y, run both,
and compare hashes. This is the portability test for M2.

### 9.4 Soak

Nightly runs of 4 virtual peers for 4 hours with random churn and network
weather, on both the native and wasm harnesses. Also a weekly real-network
soak: 2 browsers plus 2 natives against the deployed DO, with tc-netem on
the natives.

---

## 10. Implementation plan

Sizes are for one engineer who knows the codebase.

| # | milestone | size | can start | independently tested by |
|---|---|---|---|---|
| **M0** | **Determinism hardening.** One clang toolchain and language mode for every sim build; `-ftrivial-auto-var-init=zero`; codemod of the R1 sites plus a lint; integer `ArcTan2`; delete the dead float code; lints for no-FP and no-libm; UBSan corpus run and fixes; R8 (menu-owner latch; soft reset, sleep and warp gated); R12. | 1.5-2 wk | **now, on the fork** | §9.3 cross-platform hash logs identical over the corpus (the existing full-memory trace is enough for this) |
| **M1** | **Sim arena plus census.** A port sim block at 0x02040000, reserved on every platform; relocate R6 and R7 state; the `nm` CI rule; run the census (§3.4) and produce masks and a list of real divergences, then fix those in game code. | 1.5 wk | now | 4 seats with different local IDs give equal masked hashes over the corpus; rando plus self-test rollback passes |
| **M2** | **Portable snapshots.** Canonical code IDs (R5) by codemod plus a generated table; `gIntrTable`/sound callbacks kept out of S; a canonical serialiser with zstd. | 2 wk | now (in parallel with M1) | a snapshot saved on lp64 and loaded in wasm (and the reverse) gives identical hash logs |
| **M3** | **Rollback engine v2.** Ring over the arena; timeline state in the arena; `gSlotHuman` and AI controllers on all slots (§4.3); events as a column with sequence numbers and a watermark; checkpoint and region hashing; donor snapshot of a confirmed frame; resync load. | 2 wk | after M1 (the M2 serialiser is needed for transfer) | M4 harness scenarios |
| **M4** | **In-process harness plus network simulator** (§9.1), native and node. Build it alongside M3. | 1.5 wk | with M3 | itself: seeded scenario suite green |
| **M5** | **Protocol v2 and the DO room.** Sequencer, watermark, archive, seal with tail union, hash referee, desync reports to R2, snapshot relay; inputs over the WebSocket relay only. **First playable v2.** Join falls back to log replay if M2 is late. | 2 wk | after M3 | `RoomCore` unit tests; harness with the real `RoomCore`; browser plus native over `wrangler dev` |
| **M6** | **WebRTC input plane.** Browser `RTCDataChannel`; native libdatachannel; signalling through the DO; Cloudflare TURN; per-pair path selection and fallback to the DO. | 2 wk | after M5 (can prototype in parallel) | real-network soak; loss and reorder handled with no `lateDrops` |
| **M7** | **Feel.** Adaptive delay, rate-dilation time sync, reconnect grace, spectators, lobby UX, "waiting for…" UI. | 1-1.5 wk | after M5 | measured rollback depth at or below target under netem profiles |
| **M8** | **Presentation decoupling for netplay.** Render only the newest predicted frame; audio becomes state-driven music plus sound-effect events de-duplicated across rollbacks by `(frame, kind, id, source)`; drop the S\* regions from the arena; remove `gLocalPlayerId` writes from the sim; unmasked hashing. | 1.5-2 wk (netplay share) | **blocked on the renderer split** | the census shows zero divergence without masks; snapshot is about 10 KB |
| **M9** | **Mods in sessions** (§11): content hash, blob fetch, Lua sim store and VM. | 2-3 wk | after M5, coordinated with MODDING.md | harness with a sim-hook mod; cross-platform corpus with the mod |

**Critical path:** M0, then M1 and M2, then M3 with M4, then M5. That is
about 9-10 weeks to a playable v2 over the relay. M6 and M7 add about 3 more
weeks. M8 follows the renderer.

**Ready to start on the fork:** M0, M1 and M2. They touch only game code and
port code, and none of them needs the renderer.

---

## 11. Mods in sessions

This section is coordinated with `design/MODDING.md`, which was not yet
written when this was.

**The content hash agreed in HELLO** is
`H(simVersion, romHash, [ (modId, modVersion, simContentHash) in load order ], levelSetHash, sessionMods)`.
- `simContentHash` covers only a mod's **sim-relevant** files: sim-hook Lua,
  room and level data (collision, object lists, warps), and any data table
  the sim reads.
- Textures, audio, UI and presentation Lua go into a separate,
  **informational** `presentationHash`. It is logged but never enforced.
- Editor-made levels are mods. Each mod's manifest declares which class every
  file belongs to, and the loader enforces it: a presentation script has no
  write access to the sim.

**Missing content.** Mods and levels are content-addressed blobs. The DO
returns a manifest of the hashes the joiner lacks. The joiner fetches them
from R2, uploaded by the room creator when the session was created and capped
at 20 MB per room (the size is a policy choice), or from a peer over a
reliable data channel. Vanilla assets extracted from the ROM are **never
transferred**: each player extracts them from their own ROM, and a
`romHash` mismatch refuses the join. A joiner that declines the download is
refused.

**Lua that affects the sim is sim state.** The demands on the runtime:
- **Explicit state.** Sim hooks are pure functions over an explicit **mod
  sim store**: typed records (integers, fixed-point, booleans, short strings
  and IDs) in a fixed-size region of the port sim block, for example 64 KiB.
  Lua globals and upvalues are reset or frozen between hook calls. Snapshot,
  hash and transfer of mod state then cost a memcpy of the store and are
  portable for free.
- **Why not snapshot a live Lua heap.** Raw memcpy of a Lua heap would work
  for in-process rollback if it lived in a fixed arena. It is **not**
  portable: pointer width and layout differ between wasm32 and 64-bit
  natives. A canonical heap serialiser in the style of Eris costs about
  0.1-1 ms per checkpoint (estimate) and still leaves pointer-keyed table
  order exposed. Use it only if the store model proves too restrictive.
- **Determinism rules for the sim VM:**
  - integer-only arithmetic: a patched Lua 5.4 where `/`, `^`, float
    literals and `math` floats raise an error, with an `fx` fixed-point
    library instead;
  - no `os`, `io`, `require` of host modules, `math.random`, `collectgarbage`,
    `__gc` or weak tables;
  - a fixed string-hash seed (`luai_makeseed`);
  - `pairs` replaced by key-ordered iteration;
  - no tables or functions as keys in anything that gets iterated;
  - an instruction-count budget per hook (deterministic: overrunning it is a
    deterministic error, which disables the mod for the session on every
    peer at the same frame).
- **Cost.** Snapshot cost is trivial. The CPU budget dominates: the vanilla
  sim costs 6 µs per frame in wasm, and a 10-frame rollback re-runs hooks
  11 times. Cap sim hooks at about 0.3 ms per frame in wasm (measured by
  instruction count), or rollback depth becomes a frame-time problem.

**Presentation hooks** run in a separate VM per client, with a read-only view
of the latest presented sim state. They may differ per client and are never
hashed or snapshotted. They must tolerate rewinds, because after a rollback
they can see state jump backwards, and they receive the de-duplicated sound
events (M8).

---

## Appendix: measurements made for this document

- **lp64 build of `main` b59e25e**, built in the scratchpad (repo untouched).
  ROM `baserom.gba`, `--press` A every 40 frames from frame 300, giving the
  hub with 4 Kirbys at frame 2600. `PORT_DUMP` of EWRAM, IWRAM and VRAM at
  frames 2600 and 2601, deflate-6 and xz. Results in §3.2.
- **Evaluation-order test** for `Rand16()`: gcc 16.2 / g++ vs clang 23 / emcc,
  `-O2 -fwrapv -funsigned-char`. Results in R1.
- **Writable game symbols outside the map:** `nm -S` over the game objects.
  Results in §3.1 and R7.
