# The GPU renderer (first cut)

`--renderer gpu` (native) / `?renderer=gpu` (web) draws the game with OpenGL
ES 3.0 / WebGL2 / GL 3.3 core from a **render list** built from game state.
The emulated PPU (`platform/ppu.c`) stays the default; `--renderer both` runs
the two and shows the GPU picture, for comparison.

    katam --renderer gpu --gpu-view 400x224 rom.gba      # fixed world viewport
    katam --renderer gpu --gpu-view auto rom.gba         # follows the window's aspect, 160 tall
    katam --renderer both --compare-log cmp.txt ...      # per-frame GPU vs PPU pixel diff
    katam --renderer gpu --shot-at 1700:out/f1700 ...    # out/f1700-gpu.png (+ -ppu.png with both)
    katam.html?renderer=gpu&view=400x224                 # web; without view= the stage's aspect

## Why GLES 3.0 / WebGL2

One code path for every target: WebGL2 *is* GLES 3.0, Mesa/NVIDIA/AMD give
GLES 3 on Linux, desktop 3.3 core on Windows/macOS takes the same shaders with
a different `#version` line. Integer textures (`R8UI`, `R16UI`, `RGBA32I`) and
`texelFetch` let the shader do exact GBA tile/palette arithmetic. No loader
library: `platform/render/gl_api.h` lists the ~55 entry points and resolves
them through `SDL_GL_GetProcAddress` (native) or links them (emscripten).
Headless: SDL's `offscreen` driver gives an EGL context (NVIDIA, radeonsi or
llvmpipe here); the web build was checked in a headless Chromium with
SwiftShader WebGL2.

## Layout

| file | role |
|---|---|
| `platform/render/render_list.h` | **the contract**: layers, sprites, HUD regions, per-line effect state, texture-cache sources |
| `platform/render/rl_build.c` | builds it: game hooks (linker wraps) and per-frame assembly; never writes game memory |
| `platform/render/gl_renderer.c` | the GL backend; knows only the render list; replaceable |
| `platform/native/host_sdl.c`, `platform/web/host_web.c` | context creation, present, screenshots |

### The render list

* **Spaces.** `SCREEN` (GBA screen coords; menus and anything unproven),
  `WORLD` (screen coords relative to the game camera but unbounded: layers
  have content at any x, sprites carry full s32 positions), `HUD` (anchored
  to a corner of the view, per region).
* **Layers** (`RlTileLayer`, one per hardware BG): space, parallax factor and
  the resulting shift, and for a streamed room layer the room's real tilemap
  (EWRAM, `Background.unk10`) plus the screen-space window in which VRAM holds
  the game's own copy (VRAM wins there: it is what the game last wrote,
  including blocks changed in place). Outside the window the map is sampled;
  parallax pictures wrap, mirror or are re-fitted (below).
* **Sprites** (`RlSprite`), front to back: OBJ attributes with the position
  fields zero, s32 x/y, affine matrix, space, HUD anchor. Not limited to 128.
* **HUD regions**: four rectangles of the 240x160 HUD, each pinned to its
  corner; x=160 splits the bottom strip between the health bar (cols 13-19)
  and the area name / enemy health (cols 20-29), so nothing tears.
* **Effects**: a per-line register table captured as the emulated display
  scans (scroll, affine reference points, DISPCNT, BLD*, WIN*, MOSAIC,
  BGxCNT), so HBlank effects are in it.
* **Texture-cache sources**: VRAM (with off-screen sprites' animation frames
  patched in from ROM, see below) and palette RAM. The backend uploads VRAM
  by dirty 256-byte rows, palette and line table every frame, room maps
  when they change.

