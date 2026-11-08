# Netplay v2: deterministic simulation, input-only sync

Design, 2026-11-08. Revised the same day: §12 (model v2.1: no lockstep,
deadline-sealed inputs, periodic world sync) and §13 (scaling past 4
players) supersede the waiting parts of §2, §4.4 and §5, and §8's room
code is now 4 letters. Protocol v2, the room and the lobby are implemented
(branch `netlobby`); the rest is not yet. It targets the hard fork
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

**Implemented on branch `netlobby`** (a variation that keeps the max in one
place): each seated peer sends its request to the room as a control message
`{t:'view', w, h}` (the page reads it with `PortSimViewRequested`); the room
(`SessionCore._viewChanged`) computes the per-axis max over the seated peers
with the same rounding as `RoundView` (whole 16-px cells a side, 240x160 to
1024x512) and, when it changes, emits a frame-stamped `view {w, h}` event in
the event column, past the watermark. Every peer schedules it with
`PortInputLogScheduleView` after `PortSimViewUseSession()`, which lifts the
240x160 hold for that page and turns local requests into records only. A
joiner gets the event history in the archive. Spectators' requests never
count; a sealed seat stops counting.

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

### 3.4.1 The census as built (M1, branch netsim)

**Measured.** `--census 4:450:OUT` over the six recordings (attract in
focus-only mode, the others in session mode from frame 450), masks
`tests/census/masks.txt`, `tools/census.py` to name blocks.

The tool: `platform/census.c` driven by seats in census mode
(`platform/seats.c`). The seats differ only by focus. In session mode (F0)
they also run the game's network-input path with four slots: slot 0 takes
the recording's keys, slots 1-3 the AI. Masks are byte-precise; the sprite
screen fields and the display task's player byte are masked per round,
because they move with the heap. `PORT_CENSUS_DUMP=F` dumps every seat's
state at frame F, and `PORT_CENSUS_FOCUS=k` with one seat runs a single
instance under gdb.

**Real divergences found and fixed (class b):**

- **The save owner.** `gLocalPlayerId == gUnk_0203AD24` is tested at 13
  sites in 11 files. Only the link-play parent writes progress to SRAM; the
  children count the write (`sub_08031CE4`). The Dark Mind ending advances
  `gAIKirbyState` on the parent only (`code_0814EBE4.c`). Found at
  boot_to_level frame 581 (`cutscene_trigger.c:835`).
  - The fix is `PortSimSaveOwner()` (`platform/port/simblock.h`): every
    peer is the owner when the sim block's `PORT_SIM_F_SESSION` flag is
    set.

**Masks so far:**

| class | what |
|---|---|
| P | `gLocalPlayerId` |
| P | the sound engine: `gMPlayTrack_*`, `gSoundInfo`..`gMPlayInfo_3` |
| S* | `gCurLevelInfo[k]`: BG layers (`unkC0`..`unk1E0`) and alternate viewports |
| S* | the display: shadow registers, upload queues, OAM and its draw order, palettes, I/O, PLTT/VRAM/OAM |
| S* | sprite `tilesVram`, `x`/`y` and `unk1C..palId` |
| H | the host's own key poll (`keysDown`, last seat only) |

**Not converged.** Unmasked blocks remain: 38 in boot_to_level, 106-191 in
the others; the first is at frame 653 (666 in wide_view, 5164 in attract).
Each is still to be classified as a mask or a fix. The candidates:

- the HUD task (`UpdateGameplayHud`, P);
- `Kirby.base+45` (the sprite OAM slot);
- the tasks `sub_08089BCC`, `sub_0808A208` and `sub_08088C0C`;
- the AI task `sub_0800EF60` (2 bytes at frame 823: possibly real);
- `gSourcePalettes` and `gPaletteEffectManager` (palette fades of the
  local view, probably S*);
- heap objects whose fields beyond the sprite differ.

**So the M1 exit criterion (equal masked hashes over the corpus) is not met
yet.** The method and tools are in place, and each remaining block is one
`PORT_CENSUS_DUMP` plus a gdb watch away from classification.

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
| **R5** | **Host function pointers stored in sim memory** | `Task::main`, `Object2::unk78` (measured in DEBUG-TOOLING §7: 0x41A in wasm vs 0x1011D350 natively at 0x02020F58), the AI's `unk0[4]` (`code_0800ECAC.c:33-36`), `gIntrTable`. That is 52 typed members across 31 types plus untyped `void*` sites (SIXTYFOUR.md §2). Deterministic within one build, but it makes snapshots and hashes non-portable. | **Canonical code IDs.** The sim stores each function's *original GBA ROM address* (from `katam.map`, as `gen_rom_data.py` already resolves), and calls go through a generated perfect-hash table from address to host function. Codemod every store site (`x = FN(sub_…)`) and call site (`CALL(x)(…)`). A debug assertion catches unknown values. ROM function tables then stop being patched. Equality compares keep working. **Done** (`netsim-fnid`): `FN`/`CALL`, `platform/port/fnid.h`, `tools/codemod_fnid.py`, DETERMINISM.md "Code IDs"; raw EWRAM+IWRAM native vs wasm now differ only in R6 data pointers. |
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

