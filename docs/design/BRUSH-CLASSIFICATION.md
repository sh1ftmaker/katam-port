# Classifying brush pieces: the method

Status: proven by hand on three brushes (sky garden Clouds, tile set 29;
Rainbow Route Grassy soil, tile set 3; Olive Ocean Grassy dirt, tile set 4).
No classifier tool exists yet.
Written for whoever continues.

## The problem

Terrain brushes (`tools/katam/lib/autotile.mjs`) learn a "style" per material
from the original rooms: the body pieces that sit together form a community,
and the edge pieces lie on the body they touch. This is by example, so it
needs no hand-drawn 16/47-shape tile set. Its weak point is membership:
anything that sits against a material gets pulled into that material's brush.
The sky garden Ground 1 brush held clouds plus a steel block and green pillars.

What a good brush must hold, per material:

- **Open-air edges:** a repeatable flat top, left and right wall, and ceiling.
- **Corners:** outer corners, and concave corners (a one-block notch).
- **Infill:** fill that repeats in a pattern.
- Nothing that is a different material or a one-off object.

## The method (two separate questions)

1. **Shape role: no AI.** What a piece is (fill, floor/wall/ceiling edge,
   outer corner, concave corner, one-wide part, slope) is already in the
   collision around every place the piece appears in the original rooms.
   Count roles per piece and keep the dominant one with a confidence. This
   is not built yet; `learn()` only uses shapes implicitly.
2. **Material membership: needs the picture.** Whether a piece is cloud,
   steel or pillar is not in the collision. Decide it by looking at the piece
   (a person, or a small vision model such as Haiku), then record the result
   as data. A cheap pixel statistic (share of near-white pixels) finds most
   intruders but wrongly flags mostly-transparent real edge pieces, so it can
   only propose, never decide.

Rules that keep it safe and reviewable:

- **Only piece tokens and labels are committed**, never pictures or ROM bytes.
  Presets are a list of tokens such as `01a.5 01b.5 03a.5 03b.5`.
- **A human looks at every committed classification.** Use a contact sheet:
  the style's pieces on a grid, sorted by use count, magenta for transparent.
- **Exclude, don't delete.** `exclude` in a preset takes a piece out of every
  brush (`learn()` treats it like the hidden pieces), as ground and as the edge
  art drawn in the air beside ground. It stays usable as a single piece in the
  editor. Presets also name styles (`name`).
- **Exclude from one brush when another owns the piece.** A style in a preset
  can carry its own `exclude`: those pieces leave that brush only (its sources
  and the edge art it brings along), and the brush that rightly holds them
  keeps them. A brush lacking a shape (a wall, an underside) borrows that
  shape from the other brushes of its kind; `"borrow": false` on the style
  keeps it to its own nearest shapes instead.
- **Look in place before deciding.** A piece on a sheet can be half of
  something: a cone shell cut into four pieces, the foot of a palm trunk. The
  brush copies pieces one by one, so a many-piece object comes out cut. Judge
  each suspect in the original rooms (`--zoom`, below).
- **Re-run and diff after every change.** Excluding pieces changes the
  community structure; two real cloud-edge pieces left the brush as a side
  effect. Compare the piece lists before and after and look at what moved,
  and check the tile set's other brushes did not change.

## What exists

- `assets/schema/authoring/autotile/tileset-vanilla-29.json`: the first
  worked example (7 pieces excluded, brush named Clouds).
- `assets/schema/authoring/autotile/tileset-vanilla-3.json`: the second,
  below (Grassy soil, 23 pieces excluded from that brush only, no borrowing;
  one piece excluded tile-set wide).
