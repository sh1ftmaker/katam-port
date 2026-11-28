# CPU Kirbys directed by a Haiku agent

Status: prototype (branch `haiku-ai`), offline and native.  Code: the
executor in `game/src/cpu_ai_modes.c` (section "planner directives"), the
contract in `platform/port/cpu_ai.h`, the latch and the `cpugoal` line in
`platform/inputlog.c`, the planner host in `platform/cpu_planner.c`, its
native transport in `platform/native/planner_pipe.c`, the planner process
in `tools/haiku_ai/` (bridge, mock, Haiku backend, report, `run.sh`), the
tests in `tests/cpu_haiku.sh` and `tools/haiku_ai/test_planner.py`.  Builds
on `CPU-AI.md` (the modes) and `CPU-HINTS.md` (the game's CPU hints).

No real API call has been made: the Haiku path is built and unit-tested
against the official SDK on a mock transport, and runs as soon as
`ANTHROPIC_API_KEY` is exported.  Every run shown below used the mock
planner.

## The idea, and the split

> A Kirby AI driven by a Haiku agent.  Haiku can take screenshots and read
> the room's AI hints, and sets targets and sub-modes for the rule-based AI
> to execute.  It re-evaluates when a goal is accomplished or after it times
> out.

```
 slow, strategic (seconds)                fast, tactical (every frame)
 ┌──────────────────────────┐  directive  ┌──────────────────────────────┐
 │ planner: Haiku or mock   │ ──────────▶ │ executor: the CPU AI modes'   │
 │ tools/haiku_ai/bridge.py │  (latched,  │ pieces (door-group routes,    │
 │                          │  recorded)  │ HeadFor, GoTo, Guard, Mend,   │
 │ sees: picture, the room's│             │ the explorer) -> the stock    │
 │ hints, the Kirby, team,  │ ◀────────── │ SENSE and ACTION layers       │
 │ progress, past outcomes  │ observation │                               │
 └──────────────────────────┘ (on events) └──────────────────────────────┘
```

The planner is asked for one Kirby at a time, only on events: the Kirby has
no directive yet, its directive ended (done, failed with a reason, timed
out), or it has run far past its limit.  Never per frame.  While it thinks
(or if it is absent, slow or wrong) the Kirby runs its own mode, as before.

## Determinism: a directive is an input

| rule | how |
|---|---|
| Directives enter the simulation only at a latch | A recorded input-log line `cpugoal FRAME SLOT KIND A B C SECS` (`platform/inputlog.c`), applied in `PortInputLogLatch` after the modes' `cpuai` lines.  The planner host only ever calls `PortInputLogScheduleCpuGoal` (the next latch), exactly as a key press is scheduled. |
| Directive state is snapshotted, hashed sim state | In `struct CpuBrain`'s filler bytes (EWRAM, `gUnk_02038590[slot]`): zero at `CpuBrainCreate`, in every snapshot, hash and join image.  See "Where the state lives". |
| A replay never calls the planner | `PortPlannerPoll` returns while `PortInputLogReplaying()`, and the native host does not even start the planner process for `--replay` (`[planner] replaying: the recording's cpugoal lines only`).  Tested: a replay with `KATAM_PLANNER=false` set is identical and never asks. |
| The game never blocks on the planner | Non-blocking pipes, lines under `PIPE_BUF` (all or nothing), a dead planner is "gone", a 30 s answer timeout, a 30-questions-a-minute budget.  `KATAM_PLANNER_SYNC=1` waits for each answer -- a test switch that makes a *live* run reproducible; real play never sets it. |
| Online: only the host plans | Not implemented; the planner host is off in an online session.  The plan is below ("Online"). |
| No regressions | No directive = the stock brain or the mode, as before: see "Evidence". |

The directive's clock is the simulation's: `dirLeft` (seconds) ticks when
`gPortSim->frame.frameCount % 60 == 0`, so a timeout lands on the same frame
in every replay.  Nothing reads host time inside the simulation.

### Where the state lives

The brief suggested spare sim words or `struct CpuBrain` filler.  Measured:

- `gPortSim->game.reserved[0]` is the modes' word (2 bits x 16 slots: full).
- Spare sim-block bytes: `warp.reserved[2]` (8), `session.pad` (2),
  `game.paintPad` (2), `view.pad8` (1) -- 13 bytes, not enough for three
  (let alone fifteen) Kirbys.  `hdr.reserved` holds the online settings page.
- Growing the sim block (a new struct after `rando`) changes the size the
  sim hash covers, so every golden hash log (`tests/golden/`) would move;
  the mod store's `modStoreUsed` is not usable either, because the rollback
  snapshot is sized from it.
- `struct CpuBrain`'s filler (`fillerC0` 12, `fillerCE` 14, `fillerE2` 2,
  `fillerF0` 4 bytes) was **full** with the modes' state.  `unkBA` is free
  but `CpuBrainReset` zeroes it on every room change.