- **Room code: 4 letters** (changed from the 6-character Crockford code
  first proposed; implemented in `netplay/codes.mjs`). The user asked for
  four characters people can read out and type on a phone:
  - Alphabet: `BCDFGHJKMNPQRSTVWXZ`, 19 consonants. Letters only, so a phone
    shows one keyboard and never switches to digits mid-code. No vowels
    (and no Y), so no code spells a word; a 12-entry substring blocklist
    catches the few look-alike spellings (`FVCK`, `KKK`, ...; 269 codes, 0.2%
    of the space). No L or O, so nothing reads as 1, I or 0.
  - 19⁴ = 130,321 codes: two orders of magnitude above the rooms open at
    once, so random allocation almost never probes twice.
  - Input is tolerant: case, spaces, dashes and dots are ignored, a pasted
    share link is reduced to its `?room=`, and look-alikes map to the letter
    (`8`→B, `5`→S, `2`→Z, `6`→G, `U`→V).
  - The server allocates codes at random from the free ones; clients never
    choose. The code is **not** the DO name: a directory (one `Directory`
    DO) maps code → room id (128 random bits, the `SessionRoom` DO name).
    A code is reused only after its room has been empty for 20 minutes.
  - A short code is guessable by design, so: per-IP token buckets at the
    edge (30 lookups burst then one per 2 s; **6 misses burst then one per
    20 s**, about 186 guesses an hour per IP), per-IP limits on creates and
    on HELLOs per room, and host controls -- lock the lobby, approve each
    join, kick (banned for the room's life).
  - A persisted world (the 7-day archive) is never reachable by its old
    short code: creating a room also returns a 192-bit **resume token**
    (`POST /v2/resume`), which rebinds the room to a fresh code.
  - HTTP surface: `POST /v2/rooms` → `{code, resume}`, `GET /v2/rooms/CODE`,
    `POST /v2/resume {token}`, and the WebSocket at `/v2/rooms/CODE/ws`.
- **Identity:** a 128-bit random token kept on the client (localStorage, or
  the native config file), sent as the partysocket `id`, plus a display name.
  The DO keeps `identity → last slot`. It is not an account; when abuse
  matters, add an HMAC-signed invite.
- **Version gate:** checked in HELLO, and the DO refuses on any mismatch,
  naming the field (`error {code:'version', field, room, yours}`), and the
  lobby tells the player what to do about each one.
  - `protoVersion` (2).
  - `simVersion`: a hash of the sim source tree, the sim compiler and flags,
    and determinism-relevant generated data. It is stamped at build time
    (`stamp_build.py`) and is the same for web and native builds of one
    commit. It is *not* a binary hash. **Implemented** in
    `tools/stamp_build.py` (`window.KATAM_SIM`), with one temporary
    addition: while world syncs move same-build ring snapshots (host
    function pointers, R5), the wasm's own id is appended. Drop it when M2's
    portable snapshots land. An unstamped dev page uses a hash of its wasm.
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

## 12. Model v2.1: no lockstep, deadline-sealed inputs, periodic world sync

Revision, 2026-11-08, at the user's direction, implemented as protocol v2 on
branch `netlobby` (`netplay/session.mjs`, `web/net_session.js`,
`web/net_game.js`). It replaces the parts of §2, §4.4 and §5 that make peers
wait for each other. Everything else (the sim/presentation split, the
determinism audit, snapshots, mods) stands.

### 12.1 What changes, and why

| v2 as first designed | v2.1 | why |
|---|---|---|
| Each peer's column is authoritative for itself; a peer that misses the rollback window freezes everyone at the edge (§2.5) | **The room seals every frame at a deadline.** A seat whose input has not arrived is filled with its newest known input ("repeat last") and the late value is discarded. The sealed stream is the truth for everyone, the late sender included. | One slow phone must never hold up the other players. Waiting is the failure the user named. |
| Rollback (W = 10) is the correction mechanism; past the window, stall | **Rollback is shallow, optional and local.** Today's engine keeps its ring as a cheap way to fix a misprediction a few frames back; nothing on the wire knows about it, and the stall gate is off (`PortRbSetStallGate(0)`). | "Rollback is not so important if they sync periodically." |
| Desync recovery is an exceptional path (§5.3) | **World sync is a normal path.** A donor's state at a checkpoint replaces a peer's, which re-runs its inputs since. Triggers: hash mismatch, lag, join, a peer's request, and a low background rate. | Corrections that are frequent are small; the presentation layer blends them. |
| Determinism is required to stay together | **Determinism is an optimisation**: it keeps syncs rare and small (a deterministic peer's state differs from the donor's in few or no pages). Peers stay together through syncs either way. | Cross-platform determinism (R1-R14) is weeks of work; play should not wait for it. |
| Joining = snapshot + tail (or log replay until M2) | Unchanged in shape: archive (log replay) today, snapshot when portable, **plus a join sync** shortly after the joiner sits. | The join sync makes a joiner correct even if its replay was not. |
| WATERMARK every 100 ms; peers never simulate past it | The watermark is carried in every FRAMES message and events are stamped past it, but **no peer stops at it**. An event that arrives late is applied one frame late locally, and the next sync reconciles. | No global barrier anywhere: start, join, leave, pause and room changes included. |
| Host-less DO | Unchanged: the room is the sequencer, archive, referee and relay. The "host" is lobby metadata and passes on after 10 s away. | |

### 12.2 The sequencer

The room keeps a **session clock**: frame F is due at
`clock.t + (F - clock.f) * 16.74 ms`, anchored on the first input it hears
(and again when the room has been idle). Frame F is sealed when every
*expected* seat's input for F is in, or `deadlineMs` (default 100) after it
was due, whichever comes first. Due times come from the clock, not from the
first sender: a peer that stamps its inputs far ahead must not shorten the
others' deadline (an earlier draft did that, and measured as a slow-link peer
fast-forwarding everyone else).

