# Tap to move

Status: first version, off by default (TOUCH PADS = TAP, or `?tap=1`).  Code: `platform/ui/tap_nav.c` (the
gestures, the nav field, the controller, the markers), a few lines in
`platform/main.c` (the keys), `platform/ui/present.c` (the view and the
markers), `web/shell.html` (TAP mode on the page), `platform/native/host_sdl.c`
(a click is a tap; the test commands).  Test: `tests/tap_nav.sh`.
Measurement: `tools/tap_sweep.sh`.

The idea, as asked for: *"iPhone controls for Kirby where you just tap or
hold the location on the screen you want Kirby to go and it navigates there
via simple ai.  And some type of tap or marking system for attack.  May be
reduced controls, but possibly more fun to play on a phone touchscreen, and
it opens up the screen to not need button controls."*

## How to try it

- On a phone: open the page with `?tap=1` (this visit only), or set
  SETTINGS > PLAYERS > TOUCH PADS to **TAP** (saved).  During play the pads
  go and a small pause button sits in the top left corner; tap where Kirby
  should go.  Menus, pauses and cutscenes bring the pads back.
- Natively: `KATAM_TAP=1 build/lp64/katam ROM` (or `touch=3` in the saved
  settings); the left mouse button is a finger.  F1/Tab still open the menu.
- `KATAM_TAP_LOG=1` logs every target, arrival, door, enemy and give-up;
  `=2` adds every frame's state, `=3` the steering decisions.

## Gestures (as implemented)

