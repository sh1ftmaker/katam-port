# Modding platform and level editor — design

Status: design, nothing implemented. Written 2026-11-08 against `main` @ b59e25e and
`fork` @ 8316766 ("the game source lives in game/ and is edited directly").

Citations are `path:line`. Paths under `game/` are on branch `fork`. Their line numbers
were checked against `fork` and match `build/port-src` at b59e25e, because the fork
commit is the last sync output committed as it was. `platform/` and `docs/` paths are on
`main` unless noted. **[unverified]** marks claims I inferred and did not measure.

`design/NETPLAY-V2.md` appeared while this was being written, and this document is aligned with
it. Its §11 ("Mods in sessions") and this document's §3.5, §4 and §9 describe the same
contract. Where the two first disagreed, this document adopted NETPLAY-V2's choice: the sim block
address, integer-only Lua, ordered `pairs`, the HELLO content-hash shape, and the 6 virtual input
bits. §9 lists what remains open.

---

## 0. Decisions in one table

| Question | Decision | Section |
|---|---|---|
| How game code finds data | **The base pointer of each table becomes a variable.** Call sites stay as they are: the ~30 `gRoomProps[...]`-style tables become retargetable pointers, filled by a resource manager that resolves vanilla → pack → mods. `Res_Get(kind, id)` is the API for new code and tools, built over the same tables. | §1 |
| Where resolved data lives | A **fixed-address resource arena** at `0x09100000` inside the 32 MiB ROM window that is already reserved. Every peer and every architecture gets the same addresses, so pointers held in sim memory hash the same. Mutable mod and extended-room state lives in **XRAM**, which is NETPLAY-V2's *port sim block* at `0x02040000`, snapshotted and hashed. | §1.4 |
| Runtime data format | **GBA-native first.** An in-game pack compiler turns human formats into exactly the byte layouts the game reads today. Presentation assets switch to modern formats (RGBA atlases, streamed audio) once the GPU renderer exists. Sim data stays native until code that reads it is rewritten. | §2.5 |
| Vanilla assets | Extracted on first run from the player's ROM (sha1 `274b102b…`) by a **built-in, versioned, deterministic extractor** driven by a committed manifest that holds addresses and formats but no bytes. Output is a folder of PNG + JSON/CSV + song event lists. The CI gate is that extract → compile is byte-identical to the ROM. | §2 |
| Mod format | A folder or a `.katmod` zip with a `mod.toml` manifest, an overlay tree that mirrors the pack, structured JSON patches addressed by stable record ids, and Lua in `scripts/sim` and `scripts/ui`. | §3 |
| Netplay identity | NETPLAY-V2's HELLO hash `H(simVersion, romHash, [(modId, modVersion, simContentHash)] in load order, levelSetHash, sessionMods)`, with each term defined concretely here. `levelSetHash` is the hash of the *resolved* sim-resource table. Presentation content is excluded by construction, because every resource kind has a fixed realm. | §3.5 |
| Script runtime | **Lua 5.4.7** (pinned, patched), with three realms: **build** (runs once and generates resources), **sim** (deterministic, integer-only, and *stateless between calls*: all persistent state is declared schema bytes inside the game's own snapshotted memory) and **presentation** (anything goes, never reached during resim). Runner-up: Luau. | §4 |
| Custom objects | One reserved type byte, `OBJ_MOD = 0xFF`. A mod record carries a `u16` mod-type index in the record's own variable-length tail. Descriptor lookups (194 sites) go through `OBJ_DESC(obj)`. Custom objects reuse `ObjectMain` physics and collision unchanged. | §4.6 |
| Editor | **Web-first**: a browser app around the existing wasm build, with the same C pack compiler compiled into it. Playtest reloads the room and drops Kirby in via `PortWarp`. Stopping restores a rollback snapshot. Tiled (TMJ/TMX) import/export ships first, as the cheap path. A Dear ImGui overlay in the native app comes later, and only as an inspector. | §5 |
| GBA limits | **Enforced by the validator until the code that imposes them is replaced**, then lifted. Sim limits (room size, kill bitsets, room ids, content index) are lifted via XRAM in M8. Graphics limits (tile count, palettes, OBJ tile heap) are lifted by the renderer split. | §5.7 |

---

## 1. Resource layer

### 1.1 What the code does today

Ground truth. This is the whole reason the design is table-level rather than call-level.

- The ROM is mapped at `0x08000000`. Named ROM tables are **address macros** in
  `game/generated/port/rom_data.h`, for example `#define gRoomProps ((const struct RoomProps *)0x089331AC)`
  (`rom_data.h:26`), and the same for `gForegroundInfo`, `gForegroundPalettes`, `gForegroundTilemaps`,
  `gRoomTiledBGs` and `gSolidityMaps` (`:18-28`), `gUnk_08351648` (`:304`), `gSpriteTables`'s target
  `gUnk_083B909C` (`:330`), `gMPlayTable` and `gSongTable` (`:388-389`). Only about 160 of the 28,591
  data labels are named by C. Everything else is reached by **following pointers that live inside
  the ROM** (docs/ARCHITECTURE.md, "Symbols that are not in any C file").
- One table is a RAM copy already: `gLevelObjLists` is `u8[1148]`, memcpy'd from `0x08D637AC` at
  boot (`game/generated/rom_copies.c`). That is the precedent for "table base in RAM".
- ROM-resident **function pointers** are patched at load. `sRomStructFns` holds 219
  `{ROM address → C function}` pairs, written into the mapped ROM image
  (`game/generated/rom_fn_tables.c:470`, `gUnk_08351648[i].unk10`). `gSpawnFuncTable1` is rebuilt as a
  real C array of 219 entries (`rom_fn_tables.c:139`). So the mapped ROM is already *mutable
  process memory*, not a file.
- On 64-bit builds every pointer stored in a GBA struct is a `PTR32` (4 bytes,
  `platform/port/p32.h`). **Anything the game can hold a pointer to must live below 4 GiB.**
  That rules out ordinary `malloc` for resources on native 64-bit.
- The room load is **one chokepoint**. `FillLevelInfo` (`game/src/level.c:157`) resolves every
  per-room table for the new room:
  `gForegroundTilemaps`/`gRoomTiledBGs`/`gLevelObjLists` (`:175-181`), `gSolidityMaps→unk4` door and
  patch records (`:229`), the object list through the randomizer seam `PortRandoObjList` (`:230`), the
  nav graph `gUnk_08D63C28` (`:233`), RL collision and nav decompression into the per-slot buffers
  (`:362-363`), LZ77 tilemap (`:364`), song (`:375`). `LoadLevelGfx` (`level.c:109`) LZ77s the fg
  tileset into VRAM `0x6008000` (`:116`) and picks the palettes (`:119`).
- Other per-room reads outside the loader (all `game/src/`): `code_080023A4.c:674` (fg tilemap
  `unk1C`), `:1213` and `:1993` (fg anim info), `:1406-1421` (object list accessors), `:1595-1651`
  (progress/door list walks, including the unterminated walk in `sub_08002C18` at `:1649`), `:1802`, `:1937`
  and `:2033` (hard-coded BG of rooms 34/918), `:1861` (fg palette anim); `code_0800ECAC.c:45,71` and
  `code_0800ECAC_2.c:456,3077`, `kirby.c:13715` (AI-Kirby nav); `warp_star.c:2224,2271`; and hard-coded
  rooms `0x321`, `0x323`, `0x38E` and `0x396` in `code_0814A828.c:364,406,2361`, `code_0801DA58.c:92,234`,
  `code_0801C6F8.c:174,287`, `code_0802B4A8.c:731`, `code_08026044.c:550,1393`, `dark_mind.c:9852` and
  `cutscene_trigger.c:1465`. That is 124 `gRoomProps[` uses in total (`grep -c`).
- Object spawning: `CreateObject` (`code_080023A4.c:6123`) and the two scroll-in walks `sub_0800A2B4`
  and `sub_0800A460` (`:5947`, `:6007`; dispatch at `:5991/5996`, `:6051/6056`) index
  `gSpawnFuncTable1[obj->type]`, or `gSpawnFuncTable2`, which is **four NULLs**
  (`code_080023A4.c:458`), keyed by the record's `spawnTable` byte. Constructors keep the template
  pointer for the object's lifetime (`struct Object.objTemplate`, `include/data.h:399`; see
  docs/RANDO.md §1).
- Object descriptors: `gUnk_08351648[type]` (24 bytes: score `unk2`, hp `unk4`, `kirbyAbility`,
  palette `unk8`, `numTiles`, start fn `unk10`, anim `unk14`; `include/data.h:547`) is indexed
  **194 times** across `game/src`, 47 of them in `object.c`.
- Sprites: one global bank, `gSpriteTables = &gUnk_083B909C` (`game/src/init.c:41`). There are 48
  `gSpriteTables->` reads in `sprite.c` (anim command streams, attrs, oam data, 4bpp/8bpp tiles,
  palettes; `struct SpriteTables` is at `include/sprite.h:295`). `Sprite.animId` is a `u16`
  (`sprite.h:131`).
- Sound: `m4aSongNumStart(n)` → `gSongTable[n]` → `gMPlayTable[song.ms]` (`game/src/m4a.c:97`). SFX go
  through the `PlaySfxInternal` macro, which **already gates on `gLocalPlayerId`**
  (`include/data.h:15-20`), so sound is per-client today.
- Compression: LZ77 (117 call sites) and RL (10) in `platform/bios.c:162,199`. **No Huffman**:
  `HuffUnComp` has zero uses in `game/src` or `platform/`.

### 1.2 Resource kinds and ids

At the C boundary a resource is addressed by `(ResKind kind, u32 id)` with *numeric* ids, so hot
paths never hash strings. String ids exist in packs, mods and tools, and are bound to numbers once
per session (§3.4).

