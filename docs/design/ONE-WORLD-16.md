# Sixteen Kirbys in one world: what is proven

Written 2026-11-15 on branch `one-world-16` (from 5459359). It follows
`many-kirbys:docs/design/MANY-KIRBYS.md` (the plan) stage 4, which the owner
chose over linked squads: *"For 16 players, have them all on the same squad
in the same world if possible."* The full measurements, commands and raw
logs are in `netproof-eval/wide-tmp/WIDE-REPORT.md` (outside the repo,
because some of them are derived from the ROM).

## 0. The answer

**Yes, the engine runs 16 Kirbys in one world, on a desktop.** Natively and
in wasm it is deterministic, native equals wasm, and the sanitizers find no
new out-of-bounds access. The simulation costs about 2× that of four, and
online the wide build converges under rewind, join and repair.

**On a mid-range phone, 16 *people* do not fit with today's rollback.**
Replaying a frame costs about 10× simulating one. At 16 humans and 120 ms a
guest rewinds on most frames, which the model puts at about 62% of a phone
core and 12 ms spikes in a 16.7 ms frame.

8 is comfortable on desktops and marginal on phones at 120 ms (fine at
≤ 60 ms). The lever for phones is cheaper replay, not the game.

What stands between this proof and a playable 16 is not the simulation. It
is:
- the task pool;
- online seats past four and the wire format;
- the presentation (colours, sprites, HUD);
- the game's rules and the owner's decisions in §5.

What exists today (behind the flag; the default build is unchanged):

- `cmake -DKATAM_MAX_KIRBYS=N` (4..16) or `make KATAM_MAX_KIRBYS=N
  BUILD=build/wN …` builds a game with N Kirby slots.
- At N = 4 it is the live game at a different memory layout (proof W0).
- At 8 and 16 the extra Kirbys are the game's own CPU helpers. They follow
  player 1, fight, take damage and use doors. Player 1 can be a person or a
  recording.
- Offline only. Online, the wide build runs with the four existing human
  seats and N − 4 CPU Kirbys (§2.5). More than four *human* seats online is
  not built.

## 1. What changed, in one page

All new code is behind `KATAM_MAX_KIRBYS`. With it unset, every new name
spells the original constant (`KATAM_KIRBYS` 4, `KATAM_HW_SLOT(i)` `i`, and
so on). The default build is layout-neutral against the live binary and
the goldens are unchanged (§4).

- **`platform/port/wide.h`, `wide_symbols.h`, `platform/wide.c`**: the
  per-player tables move into a "wide area" at the end of the port sim block,
  sized for N:
  - `gKirbys`, animation info, `gCurLevelInfo`;
  - the room slots (solidity, tilemap, flags; one per Kirby);
  - the per-slot collision lists, the input block, the CPU brains, the pause
    menus;
  - the freeze mask and the palette-effect manager.

  Snapshots, the three state hashes, dumps and the online image already
  cover the sim block, so none of them needed a change. The sim span grows
  to 1 MiB under the flag (natively mapped; ordinary linear memory in
  wasm).
- **Exact emulation of the console's own out-of-range reads**:
  - `EWRAM_MIRROR_U8(&gCurLevelInfo[x].unk65E)` with `x` = 0xFF;
  - `gKirbys[slot]` for palette slots 4..7;
  - `gCurLevelInfo[0xFF]`, which lands in mapped zero memory as it does
    today.
- **The ROM table `gUnk_082D8D74`** holds raw pointers into the old
  `gCurLevelInfo` (risk W-1 from the plan, confirmed). It is rebuilt in the
  wide area.
- **Widened game code** (about 40 sites):
  - start-of-play creates N Kirbys and N − 1 brains;
  - room-slot choice uses a u32 mask;
  - the collision-list index becomes u16, and room slots 4.. are cleared
    every frame (they never were: the crash found at 8);
  - the freeze mask is u16 with its "everyone" bit moved to bit 15;
  - the palette-effect manager gets N + 4 slots;
  - per-Kirby local arrays are sized N.
- **Placeholders** (§5): hardware palette and VRAM slots,
  four-row ROM tables, the console HUD, the pause map, the screen-shake
  table, Kirby-on-Kirby contact, a full collision list, a full task pool,
  and a brain with no target.
