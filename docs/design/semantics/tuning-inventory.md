# Tuning-data inventory

Where the gameplay numbers (hp, damage, score, abilities, speeds, timers)
live in `game/src`, and which of them can be served as data under the
resource migration in `docs/design/MODDING.md` (R1 identity round trip,
R2 arena-backed). Each row's citations are in
`assets/schema/semantics/tuning_inventory.json`. The descriptor table is
decoded field by field in `assets/schema/semantics/obj_desc.json`, and the
boss placement parameters in `object_params.json` (types 0x38..0x52).

Kinds: **table** means a const array or struct, either in C or ROM-resident,
that can be served the way `obj_desc` is. **Literal** means the number is
hard-coded in a behaviour function, so changing it needs a code change first.
**Derived** means it is computed from another table and has to follow that
table when it changes.

## Summary

| subject | where | kind | recommendation |
|---|---|---|---|
| Enemy hp, score, copy ability | `gUnk_08351648` (`obj_desc`): `unk4`, `unk2`, `kirbyAbility`; read in `InitObject` / `ObjectMain` (object.c) | table (ROM) | Already a manifest kind. Serve it in R2 (only the `unk10`/`unk14` pointers need relocation). This is the first tuning mod. |
| Item heal amount | `obj_desc.unk4` for 0x5E..0x64 (bonus.c); rate of 1 hp per 5 frames is a literal | table + literal | Comes with `obj_desc`. |
| Boss/mini-boss hp × player count | `gUnk_08351530` s16[27][4] at 0x08351530, `InitObject` object.c:2078 | table (ROM) | **New manifest kind `boss_hp`.** `obj_desc.unk4` is ignored for 0x38..0x52. |
| Dark Mind form-1 hp per encounter | `gUnk_08351608` s16[8][4]: rows 0..3 are hp, indexed by placement `subtype2` | table (ROM) | Add it to `boss_hp`. Rows 4..7 are 0x4000/hp and have no reader we found. |
| Boss hp-bar scale | `gUnk_08351458` u16[27][4] (hud.c) | derived | Every cell is `floor(0x4000/hp)`, checked over the whole ROM table. The pack compiler should **derive** it from `boss_hp`. Otherwise an hp mod draws a wrong bar. |
| Boss phase thresholds | `unk80 <= gUnk_08351530[row][n-1] >> 1` in each boss file, with the row as a literal | derived | Follows `boss_hp` automatically. Quirk: Dark Meta Knight reads **row 0x10 (Gobbler)** for its thresholds but starts with row 0x16, so editing DMK hp does not move its phases. The fix is a code change: use `ObjTypeAltIdx`. |
| Element resist/weakness | `gUnk_08352AD0` (object.c:51), 6 × s8 per type, added to the attacker's damage per attack-kind bit | table (C) | **New kind `obj_element_mods`**, next to `obj_desc`. |
| Boss music | `gUnk_08352D80` (object.c:140) | table (C) | Trivial new kind, or fold it into a boss table. |
| Enemy/boss motion scripts | `struct Unk_08353510` arrays in 24 files (Kracko, Heavy Knight, Foley, Droppy, ...): {xspeed, yspeed, xaccel, yaccel, frames, animSlot} | table (C) | **New kind `motion_script`**. Each array is self-contained and terminated by a 0 frame count. |
| Contact damage | `InitObject` sets `base.unk63 = 1` for all types. About 50 attack constructors set their own (nearly all 1). | literal | R1: none. Later: an `obj_desc` extension field, read by `InitObject`. |
| Hitboxes / terrain bounds | `sub_0803E2B0` / `ObjectSetBounds` with literals in every constructor, often re-set per state | literal | Possible as a per-type default box. Per-state boxes stay in code. Low priority. |
| Enemy speeds, timers, odds | literals in each state function (e.g. bronto_burt.c:245 `0xC0`) | literal | Leave in code. Only a scripted/custom-object mod (M6) changes behaviour. |
| Boss attack timers / odds / speeds | literals (`counter = 0x5A`, `RandLessThan(7)`, mr_frosty.c), plus some motion-script tables | literal | Leave in code. The motion-script tables come with `motion_script`. |
| Boss defeat reward (shard drops) | `gUnk_08350E58` function table plus literal positions (code_0806F780.c) | literal | Progress logic, not tuning. Out of scope. |
| Kirby max hp | `NumVitalitiesCollected() + 6` (kirby.c:6187) | literal | One constant. Make it a named global if anyone asks. |
| Kirby walk/dash/jump/gravity/float | literals in about 40 state functions (kirby.c 6000..16500). The same constants repeat (e.g. `-0x250` fall cap at about 15 sites). | literal | First step: named constants (no behaviour change, harness identical). Then a small `kirby_physics` block. Not R1/R2. |
| Kirby small motion curves | `gUnk_0834C160..gUnk_0834C5B7` (kirby.c:386..715) | table (C) | Data-shaped already. Low value. |
| Ability attack damage & speeds | `unk63` / `xspeed` literals in each attack-object constructor (about 97 sites in code_0806F780.c). No per-ability stat table. | literal | A per-attack table keyed by constructor is possible, but it needs every constructor rewritten. Defer. |
| Spit-star damage | `(obj_desc.unk4 >> 1) + 9` (code_0806F780.c:5820) | derived | Note for modders: raising an enemy's hp also strengthens its spit star. |

