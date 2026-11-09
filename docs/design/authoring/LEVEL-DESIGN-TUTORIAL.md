# Level design tutorial: making rooms that look like the original

For an agent starting cold. [AI-GUIDE.md](AI-GUIDE.md) gives the commands and the format rules;
[rooms.md](rooms.md) gives every field. This file adds what those do not: how to work, what the
original's art is made of, and the mistakes that cost time. It was written while building
`examples/authoring/rainbow_annex/` (4 rooms, Rainbow Route looks); every token below was used
and checked there.

## 1. The order of work that succeeded

1. **Pick the look first.** A look is (tileset, palette, background, music) exactly as the
   originals pair them. List them: load every `rooms/<area>/*.room.json` of the decompiled
   vanilla project and print `size, music, tileset, palette, background` and an object-type
   count per room. For Rainbow Route:
   - forest: tileset/palette 0 + background 1 (day, rooms 101, 106, 107, 141) or 0 (night, 103, 140-143)
   - night forest with smooth grass: tileset/palette 2 + background 0/1 (104, 108, 110)
   - lumber yard / pine hills: tileset/palette 3 + background 28 (115-119, 138, 139, 171-178)
   - all with `mus_rainbow_route`.
2. **Study 3-4 original rooms of that look** (section 2) until you can name the pieces.
3. **Block out collision only** (ASCII rows), in shapes the originals use: flat runs, 1:2
   mounds, 1:1 hills with a plateau, 2-row floating ledges, 2-wide cliff walls. Odd shapes
   (one-cell overhangs, a lone slope cell under a wall) are where both the auto-tiler and
   your eye fail.
4. **Draft the art with the auto-tiler** (`tools/katam/lib/autotile.mjs`), then **lock and
   hand-place** the structures it cannot draw (log frames, trees, star blocks) by copying
   pieces from known cells of the originals, then **fix single bad cells** by token.
5. **Objects and doors**, then validate, render, look, playtest every door, repeat.

