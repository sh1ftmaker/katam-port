# Online co-op: checkpoint

**Status (2026-11-10): stages 1-4 are implemented on branch `coop` of the
zalo/katam-port fork (on top of sh1ftmaker's stage-1 checkpoint `1fdaa04`),
published at https://zalo.github.io/katam-port/mp/.**  Stage 1 (puppet
Kirbys) is sh1ftmaker's; stages 2-4 and the stage-1 follow-ups are new and
described in §8 below; the Kirby colour picker and name tags in §9; shared CPU
Kirbys, spawned objects (attacks, shots, particles) and the phone in §10;
enemies after any player, shots that hurt everyone and other games' sounds
in §11.  §1-§7 are the stage-1 checkpoint as it was written,
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

---

## 9. Kirby colour picker and name tags (2026-11-11)

### Colour picker (single player and co-op): `platform/kirby_color.c`

- **Setting**: `gPortSettings.kirbyColor` (-1 the game's, 0..13 one of the
  game's fourteen, 14 custom) with `kirbyBody` / `kirbyFeet` (0xRRGGBB);
  keys `kirbycolor`, `kirbybody`, `kirbyfeet`.  The one setting that reaches
  game memory, so it is applied only in the one-player adventure and never
  under rollback, netplay or seats (those hash the game's memory).
- **Menu**: a new **Kirby** page (`platform/ui/ui_menu.cpp`, on the web and
  native) with a live preview of the standing Kirby (decoded from the sprite
  tables and drawn pixel by pixel, because the menu pauses the game), the
  fourteen as two-tone swatches (found in the game or not), and Custom: a
  hue wheel for the body and one for the feet and cheeks.  The co-op chip
  has a Colour button that opens this page (`PortUiOpenPageJs(1)`).
- **Whose colour wins**: the setting; else the spray paint picked in the
  game; else (co-op) the seat colour; else pink.  It lives in
  `gUnk_0203AD1C[local]` (read by every Kirby rebuild) and `kirby->color`;
  Kirby init asks `PortKirbyColorInit` so a new level does not flash the
  old colour.  Nothing is written to the save.  Co-op's old `LocalColor`
  moved here; co-op only says what the seat colour is
  (`PortKirbyColorSetSeat`).
- **Custom colours** are built from a model fitted at load time to the
  game's own fourteen palettes (OKLab; per entry L = a + b * L_base,
  C = c * C_base, h = h_base + d; base = entry 3, the body fill, and entry
  9, the most-drawn feet shade).  Rebuilding each of the fourteen from its
  two base colours alone: mean OKLab error about 0.04 (a plain hue shift of
  pink: 0.047).  The HUD's palette (BG 15) is fitted the same way.  No ROM
  data is in the source: the coefficients are fitted from the ROM in hand.
- A custom Kirby's colour index stays the **nearest** of the fourteen (body
  distance + 0.1 x feet), so every table indexed by colour (UFO palette,
  link-up screen) stays in range.  Only the Kirby's own palette and the
  HUD's are replaced: `PortKirbyColorFixup` after each of the six anim-0x15
  loads in `kirby.c`, `PortKirbyHudFixup` after the six HUD loads in
  `hud.c`.  `PortHudReloadKirbyPalette` now asks for a BG palette sync (the
  game only loads that palette on the way into a level).
- **On the wire**: the pose's reserved word.  Bytes 32..33 = body BGR555 |
  0x8000, 34..35 = feet; byte 24 keeps the nearest built-in, so an older
  build shows something close.  No new message; a puppet's palette is built
  by the receiver from the same model (`PortKirbyColorSetSlot`).

### Name tags: `platform/ui/coop_tags.c`, `web/coop.js`

- The page draws them as HTML over the canvas (sharp at any size, any
  script); `PortCoopTags(out, max)` says where: for each remote Kirby drawn
  in the local room, the top of its sprites as a fraction of the canvas,
  taken from the frame actually presented (`PortRlPresentedSprite`, so the
  widened view, zoom, sub-pixel camera and interpolation are included), plus
  the picture's rectangle.  A remote Kirby in the room but off the view gets
  a position worked out from the local Kirby's and is pinned to the edge
  with an arrow.  Nothing over the pause screens or the menu.
