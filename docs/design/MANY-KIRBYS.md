# More than four Kirbys online: the logistics

Plan only; nothing here is built. Written 2026-11-14 on branch `many-kirbys`
(from `online` 85dceed). Scratch, scripts and raw results:
`/bigdata/agent-tom/projects/katamport/netproof-eval/many-tmp/` (ROM-derived
data stays there).

Every figure is tagged:

- **[M]** measured for this document, on this machine (Ryzen 9 9950X,
  node 22, the wasm node build of 85dceed). The script is named in the
  appendix.
- **[C]** cited from an earlier measurement, with its source.
- **[R]** read from the code: a count or a file:line.
- **[E]** an estimate. The assumptions are stated next to it.

---

## 0. The answer in plain language

The game was built for four Kirbys, and the port inherits that in about a
thousand places. The original link-cable game already did what we want, at four:
every Game Boy simulated the whole world (all four Kirbys, every room any of
them was in), and each one drew its own player's room. Our online engine is
that same design. One device (the host) decides the inputs, and everyone else
predicts and rewinds.

There are two ways to get more people into one game:

1. **Make the one world bigger** (8, 16, ... Kirbys in one simulation).
   - **8** is ordinary engineering: move and widen about 25 fixed arrays,
     fix about 150 loops and unrolled lines, and decide what eight Kirbys
     look like.
   - **16** works on desktops but is marginal on phones when a room is
     crowded. It also needs real game-design changes, such as doors that
     wait for everyone and a phone that pulls all 16 players to you.
   - **100 in one world** is not feasible on players' devices. See §4.
2. **Link several four-Kirby worlds** ("squads"). Each squad is exactly
   today's online game: up to 4 people, one host, the same proven engine.
   A light "party" layer on top does three things:
   - shares progress: treasures, shards and doors opened;
   - shows where everyone is;
   - lets you **call someone in another squad on the cell phone**. They
     leave their world and arrive in yours, the way a helper arrives when
     called.

   This scales by adding squads. **16 players = 4 squads** works with the
   relay as deployed today. **100 = 25 squads** needs a worker change and
   costs money, but nothing in the game has to grow.

**Recommendation.** Build squads first (§6, stages 1–3). That takes the game to 16
without touching the four-Kirby game code. Treat "8 in one world" as an
optional upgrade that makes squads roomier (stage 4). Treat 100 as squads
plus a worker change (stage 5). "100 people in one room fighting the same
boss" is a different product: it would need a server that runs the game,
and so the ROM on a server. I do not recommend it.

The four findings that constrain everything:

1. **Every device simulates every occupied room** [R]. Today the cost
   per frame is small (one replayed frame costs 22–32 µs in wasm [M]).
   But a guest replays roughly *(latency in frames) × (sim cost)* every
   time any remote player's input changes. With N people, that happens
   almost every frame from about N = 16 [E, §4.3]. At 100 Kirbys in one
   world a phone would need more than its whole frame budget just for
   replays [E].
2. **The four-ness is in memory layout, not just in loops** [R]. The
   per-player tables (`gKirbys`, `gCurLevelInfo`, CPU brains, input
   block, pause menus, demo buffers) sit at fixed GBA addresses with other
   data right behind them. Each room in use takes a ~19.7 KB "room slot",
   and there are exactly four (`level.c:355`). Sixteen slots alone
   (315 KB) are more than all of EWRAM (256 KB). Widening means moving
   them into the port's reserved span. That is possible, but it
   changes the memory layout every golden and hash depends on.
3. **The game's rules assume a small party** [R]. These are design
   questions, not code questions:
   - 74 difficulty reads of the form `table[humans − 1]` index 4-column
     ROM tables;
   - the phone has one global caller (`gUnk_02021580`) and summons
     *every* Kirby;
   - big doors, the goal star and a scroll lock wait for everyone
     (`scroll_lock.c:277` literally compares `gKirbys[0..3]`);
   - Kirby *i* owns OBJ palettes *i* and *i*+4 (`kirby.c:6156,6168`), so
     8 Kirbys would use all 16.
4. **Progress is small and nearly mergeable** [R]. The whole save is
   world-only, about 0.8 KB (`save.c:23-31`: door/room bits, treasures,
   shards, story state). There is no per-Kirby data in it. That is what
   makes linked squads workable. One caveat: one of those bitfields can
   be cleared as well as set (`code_080023A4.c:717-719`), so "merge by
   OR" needs checking bit by bit.

---

## 1. What exists, in one page

| piece | what it does | where |
|---|---|---|
| local multiplayer | 1–4 people in one simulation, one shared camera, one-room rule, drop in/out by slot mask | `platform/localmp.c` header; `docs/MULTIPLAYER.md` §13 |
| online engine | host seals inputs per frame; guests run ahead, predict "last sealed", rewind to a keyframe and replay; whole-image hash every 16th frame; join and repair = copy of the host's ~500 KB image | `platform/lmpnet.c` header |
| session layer | redundancy, acks, time sync, image transfer; currently 2 pads per frame (`k0`, `k1`) | `web/ln_session.js:51-91`; `struct Ent` in `lmpnet.c:134-136` |
| transport | worker room for the 4-letter code and `signal` relay; WebRTC data channels, with the relay as fallback | `web/online.js`, `web/net_session.js`, `netplay/session.mjs` |
| proof rig | gates R0–R11 | `make netproof ROM=…`, `netproof-eval/RIG-REPORT.md` |
| older designs | v2 with seats as a parameter and "area sharding" (shelved); server-side sim; puppet co-op | `docs/design/NETPLAY-V2.md` §13–14, `docs/NETPLAY-HANDOFF.md`, `docs/SERVER-RENDER.md`, branch `coop` (`docs/COOP-HANDOFF.md`) |

Work in flight that this plan depends on:

- **`slots-wt` / `online-slots`**: four seats online.
- **`rooms-wt` / `online-rooms`**: players in different rooms, each device
  with its own view, and the phone. Report due at
  `netproof-eval/ROOMS-REPORT.md`; not written yet when I checked.
- **`hud-wt`**: party HUD.
- **`card-wt`**: name and colour card.

