# Eight seats: the engine's side (stage 1)

Branch `eight-engine`. Stage 1 of the eight-humans plan: the human-slot layer
(`platform/localmp.c`, `port/localmp.h`) and the session engine
(`platform/lmpnet.c`, `port/lmpnet.h`) are N wide in the wide build. This note
is the contract that stage 2 (wire, hub, rig) builds on. The default build
(4 Kirbys) is unchanged: every name below equals the old constant, and the
old 4-wide functions behave as before.

## Widths

| name | default | wide build (`KATAM_MAX_KIRBYS=N`) |
|---|---|---|
| `PORT_LMP_SLOTS` (`port/wide.h`) | 4 | N |
| `PORT_LN_SLOTS` = Kirby slots a session seals | 4 | N |
| `PORT_LN_MAX_DEV` = devices (host + guests) | 4 | N |
| seats per device | 4 | 4 |
| `PORT_LN_OWN(dev, seat)` = `1 + dev*4 + seat` | ≤ 16 | ≤ 64 (a u8) |

At run time: `PortLnSlots()` and `PortLnMaxDevices()`.

**Seats the host may give: `PortLnSetSeatSlots(n)`** (host, 1..N). This is
4 until the caller raises it, and it is kept across sessions. A seat is only
granted in slots below it, and `PortLnFreeSlots` counts within it. Today's
page and rig seal k0..k3 only (`PortLnSealedGet` / `PortLnSealed4`), so a w8
module driven by them must never seat a person in slot 4..: that person's
keys would not travel, and the session diverges. The rig showed exactly that
before this limit existed (R13 churn, slot 4 given to device 3). Stage 2
calls `PortLnSetSeatSlots(PortLnSlots())` once its wire carries every slot.

**Devices.** The host is device 0, and guests are 1 .. `PORT_LN_MAX_DEV`−1.
On a w8 module, devices 4..7 are numbered and handled exactly like 1..3:
`PortLnRemoteInputSeat(f, dev, seat, keys)`, `PortLnDeviceGone(dev)`,
`PortLnSeatRequest(dev, seat, on)` and `PortLnDeviceSettings(dev, …)` all take
`dev` up to `PORT_LN_MAX_DEV`−1. The host's per-frame seat mask is bit
`(dev−1)*4 + seat`, a u64 in the wide build.

## The sealed record

**Host, read.** `PortLnSealedGetN(f, out)` fills `PORT_LN_SEALED_WORDS`
(= 10 + N − 4) u32 words. It returns 0 if frame f is not sealed.

| word | meaning |
|---|---|
| 0, 1 | keys of slots 0, 1 |
| 2 | number of events sealed with f (read them with `PortLnSealedEventGet`) |
| 3 | device 1 seat 0's key was on time |
| 4, 5, 6 | hash lo, hash hi, hashed (1/0) |
| 7, 8 | keys of slots 2, 3 |
| 9 | N (`PORT_LN_SLOTS`) |
| 10 + i | keys of slot 4 + i, for i < N − 4 |

Words 0..8 are exactly `PortLnSealedGet`'s, which is unchanged. On a w8
module, w8 needs 14 words and w16 needs 22; a 512-byte scratch buffer holds
either.

**Guest, write.** `PortLnSealedN(f, keys, n, nev)` takes `keys` as a pointer
to n u16 values in slot order (from JS: `_malloc(2*N)`, write `HEAPU16`).
Slots from n on are 0. `PortLnSealed4(f, k0..k3, nev)` is `n = 4`. As before,
a frame's events (`PortLnSealedEvent`) arrive before its record.

A slot nobody plays seals 0, so a wire that sends "slot keys follow" bits only
for non-idle pads stays sparse. The analysis's plan (flag bit 6: a mask byte
for slots 4..7 follows) is the wire's choice; the engine does not care how
the keys travel.

## Events