So the modes' state was repacked, losslessly: the three failed (room, room)
pairs (six u16s) are 20 bits each now (rooms are below 1024), in 8 bytes,
and a diagnostic door counter nothing read became the directive byte.  That
freed 7 bytes:

| bytes | field |
|---|---|
| `fillerC0[11]` | `dir`: kind (bits 0-3) \| status (bits 4-6); 0 = never had one |
| `fillerCE[0..7]` | the three blocked pairs + the next slot (was 13 bytes) |
| `fillerCE[8]` | `lastHp` (unchanged) |
| `fillerCE[9]` | operand B; once the directive has ended, the reason |
| `fillerCE[10..11]` | operand A |
| `fillerCE[12]` | operand C |
| `fillerCE[13]` | seconds left |

`CpuBrainTask`'s hook now asks `CpuAiWants(brain)`: any mode set, **or this
brain has ever had a directive** (`dir != 0`).  For a brain that never had
one and every mode CLASSIC it is false, and the stock goal layer runs
untouched -- that is why a directive also works on a CLASSIC Kirby (the
directive on the stock brain), and why no-directive runs are unchanged.

A directive is lost if the game recreates the brain (`CpuBrainCreate` at a
level start); the host sees the status drop to "none" and asks again.

## The directive vocabulary

Grounded in what the executor does today (`CPU-AI.md`): door routes (each
door record's arrival and the doors it reaches, gates it can open counted
open), walking to a door tile and taking it (opening a gate, breaking a
block on the way), walking to a point, staying with a Kirby, the explorer's
search, the stock enemy scan, food.