Where an answer from `rooms-wt` changes this plan, it is marked
**[depends: rooms]**.

---

## 2. Inventory: everything that assumes exactly four

Counts are grep counts over `game/src` + `game/include` at 85dceed [R].

### 2.1 Fixed arrays and their addresses

All of these are generated macros in `game/generated/port/ram_symbols.h`.
Sizes come from the layout asserts in `platform/port/gba_layout.h`.

| what | address | size | per | lifting it |
|---|---|---|---|---|
| `gKirbys` | 0x02020EE0 | 424 B × 4 | Kirby | the next symbol (`gUnk_02021590`) starts 0x6B0 later, so the array must be **moved**, not grown |
| `gUnk_02021590` (animation info) | 0x02021590 | `[4][185]` AnimInfo | Kirby | move |
| `gCurLevelInfo` (camera, room, BG, object pool) | 0x02023530 | 1,640 B × 4 | Kirby | move |
| room-slot data (`gUnk_02024ED0` collision, `gUnk_02026D60`, `gUnk_02028EE0` tilemap, `gUnk_02023358/88/508/510/518`, `gUnk_02026D50` flags) | 0x02023358–0x020382A0 | ≈19.7 KB × 4 | **room in use** | move; see §2.3 |
| `gUnk_020382D0` input block | 0x020382D0 | 688 B, inner `[4]`s (`main.h:23-33`) | Kirby | widen each inner `[4]` |
| `gUnk_02038590` CPU brains | 0x02038590 | 244 B × 4 | Kirby | move |
| `gUnk_02038990` demo / input tape | 0x02038990 | 2 KB × 4 | Kirby | move (demo only) |
| `gPauseMenus` | 0x0203ACC0 | 20 B × 4 | Kirby | move |
| `gUnk_02022EB0`, `gUnk_02022EC0[][8]`, `gUnk_02022F40`, `gUnk_02022F50` (per-slot collision lists, 32 per kind, **no bounds check**: `code_0806F780.c:760-764`) | 0x02022EB0… | per slot / per Kirby | slot | widen and add a bound |
| `gUnk_03000510` freeze mask (bits 0–3 per Kirby, 0x10 a global flag; `object.h:226`, `code_080332BC.c:119-188`) | 0x03000510 | `u8 unk0[4]; u8 unk4` | Kirby | u32 mask with the flag moved high; 410 `Macro_0810B1F4` users stay unchanged if the macro is |
| `gMultiSioRecv`, SIO identity (2 bits, `code_080332BC.c:44`) | IWRAM | `[4]` | link cable | not used online; leave |
| const tables per player, compiled from C: `gUnk_082D8D30/40/74` (`code_080023A4.c:150-175`), `sOtherRoomKirbyIconX[4]` (`hud.c:1490`)… | ROM data in C | `[4]` | Kirby | widen; values must be designed (spawn offsets, HUD slots) |

**ROM data pointing into these arrays [M].** I scanned the ROM for aligned
words that hold addresses inside each array. In the code region (the
original ARM literal pools, which the port does not use) there are
hundreds of hits. Above 0x08160000 (data) the counts are:

- `gKirbys`: 22
- animation info: 58
- `gCurLevelInfo`: 30
- room slots: 1,740
- `gTasks`: 516
- input block, CPU brains, pause menus: 0

Most are coincidences in graphics data. The real ones I checked, such as
`gUnk_082D8D74` (pointers to `&gCurLevelInfo[i].unk180`), are defined in
C, so moving the symbol moves them too. **Unverified:** whether any ROM
table that the port reads as raw bytes holds a real pointer into a moved
array. `tools/check_ptr_arrays.py` and the census are the tools to settle
it. This is risk W-1 in §6.

### 2.2 Loops, unrolled lines, masks

- **215** `for` loops bounded by a literal 4 (`< 4`, `<= 3`, `!= 4`) [R].
  A classifier that checks whether the body indexes a per-Kirby array
  finds **40** that are per-player for certain:
  - `cutscene_trigger.c` (21)
  - `multi_08030C94.c` (5, link cable)
  - `shard.c` (3)
  - `dark_meta_knight.c` (2)
  - `level.c:48`, `code_080332BC.c:60`, `code_0800ECAC_2.c:401`,
    `boss_challenge_door.c:133`, and others

  NETPLAY-V2 §14.1's looser classifier found 56 [C]. The rest are BG
  layers, sound channels and the like.
- **96** unrolled `gKirbys[0..3]` [R]:
  - `kirby_cannon.c` 32
  - `eight_dir_cannon.c` 20
  - `code_0806F780.c` 20
  - `scroll_lock.c` 16
  - `chest.c` 2
  - `object.c` 1
  - `in_game_interlude.c` 1

  The effect macro at `object.h:50-56` tests rooms 0..3 by hand.
- **33** unrolled `gCurLevelInfo[0..3]` [R]: `code_080023A4.c` 24,
  `scroll_lock.c` 8, `code_0800ECAC_2.c` 1.
- **Local `u8 array[4]`** sized by players, which overflows past 4:
  `kirby.c:5396` (`sub_0803D46C`, enemy targeting) and `shard.c:237`.
- **Owner byte** `unk56`, 1,681 uses [R]. It is a u8, with 0xFF meaning
  "nobody", so it is fine up to 254. The bit test `Macro_0810B1F4` is
  guarded `< 32` (`object.h:226-227`). Past 31 Kirbys every mask becomes
  multi-word.
- **Room-slot choice**: a u8 mask over four slots (`level.c:345-360`, the
  loop at `:355`).
- **Collision**: `object_collision.c:586-591`. For each Kirby's object
  list, for each Kirby: O(N² · k) in the number of Kirbys.
- Loops bounded by `gNumKirbys` (136 references) and `gNumHumanPlayers`
  (123 left after the human-slot work) are correct once the arrays are
  wider, apart from the design questions in §2.4.
- **Port side** [R]:
  - `PORT_MP_PLAYERS 4`;
  - `PortLnKirby` refuses `i > 3` (`lmpnet.c:409`);
  - `struct Ent { u16 sk[2], uk[2]; }`: two pads per frame
    (`lmpnet.c:136`);
  - the wire records `k0`/`k1` (`ln_session.js:65`);
  - `lmp.slots` is a u8 and `lmpPressed[3]` (`simblock.h:64,78`);
  - `PortHumanNext` and friends stop at 4 (`port/localmp.h:96-142`).

  The sim block is already sized by `PORT_MAX_SEATS`
  (`simblock.h:25-35`).

