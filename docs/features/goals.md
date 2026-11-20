# GOALS: achievements and the run timer

Local achievements and an optional speedrun timer. Presentation only: it reads
the frame that is shown and keeps its own record on this device. It never writes
game memory, the input log or the netplay stream, so it moves no hashes and
online play is unaffected (each device keeps its own record).

## Use

**SETTINGS > GOALS** (the front end and the pause page):

| row | what |
|---|---|
| RUN TIMER | ON / OFF. The run starts on its first frame in play |
| SPLITS | ON / OFF: the last split's area and its time against your best |
| RESET TIMER | A, or **F9** on a keyboard (web and native): a new run |
| CLEAR BEST SPLITS | forgets the best times |
| ACHIEVEMENTS | how many are done, N/34 |
| then one row per achievement | DONE, lit once unlocked; locked ones are dimmed. The description line says how to get it |

A toast ("ACHIEVEMENT" and the name) shows at the top of the picture for 3.5 s
when one unlocks. The timer sits top right: real time, then in-game time (IGT),
then the last split.

Online, the GOALS rows are dimmed like every row that has no online slot
(SETTINGS' rule), but achievements still unlock and the timer still runs.

## The achievements (34)

- Bosses: Kracko, King Golem, Moley, Mega Titan (its head), Gobbler, Wiz, Master
  and Crazy Hand (Crazy Hand), Dark Meta Knight, Dark Mind (form 2).
- Mid-bosses: one, all eight (Mr Frosty .. Bombar, object types 0x38..0x3F).
- Mirror shards: 1, 4, 8 (the save's own count).
- The map: all 9 areas visited (this session), an area with every room done,
  50% and 100% of the area map's rooms done (the pause map's own "completed":
  visited and every item/door in it), all 9 area maps found.
- Copy abilities: 1, 10, all 26; Master.
- Treasure: a chest; all 4 vitalities.
- No misses: 10 and 50 rooms in a row, a boss beaten with no miss in its room.
- Online: play online; 4 and 8 players in a session; a boss beaten online; a
  shard found online.
- One hour of play (in-game frames, all runs).

A boss counts as beaten when this device saw it with health left and then its
health ran out, or it went while the room stayed -- not within 5 s after this
Kirby's miss (a miss reloads the room). Shard, map and treasure goals read the
save, so loading a far-along save unlocks them at once.

## The timer

- Real time: wall clock while the run is going (pauses and menus included,
  a gap over 1 s -- a hidden tab -- is not counted). IGT: simulated frames in
  play (the game's HUD up, in a room, not the attract demo) at 59.7275 Hz.
- Splits: one per mirror shard, named by the shard's area, and END when Dark
  Mind is beaten (the timer stops there). The best time to each split (real
  and IGT) is kept.

## Storage

The browser's `localStorage["katam.goals"]`; natively `goals.txt` beside
`settings.ini`. Plain `key=value` lines (`unlocked=0x...`, `abilities=`,
`midbosses=`, `bosses=`, `streak=`, `frames=`, `timer=`, `splits=`,
`bestN=ms,frames`). Scripted / test runs keep nothing.

## Code

- `platform/ui/goals_core.[ch]` -- the list, the record and its text form, the
  run and splits, the time format. No game code (unit test: `make goals-test`,
  in the smoke suite as `goals`).
- `platform/ui/goals.[ch]` -- reads the game once per simulated frame
  (`PortGoalsSample`, from `present.c` beside the party HUD's sample), draws
  the toast and timer (`PortGoalsDraw`, GPU overlays `GLR_OVERLAY_GOALS` and
  the one after), storage, and the SETTINGS rows (`gPocGoalsRows`).
- `platform/ui/goals_rooms.inc` -- the area map's rooms per area, from
  `game/src/pause_area_map.c` (`sAreaMapRoomInfos`).
- Hooks: `platform/menu_poc.c` (the GOALS section, kExtRows index 1, model 21),
  `platform/web/host_web.c` / `platform/native/host_sdl.c` (`PortGoalsInit`,
  F9), `platform/render/gl_renderer.[ch]` (two more overlay slots).
- `KATAM_GOALS_TEST=1` (native `--ui` runs): GOALS on in a scripted run, its
  record kept nowhere, unlocks logged as `[goals] unlocked NAME`; add `timer`
  / `toast` to the value to show them.

Not drawn on the web page's 2D-canvas fallback (no WebGL2); the record still keeps.