- A tag is the player's name with a bar in their Kirby's colour; overlapping
  tags are lifted apart.  The chip lists everyone with a colour dot and has
  a **Show names** switch (`localStorage.katamCoopNames`).

### Room for more than four players

Both pieces are keyed so that players drawn outside `gKirbys` can join in
without changing the interfaces:

- Tags use **draw ids**, not slots: ids 1..3 are the puppet slots; ids from
  `PORT_COOP_TAG_EXTRA` (16) are reserved for extra players.  coop.js maps an
  id to a member in `midOfDrawId` / `bytesOfDrawId`.
- `PortKirbyColorBuild(choice, body, feet, out16)` builds a full palette for
  any colour without a Kirby slot, for extra players drawn by the renderer
  into palette banks of their own.
- `PortRlPresentedSprite(owner, also, ...)` finds any struct Sprite in the
  presented frame, so a renderer-drawn extra player gets an exact tag the
  same way.

### Tests

| test | result |
|---|---|
| `make coop-test` (new phase `colors`: A's custom colour on A, B draws A's puppet in exactly the same palette, B's page knows the colour, B's tag for A is on the picture in A's colour; then a built-in colour; then back to the seat colour) | 40/40 (the 35 before, 5 new) |
| `tests/coop_selftest.sh` (wasm and native LP64) | 0 failures in all three runs on both; local trajectory identical |
| `cd netplay && npm test` | 51/51 |
| `make regress-wasm` | 7/7 OK (the game unchanged while nothing is chosen) |
| `make lint-simstate` | 0 unlisted -- now also covering `coop_world`, `coop_enemy`, `coop_objects` and `kirby_color`, which were missing from `SIM_PLATFORM` |
| Browser, two windows on the dev relay | the menu's Kirby page, a custom colour on one player shown on the other's puppet, name tags following the puppets, the chip's colour dots and Colour button |
| Browser, single player | a swatch recolours Kirby and the HUD; the choice is saved |

### Gaps

- The model is fitted to the fourteen; colours far from all of them (very
  dark bodies) shade plausibly but were only checked by eye.
- The UFO ability and the link-up screen use the nearest built-in colour.
- Tags in the PPU renderer use an estimated head height (20 px) and no
  interpolation.

---

## 10. CPU Kirbys, spawned objects and the phone (2026-11-11)

The two things players noticed most: every game ran CPU Kirbys of its own
(each player saw different helpers), and nothing a puppet or a puppeted
enemy would have spawned existed in anyone else's game -- no beam, fire,
cutter, star spit, dust or enemy shot.

### CPU Kirbys: one game runs them (`platform/coop.c`, `web/coop.js`)

- **The runner** is the lowest member id among the players in a level and
  past their tutorial (pose byte 27: `PORT_COOP_F2_INLEVEL`, new
  `PORT_COOP_F2_TUTORIAL`).  A player stays eligible for `AUTH_GRACE`
  (5 s) after they last were, so a door, a warp or a level load does not
  move the helpers (each move respawns them); leaving the room, the title
  screen or the tutorial does.
- On the runner, the slots no player takes stay the game's own CPU Kirbys;
  their poses (`PortCoopSlotPose(slot)`, the pose of any non-puppet slot)
  ride with the runner's own pose as *extras* (below).
- Everyone else shows them like remote players: slot keys in coop.js are a
  member id (a player), `'c<mid>:<slot>'` (a CPU Kirby that member runs) or
  `'x<slot>'` (nobody).  Players get slots first; a CPU Kirby gives its slot
  up to a player.  A page that does not run the helpers fills every slot it
  has left with nobody, so it has no helpers of its own that the others
  cannot see.  CPU Kirbys get no name tag.
- The CPU AI only follows and reacts to "human" slots, which in co-op is the
  runner's own Kirby: the helpers follow the runner.

### The phone

A player whose game does not run the helpers calls them as usual: their game
reports the call (`PortCoopPhoneCall`, a hook where `sub_080572A0` starts it;
`PortCoopTakeCall`), the page sends `{call: [room, x, y]}` to the runner, and
the runner's game brings each of its CPU Kirbys that is not already in that
room to the caller's cell (`PortCoopAnswerCall`: the respawn route `PortWarp`
takes, which keeps the ability; one that is between rooms is sent as soon as
it can be, for up to 2 s; one that is down stays put).  The caller's game
sees them arrive as puppets and ends its call as the game would.  The
runner's own phone is the game's own.