### 2.3 Capacity limits (memory and video)

| limit | today | 8 | 16 | 100 | notes |
|---|---|---|---|---|---|
| room slots (≈19.7 KB each) | 4 in EWRAM | +79 KB | +236 KB | +1.9 MB (if 100 rooms are in use) | must live outside EWRAM. The port reserves 0x02040000–0x02080000 (256 KB) for the sim block (`simblock.h:36-37`). Natively the "EWRAM overrun" span runs to 0x020C0000. In wasm, 0x02080000–0x03000000 is ordinary unused linear memory [R]. 16 slots fit; 100 needs the native map extended |
| per-Kirby blocks (Kirby + level info + brain + animation + pause + input ≈ 5.3 KB) | 4 | +21 KB | +64 KB | +510 KB | |
| task pool | 128 (`task.h:16`). When it is full, objects silently do not spawn | — | tight | about 200 for the Kirbys alone | 16–41 tasks were in use in my runs [M]. 58 peak was measured before [C NETPLAY-V2 §14.1] |
| OBJ palettes | Kirby *i* uses *i* and *i*+4; 0xE is used for a dim effect (`kirby.c:6156,6168,11485`) | all 16 used by Kirbys, none left for enemies | impossible in hardware terms | | the port's GPU renderer can give each drawn Kirby its own palette bank (`PortKirbyColorBuild` on `coop`, `COOP-HANDOFF.md` §9). The PPU path cannot |
| OBJ VRAM for Kirbys | 2 KB each at 0x06010000 + (i << 11) (`kirby.c:6148,6160`). 0x06012000–0x06014000 is shared with effects; the object heap starts at 0x06014000 (`code_080332BC.c:40-41`) | 16 KB: collides with effects | 32 KB: all of OBJ VRAM | | assign per *viewer* to the Kirbys on that device's screen (presentation work) **[depends: rooms]** |
| OAM | 128 entries | — | — | — | already bypassed by the GPU renderer (`docs/RENDERER.md` §"Where the data comes from") |
| wasm memory | 192 MB fixed (`Makefile:41-42`, no growth) | ok | ok | 24 keyframes ≈ 45–68 MB [E] | |
| colours | 14 in the enum (`constants/kirby.h:45-58`), plus the full picker on `coop`/`card-wt` | ok | ok | duplicates | |

### 2.4 Rules that assume a small party (design, not code)

| rule | where [R] | what breaks with N > 4 |
|---|---|---|
| difficulty `table[humans − 1]` into 4-column ROM tables | 74 sites in 16 files: `dark_meta_knight.c` 16, `moley.c` 8, `king_golem.c` 7, `wiz.c`, `bombar.c`, `batafire.c` 5 each… | reads the next row past 4. Clamp at 4 and scale boss HP (an extension of `RescaleBosses`, MULTIPLAYER §13) |
| boss HP set at spawn from the count | `object.c:2081` and the rescale | ditto |
| the phone: **one** caller for the whole world (`gUnk_02021580`, `code_080332BC.c:116`); every Kirby not in the caller's room answers (`kirby.c:7064-7095`, `:10857`, `:11153`; CPU side `code_0800ECAC_2.c:2998`) | | 16 people pulled to one caller. The call state is one global for the whole world (`kirby.c:13157-13159`), so simultaneous calls by different players need a new structure. With more than four, the call needs a *target set* (your squad, or one person) |
| doors that wait for everyone (big doors, goal star, boss challenge door) | `shard.c:122,159,284`; `special_doors.c:197-225`; `warp_star.c:2003`; `boss_challenge_door.c:106` | one idle player blocks 15. Needs a quorum or squad scope |
| scroll lock fires only when all four are in the room | `scroll_lock.c:277-281` | a hard-coded 4 |
| cutscene triggers | 21 loops in `cutscene_trigger.c` | must be per group |
| lives, borrowing a life, game over when all humans are dead | `kirby.c:10633-10744` | game over for 100 people at once; borrowing from strangers |
| CPU brains: 3 at most, follow "the n-th person" | `code_080332BC.c:64`, `code_0800ECAC_2.c:2380-2410` | with more than 4 people there are no CPU helpers in one world |
| HUD: three other-Kirby icons, fixed X positions | `hud.c:1487-1493`, `DrawHudOtherKirbys` | new party HUD (`hud-wt` is on it) |
| pause menu per player; pause freezes everyone in the pauser's room | `gPauseMenus[4]`; freeze mask `code_080332BC.c:119-135` | one player pausing freezes the room for 15 others |
| sound effects play only for the local player's room | `PlaySfx`, `data.h:22-25` | per-device state inside the sim **[depends: rooms]** |
| 506 `gLocalPlayerId` reads (214 in `code_0806F780.c`, 69 in `kirby.c`, 44 in `hud.c`) | | which ones are presentation and which are simulation **[depends: rooms]** |

### 2.5 Save data

`save.c:16-31` [R]:

- file info;
- `gUnk_02028BF0` (32 B, bits **set and cleared**, `code_080023A4.c:717-719`);
- `gUnk_02028C10` (128 B);
- `gVisitedDoors` (0x240 B);
- `gTreasures` (chests, shards, spray paints, music, big chests,
  vitality, `treasures.h:14-21`);
- two encounter counters;
- story state `gAIKirbyState`.

That is about 0.8 KB, all world state and no per-Kirby state. A widened
world saves exactly as today. Linked squads need a merge rule per field
(§3.4).

### 2.6 How hard each class is

| class | examples | effort shape |
|---|---|---|
| mechanical | move arrays, widen inner `[4]`s, loops, masks, port's 2-pad structures | big but boring. The layout-neutral and hash gates catch mistakes |
| capacity | room slots, palettes, OBJ VRAM, task pool, collision lists | needs the presentation split for more than 8 **[depends: rooms]** |
| design | difficulty, phone, doors, lives, pause, cutscenes | decisions first (§7), then small code |
| infrastructure | wire format, host upload, relay | §5 |