## obj_desc (gUnk_08351648) fields

| field | meaning | confidence |
|---|---|---|
| `unk0`, `unk1` | Hurt-pose animation slots: indexes into the `unk14` AnimInfo list. After a hit, `Object.unk83` is set to one of the two at random. Inhale and carry code test `unk83 >= unk0`. | confirmed |
| `unk2` | Score, added to the killing Kirby's score | confirmed |
| `unk4` | Starting hp (`Object.unk80`) for non-bosses. Heal amount for food. Also sets the spit-star power. | confirmed |
| `kirbyAbility` | Copy ability on inhale (some constructors override it) | confirmed |
| `unk8` | Palette: the low 12 bits are the animId whose palette is loaded (variant = placement `unkF`). Bit 0x1000 means a ref-counted slot, freed on destroy. 0 means shared slot 15. | confirmed / bit 0x1000 likely |
| `pad0A` | Padding (0 in all rows) | confirmed |
| `numTiles` | OBJ VRAM tiles per instance. 0 = no sprite, `0xFFFFFFFF` = shared block at 0x06012000. Also sets the inhaled-star size. | confirmed |
| `unk10` | Start-state function (becomes `Object.unk78`) | confirmed |
| `unk14` | AnimInfo list, indexed by `Object.unk83` | confirmed |

## Boss placement parameters (types 0x38..0x52)

- **`unk1A, unk1C, unk1E, unk20`**: the **activation (wake-up) rectangle**,
  as x-offset, y-offset, width and height in px. The boss stays in its start
  state until the target Kirby's origin is inside the rectangle. Mr Frosty,
  Bonkers, Phan Phan, Box Boxer and Boxy measure it from the template x/y;
  the others measure it from the boss's current position. When it wakes, the
  boss copies the **current camera limits** into its own arena bounds
  (`Macro_081003EC`), so the rectangle is not the camera rect: the arena comes
  from the room's scroll locks. Retail rectangles usually start at room x 0
  (`unk1A == -x`) and are about one screen wide.
- **`subtype2`** is copied to `Object.subtype`. For most bosses a non-zero
  value is the hard/rematch variant: it forces the "below half hp" behaviour
  (`subtype || hp <= max/2`), and on Mr Frosty it also raises the walk speed.
  Retail uses 1 in the arena-style rooms 955/956 (both with `unk22 = 5`).
  King Golem sets it itself in arena mode or once shard 1 is owned. For Dark
  Mind form 1 it is the encounter index (0..3) into its hp table.
- **`subtype1`**: initial facing for Master Hand, Crazy Hand and Bombar
  (0 = left). It selects the encounter variant for Dark Meta Knight (non-zero
  means a different intro and no shard drop) and the start state for the
  Dark Mind trigger. The other bosses carry non-zero values in some rooms, but
  nothing reads them.
- **`unk22` bits**: 1 = counts toward the level kill counter (read by lever
  walls and spawners). 2 = pushed by solid objects. 4 = no boss-music switch
  or restore. 8 (Boxy only) = the box does not release a Minny.
- **`unk18 = 256` on Boxy**: no reader exists, so it is unused (probably an
  editor leftover).

## Open questions

- Which attack-kind bit in `unk68` corresponds to which element (fire, ice,
  spark, ...) for the six modifier columns.
- The rows 4..7 of `gUnk_08351608` (a HUD scale): we found no reader.
- What `Object.subtype` does in Crazy Hand, Dark Meta Knight and Dark Mind
  form 2 (listed as `undecoded`).
- Whether the Dark Meta Knight threshold row (0x10) is the original
  behaviour or a decompilation artefact. It should be checked against the
  disassembly.
- The flag behind `gUnk_0203AD10 & 4` ("arena mode" in these notes) needs
  confirming at runtime.