| kind | A | B | C | executor | done when | fails (reason) | default s |
|---|---|---|---|---|---|---|---|
| `goto_room` | room | door group + 1 (0 any) | - | `TravelG`: BFS over door records (arrival reach masks), HeadFor each door; with a group, an arrival that reaches that door of the room (`IN_PART`) | in the room (and in that group: `InGroup`, the live map flooded from the Kirby, reaching the group's door) | `bad_target` (no such room), `no_route`, `gave_up_door` (walked at a door 30 s, no other route) | 90 |
| `goto_door` | room (65535 = here) | door tile x (or a warp star / cannon exit) | y | `SetDoor` + `HeadFor` | arrived behind that door | `no_door` (no such door, not on the map now, not reachable even opening the gates it can), `gave_up_door`, `left_room` | 30 |
| `goto_xy` | room (65535 = here) | tile x | y | `GoToEx`, nav graph then 5 s turns straight at it | within one tile | `left_room` | 20 |
| `follow` | Kirby slot | - | - | `Guard` (the BODYGUARD: beside it, else door by door, warp after 20 s) | time up, in its room (else `timeout`) | `bad_target` | 30 |
| `explore` | - | - | - | `GoalExplorer` (the EXPLORER) | entered a room nobody had entered | `no_route`, `gave_up_door` | 90 |
| `fight` | - | - | - | goal on the nearest live boss, else enemy; the stock scan attacks | none left | `nothing_here` (none at the start) | 30 |
| `food` | - | - | - | goal on the nearest food, scan held off (as `Mend`) | HP went up | `nothing_here` | 20 |
| `wait` | - | - | - | goal on its own tile at the latch | time up | - | 10 |
| `auto` | - | - | - | the slot's own mode (or the stock brain) | time up | - | 30 |
| `cancel` | - | - | - | clears it at once | - | - | - |

Statuses: `running`, `done`, `failed`, `timeout`.  Every directive has a
limit (`SECS`, 1-255; 0 = the default above).  The room script still runs
first in a new room (up to 10 s, as for the modes), and the opening rooms
(0x323-0x325) stay the stock brain's.

The input-log line, as recorded (kind names, decimal operands):

```
cpuai 1300 E79E79E4
cpugoal 1606 2 goto_room 101 0 0 90
cpugoal 1636 1 follow 0 0 0 30
cpugoal 10563 3 goto_door 65535 58 11 40
```

### Where Haiku adds value, and where it cannot yet

The modes already route, explore and guard by rule.  What the planner can
add with the executor as it is:

- **Choosing among goals** the modes pick by a fixed rule: which unexplored
  room, which boss, when to guard, when to eat, when to give up on a target
  and try another -- with the failure history and the teammates' failures in
  view (the modes remember only the last three failed door moves).
- **Spreading the team** (the modes have no team plan, `CPU-AI.md` gap 6):
  one Kirby guards player 1 while another scouts.
- **Reading the picture** for things the brain's tables do not carry (a
  switch, a hazard, the player in trouble), as far as a 240x160 frame shows.

What it cannot do until the executor learns it -- the top of `CPU-AI.md`'s
gaps list, which is also where the CPUs stall today (Rainbow Route
108 -> 110 / 140: doors walked at for 30 s and given up):

1. **Ability gates**: a `get_ability(N)` directive (inhale an enemy that
   gives it; `wantAbility` exists) and the room's block levels in the
   observation.  Haiku could then say "take Hammer from that enemy, then
   goto_room 110".
2. **Big switches**: a `press_switch` directive (find the switch object,
   walk onto / hit it), then re-route.
3. **Bosses**: `fight` is the stock scan with a goal on the boss; a boss
   routine (keep distance, attack windows) is executor work.
4. **Fair doors**: the executor takes doors without the lock check and
   `follow`/the modes still warp after a while (`CPU-AI.md` "Cheats").

These are marked future work; the vocabulary has room (16 kinds fit the
4-bit field, ten are used).

## The observation

One JSON line per question (`platform/cpu_planner.c` `Observe`), rendered
by the bridge into compact text (`planner_core.render_observation`) for the
model.  Fields:

- **why asked**: `start`, `ended`, `lost`, `stale`, `declined`, `no_answer`;
  and **the last directive** with its status, reason and duration.
- **the Kirby**: room, area and room-table flags (boss room, area entrance),
  tile, HP / max, ability and its block tier, its own mode, the room it came
  from, whether the room was entered before, whether the room has its own CPU
  script, whether player 1 is in the room.
- **the room as the CPU hints see it** (`CpuAiView`): every door record,
  warp-star and cannon exits included (x, y, destination, door group at this
  Kirby's tier, door tile on the map now, reachable from where it stands
  opening the gates it can, destination entered) -- the decoded
  solidity-map door records and exit objects of `CPU-HINTS.md`.  The groups
  are flooded on demand from the ROM map (`DoorGroups`: each door no
  earlier flood reached starts one; its index names it).  And the brain's
  own BFS answers (`RouteQ`, which changes nothing): the nearest room worth
  exploring and the nearest unbeaten boss room, with the first door of each
  route (these respect gates, closed doors and the Kirby's tier, so they are
  achievable).
- **objects here**: food, shards, live bosses, chests, enemies -- count and
  nearest tile.