`PORT_LN_EV_KIRBYS = 10`, a = the Kirbys in all (4..N). This is the host's
CPU KIRBYS setting (`platform/wide.c` `PortWideSetTarget`), and the default
build ignores it. On the host, `PortWideRequestKirbys(n)` during a session
becomes `PortLnEvent(PORT_LN_EV_KIRBYS, n, …)`; on a guest it is refused. It
travels like any other sealed event: no new wire record, only a new type
value.

**Seats past the live count** need no event of their own. The count in play
is max(4, setting, highest person slot + 1), computed from simulation state
(`PortLmpPeopleEnd`, which counts held joins too). `PortLnSeatRequest`
prefers a free slot whose Kirby exists, and otherwise takes the next slot. It
then seals the ordinary `PORT_LN_EV_HUMAN`. Every device then holds the join
(`lmpw.waiting`; `PortLmpSlotPerson` is already true), the count rises one
Kirby a frame while play allows, and the join is applied once the Kirby
exists. The count shrinks only from the end, and never past a person's Kirby.
I chose this over the analysis's "raise by event, then grant" for two
reasons: a raise event would leave the setting raised after the person left,
against the owner's rule count = max(4, people, setting); and a guest that
rewinds over the join replays it with nothing extra to seal.

## Join and repair images

The header is unchanged (64 bytes, same offsets; bytes 52..55 hold slots
0..3). The wide build writes version 3 and appends every slot's owner (N
bytes) after the host extras. `PortLnImageBytes()` includes it.

## Telemetry (wide build only)

The wide build adds `own4..own7` (the owners of slots 4..7; slots 8.. are not
listed) and `dHiKeyOnTime` / `dHiKeyMissing` (guest devices 4 and up)
at the end of the block. Everything else keeps its name and position.

## Still four wide, on purpose

- Seats per device (4): `lk`, `rk[dev][4]`, `PortLnSeatAsk`, `sLastLocal`.
- `PortLmpSlotIsHuman` / `PortLmpRequestHuman` in a session speak of a
  device's local seats 1..3, as before.

Stage 5 (branch `eight-hud`) widened the pause page's session copies and
names: `PORT_LN_EV_PSET` and `_NAME` are sealed for every slot
(`PORT_LN_SLOTS`), `menu_poc.c` keeps `POC_SLOTS` = `PORT_LN_SLOTS` copies
(its host state, `PocHostState`, is that wide: same build only), and
`player_card.c` names `PORT_LN_SLOTS` slots (`PortCardSlotNames` lists that
many, tab-separated).  Paint (`PORT_LN_EV_PAINT`, `kirby_paint.c`) covers
every slot (`PORT_PAINT_SLOTS`): slots 0..3 in the default layout's
`game.paint`/`paintFeet`/`paintDirty`, slots 4.. in `gPortSim->paintw`
(`struct PortWidePaint`, port/wide.h, in the padding before the wide area:
snapshotted, hashed, in images), with `PORT_PAINT_DIRTY_WIDE` in
`paintDirty` while any of those reloads is pending.  A Kirby 4..'s sprite
`palId` maps back to its slot as `(palId >> 4) * 4 + (palId & 3)`.  Kirbys
4.. have colours of their own (`PortWideDefaultColor`: the sprays 1..4 do
not wear, Snow, Carbon, Ocean, Grape first), and while more than four are in
the world the GPU renderer draws whatever GRAPHICS says (`PortWideNeedsGpu`,
platform/ui/present.c).

## Exports

`_PortLnSealedN`, `_PortLnSealedGetN`, `_PortLnSlots`, `_PortLnMaxDevices`,
`_PortLnSetSeatSlots` and `_PortLnSeatSlots` are in `PORT_EXPORTS`
(Makefile), in both modules.

## Checked (2026-11-15; scratch and logs in `netproof-eval/eight-tmp/engine/`)

- **The engine at 8 devices.** `n8.mjs` drives the API above directly, with
  no wire: a host and guests 1..7 on the w8 module, joined by image. Guests
  run 4 frames ahead, records arrive 1..3 frames late, and some pads arrive
  after the host has sealed them. Over 6,000 frames: CPU KIRBYS 4 sealed, 7
  seats (4 of them past the live count, each made and then joined), devices
  5..7 gone (the count shrinks to 5), device 7 back in its own slot 7 (the
  count rises again), CPU KIRBYS 6. All 7 guests verified 5,999 frames each
  with 0 mismatches, and the final hashes agree. On the default module the
  same harness passes with 3 guests.