---

## 3. Architectures

### 3.0 Comparison

| | (a) one wider world | (b) several 4-worlds, linked loosely | (c) one sim per game room | (d) **squads**: 4-worlds that trade Kirbys (recommended) |
|---|---|---|---|---|
| consistency | today's engine with N columns | each world as today; progress synced | each room's sim as today; a hand-off at every door | each squad as today; progress synced; a hand-off only on phone calls and merges |
| game code changes | all of §2 | none | big: doors and arrivals become network events | small: export/import a Kirby, apply merged progress |
| who simulates what | everyone simulates everything | each device simulates its world | each device simulates the room its player is in | each device simulates its squad's world |
| enemies when groups meet | one copy | duplicated (the puppet co-op complaint) | one copy | duplicated unless merged or shown as ghosts (decision §7) |
| phone | game's own; needs target sets past 4 | cannot call across worlds | a call is a cross-room hand-off | game's own inside a squad; across squads it moves the called player into yours |
| per-device cost | grows with N and with rooms in use | 4-player cost | ≤ 4-player cost | 4-player cost |
| host load | N − 1 streams | 3 | ≤ 3 per room | 3 per squad |
| practical ceiling | 8 easily; 16 on desktops | any | any, but max 4 (8) per room | any, but max 4 (8) per squad |

### 3.1 (a) Widen one simulation to N Kirbys

This is how link play worked, and how online works now: every device runs
the complete world, and each draws its own player.

- **Consistency**: unchanged. The host seals N pads per frame, guests
  predict and rewind, and the whole image is hashed. The rewind engine
  composes directly. The wire needs N columns, and `struct Ent` with the
  session messages needs widening (§2.2 port side).
- **Who simulates what**: every device simulates every occupied room.
  Today single player already runs 4 occupied rooms most of the time: in
  a 20,000-frame single-player run, 72% of sampled frames had the four
  Kirbys in four different rooms [M]. More rooms is not the expensive
  part. Objects are, and they scale with rooms in use (§4.2).
- **Per-device views [depends: rooms]**: each device draws its own room,
  so VRAM, OAM, palettes, part of IO, the sound engine and anything keyed
  on `gLocalPlayerId` differ between devices. The whole-image hash
  (`lmpnet.c:286-330`, which hashes VRAM/OAM/PLTT/IO) and the image copy
  must then cover simulation state only. Whatever `rooms-wt` does for 4
  carries over to N unchanged.
- **Phone**: the game's own, plus target sets once there are more than 4
  (§2.4).
- **Enemies, bosses, difficulty**: one copy for everyone (good). Clamp the
  74 table reads at 4 and scale boss HP by N/4 (a decision).
- **Progress and saves**: unchanged, in the host's SRAM.
- **Join / leave**: unchanged in kind (image copy, human slot bits). Slot
  masks become wider than 8 bits.
- **Trust**: the host is authoritative for everyone.
- **Host and relay load**: the host uploads N − 1 streams (§4.4).
  Rewinds grow with N (§4.3).
- **Verdict**: 8 is mechanical. At 16, phones in a crowded session are
  marginal and the design rules start to fail. At 100 it is infeasible on
  CPU, host upload and gameplay alike (§4).

### 3.2 (b) Several four-Kirby worlds, linked loosely

Each group plays its own world (today's online game). A party layer shares
progress and presence; other groups' Kirbys are at most drawn as puppets
(the `coop` branch already draws puppets and name tags).

- Consistent within a world. Between worlds only progress is shared,
  through sealed POKE-style events applied by each world's host. The rig
  already has POKE events (`tools/netproof/scenario.mjs:12`).
- There is no way to *play together* across groups. That makes it a
  stepping stone, not a product. It is (d) without the hand-off.

### 3.3 (c) One simulation per game room

KATAM rooms are nearly self-contained:

- a room's slot is loaded when its first Kirby arrives. It is free for reuse
  once no Kirby is in that room, because the free-slot mask is rebuilt from
  the other Kirbys' current rooms (`level.c:332-376`);
- a fresh load of the room spawns its objects again (the decompress-and-spawn
  path above);
- `FillLevelInfo` already hands objects over when a Kirby leaves
  (NETPLAY-V2 §14.1).

So a "room simulation" could live exactly as long as someone is in the
room. Every door becomes a hand-off: the Kirby is sealed out of one room
simulation at frame F and imported into the next. That target simulation
is either created locally if nobody is there, or joined by image copy if
someone is.

- Good: one copy of every enemy, and no duplicate bosses.
- Bad:
  - every door becomes a network round trip, plus an image fetch when the
    room is occupied (roughly 60 KB deflated today, §4.1);
  - the game's own door, fade, camera and CPU-brain logic would need an
    "arrive from the network" path;
  - each room simulation is a full game instance with four Kirby slots,
    so a room still holds at most 4 (8 if widened);
  - global rules (the phone, doors that wait for everyone, boss scaling)
    become cross-simulation protocols.
- Verdict: the right shape for an MMO-style 100, but the most engineering.
  The hand-off primitive it needs is the same one (d) needs, so (d) is the
  way to get there.

### 3.4 (d) Squads: four-Kirby worlds that trade Kirbys

A **squad** is one of today's online sessions: up to 4 people (CPU helpers
in empty slots), one host, sealed inputs, rewind, hash, repair. A
**party** is the set of squads in one session. The party layer does
five things:

1. **Shared progress.** The party keeps the canonical world props (about
   0.8 KB). When a squad host sees a prop change in its world, it reports
   it. The party merges and broadcasts the result. Every squad host seals
   the merged bits into its own world as an event at a frame it chooses,
   so each squad stays byte-identical with its guests.
   - Merge rules: OR for treasures, shards, doors visited and chests; max
     for the story state.
   - `gUnk_02028BF0`'s clearable bits need a rule each. The safe default
     is last-writer-wins, carried as an event.
   - Rooms already loaded do not re-read every flag until re-entry. That
     is acceptable and matches how the game treats saves.