- *Expected* means seated, connected, live at F, and not **excused**: a seat
  that was filled `excuseAfter` (20) frames in a row stops being waited for
  at all, and is waited for again after `rejoinAfter` (10) on-time inputs in
  a row. Without excusal a chronically late seat made every frame seal at
  the full deadline (measured: 100% deadline seals, 102 ms p50 latency for
  everyone); with it, the other peers' p50 returns to 1-2 ms.
- Fill: the seat's newest received input at or before F, else its last
  sealed value. A disconnected seat reads neutral (0).
- Late inputs are counted per seat and reported in `stats` (once a second).
  The client raises its input delay one step per report that shows new late
  inputs, and lowers it after 5 quiet seconds (§2.3's policy, driven by the
  room's count instead of RTT percentiles).
- Time sync (§2.4) steers by the room's clock: `stats.clock` is the room's
  current frame. A peer targets `clock + one-way latency - delay + 2`; behind
  it, it fast-forwards (`PortRbReplayTo`); ahead of it, it holds a frame now
  and then (at most 6 a second, through the host's pacer: `portFrameDue` in
  a browser). No peer ever waits for another.

### 12.3 Hashes, the referee and donor election

Every `hashEvery` (30) frames after `inputStart`, once frame C is sealed and
simulated, each peer hashes the state at the start of C (today: the game's
own desync quantity, `gRngVal` and every Kirby's position, read from the
ring copy) and sends `{t:'hash', f, h}`. The room judges C when every live
seat has reported, or `hashWaitMs` (1.5 s) after the first report:

- the majority hash is the reference;
- a tie goes to the group whose best donor wins the election below;
- every peer outside the reference gets a sync, and everyone gets a
  `desync` notice (for logs and the lobby).

**Donor election** (`electDonor`), among peers holding the reference hash:

| policy | rule | strengths | weaknesses |
|---|---|---|---|
| `majority` (default) | longest clean hash streak, then lowest RTT to the room, then seated longest | the donor is the peer most likely to be right; stable (the authority does not flap) | in a 2-player game a tie is decided by history, which a fresh pair lacks (falls to RTT) |
| `rtt` | lowest RTT first | fastest transfer; best for big rooms | may pick a peer that just recovered |
| `host` | the host when eligible | predictable; matches players' intuition ("the host's world") | the host may be on the worst link |
| sim in the DO (later) | the room runs the sim headless from the sealed stream and is the donor | one authority, never wrong about its own inputs, needs no election | cost: wasm sim ~6 µs/frame (cited) is ~0.04% of a core at 60 Hz, but a DO's 128 MB memory limit must hold the sim arena (292 KiB after the split; the full GBA map is the problem today, SERVER-RENDER.md §4b) and the ROM is not on the server (§11: never uploaded). Viable only with a ROM-free sim pack, so not before M8/M9. |

Background syncs never target the peer that would itself be elected donor,
so the authority stays put unless a hash moves it.

### 12.4 World sync

Triggers, per peer, rate-limited to one per `minSyncGapMs` (2 s) except
joins and explicit requests, one in flight at a time, cancelled after 8 s:
`mismatch` (referee), `lag` (its checkpoints trail the reference by more than
`lagSyncFrames`, 90), `join` (3 s after a mid-game SIT, or at its first
hash), `ask` (`{t:'syncme'}`, e.g. after the engine counted inputs older
than its ring), and `periodic` (`syncEveryMs`, default 10 s).

The flow, relayed by the room as opaque bytes:

```
room   -> target  sync_req  {id, f, donor, reason}
target -> room    sync_have {id, f, ps: 4096, hashes: base64(u32 per page)}   (or no hashes)
room   -> donor   snap_req  {id, f, to, ps, hashes}
donor  -> room    snap      {id, f, size, count, h, kind}  + count x CHUNK (16 KiB)
room   -> target  snap ... + the chunks, as they arrive
target -> room    sync_done {id, ok}
```

The payload is a **page diff**: the target sends a hash per 4 KiB page of
its own state at C, and the donor sends only the pages that differ. A sync
of a peer that is already right costs about 1.5 KB of page hashes and a
36-byte empty blob. The blob layout is netsim's draft (`KSNP` header, kind
full/pages, frame, raw size, state hash) with ver 0 = today's same-build
ring snapshot, compressed with deflate (`CompressionStream`; netsim's zstd
replaces it). The target rebuilds the state, checks the donor's hash, and
calls `PortRbApplySnapshotAt(C, ...)`: the engine loads it at the next frame
boundary, applies any events it jumped over, and re-runs (picture off) to
where it was -- or jumps forward if it was behind.

