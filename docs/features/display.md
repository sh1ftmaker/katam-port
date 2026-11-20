# Display polish and capture

Picture options and screenshots/clips, all presentation-only: nothing here
reaches the simulation, the render list, a hash or the online session.

## What it does

**SETTINGS > DISPLAY**

| Row | Values | Notes |
|---|---|---|
| PIXELS | SHARP (integer), FIT, SMOOTH (sharp bilinear), **STRETCH** | STRETCH fills the whole window, shape ignored; drawn sharp-bilinear |
| **FILTER** | OFF (default), GBA LCD, CRT, SHARP | post-process on the picture, this device only |
| FULLSCREEN | (web) | unchanged, already there |
| **SCREENSHOT** | (web) | PNG, taken as the menu closes |
| **RECORD CLIP** | (web) | WebM (MP4 on Safari), starts as the menu closes; pick again to stop; shows REC |

Keys on the web page: **F9** screenshot now, **F10** start/stop a clip
(at most 20 s, longer side at most 960 px, with the game's sound when it is
playing). Files download as `katam-YYYYMMDD-HHMMSS.png/.webm`. The dev
overlay (?dev=1) has the same scaling and filter radio buttons.

Filters:
- **GBA LCD**: the GBA screen's colour response (linear-light 3x3 matrix,
  rows sum to 1) and a faint grid between game pixels.
- **CRT**: light scanlines, one per game row.
- **SHARP**: sharp bilinear sampling only (crisp at any fractional scale).

The grid and scanlines follow game pixels and fade out below ~3 / ~2 screen
pixels per game pixel, where they would only moire. Cost: one full-picture
pass with four texel fetches a pixel; OFF takes the old blit path untouched.
The game HUD layer (when drawn separately) and the party HUD sit on top
unfiltered. The 2D fallback (no WebGL2) has no filters.

Online: the filter row and the capture rows are this device's own (not in
the session's settings word); F9/F10 work anywhere.

## Where the code is

- `platform/render/screen_fx.c/.h` the filter shader; `GlrPresentRect`
  (`gl_renderer.c`) calls it instead of the blit when `gGlrScreenFx` is set.
- `platform/ui/settings.[ch]` `filter`, `PS_FX_*`, `PS_SCALE_STRETCH`,
  `PS_SCALE_SOFT`; the stretch layout in `PortLayoutFit/Compute`.
- `platform/ui/present.c` sets the filter each draw and tells the page the
  picture rectangle (`Module.portPicRect`) for cropping captures.
- `platform/menu_poc.c` the FILTER / SCREENSHOT / RECORD CLIP rows
  (`IT_FILTER`, `IT_SHOT`, `IT_CLIP`; actions `capture.shot`, `capture.clip`,
  query `capture.rec`).
- `web/capture.js` screenshots and clips; hooked from `web/shell.html`
  `present()` (same task as the GL draw, so the canvas is readable) and its
  action/query tables. "Menu closed" = no SETTINGS screen draw
  (`_PortPocScreenLastDraw`) for 20 frames.
- Test: `make display-test` (tests/display_settings_test.c, also in
  smoke/full_check).