2. **Presence.** Each squad host publishes `(player, room, x, y, ability,
   hp)` for its Kirbys a few times a second. Every device can then:
   - show everyone on the pause map and the party HUD;
   - optionally draw other squads' Kirbys in the same room as **ghosts**
     (no collisions or damage; the puppet drawing from `coop`).
3. **The cell phone.**
   - Inside a squad it is the game's own phone, as in link play.
   - Across squads, the phone offers a contact list. Calling a player
     asks them; on accept, their Kirby is **exported** from their squad
     (it becomes a CPU there, or the squad dissolves) and **imported**
     into yours in a free slot, arriving with the phone's warp-in.
   - "Kirbys can call each other on the cellphone" maps onto exactly this.
4. **Merging on contact** (optional): when two squads meet in a room and
   the seats fit, offer a merge, which is the same move as a call.
5. **Solo members** are a squad of one: single player with CPU helpers,
   linked to the party.

The new primitive is **Kirby export/import**. The source host seals
"Kirby k leaves" at frame F. The record goes to the destination host:

- position, room, ability, HP, lives, battery, colour and name: well
  under 1 KB; the Kirby struct alone is 424 B [R];
- or alternatively 5.3 KB raw / about 0.45 KB compressed for everything
  per Kirby [C NETPLAY-V2 §14.5].

The destination host seals "slot s becomes person p, at this state" at
its frame G. Both sides use paths that already exist:

- the human-slot switch `PortLmpSetSlotHuman`;
- the respawn and warp route `PortWarp`;
- the brain reset `sub_0800EE04`.

The only new code is applying the ability, HP and lives fields as a
sealed event.

- **Consistency**: every squad is exactly today's engine. Nothing crosses
  between squads except sealed events, so there is no new desync class.
- **Who simulates what**: your device simulates your squad's world and
  nothing else. CPU, memory, image size, rewinds and bandwidth are the
  4-player figures whatever the party size.
- **Per-device views**: as for 4 **[depends: rooms]**.
- **Enemies and bosses**: per squad. Scaling uses the squad's own count
  (1–4), so the game's tables stay valid. When two squads are in one room
  their enemies are separate copies. Three policies (decision §7.2):
  - ghosts in a "parallel dimension". KATAM is about mirror worlds, so
    this fits the theme;
  - merge when seats allow;
  - rooms that refuse a second squad.
- **Progress and saves**: canonical props live with the party leader, or
  in the worker's storage (well under 1 KB, decision §7.6). Each squad
  host's SRAM saves its world as today, with the merged props in it.
- **Join / leave**: join the party, then a squad with a free seat or a
  new squad of one. Leaving: the existing drop-out to the CPU. **Host
  leaving** must become host migration (the guest with the newest sealed
  frame takes over). It matters at 4 already, and at 25 squads it happens
  every session.
- **Trust**: authority over play is local to the squad. A malicious host
  can only spoil its own squad's world. Into the party it can inject
  progress flags (monotonic unlocks). For friends, fine. For public play,
  the party layer can sanity-check flags (a boss flag only after that
  boss's room was entered, and so on).
- **Host and relay load**: the 4-player figures per squad, plus presence
  (§5).

### 3.5 Hybrids worth keeping in mind

- **(d) with 8-seat squads**: stage 4 widens the world to 8, so merges fit
  more often and 8 friends can share a world. It costs the §2.1–2.3 work
  plus palettes.
- **(d) drifting toward (c)**: if "players in the same room should fight
  the same enemies" becomes a requirement, auto-merge on contact moves
  (d) toward (c) one hand-off at a time. Everything (d) builds is reused.

---

## 4. Numbers

### 4.1 Measured now

| quantity | value | tag / source |
|---|---|---|
| keyframe / image, raw | 506,084 B | [M] `kfBytes`; matches RIG-REPORT R5 [C] |
| image deflated | 52.5 KB p50, 60–71 KB at a repair | [C] RIG-REPORT R4/R8 |
| what the deflated image is made of (deflate-6, single player / 4 humans) | EWRAM 12.8 / 10.5 KB; IWRAM 4.9 / 5.0; **VRAM 44.2 / 43.8**; palettes 0.8 / 0.7; OAM 0.1 | [M] `simcost.mjs` |
| …inside EWRAM | room slots (78.8 KB raw) 7.4 / 5.7 KB deflated, i.e. **≈1.4–1.9 KB per slot**; Kirbys + level info + brains ≈ 1.0–1.5 KB for all four | [M] |
| replay cost, one frame, guest, wasm, picture off, state-only audio | **22.5 µs** (forced rewinds, 24,211 frames), **31.9 µs** (120 ms link, 1,203 frames) | [M] `rig/cost.mjs` (the rig with per-frame hashing turned off) |
| keyframe restore / save | 10.9–19.6 µs avg, ≤ 54 µs max / 17–29 µs avg | [M] |
| full-image hash | 109–110 µs avg (every 16th frame, so ~7 µs/frame) | [M] |
| sim per frame, wasm, including the JS step | 6 µs (RIG-REPORT R8 [C]); 15–16 µs p50 in my bench including my own bookkeeping [M] | the step overhead hides any difference between 1 and 4 occupied rooms [M] |
| where wasm time goes (sampling profile, single player / 4 humans; shares only, the profiler inflates absolute times) | Kirby code 6.9 / 5.5%; CPU brains 4.0 / —; objects + collision + sprites + level ≈ 15 / 15%; **DMA 21 / 18%** (it grows with Kirbys on screen, +2 µs at 4 visible); frame present 20 / 19%; sound 4 / 5% | [M] `prof2.py` |
| occupied rooms, single player | 4 distinct rooms on 72% of frames, 2 on 23% | [M] |
| tasks in use | 16–41 of 128 | [M] |
| rewinds at 2 players | 154 / min (0 ms link, scripted plus bot input); 325 / min (bot, 120 ms) | [C] RIG-REPORT R7 |
| rewind depth p50 by one-way latency | 6 (0 ms), 10 (30), 14 (60), 21 (120), 30 (200) frames | [C] R7 |
| bytes per second at 2 players, 120 ms | up 4.6 KB/s, down 16.4 KB/s (guest) | [C] R7 |
| how often a real player's buttons change | mean run 22.8 frames on Nintendo's 24 attract tapes, so **p ≈ 0.044 per frame**; the busiest tape ≈ 0.085 | [C] MULTIPLAYER §9 |
| relay at 4/8/16/100 synthetic clients (v2 protocol, dev relay) | all streams identical; sealed stream 2.1 KB/s per client at 100; fan-out 6,167 msgs/s at 100 | [C] NETPLAY-V2 §13 |

The predicted rewind rate checks out. The model below gives 2.6 rewinds/s
(156/min) at two players with p = 0.044, against 154/min measured.

### 4.2 How the simulation grows [E]

The model, from the measured replay cost (27 µs at 4 Kirbys, the midpoint
of 22.5–31.9):

    replay(N) ≈ 27 µs × (0.72 + 0.07·N) + 0.5 µs × (rooms − 4) + Q

- The per-Kirby share (0.07·N, 28% at N = 4) is from the profile, and
  matches NETPLAY-V2's native split (1.9 µs fixed + 0.23 µs per Kirby
  [C]).
