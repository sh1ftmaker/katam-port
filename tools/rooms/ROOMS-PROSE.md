# Rooms mode (FREE ROAM): online players in different rooms, each device on its own Kirby

Worktree `rooms-wt`, branch `online-rooms`: the prototype (8c44be6) merged
with `main` b75dc58 (merge commit 87b3ed4), and the shipping work on top,
uncommitted.  Every number in the tables is measured by
`tools/rooms/run_all.sh`; the prose around them is written by hand
(`tools/rooms/ROOMS-PROSE.md`) and cites the tables.

## Answer first

- **It ships as a session mode.** The host picks TOGETHER (as before) or
  FREE ROAM on HOST GAME's START row (Left/Right); FREE ROAM is sealed for
  every device at the session's first frame (`PORT_LN_EV_ROOMS`, type 7) and
  travels in every image (the sim byte `view.lmpRooms`).  Off, nothing new
  runs and every gate is as on `main` (section "Gates").
- **One wasm instance per guest, not two: the canonical shadow.** Each guest
  keeps the session *as the host shows it* (player 1's screen) as one more
  state image inside the same instance (`platform/lmpnet.c`, "the canonical
  shadow"): every batch of 16 sealed frames it is swapped in at a frame top,
  run the way a rewind replays frames (no picture, no sound, no speakers, no
  records of this device), and swapped out.  It is what the host's hashes
  verify -- byte for byte, today's machinery, no masks -- and what an image
  from the host repairs.  The live state is the personal timeline: predicted,
  rewound, drawn and heard as any guest's, on this device's own player.  It
  is checked against the shadow on the same device by the gameplay digest of
  every final frame the host has verified, and when they differ it is
  rebuilt *from the shadow* (its state, re-viewed, replayed to the present):
  no network round trip.  Memory: 1.1 MiB per guest (two state buffers)
  against a whole second program (192 MiB of wasm memory reserved, ~15 MiB
  resident, plus its history); time: the table "the cost per guest".  The
  session object is unchanged (`web/ln_session.js` needs no second guest).
- **Measured in the rig** (two, three and four devices apart for long
  stretches, meeting through doors and the phone; a mid-game join; two
  players on one device; corruption of either timeline; the task pool filled
  on one timeline; a brain pointer redirected): every final frame of every
  shadow identical to the host's; every personal digest equal except at the
  injected faults (each detected and rebuilt) and once without one, a real
  view-dependent difference (F4, section 3; caught and rebuilt); every screen on its own
  player's room once that player is in a room.  **In a browser** (WebGL2,
  two and three pages): each page on its own player's room, the host's phone
  call gathering everyone, the pause menu from each page, shadows identical,
  digests equal.
- **Not done or not clean** (section 6): the HUD's icons of absent players
  can be drawn in a stale palette; the palette residue after a re-view is now
  matched on sprites but the BG palette's animation phase and the game's own
  "one step too bright" fade ending remain history-dependent; re-view during
  a cutscene is untested; the digest does not hash the contents of non-object
  tasks; per-device groups are verified for one guest with two players only.

## 1. Who is "the local player", and what local multiplayer pins

- `gLocalPlayerId` (0x0203AD3C) is the game's "whose screen is this". It is
  set once, at the start of play (`code_080332BC.c:48`, link play from
  SIOCNT), and captured into the display task (`level.c:45`, `var->unk0`),
  which decides whose room LoadLevelGfx loads and the BG follows. It has 549
  readers: the camera's BG scroll, sprite positions, the "is this object in
  the screen's room" draw macros (`object.h`, `data.h` PlaySfx), the HUD, the
  pause screens (`gPauseMenus[gLocalPlayerId]`), palette fades
  (`Create(Hidden)PaletteFade*`), music, and a few view-only task creations.
- **Input never depends on it**: slot 0 reads the pad, other people's slots
  their latched pads, CPUs their brains (`kirby.c sub_0803F324`).
- **Local multiplayer pins one view** (`platform/localmp.c`): every device runs
  with `gLocalPlayerId = 0`; the shared camera (`PortLmpCamTarget`, a group
  centre fed into the game's two camera follow modes), the straggler rule
  (carry anyone outside player 1's widest frame), and the one-room rule
  (`PortLmpArrived` + `PortLmpFrame`: everyone into the group's room). Note
  the camera is **gameplay** state: every Kirby's `gCurLevelInfo[k]` viewport
  sizes its spawn windows. The shared camera only bends the screen player's.
- **Rooms mode** (new, `gPortSim->view.lmpRooms`, off by default): the
  shared camera, the stragglers and the one-room rule off — every Kirby's
  camera follows that Kirby on every device, as in link play — and a
  per-device screen player `sView` (`PortLmpSetView`), honoured at the start
  of play (`PortLmpLocalPlayer`) and by the re-view. Off, the byte stays 0
  and nothing runs (gates below).

## 2. How it was measured

`tools/rooms/measure.mjs` runs two wasm instances of the same build in
lockstep on the same recording, rooms mode on, one showing player 1 and the
other player 2 (or 3 and 4), and compares after every frame every byte of
EWRAM, IWRAM, I/O, palette, VRAM, OAM and the sim block, plus the out-of-map
host state and SRAM through `platform/audit.c`'s per-frame hashes. For each
byte: first frame, last frame, frames differing, and the task that owned it
at its first difference. Per frame, address-independent gameplay probes
(every Kirby's position/room/HP, the RNG, the multiset of live objects with
and without their owning player, the task set) and the gameplay digest.
`tools/rooms/classify.py` names every differing byte (`symbols.py`: RAM
symbols, struct fields from the native build's DWARF, heap by owning task)
and assigns it a class with its writer.

The corpus: `tests/recordings/rooms_apart.txt` and `rooms_wander.txt`
(new, `tools/rooms/make_rooms_recordings.py`: players warped into different
areas, a meeting by door, phone meetings both ways, pause apart, Kracko's
arena and the goal room with the other player elsewhere, 9 000 frames apart),
the existing `localmp_tour` and `players_*` recordings (which, without the
one-room rule, leave player 2 elsewhere for most of their length), views 2/3
on the four-player ones, and the rig's 40 000-frame free-play session
replayed offline. Negative control: both instances on player 1's view —
zero differing bytes.

<!-- MEASURED -->

## 3. View state that feeds back into gameplay

- **F1, found and fixed (port code): the spawn margin.** With a session view
  wider than 240×160, `platform/view.c` `Margin()` sized each Kirby's spawn
  window from `gCurLevelInfo[k].unkC0[1].unk14/16`, the BG layer's map size —
  set by LoadLevelGfx only for the screen's player's room. On another device
  the same Kirby's window was different, so objects were created by another
  player (`base.unk56`), in another order: object owners differed from frame
  408 of `localmp_tour`, the object set from 862, the RNG from 2743, the
  Kirbys from 3471 ("before the fix" table). In rooms mode the margin now
  reads the room's own size (`roomWidth/roomHeight`, which `FillLevelInfo`
  sets for every Kirby). After that, nothing in the corpus diverged.
- **F2, latent: one task pool and one heap for both.** (Now tested: the
  digest's task census and the rig's `pool` run, below.) View-only tasks
  (effects created `if (gLocalPlayerId == kirby->base.unk56)`,
  `code_0806F780.c:10006`; HUD and pause tasks; goal-star digits) take slots
  of the 128-task pool and IWRAM/EWRAM heap. Measured consequence: the same
  objects at other addresses on hundreds to thousands of frames, pointers to
  them differing (`gKirbys[].base.unk6C`, collision slots, the brains'
  targets). Order of gameplay tasks is unaffected (TaskCreate inserts by
  priority after equals), and nothing in the game walks tasks by slot. The
  risk is exhaustion: a full pool hands out `gEmptyTask` and the spawn is
  silently lost on one device only. Measured peak use is in the corpus table
  (well under 128 in this corpus); a crowded boss fight was not stressed.
  Under design (c) this can only hurt the personal instance, and the digest
  catches it.
- **F3, latent: stale pointers in the CPU brains.** (Now tested: the rig's
  `pool` run redirects a brain's object pointer on one timeline; below.) The brains
  (`gUnk_02038590[k]`, +0x3C, +0x48, +0x74, +0x88, +0x98) keep object pointers
  that outlive their objects; the memory behind a stale one is reused for
  different things on different devices (measured in the free session). No
  divergence followed in 41 540 frames, so the brains do not seem to follow
  them stale, but this is an inference, not a proof. The first version of the
  digest hashed the brains' raw bytes, then what their pointers pointed at;
  both produced false personal "mismatches" in the rig's free run (frames
  26123 and 29867, each answered by a needless repair); replayed offline
  (`--parts`), the Kirbys, objects and RNG were identical there and only the
  brain part differed. The digest now hashes heap pointers in the brains as
  "a heap pointer"; the same session replayed with it: 0/41 540 frames
  differ (the "free" row of the corpus table).
- **F4, found, not fixed: an arrival one frame late on the screen's own
  device.**  In the rig's 30 000-frame two-device run (`two`, frame 18946)
  the personal digest differed from the shadow with no injection.  Replayed
  offline from the host's sealed stream (`gates.mjs --record-dir`, then
  `tools/rooms/first_diff.mjs`): two devices on views 0 and 1 diverge on
  their own.  Player 2, carried by the respawn route (state
  `sub_08055D9C`, after a call), runs its case 2 (`kirby.c`, the arrival's
  end: `flags &= ~0x400`, a random draw) one frame later on the device that
  shows player 2 -- that Kirby's state function is skipped for a frame on
  its own screen (unkD9 and stateFn unchanged across 18945-18946).  The
  cause of the skipped update is not found.  Shipped behaviour: the digest
  catches it within the next verified batch and the live state is rebuilt
  from the shadow (one rebuild in 8.4 game minutes in that run; the screen
  re-views, a white flash).
- **Checked and not feedback:** the sound engine (its two gameplay-side readers
  choose music only), the `gUnk_0203AD10` bit 0x100 (mutes effects while the
  screen's Kirby dies), screen shake (`gUnk_0203AD18`, `viewportMod*`: the
  RNG it uses is drawn on every device; only where the result is stored
  depends on the screen), palette effects (per player, hidden for others),
  the pause menu (each device runs it in its own player's slot of
  `gPauseMenus`, but the pauser's keys drive both and the pause starts and
  ends on the same frame: `gMainFlags & 0x400` never differed), OBJ VRAM and
  palette slots (taken and freed by the draw macros only for the screen's
  room).

## 4. The design that ships, and the one that does not

**The canonical shadow (shipped).**  `platform/lmpnet.c` keeps, on a guest in
rooms mode, `sCanon` (SaveState's layout plus SRAM) and the frame it is
parked at.  At a frame top of the live timeline, when at least a batch of
sealed frames is waiting and nothing else is (no rewind, image or repair
pending, the shadow not known wrong), `CanonStart` saves the live state,
loads the shadow and sets the catch-up flag: the frame loop then runs the
shadow's frames without presenting or waiting, exactly as a rewind's replay.
Its latch takes the sealed keys and events and records nothing of this
device's (`PortLnKeyOverride`); its frame tops hash (`HashState`, the regions
the host hashes) and nothing else; its sounds are noted nowhere and its
mixer only advances the channels (`m4a_mixer.c`, `audio.c`: the live block
staged before it ran is handed over after), the ears are off
(`audio_ear.c`); `gLocalPlayerId` stays the game's (`PortLmpLocalPlayer`)
and the re-view never runs (`PortLmpReview`); localmp keeps its digests in
a ring of their own.  At the batch's end `CanonTopRun` saves the shadow and
loads the live state back at its own frame top, whose processing then goes
on.  `Verify` compares the shadow's hashes with the host's as it always
compared a guest's; `PortLnHashFinal` reports them.  An image (join or
repair) is loaded into the live state as before and adopted by the shadow
(`CanonAdopt`).  `DigestCheck` compares the two timelines' digests for every
frame the shadow ran, the live timeline latched with sealed keys, and the
host verified; a difference logs which digest parts and task types differ
(`PortLmpDigestExplain`) and `PersRepair` rebuilds the live state from the
shadow.  A rewind older than every keyframe uses the shadow as its keyframe.
JS-facing calls during a shadow run use the live frame (`NowF`).

**Two wasm instances (measured, not shipped).**  The prototype's rig ran a
second complete guest.  The page would need a second Module (the web build
is not MODULARIZE), its own 192 MiB of linear memory, the ROM copied in again
and its own history; on a phone the second 192 MiB reservation is the risk.
It buys nothing the shadow does not: the shadow is the same simulation on
the same sealed inputs, verified by the same hashes.

**The host** shows player 1 and is canonical: no shadow.

**Several people on one device (item 3).**  Which slots share a device is
simulation state: `view.lmpDev` / `lmpDevSet` (two bits per slot), set from
the session's slot table at every sealed slot change and at the mode's event
(`lmpnet.c RoomsDevices`), and `view.lmpGroupRoom[4]`, each device's group
room.  In rooms mode `RoomsGroups` applies together mode's one-room rule,
dying rule and straggler rule per device that has two or more people, with
that device's anchor (its lowest slot) in player 1's place;
`PortLmpCamTarget` bends the anchor's camera to the group's centre on every
device alike (the camera is gameplay); the zoom, the off-screen arrows and a
joiner's carry follow the same groups.  A device's screen is its anchor's.
A slot whose device is unknown (no session) is a device of its own.

## 5. The re-view

`PortLmpReview` (`platform/localmp.c`) is the display half of the game's
"the screen's player arrives in a room" and of the pause exit, without
their gameplay half (prototype, unchanged in substance).  New: it waits in
the rooms with a display of their own (918, 919); and once its fade has
ended and no palette effect runs it sets the OBJ shadow palette to its
source (`ReviewSettle`).  Measured (table "the re-view"): at the first test
point the picture now equals the always-player-2 device after 19 frames
(was 724).  At the others the remaining differences are not the re-view's:
dumps at +300 frames show the always-player-2 device itself with sprites one
step too bright (its own last fade ended there; the game's queue-indexed
restore, `palette_effects.c` "Indexed by queue position"), and BG palette
animation phases that differ by when the room was entered; the re-viewed
device shows the room's source palette.  The party HUD, the pause menu and
the sound follow the personal timeline by construction: they are the live
state's (screenshots in the browser table's directory).

## 6. What ships, what does not

Ships (uncommitted): the merge; the session mode and the HOST GAME choice;
the canonical shadow; per-device groups; the digest with a task census; the
re-view's palette settle and special-room wait; the waiting hint (party HUD:
"THE DOOR WAITS FOR 2P - R CALLS" / "THE STAR ..." while this screen's player
stands at a door to the hub or on the goal star and a person is elsewhere,
up to 4 s, never blocking); the rig, census and cost tools; the browser case.