```c
/* platform/port/res.h  (new) */
typedef enum ResKind {
    /* --- sim realm: part of the content hash --- */
    RES_ROOM_PROPS,      /* struct RoomProps, id = room id (vanilla 101..982, mods >= 1000)        */
    RES_ROOM_TILEDBG,    /* struct RoomTiledBG, id = fg/bg table index                             */
    RES_ROOM_COLLISION,  /* struct SolidityMap (+ RL blob + cell-record list), id = content index  */
    RES_ROOM_OBJLIST,    /* struct LevelInfo_1E4 (+ ObjectTemplate run), id = content index        */
    RES_ROOM_PROGRESS,   /* gUnk_08D640A4 entry (chests + door shadows), id = content index        */
    RES_ROOM_NAV,        /* gUnk_08D63C28 entry (AI-Kirby nav graph), id = content index           */
    RES_FG_ANIM,         /* struct ForegroundInfo (tile-animation descriptor), id = pixelDataIdx   */
    RES_FG_PALANIM,      /* struct ForegroundPalette, id = paletteDataIdx                          */
    RES_OBJ_DESC,        /* struct Unk_08351648, id = object type (0..0xDA vanilla, mod: §4.6)     */
    RES_METATILE_ATTR,   /* gCollisionAttributes[256] u32 terrain words                            */
    RES_ANIM_BANK,       /* struct SpriteTables anims/attrs/oamData (timing is sim-visible)        */
    RES_SONG_TABLE,      /* struct Song[] (which header, which player): ids are sim, bytes are not */
    RES_LUA_SIM,         /* compiled sim bytecode, per mod                                         */
    /* --- presentation realm: never hashed --- */
    RES_TILES,           /* 4bpp/8bpp tile pixel blobs (fg/bg tilesets, sprite tiles)              */
    RES_TILEMAP_GFX,     /* tilemap blobs (see note) */
    RES_PALETTE,
    RES_SONG_DATA,       /* MP2K headers + tracks, voicegroups, samples, waves                     */
    RES_UI_GFX,          /* HUD, pause map, title, language tilesets                               */
    RES_LUA_UI,
    RES_KIND_COUNT
} ResKind;

typedef struct ResView { const void *ptr; u32 size; u32 flags; u32 hash32; } ResView;

const void *Res_Get(ResKind kind, u32 id);    /* NULL if absent; ptr is stable for the session */
ResView     Res_View(ResKind kind, u32 id);
u32         Res_Count(ResKind kind);          /* one past the largest valid id */
void        Res_Invalidate(ResKind kind, u32 id);  /* editor hot reload only; never in netplay */
```

Two notes on the realm split. Both are binding, because the content hash depends on them.

1. **Tilemaps are sim today.** They are decompressed into `gUnk_02028EE0[slot]` (`level.c:364`),
   in EWRAM, and the tilemap is rewritten at run time when blocks break (SolidityMap cell records
   of type 1, the "2×2 tile + collision patch", docs/naming/room-tables.md summary). So until the
   renderer reads tile layers from resources directly, `RES_TILEMAP_GFX` is classed **sim**. It
   moves to presentation in M9 (§6).
2. **Tile pixels and palettes are presentation only once the hash stops covering VRAM and
   palette RAM.** Today they are DMA'd into VRAM, and the current rollback hash covers every
   region except IO (`platform/rollback.c:1222-1234`). NETPLAY-V2 classes PLTT, VRAM, OAM, the
   shadow palettes and the upload queues as **S\***: they stay in the snapshot but are masked out of
   the hash until the renderer split, after which they become P (NETPLAY-V2 §3.1, §3.3). So
   texture and palette mods are per-client safe **once V2's masked hashing ships** (their M1). Until
   then the loader treats them as sim-class and says so in the UI. One cosmetic wrinkle remains
   until the split: a joiner who receives a snapshot gets the host's VRAM contents. That is
   corrected on the next upload.

### 1.3 Overlay resolution

The stack, lowest priority first:

```
L0  rom        the mapped ROM image (only during migration stages R0/R1, and for unmigrated reads)
L1  vanilla    the extracted pack, compiled                                (§2)
L2  builtin    first-party mods shipped with the port (randomizer, QoL)   (§3)
L3..Ln mods    user mods, in resolved load order                           (§3.3)
Ln+1 editor    the editor's unsaved working set (local sessions only)      (§5)
```

The rules are deliberately few:

- **File replace**: the highest layer that has a resource's file wins, whole-file (the libultraship
  model: `ArchiveManager` maps a CRC64 of each path to the archive that owns it, and later archives
  override, `soh-web/libultraship/src/ship/resource/archive/ArchiveManager.cpp:51-76`).
- **Structured patch**: a mod's `patches/*.json` edit fields of a resolved record. They are applied
  in load order on top of the winning file (§3.2). This is what lets two mods change different
  enemies in the same room.
- **Build-phase output** (§4.2) is a layer that sits directly above its own mod.
- Resolution happens **once per session build**, never per frame. Its output is the
  *resolved resource table*: kind × id → arena offset and hash.

### 1.4 Where resolved data lives: the arena and XRAM

Requirements: below 4 GiB (PTR32); **the same address on every peer and every architecture**, because sim memory stores pointers to resources (`objTemplate`, `objlistPtr`, `unk1E0`) and the state hash covers that memory; and no new reservation on the wasm side.

The ROM window is already reserved as the full 32 MiB bus (`platform/native/mem.c:63-75`). SRAM was
moved to `0x09000000` (`mem.c:71`). The 16 MiB image stops at `0x09000000`. So:

```
0x02040000  XRAM = NETPLAY-V2 "port sim block"  mutable; snapshotted; hashed (V2 §3.1, their M1)
            ├─ port state V2 relocates there (R7: save info, rando buffers, warp queue, …)
            ├─ mod sim store, 64 KiB default (V2 §11)               §4.4
            ├─ attachments on vanilla objects                       §4.4
            └─ M8 room growth: per-slot collision/tilemap/nav buffers, extended kill bitsets
0x08000000  ROM image (16 MiB)                               read-only by convention
0x09000000  SRAM (64 KiB)                                    existing on main (mem.c:71)
0x09100000  RES arena    (14.9 MiB)  immutable after session build; not snapshotted; hashed via its table
0x0A000000  wasm GLOBAL_BASE (compiler-owned)                existing (mem.c:9)
```

- The arena is laid out by a **deterministic bump allocator in resolved-table order**: kinds in
  enum order, ids ascending, 16-byte alignment. Identical resolved tables therefore produce identical
  addresses everywhere. That is also what makes V2's portable snapshots (their M2) carry arena
  pointers unchanged.
- **XRAM** is *not* a second block. It is the sim block NETPLAY-V2 already reserves at `0x02040000`
  on every platform. On hardware that range is EWRAM's mirror, and in the port it is empty
  address space. Modding takes sub-regions in it. Its size is V2's to fix; this design needs
  64 KiB for the mod store from M5 and up to ~1 MiB for M8 room growth (4 slots × the largest room
  allowed). The snapshot covers the block's high-water mark.
- Open point: NETPLAY-V2's table lists SRAM at `0x0E000000`, while `main` relocates it to
  `0x09000000` (`mem.c:71`). The arena base must sit above wherever SRAM ends up. `0x09100000`
  assumes the `main` layout.
- If 13 MiB turns out to be too little (it will not for sim data, which is under 2 MiB in vanilla
  **[unverified estimate]**), raise wasm `GLOBAL_BASE` to `0x10000000` and give the arena
  `0x0A000000–0x0FFFFFFF`. On native, that is one more `kMap[]` entry.
- **Risk to check in M0**: the game "reads past the end of its own data in a few places" (`mem.c:64-66`).
  If any of those reads lands at or above `0x09100000`, it now reads arena bytes where it used to
  read zeros. M0 adds a startup assertion that logs the first read of that kind (natively: guard pages
  during the R0 soak).

### 1.5 Migration path (keeps the harness green at every step)

"Harness" here means what the fork commit used as its gate: md5-identical lp64 screenshots at
frames 300/900/1400 and node frame 1400, plus `make test`, `netplay-rb-test`, `seat-test` and the
cross-build DMA-stream comparison (docs/SIXTYFOUR.md). One caveat applies to everything below: the
DMA log records *source addresses*. From R2 on, the comparison has to be over (dest, length,
payload hash), not over src. Making that change is the first task of R2.

| Stage | What changes | Proof it is behaviour-neutral |
|---|---|---|
| **R0: retargetable bases** | Each ROM table macro the resource layer owns becomes a pointer variable initialised to its ROM address, e.g. `#define gRoomProps (gResBase.roomProps)` with `gResBase.roomProps = (const struct RoomProps *)0x089331AC`. `Res_Get` is implemented over `gResBase`. **No call site changes.** Rando's `PortRandoObjList` becomes the first L2 "builtin mod" hook behind `Res`. | Bit-identical everything: the pointers hold the same values. |
| **R1: identity round trip** | Extractor (§2) runs; the pack compiler compiles vanilla pack → native blobs. Debug mode `--res-identity` writes the compiled blobs *back over the ROM image at their original addresses* and runs the harness. | For every resource, compile(extract(rom)) equals the ROM bytes (or the same decompressed payload for LZ77/RL; §2.6). The harness is identical. This is the extractor's real test. |
| **R2: arena-backed** | `gResBase.*` point into the arena. Pointers *inside* blobs (`RoomTiledBG.tileset`, `ForegroundInfo.tileset`, object-list `obj`, SolidityMap `unk0/unk4`, `SpriteTables.*`) are relocated by the compiler. The ROM is touched only by the extractor and by unmigrated tables. | Screenshots identical. DMA stream identical by content. Cross-arch state hash identical (because arena addresses are). |
| **R3: per-entry access where needed** | Code that must change behaviour (variable room sizes, mod rooms, `OBJ_MOD`) calls `Res_Get` or the accessors directly. | New tests per feature; the vanilla harness stays identical. |
| **R4: presentation off VRAM** | With the renderer split, presentation kinds are consumed by the renderer by id and stop going through VRAM. | Renderer project's own gates. |

**Tables to migrate first, ordered by value per effort**:

1. **Room bundle** (`gRoomProps`, `gForegroundTilemaps`, `gRoomTiledBGs`, `gLevelObjLists`,
   `gSolidityMaps`, `gUnk_08D640A4`, `gUnk_08D63C28`, `gForegroundInfo`, `gForegroundPalettes`). One
   loader (`level.c:157-421`) plus the ~40 sites listed in §1.1. This unlocks custom rooms, the
   editor and the door graph.
2. **Object descriptors** `gUnk_08351648` (194 sites, macro swap only in R0; `OBJ_DESC()` in M6) and
   `gSpawnFuncTable1` (3 dispatch sites). These unlock tuning mods (hp, score, ability) and custom
   types.
3. **Metatile attributes** `gCollisionAttributes` / `gUnk_082D88B8` (defined in C,
   `src/code_080023A4.c:71` per docs/ROOM-FORMAT.md §3, 256 entries, 131 unused). This unlocks
   new terrain kinds.
