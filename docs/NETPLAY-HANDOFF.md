# Room-based online multiplayer (protocol v2): hand-off

**Status: shelved, 2026-11-08. Hidden behind `?online=1`.** The code stays in
the tree and deployed; nothing links to it from the normal UI.

The user tried it and said: *"the room based network multiplayer is awful.
Desyncs abound, and players are constantly rubberbanding back to several
seconds before. Let's abandon that route, but please document exactly how
it's set up for a future agent to tackle."*

This document is that record. It covers how the feature is built, file by
file; how to run every piece of it; what was measured before and after the
user's report; and a diagnosis of the two symptoms, with numbers, ranked
causes and proposed fixes. The design it implements is
`docs/design/NETPLAY-V2.md` §12 ("Model v2.1"). Read §12 after this, not
before.

Conventions: **Measured** means run for this document, at `main` d25d7d8,
with the real ROM, in the scratchpad. **Cited** means copied from another
doc, with its source named. **Read** means taken from the code without
running it.

---

## 0. Start here (checklist for the next agent)

1. Read §1 (decision), §2 (architecture) and §6 (diagnosis) of this file.
   Then read NETPLAY-V2.md §12.
2. Build: `make build/katam-node.js` (about 10 s with a warm cache). Then run
   `cd netplay && npm ci && npm test`, which should give 30 passing tests.
3. Reproduce the failure in about a minute with no browser:

       node tools/netplay_diag.mjs build/katam-node.js $ROM --seconds 60 --link-b 120

   You should see beta with `lateDrops` in the thousands, `hashes` far below
   alpha's, and `syncReasons` full of `lag` and `mismatch`. Then run it with
   `--link-b 60` and see none of that.
4. Choose a direction from §7 before you write code. The cheapest fix that
   would stop the reported symptoms is §7.1 A1+A2: bound the lead and
   decouple checkpoints from the ring. My architectural recommendation is
   §7.2 option 2, host-authoritative state streaming.
5. Gate any change on `tools/netplay_diag.mjs` at `--link-b 90` and
   `--link-b 120`, not only on `tools/netplay_v2_test.mjs`. The e2e test's
   120 ms link sits on the edge of the failure (§5.3) and passes by luck.
6. Do not deploy from a branch. The worker is live and shared (§4.3).
   `wrangler dev` is enough for everything here.
7. To bring the feature back into the UI, revert the `?online=1` gate that
   branch `webui` adds. Do it only once §6's causes 1-3 are fixed.

---

## 1. Decision