- **8 local humans** (`h8.txt`, `h8rooms.txt`, `option localmp 8` with
  `human` churn and `kirbys 4`) on native w8:
  - ASan + UBSan: 0 NEW (`tools/wide_sanclass.py`);
  - the same input twice gives identical hash logs;
  - native == wasm in keys/vid/sram/pix on every frame.
- **Default module.** `tests/layout_neutral.sh` against the live binary,
  `make regress` (same result as the reference), and the rig's quick pass:
  16 gates, none FAIL.
- **The w8 module under today's 4-wide rig** (scratch copy reading the Kirby
  address from the environment): 16 gates, none FAIL.

# Stages 2 and 3: the wire, the hub, the page (branch `eight-wire`)

## The sealed record on the wire (web/ln_session.js)

Per frame: `u8 flags`, `[u8 nev]`, `[u8 mask 4..11]`, `[u8 mask 12..19]`,
`u16` keys of every listed slot in slot order, `[u32 lo, u32 hi]`,
`nev x 17` bytes of events.  Flags: bits 0-3 slots 0..3 have keys, bit 4
hashed, bit 5 events, **bit 6** the mask byte for slots 4..11 follows, **bit
7** the one for slots 12..19.  A slot with no key pressed is not listed, so
a record whose slots 4.. are idle -- every record of the 4-Kirby module --
is the same bytes as before bits 6 and 7 meant anything
(`tools/ln_wire_test.cjs` checks this against a copy of the old encoder, and
round-trips 8- and 16-slot records).  Eight held pads: 18 B a record.

On a module with more than 4 slots the engine adapter (`wideCalls`, shared
by the page and the rig) reads `PortLnSealedGetN` and writes
`PortLnSealedN`; the 4-Kirby module keeps `PortLnSealedGet` /
`PortLnSealed4`.  `HostHub` calls `PortLnSetSeatSlots(seats)` (the room's:
8 in an 8-player game) and numbers devices 1 .. min(seats,
`PortLnMaxDevices`) - 1.

## Bounded redundancy (HostSession opts.copies, opts.maxPacketBytes)

`copies: 0` (the 4-Kirby module, unchanged): every record from the guest's
ack on, in every packet, until acked.  `copies: 4` (8-player games): each
record in 4 consecutive packets, then not again unless the guest's ack (its
contiguous sealed frontier, in every input packet: the NACK) is still on it
one retransmit timeout after its last copy; then everything from the ack
goes again (up to 64 records / `maxPacketBytes` 1100 B) and the timeout
doubles until the ack moves.  Timeout: srtt + 4 rttvar + 2 frames of the
ack's round trip, 60..1000 ms, Karn's rule.  Until a guest acks its join or
repair image the old rule holds.  Rig gate R17 proves it under 10% loss
with 150 ms bursts and under 30% loss, with a no-retransmit ablation.

## Modules

8-player games run on the existing 16-Kirby module (katam16.wasm).  Its
keyframes are 940 KiB: 24 do not fit its 192 MiB heap (PortLnStart or the
image buffer fails, node and browser alike), so the page (and the rig,
`--kf-count`) keep 16 there: 240 frames of history.

Measured (rig, wasm in node, the same 8-device session on both, 8 Kirbys in
play, 16 keyframes, under the same load):

| | w8 (KATAM_MAX_KIRBYS=8) | katam16 |
|---|---|---|
| snapshot / join image raw | 725 KB | 963 KB |
| join image deflated | 56.5 KB | 57.2 KB |
| replay, per replayed frame | 146 us | 190 us (+30%) |
| hash (every 16th frame) | 134 us | 171 us (+27%) |
| keyframe save / restore | 20 / 43 us | 27 / 56 us |
| sim, per frame | 10.9 us | 12.7 us |
| history in 192 MiB | 24 keyframes (360 f) fit | 16 (240 f) |
| replay CPU per guest, 8 people @120 ms (736 replayed f/s) | 10.7% of a core | 14.0% |