**Measured** (tools/netplay_v2_test.mjs, three game instances, 40 s):
an injected desync (beta's `gRngVal` flipped) was detected at the next
checkpoint and repaired in **1.6-3.5 s**; a sync after a real divergence
moved 9-19 pages (6-16 KB compressed); a full join sync 97 pages (51 KB).
Per-client traffic including syncs: **1-2.3 KB/s down, 0.3-0.6 KB/s up** for
an ordinary peer, and up to 6.5 KB/s up for the donor while it serves syncs.

### 12.5 The presentation seam

After a sync is applied, `web/net_game.js` emits a **correction event**
`{frame, kind: 'sync', pages, bytes, reason, at}` three ways:
`opts.onCorrection`, `Module.portNetCorrection(info)` if the host defines
it, and a DOM `CustomEvent('katam-net-correction')`. The renderer's
interpolation (branch `smooth`) can blend camera and sprite positions across
it instead of snapping. netsim's correction record (per-Kirby old/new
positions) extends the same event when its engine lands.

### 12.6 The engine seam

**Implemented (branch netsim):** `platform/port/netsim.h` and
`platform/netsim.c`. There is no rollback in the engine; that is v2.1's
choice. The netlobby stopgaps in `rollback.c` (`PortRbSnapshotAt`,
`PortRbApplySnapshotAt`, `PortRbSetStallGate`) still work, same-build only.
`web/net_game.js` should move to the calls below; the protocol is
unchanged.

| stopgap | replacement |
|---|---|
| `PortRbSealInput` | `PortNetSealFrame(frame, keys, seats)` |
| `PortRbSnapshotAt(C, dest)` | `PortNetSnapSave` / `PortNetPagesSave` of the frame-top image (`PortNetImageFrame()`) |
| `PortRbApplySnapshotAt(f, src)` | `PortNetApplyAt(blob, len)` (the frame is in the blob) |
| `PortRbSetStallGate(0)` | not needed: `PortNetSessionStart` never stalls |

**Session and inputs.** `PortNetSessionStart(seats, localSeat,
humanMask)` puts the game on its network-input path with four slots and
sets the sim-block session flag. It also starts a 1024-frame input ring:
`PortNetSetLocalInput(frame, keys)`, `PortNetSealFrame`, and
`PortNetSetHumans(frame, mask)` (applied at that frame). A missing input
is "repeat last", the same rule the room seals with. The global KEYINPUT
latch carries the **menu owner's** column, the lowest human seat, on every
peer (R8).

**The image** (`PortNetImageCapture/Apply`) is canonical and
pointer-free, 478,652 bytes at 4 seats. In order:

- EWRAM;
- IWRAM;
- the sim block's used bytes;
- I/O, with the DMA address mirrors zeroed;
- the DMA channels, canonical;
- the PPU latches;
- PLTT, VRAM, OAM;
- SRAM.

It is captured at the top of every session frame (6-8 µs). Saves read that
capture, and loads are queued to the next frame top, so any call is legal
at any time.

**Blob** (40-byte header, then one zstd frame):

```
u32 'KSNP'  u8 ver=1 u8 0 u8 kind (0 full, 1 delta, 2 pages) u8 0
u32 frame   u32 baseFrame   u32 rawSize   u32 payloadSize
u8 simVersion[8]   u8 stateHash[8]
```

- **full**: zstd level 1 of the image.
- **delta**: the image XOR a base both sides hold. The base is a blob
  this peer saved or loaded; the last 4 are kept.