Does not, or not cleanly:
- F4 (section 3): a real view-dependent desync remains; it is caught and
  healed locally, not prevented.
- The HUD's icons of other Kirbys use those Kirbys' own sprite palettes; a
  Kirby in another room may have none loaded on this screen, and its icon
  shows in a stale palette (seen: 3P's page, 1P's icon pale).  Cosmetic.
- The hint is from the screen player's position only (doors to the hub and
  the goal star); boss doors and the area doors were not checked for the
  wait-for-everyone rule.  It is not localised.
- Re-view during a cutscene or an intro walk is not specially handled and
  not tested; 918/919 wait.
- The digest hashes the contents of the Kirbys, the brains and every live
  object, and the *existence* of every other task type; the contents of
  non-object gameplay tasks are not hashed (a difference there is caught
  once it moves something hashed).  The view-only task list comes from the
  corpus and the rig; a task type the corpus never met that differs by view
  would cost a needless local rebuild (logged with its main), not a desync.
- Per-device groups: verified with one guest holding two players (the group
  gathered after every forced split); host-side groups (the host's own
  second player) only by construction.
- Phone etiquette, doors that wait: the original rules, unchanged (decisions
  for the user, below).

## 7. Rerun

```
# everything in this report (build, measure, re-view tests, gates.mjs, census, cost, regress, netproof), ~20 min:
tools/rooms/run_all.sh ROM TMPDIR netproof-eval/ROOMS-REPORT.md
# pieces:
node tools/rooms/gates.mjs --rom ROM --out TMP/gates.json [--quick] [--only two,corrupt]   # ~4 min full
node tools/rooms/census.mjs --rom ROM --rec tests/recordings/rooms_apart.txt --out TMP/census.json
node --expose-gc tools/rooms/cost.mjs --rom ROM > TMP/cost.json
node tools/rooms/review_test.mjs --rom ROM --rec tests/recordings/rooms_apart.txt --at 1800 --frames 2600
node tools/online_browser_test.mjs --cdp PORT --base URL/dist/ --rom-url URL/rom.gba --relay URL --shots DIR --cases rooms --rooms-pages 2,3  # ~2 min
python3 tools/rooms/report.py --tmp TMP --out OUT.md --prose tools/rooms/ROOMS-PROSE.md
# natively: KATAM_LOCAL_MP_ROOMS=1 KATAM_LOCAL_MP_VIEW=k [KATAM_LOCAL_MP_VIEW_AT=F:k]
```