- **blocked**: the brain's recently given-up door moves.
- **team**: every Kirby's slot, person or CPU, room, tile, HP, ability, mode
  and running directive with its target.
- **progress**: shards, rooms entered, the eight (area entrance, boss room)
  pairs and which bosses are beaten.
- **memory** (kept by the bridge): this Kirby's four earlier orders and how
  they ended, and the teammates' last four failures.
- **picture**: only when the Kirby is in player 1's room (the picture is
  player 1's camera; for a Kirby elsewhere it shows nothing about it).  The
  whole frame, not cropped (the HUD's ability and HP, and the room around
  are context), scaled 2x nearest-neighbour to 480x320 and recompressed by
  the bridge (115 KB stored -> ~21 KB): ~205 image tokens.  1x (~52 tokens)
  is small for a model to read sprites; 3x (~461) buys little more.

Size: the rendered text averaged 1,370 characters (940-1,714) over a mock
run's decisions, about 350 tokens; half the decisions carried a picture.

## Process architecture

```
 katam (native)                                  tools/haiku_ai/bridge.py
 ─────────────                                   ────────────────────────
 PortPlannerPoll, once a frame at the latch       stdin: one request per line
   event? -> Observe() -> write PNG (if on       thread pool: backend.decide()
   screen) -> {"type":"decide",...}  ───────▶      mock | haiku (anthropic SDK)
   answers waiting? (non-blocking)  ◀───────     stdout: {"id","slot","kind",...}
   -> PortInputLogScheduleCpuGoal                decisions.jsonl: observation,
   -> `cpugoal F ...` latched next frame,          directive, latency, tokens
      recorded with --record
```

- **Native**: `KATAM_PLANNER=CMD` starts `CMD` through `/bin/sh` as a child
  with a pipe each way (`planner_pipe.c`); it ends with the game (stdin
  closes).  `KATAM_PLANNER_DIR` takes the pictures.  `KATAM_PLANNER_SLOTS`
  (hex mask), `KATAM_PLANNER_FROM` (first frame), `KATAM_PLANNER_BUDGET`
  (questions a minute), `KATAM_PLANNER_SYNC` (tests).  Windows: not yet.
- **Answer handling**: matched by id; a late answer to a dropped question is
  ignored; `none` or anything invalid means "no directive" and a new
  question in 10 s; a directive that fails within 2 s backs the next
  question off 2, 4, 8, 16, 32 s (a planner asking for the impossible cannot
  storm).  A directive running 30 s past its limit is "stale": asked again.
- **The latch**: the answer is scheduled for the next latch (`lastLatched +
  1`), recorded as a `cpugoal` line, applied by `CpuAiDirect`.  The planner
  host reads the result through `CpuAiDirState` (read-only).
- **Web, later**: a page cannot hold an API key, so the same bridge would run
  on the player's machine (or a small server the player controls) and
  listen on a localhost WebSocket (`bridge.py --serve ws://127.0.0.1:PORT`,
  not built); the page's host-side planner (the same `cpu_planner.c` with a
  WebSocket `PortPlannerOps`, and `canvas.toBlob` for the picture) sends the
  same lines.  Answers enter the game the same way: a `cpugoal` input on the
  local game, a sealed event online.  `cpu_planner.c` is shared code (it
  compiles for the web build, inert without a transport); only the transport
  is per host.

## Calling Haiku

`tools/haiku_ai/haiku_planner.py`, through the official `anthropic` SDK
(2.9.0, installed into the gitignored `tools/haiku_ai/.venv`):

```python
client.messages.create(
    model="claude-haiku-5-5",
    max_tokens=2048,
    system=[{"type": "text", "text": SYSTEM_PROMPT,           # frozen, ~1,000 tokens
             "cache_control": {"type": "ephemeral"}}],
    output_config={"effort": "low",
                   "format": {"type": "json_schema", "schema": DIRECTIVE_SCHEMA}},
    messages=[{"role": "user", "content": [
        {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": ...}},
        {"type": "text", "text": render_observation(req, history, team_notes)}]}])
```