- **pages**: u32 count, then (u32 index, 4 KiB) for each page whose hash
  differs from the peer's `PortNetPageHashes`.

The page hashes are **64-bit** (8 bytes each), not the u32-per-page of the
§12.4 draft. That is 117 pages, 936 B. Use the engine's hashes, so the donor
and the target compute the same thing.

The receiver rebuilds the image and checks `stateHash` before applying.
The error codes are in `netsim.h`. `simVersion` hashes the image layout,
`PORT_MAX_SEATS` and `KATAM_SIM_VERSION`; it does **not** include the
binary or the wasm id. Native and wasm builds of one commit agree, so the
room's `sim` gate can drop the wasm id.

**Hashes.**

- `PortNetStateHash`: 64-bit, over the whole image.
- `PortNetRegionHashes`: the 16 regions of §5.1. Region 7 is folded into 0
  for now, 11 is I/O+DMA+PPU, 12 is VRAM, 13 is PLTT+OAM.
- `PortNetPageHashes`.
- `PortNetCheckpointHash(frame)`: every 30 frames, the last 64 kept.

The referee should use these instead of RNG plus Kirby positions. The
census masks (§3.4.1) still have to be applied before a hash means "same
world" across *different* local players: the image today includes
per-peer S*. That is the next step for the referee.

**Apply at frame.** `PortNetApplyAt` loads frame F at the next frame top,
then re-runs this peer's ring from F to the frame it was at, with the
picture off. `PortNetSnapLoad` loads without the catch-up.

The scanline loop now always runs, so a re-simulated frame is the same
frame. Before this, `PORT_NO_RENDER` changed the sim column on 7297 of the
7299 frames of play_rooms (the "HBlank skip" issue).

**The correction event.** `PortNetLastCorrection` returns {frame,
fromFrame, kirbyMask, per-Kirby old/new x/y/room, seq} and calls smooth's
`PortRlNoteCorrection(frame)`, which snaps the next present.

**Measured** (native LP64 / node 22 wasm, play_rooms, session from 1250,
zstd level 1):

| | native | wasm |
|---|---|---|
| full blob | 56.8-67.0 KB (of 478.7 KB raw; VRAM and SRAM included) | same bytes |
| compress full | 240-300 µs | 325-395 µs |
| delta, 1 s apart | 0.9-4.7 KB, 135-210 µs | same sizes, 230-310 µs |
| delta, 5 s apart | 1.0-6.2 KB within a room; 30-62 KB across a room change | same |
| decompress + apply | 150-200 µs + 140-170 µs | 490 µs + 84 µs |
| catch-up, 121 frames headless | 1.1-1.6 ms | 3.3 ms |

**Tests.**

- `--net-test session:F,rewind:F:N` (`NET_TEST=` under node) saves frame
  F, applies it at F+N, catches up, and requires the state hash to equal the
  first pass. It PASSED on five recordings, natively and in wasm. The wasm
  and native hashes at that frame are equal (`98CFFA336C7D1D5A` both).
- **`tests/netsim_xplat.sh`**:
  - native and node each save frame 3000, and the two images are
    byte-identical for all six recordings (`tools/netsnap_diff.py`);
  - each platform then loads the other's blob at frame 200 and plays on;
  - every frame after that matches the platform's own unbroken run on
    keys/sim/vid/sram/pix, and the native run on keys/vid/sram/pix;
  - the one exception is attract's first frame after the load, whose sim
    hash differs while vid/pix and every later frame match. Probably a DMA
    channel armed with a port-memory source, which a canonical image
    cannot carry and disarms.

**What made it portable:**

- R5, canonical code IDs (branch netsim-fnid, merged).
- R6, five store sites now use the ROM's own copy of a table the port also
  defines in C (`PORT_ROM_TABLE`, checked byte for byte against the ROM):
  `gUnk_082D8D74`, `gUnk_082EB7F8`, `gUnk_082EB2B8/2D0/2E8/300`,
  `gUnk_08355578`.
- R7, the sim block.

Other R6 store sites may exist outside the corpus. The xplat test is the
detector.

### 12.7 Native client: plan (`--net CODE`, `--net-create`)

Not implemented: the native build has no WebSocket or TLS library
(`third_party/` holds only imgui), and same-build snapshot syncs cannot cross
between a native and a wasm peer until M2 anyway (R5), nor can inputs stay
in step until M0 (R1, R2). The plan, in order:

1. **Library: libdatachannel** (MPL-2.0, C++17, Windows/macOS/Linux). It
   ships `rtc::WebSocket` (with TLS via OpenSSL or mbedTLS) *and* the
   WebRTC data channels M6 needs, so one dependency covers both, vendored
   under `third_party/` and built only for native targets.