## 8. Files

- `platform/lmpnet.c`, `platform/port/lmpnet.h`: `PORT_LN_EV_ROOMS`; the
  canonical shadow (one contiguous section before "the frame top", plus
  hooks in the latch, `Verify`, `PortLnHashFinal`, `ApplyImage`, `Rewind`,
  `PortLnFrame`, the sound/SRAM/setting notes, `PortLnStop`); telemetry
  fields appended; `RoomsDevices`, `RoomsSetView`.
- `platform/localmp.c`, `platform/port/localmp.h`: sealed mode, devices and
  groups, the digest's task census and two rings, `PortLmpDigestExplain`,
  `PortLmpWaitingFor`, the re-view's settle and wait, test hooks.
- `platform/port/simblock.h`: `view.lmpDev`, `lmpDevSet`, `lmpGroupRoom[4]`
  (were spare zero bytes; size unchanged).
- `platform/m4a_mixer.c`, `platform/audio.c`, `platform/audio_ear.c`: the
  shadow is heard by nobody.
- `platform/online_ui.c`, `platform/port/online_ui.h`: HOST GAME's mode.
- `platform/ui/party_hud.c`: the waiting hint.
- `web/online.js`: seals the mode at the host's start; rooms fields in stats.
- `Makefile` (exports), `tests/sim_state_allowlist.txt`.
- Tools: `tools/rooms/gates.mjs`, `census.mjs`, `cost.mjs` (new);
  `rig.mjs`, `rooms_run.mjs` (the two-instance prototype rig) removed;
  `run_all.sh`, `report.py` updated; `tools/netproof/run.mjs` (a `rooms`
  option, observers, per-guest flags); `tools/online_browser_test.mjs`
  (case `rooms`).