4. **Sprite bank** `gSpriteTables` (one assignment, `init.c:41`). This unlocks sprite and animation
   mods. Swapping the one pointer swaps the whole bank.
5. **Song table** `gSongTable`/`gMPlayTable` (`m4a.c:97-128` plus the `PlaySfx*` macros). This
   unlocks music and SFX mods.
6. Later: area-map tables (`gAreaMap*`, `rom_data.h:8-16`), help menu, title, the language
   tilesets (`gCrackityHackObjTileset_English` and friends).

---

## 2. The vanilla pack

### 2.1 The extractor

- **It ships in the game, as C** (`platform/res/extract_*.c`), compiled into every build, wasm
  included. It is driven by a **committed manifest** `assets/manifest.json` that holds labels,
  addresses, sizes, formats, struct walkers and names. The manifest contains no bytes. It is
  generated once by a dev-only tool (`tools/gen_asset_manifest.py`) from `decomp/katam.map`,
  `decomp/data/*.s` labels, `decomp/sound/*.inc` (song and voicegroup names and addresses, e.g.
  `voice_groups.inc`, `song_table.inc` @ `0x08B59ED0`), `decomp/graphics_file_rules.mk` (tile counts)
  and the port's structural knowledge (room_export.py's walkers). This is the same treatment
  `gen_rom_data.py` already gives labels.
- **Input**: the player's ROM. It must match a known sha1 (`274b102b…`, `decomp/katam.sha1`). Other
  regions are refused with a clear message until a manifest exists for them.
- **Output**: `<userdata>/vanilla/` natively, OPFS `vanilla/` in the browser (IndexedDB fallback).
  The output is never redistributed, and the UI says so.
- **Versioned**: `pack.toml` records `extractor = "1.3.0"`, `rom_sha1` and `manifest_sha256`. A
  version change re-extracts. Old packs are kept until the new one has validated.
- **Deterministic**: sorted iteration, canonical JSON (UTF-8, LF, two-space indent, keys in schema
  order, integers only, no floats), PNG through a pinned deflate (vendored miniz, fixed level, no
  ancillary chunks; the existing stored-deflate writer `platform/native/png.c` is a correct fallback
  but 40% larger), and WAV with fixed chunk order. `pack.lock` lists the sha256 of every file. CI
  extracts on wasm, x86-64 and arm64 and diffs `pack.lock`.
- **Cost**: one pass over 16 MiB. A few seconds natively; it runs in a worker with a progress bar
  in the browser **[unverified timing]**.

### 2.2 Folder structure

```
vanilla/
  pack.toml                      extractor/manifest versions, rom sha1
  pack.lock                      sha256 per file
  world/
    rooms.json                   room id -> folder, content index, area; the 286 real rooms
    areas.json                   area ids, names, entry rooms, map art refs, shard rooms
    graph.json                   DERIVED door graph (559 tile doors + object-borne edges); not canonical
  rooms/0803/
    room.json                    RoomProps decoded: size, song, flags (named bits), camera/parallax
                                 (unk0A/0C/0E/10, unkXmod_06/08 as 8.8), layer refs by id, raw tail
    fg.csv                       tilemap: raw GBA text-BG u16 per 8x8 cell (tile|hflip<<10|vflip<<11|pal<<12)
    bg.csv                       only if the room owns its bg (backgrounds are shared, see bgs/)
    collision.csv                one metatile-attribute index per 16x16 cell
    objects.json                 ObjectTemplate records with stable uid "0803#5", named fields +
                                 raw bytes for unknown ones, extended tail preserved
    doors.json                   ONE entry per logical door: dest room, dest cell, source cells[];
                                 compiled into BOTH SolidityMap tag-3 cells and progress tag-2 entries
    patches.json                 SolidityMap type-1 cell records (2x2 tile + collision patches)
    chests.json                  progress-list tag-1 entries
    nav.json                     AI-Kirby nav nodes, routing matrix, scripts (opaque bytes until decoded)
  bgs/<idx>/bg.json + bg.csv     RoomTiledBG for gRoomTiledBGs entries (shared backgrounds)
  gfx/
    tilesets/fg/<pixelDataIdx>.png   indexed PNG, 16-entry grayscale palette = 4bpp index; 32 tiles wide
    tilesets/fg/<idx>.anim.json      ForegroundInfo tile-animation frames
    tilesets/bg/<idx>.png
    palettes/fg/<paletteDataIdx>.json  16 banks x 16 colors as "#RRGGBB" (BGR555 exact), + palette anim
    sprites/tiles4bpp.png        the whole OBJ tile pool, 32 tiles wide (stable tile indices)
    sprites/tiles8bpp.png
    sprites/palettes.json
    sprites/anims/<animId>.json  per anim: variants -> command lists (symbolic: show_frame, get_tiles,
                                 get_palette, jump_back, play_sfx, translate, set_id_variant, ...)
    sprites/frames/<animId>/<v>_<n>.png  DERIVED previews (composited); ignored on import
    ui/...                       HUD, pause map, title, per-language tilesets
  objects/
    types.json                   per type 0..0xDA: name (constants/object_types.h), category,
                                 descriptor fields, ctor symbol, schema of template fields
  collision/attributes.json      256 entries, decoded bitfields (docs/naming/metatile-attributes.md)
  audio/
    song_table.json              id -> song file, music player, priority
    songs/<id>_<name>.song.json  MP2K header + per-track event list (byte-exact disassembly)
    songs/<id>_<name>.mid        DERIVED convenience export (lossy); not canonical
    voicegroups/<n>.json         voices: directsound/square1/square2/wave/noise/keysplit/drum + params
    samples/<addr>.wav           8-bit PCM at the header's rate; loop start in a smpl chunk; base pitch
    waves/<addr>.json            programmable-wave 16 bytes
  text/                          KATAM's "text" is mostly language-specific tile graphics (ui/);
                                 any real string tables found go here as JSON [unverified scope]
```

Why these formats:

- **PNG + JSON palette**, the decomp/gbagfx convention (indexed PNG plus separate palette), so
  4bpp round trips are exact. Colours are written as `#RRGGBB` derived from BGR555 by
  `c5 << 3 | c5 >> 2`. The importer maps back with `>> 3`, which is exact for every value the
  forward map produces. Artists edit in any indexed-PNG tool. The editor renders previews in colour.
- **CSV for grids**, because they are only edited through the editor or Tiled. Raw u16 values
  keep the round trip exact.
- **JSON for records**, with named fields where the meaning is known and `raw` bytes where it is
  not, so unknown fields survive edits.
- **Audio as an event list**, not MIDI. mid2agb (`decomp/tools/mid2agb`, driven by
  `decomp/audio_rules.mk` with `midi.cfg`) is a one-way, lossy compiler: loop points, MEMACC,
  patterns and per-track voice changes do not survive MIDI. The canonical song is the disassembled
  MP2K command stream. MIDI is emitted as a convenience export and accepted as an *import* for new
  songs (through an in-house mid2agb equivalent).

### 2.3 What is not decoded yet, and how the extractor copes

docs/ROOM-FORMAT.md §6 says plainly that **the tilemap and tileset blobs decompress, but their
interpretation is not established**. Strong circumstantial evidence says the fg tilemap is a
standard text-BG u16 map at 8 px granularity: it is decompressed into
`u16 text[1950*4]` (`include/data.h:659-662`), four entries per 16 px collision cell, and the
software PPU renders it. That is still **[unverified]**. M1's first task is to verify it by
comparing VRAM screenblocks during play against windows of the decompressed buffer, the same way
ROOM-FORMAT §5 verified collision.

Until each field is decoded, the extractor emits **raw-preserving** forms (`"raw": "hex…"`, or the
`.bin` beside the JSON). Round trip stays exact. Only editing is limited.

### 2.4 Round-trip fidelity requirements (CI gates)

| Gate | Requirement |
|---|---|
| **F0 identity** | For every resource in the manifest, `compile(extract(rom))` is byte-identical to the ROM range. For LZ77/RL blobs the requirement is the identical *decompressed* payload (§2.6). This is R1. |
| **F1 cross-platform** | `pack.lock` is identical across wasm, x86-64 and arm64. |
| **F2 canonical save** | Editor load → save with no edits gives identical files. Tiled export → import with no edits gives an identical room folder. |
| **F3 live** | For the room_export `--check` style live comparison (ROOM-FORMAT §5), collision, tilemap and object list in RAM after a room load are identical between the ROM build and the pack build, for all 286 rooms, using `PortWarp` to reach each room. |

### 2.5 In-memory representation: GBA-native first

The game reads `struct RoomTiledBG`, LZ77 streams, `ObjectTemplate` runs and so on. Rewriting every
reader is the renderer project's job and the long tail of the fork. Deciding here to keep native
formats at runtime means:

- The **pack compiler** (`platform/res/compile_*.c`, C, shared with the editor through wasm) turns
  pack files into native blobs in the arena. Compiled output is cached under
  `cache/compiled/<sha256 of inputs + compiler version>`.
- Native-format limits (10-bit tile index, 16 palette banks, `u8` type) apply to *compiled* data.
  The *pack* formats are wider (PNG of any size, string ids), so the formats do not change when
  the limits are lifted. Only the compiler and validator do.
- **Presentation kinds go modern once the renderer reads them**: RGBA atlases, any size or count of
  tiles and colours, optional `@2x`/HD variants keyed by the same resource id (the
  SoH `alt/` idea, `soh-web/libultraship/src/ship/resource/ResourceManager.cpp:111-124`, minus the
  runtime toggle for sim kinds).

### 2.6 Compression

The compiler emits **valid LZ77/RL streams from a deterministic in-house encoder**, because the game
calls `LZ77UnCompVram(ptr)` and `RLUnCompWram(ptr)` on these pointers (`level.c:116,362-364`).
Nintendo's encoder is not reproducible, so F0 compares decompressed payloads for these blobs, and
the encoder's own determinism is covered by F1. The arena is not short of space, so the encoder
favours speed (a greedy 4 KiB window) over ratio.

---

## 3. Mod packages

### 3.1 Manifest

`mod.toml` at the package root:

```toml
[mod]
id          = "kirbyfan.sky_tower"     # [a-z0-9_.-], globally unique, reverse-DNS encouraged
name        = "Sky Tower"
version     = "1.2.0"                  # semver
api         = "^1.0"                   # modding API (sim ABI) range this mod was built against
authors     = ["kirbyfan"]
license     = "CC-BY-4.0"
description = "Six new rooms above Rainbow Route."

[dependencies]                         # hard: must be present, version-matched, loaded before me
"core.objects_ext" = "^1.0"

[order]                                # soft ordering only; never creates a dependency
after  = ["someone.music_pack"]
before = []

[conflicts]
"other.sky_mod" = "*"                  # refuse to load together

[[option]]                             # user-facing settings; sim options join the content hash
key = "difficulty"  type = "enum"  values = ["normal", "hard"]  default = "normal"  realm = "sim"

[scripts]
build = ["scripts/build/main.lua"]
sim   = ["scripts/sim/main.lua"]
ui    = ["scripts/ui/main.lua"]
```

`affects_simulation` is **derived, not declared**. The loader computes it from the realms of every
resource the mod contributes (§1.2) and from whether it has `build`/`sim` scripts. The UI shows a
"changes gameplay" badge. A mod cannot lie about this, because realm membership is fixed per kind.

### 3.2 Contents and overlay rules

```
mod.toml
assets/…        same tree as vanilla/ (rooms/, gfx/, audio/, objects/, …); whole-file replace or add
patches/*.json  structured edits:
                  { "target": "rooms/0803/objects.json",
                    "ops": [ { "op": "set",    "uid": "0803#5", "path": "type",  "value": "waddle_doo" },
                             { "op": "remove", "uid": "0803#7" },
                             { "op": "add",    "uid": "kirbyfan.sky_tower#1", "value": { … } } ] }
                  For plain records an RFC 7396 merge patch: { "target": "objects/types.json",
                    "merge": { "0x12": { "hp": 4 } } }
rooms/<modroom>/… new rooms, same folder shape as vanilla rooms; ids are strings ("sky_1")
sprites/<name>/   new sprites: frame PNGs (any size, RGBA allowed) + anim.json
scripts/build|sim|ui/*.lua
```

- Paths resolve **case-sensitively** and must stay inside the package: no `..`, no absolute paths,
  no symlinks.
- Record identity: the extractor gives every vanilla list record a stable `uid` (`<room>#<index in
  vanilla list>`). Mods give their own records `<modid>#<n>`. Patches address records by uid,
  never by position.
- Order of application for a resource: the winning whole file (highest layer), then every layer's
  patches **at or above the winner's layer**, in load order. Patches below the winner are dropped,
  with a warning: they were written against a file that has since been replaced.

### 3.3 Load order and conflicts

- Load order is a topological sort over `dependencies` plus `order.after/before`. Ties break by
  `id` in byte order. The result is deterministic and independent of folder enumeration order.
  A cycle is an error that names the cycle.
- Conflict report, computed at session build and shown before play:
  - **replace/replace**: two layers ship the same file. The later one wins (info).
  - **patch/patch same field**: the later one wins (warning, both mods named).
  - **patch on a uid the winner removed**: dropped (warning).
  - **declared `conflicts`**: refuse.
  - **id collisions** for new content (two mods both define `rooms/sky_1`): impossible, because ids
    are namespaced by mod id at binding time (`kirbyfan.sky_tower:sky_1`).

### 3.4 Binding string ids to numbers (per session)

Done after resolution, deterministically:

- **Rooms**: vanilla ids keep their numbers (101..982). Mod rooms get 1000+ in (load order, id)
  order. `RoomProps` is rebuilt in the arena with `Res_Count(RES_ROOM_PROPS)` entries. Content
  indices: vanilla 0..286 kept (index 4 is the unused list, docs/naming/doors.md), mod rooms from
  287 up.
- **Object types**: vanilla 0..0xDA. Mod types get a `u16` mod-type index from 0 in the same order
  (§4.6).
- **Animations** (`u16 animId`): appended after the vanilla bank's count. **Songs** (`u16`):
  appended after the vanilla table.
- These numbers only ever appear *inside* a session. Saves and level data store string ids, so
  adding a mod never renumbers anything on disk.

### 3.5 Distribution and the netplay content hash

- `.katmod` is a zip (store or deflate) with `mod.toml` at the root. A folder with the same layout
  loads the same way; that is how the editor writes work in progress (SoH has the same duality:
  `FolderArchive` vs `O2rArchive`, `libultraship/src/ship/resource/archive/`). The packer writes
  `mod.lock` (sha256 per file). The loader verifies it.
- **No Nintendo bytes**: the packer refuses any blob of 64 bytes or more whose hash matches a blob
  in the user's vanilla pack (it has a hash index of the pack), and the in-game publisher repeats
  the check. Mods reference vanilla content by id (`vanilla:gfx/sprites/anims/767`), never by copy.
- **Content hash.** The shape is NETPLAY-V2's HELLO hash (V2 §11, §8):
  `H(simVersion, romHash, [(modId, modVersion, simContentHash) in load order], levelSetHash, sessionMods)`.
  This document fixes each term (SHA-256, so the JS side can use WebCrypto too):

```
simContentHash(mod) = SHA256( "katam-mod-sim-v1"
                            ‖ sorted (path, sha256) of the mod's SIM-realm files      (realm fixed by kind, §1.2)
                            ‖ SHA256(build + sim Lua bytecode, in file order) )
levelSetHash        = SHA256( "katam-resolved-v1"
                            ‖ sorted (kind, id, sha256(compiled bytes)) over every SIM-realm resource
                              after resolution, patches and build-phase output )      (§1.3)
sessionMods         = canonical(sim options) ‖ session seed if any build script consumed it
presentationHash    = the same construction over presentation files; logged, never enforced (V2 §11)
```

  `levelSetHash` is what really guards against desync, because it is the content the sim will
  read. The per-mod list makes the failure message actionable ("you have sky_tower 1.1, the host
  has 1.2"). A purely cosmetic mod changes neither term. The *mod list* (id, version, zip sha256,
  realm) travels with HELLO so a joiner can fetch what is missing.
- File realm is **fixed by resource kind and path**, not declared per file. V2 §11 asks that "each
  mod's manifest declares which class every file belongs to, and the loader enforces it". Fixing
  the realm by kind satisfies that with no way to mis-declare. The manifest may only *list* the
  derived classes for display.
- Presentation-only mods: listed for information, never required, never hashed.
- Fetching missing mods: content-addressed blobs via the DO manifest of missing hashes. The blobs
  come from R2, uploaded by the room creator and capped at 20 MB per room, or from a peer (V2 §11).
  The sha256 is verified before use. Vanilla assets are never transferred. **No silent
  auto-install of sim scripts**: the joiner confirms or is refused (V2 §11).

---

## 4. Lua

### 4.1 Runtime choice: Lua 5.4.7, pinned and patched

| Criterion | Lua 5.4 | Luau | Notes |
|---|---|---|---|
| Integer math | **native 64-bit ints, `//`, bitwise ops, defined wraparound** | doubles only (+ `bit32`) | The whole simulation is integer fixed point (positions are 24.8 in `s32`, `InitObject` `object.c:2043-2044`). Ints let the sim VM ban floats outright (patch 2). Deciding factor. |
| Cross-arch determinism | Integer-only sim VM is exact by construction. | Every number is a double, so float rules (contraction, libm) apply to *all* arithmetic | |
| Sandboxing | Manual but small: custom `_ENV`; no `io/os/debug/load/dofile/require` at run time, no `string.dump`, no `collectgarbage` beyond `"count"`. | Built in (`safeenv`, `table.freeze`, no bytecode loading) | Luau is better here, but the 5.4 surface is small enough to audit. |
| Budget/preemption | `lua_sethook(LUA_MASKCOUNT)`, **deterministic instruction counts** | interrupt callback | Equal. |
| wasm | Plain C89-ish; drops into the existing emscripten link | C++17, larger | 5.4 is ~250 KB of wasm **[unverified]** |
| Snapshot/serialize | No persistence library for 5.4 (Eris targets 5.2/5.3). **Not needed** with the stateless-sim model (§4.4). | None | |
| Modder familiarity | sm64coopdx mods are Lua 5.3 (`sm64coopdx/lib/lua/include/lua.h:21`, `LUA_VERSION_NUM 503`). 5.3 → 5.4 is near drop-in. | Roblox users | |
| Tooling | LuaLS (sumneko) with `---@class` annotations; we ship a generated `katam.d.lua` | Luau LSP, strict types | |

Patches to stock 5.4.7 (each is a few lines and is listed in `third_party/lua/PATCHES`):

1. `luai_makeseed` → a constant, so string hashes and table layouts are identical across runs and
   peers. `luaS_hash` works over `unsigned int` (32-bit on every target), so it is the same everywhere.
2. **Integer-only sim VM** (NETPLAY-V2 §11). In the sim and build VMs, float literals are a
   compile error. `/`, `^` and any operation that would produce a float raise a runtime error, and
   `math`'s float functions are absent; integer `//`, `%` and bitwise ops remain. This removes float
   determinism from the problem rather than managing it. Build with `-ffp-contract=off
   -fno-fast-math` anyway, using the single clang toolchain and flags V2 mandates for sim code
   (V2 §6 R2). The ui VM is stock (floats allowed).
3. The sim VM's `setmetatable` rejects `__gc`, `__mode` and `__close`. Finalizer and weak-table
   timing depends on GC pacing, which depends on allocation sizes, which differ between 32-bit and
   64-bit builds.
4. **`pairs`/`next` iterate in key order** (V2 §11): integers ascending, then strings bytewise,
   then `false`, `true`. Keys of any other type raise an error. This costs O(n log n) per
   traversal, which is fine for the small tables sim code iterates. It makes order independent of
   insertion history, table size and addresses.
5. Custom allocator per VM with a hard cap. Hitting the sim cap is a session-fatal error, reported
   ("out of script memory"), because OOM points differ between 32-bit and 64-bit and so cannot be
   handled deterministically. The cap is generous (32 MiB) so it never matters in practice.

Why not the alternatives: **Luau** is the runner-up; choose it if strict typing or the frozen-table
sandbox matters more than integers. **WASM-hosted scripting** (mods as wasm modules) has excellent
determinism, but asks players to bring a toolchain, which is the wrong audience. **JS/QuickJS**: no
integer type, heavier, and no modder base.

### 4.2 Three realms