2. **`platform/native/net_client.c`** speaks protocol v2: the HTTP calls
   (`POST /v2/rooms`, `GET /v2/rooms/CODE`) over the same library, HELLO with
   `platform: 'native'`, the FRAMES decoder (a port of `wire.mjs`, ~100
   lines), and the engine calls `web/net_game.js` makes. `--net CODE` joins,
   `--net-create` creates and prints the code (and a share URL for browser
   players), `--net-server URL` overrides the default.
3. **Lobby in the settings menu** (imgui): the same seats, ready, host
   controls; the 4-letter code shown large.
4. **Until M2**, the room's version gate (`sim` includes the binary id)
   keeps native and web players in separate rooms -- a native-only room
   works as soon as steps 1-2 land, since two copies of one native binary
   share a layout. Mixed rooms need M0 + M2.

Alternatively, for testing only, a native peer can run the session from a
small node sidecar over a local pipe; not worth shipping.

## 13. Scaling past 4 players

The playable cap stays 4: it is the sim's (`gKirbys[4]`, the u8 free-slot
mask, per-room object pools, the cable identity), and netsim owns it
(`PORT_MAX_SEATS`, experiment flag 8). Nothing in the protocol or the room
is sized by it.

**What is variable now.** `seats` is a session parameter (u16 on the wire;
the room caps it at 1024, and at the smallest `caps.seats` a member
declares); `kirbys` is a separate parameter, the sim slots a seat can drive
(4). FRAMES carries a seat bitmask of `ceil(seats/8)` bytes per frame plus a
u16 for each seat whose input changed, so a frame where nobody changed costs
the mask alone at any size. Hash reports, rosters, the lobby's seat list and
donor election are all lists. INPUT carries one seat's own column (the room
knows whose).

**Measured** (`netplay/tools/load_test.mjs`, synthetic clients over the dev
relay, inputs changing every ~23 frames, 12 s at 60 Hz):

| clients | scenario | streams identical | seal latency, fast peers p50 / p95 / p99 ms | slow peers p50 ms | down B/s per client | up B/s per client | join burst B/client | room fan-out msgs/s |
|---|---|---|---|---|---|---|---|---|
| 4 | all on time | yes | 1 / 2 / 2 | - | 1,201 | 535 | 4,415 | 247 |
| 8 | all on time | yes | 1 / 2 / 2 | - | 1,275 | 534 | 10,387 | 493 |
| 16 | all on time | yes | 1 / 2 / 2 | - | 1,463 | 530 | 8,954 | 985 |
| 100 | all on time | yes | 2 / 4 / 5 | - | 3,612 | 534 | 22,126 | 6,167 |
| 16 | 1 on a 300 ms one-way link | yes | 1 / 2 / 106 | 650 | 1,494 | 534 | 11,130 | 1,005 |
| 16 | 1 slow device (30 Hz) | yes | 1 / 74 / 106 | 18 | 1,456 | 522 | 8,954 | 971 |
| 100 | 5 on 300 ms links | yes | 2 / 3 / 106 | 650 | 3,654 | 534 | 149,395 | 6,283 |
| 100 | 5 on 300 ms links, `flushMs` 33 | yes | 20 / 37 / 138 | 651 | 3,097 | 534 | 149,397 | 2,308 |

"Seal latency" is from a client sending frame f to it receiving f sealed.
The p99 of ~106 ms in the slow scenarios is the deadline being paid while a
late seat is still expected, before it is excused. Down includes the
per-second stats and roster messages; the sealed stream alone was 2.1 KB/s
per client at 100 seats. The join burst (rosters while 100 clients connect)
is the cost the roster coalescing bounds; the slow-link runs show it higher
because a delayed client receives rosters that coalescing could not merge.
Over `wrangler dev` (local workerd), 4 clients: p50 1 ms, p95 2 ms, p99 161 ms,
1.2% of frames sealed at the deadline -- local workerd's scheduling jitter,
not the protocol.

The room seals at the deadline only for seats that miss it; excusal stops a
chronically late seat costing anyone else latency. What grows with N is
fan-out: every sealed frame goes to every member, so 100 clients is 6,000
messages a second out of one DO. `flushMs` (coalescing sealed frames, e.g.
33 ms) cuts that roughly in half at the price of latency; the roster, which
is O(N) and broadcast on change, is coalesced (`rosterMs`, 250 ms) in rooms
above 8 so a burst of joins is not O(N²).

**What a 16-to-100-player session needs, in order:**

1. **The deadline sequencer as specified here** (done): nobody waits; a slow
   player costs only themselves (their own inputs land late, the room fills
   them).
2. **Observer tiers.** Spectators already take no seat and send no input.
   Add a *relay tier*: a few trusted peers (or a second DO) re-broadcast the
   sealed stream to spectators, so the room's fan-out is to players only.
   Spectators run confirmed-only and never need syncs beyond their join.