- **Gates and tools**:
  - `tests/wide_w0.sh` (W0);
  - `tools/wide_unrelocate.py` (puts a wide dump back into the console
    layout);
  - `tools/wide_sanclass.py` (W1 report classifier).

## 2. The proof ladder

| gate | what | result |
|---|---|---|
| **W0** relocation at 4 | `tests/wide_w0.sh live wide4`, all 19 recordings | **PASS**: 19/19. 120,641 frames identical in keys/vid/sram/pix. 304 raw dumps equal after unrelocation; differences explained only as moved pointers and stale bytes of moved pointers |
| default build unchanged | `tests/layout_neutral.sh live new`, 19 recordings; `make regress`; `make netproof` | see §4 |
| **W1** 8, sanitizers | ASan + UBSan (`bounds-strict`, recover), all 19 recordings + 20k-frame bot soak | **PASS**: 0 new reports. 49 known (also in the default build's sanitizer baseline, or UB marked in the source) and 37 `gCurLevelInfo[0xFF]`. Earlier, longer soak: 33.7k frames clean |
| **W1** 16, sanitizers | same, 20k-frame bot soak (crowded and spread phases alternating) | **PASS**: 0 new reports; task peak 101/128, collision list peak 12/32 |
| determinism, same input twice | full hash log incl. sim, 20k frames | **PASS** at 8 and 16 |
| native == wasm | keys/vid/sram/pix every frame, 20k bot frames + play_rooms | **PASS** at 8 and 16 (0 differing frames) |
| 30-minute soak | as asked | **not run to the end**. The coordinator cut soaks to minutes. The 30-minute runs found three real defects before they were stopped (see §3); the 20k-frame runs are after the fixes |
| online, 8 Kirbys, 4 human seats | the netproof rig (quick), wide module | **all gates PASS**: R0–R6, R10, R12–R16. 2–4 devices, 120 ms with jitter and 2% loss, joins by image, corruption repair |
| online, 16 Kirbys, 4 human seats | same | 52 runs completed with every final frame identical (R0–R3, R5, R12, R13 three devices), 0 mismatches. Then **a wasm trap** (`memory access out of bounds` in `sub_08088C0C`, an effect object, during a rewind) in the R13 four-device run. **Open.** Not reproduced natively: 4 local humans + 12 CPU, 5,127 frames under sanitizers clean, 7,579 plain frames clean. Stopped there on the coordinator's instruction |

### 2.5 Online

The online engine (`platform/lmpnet.c`, `web/ln_session.js`) is built for
four human seats. `struct Ent` carries four pads, the slot masks are u8,
the wire's per-frame slot mask is 4 bits, and the seat checks stop at 3.
I did not widen it. What I ran instead is the wide build in the existing
rig: the same four seats online, with every device simulating all N
Kirbys. That measures what N costs a device online (replay, image, hash,
history), and checks that the wide build stays convergent under rewind,
join and repair. At 8, every gate passes. At 16, one open crash (table
above). **Online with more than four human seats is remaining work** (§6).

## 3. Cost

Wasm (node, render off, per frame, after boot). Crowded = all N Kirbys in
player 1's room; spread = the CPU helpers wander.

| N | scene | mean µs | p50 | p99 | tasks peak | snapshot raw | image deflated |
|---|---|---|---|---|---|---|---|
| 4 (live) | spread | 9.7 | 8.4 | 31 | 36 | 413 KB | 60 KB |
| 4 (live) | crowded | 11.1 | 9.6 | 41 | 47 | | 59 KB |
| 8 | spread | 12.0 | 10.5 | 37 | 52 | 622 KB | 68 KB |
| 8 | crowded | 13.0 | 11.4 | 44 | 67 | | 60 KB |
| 16 | spread | 20.7 | 16.4 | 75 | 75 | 840 KB | 73 KB |
| 16 | crowded | 18.7 | 16.5 | 55 | **114 / 128** | | 65 KB |

- Simulating 16 costs about 2× simulating 4. Each Kirby costs about
  0.7 µs a frame in wasm.
- The snapshot grows by about 27 KB raw per Kirby, almost all of it the
  room slot's 15.6 KB tilemap. It deflates to a few KB because empty slots
  are zeros.
- Native wall time per frame is about 180 µs at every N: the host loop,
  not the simulation (WIDE-REPORT §5.2).

**Rewind and replay against a 16.7 ms frame.** This is a model from
measured inputs (the rig's R8 and R15, the headline link); no new runs were
made for it.

- Replay cost per replayed frame, wasm on a desktop:
  - 103 µs at 4 Kirbys and 141 µs at 8 (rig R8, measured);
  - about 180 µs at 16, from the forced-rewind wall-time ratio
    7.7 : 10.0 : 13.1 s.
- Rewind depth p50 is 22 frames at 120 ms one-way (14 at 60 ms).
- Rewinds per guest are 3 a second at 2 humans and 14 at 4 (measured). For
  H humans: 60·(1 − 0.949^(H−1)) × 1.6 (calibrated): 29 a second at 8, 52
  at 16.
- Phone = desktop × 3.
- CPU Kirbys never cause rewinds; only remote *humans* do.

| humans / Kirbys | replayed f/s | desktop core | phone core | worst rewind spike, desktop / phone |
|---|---|---|---|---|
| 4 / 4 (today) | ~300 | 3% | 9% | 2.3 / 6.8 ms |
| 4 / 16 | ~300 | 6% | 16% | 4.0 / 11.9 ms |
| 8 / 8 | ~640 | 9% | 27% | 3.1 / 9.3 ms |
| 16 / 16 | ~1,140 | 21% | **62%** | 4.0 / **11.9 ms, on most frames** |
| 16 / 16 at 60 ms | ~730 | 13% | 39% | 2.5 / 7.6 ms |
| 16 / 16, replay cut to sim cost (~20 µs) | ~1,140 | 2% | 7% | 0.4 / 1.3 ms |

The sim step itself is 10–20 µs. Most of the replay cost is rollback
overhead: keyframe restore, the per-frame audio/sound judging, the hash.
That overhead, not the game, decides phones.

- Hash: 90 µs at 4, 151 µs at 8, and (by size) about 270 µs at 16, every
  16th frame.
- History, 24 keyframes: 11.6 MiB at 4, 16.4 MiB at 8, about 27 MiB at 16
  (of a fixed 192 MB). Not a problem.

Wire. The sealed stream is already sparse: per frame, a slot mask, then
2 bytes per pad that is not idle. Widening to 16 means a 2-byte mask.

- Record size, with about half the pads active: about 3 bytes at 2 players,
  5 at 4 and 19 at 16.
- Measured at 2 players and 120 ms: 13.9 KB/s down per guest and 4.6 KB/s
  up.
- Scaled by record size, 16 humans would be about 88 KB/s per guest, and
  the host uploads 15 copies: **about 1.3 MB/s**.
- With redundancy bounded to about 4 copies of each record (instead of
  "until acked", about 45 at 120 ms): about 8 KB/s per guest and about
  120 KB/s for the host.

The plan's conclusion holds: the wire must change before more than four
humans.

## 4. The default build

Shared files were touched (game headers, about 25 game sources and the
platform address macros), always as `KATAM_*` names that equal the
original constants, or as `#if KATAM_WIDE` blocks.

- `tests/layout_neutral.sh` against the live binary: **19/19 clean**, run
  after the bulk of the shared edits (`KATAM_HW_SLOT`, `FREEZE_ALL`, the
  palette-effect slots).
- The edits after that run are all identity macros or `#if KATAM_WIDE`
  blocks: `KATAM_HUD_KIRBYS`, `KATAM_ROM_ROW`, `KATAM_MIN4`,
  `KATAM_COLL_IDX`, task.c/object_collision.c/code_0800ECAC_2.c blocks,
  `warp.c`, the ASan-only `mem_posix.c` check, and the guards in
  `gba_layout.h`.
- **The final default-build gates were not re-run after those**, because
  the coordinator stopped long runs. `make regress` goldens were not
  touched. Before merging, run
  `netproof-eval/wide-tmp/tools/final_gates.sh`: layout_neutral, W0 at the
  final code, regress and netproof, in one go.

## 5. Rules: what breaks past four, what the proof does, and what is the owner's

Every item below is a **placeholder** in the wide build. Each was chosen as
the most conservative behaviour: the extra Kirbys do less, never more, and
nothing is shared or overwritten.

| rule / limit | where | what the proof does | owner's decision |
|---|---|---|---|
| Colours and sprite memory: the console has 4 Kirby palette pairs and 4 × 2 KiB of OBJ VRAM | `KATAM_KIRBY_TILES` / `KATAM_KIRBY_PAL`, `platform/wide_sprites.c` | **Done (GPU renderer).** Kirbys 4.. have their own 2 KiB of tiles and two palette lines in the sim block (`WIDE_OFF_OBJX`); the render list's sprite hook hands the GPU the real tile and line (`RlSprite.tileX` / `palHi`). OAM still says slot *i* & 3, so the classic (PPU) renderer shows that placeholder past 4. Kirby *i* wears the spray colour of its own `color` (no player-card paint past 4) | colours beyond the sprays; PPU fallback stays a placeholder |
| Four-row ROM tables: spawn offsets, respawn rooms, CPU path data, pause-map offsets, colour per player | 41 sites, `KATAM_ROM_ROW` | Kirby *i* uses row *i* & 3 | where 16 players spawn and respawn |
| The warp-star ride's sprites (the cell phone's L home, level warp stars, the goal star): a per-player variant, `+ unk56`, of which the ROM has 4 (the ability hats' animations exactly 4, Kirby's own star 8) | `kirby.c` sub_0803F790, 6 ability effects in `code_0806F780.c`; `KATAM_ROM_ROW` | Kirby *i* rides star *i* & 3 (its palette lines mirror that slot). Before this a Kirby 4.. with an ability, or any Kirby 8.., animated from past the end of the animation's data and the page's wasm trapped (`tools/netproof/lwarp_test.mjs`, `tests/wide_lwarp.sh`) | a star of its own colour past 4 |
| The console HUD keeps sprites for 4 | `hud.c` | shows Kirbys 0..3 only | HUD for 16 (the port's party HUD) |
| The pause maps' Kirby markers (the console keeps 4) | `pause_area_map.c`, `pause_world_map.c`, `KATAM_MAP_MARKERS` | **Done.** Up to 8 markers (OBJ tiles 0x3800..0x4000, below the VRAM heap): every Kirby while there are 8 or fewer, else the pauser, the screen's player, the people, then CPUs. Area map: markers 4..7 at the middles of a room's edges; world map: Kirby *i* at the ROM's spot for *i* & 3, 6 px right. The pause menus visit every Kirby in play (`KATAM_PAUSE_MENUS`), so players 5.. pause, turn pages and close | markers for 9..16 |
| Kirby-on-Kirby contact (standing on heads, sharing food) is 4 bits per Kirby in a u16 and a u8 mask | `object_collision.c` | only Kirbys 0..3 interact. The rest pass through each other | should 16 Kirbys stack, push, share food? |
| Screen shake: 4 rooms at once | `code_0806F780.c` | at most 4 rooms shake | none (cosmetic) |
| Collision lists: 32 objects per kind per room, unchecked | `code_0806F780.c` | past 31 an object skips collision that frame (counted; never reached in the soaks: peak 12) | none, if a real bound is added |
| **Task pool: 128**, and callers never check | `task.c` | when full, the object does not spawn and its writes go to scratch (counted). 16 crowded peaks at 114 in the bench, 101 in the soak; the 30-minute soak hit the limit once and crashed before this fix | **must** be raised for 16: move tasks out of IWRAM (16-bit links) |
| Difficulty `table[humans − 1]` (77 reads) | bosses | unchanged: humans stay ≤ 4 in the proof | boss HP and difficulty for 5–16 people (clamp at 4 and scale HP?) |
| The phone: one caller for the world, everyone answers | `gUnk_02021580` | unchanged: a call pulls all 15 | call your group, one person, or everyone? |
| Doors and goal star that wait for everyone | `shard.c`, `special_doors.c`, `warp_star.c` | unchanged (they wait for humans) | everyone, a majority, or the nearby group? |
| Scroll lock fires when Kirbys 0..3 are all inside | `scroll_lock.c:277` | unchanged: Kirbys 4.. are ignored | locks for 16? |
| Shards and other 4-wide loops (`i < 4`) | `shard.c` and others | Kirbys 4.. cannot collect shards | who can pick up what |
| Lives, borrowing, game over | `kirby.c` | unchanged (humans only) | game over for 16 people at once? |
| Pause freezes the pauser's room | freeze mask | widened. Offline one pauser still freezes the whole room; **online pause is each device's own** (EIGHT-SEATS-ENGINE.md "Pause per device"): the shared world never pauses | - |
| Palette effects: door fades per Kirby | `palette_effects.c` | each Kirby gets its own slot (N + 4) | none |
| CPU brains: a stone-ability brain with no target | `code_0800ECAC_2.c` | treated as "far" (the console reads BIOS through NULL) | none |

## 6. What remains, honestly estimated

Proportions, in the plan's focused engineer-weeks. Agents have moved
faster on this codebase.

| work | size | notes |
|---|---|---|
| Task pool out of IWRAM (32-bit links, or a second pool) | 1 week | required for 16 crowded; `task.c` plus `GAME_REPIN_FOCUS` |
| Renderer: palette and tiles per drawn Kirby, colours beyond the sprays | 1–2 weeks | the GPU renderer path; PPU stays 4 |
| Online seats past 4 (and the 16-Kirby rig crash, §2): lmpnet `[4]`s, localmp human mask (u8 → u16), `lmp.keys[3]`, wire slot mask (4 bits → 16), `ln_session.js`, the rig at N seats, change-mask plus bounded redundancy | 2–3 weeks | the biggest piece. Host upload grows with H − 1 |
| Cheaper replay for phones (hash and keyframe cost, sound judging) | 1–2 weeks | decides whether a phone can be in a 16-human session |
| Menus for joining 5..16, a HUD for 16, the pause map | 1–2 weeks | with the menus/HUD agents |
| Rules (§5) once decided | 1–2 weeks | small code each, many sites |
| Session page, relay settings (`maxSpectators`), presence | days | the worker already admits 20 |
| Remaining 4-wide loops (shards, cannons, cutscene triggers: 98 unrolled `gKirbys[0..3]`, 47 `gCurLevelInfo[0..3]`) | 1 week | today they silently ignore Kirbys 4.. |

Total to a playable 16 in one world: **about 8–14 weeks**. Online seats,
the renderer and the rules are most of it; the simulation itself is done.

## 7. The plan's claims, checked

| claim (MANY-KIRBYS.md) | status |
|---|---|
| tables at fixed addresses must be moved, not grown | **confirmed**; moved into the sim block |
| ROM data may point into moved arrays (risk W-1) | **confirmed**: `gUnk_082D8D74` (12 pointers to `gCurLevelInfo[i].unk180[l]`), used by the BG code |
| 74 `table[humans − 1]` reads | 77 in this tree (**confirmed**, ±) |
| 96 unrolled `gKirbys[0..3]` / 33 `gCurLevelInfo[0..3]` | 98 / **47** |
| `scroll_lock.c:277` compares four Kirbys | **confirmed** |
| Kirby *i* owns palettes *i* and *i*+4 | **confirmed**; also 2 KiB of OBJ VRAM each, at 166 sites |
| collision lists have no bound | **confirmed**; plus the lists of slots 4.. were never cleared |
| task pool "tight" at 16 | **confirmed and worse**: when full, callers corrupt IWRAM globals |
| 16 slots exceed EWRAM | **confirmed**; they live in a 1 MiB sim span |
| replay ≈ 27 µs at 4 (wasm) | the plain sim step is 10–11 µs; the rig's replay including rollback overhead is 103 µs (WIDE-REPORT §5.3) |
| replay grows ~0.07·N | sim ≈ +0.7 µs per Kirby (**confirmed** order); rollback overhead grows with the image |
| image 806 KB raw at 16 | 840 KB (**confirmed**); deflated 65–73 KB, not 85 |
| per-device rooms change the hash | untested here; the wide build hashes the whole sim block |

## 8. Published as EXPERIMENTAL > CPU KIRBYS (2026-11-15, branch `experimental`)

The owner: *"allow me to add cpu kirbys one or 4 at a time in the
experimental settings section."*

- **The setting.** SETTINGS > EXPERIMENTAL (a new top-level section; LIGHTING
  moved into it) has CPU KIRBYS (3..15 helpers; Left/Right one at a time),
  ADD 4 and REMOVE 4. Saved as `kirbys=N` (Kirbys in all, 4..16) in the
  settings text. Never online; unavailable in a local two-player game.
- **Live count (wide build only, `platform/wide.c` "The live count").**
  `gPortSim->wideCtl.target` is simulation state, set by a frame-stamped
  input (`kirbys FRAME N` in a recording; the host schedules it). Once a
  frame, in story play (not the title, demo, a pause, a door, nor the
  opening's cutscenes: `OpeningHolds`, below NORMAL an in-game interlude or
  player 1 in the hub), one Kirby joins (made as the play start makes one, its brain, then
  the respawn route to player 1) or leaves (the last one: carried to
  player 1 unless it shares a room with a Kirby that stays, then its owned
  objects and its two tasks destroyed, `gNumKirbys` down). Play starts with
  the target (at least 4). Joins wait for room in the task pool (12 slots
  of headroom). Kirby i >= 4 wears the colour of Kirby i & 3 (they share a
  palette).
- **Web.** The default module stays the 4-Kirby build. `make dist` builds a
  second module, `katam16.js` / `katam16.wasm` (`make web16`,
  KATAM_MAX_KIRBYS=16), and the page loads it when `kirbys` > 4. From the
  ordinary module, a change above 3 asks (NO / YES), writes the save and
  reloads; inside the 16 module changes are live; back at 3 the next load is
  the ordinary module. Both modules use the same save record (the save
  buffers are not among the moved tables, so the format is the same).
  `?kirbys=4` forces the ordinary module for one visit.
- **No multiplayer in the 16 module**: MULTIPLAYER > LOCAL / ONLINE
  unavailable ("SET CPU KIRBYS TO 3 FIRST"), pad drop-in refused, invite and
  netplay links and `?localmp` not followed.
- **Native**: the default build shows the rows as needing the 16-Kirby
  build; a wide native build applies the setting (or `KATAM_KIRBYS=N`).

## 9. Slot audit: reads and writes that only hold for Kirbys 0..3 (2026-11-16, branch `six-audit`)

A 7-player session trapped on the warp-star ride: `variant + unk56` into an
animation with four variants (fixed in d1d531e, §5). This section is the
search for the rest of that family across `game/src` and the platform
code that mirrors per-player state.

**Method.**
- *Static.* A taint sweep over `game/src`. Sources: `unk56`,
  `gLocalPlayerId`, `kirby - gKirbys`, `playerId`/`kirbyIdx` parameters, and
  loop counters over `gNumKirbys`/`KATAM_KIRBYS`/`PortHumanEnd()`. Sinks:
  subscripts of tables that are not widened, shifts, and sprite fields
  (`variant`, `animId`, `palId`, `unk14`, `tilesVram`). The sweep produced
  253 rows plus 86 callees that take a slot argument. Every row was read and
  classified; the scripts and their output are in the scratch directory,
  outside the tree.
- *Dynamic.* ASan + UBSan (`-fsanitize=address,undefined,bounds-strict`)
  native katam16 builds, and a new debug option `-DKATAM_ROM_CHECK=ON`
  (`platform/rom_check.c`, `PORT_ROM_CHECK_ANIM` at the 8 places
  `sprite.c` interprets an animation). The ROM is one mapped buffer, so
  ASan cannot see a read past an animation's variants. This check counts
  each animation's variants from the loaded ROM and reports, with a stack,
  the first interpretation of each (animId, variant) pair past that count.
  It was calibrated on the default build: the 19 recordings make no report.
- *Drivers.* The 19 recordings at 16 Kirbys; `tests/wide_lwarp.sh`; and two
  new generated suites with 16 local players in rooms mode:
  - `tests/wide_abilities.sh`: all 26 abilities held by Kirbys 4..15. Each
    is given by a new test input, `ability FRAME SLOT N` (inputlog.c, then
    warp.c `PortTestGiveAbility`), which gives the ability as the game
    does. Each Kirby taps and holds B, uses B with every direction, jumps
    and attacks, slides, walks and flies. It drops the ability (Select) and
    swallows what is near. Some Kirbys are on 1 HP, so they die and come
    back. In the last phase some press R (the cell phone) and L (the warp
    star home).
  - `tests/wide_stars.sh`: the level warp star for Kirbys 5, 7, 9, 12 and
    15 (15 with the Master sword), and the L star home with the Master
    sword.
  - Scenario runs for the goal star (rooms 980..982) and both cannons
    (rooms 126 and 503).

### Findings

Slots: the Kirbys the bug affects. "Exercised": how the site was reached at
run time; "read" means it was found and checked by reading only.

| site | class | slots | consequence | status | exercised |
|---|---|---|---|---|---|
| `code_0806F780.c` RequestScreenShake (~:947) | RAM array of 4 indexed past its end | any, with Kirbys in 5+ rooms | four rooms shake and a fifth asks: `unk0[-1]`, an 8-byte write before the task's struct (IWRAM heap) | **fixed** (wide only): the fifth room does not shake (the §5 placeholder, now safe) | wide_abilities: UBSan `index -1`, then a NULL Kirby in a CPU brain. Clean after the fix |
| `warp_star.c` StarShared.unkB5 (`STAR_MASK`/`STAR_ADD`/`STAR_RESET`) | u8 bitmask, bit = slot | 8..15 | a level warp star never released a rider 8..15: it stayed in the star's room. The goal star never counted, moved or released such a rider | **fixed** (KATAM_KIRBYS > 8): riders 8.. kept beside the object (`struct PortWideRide`, sim state in the wide padding; zero at N = 4) | wide_stars: before the fix 9, 12 and 15 stayed in room 142; after it they reach 195. Goal star: read (see below) |
| `eight_dir_cannon.c` EDC_IN (`EDC_HI_*`) | 8+4 bits for 16 Kirbys | 12..15 | people 13..16 could not get in an eight-direction cannon (cannons are on the way forward) | **fixed** (KATAM_KIRBYS > 12): the same side table | scenario (room 503): sanitizer-clean. The launch itself is read only |
| `code_0806F780.c` struct Unk_0808859C.unk6, sub_0808781C (~:9675) | u8 bitmask, bit = slot | 8..15 | the room-wide send after a boss or cutscene (`sub_0808859C`) never marked Kirbys 8..15 as sent, so it sent them again every frame they stayed | **fixed** (KATAM_KIRBYS > 8): u16 in the struct's padding byte (size unchanged) | read |
| `scroll_lock.c` unk4 per-Kirby bits (`SL_IN`/`SL_CAM`) | bitmask aliasing | 4..15 | Kirby i ≥ 4's "inside" bit was Kirby i−4's "camera taken" bit, and its camera bit was Kirby i+4's inside bit: one Kirby's entry skipped another's camera setup | **fixed** (KATAM_KIRBYS > 4): Kirbys 4.. use the unused `filler40` (bit i inside, bit 16+i camera) | the 19 recordings (sanitizer only; there is no behaviour check) |
| `data.h` FREEZE_ALL = 0x8000 | bitmask collision | 15 (N = 16) | bit 15 was both Kirby 15's freeze bit and "everyone": any freeze in Kirby 15's room froze every room's objects | **fixed** (N = 16 only): the mask is a u32 and FREEZE_ALL is 0x10000 (`WIDE_SZ_FREEZE`; the wide area's offsets do not move) | read; the 19 recordings stay clean |
| `code_0806F780.c` sub_080724C4 (~:1963) | NULL read (not slot-specific) | all, default build too | the inhale hitbox takes a block tile (flag 0x80000 with no object) and reads `unk6C->kind`. The console reads BIOS and wasm reads 0, so neither matches; native segfaulted | **fixed**: a NULL check with the same outcome | wide_abilities (segfault before), and the console's Kirbys 1..3 on the default build |
| `code_0806F780.c` sub_08075EDC, `MASTER_HAT_ROW` | ROM table read past a C array (not slot-specific) | all, default build too | the Master sword hat indexes gUnk_083500D8 (54 rows) by animation up to 0xB8; in ROM the table runs on into gUnk_083501B0, and the ride is row 0x5A. The port read past the array, into another global | **fixed**: rows 54.. come from gUnk_083501B0, as the ROM | wide_abilities and wide_stars (Master sword riding). ASan global-buffer-overflow before; also with Kirbys 1..3 on the default build |
| `object_collision.c:214-218` Object.unk90 | u8, 2 bits a slot | 4..15 | Kirbys 4.. never register contact; star stone blocks (`star_stone_block.c:108-111`, 0..3 only) are pushed by 0..3 only | not fixed (rule: who pushes blocks) | read |
| `kirby.c:6129` Macro_0803E920 | OAM layer by slot | 4..15 | layers 20..31 (others' Kirbys are 16..19): draw order against effects and HUD differs | not fixed (cosmetic) | the 19 recordings |
| `kirby.c:11389,11415` | speed by slot | 11..15 (7..15 for the second) | the spread after an explosion reverses direction for high slots | not fixed (cosmetic) | read |
| `code_0800ECAC_2.c:2518` | `(slot + 1) & 3` | 4..15 | a CPU's partner is a Kirby among 0..3 | not fixed (AI quality) | the 19 recordings |
| `kirby.c` sub_0803F790 and 6 hats (d1d531e) | 4 per-player star variants | 4..15 | Kirby i rides star i & 3 | placeholder (§5) | wide_lwarp, wide_stars |
| `kirby.c:16334` (stone, `(counter >> 5) + 3` into row 0x34) on the normal Kirby's table (animation 202, 3 variants) | ROM variant past its count (not slot-specific) | all | rom-check report; the console's own Kirbys 1..3 on the default build make the same one with the same moves. It is likely the test's sudden ability change (the stone's code on a table that is not the stone's) | not fixed: allowed by name in wide_abilities | wide_abilities, default-build control |

The fixes add no new placeholders. Everything above that needs a rule is
already in §5 (Kirby-on-Kirby contact, star colours, HUD, scroll lock,
cannons). Checked and safe (already widened, or provably 0..3):
- the pause maps (`KATAM_MAP_MARKERS`);
- the HUD (`KATAM_HUD_KIRBYS`);
- palettes and tiles (`KATAM_KIRBY_PAL`/`TILES`, the 41 `KATAM_ROM_ROW`
  sites, the palette-effect slots);
- `kirby_cannon.c` (u16, every Kirby);
- the 74 boss rows;
- the cutscene triggers (loops of 4 over arrays of 4);
- the sound macros (`gLocalPlayerId` only);
- link play, the demo and the sub-games (four at most);
- `input_devices.c` (players 0..3 by construction);
- `netsim.c` (4 by loop bound).

### Not exercised at run time

- The goal star with 9+ riders. Generated runs with 7+ Kirbys warped onto
  it leave some standing beside it, so it never departs; with up to 5
  Kirbys it works. The rider-mask fix is the same code as the level warp
  star's (which is exercised).
- The room-wide send after a boss (`sub_0808781C`). No recording or
  scenario beats a boss with Kirbys 8.. alive and outside a door.
- FREEZE_ALL at Kirby 15. Only the 19 recordings and the scenarios run over
  it; there is no targeted freeze in Kirby 15's room with a check elsewhere.
- What the eight-direction cannon does for 13..16 after they get in, and
  what the scroll lock's camera does for 4..15. Both were sanitizer-clean
  only.
- Mirrors, doors and bosses with Kirbys 4.. as *people*. The recordings
  cover them with CPU Kirbys 4..15 (card_boss, players_boss, warp_sweep).
- Online 8 humans: the rig suites (rig, rig-lwarp, rig-pause, rig-r16,
  web-on-eight) ran as regressions, with no new 8-human scenario.

### Gates for this class

- `tests/wide_abilities.sh` (all abilities, Kirbys 4..15).
- `tests/wide_stars.sh` (warp star riders 8.., Master sword; wired into
  `tools/full_check.sh` as `wide-stars`, building `build/w16n` with
  `tools/wide_native.sh`).
- `tests/wide_lwarp.sh` (now sanitizer-aware: it classifies with
  `tools/wide_sanclass.py`, which also learned upper-case file names and the
  source's "TODO: UB" marks).
- With an ASan+UBSan katam16 built with `-DKATAM_ROM_CHECK=ON`, all three
  plus the 19 recordings report nothing new.

### Gates run at 44b3200

| gate | result |
|---|---|
| ASan+UBSan katam16 with `KATAM_ROM_CHECK`: the 19 recordings at 16 Kirbys | 19/19 finish, 0 NEW sanitizer reports (65 known, 37 `gCurLevelInfo[0xFF]`), no rom-check report, no sprite turned away |
| the same, default build (calibration) | 0 NEW, no rom-check report |
| `tests/wide_abilities.sh`, `tests/wide_stars.sh`, `tests/wide_lwarp.sh` (sanitizer build) | PASS (wide_stars and wide_abilities failed before the fixes) |
| `tests/layout_neutral.sh` 38daa09 vs this branch | 19/19 layout-neutral |
| `tests/wide_w0.sh` (W0) | PASS 19/19, 120,641 frames identical, 304 dumps equal up to layout |
| `make regress` (native) | as baseline (aim's sim@3318: the known build-directory effect) |
| full_check: rig, rig-r16, rig-pause, rig-lwarp, web-on-eight, wide-stars, lint | 7 PASS |