- `assets/schema/authoring/autotile/tileset-vanilla-4.json`: the third,
  below (Olive Ocean's Grassy dirt, 13 pieces excluded).
- `tools/katam/lib/brushtest.mjs`: the test level (below), and the editor's
  **🧪 Test level** button.
- `tools/katam/dev/brush_sheet.mjs`: learns one tile set the way the editor
  does and draws what it learned. Once per ROM:

      build/lp64/katam --res-extract /tmp/van-pack ROM
      node tools/katam/katam.mjs decompile --rom ROM --pack /tmp/van-pack -o /tmp/van

  then

      node tools/katam/dev/brush_sheet.mjs /tmp/van tileset:vanilla/3 /tmp/sheets [--rules none] [--zoom solid-1:28,39]

  writes `styles.json` (each style's pieces: token, uses, shape role and the
  share of its uses in that role), `sheet-solid-1.png` (the style's pieces by
  role: FILL, TOP, CORNER TL, ... NOTCH UR, SLOPE; numbered; a red number is
  a piece whose role holds for under 60% of its uses), `paint-solid-1.png` (a
  test shape painted with the brush: step, notch, ledge, pillar, overhang,
  1:1 and gentle slopes), `test-solid-1.png` (the test level, below),
  `rim.png` (the edge art) and, with `--zoom`,
  `zoom.png` (those pieces in place in the original, 5x5 cells, up to three
  rooms; `--zoom rim:2,5` for edge art). `--rules none` shows what is learned
  without the preset. The pictures are the player's ROM art: the script
  refuses to write inside the repository.

## The test level

One room layout per brush kind, the same for every tile set
(`tools/katam/lib/brushtest.mjs`), so any brush can be painted on it and
compared with any other. The ground layout (75 x 26, the most a room holds)
contains every way a ground cell can sit among its eight neighbours -- the
47 "blob" shapes, corners counted only where both their sides are ground
(`blobOf` in `autotile.mjs`) -- in level-like pieces: loose blocks and bars,
a window of one-wide lines, crosses with filled quadrants, a block with a
hole one in from each side, a block with arms, a block, a pit, a pillar and
a wall with ledges on the floor; and the six slopes against flat ground,
walls, each other and a second storey (1:1 and gentle hills, a peak, a
valley, a slope into a wall, two-high hills, gentle into 1:1 and back). The
self-test checks that all 47 shapes are there. Platforms, ladders, water and
hazards get small layouts of runs, stacks and pools on a floor painted with
the tile set's first ground brush.

Painting a brush on it shows, per cell, where its piece came from: its own
piece of that shape, its own piece of the nearest shape it has (yellow), or
another brush's piece (red). Two ways in:

- **Editor**: select a terrain brush; in the Tiles tab, **🧪 Test level**
  makes a new room in the current look, painted with that brush, with the
  yellow and red marks (until a cell is repainted) and the sums in a toast.
  The room can be played.
- **`brush_sheet.mjs`** writes `test-solid-1.png` and so on, with the sums
  and the missing shapes in `styles.json`.

The ground brushes of the tile sets with presets (own shapes of 47, slopes of
6, and the 649 ground cells of the level: own / nearest / borrowed):

| Tile set | Brush | Shapes | Slopes | Own | Nearest | Borrowed |
|---|---|---|---|---|---|---|
| 0 | Grassy cobbles | 5 | 2 | 343 | 44 | 262 |
| 0 | Grassy cliff | 22 | 6 | 561 | 78 | 10 |
| 0 | Grassy hill | 25 | 6 | 563 | 78 | 8 |
| 2 | Grassy earth | 30 | 6 | 595 | 54 | 0 |
| 3 | Grassy soil | 5 | 6 | 404 | 245 | 0 |
| 3 | Log frame | 44 | 6 | 621 | 28 | 0 |
| 3 | Log ends | 23 | 0 | 519 | 59 | 71 |
| 4 | Grassy dirt | 28 | 6 | 584 | 65 | 0 |
| 5 | Sand | 25 | 4 | 574 | 75 | 0 |
| 15 | Mossy stones | 36 | 6 | 605 | 44 | 0 |
| 15 | Stone block | 14 | 0 | 467 | 182 | 0 |
| 17 | Pink moss | 40 | 6 | 619 | 30 | 0 |
| 17 | Stone block | 29 | 0 | 522 | 71 | 56 |
| 18 | Snowy rock | 37 | 6 | 582 | 67 | 0 |
| 28 | Clouds | 6 | 0 | 401 | 248 | 0 |
| 29 | Clouds | 15 | 6 | 532 | 95 | 22 |
| 29 | Ground 2 | 4 | 0 | 72 | 72 | 505 |

(brush_sheet's seed; the editor's differs, so its counts move by a few.)
Read with the pictures: a low shape count is expected of a material the
original only builds one way (soil inside logs, cobbles under grass) and is
not by itself a fault; red cells are where a brush draws another material.
Grassy cobbles takes its walls and undersides from the cliff and hill
brushes; tile set 29's Clouds takes the undersides of a raised shelf from
Ground 2, which is mostly other brushes' pieces itself.

## Worked example 2: Rainbow Route dirt (tile set 3)

Rainbow Route's rooms use tile sets 0, 2, 3, 5, 15, 17, 18 and 28 (room
numbers do not follow areas: 130-135 are Olive Ocean). Painting each one's
learned ground (`paint-solid-1.png`) shows the materials: 0 grassy rock,
cliff and cobbles, 2 rocky earth, 3 soil and logs, 5 sand, 15 and 17 mossy
cave stone, 18 snowy rock, 28 clouds. The dirt is tile set 3's Grassy soil
(the lumber yard, 115-119, 138-139, 171-178): grass over brown soil with
roots. Its brush (55 pieces) painted log ends, log pillars, vine-tied joints
and barrels.