3. **Sharding by area.** A big session is many small simulations: peers in
   the same game room (or area) share a sim and need each other's inputs;
   peers elsewhere do not. A *shard* is (room-or-area id, sim state); each
   shard is its own SessionCore (its own seats, sequencer, referee, donor),
   and a player moving between areas is a LEAVE from one shard and a
   snapshot JOIN to the other -- the same operations as today. A directory
   DO above the shards holds the session's global state (who is where, the
   save). The shard boundary must be one the game respects: KATAM's levels
   already isolate rooms (per-room object pools, per-room `LevelInfo`), so
   "players in the same room" is the natural unit.
4. **Interest management inside a shard** if a shard itself gets crowded:
   FRAMES per member filtered to the seats near it would break the
   one-stream-for-everyone property, so prefer smaller shards.

**What the protocol needs now so this is possible later** (all present):
seat counts as parameters, not constants; seat ids u16 with bitmask frames;
per-member stream state in the room (`streaming`, delta base) so a member can
be moved between streams; the sync relay keyed by transfer id, not by seat;
`signal` relay as the seam for WebRTC (M6) and for relay-tier peers; and a
room id that is not the code, so a directory can map one code to many
shards.

## 14. Simulation past 4 players (the sim side)

§13 covers the protocol, which is sized by parameters. This section is the
simulation's half. Citations are at `main` 568985c. "Measured" means a
1edaa0f binary or a scratch instrumented copy that is layout-neutral
against it, on a Ryzen 9 9950X with node 22.

### 14.1 Where the game assumes four

Counted by grep and a loop classifier:

- **56** `for` loops with a literal 4 that index a per-player array. 22 are
  in `cutscene_trigger.c` and 13 in `pause_help.c`.
- **96** unrolled `gKirbys[0..3]`, mostly in `kirby_cannon.c`,
  `eight_dir_cannon.c`, `code_0806F780.c` and `scroll_lock.c`, plus **33**
  unrolled `gCurLevelInfo[0..3]`.
- **74** `[gNumHumanPlayers - 1]` indexes into 4-column ROM tables
  (difficulty, e.g. `bombar.c:668`).
- 513 `gLocalPlayerId` reads.
- 425 uses of the owner bit test `Macro_0810B1F4`.
- 220 `FindTargetKirby` calls.
- Loops bounded by `gNumKirbys` (99 of them) are correct once the arrays
  are wider.

| item | today | to lift | class |
|---|---|---|---|
| `gKirbys` | 0x02020EE0, 424 B × 4 | relocate (sim block), +424 B/player | layout |
| `gCurLevelInfo` | 0x02023530, 1640 B × 4, including a 64-node object pool | relocate, +1640 B/player | layout |
| AI `gUnk_02038590` | 244 B × 4; created for `arg0..3` (`code_080332BC.c:64`) | relocate | layout |
| input block `gUnk_020382D0` | `unk8[3][4]` etc. (`include/main.h:27`) | widen every inner `[4]` | layout |
| per-Kirby animation table and demo buffer | `gUnk_02021590`, `gUnk_02038990` | relocate | layout |
| **room slots** | ≈19.8 KB per slot (tilemap, collision, lists), indexed by `LevelInfo.unk65E`; one per *occupied room*, a u8 mask over 4 (`level.c:345-360`) | widen the mask; slots ≥ distinct occupied rooms; split the slot/player loop at `object_collision.c:586` | layout |
| owner/visibility mask `gUnk_03000510` | bits 0-3 are per Kirby, 0x10 is a flag (`object.h:46-62,226`) | u32, move the flag high | layout |
| enemy targeting | `FindTargetKirby` scans the room (`kirby.c:5352`); `sub_0803D46C` has a local `u8 array[4]` (`:5390`, overflows past 4) | widen; O(enemies × N) | cost |
| difficulty | 74 `[gNumHumanPlayers-1]` into 4-column ROM tables | policy: clamp at 4 | design |
| phone/helpers, HUD | 4-entry ROM tables (`hud.c:1487`) | disable / new HUD | design |
| OBJ palettes | Kirby *i* uses palettes *i* and *i*+4 (`kirby.c:6145-6168`) | 8 Kirbys use all 16; beyond that, palettes per viewer (M8) | hardware |
| OBJ VRAM | 2 KiB per Kirby at 0x06010000 + (i << 11) | 16 Kirbys use all 32 KiB; beyond that, per viewer (M8) | hardware |
| task pool | 128 (`task.h:16`); full means objects silently do not spawn; peak 58 measured | 100 players need about 200 for the Kirbys alone; move to EWRAM | layout |
| SIO identity | 2 bits (`code_080332BC.c:44`) | retired in v2 | n/a |
| port | `PORT_MP_PLAYERS 4` → `PORT_RB_PLAYERS`; `warp.c:162`; the sim block is already sized by `PORT_MAX_SEATS` | widen | layout |

