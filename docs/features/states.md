# Save states, rewind, fast-forward

Single-player conveniences in the style of other GBA emulators. They only work
on your own device: while you are in an online room (or any session) every one
of them is switched off, so they can't cause a desync or give anyone an
advantage.

## Use

| What | Keyboard | Touch | Menu |
|---|---|---|---|
| Save state to slot n | Shift+F1 … Shift+F8 | SAVE (current slot) | SETTINGS > GAME > SAVE STATES > SAVE STATE |
| Load state from slot n | F1 … F8 | LOAD (current slot) | … > LOAD STATE |
| Rewind (hold) | hold `` ` `` (backquote) | hold ◀◀ | … > REWIND on/off |
| Fast-forward (hold) | hold Tab | hold ▶▶ | … > FAST SPEED 2X/3X/4X/8X |
| Slow motion (hold) | hold Shift+Tab | | … > SLOW SPEED HALF/THIRD/QUARTER |

- Slot 9 has no hotkey, because F9 and F10 are screenshot and clip
  (web/capture.js). Use the menu's SLOT row for it.
- The touch buttons sit in a small row that appears with the touch pads.
- SLOT in the menu picks which slot the touch buttons and the menu's
  SAVE/LOAD use. A save or load chosen in the menu happens as the SETTINGS
  screen closes.
- With `?dev=1`, F1 and Tab belong to the developer overlay, so slot 1 and
  fast-forward have no hotkey there.
- Rewind keeps about 15 seconds of play and steps back at 2× speed. When you
  let go, the game carries on from that point.
- Fast-forward runs n frames for each screen refresh. On a 120 Hz screen it is
  twice as fast as on a 60 Hz one.

## What a state holds

A state is the rollback engine's snapshot (`PortRbSnapshotSave`): the whole
GBA memory the game writes plus the port's sim block, DMA and PPU latches,
compressed with zstd to about 50 KiB. It is kept in this browser (IndexedDB
`katam-states`; the speed and rewind settings are in localStorage
`katam.states`).

- **The save file is not part of a state.** Loading a state never undoes or
  overwrites an in-game save.
- **A state only loads in the build that saved it.** The header carries a
  fingerprint of the build's memory layout. A state saved by another build
  says "STATE IS FROM ANOTHER VERSION" and is not loaded, because the game
  memory contains function-table indices that change between builds.
- Some state outside the game, like the party HUD's card animation and the
  CPU Kirbys' host-side AI, is not restored. The game itself resumes exactly.

## Code

- `platform/states.c`, `platform/port/states.h`: the engine. Slots, the rewind
  ring (a backward chain of zstd-compressed XOR deltas, one every 2 frames,
  at most 16 MiB) and the online guard (`PortStAllowed`). It runs from
  `PortStFrame` at the top of `PortPresentFrame` (platform/main.c), the same
  point the rollback engine restores at. It does nothing while the SETTINGS
  screen is up.
- `platform/states_menu.c`: the SAVE STATES rows (`gPocStateRows`). They are
  registered in platform/menu_poc.c as an `EXT_SECTION` inside GAME.
- `web/states.js`: the keys, the touch row, the on-screen notices, and the
  IndexedDB/localStorage storage. `Module.portStSpeed` sets the pacing in
  `PortWebAwaitFrame` (platform/web/host_web.c).
- Recording is off until the page turns it on, so native builds, the headless
  harness and the goldens are untouched.
- Test: `node tools/states_test.mjs build/katam-node.js <rom>` (~8 s; in smoke
  and in full_check). It checks that a loaded state repeats the original frames
  exactly, and that rewind goes back through frames identical to the
  originals.