- **Thinking**: Haiku 5.5 thinks adaptively by default; `effort: "low"`
  keeps a planner tick short (no `thinking` parameter is sent; a thinking
  budget would be a 400).
- **Structured reply**: `output_config.format` with a flat schema (kind
  enum, room, group, x, y, who, secs, why -- all required,
  `additionalProperties: false`).  No forced tool call.
- **Caching**: the system prompt is byte-identical in every request (no
  timestamps, no per-run text) and above Haiku 5.5's 512-token minimum, so
  every decision after the first in five minutes reads it from the cache.
- **Response handling**: `stop_reason` first (`refusal`, `max_tokens` and
  anything but `end_turn` are errors), then the first `text` block (thinking
  blocks come before it), `json.loads`, then `normalize_directive` (the
  vocabulary, ranges, not following itself).  Any error -- the network, a
  429/5xx after the SDK's two retries, a bad reply -- answers `none`: the
  Kirby keeps its mode and the game asks again later.
- **Tests without the network** (`test_planner.py`, 13 tests): the request's
  shape (model, cached system block, effort, schema, image block of a 480x320
  PNG, no `thinking`), the cacheable prefix being identical between
  decisions, reply parsing (good, thinking-first, `max_tokens`, `refusal`,
  not JSON, no text, unknown kind, unexpected stop), and a round trip through
  the **real SDK** on `httpx2.MockTransport` (the request is built and
  serialised by the SDK and answered in-process: URL, auth header, body,
  usage and request id checked), plus a 529 from that transport becoming
  `none` in the bridge.

## Online (designed, not built)

Only the host plans.  The host's planner host runs as offline but, instead
of `PortInputLogScheduleCpuGoal`, seals a session event: a new
`PORT_LN_EV_CPUGOAL` with `a = slot | kind << 8 | secs << 16`, `b = A`,
`c = B | C << 8`, at the next sealed frame (`PortLnEvent` picks it).  Every
peer applies it in the event switch (`platform/lmpnet.c`, beside
`PORT_LN_EV_CPUAI`) by calling `PortCpuGoalApply`; a guest that already ran
the frame rewinds to it like any late input.  The directive state is in
EWRAM, so keyframes and join images carry running directives; guests never
run a planner (`PortPlannerPoll` already returns unless offline; online it
would require `PortLnRole() == PORT_LN_HOST` and not the shadow/overlay).
Left to do: the event constant and apply case, the host-side send, the
pause overlay's fork ignoring it, recording an online session's directives
(as for `cpuai`), and a rig test (`tools/netproof`) that a guest's hashes
match with directives flowing.  The host's planner latency only delays the
directive, never the session.

## Cost

Haiku 5.5: $0.10 / $0.50 per million input / output tokens; cache reads
0.1x, 5-minute cache writes 1.25x the input price.  Per decision, estimated
(no call has been made; text at ~4 characters a token, images at w*h/750):

| part | tokens | price | $ |
|---|---|---|---|
| system prompt + schema, cached read | ~1,150 | $0.01 / M | 0.0000115 |
| observation text | ~350 | $0.10 / M | 0.000035 |
| picture, half the decisions (480x320) | ~100 avg | $0.10 / M | 0.000010 |
| output: the directive (~60) + low-effort thinking (assume ~200) | ~260 | $0.50 / M | 0.000130 |
| **per decision** | | | **~$0.00019** |

A cache write (~$0.00014) happens after any five-minute pause.  Output
dominates; thinking is the uncertain term (effort `low` keeps it small).

Rate: the mock run asked 25 times in 3.6 minutes of play for three Kirbys
(~7 a minute, ~430 an hour) -> **~$0.08 an hour**.  The budget caps it at 30
a minute (1,800 an hour) -> at most **~$0.35 an hour** at these sizes; the
16-Kirby build shares the same cap unless it is raised.  Without caching the
system prompt adds ~$0.0001 a decision (~1.5x).