### Spawned objects: ghosts (`platform/coop_fx.c`)

Visual replication at the sprite level: the game that runs a spawner reports
what its spawned objects *draw*; every other game draws copies ("ghosts")
through the game's own sprite code.  Nothing is simulated, so nothing can
diverge; a ghost has no collision and is gone the frame its source stops
reporting it.  Hits stay where they were: a player's attacks land in their
own game, so the others only need to see them.

- **Who spawned what**: `TaskCreate` (task.c) tags each new task with the
  origin of the task creating it -- a Kirby slot's task is that Kirby; a
  regular enemy from the room's object list running its own state (not a
  puppet) is that enemy; anything else passes its own origin on.  Bosses,
  spawner children and items start none (they exist in every game already).
- **What it drew**: `DisplaySprite` / `sub_081564D8` (sprite.c) hand every
  sprite of a tagged task to `PortCoopFxCapture`, except the Kirbys' own
  (puppets draw those): animation, variant, script cursor, world position,
  palette, flags, OAM layer -- 18 bytes (`PORT_COOP_FX_REC`, layout in
  `coop.h`), at most 32 a frame.
- **Drawing** happens at the top of the next frame (`PortCoopFxFrame` from
  `PortCoopFrameTop`), after every task: the script is walked to the cursor
  for the frame and its tiles; the tiles go into OBJ VRAM that is free *this
  frame* (allocated and freed again at once, so the game's own objects always
  get it first; the queue drains in order after the picture is composed) and
  join the game's own upload queue; `DisplaySprite` does the rest, so both
  renderers see an ordinary sprite.  Ghosts go in last and get what is left
  of the OAM.
- **Shared tiles**: many effects draw from one sheet loaded once per room
  into a block shared by every object of the kind (`sub_0803DE54`), from
  whichever variant the first object had.  A ghost of such a frame points at
  this game's own copy of the block when it holds one (`GameBlock`), else
  uploads only the tiles the frame's OAM reaches, starting the sprite before
  its upload when the first one is far in.  Ghosts showing the same tiles in
  one frame share one upload.  Budget: 8 KB of ghost uploads a frame, 12
  queue entries always left to the game.
- **Palettes**: 0..3 / 4..7 are a Kirby slot's body / hat -- the page remaps
  the sender's slot (its own Kirby, or a CPU Kirby it runs) to the puppet
  slot showing that Kirby; 8..14 are the game's per-room palette registry
  (`gUnk_02022E60`), sent as the animation and variant it holds (the variant
  is recorded by a hook in `sub_0803DFAC`) and looked up here, loaded only
  while fewer than 5 are in use; 14 / 15 are fixed.  Anything else is
  skipped rather than drawn in the wrong colours.
- **Enemies**: a ghost of a shared enemy's child is drawn only over this
  game's puppet of that enemy, never beside a copy that spawns its own.
- **Timing**: each frame's records ride with that frame's pose and are
  played back with it -- the effects line up with the puppet exactly.

### Extras: how both travel

Per pose message, an optional blob (`encodeExtras` in coop.js): `u8 1, u8 n`,
then per pose `u16 seq, u8 nCpu, nCpu x {u8 slot, pose}, u16 fxLen, fx`.  On
a room without the binary pose relay (the deployed worker) it goes inside the
pose's own `signal` (no extra messages); with the binary relay it is one
`signal` to everyone right after the pose, only when there is something to
send.  An idle player with no extras still drops to 4 messages a second.
The room's sim id is now `coop/3`: a `coop/2` page would run helpers of its
own that the others cannot see.

### Fixed on the way

- A file that went back into its tutorial gave its puppet slots back to the
  CPU and forgot they were wanted, so they were not taken over again until a
  level load; `GiveBack` now keeps `wanted`.