M9 (MODDING.md) replaces the VRAM/palette references (`RlTileLayer.map`,
`RlSprite.attr2`'s tile and palette) with resource ids; spaces, anchors,
order and the effect description stay.

### Where the data comes from

* **Sprites**: `-Wl,--wrap=DisplaySprite,--wrap=sub_081564D8` call
  `PortRlEmitSprite` first, which repeats the game's attribute arithmetic
  without side effects, keeping positions before the 9-bit mask and before
  the 240x160 cull and the 128-entry pool. `--wrap=DrawToOamBuffer` commits
  the list in DrawToOamBuffer's own order (stable by link layer) and
  snapshots the camera and Background records -- the frame the next present
  shows. Every frame the committed sprites the game *did* write are checked
  against real OAM, entry by entry; on any mismatch that frame uses OAM only
  (1 frame in 20950 over the four recordings). OAM entries the hooks do not
  account for (a frozen pause frame) are appended from OAM.
  The game is not edited: wraps keep its machine code byte-identical, which
  keeps the regression hash's layout artifacts out of the way (below). HUD
  sprites are recognised by address (they live in the HUD task's struct).
  Off-screen "full" sprites never get their per-frame tiles uploaded by the
  game; the list patches them into its own VRAM copy from the ROM tables so
  they animate in the margin. (Apple's ld has no `--wrap`: `PORT_RL_NO_WRAP`
  falls back to OAM sprites.)
* **Layers/camera**: committed at the same moment, validated at present
  against the live BGxCNT/scroll registers (screen base, char base, low
  scroll bits); a record that does not match leaves that BG as a SCREEN layer.

## What the GPU draws vs the PPU

At 240x160 the GPU draws *everything* the PPU does -- text and affine BGs,
regular/affine/double-size sprites, 1D/2D mapping, priorities, WIN0/WIN1/
OBJWIN, alpha blending (BLDCNT and semi-transparent OBJ), brightness up/down,
vertical BG mosaic, forced blank, per-line HBlank effects -- with the same
5-bit integer arithmetic. **Measured: 0 differing pixels on every frame of
all four recordings** (attract 6600, boot_to_level 1400, play_rooms 7300,
warp_sweep 5650 frames; NVIDIA GLES 3.2 and Mesa radeonsi), and on the 407
native-framed frames of a 400x224 play_rooms run. No screen falls back to the
PPU. Not implemented in either: OBJ mosaic, bitmap modes (unused).

## Widescreen

Gameplay frames ("world": the room record validates and the room layer is a
streamed text BG) use the requested view; everything else (title, menus,
pause, transitions without a room) is drawn at 240x160 and integer-scaled.

* **Camera.** Centred on the game's camera, then clamped -- in the renderer,
  the game's camera is untouched -- to the game's own camera rectangle
  (`unk_S32Vec2_6C/74`) widened by the 8-px margins it insets from the map on
  top/left/right (not the bottom 40, never shown and empty in room 101), and
  never past the map. A scroll lock (a rectangle narrower than the room)
  keeps hiding what it hides. If the room is smaller than the view the rest
  is **letterboxed black**; the HUD is drawn over the bars. Chosen over
  showing the backdrop (the old purple void) or the map's far end (the
  wrap): it never shows content the game did not author for that spot.
* **Streamed windows** end where the map ends (bg.c reads two rows past it).
* **Parallax.** A picture drawn to fit the room (its 240-px window stays
  inside it over the camera range) is re-fitted: the wider window slides
  over the same range of the picture. One that runs off its own end at 240
  tiles and wraps. One narrower than the view is mirrored. Vertical edges
  repeat the edge row (visible as streaks only where a room is taller than
  its sky, e.g. bottom of room 202 at 400x224).
* **HUD**: four corner regions, BG1 and HUD sprites both; see below for
  its size.

### HUD layer

The HUD lives in GBA screen space, 240x160, and the world view does not:
zoomed in, the view is narrower and shorter than the HUD, so drawn in view
pixels its regions overlap and the bottom strip leaves the picture; zoomed
out, it shrinks with the world. So the HUD has a size of its own, tied to
the window: **Auto** is the scale the world has at zoom 1 in this window
(`PortLayoutHudScale`; the scripted path uses the scale a 240x160 picture
would get), or the player's fixed 1x-8x; either way capped so the whole
240x160 HUD fits inside the world picture (a portrait phone, Fill window).

When that scale is the world's and the view is at least 240x160 -- zoom 1,
Auto, any aspect -- the HUD stays in the composite exactly as before (the
corner offsets are whole view pixels), which keeps the 240x160 picture
identical to the PPU's. Otherwise (`GlrSetHudLayer`):

* The sprite and composite passes leave the HUD out (the layer in
  `RL_SPACE_HUD`, the sprites in it) and run again for the HUD alone into a
  HUD atlas: one slot per region, at HUD coordinates with a 32-px gutter for
  HUD sprites that hang over their region's edge (a sprite belongs to the
  region holding its centre, as before, so nothing tears at x=160 or 120).
  Same priority, window and per-line colour-effect rules, evaluated on the
  HUD's own lines: a fade darkens or brightens the HUD exactly when it does
  on the GBA. Where alpha blending has the world as its second target the
  HUD pixel is written premultiplied (eva/16 of it, 1 - evb/16 of the world)
  and blended at present -- the one place this is an approximation (no
  5-bit rounding); a world layer the GBA would have drawn *over* the HUD by
  priority is under it here.
* `GlrPresentHud` draws each slot over the world picture, pinned to its
  corner (or centred, original strip), nearest or -- with Smooth scaling and
  a fractional scale -- sharp bilinear.