| Realm | When it runs | Can | Cannot |
|---|---|---|---|
| **build** | Once, at session build, before frame 0. Deterministic inputs: resolved resources, sim options, session seed. | Read and write *resources* (generate rooms, shuffle object lists, tweak descriptors). Its output becomes a resource layer and is hashed. | Touch game memory. Persist anything else. |
| **sim** | Inside the simulated frame, including every rollback re-simulation. | Read/write game state through the API, register object types, declare state schemas, use `sim.rand`, `fx` math | Floats (integer-only VM, §4.1), `math.random`, wall clock, I/O, persistent Lua state (§4.4), anything in the presentation API |
| **ui** (presentation) | Once per *presented* frame, after the sim; never during resim. Own VM. | Draw HUD/overlays, decorate the render list, play local sounds, menus, mod settings, read the latest sim state read-only, local save files, floats, timers, `pairs` on anything | Write sim state. Its only channel into the sim is **input** (§4.5). |

The randomizer (`platform/rando.c`) is the model build-phase mod. Its mapping is already "a pure
function of (seed, room, index-in-list)" (`rando.c` header, docs/RANDO.md §4). Rewritten as
`builtin/rando/scripts/build/main.lua` it becomes a data transform at session build instead of a
C hook at room load. The C version stays as the reference until the Lua version matches it.

### 4.3 Numbers, fixed point and RNG

- All sim API values are **integers**. Positions and speeds use the game's own units: `x`/`y` are
  `s32` with 8 fractional bits (`template.x * 0x100`, `object.c:2043`), and `xspeed`/`yspeed` are
  `s16` sub-pixels per frame. The API never converts to floats. Passing a float where an integer is
  expected raises an error at the boundary (`lua_tointegerx` strict).
- The `fx` module provides `fx.mul(a, b, shift)`, `fx.div`, `fx.sin(angle)` and `fx.cos` from the
  game's own `gSineTable[0x500]` (`game/src/trig.c:8`), `fx.atan2` from the BIOS `ArcTan2`
  (`platform/bios.c:71`), and `fx.sqrt` (`bios.c:53`). These are the same functions the game uses,
  so they give identical results everywhere.
- `sim.rand(n)` is a **separate LCG stream in XRAM**, seeded from the session seed. It is *not*
  `gRngVal` (`include/random.h:13`), so a mod that rolls dice does not shift vanilla enemy
  behaviour. `sim.vanilla_rand()` exists for mods that deliberately want the game's stream.

### 4.4 The state model: stateless sim VM, state in snapshotted memory

The rollback snapshot is "six memcpys" of the GBA map plus the port's out-of-map state
(`platform/port/rollback.h`, 387 KiB, ~5 µs). A Lua heap cannot join that cheaply. It is megabytes,
full of host pointers, and laid out differently on 32-bit and 64-bit. So **the sim VM holds no
state between calls**:

- **Per-object state** is a declared schema. It is allocated *inside the object's own task struct*
  (`TaskCreate(ObjectMain, sizeof(struct ModObject) + schema_bytes, …)`, `task.c:70`), so it lives
  in EWRAM/IWRAM and is snapshotted and hashed with no extra work. Lua sees it as a userdata proxy
  (`o.state.timer`).
- **Attachments on vanilla objects** (a mod adds a field to Waddle Dee): an XRAM table keyed by
  task slot index, allocated on spawn and freed in the `ObjectDestroy` hook.
- **Mod-global state**: `sim.store` is a declared schema block in the mod sim store (XRAM, 64 KiB
  in total by default, NETPLAY-V2 §11), sub-allocated per mod in load order.
- **Field types**: `i8/u8/i16/u16/i32/u32/bool`, `enum{…}`, `str<N>` (fixed-length, NUL-padded short
  strings, as V2 §11 allows), `objref` (stored as task index plus a generation counter, never as a
  pointer), `roomref`/`typeref` (session-bound numeric ids, §3.4), and fixed-length arrays of these.
  No tables, no functions.
- **Function pointers the sim stores** for mod objects (`obj->unk78 = ModObject_Tick`, §4.6) are
  port functions. Under V2's canonical code IDs (V2 §6 R5) they get IDs from a reserved
  port range (proposed `0x0F00_0000 + n`, outside any GBA ROM address) in the same generated table.
- **Load-time verifier**: after a sim module's main chunk has run (registrations happen there), the
  loader walks every function prototype reachable from registered callbacks and rejects
  `OP_SETUPVAL` on any upvalue that resolves to a main-chunk local. The check follows upvalue
  descriptors (`instack`/`idx`) up to level 0, which is about 200 lines over 5.4's `Proto`. Then the
  module environment and every table reachable from it are frozen (read-only proxies; no
  `rawset` in sim). A closure's own locals, created and dropped within one call, are fine.
- This keeps snapshot cost at zero for the VM (garbage between frames is unobservable), makes the
  hash cover mod state automatically, and gives identical memory images across architectures.
  Authoring consequence: no coroutines across frames in sim. State machines are expressed as states
  with timers (§4.6), which is how every KATAM object is already written (`ObjectSetFunc(obj, anim,
  fn)`, `include/object.h:390`; e.g. `minny.c:69,139`).

**Errors in sim**: a Lua error or an instruction-budget overrun happens on every peer at the same
frame, because inputs and state are identical. The policy is deterministic: that mod's sim hooks are
disabled for the rest of the session on every peer, the error is shown, and the session continues.

### 4.5 Input

- **Presentation-side remapping** (key rebinding, macros, touch layouts) happens *before* input is
  sampled for sync. It is purely local and needs no hashing, because input is the synced quantity.
- **Sim-side transform** `sim.on("input", fn(player, keys) -> keys)` runs after the synced input for
  the frame is applied. Every peer runs it on the same input. Inserted where the game reads keys,
  `GetInput()` at `game/src/main.c:231`, on the port's applied keys (rollback applies keys through
  `PortRbApplyKeys`, see platform/seats.c **[unverified exact line]**).
- **UI → sim commands** (a mod menu or a mod-defined button that changes gameplay) travel as
  *input*: NETPLAY-V2 reserves **bits 10-15 of the 16-bit input word** for virtual inputs (V2 §2.2).
  The session binds them to mod actions in load order, at most 6 per session, and the binding is
  part of `sessionMods`. A ui script calls `ui.press_action("dash")`; sim scripts read
  `sim.action(player, "dash")`. Anything richer (a menu choice) is either a **sim option** fixed at
  session start or a V2 `MOD` event (V2 §2.2, "if ever allowed mid-session").

### 4.6 Custom object types

**Reserved slot.** Vanilla types run 0x00..0xDA (`include/constants/object_types.h:227`, `OBJ_SHOOTY_BOMB 0xDA`).
`OBJ_MOD = 0xFF` is reserved **[verify: no vanilla code treats type 0xFF as a sentinel]**.
`struct Object.type` stays a `u8` (`include/data.h:370`), so no struct layout changes.

**Level record.** Custom objects are ordinary `ObjectTemplate` records with `spawnTable = 1`,
`type = OBJ_MOD`, and a **tail after byte 36** carrying `u16 modType` plus up to 28 bytes of
per-placement parameters. This works without format changes because the walk is
`obj = (void *)obj + obj->unk1` (stride byte; the three walks at `code_080023A4.c:5901,5956,6016`) and "the size is per-record
data, not a constant" (docs/ROOM-FORMAT.md §4).

**Spawn.** `gSpawnFuncTable1` grows to 256 entries (it is a generated C array,
`rom_fn_tables.c:139`): `[0xDB..0xFE] = PortSpawnMissing` and `[0xFF] = ModObject_Create`.

```c
void *ModObject_Create(const struct ObjectTemplate *t, u8 player) {
    const struct ModType *mt = ModTypeFromTemplate(t);              /* tail u16 -> session table   */
    struct Task *task = TaskCreate(ObjectMain, sizeof(struct ModObject) + mt->stateBytes,
                                   0x1000, TASK_USE_EWRAM, ModObject_Destroy);
    struct ModObject *m = TaskGetStructPtr(task);
    InitObject(&m->obj, (struct ObjectTemplate *)t, player);       /* object.c:2031, unchanged     */
    m->modType = mt->index; m->stateIdx = mt->initialState; m->gen = NextGen();
    ObjectSetBounds(&m->obj.base, mt->hb[0], mt->hb[1], mt->hb[2], mt->hb[3]);
    ObjectInitSprite(&m->obj);
    m->obj.unk78 = ModObject_Tick;                                  /* the state fn ObjectMain calls */
    Lua_SimCall(mt, LUA_EV_INIT, m);
    return m;
}
```

`ObjectMain` (`object.c:185`) runs unchanged around it: graphics allocation (`sub_0809A630`, `:429`),
the state function `fp(obj)` at `:236`, speed integration, terrain sampling and collision. Custom
objects get KATAM physics and interactions for free. `ModObject_Tick` calls the Lua function for
`(modType, stateIdx)` from a frozen registry, so the object stores an index, never a Lua reference.

**Descriptors.** The 194 `gUnk_08351648[x->type]` sites are rewritten mechanically on the fork to
`OBJ_DESC(x)`:

```c
#define OBJ_DESC(o) ((o)->type != OBJ_MOD ? &gResBase.objDesc[(o)->type] : ModObjDesc((struct ModObject *)(o)))
```

Constant-index sites (`gUnk_08351648[OBJ_DROPPY]`, `object.c:452,2174`) stay as they are. The range
predicates `ObjType0To37`, `ObjType38To52` and friends (`include/constants/object_types.h`) become
`ObjIsEnemy(o)` and so on, consulting the mod descriptor's `category`. Without that, a custom enemy
would be exempt from enemy-only rules such as the score tally (`object.c:189-191`), the despawn SFX
(`:200`) and boss accounting in `ObjectDestroy` (`:339-345`).

**Hooking vanilla types** without replacing them: `sim.hook_type("waddle_dee", {...})`. A 256-bit
"hooked" bitmap is checked in `ObjectMain` before any Lua call, so unhooked types cost one bit test.

**Sprites for custom types**: a mod's `sprites/<name>/` (frame PNGs + anim.json) compiles into an
**extended sprite bank**. The compiler builds a new `struct SpriteTables` in the arena holding the
vanilla anims followed by the mod anims, and retargets `gSpriteTables` (`init.c:41`). In the GBA
era it also slices each frame into OAM shapes and charges `numTiles` (and the validator, §5.7,
checks the 512-tile heap). In the renderer era frames are whole textures.

**Custom abilities** are out of scope for v1. Kirby's ability is 5 bits
(`transitioningAbility & 0x1F`, `kirby.c:15270`) and the ability code is the largest state machine
in the game. v1 offers `on_ability_gain` (veto or replace with a *vanilla* ability) and per-ability
parameter hooks.

