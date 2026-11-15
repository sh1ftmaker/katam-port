# Rooms mode: online players in different rooms, each device on its own Kirby

Worktree `rooms-wt`, branch `online-rooms` (from `online` 85dceed), uncommitted.
Every number in the tables below is measured by `tools/rooms/run_all.sh`; the
prose around them is written by hand (`tools/rooms/ROOMS-PROSE.md`) and cites
the tables.

## Answer first

- **It works, and the original design carries it.** With players free to be in
  different rooms and each device showing its own Kirby, the gameplay of two
  devices fed the same inputs stayed identical over the whole corpus (about
  145 000 frames, players apart most of the time, meeting through doors and
  the cell phone, pausing apart, a boss room and the goal room with the other
  player elsewhere) — **after one fix**: the port's own wide-view spawn
  margin read a per-screen BG field (below, F1). The original game's link
  play already kept gameplay independent of the screen; the port's additions
  were the risk, and the measurement found exactly one.
- **The raw images never agree, and cannot be made to by masks alone.** Every
  frame about 2 000–3 000 byte runs differ: the display and its shadows,
  palettes, BG streaming, sound, HUD, pause screens — and, the hard part, the
  *heap*: view-only tasks (HUD effects, pause screens, effects created only
  for the screen's player) take task slots and heap first, so the same
  gameplay objects sit at other addresses and the pointers to them differ
  (class H below). A mask cannot say "this byte is the same object
  elsewhere".
- **Recommended: design (c), a canonical instance plus a personal instance on
  each guest device.** The canonical instance shows player 1's view on every
  device, so it is today's byte-identical image: today's whole-image hash
  with no masks, today's join and repair, untouched. The personal instance
  is what the player sees and hears; it is checked against the canonical one
  on the same device by a positively built gameplay digest, and repaired
  from a host image plus a **re-view** (the game's own room-arrival and
  pause-exit display paths, without their gameplay half). The host shows
  player 1 and needs no second instance.
- **The prototype holds in the rig** (table "the rig"; the corruption run is
  lockstep, so an injection cannot be healed silently by a rewind; the
  screen check skips the 2 s after each image load, while the re-view may
  legitimately wait): over five runs the
  canonical instance matched the host on every final frame; the personal
  instance's digest matched the canonical's on every compared frame; a
  mid-game join with the players apart landed on player 2's screen; gameplay
  corruption was detected and repaired on both instances; zero unexplained
  byte differences in the classification.
- **What is not done**: the personal instance in the rig is a second full
  session guest whose outgoing traffic is dropped (the shipped form would
  share one session object); the rooms switch is a host setting latched into
  the sim block, not a sealed session event; several local players on one
  device in rooms mode (the one-room rule per device) is designed, not built;
  the re-view leaves small palette/HUD-icon colour differences in some rooms
  until the next room change (table "the re-view"); special rooms with their
  own display setup (918/919, `sub_08002EC4`) are not handled by the re-view.

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
- **F2, latent: one task pool and one heap for both.** View-only tasks
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
- **F3, latent: stale pointers in the CPU brains.** The brains
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

## 4. The designs

**(a) One instance per device, per-device views, a gameplay hash.**
Verification needs a hash over "everything except the view", and the
measurement says that set is not a byte mask: besides ~60 view symbols, the
H class is heap layout — same objects, other addresses, other slot
numbers, other pointers. It needs a positively built, address-independent
gameplay hash (walk the tasks, hash each gameplay struct's fields with
pointers translated to identities). The digest here is that idea, for the
Kirbys, brains and objects; making it complete means a reviewed field list
for every task type that holds gameplay (dozens: boss parts, doors, switches,
chests, cannons, the goal star, the cutscene triggers…) and maintaining it.
Join and repair: the host's image, then a re-view. Cheapest at run time (one
simulation); weakest verification: anything outside the hash diverges
unseen. Pool exhaustion (F2) is a real desync here.

**(b) Every player's view inside the image.** The GBA has one VRAM, one
palette, one OAM, one set of display registers, one sound engine; the game
draws into them. Keeping four views in the image means four copies of all of
it, and every writer above taught to write the right copy — the game's whole
display and sound pipeline. Not credible for the gain.