Keep the room in a small generator script (collision rows + style map + "copy this original
cell here" list + objects), not hand-edited JSON: you will rebuild 20 times.

## 2. Studying an original room before drawing

- Render it with a grid: `katam render room:rainbow_route/106 -o r.png --base V --scale 3 --overlay grid`,
  then crop with ImageMagick (`magick r.png -crop WxH+X+Y +repage c.png`, cells are 48 px at
  scale 3) and Read the crop. Whole-room renders are too small to judge pieces.
- Print the same window as text: for each cell its art legend key and its collision. Seeing
  `a2#` under every `aw/` (slope piece over "piece under a slope") teaches the grammar faster
  than pictures. A 30-line Python helper does this (decode `art.rows` with the legend key
  width, same for `collision`).
- Questions to answer per look: which row is the floor (row 9 in a 13-high room), what is drawn
  in the **air** cell above the floor, how the top alternates, what fills, what a wall looks
  like, how a floating ledge ends underneath, how slopes meet flat ground, where doors are.
- Find scenery pieces by searching every room of the tileset for a token (e.g. a tree trunk)
  and look at each hit: most are partly hidden behind solid logs; pick the one drawn whole.

## 3. Piece vocabulary

Tokens are `TL TR BL BR`, each `TTTfP` (see AI-GUIDE). `h` mirrors a tile; to mirror a whole
piece swap TL<->TR and BL<->BR and toggle `.`<->`h`, `v`<->`x` on all four (the originals do
this; light then comes from the other side, which is acceptable for logs, not for faces).

### Forest (tileset 0, palette 0)

Three ground materials (the auto-tiler's presets, `assets/schema/authoring/autotile/tileset-vanilla-0.json`):
*Grassy cobbles* (flat ground), *Grassy hill* (1:1 hills, smooth tan rock), *Grassy cliff*
(pillars, walls, ledge sides, brown-edged rock).

- **The grass is drawn half in the air.** The cell above a floor is collision air but its
  bottom tiles hold the leaf tips: `000.0 000.0 092.0 094.0`, `000.0 000.0 094.0 040.0`,
  `000.0 000.0 005.0 006.0`, `000.0 000.0 13e.0 094.0` (vary them, never one repeated).
- Top (solid) row of cobbles: two pieces alternating, `0b0.0 0b1.0 0d0.0 016.0` /
  `0b2.0 0b3.0 0d2.0 0d3.0`.
- Row under it: `0f0.0 036.0 0e3.0 056.0` / `108.0 136.0 128.0 156.0` (alternating), then
  `1eb.0 1b5.0 1a0.0 1a1.0` / `148.0 149.0 1a2.0 1a3.0`. The original leaves the last two
  rows of a 13-row room empty or uses whatever: the camera never shows them.
- 1:1 slope (collision 11, `floor_1_1_riseR`): the same piece on every step,
  `02c.0 02d.0 04c.0 04d.0`; under each slope cell `124.0 125.0 144.0 145.0`; in the air left
  of the foot `000.0 000.0 003.0 004.0`. Use the *Grassy hill* style for the slope, the plateau
  and two rows of fill under it; with cobbles the auto-tiler leaves a pale diagonal seam.
- 1:2 mound (collision 5,6 up, 7,8 down): `000.0 0e5.0 104.0 105.0` (low), `0e6.0 0e7.0 106.0 107.0`
  (high), cap `021.0 022.0 04c.0 04d.0`, down side `222.0 223.0 242.0 243.0` (high),
  `224.0 000.0 244.0 245.0` (low). In the air above the high half: `000.0 000.0 0c6.0 0c7.0`.
- Floating ledge (2 rows): top `021.0 022.0 019.0 01a.0`, underside `0ac.0 0ad.0 011.0 0cd.0`,
  right end `025.0 026.0 01b.0 184.0` over `03b.0 1a4.0 0ce.0 05c.0`.
- Cliff wall, 2 wide, repeating down: left column `12c.0 12d.0 110.0 111.0`,
  `0ac.0 0ad.0 011.0 0cd.0`, `031.0 0ed.0 051.0 10d.0`; right column `12e.0 12f.0 112.0 113.0`,
  `0ae.0 0af.0 0ce.0 0cf.0`, `0ee.0 068.0 10e.0 10f.0`. Paint walls with the *Grassy cliff*
  style or the auto-tiler mixes in pale cobble pieces.
- One-way brick platform (collision 15): `211.1 212.1 000.0 000.0` (bank 1).
- Ladder: top (collision 32 `ladder_top`) `20f.2 210.2 0de.3 0deh3`, body (33)
  `0fe.3 0feh3 0fe.3 0feh3`, foot `0fe.3 0feh3 16f.3 16fh3`. Put it beside a ledge, its top
  level with the ledge surface, never through the ledge.
- Star block (collision 240 `breakable_0` + a patch that turns it to air): art from room 106
  cell (45,8).
- Indoor hollow: vine-wrapped log ceiling `15c.2 15d.2 17c.2 17d.2` / `15e.2 15f.2 17e.2 17f.2`
  with vines hanging into the air row below (`19c.2 19d.2 000.0 000.0` / `19e.2 19f.2 000.0 000.0`);
  tree-trunk walls as in room 141 x0-1 and x44-45 (a 3-row cycle).

### Lumber yard / pine hills (tileset 3, palette 3, background 28)

- *Grassy soil* ground: air row above the floor holds blades `000.2 000.2 0eb.2 0eb.2`,
  `000.2 000.2 000.2 0eb.2`, and sprouts `1ca.2 1cb.2 1ea.2 1eb.2` (use sprouts sparingly,
  on hill tops and every 6-10 cells). Top row alternates `10c.2 10b.2 12a.2 12b.2` /
  `10a.2 10d.2 12c.2 12d.2`; roots below `14a.2 14b.2 16a.2 16b.2` / `14c.2 14d.2 16c.2 16d.2`.
- 1:2 hill: up low `000.2 000.2 014.2 015.2`, up high `016.2 017.2 036.2 037.2`, cap
  `018.2 019.2 038.2 039.2`, then the mirrors for the way down (`019h2 018h2 039h2 038h2`,
  `017h2 016h2 037h2 036h2`, `000.0 000.0 015h2 014h2`). The auto-tiler draws these hills well.
- Floating log (solid): left end, middles, right end copied from room 172 row 4 x27-30.
- Lashed log frames (stairs, cabins): the auto-tiler cannot draw them. Copy a frame as a unit
  (room 172 x3-15 rows 5-8 is a 4-step stair; mirrored it rises to the right) and **also copy
  the grass row under it** (172 rows 9-10), or the ground under the frame becomes root fill.
- Pine tree (scenery, all air cells): room 173 x0-4 rows 0-8 is a whole tree, trunk at x1-2,
  base meeting the grass at row 8 (`2c4.0 2a5.0 2c7.0 2c8.0`, `2c6.0 000h0 2c9.0 0eb.2`). Its
  crown is cut by the top of the room, as all original pines are. Add a mirrored column of
  needle tips on its left (mirror of 173 x3 rows 0-1, x4 rows 2-4) when it stands mid-room.
  Copy only the non-empty cells, or it erases what is behind it.

## 4. Collision must match the art

- Every cell that looks walkable is solid (13) or platform (15); every slope piece has the
  slope value of its kind (11/12 for 1:1, 5,6 / 7,8 for 1:2 in that order); the air rows with
  grass tips stay air. Paint art and collision from the **same** grid; the render's
  `--overlay collision` tints solid grey, platform blue, slopes orange, doors green: check
  that no tint sits on sky and no ground lacks a tint.
- Logs and frames are solid; benches, bricks and plank bridges are one-way.
- Ladders: `ladder_top` at the surface level, `ladder` below down to the floor; nothing solid
  in the ladder column.

## 5. Doors and arrival

- Each door: a `doors` entry, collision 255 under its cell, a `door` object at the cell
  centre (`x = cx*16+8, y = cy*16+8`). Door art is drawn by the object; leave the cell's art
  as it would be without a door (sky, grass tips).
- **arrive = destination door cell + (1, 1).** Measured in play: the game puts Kirby on the
  tile (arrive.x - 1, arrive.y - 1), so arriving at `{x: dx+1, y: dy+1}` puts him standing on
  the door cell (dx, dy). Keep the cell right of every door free: the validator's
  `arrive-blocked` check assumes another convention and gives a false error when it is solid.
- Two-way between your rooms; a one-way exit into a vanilla room arrives on that room's
  door (read its `doors`/`door` objects and apply the same +1,+1). Do not ship an edited copy
  of a vanilla room to add an entrance; say in `project.json` where one belongs.

## 6. Placing enemies, food, chests (habits of the originals)

- Density: about one enemy per 8-12 cells of a horizontal room, 4-5 in a 16x33 climb. Rainbow
  Route uses Waddle Dee, Waddle Doo, Bronto Burt, Sparky, Sir Kibble, Rolypoly, Scarfy, Chip,
  Wheelie, Gordo (lumber yard). Ability enemies (Doo, Sparky, Kibble) sit where the player
  has room to use the ability.
- Walkers stand in the air cell above a floor (`y = (floorRow-1)*16+8`). On ledges use
  `waddle_dee_1` with `params.movePattern: 1` (paces back and forth on a counter); the
  default walks straight and falls off, then crowds the door below.
- Keep 6+ cells between an arrival and any enemy that walks or hops toward Kirby. A
  playtest caught a Waddle Dee hitting Kirby on arrival 4 cells away, and a Sparky 3 cells away.
- Food rewards a detour: on a brick row above a hill, on a floating log, on top of a stair.
  Flying enemies (Bronto Burt, Scarfy) go in open air two or more rows above the walk line.
- Chests: `small_chest` with `params.contents` (2 tomato, 3 meat, 4 soda, 0 1UP) and
  `params.chestId`. **chestId is a global save bit (0-83) and every value is used by a
  vanilla chest**: opening yours marks that vanilla chest opened, and it shows opened if the
  player already opened that one. The rainbow_annex hollow reuses 2 (room 570's meat chest).
  There is no clean choice today; see the tool wishes in the report.
- Every object gets its own `despawnBit`.

## 7. The camera

- 15x10 cells on screen. The bottom 40 px of a room are never shown: floors at least 3 rows
  above the bottom (row 9 of 13, row 29 of 33). The top rows are shown: a crown cut by the
  top edge is fine, a stray piece there is visible.
- The renderer draws the whole room; mentally crop to what the camera follows.

## 8. The loop and how to read its output

- `katam validate DIR --base V`: fix every error. Expected warnings: `nav` (no helper graph),
  a one-way door back into vanilla ("has no door back"). The validator rejects `\` as a
  legend key (use `~` for the 1:1 down slope).
- `katam render ROOM -o x.png --project DIR --base V --scale 2` and the same with
  `--overlay collision,objects,doors,grid`. Read both, then crops at 2-3x of every structure.
  Magenta = palette bank 15 / out-of-range: not allowed where the camera looks.
- `katam playtest DIR --rom ROM -o OUT --base V --room ROOM --at X:Y --frames N --input S --shots F,F --bin build/lp64/katam`
  - `--at X:Y` is an arrive cell (same +1,+1 rule), so `--at` = door cell + (1,1) tests a door.
  - Input lines `k FRAME HEXMASK` (Up 040, Right 010, A 001; frames count from arrival).
    **Do not use `hold MASK FROM TO`: its mask is parsed as decimal**, so `hold 40 ...` presses
    0x28, not Up. A door test is `k 40 040` / `k 52 000` with 260 frames; the JSON's
    `"result": "ok: alive, left for room N"` and `finalRoom` tell where you went.
  - A run-through: hold Right (`010`) with A taps (`011` for 20 frames every 36) and
    `--shots` every 100 frames; montage the PNGs (`magick a.png b.png +append`) and look.

## 9. Mistakes made and how they were caught

| mistake | caught by |
|---|---|
| 1:1 hill drawn with two slope cells in a row | render: broken grass band; collision text showed `//` |
| hill slope with the cobble material: pale diagonal seam | 3x crop; fixed with the *Grassy hill* style |
| cliff wall partly pale cobbles | full render; fixed with the *Grassy cliff* style map |
| tree stamp copied empty cells over a log frame | render: frame with a missing foot |
| ground under a log frame drawn as root fill | 2x crop; fixed by copying the original's grass row under it |
| auto-tiler strewing a pine-trunk foot on open grass | listing air cells with art by token; replaced |
| star blocks without art (collision only) | render showed nothing; art must be placed too |
| enemies reaching the arrival | playtest screenshots ("OUCH!") |
| door tests that never left the room | `hold` decimal mask; switched to `k` lines |

## 10. Checklist before a room is done

- [ ] Look = an original (tileset, palette, background, music) combination.
- [ ] Floor 3+ rows above the bottom; nothing important in the last 40 px.
- [ ] Every surface has the air row of tips above it; tops alternate; no single piece repeated
      over a long run; no magenta or stray piece in the camera's view.
- [ ] Slopes: right value under each slope piece, cap and under-piece in place.
- [ ] Collision overlay: no tint on sky, no untinted ground; ladders and one-ways correct.
- [ ] Doors: 255 + `door` object + entry; arrive = door + (1,1); cell right of door free;
      partner door exists (or the one-way is intended and noted).
- [ ] Enemies 6+ cells from arrivals; ledge walkers pace; distinct despawn bits.
- [ ] `validate` 0 errors; every door playtested (`finalRoom` is the partner); one run-through.
- [ ] `name`/`description` say what the room is for.
