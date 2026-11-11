# Online co-op: checkpoint

**Status (2026-11-10): stages 1-4 are implemented on branch `coop` of the
zalo/katam-port fork (on top of sh1ftmaker's stage-1 checkpoint `1fdaa04`),
published at https://zalo.github.io/katam-port/mp/.**  Stage 1 (puppet
Kirbys) is sh1ftmaker's; stages 2-4 and the stage-1 follow-ups are new and
described in §8 below.  §1-§7 are the stage-1 checkpoint as it was written,
kept for its detail; where §8 contradicts them, §8 is current.

Read this file first, then `platform/port/coop.h` (the pose layout and the C
API) and the header comments of `web/coop.js`.

Conventions: **Run** means I ran it for this checkpoint's work and saw the
result. **Reported** means a subagent reported it and I did not repeat it.
**Read** means taken from the code without running it.

---

## 1. What this is, and what it replaces

Every player runs their own complete game. Nothing is shared except small
messages: in stage 1, each player's own Kirby pose. Other players are drawn
as *puppets* in the three CPU Kirby slots. There is no shared input
timeline, no rollback and no world snapshot, so a player's own Kirby is
never corrected by the network.

The model is copied from Ship of Harkinian's Anchor co-op as implemented in
the web fork at `/bigdata/agent-tom/projects/soh-web` (see its README
"Multiplayer", `soh/soh/Network/Anchor/ActorSync.{h,cpp}`,
`WorldSync.h`). The owner pointed at that project as the model.

It replaces the lockstep/rollback route described in
`docs/NETPLAY-HANDOFF.md`, which was shelved for desyncs and rubberbanding.
That code (`platform/rollback.c`, `platform/netsim.c`, `web/net_game.js`,
`web/lobby.js`, `?room=` / `?online=1` / `?world=`) is untouched and unused
by co-op. Co-op refuses to start while rollback, netplay or seats are
active.

## 2. Where things are

| | |
|---|---|
| Branch | `coop`, pushed to remote `github` (sh1ftmaker/katam-port), one commit on top of `main` 1249ebf |
| Working copy | `/bigdata/agent-tom/projects/katamport/coop-wt` (a git worktree of `port/`) |
| Live page | https://sh1ftmaker.github.io/katam-port/mp/ — build label `c69cacf` |
| Publish | from the worktree: `source ~/emsdk/emsdk_env.sh; PAGES_REMOTE=github PAGES_SUBDIR=mp scripts/publish-pages.sh` (fast-forward only) |
| Server | the existing `katam-netplay` Cloudflare worker (`netplay/`). **Not redeployed**: the live worker has no pose relay, so live clients use the fallback path (§4) |

How a player uses it: open `/mp/` (a plain visit creates a room and the
address becomes `/mp/?coop=CODE`), send that address to a friend. Each
player loads their own ROM and keeps their own save. On any other page of
the build, `?coop=1` creates and `?coop=CODE` joins.

## 3. The pieces

### Engine (C)

- `platform/coop.c`, `platform/port/coop.h`: the puppet layer.
- `game/src/kirby.c`: one early branch at the top of the Kirby task
  (`sub_0803EE18`). A puppet slot copies in its pose and runs only the draw
  half (`sub_0803F46C`); input, state machine, physics, tile collision and
  door checks are skipped. With co-op off the branch tests a byte that is
  always 0.
- Taking a slot: its LevelInfo is made inactive
  (`FillLevelInfo(slot, 0xFFFF, …)`), which idles its AI and stops its room
  being simulated; its ability is held at NORMAL for 4 frames so the CPU's
  ability helper objects destroy themselves.
- Giving a slot back: the CPU Kirby respawns beside the local player through
  the respawn route (`sub_08055920`). `PortWarp` cannot be used for this.
- Side effects removed (Reported): puppets carry flags
  `0x200|0x100|0x800|0x2000000` (no damage, pickups, doors); enemy targeting
  skips them (`FindTargetKirby`, `sub_0803D46C`, `sub_0803D5CC`,
  `sub_0803E324`); scroll-lock triggers skip them
  (`game/src/scroll_lock.c`); the pause area map and HUD handle a slot with
  no room (`pause_area_map.c`, `hud.c`). A puppet's roomId is never 0xFFFF.
- Frame hook: `platform/main.c` (`PortCoopFrameTop` beside
  `PortNetFrameTop`).

API (all in `PORT_EXPORTS`): `PortCoopPoseSize`, `PortCoopEnable`,
`PortCoopReady`, `PortCoopSetPuppet(slot,on)`, `PortCoopSetPose(slot,ptr)`,
`PortCoopSetAbsent(slot)`, `PortCoopLocalPose(out)`, `PortCoopPoseBuffer`,
and the test-only `PortCoopSelfTest`.