`FillLevelInfo` already hands a leaving Kirby's spawned objects to the Kirby
that stays (`level.c:308-325`): a per-room ownership hand-off, which is the
primitive sharding needs.

### 14.2 Cost today

**Measured**, per frame, with the scanline loop running and composition off:

| | native | wasm (node) |
|---|---|---|
| sim (logic + VBlank handler) | mean 2.7 µs, p99 6.5 µs | mean 7.5 µs, p50 5.3 µs |
| scanline loop | 0.64 µs | 1.31 µs |
| composition | 715 µs | 1198 µs |

Where the time goes:

- 112-114 ns per Kirby task, 64-68 ns per AI, 109-145 ns per object.
- **Objects drive the cost, not rooms.** All 4 Kirbys in one room costs
  2.2-3.4 µs, spread over 4 rooms 2.9-3.2 µs.
- Per Kirby, from N = 1..4: `1.96 + 0.25·N` µs in one room, `1.80 + 0.21·N`
  spread.
- Entering a room nobody else is in costs 11-19 µs once (decompress into
  the slot). For the local Kirby it is 80-135 µs, mostly VRAM.

### 14.3 Estimates at 8, 16 and 100

The model is `T(N) ≈ 1.9 + 0.23·N + 0.63 µs + Q`, where Q is the
O(enemies × N) targeting term: 25-50 µs at 100 players, 50 rooms and 250
enemies. It is extrapolated from N ≤ 4. A mid-range phone is taken as ×3
wasm (Geekbench ratio, cited).

| N | native | wasm | phone mid | rollback / catch-up of 10 frames, phone |
|---|---|---|---|---|
| 4 | 3.3 µs | 6.6 µs | 20 µs | 0.28 ms |
| 8 | 4.4 | 10 | 30 | 0.40 |
| 16 | 6.2 | 14 | 43 | 0.64 |
| 100 | 50-75 | 115-175 | 350-520 µs | 4.7-7.2 ms |

Under v2.1 (no rollback; catch-up only after a sync), CPU fits even at 100.

### 14.4 Snapshot size

**Measured.** EWRAM+IWRAM is 288 KiB raw and 8.9-18 KB at zstd-3. That is
about 1.5-2.2 KB per occupied room and 0.35-0.45 KB per Kirby. The engine's
full image (§12.6) adds VRAM and SRAM, giving 57-67 KB at zstd-1.

**Estimated**, with as many room slots as players:

| N | rooms | raw | zstd |
|---|---|---|---|
| 8 | 8 | 395 KB | 23-29 KB |
| 16 | 16 | 596 KB | 38-49 KB |
| 100 | 50-100 | 2.7 MB | 125-270 KB |

A 1 s delta is 1-5 KB today and grows with activity, not with N.

### 14.5 Recommendation

- **8: reachable by widening and relocating** the arrays above into the
  sim block, behind a build flag (`PORT_MAX_SEATS=8` already sizes the
  block). Two decisions are needed: palettes (all 16 used by Kirbys) and
  four new Kirby colours.
- **16: also needs the M8 presentation split.** OBJ VRAM and palettes must
  be assigned per viewer, to the Kirbys on that viewer's screen. The task
  pool must move to EWRAM.
- **100: room sharding, not one world.**
  - A shard is a set of game rooms, simulated only by the peers whose
    players are in it, with its own sealed stream and donor (§13).
  - **Handoff:** the old shard seals the Kirby at an agreed frame. Its
    record (5.3 KB raw, about 0.45 KB compressed) moves to the destination,
    which loads the room slot (11-19 µs).
  - **Global state** (doors, chests, treasures, boss flags, `gWorldProps`)
    becomes server events, not lockstep.
  - **Drift repair** is a per-shard donor sync of 10-20 KB.
  - **Design work:** "everyone in the room" gates (`scroll_lock.c:277`,
    cutscene triggers) become per-shard quorums; difficulty is clamped;
    phone helpers are disabled.
  - **Cap per shard:** about 8 Kirbys, the palette limit. The collision
    lists hold 32 per kind and slot, with no bounds check
    (`code_0806F780.c:760-764`).
- **A `KATAM_MAX_KIRBYS=8` flag** would have to change:
  - `ram_symbols.h` relocations for every per-player and per-slot array;
  - the inner `[4]`s of `Unk_020382D0`;
  - `Unk_03000510`;
  - `level.c:345-360`, `code_080332BC.c:59,64`, `object.h:46-62,226`,
    `kirby.c:5390` and `kirby.c:6137-6168`, `object_collision.c:586`;
  - the 56 + 129 literal and unrolled sites;
  - the port's `PORT_MP_PLAYERS`.

  Not prototyped here.

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