- 0.5 µs per extra room is the native per-room increment [C §14.2]
  doubled for wasm.
- Q is enemies × Kirbys targeting: 50–100 µs at 100 Kirbys, 50 rooms and
  250 enemies (NETPLAY-V2's native 25–50 µs [C] doubled). It is zero
  below 16.
- A mid-range phone is taken as 3× slower (NETPLAY-V2's Geekbench ratio
  [C]). Low-end phones can be 5–6×.

| N | rooms in use | replay µs/frame, desktop wasm | phone ×3 |
|---|---|---|---|
| 4 | 4 | 27 | 81 |
| 8 | 8 | 37 | 110 |
| 16 | 16 | 56 | 167 |
| 100 | 50 | ~306 (200–450) | ~920 |

### 4.3 How rewinds grow with players [E]

A guest rewinds when any other player's sealed input differs from its
prediction ("last sealed"). With N − 1 other players changing input
independently with probability p per frame, and one sealed packet per
frame:

    rewinds/s = 60 · (1 − (1 − p)^(N − 1)),   replayed frames/s = rewinds/s × depth

The table uses p = 0.044 (typical; busy players, p = 0.085, are in the
scratch output). "Spike" is the worst single frame: one rewind of full
depth.

| N | rooms | latency (one way) | rewinds/s | replayed f/s | CPU, desktop | spike, desktop | CPU, phone ×3 | spike, phone |
|---|---|---|---|---|---|---|---|---|
| 2 | 2 | 120 ms | 2.6 | 55 | 0.1% | 0.5 ms | 0.4% | 1.5 ms |
| 4 | 4 | 120 ms | 7.6 | 159 | 0.4% | 0.6 ms | 1.3% | 1.7 ms |
| 8 | 8 | 120 ms | 16.2 | 340 | 1.3% | 0.8 ms | 3.8% | 2.3 ms |
| 16 | 16 | 60 ms | 29.4 | 412 | 2.4% | 0.8 ms | 7.1% | 2.3 ms |
| 16 | 16 | 120 ms | 29.4 | 618 | 3.5% | 1.2 ms | 10.5% | 3.5 ms |
| 100 | 50 | 60 ms | 59.3 | 830 | 26% | 4.3 ms | 77% | 12.9 ms |
| 100 | 50 | 120 ms | 59.3 | 1,245 | 39% | 6.4 ms | **116%** | **19.3 ms** (a frame is 16.7 ms) |

What this means:

- **Up to 16 the CPU is fine.** Even on a phone the replays take about a
  tenth of a core.
- **At 100 in one world a phone cannot keep up.** A desktop spends a third
  of a core replaying.
- What does grow at 16 is the **feel**. Every remote Kirby on screen is
  corrected about 2.6 times a second (p × 60), each correction a jump of
  tens of pixels at 120 ms (p90 37 px [C R7]), and there are 15 of them.
  Smoothing (an open "known gap") becomes mandatory.
- In squads all of this stays at the N = 4 row.

### 4.4 Image, history, hash and wire [E]

Image model: 506 KB + 19.7 KB per extra room slot + 5.3 KB per extra
Kirby (raw); +1.7 KB / +0.4 KB deflated.

| N | slots | image raw | image deflated | history (24 keyframes) | hash per 16 frames |
|---|---|---|---|---|---|
| 4 | 4 | 506 KB | ~60 KB | 11.9 MB | 110 µs [M] |
| 8 | 8 | 606 KB | ~68 KB | 14.2 MB | ~130 µs |
| 16 | 16 | 806 KB | ~85 KB | 18.9 MB | ~175 µs |
| 100 | 50 / 100 | 1.9 / 2.9 MB | ~180 / ~260 KB | 45 / 68 MB (of a fixed 192 MB) | 420 / 630 µs |

If per-device views take VRAM out of the image **[depends: rooms]**, the
deflated image loses about 44 KB of its ~60 [M].

**The sealed stream per guest**, today's format generalised (5 B per frame
record, i.e. flags plus 2 B per player), with the host resending every
unacknowledged record until acked:

- About 45 records per packet at 120 ms, calibrated from the measured
  16.4 KB/s down at 2 players.
- A "change-mask" format, as protocol v2 has (`ceil(N/8)` bytes plus 2 B
  per changed pad), costs less.

| N | per guest, today's format | per guest, change-mask | host upload (N − 1 copies), today / change-mask | host upload, change-mask + each record sent ~4× instead of until acked |
|---|---|---|---|---|
| 4 | 27 KB/s | 9 KB/s | 81 / 27 KB/s | 6 KB/s |
| 8 | 49 KB/s | 10 KB/s | 340 / 70 KB/s | 15 KB/s |
| 16 | 92 KB/s | 15 KB/s | **1.4 MB/s** / 220 KB/s | 40 KB/s |
| 100 | 545 KB/s | 64 KB/s | 54 / 6.4 MB/s | **0.7 MB/s** (5.5 Mbit/s) |

Before anything past 4 ships, the wire must change: a change mask plus
bounded redundancy with a NACK for repair. With that, 16 in one world is
fine on any broadband host. At 100, a home host's upload is the next wall
before the CPU is. In squads each host uploads the N = 4 row.