## Evidence

All native, headless, offline, the mock planner (`tests/cpu_haiku.sh
build/lp64/katam OLD`, where OLD is a build of `b72ca92`):

```
ok   planner unit tests: Ran 13 tests in 0.405s
ok   hub: Kirby 2 goto_room 101: done (f1988)
ok   hub: Kirby 3 goto_xy (11,14) in the hub: done (f1931)
ok   hub: Kirby 4 wait 6 s: done (f2040)
ok   hub: Kirby 4 goto_door (19,14): done (through to 101) (f2257)
ok   hub: Kirby 3 goto_room 2: failed, bad_target (f2501)
ok   hub: Kirby 2 follow player 1 from room 101 for 5 s: timeout (f3000)
ok   hub twice: 3599 frames, hash logs identical
ok   chambers: Kirby 2 goto_room 709 group 0 (its own chamber): done (f1502)
ok   chambers: Kirby 3 goto_room 704 (its chamber's door): done (f1524)
ok   chambers: Kirby 4 goto_xy (10,9) in the lower chamber: done (f1622)
ok   chambers: Kirby 2 goto_door (1,3): done (f2480)
ok   chambers twice: 2699 frames, hash logs identical
ok   none: no directive ran
ok   none: no mode ran
ok   none, replayed with KATAM_PLANNER set: 5999 frames, hash logs identical
ok   none: the replay never asked the planner
ok   none vs the old build: keys/vid/sram/pix identical on all 5999 frames; sim differs from frame 2432 (code layout: tests/layout_neutral.sh)
ok   mix (modes, no directive): the brain's notes are those of the old build (71 lines)
ok   e2e: the live run recorded 10 directives (10 decisions)
ok   e2e: 7 directives ended done
ok   e2e: the recording replayed: 7199 frames, hash logs identical
ok   e2e: a second live run recorded the same file
CPU HAIKU PASS
```

No regressions:

- `tests/cpu_ai.sh`: PASS.
- `make regress` (the goldens): every recording OK except `aim`, which
  diverges at `sim@3318` **at b72ca92 too** (before this branch).
