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
| Pause freezes the pauser's room | freeze mask | widened. One pauser still freezes the whole room | may one of 16 freeze the room? |
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