Pose: 36 bytes, little-endian, no pointers. `i32 x` @0, `i32 y` @4,
`u16 room` @8, `u8 flags` @10 (bit0 hidden), then version, the resolved
sprite ids and variants for body, hat and overlay, animation index, ability,
colour (carried, not applied), animation speed and frame, hp, lives. The
authoritative layout is in `coop.h`. JavaScript treats everything after
offset 10 as opaque.

### Server (`netplay/`)

- `wire.mjs`, `session.mjs`: `POSE_UP` (0x40) from a member is relayed as
  `POSE_DOWN` (0x41, prefixed with the sender's member id) to every other
  connected member. The room checks sizes only. Wrong sizes and anything
  over 40 messages/s per member are dropped, not fatal.
- `welcome.features` contains `'pose'`.
- A room whose sim id starts with `coop/` ignores `start`, so it stays in
  the lobby phase; the existing version check keeps co-op and lockstep
  clients out of each other's rooms. Room codes, the directory, resume and
  host handover are unchanged.

Wire format: `u8 0x40, u8 flags (bit0 PAUSED, bit1 KEY), u8 n (1..4),
u8 size, then n × {u16 seq, pose}`. `seq` is the sender's clock in
59.7275 Hz frames, wrapping.

### Client (`web/`)

- `net_session.js`: `sendPose()`, a `'pose'` event, an opt-in quiet-socket
  check (8 s) and jittered backoff, `reconnectNow()`, `setPaused()`. The
  lockstep lobby keeps its old behaviour.
- `coop.js`: the top half is logic (`window.KatamCoop`, also loadable from
  node), the bottom half is page glue.
  - Sends: one pose sampled per presented frame, sent every second frame
    with two poses (about 30 messages/s), 4/s when standing still, nothing
    when alone.
  - Receives: a jitter buffer per remote player. Playback delay 3–12 frames,
    x/y interpolated within a room, snap on room change or a 48 px jump, up
    to 6 frames of extrapolation, absent after 1.5 s without data, jump to
    newest on resume.
  - Slots: other players take puppet slots 1–3 in seat order. More than
    three others are not drawn; the chip says how many.
  - A hidden tab sends one PAUSED message and then nothing. Web Locks give a
    duplicated tab its own identity.
  - A status chip (top left): room code, players, invite link.

## 4. The fallback path, and the worker redeploy

The deployed worker closes a connection on any binary type it does not
know. So the client sends binary poses only when `welcome.features` lists
`'pose'`; otherwise it sends each pose as an existing JSON
`signal {to, data:{pose: base64}}`, one per other member. That is what the
live page does today (**Run**: two headless windows on the live page, each
in its own browser profile, `features: []`, each drew the other's puppet).

To switch the live site to the binary relay: `cd netplay && npx wrangler
deploy` from this branch. The worker is live and shared with the old
lockstep rooms; it is the owner's to deploy. The change is additive (the
30 older netplay tests pass), but it has not been run on Cloudflare.

## 5. Tests and what they showed

| test | how | result |
|---|---|---|
| Engine self-test, no network | `tests/coop_selftest.sh` (native, or `--wasm <katam-node.js>`) | **Reported**: 0 failures over about 5,576 checks and 7,300 frames on both builds; the local player's per-frame trajectory and state hash are identical with and without a puppet |
| Two instances, real engine | `make BUILD=build/eng coop-test` (needs `netplay/node_modules` and the ROM) | **Run**: 12/12, six runs in a row. At 120 ms + 40 ms jitter the puppet trails by about 12 frames, p95 error under 1 px; survives a 5 s freeze each way, a clean reconnect (about 0.8 s) and a silent drop (about 9 s) |
| Server and client units | `cd netplay && npm test` | **Run**: 51/51 (30 older + 21 new) |
| Same timeline, fake engine | `node tools/coop_test.mjs --fake-engine` | **Reported**: 12/12 |
| Browser, local relay | two headless Chromium windows | **Run**: both showed 2 players, puppet drawn and moved, no exceptions |
| Browser, live page | as above against `/mp/` | **Run**: same, over the fallback path |
| Regression gates with co-op off | `make regress`, `make regress-wasm`, layout-neutral gate | **Reported**: wasm 7/7 OK; native shows sim-column differences in `aim`, `play_rooms`, `warp_sweep`, `wide_view`, all explained as layout, and unmodified `main` built in this worktree shows the same kind of difference. Goldens were not regenerated |

Harness notes that cost time:

- Give each browser window its own profile (`Target.createBrowserContext`).
  In one profile the second window finds the ROM in IndexedDB and boots
  before a test can call `PortPlaytestBoot`.
- Open each tab in its own window (`newWindow: true`), or the background
  tab is hidden and reports itself as away.
- The playtest warp is queued at frame 1300 but lands later. Wait for the
  `warp: player 0` log line before measuring.

## 6. Known gaps in stage 1

- Not tried by a person, and not on a phone.
- Colours differ between views: everyone is pink in their own game, and a
  remote player takes the colour of the slot they land in. The pose carries
  the sender's colour but it is not applied.
- At most three other players are drawn.
- Animation is not re-seeked to the sender's frame; it restarts in step
  when the sprite changes.
- Remote ability hats were only tested with NORMAL-state sprite ids.
- Bosses that write into every Kirby (Dark Meta Knight, Mega Titan) still
  write into puppets; this affects only how the puppet looks. Cutscenes,
  sub-games, the warp star and the phone call with all three slots taken
  were not traced.
- `PortCoopReady` is already 1 during the opening sequence, so puppets are
  taken before the first room.
- A reconnect in the lobby phase can land on a different seat. Puppet slots
  are keyed by member id, so only the seat number changes.
- Clients with different room sets (mods, editor rooms) only get "that room
  does not exist here, show as absent".
- The four native goldens above need re-checking in `port/` on merge.

## 7. What comes next

Agreed in outline with the owner; none of it is started.

1. **Stage 2, shared progress.** Mirror shards, switches, chests, the map,
   opened doors, sent as events. Reference: soh-web `WorldSync.h` (progress
   is shared, consumables stay per player; an item carries the flag that
   produced it so two players opening one chest at once do not duplicate
   it). Needs a reliable event message beside the pose stream; the pose
   relay drops under load by design.
2. **Stage 3, enemies.** Zelda uses one owner per room. KATAM spawns
   enemies as the camera approaches and drops them off-screen, so a room
   owner would not have the enemies near a distant player loaded: ownership
   should be per enemy, by whoever is nearest or spawned it, with damage and
   kills sent as events. Enemies would be matched by room and spawn-table
   index (`CreateObjTemplateAndObj`, `spawnTable` in
   `game/include/object.h`); that this gives a stable index is **not
   confirmed**.
3. **Stage 4, bosses**, one at a time, as state-machine field tables
   (soh-web `ActorSync.cpp`, "State-synced actors").

Smaller items that would improve stage 1: apply the sender's colour,
sender-frame animation seek, a name field in the chip, and the worker
redeploy.

Decisions still open: whether CPU Kirbys in unused slots stay (they do
today), and what to do beyond four players (extra puppets would have to be
drawn outside `gKirbys`).

---

## 8. Stages 2-4 and the stage-1 follow-ups (2026-11-10)

Everything below travels over the room's existing JSON `signal` relay
(`net.sendSignal`, `web/net_session.js`): reliable, in order, and present on
the deployed worker, so **no worker redeploy is needed** for any of it.  The
room's sim id is now `coop/2`, so these pages never share a room with
stage-1 pages.  All engine halves are 0 / one branch when co-op is off
(`make regress-wasm` 7/7 on this branch).

### Stage 2: shared progress (`platform/coop_world.c`)

What is shared is exactly the game's own world save, `gWorldProps`
(`game/src/save.c`): the save-scope despawn bits, the world state slots (big
switches, special doors, hub mirrors), `gVisitedDoors` (the area map's doors
and chests), `gTreasures` (chests, shards, spray paints, sound player, area
maps, vitality), the shadow Kirby and Mirra encounter bits, and the story
stage.  Consumables, lives and abilities stay per player (soh-web's rule).

- Merge rules: every flag by OR (every writer in `game/src` only sets bits or
  stores 1, so merging is idempotent and order-free); the vitality count by
  the chest that produced it (a delta record carries the chest bit and the
  increment, and the increment applies only if the bit is new here; a full
  record counts the vitality chests found in the ROM's object lists -- 4:
  treasure indices 46, 63, 69, 83); the story stage by maximum, only once
  both sides are past the tutorial (>= 300).
- The engine keeps a shadow of the image; `PortCoopWorldTake` returns a
  DELTA record of new bits each frame, or a FULL record when tracking
  (re)starts (a file loaded, back from the title) -- the page sends that to
  everyone, and sends a full record to anyone who joins or reconnects.
  `PortCoopWorldApply` merges, writes the save (`UpdateSaveBufferByOffset`,
  as opening a chest does), opens any chest already on screen, and updates
  max HP for a new heart; records that arrive on the title screen, in the
  tutorial or during a load are queued (32) and applied at the next ready
  frame.  Record format: `platform/port/coop.h`.
- The page shows a notice ("Zed found a Mirror Shard (3/8)") and has a
  **Share progress** switch (localStorage `katamCoopShare`).

### Stage 3: shared enemies (`platform/coop_enemy.c`, hook in `object.c`)

- One owner per room: the lowest member id among the players the page sees
  in the room (from the played-back poses).  Two pages that briefly
  disagree both run their own enemies -- what happens without co-op.
- The owner streams `PortCoopEnemyCollect` (room's live regular enemies,
  types 0x00-0x37: index, x, y, hp, animation index `unk83`, facing) about
  30 times a second to the others in the room; they play it back a few
  frames behind with interpolation (`EnemyBuffer`, `web/coop.js`) into
  `PortCoopEnemySetPoses`.  Their copy of each enemy becomes a puppet while
  the stream is fresh (4 frames): `ObjectMain` skips state, gravity,
  movement and terrain, places it and draws it (sprite from `unk83` through
  the normal draw), and still registers it for collisions -- it can be hit
  and it hurts on contact.
- Events (8-byte records, `PortCoopEnemyTake` -> everyone ->
  `PortCoopEnemyEvent`): a kill anywhere kills the same enemy everywhere as
  if defeated (`unk80 = 0; unkC |= 0x2000`, dark_mind.c's way: poof, no
  item, score credited to the remote slot); a hit on a puppet goes to the
  owner, who takes the hp off its copy; the hitter's copy plays its own
  knockback for 24 frames before going back on the owner's pose.
- Matching is by room and the index of the object's record in the room's
  object list (`platform/coop_objects.c`: the ROM's list, not rando's copy).
  Run-time objects (spawner children, projectiles) and enemies outside the
  owner's camera stay local.

### Stage 4: minibosses and bosses (same file, `PortCoopObjectDamage` in `object.c`)

Not state-synced: each player fights their own copy, and the copies share
one health pool.  hp a copy loses is sent as a boss hit; every other copy
takes it through the game's own reaction -- `sub_0809AF38` is split after
the damage into `sub_0809AF38_React`, so a remote hit gets the flash, the
shake and, at 0 hp, the game's own death (explosion, shard, music).  A kill
is sent too.  Every touched boss's hp is broadcast once a second and taken
by a copy nobody here has touched (a player who walks in late).  Matching:
room + object-list index, or room + type for bosses spawned at run time.

### Stage-1 follow-ups

- **Colours**: a puppet wears the sender's colour (pose byte 24), and the
  local Kirby takes its seat colour (pink, yellow, red, green, then the
  spray paint colours) unless the player chose a spray paint, so everyone is
  the same colour in every game; the HUD palette follows
  (`PortHudReloadKirbyPalette`).  `PortCoopSetLocalColor(-1)` restores.
- **Animation seek**: a puppet's new animation starts at the sender's frame
  (`sub_0815521C`, the seek hud.c uses).
- **Tutorial**: puppet slots are taken only past the tutorial
  (`gAIKirbyState >= 300`): the opening and tutorial script the CPU Kirbys.
  The self-test arms itself regardless.
- **Chip**: a name field (renames at once), Share progress and Share
  enemies and bosses switches, notices under the chip.

### Tests (all run for this checkpoint)

| test | result |
|---|---|
| `make coop-test` (two real instances, dev relay; phases follow, freezeA, freezeB, reconnect, drop, enemies, bosses, progress; the progress phase adds a third instance) | 35/35 |
| `tests/coop_selftest.sh --wasm build/katam-node.js` | 0 failures in all three runs; local trajectory identical with and without a puppet |
| `cd netplay && npm test` | 51/51 |
| `make regress-wasm` | 7/7 OK |
| `make lint-simstate` (LP64 build in `build/lp64`) | 0 unlisted |
| Browser: two headless windows in separate profiles on the dev relay (`?coop=1&coopRelay=...`) | colours match across views, enemies in the same places, a rename and a found shard reach the other window |

The harness moves both playtest files past the tutorial (writes
`gAIKirbyState = 300`) right after the warp; progress and boss tests write
the game's memory as the game's own code would (a shard bit, an enemy's hp)
rather than playing the game.  Not tried by two people, and not on a phone.

### Known gaps

- Enemy projectiles and spawner children are local: a puppeted enemy does not
  shoot in a non-owner's game.  Enemies the owner has not spawned are run
  locally by whoever has them.
- Minibosses and bosses act independently in each game; only their health
  and death are shared.  Bosses that write into every Kirby still write into
  puppets (looks only).
- At most three other players are drawn.  CPU Kirbys stay in unused slots.
- Chests opened by someone else open on screen; big switches and doors show
  their new state on the next room load.
- A player in the tutorial sees no puppets and merges progress only once
  past it.

### Next

Projectile sync for puppeted enemies; extra puppets drawn outside `gKirbys`
for more than four players; state-synced bosses where a shared health pool
is not enough (Dark Mind's mirror phases, Mega Titan's arms); a real
two-person and phone test.