---

## 5. Infrastructure

### 5.1 What the deployed worker already does [R]

- **Members per room.** A room admits `seats + maxSpectators` members:
  4 + 16 = 20 by default (`netplay/session.mjs:60,401`). The host can
  raise both in the lobby phase (`settings`, up to 1,024 seats and 1,000
  spectators, `:543-558`). **No redeploy is needed for 16 or 20 members.**
- **`signal`.** It is unicast to a member id (`session.mjs:501-507`), with
  a 64 KiB message limit (`:213`). It carries the WebRTC introductions,
  and today it is also the fallback transport for all game traffic
  (`web/online.js`, base64 in JSON).
- **Rosters.** They are O(N) and coalesced above 8 members
  (`session.mjs:340`).
- **The host-side guard.** `online.js` answers a second guest with
  `busy` (`web/online.js:542`). That is page logic, not a worker limit.
- **Billing.** The room is a Durable Object with non-hibernating
  WebSockets (`server.mjs:284`, `ws.accept()`). Duration is billed per
  room DO while any socket is open, so cost scales with
  **session-hours, not players**. Relay-fallback traffic adds per-message
  request billing. (Cloudflare's current prices were not checked for this
  document. Verify before deciding.)

### 5.2 Peer-to-peer limits

- **Star from the host.** Today one host holds N − 1 data channels.
  Browsers allow far more peer connections than 15. The binding limits
  are the host's upload (§4.4) and a phone host's CPU for DTLS/SCTP
  (not measured).