The sheet by role shows why: in the original the soil always lies inside log
frames, so every wall, corner and underside the soil brush learned is a log.
Those logs belong to the Log frame and Log ends brushes (one is used 5 times
by the soil and 600 by Log frame), so a tile-set `exclude` would have gutted
them. Instead Grassy soil has its own `exclude` (23 pieces: the logs, a
slanted log and the cell under it, a barrel, and a blank piece the original
uses as a wall at a room's edge, all checked with `--zoom`) and
`"borrow": false`. With only `exclude`, the brush still drew logs, by two
routes: it borrowed walls from the log brushes, and it took the original's
neighbours of its sources (log undersides). `borrow: false` closes both; the
edge-art vote also skips the brush's excluded pieces.

Painting in the editor then showed grey posts above a floating ledge: the
foot of a background tree trunk, which the original stands on the grass, so
it counted as edge art. It is no brush's edge art, so it went in the tile
set's `exclude`.

Result: 32 pieces, all soil and grass (grass tops, fill, notches, slopes);
walls and undersides are the fill cut square; a one-wide pillar comes out as
stacked grass tops (the original has no one-wide soil). Log frame, Plank
bridge and the ladder are byte-identical, and so is every other preset tile
set (0, 2, 5, 15, 17, 18, 28, 29). Log ends changed in one place: its gentle
hill used to borrow the soil's grass slopes and now uses slanted logs (a
source beside a piece that is not edge art costs more, and the soil slopes it
borrowed stood beside the trunk foot).

## Worked example 3: Olive Ocean beach dirt (tile set 4)

Tile set 4 (Olive Ocean 130-135, Cabbage Cavern 191) is orange soil under
grass. It had no preset, and its one ground style (87 pieces, 3357 cells)
painted cut-off palm trunks, starfish and half cone shells. Unlike the
lumber yard, nothing here belongs to another brush, so the tile set's own
`exclude` does it. Suspects, judged in place with `--zoom`:

| Pieces | In place | Decision |
|---|---|---|
| a grey block (`05c.5 ...`) | a steel block of a bomb chain | exclude: another material |
| round shell, starfish | whole one-piece decorations, 70 uses each | exclude: objects (still single pieces) |
| four cone-shell pieces | one shell straddling cells, cut four ways | exclude: comes out cut |
| grey slopes, grey-backed ledge ends | cave rooms draw the rock wall into the piece | keep: in open air the brush takes the see-through ones |
| edge art: palm-trunk feet (2), signpost, mountain foot | the bottoms of tall objects and background | exclude |
| edge art: stems of a flowering palm (2) | the bottom of a plant whose leaves sit above | exclude: comes out cut |
| edge art: fern pieces | read as whole tufts when painted | keep |

Re-run and diff: the 7 excluded ground pieces left the style, plus two rare
real ones as a side effect (a cave ceiling, 2 uses; a wall beside a bridge,
1 use); 6 edge pieces left; nothing joined; the brush is named Grassy dirt.

## Next steps (in order)

1. Check the Clouds brush in the editor; look at the 2 edge pieces that moved
   out of it, and at "Ground 2" (7 pieces) of the same tile set.
2. Move the role tagger into the editor. `brush_sheet.mjs` has one (a
   piece's dominant role over every place it is used) and lays sheets out by
   role, so gaps ("no concave corner") show; `learn()` still uses shapes only
   implicitly.
3. Only then a membership classifier: render sheets, ask a vision model per
   piece with a short material list for that tile set, and score it against
   hand-labelled brushes (start with Clouds: 123 cloud, 7 not) and against
   the white-share baseline. Output the preset format above.
4. An Advanced editor panel to click a piece in or out of a brush and save
   the preset.
5. Roll out to the other tile sets (those with presets: 0, 2, 3, 5, 15, 17,
   18, 28). Rainbow Route's tile set 2 (Grassy earth) still holds a steel
   block, a star block and a tree trunk; tile set 3's edge art still holds
   pieces of background pine trees (`--zoom rim:10,12,13,18`), which only the
   log brushes draw.

Seen while testing, not caused by presets (upstream's code does it too):
painting a block onto an existing floor in the editor leaves some floor
cells under the block with their grass tops.

Note: sending ROM tile pictures to a model sends ROM-derived art off the
machine; get the user's say-so first.
