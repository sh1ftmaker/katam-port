# Classifying brush pieces: the method

Status: proven by hand on one brush (sky garden Clouds, tile set 29). No
classifier tool exists yet. Written for whoever continues.

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
  brush (`learn()` treats it like the hidden pieces). It stays usable as a
  single piece in the editor. Presets also name styles (`name`).
- **Re-run and diff after every change.** Excluding pieces changes the
  community structure; two real cloud-edge pieces left the brush as a side
  effect. Compare the piece lists before and after and look at what moved.

## What exists

- `assets/schema/authoring/autotile/tileset-vanilla-29.json`: the worked
  example (7 pieces excluded, brush named Clouds).
- `tools/katam/dev/brush_sheet.mjs`: loads a decompiled vanilla project,
  runs `learn()` for one tile set and draws pieces. Run
  `node tools/katam/katam.mjs decompile --rom ROM --pack build/res/vanilla -o DIR`
  first and set `CLAUDE_JOB_DIR`-style paths inside it as needed; it was a
  scratch script, so expect to edit its paths. Output pictures come from the
  player's ROM: keep them outside the repo.

## Next steps (in order)

1. Check the Clouds brush in the editor; look at the 2 edge pieces that moved
   out of it, and at "Ground 2" (7 pieces) of the same tile set.
2. Build the role tagger (question 1) and a contact sheet laid out as
   material by role, so gaps ("no concave corner") show.
3. Only then a membership classifier: render sheets, ask a vision model per
   piece with a short material list for that tile set, and score it against
   hand-labelled brushes (start with Clouds: 123 cloud, 7 not) and against
   the white-share baseline. Output the preset format above.
4. An Advanced editor panel to click a piece in or out of a brush and save
   the preset.
5. Roll out to the other tile sets (those with presets: 0, 2, 3, 5, 15, 17,
   18, 28).

Note: sending ROM tile pictures to a model sends ROM-derived art off the
machine; get the user's say-so first.