Rewinds, depth, bandwidth and the game itself were identical between the
two (the same rewinds/min to the decimal).

The sim id names the module (`KATAM_SIM16` = `KATAM_SIM` + `.k` + the
16-Kirby wasm's id; tools/stamp_build.py).  A joiner on the other module
reloads into it with `?mod=16&join=CODE` (or `?mod=4`), once per code a
minute, when the invite link (`mod=16`), the public list (`mod`), the
room's version gate (`error version sim`, whose `room` is the other
module's id) or the host (`bad` with `mod`) says so.

## Menus

ONLINE has a fourth row (platform/online_ui.c `PORT_OUI_ROW_OTHER_MODULE`):
HOST 8 PLAYERS on the 4-Kirby module, HOST 4 PLAYERS ("LIGHTER ON PHONES")
on the 16-Kirby one; A pushes `PORT_OUI_EV_OTHER_MODULE` and the page
reloads on the other module (`katamOnline.otherModule()` is the same from
JS).  On the 16-Kirby module MULTIPLAYER > ONLINE is open (its games hold 8:
`PortOuiSeats()`).

**The default module (2026-11-16).**  The page loads the 16-Kirby module
unless `?mod=4` (or `?kirbys=4`, the parked `?room=` / `?world=` /
`?online=1` flags, the room playtest page): a fresh visitor gets katam16 at
4 Kirbys, hosts 8-seat rooms, and has LOCAL multiplayer there (the wide
build's human-slot layer already carried online seats; the refusal was
only untested -- `devices_browser_test --cases solo,lmp,local --mod default`
and `menu_exits_test --cases default,local --mod default` now run it).  A
4-player room's invite link names `mod=4`; the public list's games without
`mod` are 4-Kirby module games (web/online_browse.js `modOf`).  CPU KIRBYS
on `?mod=4` above 3 reloads to the page's address without `?mod`.

# Before publishing: pause, the opening, W0 (branch `eight-fix`)

**Players 5..16 pause.**  START by a player in slot 4.. trapped: the area
map read `roomInfos[pauser]` from an array of four (a NULL or garbage
room info).  Behind it, the pause menus (`gPauseMenus`, one per Kirby in
the wide area) were set up, read and closed for 0..3 only, so such a pauser
could not close the pause, and a screen whose own player is 5.. (rooms
mode) had no menu to turn pages with.  Now `KATAM_PAUSE_MENUS` (port/wide.h)
makes the pause screens' loops visit every Kirby in play, and the area and
world maps keep `KATAM_MAP_MARKERS` (8) markers: every Kirby up to eight,
else the pauser, the screen's own player, the other people, then CPU Kirbys
(`PortWideMapMarkers`).  On the area map markers 4..7 stand at the middles
of a room's edges (the console's four at its corners), each in its Kirby's
own colours (GPU renderer); on the world map Kirby i stands at the ROM's
spot for i & 3, 6 px to the right.  4 at N = 4: the default build and W0
are the console's code.  The rig no longer masks START for slots 4..
(`--pause-mask` brings the mask back).

**The opening.**  The live count used to change only from NORMAL (300).  It
now changes in the opening too, except while the opening's cutscenes play
(`platform/wide.c OpeningHolds`: an in-game interlude, or player 1 in the
hub 0x321 below NORMAL); a Kirby made then is made as the start makes
Kirbys 1..3 and copies player 1 in the tutorial.  A seat past the count on a
fresh file is given within a second (browser `eight`: about 300 ms at story
stage 100), and CPU KIRBYS changes the count on a fresh file.  A second hold
sat behind it: once player 1 leaves the tutorial the CPU Kirbys walk to the
hub and wait there for the opening cutscene, and a join of a Kirby in the
hub waits ("the story has it", `localmp.c Scripted`) -- with player 1 warped
on, until NORMAL.  Kirbys 1..3 are the story's and keep that rule (as in the
default build); a held join of slot 4.. now has its Kirby carried from the
hub to player 1 and goes ahead once it arrives (`FetchFromHub`: about 10
frames).  The rig leaves the scenario's story stage alone (`--story-normal`
seals NORMAL as before).