| gesture | where | what Kirby does |
|---|---|---|
| tap | open ground, or at most 16 px (about Kirby's height) above it | runs there (anything more than three cells; a walk for the last steps) and stops on the spot; jumps, flies, drops and steps off ledges as the way needs -- along the ground as far as it goes, then up: floating is slow |
| tap | higher in the air | runs as near as the ground gets him, flies the rest; once there he lets go (the air out, and down) -- unless a finger still holds the target, or there is a pit, spikes or an enemy under him: then he hovers |
| tap | inside solid ground | goes to the top of that block (or the nearest open cell) |
| hold and drag | anywhere | the target follows the finger; let go and he stops where it was released |
| tap | an enemy (finger-sized slop) | marks it with a red reticle; he stands off on its row at the right range, faces it, then: no ability -- inhales it (B held); a mouthful -- spits a star at it; an ability -- attacks with B (range per ability).  Puffed, he exhales at it.  The mark goes when the enemy does (or after 15 s / 6 bouts) |
| tap | a door, mirror or star door (its picture: the door cell or the one above; a door that is an object -- the hub's mirrors, an area door -- by its own rect, 8 px more slop) | walks to it, lets the air out if puffed, presses Up |
| tap | a treasure chest | walks into it and presses Up (that is what opens one); done when he plays the opening |
| tap | a breakable block (star block, bomb block; finger-sized slop) | yellow ring on it; stands right beside it on ground (its row, or the row above or below in the same column, which is then the one he hits; else two cells off; else hovering beside it), faces it and hits: B (a star with a mouthful, the ability's attack, an air puff when puffed; with nothing, a long inhale -- weak blocks come in, then a star), else a slide; again; then the red ring.  Done when that cell is no longer breakable.  Hard blocks (strength 2..4) want the right ability: he tries, the game decides |
| tap | a heavy star block (star stone blocks, the pushable kind) | stands on its row within 1..3 cells on ground -- one cell further back than the nearest when he can stand there, since the inhale draws the block until it meets him (804's has to clear a cell to fall into its shaft) -- faces it, and holds B 110 frames: the strong inhale (B held 48 frames, `sub_080720F8`) is a push of 2, which is what one moves (a walk into it is 1: two Kirbys); then a walk into it, then the inhale again.  Done when it moved 8 px or fell |
| tap | food, a 1-up, a battery, candy | walks to it (touching takes it) |
| tap | a puzzle switch, small floor button or big switch (the switch's own rect, finger-sized slop) | yellow ring on it; walks onto it and stops there -- his body is a hit of the type both kinds take (`unk68` 0x82), so standing on it presses it, and he stays on it (a held switch stays down).  Done when the switch reports down (`unkB6` 0x101) with him stopped on it.  If it does not go down: a hop onto it, then from beside it, facing it, B, a slide, B, a slide, then the red ring.  A switch the game made inert is not one (a tap there is a walk) |
| tap | Kirby | attacks in place (B): with a mouthful that spits, puffed it exhales; clears any target.  A tap nearer a switch he stands beside than his centre is the switch |
| hold | Kirby, still (14 frames, within finger slop) | inhales for as long as the finger stays (B held): long enough, the strong inhale that moves heavy blocks; a swipe down from the hold swallows |
| swipe down | starting on Kirby | a mouthful: swallow (Down); an ability: drop it (Select); else crouch |
| swipe diagonally | starting on Kirby, up or down | the slide, a ground kick, that way: face it, Down 6 frames, then Down+A (Down+A from nothing is a jump) |
| swipe up | starting on Kirby | calls for help: R, the phone (a battery bar; the helpers come) |
| second finger | anywhere, while one is down | a quick tap: attack (B, on release); held 30 frames with the first: L, held while both stay -- "hold L to start over" (48 frames of it return to the hub where the room allows) |
| pause button | top left (the page's Start pad, restyled) | Start, as ever |

A real pad or keyboard press (not Start or Select) takes the autopilot's hands
off at once: manual input always wins.

Not done: guard (the game has none to teach; R/L are the phone and the
return to the hub, above), ladders as such (he flies past them), the throw
and stone abilities' special uses.

## The tutorial, lesson by lesson

The tutorial (rooms 803-805; the story before NORMAL) is "follow the
helpers": no instruction cards until its end, the three CPU Kirbys walk
ahead as you trip the room's triggers and demonstrate.  What each room asks
of the player, and the tap that does it (tests/tap_tutorial.sh plays it all,
from a new game, with the page's default CPU AI mix, fingers only but A on
the last card):

| room | the lesson | with tap to move |
|---|---|---|
| 803 | walk, jump, fly over the pillar's foot | tap ahead; the path jumps and flies |
| 803 | inhale an enemy, spit it, swallow it | tap the enemy (inhale), tap Kirby (spit) or swipe down (swallow); or hold Kirby (inhale) |
| 803 | break the star blocks under the pillar (the only way on) | tap a block: a long inhale takes it in, or a star breaks it |
| 804 | a floor switch opens a sliding door; a second, down in a pit, holds the next | tap each switch: he stands on it |
| 804 | a bomb block: hit it and its chain of metal blocks goes | tap it: a long inhale or a hit sets it off |
| 804 | a heavy star block in the only gap: drag it off its ledge | tap it: the strong inhale (B held) moves it |
| 805 | copy abilities (Sword Knight, Cupie, Flamer): take one, use it, cut blocks | tap the enemy, swipe down; tap the blocks (the ability's attack) |
| 805 | the big chest (the map): Up inside it | tap the chest |
| 805 | the mirror to the hub, then the card "Using L & R: call for help with R, hold L to start over" | tap the mirror; the card takes A (the pads come back while it is up); then swipe up from Kirby (R) and two fingers held (L) |

The slide is no room's gate but the helpers' scripts use it in puzzle rooms
(146, 206, 426, 617, 733, 815: "go to a node, face right, slide"), so it
is a gesture of its own (the diagonal swipe).

## The decisions, and why

### An autopilot that presses player 1's buttons, outside the simulation

Each live frame, right before the key latch, `PortTapFrame` reads the game
and returns a GBA key mask; `main.c` ORs it into this device's player 1 keys
exactly where the host's own buttons go (`sHostKeys | PortTapKeys()`, also in
`PortSetKeys` and `PortCurrentKeys`).  Consequences:

- The input log records keys as always.  A recording made with tap controls
  replays bit for bit with no new latch, and a replay never runs the
  autopilot (`PortInputLogReplaying`: it returns 0).
- Online lockstep is unchanged: the session samples `PortCurrentKeys`, which
  now includes the autopilot's keys -- to the session it is this device's
  input.  Rollback catch-up frames and the pause overlay's fork never run it.
- No sim state, no layout change, no hash change: its state is statics in a
  host file (linked last natively, with the other UI files), the game is
  only read.  Off (the default) it returns 0 and nothing changes at all.
- The taps are never recorded; only the keys they led to.

**Why C, and not the page's touch device in JS** (the first idea): the
touch device in `web/input_devices.js` always drives player 1 (TOUCH never
joins and is never claimed by another seat), so ORing the mask in at the C
side has the same meaning, and it buys three things: the controller runs
exactly once per simulated frame (the page's `requestAnimationFrame` is not
1:1 with sim frames at 120 Hz or during a catch-up burst, and a jump is a
count of frames of A); it reads game memory with the game's own headers
(the Kirby, the room's live collision map, the objects); and the same code
runs natively, which makes headless tests possible.  The page only forwards
fingers (in canvas pixels) and decides when the picture takes them.

Not through the CPU brain: driving player 1 with `CpuBrain` would change sim
state (the brain's struct, its RNG calls) and need a latch for every
command.  The brain's ACTION layer was the reference for the key patterns
(the double-tap run, flaps, exhales), not code that runs.

### Reading the world

- **The view**: `present.c` hands the tap module the same mapping the party
  HUD's name tags use (world = view pixel + camera + view offset; the picture
  is `world[]` in framebuffer pixels).  A finger is mapped when it lands,
  through the picture the person was looking at, so zoom, widescreen, the
  GPU renderer's own view and the Classic 240x160 picture all map
  correctly (the 2D canvas too: `host_web.c`).
- **The room**: the Kirby's room's live collision map (`gUnk_02024ED0[li->unk65E]`,
  blocks already broken are open) as nav classes: solid (incl. breakable,
  Mini-only gaps), one-way platform (0x4), drop-through (0x8), floor slope,
  water, hazard (0x2000), door (0x4000), ladder.  Wall objects the game
  registers as solid each frame (sliding doors, star stone blocks, lava
  walls, flame columns) are stamped in as solid; cannons as hazards (they
  catch him).
- **The objects**: from the task table (live tasks whose struct is an object
  in the Kirby's room), as `PortLnFindEnemy` does.  Not the game's per-room
  collision list: it is built during a frame and cleared at its end, which is
  when this runs (a first version saw no enemies at all).

### The nav field

A cost-to-go field from the target over the room's cells (Dijkstra over
reversed moves, at most 1950 cells, re-run when the target cell or the map
changes or every 30 frames): every cell knows its best next step, Kirby's
progress is the cost at his cell, and a moving target (a drag, an enemy) is
just a re-run.  Moves and costs: across 2 on ground, 5 through air (a run is
1.9 px a frame, flight sideways 1.1: a way along the ground and a climb at
the end beats a float across, even one that drops and climbs back), 3 in
water; up 4 (a jump or a flap), 2 up a slope; down 1; a one-way floor cannot
be entered from above unless it is a drop-through one (3, Down).  Hazards
+60, standing on solid spikes +30, the bottom row of a pit +40, any other
cell over a pit's column +3 (cross it high, come down over ground).  Kirby can fly, so
nearly everything open is reachable; when the target is not (a sealed part
of the room, behind a gate), he goes to the nearest reachable cell and, once
there, the target is given up (red ring) rather than pushed against.

### The controller, from measured physics

Measured in room 801 with `KATAM_TAP_LOG=2` (8.8 units, y down, yspeed > 0
is up): a ground jump starts at 1024 and loses 38 a frame while A is held
(release cuts it to ~138): 4 frames of A rise 16 px, 10 frames 34, ~23
frames 54, the most.  A press in the air puffs Kirby up (flag 0x40) and,
held, he climbs at 336 (1.3 px a frame) for as long as it is held; released,
he floats down at 288 at most.  B while puffed exhales and he falls at 640.
Walking is 332, running 476 (a double tap; flag 0x10, kept through a jump
and with a mouthful), a running jump 504 across, flying sideways 272; a walk
slides ~15 px to a stop, a run ~31; in the air he hardly slows by himself,
and pressing the other way brakes 18 a frame.  He lands still puffed.  The
"in the air" flag (0x20) is not set while puffed: on the ground or not is
`unk62 & 4`.

Each frame the next few steps of the field from Kirby's cell give:
- **across**: Left/Right toward a point two steps out (the target itself on
  the last cells); a run (the double tap) whenever the target is more than
  three cells off and the next step is along the ground, not while a jump is
  under way; coasting when the slide alone gets there (and he is moving: a
  belt can hold him still), so he stops on the spot; in the air near the
  target, braking the other way once the stop would reach it; in the air
  with the way going down first (a dip), on across the way it then goes, not
  straight down (letting go ends a run);
- **up**: a jump held for the height the path climbs (7/14/23 frames for
  1/2/3+ cells), then A held while puffed for the rest -- he puffs only for a
  climb from where he is, not for one further on (he lands and runs there
  first); up a slope or over a hill is a walk, never a puff;
- **gaps and pits**: a jump at a gap (14 frames: a running jump clears most);
  over a pit he puffs once the jump falls;
- **down**: Down on a drop-through floor; step off to the far side of his own
  cell when the floor beside it holds him up; puffed in the air, when the way
  goes down onto safe ground and does not climb again soon, exhale and drop
  (then run); landed puffed, exhale unless the climb is right there; dive
  when bobbing on water;
- **corners**: line up in his own column before a vertical step past a
  corner; in a row with a lip above or below, keep his centre on the row's
  (he is a row tall);
- **enemies in the way**: a hop (sooner when running); a jump coming down
  onto one puffs and floats over it, for a second at most (not down a shaft);
- **a mouthful** cannot fly: when the way climbs more than a jump, swallow;
- **arrival**: on the floor, within 6 px, slow; puffed, exhale first.  An
  air point: within 8 px, then he lets go (see the table).  A target whose
  floor goes (a star block under it broken, by his own exhale too) moves to
  the floor below, or becomes that point in the air;
- **watchdog**: no progress for 90 frames: a hop; 180: a long jump (A also
  fires a cannon he is sitting in); 330: give up (the ring goes red).

### Feedback

GL overlays after the party HUD's (`GLR_OVERLAY_TAP`, three more slots):
a yellow ring that breathes at the target (an arrow over it for a door, red
for a second and a half when given up), a red reticle on a marked enemy, a
dot every other cell along the path.  Drawn in world pixels at the picture's
scale, so they sit in the game's pixel grid and follow zoom.  Not on the 2D
canvas (no WebGL2): the taps work there, the markers are not drawn.

### Settings and the page

PLAYERS > TOUCH PADS gains **TAP** (`PS_TOUCH_TAP`, saved as `touch=3`;
description "TAP WHERE KIRBY SHOULD GO").  `?tap=1` forces it for a visit
(`PortTapForceJs`), `KATAM_TAP=1` natively.  On the page, TAP keeps the pads
for everything that is not play and adds `body.tap-play` while the game's
HUD is up (`PortTapActiveJs`: the HUD is up, a room is loaded, not
replaying): every pad but Start is hidden, Start becomes a 44 px pause
button in the top left corner, and pointer events on the stage go to the
game in canvas pixels.  In portrait there is no reserved pad band in TAP
mode, so the picture sits mid-screen and does not jump when the pads come
back for a menu (they lie over the bottom, as in landscape).  A one-time
notice says what the taps do.

## Interplay

- **Pause and menus**: Start (the corner button) pauses; with the HUD down
  the pads are back and the menus work as before.  Targets survive a pause.
- **Local multiplayer**: it drives player 1 of this device only; players 2-4
  keep their pads (`devs` assigns them as before).  It reads
  `gLocalPlayerId`'s Kirby (player 1 locally).
- **Online**: it is this device's player 1 input; the Kirby it reads is the
  one this device's camera follows (`gLocalPlayerId`).  A device with
  several local seats in a session: the autopilot drives seat 0.
- **Both schemes**: TAP hides the pads during play; a pad or keyboard used
  meanwhile takes over at once (any button but Start and Select cancels the
  target).

## Evidence

`tests/tap_nav.sh build/lp64/katam` (a live run, taps injected through the
devices script, then its recording replayed):

    ok   hub: the ground below: Kirby at the target (f1759 202,296 160 frames)
    ok   hub: the high platform (a flight): Kirby at the target (f2092 246,72 193 frames)
    ok   door: room 801 -> 101 (f2453)
    ok   101: a point along the ground: Kirby at the target (f2868 106,136 69 frames)
    ok   101: the marked enemy was inhaled
    ok   101: swipe down swallowed it (ability 7)
    ok   door: room 101 -> 106 (f4030)
    ok   door: room 106 -> 107 (f5334)
    ok   310: the small switch went down under Kirby (f1940 after 213 frames)
    ok   390: the big switch went down under Kirby (f2060 after 109 frames)
    ok   replay: 5999 frames, the live run's hash log replays identically (tap controls off and on)
    ok   recording: keys only (419 key lines, no taps)
    TAP NAV PASS

Room 101 to its far door is the whole of Rainbow Route's first room: the
hill, a block, the six-cell column (a flight), both pits.

Running and floating (the "always run, float only the rest" change):
one tap from the same start, so the two controllers face the same room --
8 random reachable cells (3..25 cells away) in each of the 32 sweep rooms,
256 taps each, with fresh state per tap:

| | arrived | hits taken | time, taps both reached |
|---|---|---|---|
| before | 228 | 78 | 37595 frames |
| running | 227 | 66 | 35434 frames (-6%) |

In room 101 (Rainbow Route), a point on the ground past the hill: 318 ->
272 frames; the air above the six-cell column: 478 -> 390, now on the
ground for most of the way (it floated from the hill to the column before).
Variants that floated over enemies for half a second, or not near the
target, lost more than they won (more hits, or a room given up).

`tools/tap_sweep.sh build/lp64/katam`: 32 rooms across every area (the
warp_sweep recording's), a tap every 900 frames on a random cell Kirby can
stand in and reach, 3..25 cells away: **232 of 247 arrived (94%)**, median
50-320 frames per room; 5 given up as stuck, 9 not there when the next tap
came, 1 left the room on the way.  The first version of the controller
managed 80%; the fixes that took it to 94% came from these runs (ledge
step-offs, corners, lips in flight, wall objects, mouthfuls).

In the browser (headless Chromium, an emulated iPhone 390x844 at DPR 3,
real touch events through CDP): the pads hide in play with the corner pause
button and the notice; a tap beside Kirby walks him there; a tap on the hub's
mirror shows the door ring and takes him to Rainbow Route; a tap on a Waddle
Doo shows the reticle and he inhales it; a swipe down on him swallows it
(BEAM on the HUD); a hold-and-drag moves the target with the finger; the
pause button pauses and the pads come back; SETTINGS > PLAYERS > TOUCH PADS
steps to TAP and saves `touch=3`.

Unchanged: `make regress` (as on main at 078f94b: only `aim`, which
diverges there too), `tests/cpu_ai.sh`, `make input-devices-test`.

## Limits (honest)

- Enemies: he hops over one in his way and floats over one he would land on,
  but otherwise runs into them; in the sweep he takes hits and loses
  abilities on long walks.  Running on the ground meets more of them than
  floating above did, which the floating-over makes up for (fewer hits than
  before, above).  The player is expected to tap them; there is no
  auto-defence.  Letting go at an air point near a walking enemy can cost a
  hit afterwards.
- Ability ranges are rough (three bands); abilities with special uses
  (stone, throw, UFO, missile) are only pressed, not played.  Boss fights
  are untested.
- Water: steering and a dive work; swimming was barely exercised (the one
  water pool in the sweep sits behind a cannon).  Cannons catch him (avoided
  when there is another way; a stalled Kirby presses A, which fires one).
- Gates and ability blocks: a target behind them is "cannot get there"
  (red); he does not fetch abilities.  Switches he presses when tapped
  (tests/tap_nav.sh: a small one in room 310, a big one in 390; a timed
  one in 610 by hand), but only ones he can reach: a switch inside the
  puzzle it solves shows the red ring.
- Ladders: not climbed as ladders; he flies.
- One door per tap; no "go to the next room" route across doors (the CPU AI's
  door-group routing could do it, see "Next").
- The markers need WebGL2 (the 2D canvas maps taps but draws no markers).
- Natively a click on the menu's gear while TAP is on is a tap (F1 works).
- Found while sweeping, not caused by this (input alone reaches it): the
  four-Kirby native build can segfault in `sir_kibble.c:530` when a cutter
  outlives its thrower (the wide build guards that NULL; wasm reads it
  harmlessly).  `tools/tap_sweep.sh` counts such a run's taps up to the end.
- Fat fingers on a small picture: the slop is finger-sized in CSS pixels
  (about 14-26), but a tap near Kirby is "Kirby" (16 css px), not a step
  (a switch beside him excepted).

## Next steps

1. Play it on a real iPhone and tune: slop sizes, the ring's size, whether
   run thresholds, whether letting go at an air point should drift down
   puffed instead of dropping.
2. Auto-defence: mark the nearest enemy in the way when he has an ability
   (or inhale it when he has none) -- an option, so tapping stays the game.
3. Routes across rooms: a tap on a door's destination on the pause map, or a
   long-press "explore" using the CPU AI's door-group routing
   (`cpu_ai_modes.c`), which is a pure function of the ROM data.
4. Guard: a two-finger hold, or a hold on Kirby.
5. Ladders and water as first-class moves; cannons aimed at the target.
6. Markers on the 2D canvas (blend like `PortPartyHudDrawSoft`).
7. A browser test in `tools/devices_browser_test.mjs` (touch events on the
   GL canvas, the reticle, the pause button) for `tools/full_check.sh`.
