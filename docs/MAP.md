# Where the game actually is

A map of the whole thing, measured 2026-08-04 rather than remembered, written
to answer one question: **if we want to redesign this, what are we redesigning
and where does it live?**

The short answer is that the game is not in this repository, the parts of it
that are hard to change are not the parts that look hard, and the thing
standing between us and level editing, mod support and new enemies is not the
architecture.

---

## 0. Two repositories, and only one of them is the game

| | lines | what it is |
|---|---|---|
| **`katam-port`** (here) | 13,393 C + 1,168 Python | host, platform, tooling. **Zero game logic.** |
| **`katam/src`** (the decompilation, `$KATAM_DECOMP`) | 180,796 C in 153 files | the finished game |
| **`katam/wip`** | 12,511 C in 7 files | C bodies for what is still assembly upstream |
| **`katam/asm`** | 49,511 lines in 12 files | what the GBA build actually links |
| **`katam/data`** | 14.4 MB | tilesets, maps, levels, sound |

`wip/` deserves its own line. The GBA build keeps it out of the link because a
partially-decompiled file cannot byte-match; the **port links it instead of the
assembly**, which is why the decomp's HEAD reads *"PORT GATE IS ZERO: every
real function in the game now has a C body"*. So the port already runs a game
that is 100% C even though the cartridge build is 92%. `wip/sprite.c` is
local-only and is never opened as a pull request.

Nothing here ever edits the decompilation. `make sync` runs
`tools/portify.py`, which copies the decomp into `build/port-src/` applying
exact-text patches, and the port builds from the copy. That indirection is the
most important fact on this page for redesign purposes: **it is a seam we
already own, that survives the decomp moving underneath us, and that reports
loudly when a pattern it depends on stops matching.** The randomizer is one
patch through it. So is the widescreen view.

Everything below with a `src/` or `asm/` prefix is in the decompilation.
Everything with `platform/` or `tools/` is here.

---

## 1. Coverage: what state the source is actually in

```
1,413,968 bytes of code    92.08% in C      7.92% still ARM assembly
   41,548 symbols          16.27% named    83.73% still sub_XXXXXXXX / gUnk_XXXXXXXX
14,386,873 bytes of data     8.79% in src   91.21% in data/*.s
                                            68.59% of all data is unlabelled .incbin
```

Read those three lines as three different problems:

- **7.92% assembly** is a problem the *port has already solved locally* —
  `wip/` has C for all of it. It matters upstream, where a body has to
  byte-match before it can replace the assembly, and it is somebody else's
  active work. It does not block anything on this side.
- **83.73% unnamed symbols** is the tax we actually pay, every day, on every
  question. It is not an architecture problem and no redesign fixes it.
- **68.59% unlabelled data** — 9.87 MB in 29,338 `.incbin` blocks — is the
  wall that *every* content ambition is behind. Level creator, new enemies,
  new powers, mod support: all of them terminate here, and none of them
  terminate on code structure.

---

## 2. The engine, bottom up

### Boot and the frame loop
| | |
|---|---|
| `asm/crt0.s` (88) | entry, interrupt vector |
| `src/main.c` (690) | `GameInit`, `GameLoop`, `gIntrTable`, `VBlankIntr`, `GetInput`, OAM upload |
| `src/init.c` (120) | one-time setup |
| **port** | `platform/main.c`, `platform/bios.c`, `platform/dma.c` |

`GameLoop` never returns and blocks in `VBlankIntrWait`. The port unwinds it
with Asyncify rather than restructuring it — see `docs/ARCHITECTURE.md`.

### The scheduler — this *is* the entity system
`src/task.c` (399 lines).

```c
struct Task *TaskCreate(TaskMain main, u16 structSize, u16 priority,
                        u16 flags, TaskDestructor dtor);
```

Every live thing in the game — every enemy, every menu, every cutscene, every
HUD element — is a Task holding a variable-size struct in IWRAM or EWRAM, run
once a frame in priority order by `TasksExec`. There is no separate entity
system, component registry or scene graph anywhere in this game. There is this.

It is worth being clear that this is *already* the modern pattern, badly
named. `TaskCreate(ObjectMain, sizeof(struct Object2), 0x1000, TASK_USE_EWRAM,
ObjectDestroy)` is a prefab spawn with an update function and a destructor.

The one genuinely awkward part is `TaskGetStructPtr`, which reconstructs a
pointer from a region base plus a stored offset. That macro is why the port
maps the GBA memory at its real addresses instead of shimming — see §5.