### Tests

| test | result |
|---|---|
| `make coop-test` (new phases `helpers`: A runs the CPU Kirbys and B shows A's two within 2 px of A's recent positions, in the same room, with no helpers of its own; A back in its tutorial hands them to B and takes them back after; a warp does not move them; B calls from another room and A's helpers arrive in both games -- and `fx`: 4 s of A attacking, B draws at least half as many ghosts as A captures: measured 1011 of 1054, nothing skipped) | 49/49 (the 40 before, 9 new) |
| `tests/coop_selftest.sh` (wasm and native LP64) | 0 failures in all three runs on both; local trajectory identical |
| `cd netplay && npm test` | 51/51 |
| `make regress-wasm` | 7/7 OK (the game unchanged with co-op off) |
| `make lint-simstate` (now also `coop_fx`) | 0 unlisted, 0 stale |
| Browser, two windows on the dev relay | A's Beam whip and a sword knight's slash (A's enemy) drawn on B in the right place and colours; B phoned from another room and A's Sword and Fire CPU Kirbys appeared next to B in both games |

`PortCoopTestAbility(ability)` gives the local Kirby an ability the way
swallowing does (tests and the browser harness only).  `PortCoopFxStat(i)`
holds counters (captured, drawn, skipped by reason) and the last few lookup
and palette failures (see the end of `coop_fx.c`).

### Gaps

- ~~Enemy shots do not hurt a player who is not the room's owner.~~ Fixed in
  §11.
- What the runner's CPU Kirbys spawn in a room the runner is not in is not
  seen by anyone (the game only draws objects in the local player's room).
- Rotated and scaled sprites are drawn unrotated; a ghost whose palette
  cannot be resolved here is skipped.
- The helpers follow the runner, not the player who called them, once their
  AI picks a new room (a minute or more).
- `coop/2` pages cannot join `coop/3` rooms.
- Not tried by two people, and not on a phone.

## 11. Enemies after any player, shots that hurt everyone, sounds (2026-11-11)

The three gaps that showed most in play: a room's enemies only ever went
after the player whose game runs them (the room's owner), their shots could
not hurt anyone else, and everything another game did was silent.

### Enemies go after any player (`platform/coop.c`, hooks in `kirby.c`)

The game's targeting (`FindTargetKirby`, and `sub_0803D5CC` by x only) only
*reads* the Kirby it picks -- position, facing, state; the only writes
through a target are two boss hands.  So for a regular enemy (types
0x00..0x37, the kind the games share) it now picks among the local Kirby and
the puppets standing in the enemy's room (`PortCoopFindTarget`): in the game
that runs the enemy it goes after the nearest player, and every other game
sees that through the shared pose.  The game's own rule is kept: players
first (its loop covers `gNumHumanPlayers` and takes a CPU Kirby only when no
player is in the room), then CPU Kirbys, a Kirby flagged `unkC & 0x8000` only
as a last resort.  A puppet counts while its pose is drawn in that room and
its hp is above 0; the page tells the engine which puppets are CPU Kirbys
(`PortCoopSetPuppetCpu`).  Bosses and minibosses keep going after the local
player only: every game fights its own copy.

Contact damage needed nothing new: a puppeted enemy is solid in the victim's
own game and stands where the owner has it.

### Shots hurt everyone (`platform/coop_fx.c`, hooks in `object_collision.c`)

- **Reported**: at the top of the collision task, the game that runs an
  enemy reports what that enemy spawned and put in this frame's collision
  lists to hit Kirbys (`PortCoopFxHazardScan`: flag 0x10000000, not 0x200,
  in the local room's group, origin `OR_ENEMY` via the task's tag): world
  position, both hitboxes (`unk38..3B` and the sprite's `unk20[0]`), facing,
  attack flags `unk68`, damage `unk63`, knockback `unk64/66` -- 24 bytes
  each, at most 10 a frame, after the sprite records in the same frame
  (`PORT_COOP_FX_HAZ_REC`, layout in `coop.h`).  Grabs (attack flag 0x20)
  hand the Kirby to the enemy's state, which does not run elsewhere, and are
  left out; so are enemy and boss bodies.
- **Applied**: right after the game's own collisions, each other game builds
  a stand-in `ObjectBase` per hazard (`sProxy`) and `PortCoopHazardTouch`
  (object_collision.c) tests it against the Kirbys that game runs with the
  attack-against-Kirby loop's own overlap test.  A touch that would hurt
  goes through the game's own reaction, `HandleKirbyCollision`: damage,
  knockback away from the shot, losing the ability, and the invincible
  frames that keep one shot from hitting twice.  A touch that would not hurt
  is left alone, so a Kirby's `unk6C` (also how doors and warps hand a Kirby
  its destination) only ever names a stand-in that hurt it.
- As with ghosts, a hazard applies only over this game's puppet of its enemy
  -- a copy that runs its own state fires its own shots.
- The shot is played back with the frame it belongs to, so it hurts where
  its ghost is drawn; in the owner's game the shot still passes through the
  puppet.

### Sounds (`platform/coop_fx.c`, hooks in `data.h` and `m4a.c`)

The game plays a sound for the local Kirby and for objects in the local room
(`PlaySfx`).  A puppet runs no state and a ghost is no object, so another
player's jumps, inhale and attacks, a helper's attacks and everything a
shared enemy does in its owner's game were silent everywhere else.

- **Reported** (`PortCoopFxSound`, from `PlaySfx`/`PlaySfxAlt`): the local
  Kirby's own sounds (a CPU Kirby is silent in the game itself), what its or
  a CPU Kirby's spawning plays, and what a shared enemy running its own state
  plays; and (`PortCoopFxSoundStop`, from `m4aSongNumStop`) the stops of the
  looping ones -- inhale, fire breath, the slide.  Songs on the music player
  are never sent (the invincibility tune is the sender's own).
- **Repeated**: each frame sent carries the sounds of the last 8 frames with
  the frame they were made in (8 bytes each, at most 16, after the hazards);
  the receiver plays each stamp once, so a frame the page skips or shows
  twice loses or doubles nothing.
- **Played** when the receiver is in that room and, for an enemy's, shows
  that enemy as a puppet, through the game's own `PlaySfx` test.  A sound
  started here is stopped again by its stop, when its source goes quiet for
  half a second, when the local player leaves the room, or after 10 s still
  playing (a lost stop).

The room's sim id is now `coop/4`: a `coop/3` page drops frames longer than
its limit.

### Tests

| test | result |
|---|---|
| `make coop-test` (new phase `shots`: both in room 113 with two Shotzos, B near one, A further off; A's game aims that Shotzo at B's puppet (`PortCoopTestTarget`), B's Shotzos are puppets, and a bullet of A's takes 1 hp off B in B's own game through the hazard path; `fx` adds: A's sounds play on B -- 151 reported, 141 played -- and A letting go of the inhale stops B's copy) | 55/55 (the 49 before, 6 new) |
| `tests/coop_selftest.sh` (wasm and native LP64) | 0 failures in all three runs on both; local trajectory identical |
| `cd netplay && npm test` | 51/51 |
| `make regress-wasm` | 7/7 OK (the game unchanged with co-op off) |
| `make lint-simstate` | 0 unlisted, 0 stale |

The self-test's "trajectory identical with or without a puppet" check is the
one place this work *wants* a difference: with puppets targetable, room 310's
enemies went after the puppet at frame 4882 and the local Kirby's path
changed.  Puppets are therefore not targetable while the self-test is armed,
so the check keeps proving that a puppet pushes, blocks and triggers nothing;
`shots` covers the targeting.

### Gaps

- Hits are decided where the shot is drawn, a link's latency after the
  owner fired it; a player can be hurt by a shot that, in the owner's game,
  has already gone past (and the other way round).
- Shots from minibosses and bosses, and from enemies a game runs only for
  itself (outside the owner's camera), are still each game's own.
- A forwarded sound shares the game's music players with local ones: a
  remote inhale can cut short a local sound of the same player, as two local
  sounds would.
- Grabs (Droppy, Prank, Mr. Frosty's, Phan Phan's) only grab the owner.

