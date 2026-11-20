# Input remapping and rumble (SETTINGS > PLAYERS > CONTROLS)

Every GBA button (A, B, L, R, START, SELECT, the four directions) and two of
the page's own actions (FULLSCREEN, MUTE) can be rebound for each keyboard
set and each gamepad. Each device's layout is saved in this browser. Gamepads
can rumble when your Kirby is hurt or gets an ability.

## Use

SETTINGS > PLAYERS > CONTROLS (web page only):

| Row | Left / Right / A |
|---|---|
| CONTROLLER | which device the rows edit: KEYBOARD (set A: WASD, J/K...), KEYS 2 (set B: arrows, numpad), PAD n + its family (XBOX, PLAYSTATION, SWITCH PRO, GAMEPAD) |
| A ... RIGHT, FULLSCREEN, MUTE | A: press to bind. The value turns to PRESS A KEY / PRESS A BUTTON, and the next new press on that device becomes the action's only input. Escape (keyboard) or 6 s with no press cancels it. Left puts that one action back to its default. |
| RUMBLE | ON / OFF (all pads) |
| RESET CONTROLLER | this device back to its defaults |

- **Conflicts:** an input that is on two actions shows `!` before the value, and
  the description names the other action (`ALSO ON A`). It still works (both
  buttons are pressed at once). This is a warning, not a block.
- After a bind, the game gets no input until every key and button is let go,
  so the press that bound the button doesn't also count as a menu press.
- **Defaults:** keyboard sets as before. Pads use the browser's standard
  mapping, with button names for Xbox, PlayStation and Switch Pro. Xbox,
  PlayStation and other pads keep bottom/right = A and left/top = B. The
  Switch Pro uses Nintendo's layout: its A (right) is GBA A and its B
  (bottom) is GBA B. Pads without a standard mapping keep the old
  fallback (hats and the 8-button START/SELECT order) through the `dU/dD/dL/dR`
  d-pad inputs.
- **Rumble** uses `Gamepad.vibrationActuator.playEffect` (Chromium, Safari)
  or `hapticActuators[0].pulse` (Firefox). There is a strong 220 ms pulse when
  a player's Kirby loses health and a light 110 ms one when it gets a new
  ability, but only while the HUD is up. If the browser or pad has neither
  API, there is no rumble. Online, rumble covers this device's own player only.
- **Online:** the masks still reach the game exactly as before, so a remap
  can't desync anything and gives no advantage. The CONTROLS rows are dimmed
  during an online game because the menu is drawn into game memory. Bindings
  set beforehand still apply.

## Storage

`localStorage["katam.remap"]` holds
`{v:1, kb:{a,b}, pads:{<Gamepad.id>: profile}, rumble}`. A profile is
`{ACTION: [input...]}`. Keyboard inputs are `KeyboardEvent.code`. Pad
inputs are `b<n>` (button), `a<n>+`/`a<n>-` (axis) and `dU dD dL dR` (the
d-pad as `input_devices.js` reads it). A device with no profile saved uses
its defaults.

## Code

- `web/input_remap.js`: profiles, defaults, names, press to bind,
  conflicts, persistence, rumble, and the menu bridge
  (`remap.*` UI queries and the `remap.step` action).
- `web/input_devices.js`: if `devs.remap` is set, it builds key and pad
  masks from the remap and mutes the game while a bind is waiting or
  settling. Without it, nothing changes.
- `platform/controls_ui.c`: the menu section (`gPocControlsRows`, 15
  rows, the same numbers as `KatamRemap.ROW`) and `PortRumbleProbe`
  (reads Kirby health and ability, writes nothing).
- `platform/menu_poc.c`: `IT_SEC_CONTROLS` in PLAYERS (web), and
  `kSecControls` as ext section 1.
- `web/shell.html`: loads the script, `KatamRemap.install`, the rumble
  watch in `inputFrame`, and `KatamRemap.hookMenu` after `portUiAction`.
- Test: `tools/input_remap_test.mjs`, which runs in `make input-devices-test`
  (part of smoke).

Native builds are not covered: the host answers no UI queries, so the rows
read WEB PAGE ONLY.