| | |
|---|---|
| Date | 2026-11-08 (built and shelved the same day) |
| Decision | Shelved. The UI entry points are hidden behind `?online=1` (branch `webui`). Code, tests and the deployed worker are kept. |
| Why | Desyncs, plus constant rubberbanding by several seconds (the user's words above). §6 shows why. The short version: a client on a link of roughly 150 ms RTT or worse runs further ahead of the sealed input stream than the engine's 32-frame rollback ring reaches. Every older input is then silently dropped, so that client's world diverges. The repair, a world sync, comes 2-3 s later and snaps it back, and then the divergence starts again. |
| What still works | LAN and symmetric links up to about 60 ms one way: no desyncs in 60 s runs (§6.1). The older link-cable and Path B modes (`?room=name`, `?world=name`, docs/NETPLAY.md) are separate, and their tests pass. |

---

## 2. Architecture

### 2.1 The pieces

```
 browser tab (one per player)                     Cloudflare (katam-netplay worker)
 +-------------------------------------------+    +----------------------------------+
 | web/lobby.js      UI, ?room= / ?online=1  |    | server.mjs  fetch: /v2/*         |
 | web/net_session.js  KatamNet: HTTP + WS,  |<-->|   Directory DO (one, 'global')   |
 |                     codecs, reconnect     | WS |     codes.mjs + directory.mjs    |
 | web/net_game.js   drives the engine:      |    |   SessionRoom DO (one per room)  |
 |   seal, stamp, time sync, checkpoints,    |    |     SessionCore (session.mjs):   |
 |   world sync (donor + target)             |    |     lobby, seats, sequencer,     |
 | katam.wasm                                |    |     archive, referee, donor      |
 |   platform/rollback.c  (USED: ring, seal, |    |     election, sync relay         |
 |     rollback, snapshot-at, apply-at)      |    |     wire.mjs (binary codecs)     |
 |   platform/netsim.c    (NOT used by web)  |    +----------------------------------+
 +-------------------------------------------+    dev: netplay/dev-relay.mjs (same core,
                                                       node http + ws)
```

### 2.2 Per frame: input to sim (Read; the numbers are defaults)

```
local keys (host poll, Module._PortCurrentKeys)
  -> net_game.tick() [called from Module.portPresent after every presented frame]
       stamp at frame now+delay (delay 1..12, adaptive):
         Module._PortRbConfirmInput(seat, f, keys)   -- our own column, applied locally at once
         net.sendInput(from, keys[])                 -- INPUT 0x20 u32 first,u8 n,n*u16
  -> SessionRoom: SessionCore._input
       pending[f].keys[seat] = keys  (only if f >= frontier, else lateInputs++ and dropped)
       _advance(): seal frame F = frontier when every *expected* seat's input for F is in,
                   or at due(F) + deadlineMs (100); due(F) = clock.t + (F-clock.f)*16.74 ms,
                   with the clock anchored on the first input heard
       _seal: missing seat -> "repeat last" (m.latest at or before F), fills++,
              lateStreak++; after excuseAfter=20 fills in a row the seat stops being
              waited for, until rejoinAfter=10 inputs in a row arrive on time
       _flush: FRAMES 0x21 to every streaming member (delta vs. the previous frame)
  -> net_session FrameDecoder -> 'frames' -> net_game queue
  -> next tick: pump(): Module._PortRbSealInput(s, f, v[s]) for every seat
       platform/rollback.c PortRbSealInput: overwrite the timeline; if F was already
       simulated with another value -> rollback to F (sCatchUp=2) at the next frame top,
       if the ring still holds F; else  lateDrops++  and the input is LOST (the timeline
       changes, the state does not)
  -> PortRbFrame (top of every frame, platform/main.c PortPresentFrame):
       pending world sync? -> load it; rollback requested? -> restore ring slot;
       save ring slot (403 KiB memcpy); apply events; drive the slots from the timeline
       (sealed, else predicted "repeat last")
```

### 2.3 Hashes, referee, donor, sync (Read; flow verified in the logs)

```
every hashEvery=30 frames C past inputStart, once C <= frontier (inputs < C sealed),
C < now, and now - C <= 30 (the ring still holds C):
  net_game.checkpoints: Module._PortRbSnapshotAt(C) -> copy (kept: last 8)
                        simHash = FNV over gRngVal + gKirbys[0..3].x,y      <- the ENTIRE referee hash
                        {t:'hash', f:C, h}
  SessionCore._hash/_judge: when every live seat has reported C, or after hashWaitMs (1.5 s):
        majority hash = reference; tie -> group with the best donor; each peer outside
        the reference -> _mismatch -> _startSync(m, 'mismatch'); everyone gets 'desync'
  other triggers (_housekeeping, every tick):
        'lag'      lastHashFrame + 90 < reference.f          (a peer that stopped hashing)
        'periodic' syncEveryMs = 10 s since the last sync    (never the would-be donor)
        'join'     3 s after a mid-game sit;  'ask' = {t:'syncme'} (no client sends it)
        rate limit: minSyncGapMs = 2 s per peer (not for join/ask), 1 in flight, 8 s timeout
  _donorFor: a member holding the reference hash at reference.f;
             electDonor 'majority': longest cleanStreak, then RTT, then seated longest
  the sync, relayed opaquely by the room:
    room  -> target  sync_req  {id, f = donor.lastHashFrame, donor, reason}
    target-> room    sync_have {id, f, ps:4096, hashes: base64(u32 FNV per 4 KiB page of ITS copy at f)}
    room  -> donor   snap_req  {id, f, to, ps, hashes}
    donor -> room    snap {id,f,size,count,h,kind:'pages'|'full'} + count x CHUNK 0x30 (16 KiB)
                     blob = 36-byte KSNP v0 header + deflate-raw(pageSize,nPages,count,idx[],pages)
    room  -> target  snap ..., chunks
    target: applyBlob onto its own copy at f; check simHash == donor's h;
            Module._PortRbApplySnapshotAt(f, snap)
            -> next frame top: LoadFrom(snap); ring invalidated; if it was ahead of f
               (normal): re-simulate f..old with the picture off from ITS OWN timeline
               (sealed inputs, plus predictions and its own stamped inputs past the
               frontier), in one burst; if it was behind f: jump forward to f
            correction event {frame:f, kind:'sync', pages, bytes, reason, at}
            -> opts.onCorrection, Module.portNetCorrection, DOM 'katam-net-correction'
               (nothing in the page listens to the last two; the GPU renderer's
               PortRlNoteCorrection is only called by netsim.c, not on this path)
    target-> room    sync_done {id, ok}
```

To answer the brief's questions directly:

- **When is a sync taken at checkpoint F applied?** At the target's next frame
  top after the blob arrives. That was 2-375 ms after `sync_req` in my runs
  (§6.1, `syncWall`).
- **Does the catch-up resim complete?** Yes, in one burst with the picture
  off. The re-run is 23-91 frames (§6.1, `rewind`).
- **Does the receiver re-run its own inputs since F?** Yes. Its timeline
  holds everything sealed since F, plus its own stamped inputs and its
  predictions past the frontier. The donor's inputs are not sent and are not
  needed.

So a sync does **not** by itself rewind the picture by `now - F`. What the
player sees is the gap between the target's own wrong world and the donor's
right one, re-simulated to now. When the target was dropping inputs, that
gap is seconds of wrong motion (§6).

---

## 3. File map

### 3.1 Server: `netplay/`

| file | role | state |
|---|---|---|
| `server.mjs` | Worker entry. `/v2/*` goes to `v2()`: `Directory` DO (`idFromName('global')`) for codes, `SessionRoom` DO per room id (128-bit, never shown). Anything else goes to partyserver's `GameRoom` (protocol v1). `SessionRoom` keeps one `SessionCore`, plain WebSockets (**no hibernation**, on purpose), and a `setTimeout` deadline timer from `core.nextWakeIn()`. It saves `exportState()` every 30 s and when the room empties. | live, deployed |
| `session.mjs` | **SessionCore**, the whole v2 room. Lobby (`_hello` version gate on `sim`/`rom`/`content`, seats, host, approval, kick, settings). Sequencer (`_input`, `_advance`, `_seal`, `_flush`, `_expected` with excusal). Events/watermark (`_event`, `_eventFrame`). Archive (`_sendArchive`). Referee (`_hash`, `_judge`). Donor election (`electDonor`, `_donorFor`). Sync relay (`_startSync`, `_syncMsg`, `_chunk`, `_endSync`). Timers (`_housekeeping`: drop to AI, host handover, judge timeouts, sync triggers, `stats` once a second). Persistence (`exportState`/`importState`). `DEFAULT_PARAMS` holds every tunable. | live |
| `wire.mjs` | v2 binary codecs: INPUT 0x20, FRAMES 0x21 / ARCHIVE 0x22, CHUNK 0x30. The client's copy is in `web/net_session.js` and must agree. | live |
| `codes.mjs` | 4-letter codes (alphabet `BCDFGHJKMNPQRSTVWXZ`, look-alikes, blocklist), code directory, resume tokens, `RateLimiter`. | live |
| `directory.mjs` | `DirectoryService`: create / lookup / resume with edge limits. | live |
| `dev-relay.mjs` | `startRelay(port)`: v1 rooms by path, plus v2 under `/v2/` (`startV2`), on node http+ws. `relay.v2.rooms` exposes each room's `core` to in-process tests. | live (dev only) |
| `protocol.mjs` | Protocol v1 (`RoomCore`: link-cable words, Path B inputs). | live for v1 (`?room=name`, `?world=`), not v2 |
| `framecodec.mjs` | Frame (picture) codec for server-side render (docs/SERVER-RENDER.md). | not part of v2 |
| `world/` | Spike of the sim running in workerd (`katam_host.mjs`, `boot.mjs`, `wrangler.spike.jsonc`). | spike, not deployed by `wrangler.jsonc` |
| `wrangler.jsonc` | Worker `katam-netplay`, account `52eac477…`. DO bindings `GameRoom`, `SessionRoom`, `Directory`. Migrations `v1`, `v2` (SQLite classes). | live |
| `test/*.test.mjs` | `codes`, `wire`, `session` (SessionCore against a fake clock), with `harness.mjs`. | 30/30 pass |
| `tools/load_test.mjs` | N synthetic clients, no game. | passes |

### 3.2 Web client: `web/`

| file | role | state |
|---|---|---|
| `lobby.js` | The v2 UI. It claims the page when `?room=CODE` (4 letters) or `?online=1` is present (`window.KatamLobby.claims`). Create/Join, seats, ready, host controls, status chip. `startSession()` blanks SRAM, hashes the ROM (SHA-1) and `simVersion()` (`window.KATAM_SIM` stamp, else `dev.w<sha1(katam.wasm)>`), connects, creates the game, and chains `Module.portPresent`, which calls `game.tick()`, and `Module.portNetIdle`. `caps.sync = !!M._PortRbSnapshotAt`. | live, hidden |
| `net_session.js` | `KatamNet`: `createRoom` / `lookupRoom` / `resumeRoom` (HTTP), `connect()` (WS, reconnect with backoff, ping/RTT, FrameDecoder, chunk reassembly), `normalizeCode`, `explainError`, `DEFAULT_BASE = 'https://katam-netplay.sh1ftmaker.workers.dev'`. | live |
| `net_game.js` | Drives the engine (§2.2-2.3). `begin()` runs the same boot as `rb_boot.js` (F0 = 2200, events at 0 and F0, then `PortRbReplayTo`). `tick()` handles pump, fast-forward, hold, focus, stamping, checkpoints and view requests. Also: `simHash`, `pageHashes`, `buildBlob` / `applyBlob` (KSNP v0 + deflate), the `stats` handler (adaptive delay), `clockTarget()` (time sync). | live, **stopgap engine API** |
| `rb_boot.js` | `createKatamWorldSession`: boot-to-world as inputs (`F0`, `bootMask`). Shared with Path B. | live |
| `rb_net.js` | Path B (v1) rollback driver over a v1 room (`?world=`), with the stall gate on. | v1, not used by v2 |
| `mp_net.js` | Path A (v1) link-cable transport (`?room=name`). | v1 |
| `shell.html` | Script tags for `net_session.js`, `net_game.js`, `lobby.js` (around line 4031). The v1 hookup returns early if `KatamLobby.claims` (around line 4072). | live |

### 3.3 Engine (C, exported to wasm)

| file | role | state |
|---|---|---|
| `platform/rollback.c` / `port/rollback.h` | The engine the web client uses. Timeline (`sTl`, 4 columns), prediction (repeat last), ring of `PORT_RB_MAX_FRAMES` = **32** full snapshots (403 KiB each, 12.9 MB of ring), `PortRbConfirmInput` / `PortRbSealInput` (rollback or `lateDrops`), events (`PortRbScheduleEvent`, `PortRbAssignSlot`), `PortRbFrame` (the frame hook), `PortRbReplayTo` (headless catch-up), `PortRbNetPlay` (focus). The **v2 stopgaps**: `PortRbSnapshotAt`, `PortRbApplySnapshotAt`, `PortRbSetStallGate(0)`. Same-build snapshots only (host pointers in the image). | live |
| `platform/netsim.c` / `port/netsim.h` | The intended engine (§12.6): `PortNetSessionStart`, a 1024-frame input ring, `PortNetSealFrame`, a canonical portable image (478,652 B), KSNP v1 full/delta/pages blobs with zstd, 64-bit state/region/page/checkpoint hashes, `PortNetApplyAt` (load plus headless catch-up), `PortNetLastCorrection` → `PortRlNoteCorrection`, and the menu-owner KEYINPUT latch (R8). **No rollback**: a sealed input that contradicts an already-simulated frame is recorded but not re-simulated (`PortNetSealFrame` just writes the ring). Exported (`_PortNet*` in the Makefile's `PORT_EXPORTS`); **called by nothing in `web/`**. | built, tested (xplat), unused |
| `platform/port/simblock.h` | Port sim block at 0x02040000: session flag, slot map (`slotPeer`), story floor, frame state. It is inside both snapshots. | live |
| `platform/census.c` (+ `port/census.h`, `tests/census/masks.txt`, `tools/census.py`) | Divergence census: N seats differing only by focus, masked comparison (NETPLAY-V2 §3.4.1). Native only (`--census`). | tool; not converged |
| `platform/fnid.c` | Canonical code ids (R5), used by netsim's portable image. | live |
| `platform/seats.c` | N instances in one process (census, seat tests). | live |
| `platform/view.c` | Session view (`PortSimViewUseSession`, `PortInputLogScheduleView`; NETPLAY-V2 §2.2a). | live |
| `platform/render/rl_build.c` | `PortRlNoteCorrection` snaps renderer interpolation. Only netsim calls it. | not reached on the v2 path |
| `platform/main.c` | `PortPresentFrame`: `PortRbFrame()` then `PortNetFrameTop()`. Catch-up frames neither blit nor wait. | live |
| `platform/web/host_web.c` | `PortWebAwaitFrame`: rAF pacing, a debt repay of up to 8 periods, `Module.portFrameDue` (the hold uses it). Hidden tabs run on `setTimeout(4)`. | live |

### 3.4 The C/wasm API the web client actually calls (Read: `grep Module._ web/net_game.js`)

`_PortFrameNumber`, `_PortCurrentKeys`, `_PortRbInit(32, 4)`, `_PortRbSetStallGate(0)`, `_PortRbSetSelf`,
`_PortRbScheduleEvent`, `_PortRbAssignSlot`, `_PortRbConfirmInput`, `_PortRbSealInput`,
`_PortRbReplayTo`, `_PortRbCatchingUp`, `_PortRbNetPlay`, `_PortRbSnapshotBytes`,
`_PortRbSnapshotAt`, `_PortRbApplySnapshotAt`, `_PortRbGetStats`, `_PortSimViewUseSession` /
`_PortSimViewPinNative`, `_PortSimViewRequested`, `_PortInputLogScheduleView`, `_malloc`.

The intended replacement (NETPLAY-V2 §12.6) maps them like this:

| stopgap | intended |
|---|---|
| `_PortRbSealInput` | `_PortNetSealFrame` |
| `_PortRbSnapshotAt` | `_PortNetSnapSave` / `_PortNetPagesSave` |
| `_PortRbApplySnapshotAt` | `_PortNetApplyAt` |
| `_PortRbSetStallGate(0)` | not needed |
| JS `simHash` (RNG + 4 positions) | `_PortNetCheckpointHash` (whole image), after the census masks |
| JS u32 FNV page hashes | `_PortNetPageHashes` (64-bit, 117 pages) |

**Warning:** migrating as-is would make things worse. netsim has no rollback,
so every mispredicted remote input would become a divergence that only a
world sync repairs (§7.1 A6).

### 3.5 Wire protocol v2 (Read: `netplay/README.md`, `session.mjs`, `wire.mjs`)

HTTP:
- `POST /v2/rooms` returns `{code, resume}`.
- `GET /v2/rooms/CODE`
- `POST /v2/resume {token}`
- WebSocket at `/v2/rooms/CODE/ws`.

Control messages are JSON, with the type in `t`.

| direction | messages |
|---|---|
| client → room | `hello {proto:2, sim, rom, content, identity, name, platform, view{w,h}, caps{seats, sync}, want:'play'\|'watch', create?{params}}`, `sit {seat?}`, `stand`, `leave`, `ready`, `name`, `view`, `archive`, `hash {f,h}`, `syncme`, `sync_have {id,f,ps?,hashes?}`, `snap {id,f,size,count,h,kind}`, `snap_fail {id,why}`, `sync_done {id,ok,pages?,why?}`, `ping {c,rtt}`, `signal {to,data}` |
| host only | `start`, `lock`, `approval`, `approve`, `deny`, `kick`, `settings {params}` |
| room → client | `welcome {mid, role, seat, phase, params, host, frontier}`, `roster`, `error {code, field?, room?, yours?}`, `start {params, frame, events}`, `ev {seq, f, kind: seat\|unseat\|slot\|view, ...}`, `archive_begin {from, to, events}` / `archive_end {to}`, `stats {frontier, head, wm, clock, late, fills, ahead, sealed, deadlineSeals, behind}` (1/s), `desync {f, groups[{h,mids}], ref}`, `sync_req {id,f,donor,reason}`, `snap_req {id,f,to,ps,hashes,reason}`, `snap {..., from, reason}`, `sync_cancel {id,why}`, `join_req`, `notice`, `pong`, `signal` |

Binary messages, all little-endian:

| message | layout |
|---|---|
| INPUT | `0x20 u32 first u8 n n×u16` |
| FRAMES / ARCHIVE | `0x21/0x22 u8 flags(bit0 key) u16 seats u32 first u16 n u32 watermark`, then per frame `ceil(seats/8)` bytes of changed-seat mask + `u16` per changed seat (delta against the previous frame, or zeros on a key message) |
| CHUNK | `0x30 u32 id u16 index u16 count` + up to 16 KiB |

The FRAMES delta has **no sequence number**. A dropped or reordered message
decodes silently into wrong inputs. TCP prevents that today. An unreliable
WebRTC channel would not (§6.4).

---

## 4. How to run everything (each command run for this document, 2026-11-08)

`ROM=/path/to/baserom.gba`. Build first: `make build/katam-node.js` (emsdk in
`~/emsdk`). For the native tools, build the LP64 tree with
`cmake -S . -B build/lp64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKATAM_ALLOW_LP64=ON && cmake --build build/lp64 -j`
(22 s).

### 4.1 Server and unit tests

| command | result today |
|---|---|
| `cd netplay && npm ci && npm test` | 30/30 pass |
| `cd netplay && npm run relay` (dev relay on 127.0.0.1:8787; run under `sbg` here) | `curl -X POST 127.0.0.1:8787/v2/rooms` returns `{"code":"CPVF","resume":…}` |
| `cd netplay && npx wrangler dev --port 8788 --ip 127.0.0.1` (under `sbg`) | "Ready on http://127.0.0.1:8788"; the e2e test passes against it (§4.2) |
| `cd netplay && npm run load -- --clients 16 --slow 1` | LOAD TEST PASSED: 528 B/s up per client, 4.8% deadline seals, slow client up to 27 frames behind |

### 4.2 Game end to end

| command | result today |
|---|---|
| `node tools/netplay_v2_test.mjs build/katam-node.js $ROM` | PASSED 3 of 3 runs alone: repair 1.76-1.86 s, tails agree. **But** beta (120 ms link) logged `lateDrops` of 0, 268 and 604 across those runs, and the room counted 5-10 desyncs. |
| `... --url http://127.0.0.1:8788` (wrangler dev) | PASSED alone (repair 1.96 s). **FAILED** once when run concurrently with another session and a census: repair 14.4 s, tails differ, beta lateDrops 5320, beta hashed only 34 of 79 checkpoints. The test is on the edge (§6.2). |
| `node tools/netplay_diag.mjs build/katam-node.js $ROM [--link-b 120] ...` | new, diagnostic only (§6). The options are in its header. |
| `make netplay-rb-test netplay-boot-test netplay-link-test netplay-test ROM=$ROM` | all PASS. These are v1 / Path A / Path B, not v2. |

### 4.3 Deploy / undeploy (manual, never from CI; **do not run casually**)

- Deploy: `cd netplay && npx wrangler deploy`. This was last done 2026-11-08,
  version `c2e935b5-924a-4e3a-b725-c4c52aacb64c`, URL
  `https://katam-netplay.sh1ftmaker.workers.dev`. It still answers: a lookup
  of `GET /v2/rooms/BCDF` gives "No game with that code".
- Rollback to an earlier version: `npx wrangler rollback [version-id]`.
  List versions with `npx wrangler deployments list`.
- Undeploy the whole worker: `npx wrangler delete`. This also kills v1
  `?room=` / `?world=`, which share the worker.
- Cost note: `SessionRoom` does not hibernate. A room with sockets open is
  billed by wall-clock duration (the design estimate is ~$0.0065/h per
  4-player room, NETPLAY-V2 §13). Storage is one `state` value per room (the
  archive, `exportState`). SQLite DO values are capped at 2 MB, and the
  archive grows with session length (§6.5).

### 4.4 In a browser

Serve `build/dist` (`make dist`) or `web/` with the wasm. Open
`?online=1&relay=http://127.0.0.1:8787`, choose Create, and open the invite
link in a second tab. Each tab is a player. Other URL parameters:

| parameter | effect |
|---|---|
| `?room=CODE` | join |
| `&watch=1` | spectator |
| `?relay=URL` | server override (the default is `KatamNet.DEFAULT_BASE`) |
| `?room=name` (not a 4-letter code) | v1 link cable |
| `?world=name` | v1 Path B |

Two tabs on one machine are a poor test. A hidden tab paces on
`setTimeout(4)`, which browsers throttle, so it falls behind and syncs
(compare `--stall-b` in §6.1). I did not run a browser session for this
document. The node harness reproduces both symptoms without one.

### 4.5 Engine-level tests

| command | result today |
|---|---|
| `ROM=$ROM tests/netsim_xplat.sh play_rooms` | PASS: snapshot@3000 identical across native and wasm (64,525 B); cross-loads 0/4299 frames differ |
| `build/lp64/katam --no-audio --turbo --replay tests/recordings/play_rooms.txt --net-test session:1250,rewind:3000:120 $ROM` | NETSIM REWIND PASSED, state B21D0ABEBDE38FC1, catch-up of 121 frames in 1.5 ms |
| `REPLAY=tests/recordings/play_rooms.txt NET_TEST=session:1250,rewind:3000:120 AUDIO=0 node tools/headless_test.js build/katam-node.js $ROM 3200` | NETSIM REWIND PASSED, state **98CFFA336C7D1D5A**, 3.3 ms |
| `build/lp64/katam --no-audio --turbo --replay tests/recordings/boot_to_level.txt --census 4:450:OUT --census-masks tests/census/masks.txt $ROM` | 1399 rounds, 906 with an unmasked difference (first at frame 493), 234 blocks, **38 unmasked**. This matches NETPLAY-V2 §3.4.1. |

Two notes on that table:

- **Open discrepancy:** NETPLAY-V2 §12.6 says the native and wasm rewind
  hashes are equal (`98CFFA336C7D1D5A` both). At d25d7d8, native LP64 gives
  `B21D0ABEBDE38FC1` for the same spec. xplat (session at 50) still passes,
  so this is either a session-start-dependent difference or a stale claim.
  Not investigated.
- The census needs `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

---

## 5. Measurements before this document (cited)

| what | number | source |
|---|---|---|
| injected desync (beta's gRngVal flipped) detected and repaired | 1.6-3.5 s | NETPLAY-V2 §12.4 (tools/netplay_v2_test.mjs) |
| sync after a real divergence | 9-19 pages, 6-16 KB; a full join sync is 97 pages, 51 KB | §12.4 |
| traffic per client | 1-2.3 KB/s down, 0.3-0.6 KB/s up; donor up to 6.5 KB/s up | §12.4 |
| excusal | without it, a chronically late seat forced 100% deadline seals, 102 ms p50 latency; with it, p50 1-2 ms | §12.2 |
| netsim full blob | 56.8-67.0 KB of 478.7 KB raw; compress 240-395 µs | §12.6 |
| netsim delta | 1 s apart 0.9-4.7 KB; 5 s apart 1-6 KB within a room, 30-62 KB across a room change | §12.6 |
| catch-up of 121 frames headless | 1.1-1.6 ms native, 3.3 ms wasm | §12.6 |
| census | 38 unmasked blocks (boot_to_level), 106-191 in the others | §3.4.1 |
| wasm sim cost | ~6 µs/frame | §12.3 (cited there) |

---

## 6. Diagnosis

**Tool.** `tools/netplay_diag.mjs` (added on this branch, read-only). It runs
two real instances (alpha the host, beta), the real client and the real room
(the in-process dev relay, or `--url`), paced at 59.73 Hz. It adds per-client
one-way latency, with jitter in strict FIFO order like TCP. Input is
"human-like" (held directions of 6-40 frames, jumps, B taps). It records:

- sync reasons;
- every correction: checkpoint, apply frame, rewind depth, wall time, and the
  own and other Kirby's displacement across it;
- per-frame position jumps by cause;
- own-input overrides;
- lead over the sealed frontier;
- engine stats;
- both peers' full images at every common checkpoint, as raw diff and after
  the census masks;
- each engine's simulated timeline against the sealed stream it was sent.

Raw JSON is in the scratchpad (`s*`, `t*`, `u*`, `v*`, `w*`, `y*`).

> **Harness pitfall (cost me a wrong first conclusion).** Independent
> `setTimeout`s per message reorder messages. The FRAMES delta decoder then
> silently produces wrong sealed inputs, which looked like a client-side
> divergence at 30 ms ± 20 ms. The diag harness uses FIFO queues. The
> `laggyWebSocket` in `tools/netplay_v2_test.mjs` uses fixed latency, which
> does not reorder. Any runs labelled `s2`, `s4`, `s6` in the scratchpad are
> from the reordering harness: ignore them.

### 6.1 Results (Measured, 60 s unless noted; human-like input)

The link column is alpha / beta one-way latency in ms; j is jitter.

| run | link | room desyncs | beta syncs (reasons) | beta lateDrops | beta lead over frontier p50/p90/max | beta checkpoints hashed (alpha's) | beta rewind frames p50/max | other-Kirby jump at a correction (max px) |
|---|---|---|---|---|---|---|---|---|
| v1 | 30 / 30, j15 | 0 | 2 periodic | 0 | 5 / 10 / 12 | 118 (118) | 23 / 23 | 23 |
| v5 | 60 / 60, j20 | 0 | 1 periodic | 0 | 8 / 14 / 16 | 118 (118) | 41 / 41 | 1.3 |
| v2 | 0 / 60 | 0 | 5 periodic | 0 | 8 / 13 / 17 | 118 (118) | 34 / 40 | 8.5 |
| v3 | 0 / 90 | 8 | 3 mismatch, 1 lag, 4 periodic | **2,020** | 12 / **31** / 38 | 102 (119) | 44 / 54 | 77 |
| s3 | 0 / 120 | 22 | 7 mismatch, 6 lag, 2 periodic | **5,912** | **28** / 34 / 40 | 75 (118) | 53 / 61 | 112 |
| v4 | 0 / 120, j40 | 15 | 10 mismatch, **10 lag** | **12,656** | **32** / 37 / 44 | **19** (118) | 70 / 89 | 158 |
| y1 | 0 / 120, **wrangler dev**, 45 s | 8 | 1 mismatch, **15 lag** | **9,348** | **37** / 51 / 66 | **13** (89) | 74 / 91 | **329** |
| w1 | beta at 30 Hz | 6 | 3 mismatch, 4 periodic | 0 | -1 / 3 / 7 | 119 (119) | 10 / 31 | 17 |
| w2 | LAN, beta frozen 5 s | 0 | 2 lag, 4 periodic | 672 (benign, see below) | 0 / 5 / 14 | 110 (119) | jump forward 185 | 35 (own 37) |
| w3 | 30 / 30 j15, beta frozen 5 s | 0 | 2 lag, 5 periodic | 708 (benign) | 5 / 11 / 19 | 109 (119) | jump forward 189 | 46 (own **140**) |

**Rubberbanding.** On the bad links, beta's own Kirby moved up to 40 px at
a sync (v4) and 24 px (y1), and 9-18 times a minute on rollbacks. The other
Kirby moved 100-330 px, once every 2-3 s.

**Engine timeline against the sealed stream.**

| runs | frames where the engine simulated a different input than the room sealed |
|---|---|
| v1, v2, v5 | 0 of ~3,530 |
| v3 | 285 |
| s3 | 516 (nosync variant) |
| v4 | **1,840 (52%)** |

All of the differences are on column 0, alpha's: the column whose sealed
values reach beta latest.

**Natural divergence with no syncs at all** (`--nosync`, LAN, 120 s, run s5):

- 0 of 238 checkpoints had a referee (simHash) mismatch.
- The full images always differ in 12-21 pages.
- Bytes still different after my approximation of the census masks grow from
  25 at the first checkpoint to ~1,500 after 5 s, then plateau.

My masking is approximate: `census.c`'s per-round sprite-field masks are not
replicated, so this overcounts. The top ranges are in the gKirbys struct
(sprite fields), the IWRAM task heap (0x0300417c, 717 B; 0x03004604, 798 B)
and EWRAM 0x02022138.

With 120 ms and no syncs (v6), simHash mismatched at 84 of 89 checkpoints,
and the first mismatch came 8 s in.

### 6.2 Root causes, ranked

**1. The rollback ring is shorter than the lead the time sync gives a
far client. Late sealed inputs are dropped and the world diverges for good.
(Primary cause of "desyncs abound".)**

- `net_game.js` runs each client at
  `clock + one-way + ceil(one-way) - delay + 2` (`clockTarget`) and stamps its
  input `delay` ahead. The sealed stream reaches it one more one-way later.
  Seals also wait up to `deadlineMs` (100) for a late seat.
- The client's present therefore leads its sealed frontier by about
  RTT + seal wait + 2 frames. That is p50 28-37 frames at 120 ms one-way
  (a 240 ms RTT link).
- The engine's ring is 32 frames (`PORT_RB_MAX_FRAMES`, 535 ms). Any sealed
  input for a frame more than 32 frames back is counted (`lateDrops`) and
  dropped: `PortRbSealInput` changes the timeline but cannot re-simulate.
- The client keeps the wrong state and nothing tells it. The room sees it
  only through hashes.
- Where it starts: at 60 ms one-way on the far client, never (0 drops in 60
  s). At 90 ms, a few percent of frames (lead p90 31). At 120 ms, constantly.
- RTTs of 150-250 ms are normal for a phone or Wi-Fi player whose
  `SessionRoom` sits near the *other* player. The DO is placed near whoever
  created the room.
- `PortRbSetStallGate(0)` deliberately removed the guard that made v1 stop
  at the ring edge. Nothing replaced it.

**2. Checkpoints are tied to the ring, so the far client cannot report its
hashes and is synced for 'lag' every ~2-3 s. (Primary cause of "constantly
rubberbanding".)**

- `checkpoints()` skips checkpoint C when `now - C > DEPTH - 2` (30): the
  ring no longer holds the frame-start state of C.
- A client that leads its frontier by 30 frames or more hashes almost
  nothing: 19 of 118 checkpoints (v4), 13 of 89 (y1).
- The room then fires `lag` whenever `lastHashFrame + 90 < reference.f`,
  rate-limited only by `minSyncGapMs` (2 s): 10-15 lag syncs a minute.
- The referee also waits the full `hashWaitMs` (1.5 s) on every checkpoint
  for the missing report, which delays mismatch syncs for everyone.
- Each sync re-simulates from the donor's checkpoint, 70-91 frames back.
- Because of cause 1, the client's world had drifted in the meantime, so the
  result is a visible snap: other Kirby up to 330 px, own up to 40 px. Then
  it starts drifting again.
- This drift-then-snap every 2-3 s is the rubberbanding. "Back to several
  seconds before" describes it well: the snap undoes up to 2-3 s of
  predicted ("repeat last") motion of the other player.

**3. Detection and repair are slow compared with how fast a desync
re-forms.**

Time from a divergence to its repair:

| step | cost |
|---|---|
| next checkpoint | ≤ 0.5 s |
| the room waits for every report | up to 1.5 s |
| `minSyncGapMs` between syncs | 2 s |
| transfer and apply (`syncWall`) | 0.25-0.4 s |

While causes 1 and 2 hold, the client re-diverges right after each sync. The
sync machinery works as built: it repairs and re-repairs. It cannot fix a
client that drops inputs continuously.

**4. The referee hash is weak.** It covers `gRngVal` plus the four Kirbys'
x and y, and nothing else: enemies, items, HP, ability, room objects. In the
no-sync LAN run the image differences grew while simHash stayed equal for
120 s.

- A divergence that never touches RNG or Kirby positions is never detected,
  except by the 10 s periodic sync.
- One that does touch them is detected late.
- It did **not** cause false mismatches between honest peers in any run.
  Unmasked census divergences alone did not make honest peers' referee
  hashes disagree (0/238 at LAN, 0/118 at 30/30 and 60/60).
- Switching to the whole-image hash before the census converges *would*
  cause false mismatches: 12-21 pages always differ. Every checkpoint would
  then sync.

**5. Every sync, even of an honest peer, overwrites per-peer state.**

- Honest peers always differ in 12-21 pages of the snapshot: VRAM, palettes,
  OAM, the BG layer and streaming state of the local viewport, sound engine
  state, and the per-viewport and HUD bytes in §6.1.
- The page diff therefore always ships those pages, about 16 KB per periodic
  sync, and the target takes the donor's picture-side state.
- The target's camera focus is re-pinned every frame (`GAME_REPIN_FOCUS`),
  but its VRAM, palettes and sound state become the donor's until the game
  overwrites them.
- Expect a visual and audio glitch on every sync, including the periodic one
  every 10 s when nothing is wrong. Measured: v1, v2 and v5 synced 1-5 times
  a minute on clean links, 12-21 pages each.

**6. Smaller items.**

- **Time sync untuned.** In the node harness each client fast-forwards
  ~2.2 times a second, skipping 3-4 frames each time. Every one was the clock
  branch (`now < target - 3`), not the frontier branch. The far client is
  held ~5 times a second, near the 6/s cap.
  - Idle-input runs show **0** fast-forwards, so most of this is probably
    the node vsync shim not repaying the time a rollback frame takes. The
    browser pacer repays up to 8 periods.
  - Unverified in a browser. If it is real there, it is a stutter, not a
    rubberband.
- **30 Hz client: RNG divergence with identical inputs (w1).** 6 referee
  mismatches in 60 s, all `rng` only. Both engines' timelines are equal to
  the sealed stream. This is a real determinism bug somewhere on the
  fast-forward / catch-up path; that client fast-forwards 432 times a minute.
  Not localised.
- **Frozen client (w2, w3).** A 5 s freeze (hidden tab, phone lock, GC)
  recovers by jumping forward to the donor's checkpoint: own Kirby 37-140 px.
  The lateDrops counted there are frames skipped by the jump. They are
  harmless, but they inflate the counter.
- **Renderer not told.** The renderer is never told about corrections on
  this path. `PortRlNoteCorrection` is only called by netsim, so the GPU
  renderer's interpolation can blend across a snap. Nothing in the page
  listens to `katam-net-correction`.
- **Late events.** An event (seat, slot, view) that arrives late is applied
  one frame late locally (`schedule()`), which is a guaranteed divergence
  until the next sync.
- **R8, global KEYINPUT latch.** In rollback.c each peer's latch carries its
  own column (`PortRbApplyKeys(self)`). netsim fixes this with the menu
  owner; rollback.c does not. A run tapping START every 10 s (t_e) showed no
  referee mismatch in 45 s, but the hash would not see a menu divergence.
- **simVersion.** The `simVersion` gate includes the wasm hash
  (`dev.w<sha1>` or the stamp), so any rebuild splits rooms.

### 6.3 What is not the cause

- The sequencer, excusal and deadline logic behave as designed. Own-input
  overrides (the late-sender rule) were 0-0.9% of frames.
- The sync transfer itself is fine: 0 sync failures in every run, 2-375 ms
  from request to apply.
- Catch-up re-simulation completes, and it re-runs the target's own inputs.
- The room's sealed streams were byte-identical at both clients
  (`missingSealed` 0).

### 6.4 Robustness notes (no current symptom)

- FRAMES deltas have no sequence numbers or checksum (§3.5, and the harness
  pitfall above).
- `net_session.js` on an undecodable message resets the decoder and logs
  "asking for the archive again", but it never sends `archive`.

### 6.5 Known open issues reported by the builders (verified or not)

| issue | status here |
|---|---|
| web uses the PortRb stopgaps, not PortNet* | verified (§3.4) |
| referee hash = RNG + Kirby positions | verified (cause 4) |
| census not converged | verified: 38 unmasked blocks, boot_to_level |
| background syncs overwrite per-player state | verified (cause 5) |
| R8 menus diverge | not reproduced; hash blind to it |
| simVersion includes the wasm id | verified (`lobby.js simVersion`) |
| lateDrops on 120 ms links | verified, and it is the main cause (cause 1) |
| time sync untuned | partly; see cause 6 |
| native client not built | true (NETPLAY-V2 §12.7 plan) |
| WebRTC not built | true (`signal` is relayed, nothing uses it) |
| non-hibernating DO billing | true by design (§4.3) |
| archive storage limits | Read: `exportState` puts the whole archive in one storage value; the SQLite DO limit is 2 MB per value; at ~2 change records per frame-change this lasts hours, not days; unmeasured |

---

## 7. What to do next

### 7.1 If v2.1 is to be rescued: fixes in order

| # | fix | where | effort | expected effect |
|---|---|---|---|---|
| A1 | **Bound the lead.** Never simulate more than `ring - 6` frames past the sealed frontier. Hold frames (rate dilation, or a short stall) instead of predicting further. Or raise the ring: `PORT_RB_MAX_FRAMES` 32 → 96 costs 38 MB of ring at 403 KiB a slot, too much for phones. Better: snapshot every 4th frame and re-simulate from the nearest one (a 128-frame window in 32 slots). | `net_game.js tick()`, `rollback.c` | 1-3 days | removes cause 1 up to ~350-500 ms RTT |
| A2 | **Decouple checkpoints from the ring.** Copy the frame-start state of every checkpoint frame C when it is simulated. Re-take it if a rollback re-simulates C. Hash it once C ≤ frontier, however far ahead `now` is. | `rollback.c` (a checkpoint slot), `net_game.js checkpoints()` | 1 day | removes cause 2 (lag syncs) |
| A3 | Mask per-peer state out of sync blobs: keep the target's own VRAM, PLTT, OAM, IO, sound engine and the masked ranges of `tests/census/masks.txt` when applying. Turn periodic syncs off, or make them hash-only. | `net_game.js applyBlob`, room param `syncEveryMs: 0` | 1 day | removes cause 5 |
| A4 | Stronger referee: a masked image hash (census masks), or at least add object positions and HP. Needs the census to converge first, or false mismatches will sync every checkpoint. | `census.c` work, then `simHash` | 1-3 weeks (census is open-ended: 38-191 blocks to classify) | detects cause-4 divergences |
| A5 | Find the 30 Hz `rng` divergence (cause 6): run two instances, one fast-forwarding every other frame, and bisect with `--hash-log` per frame. | `rollback.c` / `main.c` catch-up path | 1-3 days | |
| A6 | Move to netsim's portable image **and add rollback to it**: keep N frame-top images or deltas, and on a contradicting `PortNetSealFrame` load the image at that frame and re-run. Without rollback, every misprediction becomes a desync. | `netsim.c`, `net_game.js` | 1 week | cross-build / native rooms |
| A7 | Sequence numbers on FRAMES; resend-from-archive on a gap. | `wire.mjs`, `net_session.js` | ½ day | prerequisite for WebRTC |
| A8 | Wire the correction to the renderer (`PortRlNoteCorrection` from `PortRbApplySnapshotAt` and rollbacks), and blend in the page. | `rollback.c`, shell | ½ day | cosmetic |

A1+A2+A3 would most likely make the reported symptoms go away on ordinary
links. Rollback would still be the correction mechanism, and syncs would
become the exception path again, which is NETPLAY-V2's original §5. That is
roughly a week. The project would still carry an unconverged determinism
story (A4, A5), and every new divergence becomes a 1-3 s snap.

### 7.2 Alternative architectures

| option | how | pros | cons | effort |
|---|---|---|---|---|
| 1. Classic rollback, finished (GGPO) | Peers exchange inputs. Bounded prediction window (A1). Frame-advantage time sync: the leader slows down, nobody stalls inside the window. Stall only past the window. Desync = bug; detection by masked hash; syncs only for join or recovery. | the best feel at low RTT; no server sim; the most code already exists (v1 Path B is this with the stall gate) | needs full determinism: the census converged (A4), R1/R2 cross-compiler RNG for mixed builds, the 30 Hz bug (A5); a stall when anyone's RTT exceeds the window | 3-6 weeks, open-ended because of the census |
| **2. Host-authoritative state streaming (recommended)** | One player's browser (the host) runs the only authoritative sim from the sealed input stream; the DO stays sequencer and relay. Every other client sends inputs and receives netsim deltas (`PortNetDeltaSave`, 0.9-4.7 KB per 1 s delta; at 10-20 Hz expect roughly 5-20 KB/s). It applies them with `PortNetSnapLoad` and renders. Later: predict the local Kirby only and reconcile it (needs local re-sim, i.e. rollback, for one Kirby). | **no desync by construction**; determinism and the census leave the critical path; netsim's portable, cross-platform image, deltas and correction record already exist and are tested (xplat); joins are a full blob (57-67 KB) | input latency = RTT to the host (+1 frame) for non-hosts until local prediction lands; host upload ~10-20 KB/s per peer; the host's device must keep 60 Hz; host migration needs a state handoff (the last blob) | 1-2 weeks for v1 without prediction; +1-2 weeks for own-Kirby prediction |
| 3. Server-authoritative in a Worker or container | Same as option 2, with the sim in the DO (`netplay/world/` spike) | one neutral authority | the ROM must not be uploaded (NETPLAY-V2 §11), so it needs a ROM-free sim pack (M8/M9); DO 128 MB limit; wasm in workerd needs a host shim | months (blocked on the content pipeline) |
| 4. Video streaming (docs/SERVER-RENDER.md, `framecodec.mjs`) | the host streams frames, clients stream inputs | trivially consistent | bandwidth, latency, quality | exists as a spike; product-wise a different feature |

**Recommendation: option 2.** The user's complaint is consistency. Option 2
makes consistency structural rather than something earned through
determinism, and its building blocks were measured in this tree (NETPLAY-V2
§12.6). Keep the room, the codes, the lobby, the sequencer and the archive
as they are. What changes is the client's job (apply state instead of
simulating others) and one new message kind (a delta stream from the host).
Add rollback-based own-Kirby prediction only if the input latency proves
objectionable at the RTTs that matter.

---

## 8. Appendix: reproducing the §6 runs

```
S=<scratch>; R="node tools/netplay_diag.mjs build/katam-node.js $ROM --seconds 60"
$R --link-a 30 --link-b 30 --jitter 15 --json $S/v1.json
$R --link-b 60  --json $S/v2.json
$R --link-b 90  --json $S/v3.json
$R --link-b 120 --json $S/s3.json
$R --link-b 120 --jitter 40 --json $S/v4.json
$R --link-a 60 --link-b 60 --jitter 20 --json $S/v5.json
$R --link-b 120 --nosync --json $S/v6.json
$R --seconds 120 --nosync --json $S/s5.json
$R --every-b 2 --json $S/w1.json
$R --stall-b 20:5 --json $S/w2.json
# against wrangler dev (sbg start wdev -- npx wrangler dev --port 8788 --ip 127.0.0.1, in netplay/)
$R --seconds 45 --link-b 120 --url http://127.0.0.1:8788 --json $S/y1.json
```

I ran up to six in parallel on a 32-core machine. Timing-sensitive results
(lateDrops) get worse under CPU contention, as the failed e2e run shows, so
rerun alone before quoting a borderline number. `--stall-b T:S` counts T from
process start; the game starts ~1-2 s later.