### 4.7 API sketch

```lua
-- scripts/sim/main.lua
local sim, fx = katam.sim, katam.fx

sim.object_type {
  id = "spinny", category = "enemy",          -- enemy | miniboss | boss | item | gadget | effect
  sprite = "spinny", hp = 2, score = 200, ability = "cutter",
  hitbox = { -6, -12, 6, 0 },
  params = { speed = "u16" },                 -- per-placement fields (record tail), editor-visible
  state  = { timer = "i16", spins = "u8" },   -- per-instance, lives in the task struct
  initial = "idle",
  states = {
    idle = { anim = "idle",
             enter = function(o) o.state.timer = 60 end,
             tick  = function(o) o.state.timer = o.state.timer - 1
                                 if o.state.timer <= 0 then return "spin" end end },
    spin = { anim = "spin",
             tick  = function(o) o.xspeed = o.facing * o.params.speed
                                 if o:touching_wall() then o:turn() end end },
  },
  on_damage = function(o, amount, src) if src.kind == "ice" then return amount * 2 end end,
}

sim.on("room_enter", function(ev) if ev.room == "kirbyfan.sky_tower:sky_1" then sim.store.visits = sim.store.visits + 1 end end)
sim.store_schema { visits = "u16" }
```

```lua
-- scripts/ui/main.lua
katam.ui.on("hud", function(g) g:text(4, 150, ("Sky visits: %d"):format(katam.view.store("kirbyfan.sky_tower").visits)) end)
```

### 4.8 Hook catalogue and insertion points

Every sim hook is gated by a per-event "anyone subscribed" flag, so vanilla play costs a branch
per site.

| Hook | Realm | Signature → result | Insertion point |
|---|---|---|---|
| `frame_begin` / `frame_end` | sim | `(frame)` | around `TasksExec()`, `game/src/main.c:246` |
| `input` | sim | `(player, keys) -> keys` | `GetInput()`, `main.c:231` (port's applied keys) |
| `room_load` | sim | `(room, list) -> list` (record edits for this load) | `FillLevelInfo`, `level.c:230` (the `PortRandoObjList` seam; one copy per player, two buffers, as rando.c does) |
| `room_exit` | sim | `(player, room)` | `level.c:166-167` (before `sub_08003028`, `code_080023A4.c:1820`) |
| `room_enter` | sim | `(player, room, slot)` | end of `FillLevelInfo`, after `sub_08002EC4` (`level.c:415`; fn at `code_080023A4.c:1764`) |
| `spawn` | sim | `(template) -> template \| false` | **first unify** the three dispatch sites (`code_080023A4.c:5991,6051,6132`) into one `PortSpawnFromTemplate`, then hook it |
| `object_init` | sim | `(o)` | end of `InitObject`, `object.c:2031` |
| `object_tick` pre/post | sim | `(o)` | `ObjectMain` around `fp(obj)`, `object.c:236` |
| `object_destroy` | sim | `(o)` | `ObjectDestroy`, `object.c:330` (and `sub_0809A580`, `:409`) |
| `object_damaged` | sim | `(o, amount, src) -> amount` | `sub_0809AA10`, `r10->unk80 -= sp04` at `object.c:706` |
| `kirby_damaged` | sim | `(k, amount, src) -> amount` | `sub_0804A728`, `kirby->hp -= objBase->unk63` at `kirby.c:9020` |
| `contact` | sim | `(a, b, kind) -> bool` (allow) | `HandleKirbyCollision` / `HandleObjectCollision` / `HandleAttackObjectCollision`, `object_collision.c:85,124,275` |
| `ability_swallow` | sim | `(k, from_obj, ability) -> ability` | `ability_objects.c:648,675` (`transitioningAbility = …kirbyAbility`) |
| `ability_gain` | sim | `(k, ability)` | `sub_0805C618`, `kirby.c:15270` |
| `ability_lost` | sim | `(k, ability)` | `kirby->ability = KIRBY_ABILITY_NORMAL` sites (`kirby.c:6190,9037,9134,…`); funnel through one helper first |
| `warp` (door/star/respawn) | sim | `(k, from, to, cell) -> to, cell` | `sub_080562D0`, `kirby.c:12865` (door and respawn branches per platform/warp.c comments); warp star dest `warp_star.c:1735,1806` |
| `game_flag` | sim | read/write via API | `GetStateSlot`, `code_080023A4.c:1461` (room/session/world scopes) |
| `song` | ui* | `(n) -> n` | `m4aSongNumStart`, `m4a.c:97` (+ `…OrChange` `:107`, `…OrContinue` `:128`). *Audio is P in NETPLAY-V2 (§3.1). Once V2's R10 removes the two sim reads of player state (`collection_room.c:1748`, `code_0806F780.c:9382`), the hook is per-client. Until then, replacing a song id is sim-class.* |
| `sfx` | ui* | `(obj, n) -> n \| false` | `PlaySfxInternal` macro, `include/data.h:15`. After V2 M8 this is the de-duplicated `(frame, kind, id, source)` sound-event stream |
| `hud` | ui | `(gfx)` | after `UpdateGameplayHud`, `hud.c:305`; becomes the HUD layer callback after the renderer split |
| `render` | ui | `(list)` | renderer's world render list (renderer project) |
| `save` / `load` | ui (local) + sim (session) | — | mod sidecar files beside the save (`save.c:41` `WriteSaveSectionByID` triggers the write) |

**Saves.** Vanilla SRAM is untouched. Each mod gets `saves/<slot>/mods/<modid>.bin` (sim schema
block, loaded into XRAM at session start and written when the game saves; S-class like SRAM itself, snapshotted on write, V2 §3.1) and `<modid>.json`
(ui-local preferences). Mod rooms' visited-door bits and world kill bits live in the same sidecar,
because `gVisitedDoors` is `[0x120]` and saved to SRAM (`ram_symbols.h:185`, docs/naming/doors.md).
In netplay the host's sidecar is sent at session start, the way the game's multi-cart mode already
sends the parent's world properties (`rollback.c` header comment).

### 4.9 Performance budget

Measured anchors: a re-simulated frame costs 2.5 µs natively and 6.2 µs in wasm, and an 8-frame
rollback is about 25 µs (`platform/port/rollback.h` header). Sim Lua runs inside every re-simulated
frame, so it multiplies by the rollback depth (V2 plans for 10 frames, so hooks run up to 11 times;
V2 §11).

| Item | Budget (wasm, mid-range phone) |
|---|---|
| sim Lua per frame, all mods together | **≤ 0.3 ms** by instruction count (V2 §11); a 10-frame rollback then adds ≤ 3 ms |
| sim Lua per frame, hard cap | 200 k VM instructions per mod per frame (deterministic count) → error policy §4.4 |
| hook dispatch, no subscribers | one predictable branch |
| a Lua call through the C boundary | ~1-3 µs wasm **[unverified; measure in M5 spike]** → 40 custom objects ≈ 120 µs |
| ui Lua per presented frame | ≤ 1 ms |
| build phase | ≤ 2 s at session start (shown as a progress step) |

---

## 5. Level editor

### 5.1 What it edits

| Entity | Backing data (pack) | Compiles to |
|---|---|---|
| **World/area graph** | `world/areas.json`, `world/rooms.json` | `RoomProps` set, area-map tables |
| **Room** | `rooms/<id>/room.json` | `RoomProps` (size, song, flags, camera, parallax 8.8) |
| **Tile layers** | `fg.csv` (+ tileset, palette), `bg.csv` via `bgs/` | `RoomTiledBG` + LZ77 tilemap |
| **Collision** | `collision.csv` + `collision/attributes.json` | RL `SolidityMap.unk0` |
| **Breakable/patch cells** | `patches.json` | `SolidityMap.unk4` type-1 records |
| **Doors** | `doors.json` (one per logical door) | `SolidityMap.unk4` tag-3 cells **and** progress tag-2 entries, paired |
| **Chests** | `chests.json` | progress tag-1 entries |
| **Objects** | `objects.json` (uids, typed params) | `ObjectTemplate` run with stride tails |
| **Object-borne exits** | warp stars (`template.unk1E` = dest room, `warp_star.c:1735`), mirrors/special doors **[dest field unverified]**, cannons | in `objects.json` |
| **Scripted exits** | boss rooms (Wiz 219, Crazy Hand 417 have no door edge, docs/ROOM-FORMAT.md §7) | annotations in `room.json` (editor-only metadata for validation) |
| **Animated tiles/palettes** | `gfx/tilesets/*.anim.json`, palette anim | `ForegroundInfo` / `ForegroundPalette` |
| **AI-Kirby nav** | `nav.json` | `gUnk_08D63C28` entry |
| **Music, palette per room** | `room.json.song`, `paletteDataIdx` ref | `RoomProps` |

The editor's unit of tile painting is the **metatile** (16×16 = 2×2 tile entries + 1 collision
index), because collision is 16 px while tiles are 8 px. The extractor infers a per-tileset metatile
dictionary from vanilla rooms (dedupe of 2×2 groups with their collision). A raw 8 px mode exists for
detail work. The dictionary is editor data. The compiled room is still CSV.

### 5.2 UX

- **World view** (left, or full screen): rooms as nodes grouped by area, laid out from saved
  positions (auto-layout the first time). Edge styles by kind: tile door, warp star, mirror,
  scripted. Errors are badged on nodes. Double-click opens the room.
- **Room canvas** (centre): layer stack (bg parallax preview at its 8.8 ratio, fg, collision
  overlay colour-coded by attribute class, objects with sprite previews and hitboxes, doors as
  linked rectangles, camera bounds, spawn cells, nav nodes). Tools: metatile brush, rect/fill,
  collision brush, object place/drag, door link (drag from a door to a room node, then pick the
  arrival cell in a mini-canvas), eyedropper.
- **Inspector** (right): typed fields from `objects/types.json` schemas and mod `params`.
  Unknown template bytes are shown raw and preserved.
- **Validation panel** (bottom): live list. Click to jump.
- **Playtest bar**: Play here · Play from door · Stop · Reload room · Toggle collision overlay in game.

### 5.3 Live playtest