- `tests/layout_neutral.sh b72ca92-build this-build` over every committed
  recording and a 12,000-frame CLASSIC run: all layout-neutral (keys, video,
  save and picture identical on every frame; the sim differences are host
  pointers spelled at another code layout -- `docs/DETERMINISM.md`).  A
  mode run (no directive) is not byte-identical to before by design (the
  brain's filler is laid out differently), but its `[cpuai]` notes -- every
  room, door, through, blocked, warp, hurt -- are identical.
- `lint-simstate`, `codemod_fnid --check`, `lint_determinism`: clean.  The
  16-Kirby build compiles.

A decision, end to end (mock run, `tools/haiku_ai/run.sh --frames 14400`):

```
Kirby 3 (slot 2), game frame 7687 (128 s). Asked because: ended.
Last order: goto_room 708 -> done (ok) after 5s.
Earlier orders (oldest first): goto_room 108 -> done (ok) after 14s; goto_room 103 -> done (ok) ...
You: room 708 (area 1), tile (2,23), HP 6/6, ability burning (breaks blocks up to tier 2),
own mode EXPLORER, came from room 195. Room entered before: yes. ...
Doors here: (32,2)->room 740 [group 0; open, reachable, never entered]; (43,2)->room 706
[group 0; open, reachable, never entered]; (22,28)->room 743 [group 0; closed, reachable, never
entered]; (28,28)->room 743 [group 0; open, reachable, never entered].
Nearest room worth exploring by door route: 743, first door (28,28).
Nearest unbeaten boss room by door route: 710, first door (32,2).
Team: slot 0 player 1: room 801 (23,12) HP 6 normal; slot 1 CPU Kirby 2: room 801 (24,12) ...
doing follow 0; slot 3 CPU Kirby 4: room 142 (25,11) HP 6 normal, doing goto_room 710.
...
-> {"kind": "goto_room", "room": 743, "secs": 90, "why": "nearest room worth exploring"}

[planner] f7687 k2 #14 -> goto_room 743 0 0 90s at f7687 (0 ms)
[cpugoal] f7687 k2 goto_room 743 0 0 90s
[cpuai] f7687 k2 goal 1 743 5898240
[cpuai] f7688 k2 door 708 743 386014236
[cpuai] f9488 k2 blocked 708 743 0
[cpuai] f9488 k2 goal-end 1 3 3             (kind 1 goto_room, status 3 failed, why 3 gave_up_door)
[planner] f9488 k2 goto_room failed (gave_up_door) after 30s
[planner] f9489 k2 ask #18 (ended)
[planner] f9489 k2 #18 -> goto_room 706 0 0 90s at f9489 (0 ms)
```

The 25 decisions of that run: 17 done, 4 failed, 1 timeout, 3 still running
at the end.  Kirby 3 went 101, 106, 107, 108, 103, 142, 195, 708 and 706 on
its orders; Kirby 4 timed out on its way to the boss room 710 and later
gave its door up.  The mock is a baseline, not a strategy.

## How to run it

```sh
cmake -S . -B build/lp64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKATAM_ALLOW_LP64=ON
cmake --build build/lp64 -j
ROM=~/roms/katam.gba tools/haiku_ai/run.sh --verify              # mock, headless
ROM=~/roms/katam.gba tests/cpu_haiku.sh build/lp64/katam          # the tests

# Haiku
python3 -m venv tools/haiku_ai/.venv
tools/haiku_ai/.venv/bin/pip install anthropic
export ANTHROPIC_API_KEY=...
ROM=~/roms/katam.gba tools/haiku_ai/run.sh --backend haiku --frames 7200 --verify
ROM=~/roms/katam.gba tools/haiku_ai/run.sh --backend haiku --window   # watch it, async
tools/haiku_ai/report.py build/haiku/run-*/decisions.jsonl --obs 3
```

Headless, `run.sh` waits for each answer (`--sync`: game time stops while
Haiku thinks, so a two-minute run is reproducible and quick to judge);
`--window` plays at real speed and never waits (`--async`), which is the
real thing.  Either way the recording (`live.txt`) replays without the
planner, and `--verify` proves it.

## Limits

- The executor is the modes': routes ignore door locks, some doors are only
  found to be gates by walking at them for 30 s, and `follow` keeps the
  BODYGUARD's warp.  The planner can only choose among what that can do.
- Objects (enemies, food) exist only near a player's camera, so `fight` and
  `food` mostly fail for a Kirby far from the players.
- The picture is player 1's camera; a Kirby elsewhere gets text only.
- A question asked during a door transition can describe the room being
  left (the room change is seen ~11 frames after the door is taken).
- `goto_room` into a door group checks the group on arrival (and routes to
  it); a room whose groups only join through other rooms can fail with
  `no_route` where a person would find a way.  The groups are a flood from
  each door in turn; with one-way cells (crumble blocks) a group can hold a
  door that cannot get back to the group's first door, and then arriving
  "in the group" means reaching that first door.
- Online, the web build and Windows are not wired (above).
- Token counts and latency are estimates until a real key is used:
  `report.py` prints the real usage and cost from the decision log.

## Next

1. Run it with a key: compare Haiku with the mock on the same start
   (decisions done/failed, rooms reached), and tune the system prompt and
   the observation from the decision log.
2. Executor work Haiku can then direct: `get_ability`, `press_switch`, the
   room's block levels and switch objects in the observation.
3. Online: `PORT_LN_EV_CPUGOAL`, host-only planning, a netproof rig test.
4. The web bridge (localhost WebSocket) and a page transport.
5. A team-level plan (one question for the whole team, directives for each
   Kirby) once the per-Kirby loop is understood.