**W0.**  `tools/wide_unrelocate.py` maps the human-slot layer's fields
(`gPortSim->lmpw`) back into the default build's four-slot fields, from
`simfield` lines in the KATAM_WIDE_MAP file; nothing is masked.  19/19.

# Pause per device (branch `six-pause`)

**What went wrong.**  The pause is the game's, one for the shared world: a
Kirby's START (`kirby.c` the pressed edge `unk11A & 8`, then
`StartInGameInterlude`, gUnk_0203AD50 = that Kirby) pauses everyone, and the
pause screens read a mixture of pads.  `pause_help.c PauseMenuFetchInputs`
gives the pauser's pad to the pauser's menu and to the screen's player's
(gLocalPlayerId), the console's pad to every other menu -- online that is
slot 0's column (`lmpnet.c PortLnKeyOverride: *keys = k[0]`), the host's --
and Select / L / R are taken from all menus at once.  On the SETTINGS page
the cursor (`pause_settings.c PauseSettingsRepeat`) took gRepeatedKeys, the
console's pad again, whenever the pauser was the screen's own player
(`PortLmpViewer() == gLocalPlayerId`): in FREE ROAM (the default) that is
exactly the pauser's own device, where the cursor then followed the host's
d-pad while on every other device it followed the pauser's.  Hence what was
seen with six tabs: everyone paused, the pauser moved the others' menus, the
host moved the pauser's, the host's pause moved everyone's.  Nothing here is
wide-only: the 4-Kirby module behaves the same (any guest that pauses in
FREE ROAM).

**What it is now.**  Online, START in play never reaches the session and
opens a pause OVERLAY on that device only (`lmpnet.c` "the pause overlay"):

- In play (`PauseInPlay`: the gameplay HUD's task, no in-game interlude, not
  the demo) START is taken out of what a device sends for its seats
  (`PortLnLocalInputSeat` returns the keys that go to the session;
  `web/ln_session.js` sends those) and out of every key the host seals
  (and the guests predict).  The shared world therefore never pauses; no
  game code changed for that.
- START on a seat opens the overlay: at the next frame top the live state is
  copied into a private FORK, and from then on each frame runs one frame of
  the fork after the live frame's top -- this device's pad on its own Kirby,
  every other slot's keys 0, so the game's own pause opens there with this
  player as the pauser and its own screens (help, area map, world map with
  every marker, SETTINGS with ONLINE, the list and REMOVE, MAIN MENU) run
  exactly as offline.  The fork frame is composed (both renderers) but not
  presented; the live frame that follows runs quiet (simulated, not
  composed: `ppu.c`, the render list) and presents the fork's picture.
  Pacing, audio and everything the page calls between frames see the live
  state.
- While it is up the seat sends 0s: its Kirby stands in the shared game.
  When the fork's pause closes (B / START / leaving, as the game has it) the
  overlay goes, and the buttons still held are kept back until released.
- Device state only: the fork is never in a keyframe, an image or a hash; a
  rewind, a repair or a join image leaves it as it is.  Its buffers hold the
  snapshot and the menus' host state (`menu_poc.c` keeps the fork's screen
  state and shows the session's copies -- names, settings -- as they are
  now: `PortPocHostStateLoadOwn`); the slot table is never swapped.
- The SETTINGS page on the overlay draws this device's own settings and
  carries an edit out on this device at the next live frame
  (`PortLnOverlayEdit`), where the shared pause wrote the session's edit
  request and carried it out from the final frame; the copies other devices
  hold follow through `PORT_LN_EV_PSET` as before.  What a session forbids
  (rows dimmed NOT DURING AN ONLINE GAME) stays forbidden.  REMOVE and MAIN
  MENU are the page's own actions (`online.kick`, `online.leave`), deferred
  by the page to its next turn, so they run with the live state in memory.