### Allocators
| | |
|---|---|
| `src/malloc_ewram.c` (105) | `EwramMalloc`/`EwramFree` — object structs |
| `src/malloc_vram.c` (52) | `VramMalloc(numTiles)` — the 512-tile object tile heap |

`VramMalloc` answers exhaustion by returning `ewram_end`, a real address, and
the game carries on writing. Every content feature that adds sprites collides
with this. It is the single hardest budget in the game and it is 52 lines long.

### The object model
`src/object.c` (2,332) and `include/object.h`.

Four-level struct inheritance by embedding:

```
struct ObjectBase   position (Q24.8), velocity, two separate hitboxes,
                    flags, roomId, damage/vulnerability words, struct Sprite
   └ struct Object2   + type, subtype, kirbyAbility, unk78/unk7C behaviour
                        function pointers, back-pointer to the ROM record
       └ struct Object4
           └ struct Kirby   + ability state, score, ~0x104 of player state
```

`ObjectBase`'s flag bits are the best-documented thing in the codebase — the
annotations in `include/data.h:278` came half from zalo's runtime-verified
field docs and half from reading the collision sweep. Most of the rest of the
game has nothing like it.

Dispatch:

```
gUnk_08351648[type]        219 descriptors, 0x18 each:
                             +0x06 kirbyAbility   +0x0C numTiles
                             +0x10 constructor
gSpawnFuncTable1[type]     219 constructors, indexed by the same byte
```

`InitObject` copies position/type/subtype out of the 36-byte ROM record and
installs `unk78` from the descriptor. `ObjectMain` runs `unk7C` each frame,
culls off-screen, handles death and scoring.

### The actors — and this is the good news
**93 of the decompilation's 153 C files are one species each.**

`waddle_dee.c`, `sparky.c`, `sir_kibble.c`, `kracko.c`, `mega_titan.c`,
`chest.c`, `mirror_door.c`, `star_platform.c`, `cannon_fuse.c`, … each with a
`CreateX(struct Object *record, u8 playerId)` constructor and a set of state
functions swapped into `unk7C`.

That is a per-actor module with a constructor and a state machine, one file
per actor, already. **The game is not a monolith at the actor level.** Adding
a new enemy is: write a file, add a descriptor entry, add a constructor to the
table. All three of those are things the port can already synthesise —
`tools/gen_rom_data.py` rebuilds those tables from scratch every build.

### Kirby — and this is the bad news
| | |
|---|---|
| `src/kirby.c` | **23,986 lines**, one file |
| `src/sub_0800F044.c` (509) | the buddy/helper AI |
| `src/code_0806F780.c` (15,612) | unidentified; pulls in kirby, treasures, bonus, trig — ability-adjacent objects |
| `src/code_0800ECAC_2.c` (5,435) | unidentified; kirby-adjacent |
| `src/ability_objects.c` (759) | ability pickups |

`kirby.c` holds the player state machine, every copy ability's behaviour, the
room-load path (`FillLevelInfo`), and the player-vs-world interaction — in one
translation unit of functions named `sub_0803FE74`. This is the monolith, it
is 13% of the game's C by volume, and it is where "improved Kirby AI", "new
powers" and "player limit unlock" all land.

### Rendering
| | |
|---|---|
| `src/sprite_1.c` (221), `src/sprite_2.c` (571) | the animation command VM — `AnimCmd_GetTiles`, `AnimCmd_SetIdAndVariant`, `AnimCmd_PlaySoundEffect`, `AnimCmd_JumpBack` … — and `DrawToOamBuffer` |
| `asm/sprite.s` (3,804) | **still assembly, claimed by another contributor — do not touch** |
| `src/bg.c` (546) | background setup and scroll |
| `src/palette.c` (193) | palette load, fades, grayscale, RGB remap |
| **port** | `platform/ppu.c` (958) — scanline renderer; `platform/view.c` (527) — the widescreen/zoom view rectangle |

Sprites are an interpreted bytecode. That is a real extension point: new
animations need no new code.

### Levels and rooms — **already a decoded format**
This is the finding that most changes the redesign conversation.

```c
struct RoomProps {              /* 40 bytes, 983 entries at 0x089331AC */
    u16 songIdx;                    u16 priorityFlags;
    u16 pixelDataIdx;               u16 paletteDataIdx;
    u16 mapDataIdx;                 u16 solidityMapIdx;
    u16 objectListIdx;              u16 objectList2Idx;
    u16 backgroundIdx;              u16 doorsIdx;
    s16 unkXmod_06 … unk10;         u16 unk02, unk04, unk0A, unk0C, unk22;
};

struct RoomTiledBG {            /* 0x20 bytes — fully named */
    u16 width, height;  const u32 *tileset;  u32 tilesetSize;
    const u16 *palette; u16 paletteOffset, paletteSize;
    const u16 *tilemap; …
};
```