- **NAT.** Only STUN is configured (`stun.cloudflare.com`). Any pair that
  cannot connect directly falls back to the worker relay, and the more
  members, the likelier at least one is relayed. A TURN service (e.g.
  Cloudflare's paid TURN) is an alternative to the DO relay for heavy
  links. That is a cost decision.

### 5.3 What 16 needs, and what 100 needs on top

| | 16 (4 squads, or one world of 16) | 100 (25 squads) |
|---|---|---|
| room and introductions | deployed worker, host raises `maxSpectators` | same; 100 members is within the limits as written. Not load-tested on the deployed worker (the v2 test ran the dev relay and local workerd [C]) |
| party layer (progress, presence, calls) | the 4 squad hosts talk to each other directly (6 WebRTC links, or `signal` unicast). About 4 hosts × 5 Hz × 3 peers = 60 msgs/s. **No worker change** | `signal` is unicast, so presence through it is O(N²): 100 × 5 Hz × 99 ≈ 50,000 msgs/s. **Needs a worker change**: a party channel where each squad host posts its squad's presence at 5 Hz and the DO broadcasts one combined frame (25 in, 100 out, about 1.2 KB each, ≈ 600 KB/s out of one DO) [E] |
| canonical progress | party leader's device | the DO (well under 1 KB of storage per party), so it survives the leader leaving |
| relay fallback | a few members | at, say, a 10% relay share, 10 members × 60 packets/s ≈ 600 msgs/s through one DO; sustained, that is money and the DO's single-thread budget. TURN or a relay tier (NETPLAY-V2 §13) |
| moderation | friends | names, kicking, abuse. A product decision |
| host migration | needed | essential (25 hosts per session) |

---

## 6. Roadmap

Effort is in focused engineer-weeks, for relative size. Agents have
moved faster on this codebase (online v1 went from proof to shipped in
about two days), so treat these as proportions with wide error bars [E].

### Stage 0: today, in flight elsewhere (entry for everything below)

- 4 seats online (`slots-wt`).
- Per-device rooms, views and the phone (`rooms-wt`). This decides what
  the hash and image cover.
- Party HUD (`hud-wt`) and the name/colour card (`card-wt`).
- **Exit**: their gates pass on `online`, and the rig runs at 4 seats.

### Stage 1: session plumbing for more than 4 (1–2 weeks)

- An N-column sealed record with a change mask, bounded redundancy and
  NACK repair (§4.4).
- **Host migration**: a guest holding every sealed frame below F becomes
  host at F, and the others re-anchor.
- A party roster in the room (names, colours) and `maxSpectators` raised
  by the host.
- **Proof before building**: the rig's link model at 4 seats with the new
  format. Bytes/s per guest must match the §4.4 model within 25%.
- **Gate P1**: R0–R6 pass at 4 seats over the new wire.
- **Gate P2**: kill the host at 50 random frames; the session continues,
  every final frame is identical, the stall is ≤ 1 s, and there are 0
  repairs beyond one image.
- Risk: host migration interacts with sealed events and the sound
  judging in `lmpnet.c`.

### Stage 2: the Kirby hand-off primitive (1–2 weeks)

- Export a Kirby at a sealed frame; import it into a free slot of another
  world at a sealed frame (§3.4).
- **Proof before building**: in one process, two instances (two worlds).
  Move a Kirby back and forth 200 times at random frames: mid-air, with
  every ability, at 1 HP, at 0 lives, in a cannon, in a goal sequence, in
  the intro.
- **Gate T1**: the record arrives identical; both worlds stay identical
  with their guests (R2-style compare); no Kirby is stuck or duplicated;
  the arrival uses the phone's warp-in.
- **Gate T2**: the same inside the netproof rig over the 120 ms link.
- Risk: states that cannot be moved yet (doors, cannons, cutscenes). The
  `lmp.joining` logic in `localmp.c` already solves "move when movable".

### Stage 3: squads and the party (3–5 weeks). This is "16 players"

- Party layer: progress merge (field-by-field rules, §2.5), presence,
  the cross-squad phone (contact list, accept, hand-off), merge on
  contact (if chosen), ghosts (if chosen; the `coop` puppet drawing).
  Pause map and party HUD show everyone.
- **Proof before building**: the merge rules on recorded progress. Replay
  the existing recordings as four squads with different routes; check
  every world's props converge to the merge and that no squad's world
  diverges from its guests. Settle `gUnk_02028BF0`'s clearable bits first.
- **Gate S1**: 4 squads × 4 synthetic players for 30 game minutes. Every
  squad passes R2 (all final frames identical), and progress converges
  within 1 s of any change.
- **Gate S2**: 200 cross-squad calls; T1 holds for each.
- **Gate S3**: per-device CPU, bytes and rewinds equal the 4-player
  figures within 10%.
- **Gate S4**: two real devices plus 14 synthetic clients on the deployed
  worker, with no worker change.
- Risks: progress applied mid-room shows inconsistently until re-entry;
  griefing through flags (§3.4 trust).

### Stage 4 (optional, can run beside 3): 8 Kirbys in one world (3–6 weeks plus design)

- `KATAM_MAX_KIRBYS` build flag:
  - move the §2.1 arrays to the sim span through `ram_symbols.h`;
  - widen the inner `[4]`s, the masks, the 40–56 loops and the 129
    unrolled lines;
  - give the room slots N entries and bound the collision lists;
  - apply the policy decisions (§7) for difficulty, phone targets and
    doors;
  - renderer-side palettes for Kirbys beyond 4.
- **Proof before building (W0)**: relocation only, still at 4. Move
  `gKirbys`, `gCurLevelInfo` and the room slots to new addresses. The
  run must be identical apart from layout (`tests/layout_neutral.sh`
  against main; goldens identical in every non-sim column). This retires
  risk W-1 (ROM data pointing at the old addresses, §2.1) before the real
  work starts.
- **Gate W1**: 8 bots for 30 minutes on a native ASan/UBSan build with
  `PORT_OVERRUN_TRAP=1`: no out-of-bounds; the wasm and native hash logs
  are equal.
- **Gate W2**: the rig at 8 seats with synthetic peers: R0–R6 pass.
- **Gate W3**: on a real mid-range phone, the p99 frame including replays
  is < 4 ms at 120 ms latency (model: 2.3 ms).
- **16 in one world** is the same flag set to 16, plus OBJ VRAM per
  viewer **[depends: rooms]** and a task pool moved to EWRAM. Gate it on
  W3 at 16 (model: 3.5 ms) and on a playtest of the design rules.
- Risks:
  - the goldens all change (a layout move);
  - palettes and colours;
  - crowded rooms are not fun at 240×160 (the port's wide views help).

### Stage 5: 100 = 25 squads (3–6 weeks, plus running costs)

- A worker party channel (presence aggregation, progress authority,
  persistence), TURN or a relay tier for relayed members, moderation, and
  party-scale UI (find a player, call, squad list).
- **Proof before building**: a load test with 100 synthetic clients on
  `wrangler dev` and then on a test deployment the user starts. Measure
  DO messages/s, CPU time, p99 presence latency and the bill for one
  2-hour session.
- **Gate L1**: p99 presence < 250 ms; no squad affected by the party's
  load; projected cost within the user's budget.

### Where feasibility ends

- **One world**:
  - 8: yes;
  - 16: yes on desktops, marginal on phones, and needs design changes;
  - beyond 16: no. Phone CPU (§4.3), host upload (§4.4) and the rules
    (§2.4) all fail.
- **Squads**:
  - 16: yes, with the deployed worker;
  - 100: yes, with a worker change and running costs;
  - beyond about 100: the party channel needs sharding by area. That is
    product work of its own.
- **Everyone in one place, sharing enemies, at 100**: a different product.
  It needs a server-authoritative simulation. That cannot run in a
  Durable Object: the memory map needs more than 128 MB
  (`docs/SERVER-RENDER.md` §4b). So it needs a container or a server,
  **with the ROM on that server**. Then it needs interest management. Not
  recommended.

---

## 7. Decisions that are the user's

1. **Which experience to aim for**: "up to 16 friends in squads of 4 who
   call each other" (stages 1–3), "8 together in one world" (stage 4), or
   both.
2. **When two squads meet in a room**: ghosts in a parallel world
   (no interaction), automatic merge when seats allow, or rooms that
   refuse a second squad.
3. **What progress is shared**: everything (one squad beating a boss
   counts for all), or only collectibles. And what happens when one squad
   finishes the game.
4. **The phone across squads**: does a call move the callee into your
   squad or you into theirs? Who is bumped when the squad is full? Can a
   player refuse calls?
5. **Inside a bigger world (stage 4)**: does a call summon everyone or
   only your group? Do doors wait for everyone, a majority or the group?
   Is boss HP scaled past 4? Does one person pausing freeze the room?
6. **Where the party's progress lives**: the leader's device, or the
   worker (a redeploy and a little storage).
7. **The worker for 100**: redeploying a changed worker, TURN or a relay
   tier, and the running cost.
8. **Public or friends-only**: trust, names and moderation.
9. **Dropping GBA hardware limits for more than 4 in one world**:
   palettes and VRAM handled by the renderer, no PPU fallback for those
   sessions. This is in line with the stated "no GBA fidelity" direction,
   but it is a visible change.

---

## Appendix: how the measurements were made

All in `netproof-eval/many-tmp/`, against `build/katam-node.js` built from
this worktree (85dceed, `make build/katam-node.js`):

- `tools/simcost.mjs MODULE ROM OUT [sp|mp4]`: 12,000–20,000 frames,
  boot keys, then the rig's human-like bot (`tools/netproof/scenario.mjs`)
  on every human, player 1 warped through the rig's 18 rooms every 560
  frames. It logs per-frame step time, distinct rooms and tasks, and
  deflates each region at frame 9000.
- `node --cpu-prof --cpu-prof-interval 25` over the same, summarised by
  `tools/prof.py` and `tools/prof2.py`: function self-time grouped by
  source file. Only the shares are meaningful.
- `rig/cost.mjs`: a copy of `tools/netproof` with the forced per-frame
  hashing removed (`run.mjs:57,67`). It runs one 6,000-frame session at
  120 ms ±30 jitter and 2% loss, and one with forced 20-frame rewinds
  every 7 frames, then reads the guest's telemetry (`catchUpUsSum /
  replayedFrames`, keyframe and hash timings).
- `tools/romptr.py ROM [from]`: aligned ROM words pointing into each
  per-player array, optionally only above an offset.
- `tools/loops.py game/src`: for loops bounded by 4 whose next 25 lines
  index a per-player array with the loop variable.
- `tools/model.py`: the §4.2–4.4 extrapolations, with every input labelled.