1. The editor's working set is the top resource layer (`editor`, §1.3) in the same page as the game wasm.
2. **Play here**: take a rollback snapshot (`PortRbSnapshotSave`, `rollback.h`). Recompile the
   edited room's resources, `Res_Invalidate`, then `PortWarp(0, room, cellX, cellY)`
   (`platform/warp.c:160`). Warp takes the RESPAWN branch and runs the game's own fade, teardown,
   `FillLevelInfo` and respawn (warp.c header), so the room is reloaded from resources and Kirby
   lands where the cursor was. Warping to the current room works: `FillLevelInfo` handles
   `prevRoom == roomId` (`level.c:371-372`), and with one player the slot is re-decompressed
   (`level.c:340-366`).
3. **Stop**: `PortRbSnapshotLoad`. The game is back exactly where it was before playtest.
4. **Live collision tweak** while playing (editor-only, never in netplay): write straight into the
   active slot's `gUnk_02024ED0[slot]` buffer (`ram_symbols.h:98`), so the change is felt instantly.
   The next reload makes it real.
5. Native builds get the same loop over a local WebSocket bridge **later** (M12). It is not needed
   for v1.

### 5.4 Web-first, not ImGui (recommendation)

| | Browser app around the wasm | Dear ImGui in the native app |
|---|---|---|
| Reach | Every platform the port runs on, phones included (this user works through `terminal.sels.tech`) | Desktop only |
| Playtest | Same page, same wasm, instant | Instant |
| File access | File System Access API (Chromium) to edit a real folder; OPFS + zip import/export elsewhere | Native FS |
| UI work for graph/canvas/inspector | DOM + canvas/WebGL; mature libraries | Fast to start, slower to polish; poor on touch |
| Shared code | The C pack compiler and validator compile into the same wasm, so there is **one implementation** | Same |
| Risk | Large-file perf in the DOM (mitigated by canvas rendering) | A second UI stack to maintain |

Decision: **web editor** (TypeScript, canvas 2D first; WebGL when the renderer's tile pipeline can
be reused). Native gets an **ImGui dev overlay** later, for runtime inspection (object inspector,
room info, collision overlay) only. That overlay is a debugging tool, not a second editor.

### 5.5 Tiled as the cheap first step

Ships before the web editor (M4) so people can make rooms immediately.

- **Export**: room folder → `.tmj` (JSON, preferred) or `.tmx`, with an 8 px grid. Layers: `bg`
  (tile), `fg` (tile; Tiled's flip flags map to GBA hflip/vflip, and the GBA palette bank goes in
  a per-tileset split: one Tiled tileset per palette bank, all over the same PNG), `collision`
  (tile layer using a generated 16×16 collision tileset placed on even cells only), `objects`
  (object layer, `type` = object type name, custom properties = typed params, `uid` property),
  `doors` (object layer, rectangles with `dest_room`/`dest_x`/`dest_y`). Room props go in map
  properties.
- **Import** validates evenness of the collision placement, tileset/bank consistency, unknown
  property names, and the §5.6 rules, then writes the canonical room folder. Gate F2: export → import
  with no edits is identity.
- Tiled is interchange, not the canonical format. TMX properties round-trip poorly, and Tiled has
  no concept of the door pairing invariant.

### 5.6 Validation

| Check | Severity | Why |
|---|---|---|
| Door pairing: every logical door compiles to N tag-3 cells and exactly one tag-2 entry; destinations exist | error (compiler guarantees) | A mismatch hangs the game: both walks are `for (;;)` with zero-stride termination (docs/ROOM-FORMAT.md §7; `sub_08002C18`, `code_080023A4.c:1648`) |
| Destination cell is not solid and has floor within N cells | warning | Arrive-in-wall |
| One-way doors | info | Often intended |
| Reachability from the area's entry room over all edge kinds | warning on unreachable rooms | 18 vanilla rooms have no incoming door edge (warp.c header), so object/scripted edges must count |
| Boss/miniboss room has an exit (door unlocked by trigger, or scripted exit annotation) | warning | |
| Object list: stride ≥ 36, terminator present, type valid, `spawnTable` ∈ {1, 2} | error | |
| Kill-tracked objects per room (template `unk2 == 0`): bit indices unique and < 32 | error (until M8) | `gUnk_02023358` is `u8[4]` per slot (`ram_symbols.h:90`, `code_080023A4.c:1438`) |
| Room size: `(w/16)*(h/16) ≤ 1950` | error (until M8) | fixed per-slot buffers `gUnk_02024ED0[4][1950]`, `gUnk_02028EE0`, `gUnk_02026D60[4][1954]` (`ram_symbols.h:98-103`) |
| fg tileset ≤ 1024 tiles, palettes within available banks | error (until renderer) | 10-bit tile index; LZ77 into charblock at `0x6008000` (`level.c:116`) |
| OBJ tile demand in any spawn window ≤ 512 | warning (until renderer) | `VramMalloc` heap `0x200`, and overflow returns `ewram_end` instead of failing (docs/RANDO.md §3); reuse `tools/rando_preview.py`'s estimate |
| Tasks: estimated live objects ≤ 128 | warning | `MAX_TASK_NUM 0x80` (`include/task.h:16`) |
| Nav graph present | warning | CPU Kirbys navigate by `gUnk_08D63C28`. The editor generates a trivial graph (one node per walkable region) or marks the room "no helper navigation" |
| Room referenced by hard-coded engine code (0x321, 0x323, 0x38E, 0x396, 918, 34) | warning | `code_0814A828.c:364`, `cutscene_trigger.c:1465`, `code_08026044.c:550`, `dark_mind.c:9852`, `code_080023A4.c:1802,1937` |
| Song/palette/tileset refs resolve | error | |

### 5.7 GBA limits: where each is enforced and when it is lifted

| Limit | Imposed by | Until lifted | Lifted by |
|---|---|---|---|
| Room ≤ 1950 collision cells | `gUnk_02024ED0[4][1950]` and siblings | validator | **M8**: the three `ram_symbols.h` macros become per-slot pointer arrays into XRAM, sized at load. `gUnk_02024ED0[slot][i]` still compiles unchanged when the macro names a `u8 *[4]`. |
| 32 kill-tracked objects/room; session/world bitsets | `gUnk_02023358`, `gUnk_02023368`, `gUnk_02028BF0` (`code_080023A4.c:1438-1444`) | validator | M8: bitsets in XRAM (world bits for mod rooms in the sidecar) |
| Room ids ≤ 982, content index ≤ 286, `gVisitedDoors[0x120]` | ROM table extents; SRAM layout | mods can only *edit* rooms until M8 | M8: arena-built `RoomProps`, an XRAM extension for `gVisitedDoors` indices ≥ 0x120 (6 sites in `code_080023A4.c`, 1 in `save.c`) |
| `u8` object type | `struct Object.type` | — | `OBJ_MOD` + `u16` mod type (M6); no layout change |
| fg tiles ≤ 1024, 16 banks × 16 colours | 10-bit tilemap index, VRAM, palette RAM | validator | **renderer split**: tile layers drawn from resources; RGBA tiles; any count |
| 512 OBJ tiles shared with Kirby, HUD and effects | `VramMalloc` heap (`sub_080332BC`, docs/RANDO.md §3) | validator warning | renderer split: VRAM becomes a texture cache and `VramMalloc` becomes accounting with a large cap |
| 3 BG layers (fg, bg, an unused 3rd) | `LevelInfo.unk180[3]`, DISPCNT | — | renderer split: N parallax layers as render-list entries (presentation realm) |
| 240×160 view | five places (docs/VIEW.md) | already lifted by `platform/view.c` | — |
| 128 tasks; IWRAM heap 0x2604 | `task.c` | validator warning | raise `MAX_TASK_NUM` if needed (task links are `u16` IWRAM offsets, so tasks stay in IWRAM; ~20 bytes each) **[unverified headroom]** |
| 4 room slots | one per player | not a modding limit | — |
| 5-bit ability id | `transitioningAbility & 0x1F` | v1 out of scope | post-v1 ability work |

---

## 6. Roadmap

Sizes are engineer-weeks for one person who knows the codebase: S ≤ 1, M 2-3, L 4-6, XL 8+.

| # | Milestone | Size | Depends on | Ships / test |
|---|---|---|---|---|
| **M0** | **Resource seam R0**: `platform/port/res.h`; ROM table macros → `gResBase.*` variables; `Res_Get` over them; rando behind `Res` as builtin layer L2; read-past-end guard (§1.4) | S-M | — | Harness bit-identical. **Start now on `fork`.** |
| **M1** | **Asset manifest + extractor v1 + pack compiler (R1)**: rooms (props, collision, objects, progress/doors, nav raw, fg/bg tilemaps, tilesets, palettes, anims), object descriptors, metatile attrs; verify the tilemap interpretation first (§2.3); F0/F1 gates; `--res-identity` | L | M0 | CI: identity for every resource; `pack.lock` equal across 3 arches. Manifest generation and the tilemap verification can **start now**. |
| **M2** | **Arena-backed runtime (R2)**: arena at `0x09100000`; compiler relocates internal pointers; DMA comparison by content; F3 live check over all 286 rooms via `PortWarp` | M | M1 | Harness identical by content; cross-arch state hash identical |
| **M3** | **Data-mod loader v0**: `mod.toml`, folder/zip, overlay + patches, load order, conflict report, content hash, mod list UI (web + native), "no vanilla bytes" packer check; rando moved to a builtin mod | M | M2 (M0 is enough for object-list-only mods) | First mods: enemy re-placements, descriptor tuning. Hash test: cosmetic mod → same hash |
| **M4** | **Tiled export/import** (CLI + in-browser) + vanilla-limit validator v1 | M | M1, M3 | First custom *rooms*, replacing vanilla rooms in place (no new ids yet). F2 gate |
| **M5** | **Lua sim runtime v0**: 5.4.7 + patches in all builds; sandbox; upvalue verifier; schema state (task-struct and XRAM `sim.store`); hooks `frame_*`, `room_*`, `spawn`, `object_*`, `object_damaged`, `kirby_damaged`, `ability_*`; build realm; rando ported to Lua (must match C output) | L | M3; for the netplay test, V2 M1 (sim block at 0x02040000) and V2 M2 (portable snapshots and canonical code IDs) | Determinism suite: scripted input + test mods, per-frame state hash equal on wasm, x86-64 and arm64; `netplay-rb-test` with mods; rollback self-test with mods |
| **M6** | **Custom object types**: `OBJ_MOD`, `gSpawnFuncTable1[256]`, `OBJ_DESC` rewrite (194 sites), category predicates, sprite bank extension + OAM slicing, template tails | L | M5, M2 | A mod enemy placed via Tiled behaves under KATAM physics; vanilla harness identical |
| **M7** | **Web editor v1**: world graph + room canvas + inspector + validation + playtest (snapshot/warp/stop) + save as mod folder/zip | XL | M3, M4 (formats), warp.c | Make a 3-room mod start to finish in the browser |
| **M8** | **Sim limit lifting**: XRAM; per-slot buffers sized per room; extended kill bitsets; new room ids ≥ 1000 and content index ≥ 287; sidecar saves for mod rooms; area membership for new rooms (pause map shows "?" until map art exists) | M-L | M2, V2 M1 (sim block) | Rooms larger than 1950 cells; new rooms reachable by doors; save/load round trip |
| **M9** | **Renderer-era assets**: presentation kinds consumed by id; RGBA/HD tiles; unlimited tiles, palettes and layers; tilemap moves to presentation; audio sequencer out of the sim (event queue); hash drops VRAM, PLTT, OAM and audio | L | **renderer split**, V2 M8 (presentation decoupling) | Texture mod on one peer only, no desync, *unmasked* hashes |
| **M10** | **Presentation Lua + audio/text mods**: ui VM, HUD/render hooks, song event-list import, MIDI import, sample replacement, mod settings UI | M | M5, M9 for per-client audio | Music pack mod; HUD mod |
| **M11** | **Netplay with mods** = V2's M9: HELLO content hash + mod list; content-addressed blob fetch (R2 or peer); confirm-or-refuse for sim scripts; virtual input bits 10-15 bound to mod actions | M | V2 M5 (protocol v2), M3, M5 | 2-peer session with a sim mod fetched from the host |
| **M12** | Native ImGui dev overlay; native ↔ web-editor bridge for desktop playtest | M | M7 | Optional |