Every field of a room except five `u16`s has a name. A room is: a song, a
tileset, a palette, a tilemap, a solidity map, two object lists, a background,
a door table. `gLevelObjLists` (0x08D637AC) is 287 object lists, 3,482 records,
36 bytes each. `gUnk_08D640A4` is the door tables. `gForegroundInfo` is the
foreground.

Runtime state is `struct LevelInfo` (0x668 bytes, one per player) — viewport,
level bounds, three `struct Background`, three `struct RoomTiledBG`, the object
list pointer, the current room id.

| | |
|---|---|
| `wip/code.c` `FillLevelInfo` | resolves a room id into a `LevelInfo` — **this is where the randomizer hooks**. Still `asm/code.s:738` upstream |
| `src/code_080023A4.c` (6,153) | `CreateLevelObjects`, `CreateObject`, tile collision, doors, foreground |
| `src/code_080332BC.c` | the 0x200-tile heap setup |
| `src/scroll_lock.c`, `src/special_doors.c`, `src/mirror_door.c` | room transitions |

**A level editor does not need a redesign. It needs the tileset/tilemap/
solidity blob formats decoded and a way to inject substitute tables at load —
and the second half of that already exists and is in production.**

### Collision
Tile/solidity in `code_080023A4.c`; object-vs-object in `object.c`
(`sub_08037314` sweeps per room, `sub_08038010` does the pair test). Two
distinct hitboxes per object in two different encodings — see the warning at
`include/data.h:323`, which is there because someone will otherwise try to
normalise them.

### Audio
| | |
|---|---|
| `src/m4a.c` (1,418), `src/m4a_tables.c` | the sequencer |
| `asm/m4a_asm.s` (1,630) | the mixer — **never a decompilation target** |
| **port** | `platform/m4a_mixer.c` (1,146) — reimplemented, not ported |

### Menus, maps, front end
`title_screen.c`, `subgame_menu.c` (1,168), `pause_area_map.c` (3,472),
`pause_world_map.c` (1,559), `pause_help.c`, `pause_fade.c`,
`collection_room.c` (1,968), `treasures.c`, `code_08138D64.c` (5,466 —
file select / save UI), `code_0814A828.c` (2,701 — hub/world map).

### Cutscenes
`src/code_08026044.c` (2,291) — `struct Cutscene`, a function pointer plus
eight sprites — and `asm/cutscene_trigger.s` (6,525, **still assembly**).

### Multiplayer
`multi_sio.c`, `multi_sio_08158934.c`, `asm/multi_sio_asm.s`,
`multi_boot.c`, `multi_boot_util.c`, `sio32_multi_load.c`,
`multi_08019F28.c`, `multi_08030C94.c` (1,448), `multi_08032E98.c`,
`asm/code_08032E98.s` (**20,335 lines — the largest remaining assembly blob**).
Port side: `platform/sio.c`, `platform/mp.c`, `platform/port/mp.h`.

### Save
`src/save.c` (260) — checksummed sections. `src/agb_sram.c` is
**unportable by construction** (it copies its own machine code to the stack)
and is replaced wholesale by `platform/sram.c`.

### Subgames
`src/subgames.c` (11,914) — the third-largest file in the game, and entirely
self-contained.

### The data
`data/*.s`, 28,592 labels over 29,338 `.incbin`s. Concentrated:
`data_8.s` (15,632 incbins), `data_23.s` (8,649), `data_1.s` (3,269).
Only ~160 labels are ever named by C; the rest are reached by following
pointers that already live inside the ROM image.

---

## 3. What the port layer already replaced

Worth listing, because it is the proof that subsystem-at-a-time replacement
works on this codebase and is not theoretical:

| subsystem | replaced by | status |
|---|---|---|
| the PPU | `platform/ppu.c` | scanline renderer, complete enough for the whole game |
| BIOS calls | `platform/bios.c` | 15 syscalls reimplemented |
| DMA | `platform/dma.c` | one macro redirected, catches all of them |
| save memory | `platform/sram.c` | had to be, the original is unportable |
| the audio mixer | `platform/m4a_mixer.c` | reimplemented from scratch |
| the view rectangle | `platform/view.c` | widescreen + dynamic zoom |
| the object type byte | `platform/rando.c` | one byte per record |
| ROM function tables | `tools/gen_rom_data.py` | 312/314 entries rebuilt from `katam.map` |
| linker-assigned globals | `tools/gen_ram_symbols.py` | 189 symbols |
| struct layout | `platform/port/gba_layout.h` | 246 types, asserted every build |