Non-world frames (title, menus, pause) have no HUD regions and are the
240x160 picture as always. Checked: forcing the layer at 1:1
(`KATAM_HUD_LAYER=1`, window 240x160) gives the same window, pixel for
pixel, as the in-composite HUD on every 15th frame of play_rooms and
warp_sweep (956 frames, fades included). `--shot-at F:PREFIX` also writes
`PREFIX-win.png`, the presented window, which is the picture that has the
HUD layer in it (`PREFIX-gpu.png` is the composite only).

### Object pop-in

The game spawns objects 48 px (3 cells) past the 240x160 screen and destroys
them past a 48-px margin. Those windows now follow the GPU view as a recorded
simulation input (docs/VIEW.md, "Spawn margin"; the numbers below are from
before that, with the stock windows). `KATAM_SPAWNSTAT=WxH` counts spawns and
despawns inside a view, which is the measure that matters now (0 scroll-in
spawns in view at 746x320, against 32 before). `KATAM_RL_POPSTAT=1` counts
sprites appearing/vanishing inside the view but outside 240x160 (room entries
excluded), animation and emitted effects included:

| view | play_rooms | warp_sweep | attract |
|---|---|---|---|
| 320x180 | 174 / 6938 frames (110 appear, 86 vanish) | 165 / 4739 | 174 / 6538 |
| 400x224 | 262 / 6938 (162, 160) | 316 / 4739 | 176 / 6538 |
| 480x270 | 354 / 6938 (211, 216) | 407 / 4739 | 188 / 6538 |

So ~2.5% of gameplay frames at 320 wide and ~4-7% at 400-480 show an object
popping in or out in the margin; most appearances are 0-48 px past the edge
(the spawn window and off-screen projectiles), 48+ px only from 400 up.

## Proving the simulation unchanged

The hooks only read game memory. Checked with the coordinator's gate
(`tests/layout_neutral.sh`, `tools/layout_diff.py` from the resources
branch): against the parent build, keys/vid/sram/pix match on every frame of
all four recordings with the renderer off, and the `sim` differences are the
same host pointers at other addresses (the port's static data moved because
the image grew). Goldens re-recorded; only `sim` moved. With the renderer on
(`gpu` at 400x224 and `both`), the same binary's keys/sim/vid/sram match a
renderer-off run on every frame. `KATAM_VIDEODRIVER=offscreen
KATAM_REGRESS_ARGS="--renderer gpu" tests/regress.sh` runs regress with it on.

The renderer-only SDL entry points are declared `noplt` so the PLT does not
grow and move the game's code (the sim hash canonicalises function pointers
by exact address; see the coordinator's note on layout sensitivity).

Unrelated finding: `PortSetRenderEnabled(0)` (used by rollback resim) makes
`PortPresentFrame` skip `PortRenderFrame` entirely, i.e. the HBlank DMA and
HBlank/VCount interrupts, although its comment says only composition is
skipped. The GPU path does not use it (`gPortPpuCompose` instead).

## Display rate, scaling

The window is resizable; the view (game pixels) is independent of it and is
integer-scaled (nearest) into it, fractional only below 1x. With `--vsync`
the GPU path presents at the display's rate, re-showing the latest sim frame
until the next 59.73 Hz sim frame is due (`PaceFrame`).

## Next

* **Interpolation** (S-M): keep the previous and current RlFrame; present
  with alpha = time since the sim frame / period. Camera: interpolate the
  view origin (needs layer sampling in sub-pixel/float and a slightly larger
  composite, ~1 day). Sprites: match by owner (`struct Sprite` address, as
  the pop-in census already does) and lerp positions; teleports and
  animation-frame changes snap.
* **Effects** (S each): OBJ mosaic; windows as world-space shapes instead of
  the 240-wide edge heuristic; HBlank tables beyond line 160 synthesised
  rather than clamped; a parallax layer taller than its picture.
* **Block-mapped layers** (`Background.unk2E & 0x40`, a table of tile
  blocks): today window-only in wide views; flatten on CPU into the atlas (S).
* **Asset-by-id (M9, L)**: replace `map`/tile/palette references with
  resource ids; tiles as atlas textures, the composite unchanged in shape.
* **Per-element HUD anchors** (S): regions from hud.c's own element list.
  The HUD layer's atlas takes any number of regions up to 8 unchanged.
* **Culling**: done. The spawn/despawn windows follow the view, as a
  recorded input (docs/VIEW.md, "Spawn margin"). Netplay holds them at
  240x160 until v2 agrees on a session view.