Critical path to "a player makes and plays a custom level": M0 → M1 → M2 → M3 → M4 (Tiled), about
10-14 weeks. Custom enemies add M5 → M6. The full editor (M7) can run in parallel with M5 and M6
once M4's formats are frozen.

**Can start immediately on `fork`, with no dependency on the renderer or netplay:**

- M0 entirely. It is macro edits in `game/generated/port/rom_data.h` plus `platform/port/res.h`.
  Coordinate with whoever is editing `game/` so the two do not collide on generated headers.
- M1's manifest generator and the tilemap-format verification (an extension of `tools/room_export.py`).
- The M5 *VM spike*, off the game entirely: embed Lua 5.4.7 with patches, then run a determinism
  test of a script workload hashed per step across wasm, x86-64 and arm64. Measure call cost.
- Unifying the three spawn dispatch sites into one function (behaviour-neutral, needed by `spawn`).

**Blocked on the renderer split**: M9; lifting the tile, palette and OBJ heap limits; presentation-
only texture mods with *unmasked* hashing. **Blocked on NETPLAY-V2**: the netplay test of M5 (V2 M1/M2),
M8's use of the sim block (V2 M1), and M11 (V2 M5). Note that V2's M0/M1 (single clang toolchain,
sim block) land *before* M5 here, which is the order needed.

---

## 7. Top risks

1. **Undecoded formats** (tilemap/tileset interpretation, nav scripts, `priorityFlags` bit 5, mirror
   destinations). Mitigation: raw-preserving extraction, F0/F3 gates, decode incrementally. Editing
   capability grows as decoding does. Round trip never depends on understanding.
2. **Hidden ROM reads**. Only ~160 of 28,591 labels are named. The rest are reached by pointer
   chasing, so code that follows a pointer into "old" ROM data silently ignores a mod. Mitigation:
   the compiler relocates every pointer it knows; a debug "ROM read audit" (native: `mprotect` the
   migrated ROM ranges after R2, log the faulting site) turns silent no-ops into a list.
3. **Determinism of Lua across 32-bit and 64-bit builds.** Mitigation: the stateless-VM model,
   integer-only patched VM (seed, no `__gc`/weak tables, key-ordered `pairs`, one clang toolchain),
   and a cross-arch CI determinism suite *before* any netplay use.
4. **Hash domain vs presentation mods.** VRAM, palette RAM and m4a state are hashed today.
   NETPLAY-V2 masks them as S\* (and classes audio as P) from its M1, and removes them for real at
   its M8 (renderer split). Until V2 M1 ships, every graphics or audio mod is effectively
   sim-class. Sound is already per-client (`PlaySfxInternal` gates on `gLocalPlayerId`,
   `include/data.h:16`), which is why V2 makes audio P.
5. **Fixed-address arena assumptions**: identical addresses on every peer, < 4 GiB, no collision
   with the game's past-end reads at `0x09100000`. Mitigation: guard in M0, cross-arch state-hash
   test in M2.
6. **Door/progress dual-table invariant** hangs the game when violated. Mitigation: one door
   definition compiled to both sides, so the invariant is enforced by construction, plus the
   validator.
7. **Hard-coded room ids and object-type ranges in engine code** (§5.6 list; `ObjType*` predicates).
   Edits to those rooms can break cutscenes. Custom types are invisible to the predicates until
   they are rewritten (M6).
8. **Scope**. The editor is the largest item. Mitigation: Tiled first (M4) gives players a working
   path months earlier.
9. **Legal hygiene**. Mitigation: the extractor ships no data; the manifest holds addresses and
   names only; the packer's vanilla-hash check; the pack cache is never uploaded or shared by any
   built-in feature.
10. **wasm memory on mobile**. The arena lives in already-reserved linear memory, but extraction and
    compilation peak memory needs measuring on iOS Safari **[unverified]**.

---

## 8. Prior art borrowed, and what was rejected

- **Ship of Harkinian / libultraship** (`/bigdata/agent-tom/projects/soh-web`): ROM → archive extraction at
  first run (`soh/soh/Extractor/Extract.cpp`); path-keyed resources with later archives overriding
  earlier ones (`ArchiveManager.cpp:51-76`); folder and zip archives interchangeable; an `alt/` prefix
  for alternate assets (`ResourceManager.cpp:111-124`). *Rejected*: per-resource lazy loading
  through `shared_ptr` caches on the hot path. KATAM's sim needs stable raw pointers, so resolution
  is per session.
- **sm64coopdx** (`/bigdata/agent-tom/projects/sm64coopdx`): the shape of the hook catalogue
  (`src/pc/lua/smlua_hooks.h:19-88`, `HOOK_UPDATE` … `HOOK_ON_WARP`), `hook_behavior(id, list, replace,
  init, loop)` for custom and overridden object behaviours (`smlua_hooks.c:832`), the mod folder plus
  file hashes and cache for transfer (`src/pc/mods/mod.h`, `mod_cache.h:15`), and live reload.
  *Rejected*: one shared `lua_State` for all mods, and **sync tables**
  (`smlua_sync_table.c`) that replicate Lua state by authority over the network. That model is
  incompatible with input-only rollback, which is why state lives in snapshotted game memory here.
- **Tiled**: interchange only (§5.5).
- **decomp tooling**: `gbagfx` PNG/palette conventions, `mid2agb` semantics for MIDI import,
  `*.inc` labels and `graphics_file_rules.mk` tile counts as manifest sources. No decomp asset bytes
  are used (the decomp tree contains extracted assets and a `baserom.gba`; none of it may enter the
  port).

---

## 9. Contract with NETPLAY-V2

Read against `design/NETPLAY-V2.md` (its §3, §6, §8, §10, §11). Agreed, and adopted above:

| Point | NETPLAY-V2 | Here |
|---|---|---|
| Session identity | HELLO `contentHash` = `H(simVersion, romHash, [(modId, modVersion, simContentHash)], levelSetHash, sessionMods)` (§8, §11) | Each term is defined in §3.5. `levelSetHash` is the resolved sim-resource table. |
| Memory classes | S / P / S\* masks; PLTT, VRAM, OAM, shadow regs and upload queues are S\*; audio is P (§3.1) | Resource realms in §1.2 map onto these: presentation kinds only ever reach S\* or P memory. |
| Sim block | port sim block at `0x02040000`, every platform (§3.1, their M1) | XRAM *is* that block (§1.4); mod sub-regions listed there. |
| Mod state | typed sim store, 64 KiB, globals and upvalues frozen (§11) | §4.4 (task-struct state + store + attachments, load-time upvalue verifier). |
| Lua rules | integer-only 5.4, fixed seed, ordered `pairs`, no `__gc`/weak tables, instruction budget, deterministic disable (§11) | §4.1 patches 1-5, §4.4 error policy. |
| CPU budget | ~0.3 ms/frame wasm for all sim hooks (§11) | §4.9 |
| Virtual inputs | bits 10-15 of the input word (§2.2) | §4.5, bound to mod actions in load order |
| Content transfer | content-addressed blobs, R2 (20 MB/room) or peer; vanilla never transferred; romHash mismatch refuses (§11) | §3.5 |
| Function pointers in sim memory | canonical code IDs (§6 R5) | Mod trampolines get IDs in a reserved port range (§4.4). |
| Toolchain | one clang toolchain and language mode for sim code (§6 R2) | The Lua VM is compiled as sim code under the same rule (§4.1 patch 2). |

Still open (to settle with the NETPLAY-V2 author):

1. **SRAM address.** V2's table lists SRAM at `0x0E000000`; `main` has it at `0x09000000`
   (`platform/native/mem.c:71`). The resource arena at `0x09100000` assumes the `main` layout and
   must move if SRAM does.
2. **Sim block size.** This design needs about 64 KiB (mod store) from M5 and up to ~1 MiB for M8
   room growth. V2's snapshot-size estimates (§3.2, ~292 KiB raw, ~10 KB compressed) should budget
   for it. Most of it is zero, so it compresses well.
3. **Arena in snapshots.** The arena is immutable for a session and identical on every peer (the
   same `levelSetHash` gives the same layout), so it is *not* in snapshots. A joiner rebuilds it from
   its own pack plus the fetched mods before loading the donor snapshot. V2's join sequence (§4.2)
   needs this "build content, then load snapshot" step.
4. **Editor sessions.** The editor layer (§1.3) and `Res_Invalidate` are local-only. A netplay
   session refuses them (they would change `levelSetHash` mid-session). Mid-session level changes
   would be a V2 `MOD` event plus a full re-resolve; not planned.
5. **Script OOM** (§4.1 patch 5) is a session-fatal error. V2 should carry the reason to all
   peers through the seal/desync-report path.