Each of those is a subsystem lifted out from under the game without the game
noticing. That is the strangler pattern, and it is already the house style.

---

## 4. The three things that are genuinely hard

Not "the architecture is confounding" — specifically these.

**1. Address identity.** The game's own allocator hands out offsets from a
region base. `TaskGetStructPtr` reconstructs pointers arithmetically. ROM
pointers point into ROM. That is why the port maps the GBA memory map at its
real addresses inside linear memory, and it is why the port compiles the
decomp's code *as written*. Any redesign that relocates data breaks this, and
the moment it breaks, `make test` stops being able to tell us whether we broke
the game.

**2. The oracle.** Right now every change can be checked against a real ROM:
byte-identical codegen upstream, and here a headless run that compares frames,
palettes, audio RMS and diagnostics. **That oracle is the most valuable asset
either repository has**, and it only exists while the code still resembles the
original. A from-scratch rewrite trades "does it match?" for "does it feel
right?" on 180,000 lines at once.

**3. 9.87 MB of unlabelled data.** No amount of code restructuring touches
this. It is decoded one format at a time, by measurement.

---

## 5. So what would a redesign actually be?

Three honest options, and they are not equally good.

### A. Rewrite the game against modern paradigms
Reimplement Kirby, the objects, the levels and the rendering as a clean modern
engine, using the decomp as reference.

This throws away the oracle on day one and takes on 180,000 lines of behaviour
that nobody has finished *naming*, let alone specified. 83.7% of symbols are
`sub_XXXXXXXX`; a rewrite has to know what each one does. It is not that this
is impossible — it is that it is a multi-year project whose first playable
build is worse than what we have today, and whose bugs are undiagnosable
because there is nothing to diff against.

**Recommendation: no, not as a whole-game move.**

### B. An extraction and injection layer — *what the randomizer already is*
Keep the decompilation as the engine of record. Build, on this side of the
`portify.py` seam, a layer that:

1. **exports** the game's data — rooms, object lists, tilesets, solidity maps,
   door tables, object descriptors — into an editable format;
2. **injects** substitute data at the points where the game resolves an index
   into a pointer, exactly as `PortRandoObjList` intercepts
   `levelInfo->objlistPtr`;
3. **registers** new content — a new object type is a descriptor entry plus a
   constructor, and both tables are already synthesised by
   `tools/gen_rom_data.py` rather than read from ROM.

This is where the leverage is, and it is almost embarrassingly cheap relative
to A. Every ambition on the list routes through it:

| goal | what it needs from this layer |
|---|---|
| Enemy/boss randomizer | ✅ shipped |
| Mod support | the export/inject layer *is* mod support |
| Level creator | + tilemap and solidity blob formats decoded |
| New enemies | + a slot in the descriptor table (already synthesised) |
| New powers | + the ability side of `kirby.c`, which is the hard part |

**Recommendation: yes, this is the redesign. Start here.**

### C. Strangle subsystems, one at a time, where they are already understood
Continue what the port has been doing: replace a subsystem behind an
interface, keeping the oracle green through every step. Renderer, audio, save
and view are done. The next candidates in rough order of ratio:

1. **A named entity layer over Tasks** — not replacing `task.c`, wrapping it,
   so new content is written against a modern API while the retail objects
   keep using the old one. Both are just `TaskCreate` underneath.
2. **The level/room loader** — already a decoded format; making it accept
   supplied data rather than only ROM indices is the B layer's foundation.
3. **`kirby.c`** — last, because it is 24,000 lines of unnamed functions and
   because nothing else is blocked on it.

**Recommendation: yes, opportunistically, where a subsystem is already
understood well enough that "understood" is not the work.**

---

## 6. The one thing to do first, if we only do one

**Decode the room data formats and build the export half of B.**

Rationale: it is measurable, it is verifiable against a real ROM without
playing the game (`tools/rando_preview.py` is the template), it unblocks the
level creator and mod support and new enemies simultaneously, and — unlike
every other item — nothing about it becomes cheaper if we do it after a
rewrite. It is the same work either way, so it is the work that should happen
first.

Second: **naming**. 83.7% undocumented is the tax on every conversation we are
going to have about any of this, it is upstreamable, and it is the one form of
progress that is never invalidated by a later architectural decision.
