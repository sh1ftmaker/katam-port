# Classifying brush pieces: the method

Status: proven by hand on two brushes (sky garden Clouds, tile set 29;
Rainbow Route Grassy soil, tile set 3). No classifier tool exists yet.
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
  1:1 and gentle slopes), `rim.png` (the edge art) and, with `--zoom`,
  `zoom.png` (those pieces in place in the original, 5x5 cells, up to three
  rooms; `--zoom rim:2,5` for edge art). `--rules none` shows what is learned
  without the preset. The pictures are the player's ROM art: the script
  refuses to write inside the repository.

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