**(c) A canonical instance for truth, a personal instance for the screen
(recommended).** Measured above: the canonical instance stays byte-identical
to the host with today's machinery unchanged; the personal instance's
gameplay stays identical to the canonical's (digest equal on every compared
frame once the digest stopped looking through heap pointers), and when it
does not (injected), it is repaired from the host's image and re-viewed.
Costs: a second simulation on each guest (render-off sim is ~6 µs/frame in
wasm; the personal instance renders and is the one that predicts and rewinds,
so its cost is today's guest cost; the canonical one only needs the sealed
frames — in the prototype it is a full guest too); a second 0.5 MB image
plus its keyframe history (24 × 494 KiB ≈ 12 MiB in the rig's settings);
the re-view's room load (an LZ77 tileset + BG redraw, once per join/repair,
repeated if a rewind crosses it — measured re-views per run in the rig
table). The canonical instance's sound is muted; the personal one's is
the device's sound, room-correct by construction.

**(d) Variants.** (c) with the canonical instance running only sealed frames
(no prediction, no rewinds, no rendering: cheapest truth) — the prototype
does not do this yet, it is the shipped form. (a) with the digest shipped
next to the hash ("trust the digest") for very weak devices. A seats-style
single wasm instance swapping two 0.5 MB images per frame
(`platform/seats.c` already does this) instead of two wasm instances.

**Cross-cutting, as measured:**

- *Sound*: today the session's sound state is canonical and inside the
  image. Under (c) the canonical instance keeps that (muted); the personal
  instance's m4a state is per device (class VS) and its replays need today's
  guest sound judging (refire/mispredict), unchanged.
- *Pause*: the pause starts and ends on the same frame everywhere; what each
  screen shows during it is its own player's menu slot driven by the
  pauser's keys. Not checked visually.
- *Several people on one device*: they share that screen, so the one-room
  rule, the stragglers and the shared camera apply per device. The shared
  camera bends a Kirby's camera, which sizes spawn windows — gameplay — so
  the per-device grouping must be session state in the canonical simulation
  (a slot→device map in the sim block, the group camera applied to the
  group's anchor regardless of `gLocalPlayerId`). Not built.
- *Cell phone*: the original's own mechanism works unchanged: the caller
  presses R, the others answer with R (`kirby.c` Macro_0803FF64_7) and arrive
  in the caller's room; CPUs answer by themselves. Measured meetings: the
  rig's "meetings" column.
- *Big doors and the goal game*: they wait for every person
  (`shard.c`, `special_doors.c`, `warp_star.c:2003`), in any room — the
  original's link-play rule. In `rooms_apart` the goal star waited for
  player 2 until player 2 arrived. A shipped version wants a "waiting for
  the others — call them" hint; no simulation change.
- *Boss rooms*: a boss fight with the other player elsewhere showed no
  divergence; boss HP scales with the number of people playing, not present
  (the original rule).
- *The join image*: unchanged — the host's image (player 1's view), 506 KB raw.
  The joiner's canonical instance loads it as today; its personal instance
  loads the same bytes and re-views. No extra data on the wire.

## 5. The re-view

`PortLmpReview` (`platform/localmp.c`) is the display half of the game's
"the screen's player arrives in a room" (`kirby.c sub_080562D0`) and of the
pause exit (`in_game_interlude.c RestoreGameplayAfterInGameInterludeScreen`):
re-pin `gLocalPlayerId` and the display task, the Kirbys' and shared sprites'
OBJ tiles and palettes (`sub_0803E458` + `sub_0803E050`), SaveDisplayState +
RestoreDisplayState (LoadLevelGfx, animated tiles, BG redraw), the room's
palettes, a fade from white for the screen's player and a hidden one for the
old (the arrival's own pair), the HUD (`RefreshGameplayHud`, the HUD's
per-Kirby icon sprites re-initialised as `CreateGameplayHud` does, the
Kirbys' 0x80000 hide bits cleared — `DrawHudOtherKirbys` sets them for
Kirbys in other rooms and would never clear them on the new screen's
Kirby), the area name, the room's music. Objects' sprites follow by
themselves: the game's draw macros free tiles for objects outside the
screen's room and take them inside it. It waits while paused or between
rooms. Measured residue against a device that showed player 2 from the start
(table): the room, camera, BG, objects, Kirbys and HUD layout match; in some
rooms the OBJ palette stays one step (1/31) brighter and the HUD's
other-Kirby icons a shade off until the next palette event; native
screenshots (`tools/rooms/pngdiff.py`) show differences confined to the HUD
icons, the screen's own Kirby's shading, or the ability label.