- The fork's sound is never heard (state only, as the canonical shadow's:
  `PortLnShadowRunning`); its frames reach no digest, no telemetry of the
  session, no input log, no warp queue.  The party HUD and the name tags
  hide while the overlay is up.

What a player notices: their pause no longer stops anyone, nobody else's
stops them; the world goes on behind their menu (their Kirby standing); the
maps show where everyone was when they paused; the pause screens' own sound
effects are not heard online (the game's are); and a press of START during
play reaches nothing but the overlay (the host's START used to skip a few
prompts that also take A or B).  Offline and local multiplayer are as they
were.  Fixed on the way, everywhere: the SETTINGS cursor reads the pauser's
pad whenever the pauser is another person on the console
(`PortLmpHumanSlot(gUnk_0203AD50)`), not "unless the pauser is the screen's
player" -- local FREE ROAM had the same fault.

Telemetry (`ovl*` at the end of the block): state, seat, opens, closes,
refused (START where the fork could not pause: a door, a cutscene), fork
frames, edits, the swap's cost (about 30 us per swap on the 4-Kirby module in
node), STARTs kept out of the session.  `PortLnOverlayObserve` for tests.

Until the fork's pause has begun (a START it cannot pause on: a door, a
cutscene -- refused after 24 frames) the live picture stays; the overlay
ends as the pause screens go (the white before the game fades back), and
the live game is shown from there.

Checked: `tools/netproof/pause_test.mjs` (full_check suite `rig-pause`; both modules: guests and the host
pausing alone and two at once, slots 4 and 5 on the 16-Kirby module, pages
and cursors moved only by their own pads while others mash, rollbacks every
37 frames and a repair image under an open overlay, sealed keys idle while
up, no START ever sealed, every final frame identical); the browser cases
`exits`, `three`, `rooms`, `eight` rewritten for per-device pause; the
rig's pause bot reads the overlay; regress and layout-neutral against main
unchanged (sim column identical).

**Two faults found live (e7f8a33), fixed.**  *A guest's page aborted
(OOM) on START* on the 16-Kirby module: the overlay took its two buffers
(2 x ~940 KiB) on the first START, and a FREE ROAM guest there holds 16
keyframes, the canonical shadow's two buffers and the images -- 2.2 MiB was
left of the 192 MiB heap, and emscripten's malloc aborts the page when the
fixed heap cannot hold a block.  Now the buffers are taken as the session
starts; every session allocation goes through `LnAlloc`, which refuses
(NULL, logged) what would leave less than 2 MiB free, and every caller
treats that as a refusal (no overlay: START opens nothing and is counted,
`ovlNoMemory`); and the wide modules' heap is 200 MiB (Makefile, only with
`KATAM_MAX_KIRBYS`; the 4-Kirby module stays 192, where the same guest keeps
~10 MiB free).  Measured in node (`pause_test.mjs memory`): katam16 FREE ROAM
guest 10.2 MiB free at its lowest, host 27 MiB; `PortLnHeapInfo` and the
log line "pause overlay ready; heap free ..." report it on every device.
*The overlay's pause screens were garbled with the GPU renderer* (help
page: a repeated tile pattern), on both modules, with or without anyone
else: the quiet live frame's scanline loop overwrote the render list's
per-line register table (`PortRlCaptureLine`) after the fork's frame had
been built, so the fork's tiles were drawn with the live game's BG control
and scroll.  The quiet frame (and a fork frame not shown) no longer
captures lines.  The node rig composes with the PPU and never saw it; the
browser cases now count the colours of the pause page's GPU picture
(`coloursIn`: the garbled page had 6, the real ones 14-25).

# Sixteen seats (branch `six-sixteen`)

