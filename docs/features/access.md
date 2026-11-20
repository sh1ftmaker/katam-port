# ACCESSIBILITY (SETTINGS)

A top-level SETTINGS section, between GOALS and EXPERIMENTAL. It is in
the front end's SETTINGS and the pause page's. Every option is presentation,
or this device's own buttons before the latch takes them. Nothing is written
to game memory, so the simulation hashes and the goldens stay the same.

| Row | Values | What it does |
|---|---|---|
| KIRBY MARKERS | OFF / ON | Puts a shape before each Kirby's tag on the party HUD cards, on the "1P" tag and on the name tags. The shapes are circle, square, triangle, diamond, plus, cross, heart and ring, chosen by slot. Colour is then not the only way to tell the Kirbys apart. |
| HIGH CONTRAST | OFF / ON | Other players' tags and name tags are white on black, and the card names are white. On the web, the touch pads are also opaque black with white labels and a white border. |
| FLASHING | FULL / REDUCED | REDUCED lets a brightness-up blend (BLDCNT effect 2: white flashes and fades to white) rise only one step in 16 per frame. A flash of 1–2 frames barely shows, and a slow fade still reaches white. Works in both the software and the GPU renderer. |
| SCREEN SHAKE | FULL / LESS / OFF | Halves or removes the room layer's camera shake. Only the ENHANCED graphics renderer does this (`rl_build.c` shakeD). CLASSIC keeps the game's own shake. |
| HOLD B | HOLD / TOGGLE | With TOGGLE, if you hold B for at least 0.35 s and let go, B stays held (for example, Kirby keeps inhaling). Your next B press releases it, and that press does not count as a new press. Taps are not changed. This applies to every local player (keyboard, pad, touch). |
| TOUCH SIZE | 100 / 125 / 150 / 175 % | Web only. Scales the on-screen pads (`--ts` in `web/shell.html`), with caps so they still fit in portrait. |

## Defaults and saving

The options are saved as one word, `gPortSettings.access`, written as
`access=0x..` in `katam.settings`. Older builds wrote no key; when the key is
missing, the defaults below apply:

- On the web, `prefers-reduced-motion: reduce` turns on reduced flashing
  and sets shake to OFF.
- On the web, `prefers-contrast: more` turns on high contrast and the
  markers.
- Natively (and under node), everything starts off.

DEFAULTS in ADVANCED resets the options to the same values.

## Online

The rows are greyed out during an online session, like other device-only
rows that have no online slot. The values set before joining still apply,
because they only affect this device's picture and buttons. A held B is
ordinary input, sent like any other press, so it cannot desync or cheat.

## Code

- `platform/access.c` and `platform/port/access.h` hold the rows
  (`gPocAccessRows`), the hold toggle, the flash limiter, the shake factor,
  the markers and the defaults.
- Hooks:
  - `menu_poc.c`: the section, `kExtRows[4]`, model 24.
  - `ui/settings.[ch]`: the `access` field and its key.
  - `main.c` PortSetKeys and `localmp.c` PortLmpSetHostKeys: HOLD B.
  - `ppu.c` and `render/rl_build.c`: FLASHING and SHAKE.
  - `ui/party_hud.c`: the marker glyphs and contrast.
  - `web/shell.html` applyAccess: the touch size and the contrast of the pads.
- Test: `make -s access-test` (`tests/access_native_test.c`, under a
  second). It is registered as the `access` suite in smoke and in the full
  check. The menu section is also covered by `tools/menu_exits_test.mjs`
  (`SECTIONS` includes ACCESS).