## 6. What a shipped version still needs (rough size)

1. Session: the rooms switch as a sealed session parameter (in the join
   image already: the sim byte), not a host setting. Small (~50 lines C/JS).
2. The guest device's two instances in `web/ln_session.js`/`web/online.js`:
   one session object feeding both, the canonical instance on sealed frames
   only, render/sound from the personal one, the digest comparison and the
   personal repair (in the rig: `tools/rooms/rooms_run.mjs`). Medium
   (~400–700 lines JS), and it collides with the four-device work in
   `slots-wt` — do it after that merge.
3. Per-device groups for several local people on one device (sim-block
   slot→device map, per-group one-room rule and camera). Medium (~300–500
   lines C + recordings).
4. Re-view polish: the palette/HUD residue, special rooms (918/919),
   Dark Mind's arenas, re-view while a fade or cutscene runs. Small–medium.
5. Digest review: a positive field list for the task types that hold
   gameplay beyond Kirbys/brains/objects (the digest is the personal
   instance's only check). Medium, mostly reading.
6. UI: whose screen the device shows, "waiting for the others" at big doors
   and the goal star. Small.

## 7. Rerun

```
# everything in this report (build, measure, re-view tests, rig, gates, netproof), ~15 min:
tools/rooms/run_all.sh ROM TMPDIR netproof-eval/ROOMS-REPORT.md
# pieces:
node tools/rooms/measure.mjs --rom ROM --rec tests/recordings/rooms_apart.txt --out TMP --views 0,1   # ~12 s
node tools/rooms/review_test.mjs --rom ROM --rec tests/recordings/rooms_apart.txt --at 1800 --frames 2600  # ~20 s
node tools/rooms/rig.mjs --rom ROM --out TMP/rig.json [--quick] [--only join]                       # ~1-2 min
python3 tools/rooms/report.py --tmp TMP --out OUT.md --prose tools/rooms/ROOMS-PROSE.md
# natively (screenshots, gdb): KATAM_LOCAL_MP_ROOMS=1 KATAM_LOCAL_MP_VIEW=k [KATAM_LOCAL_MP_VIEW_AT=F:k]
```

## 8. Files

Engine (all additive; every path is behind `gPortSim->view.lmpRooms`, a
spare zero byte, or a host setting that defaults off):

- `platform/port/simblock.h`: `view.lmpRooms` (was `reserved[0]`, size and
  layout asserts unchanged).
- `platform/localmp.c`, `platform/port/localmp.h`: rooms mode (switch, view,
  `PortLmpLocalPlayer`, the re-view, the gameplay digest, the test hooks);
  the shared camera, stragglers, one-room rule and joiner carrying skipped in
  rooms mode.
- `game/src/code_080332BC.c`: `gLocalPlayerId = PortLmpLocalPlayer(arg1)`.
- `platform/view.c`: the spawn margin from the room's size in rooms mode (F1).
- `platform/inputlog.c`: `option rooms 1`.
- `Makefile`: exports (a separate `PORT_EXPORTS +=` line, to merge cleanly).
- `tests/sim_state_allowlist.txt`: the new host statics.

Tools and recordings (new): `tools/rooms/` (`measure.mjs`, `review_test.mjs`,
`rooms_run.mjs`, `rig.mjs`, `symbols.py`, `summarize.py`, `classify.py`,
`objdiff.py`, `pngdiff.py`, `report.py`, `make_rooms_recordings.py`,
`run_all.sh`, this prose), `tests/recordings/rooms_apart.txt`,
`rooms_wander.txt` (inputs only, `no-regress`).

Not touched: `platform/lmpnet.c`, `web/ln_session.js`, `web/online.js`
(the four-device work in `slots-wt`).