A room hosted on the 16-Kirby module (the page's default) holds 16 people
on up to 16 devices; `?mod=4` is unchanged (4 seats, 192 MiB, 24
keyframes).  Nothing in the engine or the wire was eight wide: the engine's
devices are `PORT_LN_MAX_DEV` = 16 there, the per-frame seat mask is a u64,
the sealed record's mask bytes cover slots 4..19, and paint, names, pause
copies and the party HUD were already `PORT_LN_SLOTS` wide.  What changed:

- `web/online.js seats()` is 16 on the wide module (min(16, `PortLnSlots`)),
  so HostHub's `seats`, `PortLnSetSeatSlots`, the device ids (1..15), the
  join refusal text, the public listing's `max` (the worker accepts 4..16)
  and the status line follow; `noOwners()` is 16 wide.  The invite link's
  `mod=16` is unchanged.
- `platform/online_ui.c PortOuiSeats()` is 16 (HOST GAME's PLAYERS n/16,
  the counts in words up to SIXTEEN); ONLINE's fourth row on the 4-Kirby
  module reads HOST 16 PLAYERS ("UP TO SIXTEEN - THE PAGE RELOADS"), and
  MULTIPLAYER's ONLINE row on the wide module PLAY WITH UP TO SIXTEEN ONLINE.
  All fallback-font / description-cut text composed at run time, as before.
- Memory: the wide builds' heap is 256 MiB (Makefile `INITIAL_MEMORY`, was
  200) and a wide page keeps 32 keyframes (`KF_COUNT_WIDE`: 480 frames of
  history, ~30 MiB; was 16 / 240 frames), so a guest's lead cap is
  15 x 30 - 10 = 440 frames.  The browser `eight` case measures ~50 MiB free
  on every guest after the session's buffers (was ~10).  The rig's default
  `--kf-count` on the 16-Kirby module is 32 to match.
- Packets: a record with all 16 pads held is 1 + 2 + 32 (+ 8 hash) = 43 B,
  so `maxPacketBytes` 1100 still carries ~25 records a packet and `copies: 4`
  (four records' worth in steady state, ~170 B) is unchanged.
- Colours: `PortWideDefaultColor` already cycles the sprays Kirbys 1..4 do not
  wear: Kirbys 9..14 are ORANGE, SAPPHIRE, EMERALD, CHOCOLATE, CHERRY, CHALK,
  and 15, 16 wrap to SNOW, CARBON (the game has 14 sprays; four are worn by
  Kirbys 1..4).  A player card's paint goes on top, as for 5..8.

Checked: `tools/netproof/sixteen_test.mjs` (host + 15 guests on the 16-Kirby
node module, guests 8..15 joining mid-game into seats past the live count,
32 keyframes, copies 4 / 1100 B, 60 ms +-15, 2% loss; TOGETHER and FREE
ROAM): 16 people, the count rises to 16, every guest's every compared frame
identical.  Guests replay ~800 frames/s there (about 15% of a core at
190 us a frame).
In the browser, `online_browser_test.mjs --cases eight --eight-tabs 12`
(12 tabs on the 16-Kirby page: 11 people at the start, the 12th joining
mid-game into a Kirby made for it, CPU KIRBYS 13): 108 ok, 0 failed; every
guest's compared frames identical, largest sealed packet 607 B.

**Fit and finish for 9..16 (same branch, later).**  A guest stages up to
255 sealed events a frame in the wide build (was 16: at a 16-player
session's first frame the settings copies and names of everyone with a card
are more, and the guest dropped the rest -- an RG_HOST mismatch and a
repair image on every guest; the 4-Kirby build keeps 16).  The pause maps
mark every Kirby (ONE-WORLD-16.md, the markers row).  HOST GAME's START row
says "n OF 16" and its count description follows the players.  The pause
ONLINE list has a row per slot, and REMOVE works on any of them.
`tools/smoke.sh` (the one-minute check; `tools/full_check.sh --smoke`) runs
a 10-tab browser session with cards on the default module, through the
host's ONLINE list to the last player's row and REMOVE.  Left as it is, on
purpose: the engine's telemetry lists slots 4..7's owners only, and
`--smoke-tabs 16` (61 s alone) is not the smoke default.
