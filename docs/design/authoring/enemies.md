# Enemies and other object types

Status: per-type **stats are implemented** (`objects/<name>.object.json`, `katam-object/1`,
compiles to the `obj_desc` pack kind the game loads today; 219/219 round-trip). **Behaviour is
designed here**, not implemented: it is code in `game/src/<enemy>.c`.

The inventory of where every tuning number lives is the semantics registry's
`tuning_inventory.json` (branch `semantics`); this document assigns each to a stage.

## What an object type is in the game

| piece | where | data or code |
|---|---|---|
| descriptor: hurt anim slots, score, hp, copy ability, palette spec, VRAM tiles, start state, animation list | `gUnk_08351648[type]` (`obj_desc`) | **table** — authoring today |
| per-placement parameters (movement pattern, speed, rectangles, flags) | `ObjectTemplate` in each room | **room data** — authoring today (`objects[].params`, registry `object_params.json`) |
| contact damage, hitbox, terrain bounds | literals in each type's constructor (`ObjectSetFunc`, `ObjectSetBounds` calls) | code |
| element resist/weakness | `object.c` table | table, not yet a resource |
| keyframed motion scripts (`{xspeed, yspeed, xaccel, yaccel, frames, animSlot}`) | per-file C tables (e.g. `kracko.c`) | table, not yet a resource |
| walk/fly speeds, jump impulses, timers, state machine | literals and control flow in the type's `.c` | code |
| sprites and animations | `gSpriteTables` | ROM table, no authoring kind yet |

## The authoring file

```json
{"format": "katam-object/1", "id": "object:waddle_dee_1", "bind": {"type": 0},
 "constName": "OBJ_WADDLE_DEE_1", "category": "enemy", "ability": "normal", "numTiles": 9,
 "stats": {"hurtAnimSlot": 4, "hurtAnimSlotAlt": 5, "score": 200, "hp": 2, "paletteSpec": 776},
 "raw": {"pad0A": [0, 0]},
 "code": {"constructor": "0x080A3CF1", "animation": "0x08353408"}}
```

(Decompiled with the registry loaded; without it `stats` is empty and those fields sit in `raw`
under their C names `unk0 unk1 unk2 unk4 unk8`.) A mod overrides one by shipping a file with the
same id: e.g. tougher Waddle Dees = the same file with `"hp": 4`. `code` holds the type's
constructor and animation bank as ROM references: an object type cannot be *created* from data
yet, only tuned.

Tuning is playable today: `katam compile <mod> --base <vanilla> -o OUT`, then
`build/lp64/katam --res-pack OUT/pack rom.gba`.

## Migration path for behaviour (MODDING.md R1/R2, §4.6)

Stages, each gated like M1 was (identity round trip, then `regress-pack` with the goldens
unchanged; layout-neutrality for any C change):

1. **Tables that are already tables** (`data_candidate: yes` in the inventory): element
   modifiers, motion scripts, boss HP by player count, boss music. Each becomes a manifest kind
   (R0 already gives every ROM label a slot; C-defined tables need a `RES_BASE` view first) and
   an authoring section on the object file: `"element": {"fire": 2, "ice": 0, …}`,
   `"motion": [{"xspeed": …, "frames": 12, "anim": 3}, …]`. No behaviour change, pure data.
2. **Literals that are parameters** (`partial`): contact damage, hitbox, terrain bounds, the
   speeds a constructor picks from `subtype`. The C change is mechanical: the literal becomes a
   read from a per-type parameter block (`gObjParams[type].contactDamage`), whose vanilla values
   are the old literals (so the round trip is the gate). Authoring: `"combat": {"contactDamage":
   1, "hitbox": [-6, -12, 6, 0]}`, `"movement": {"walkSpeeds": [128, 192, 256, 384]}` in 8.8
   px/frame, named in the schema with units. Done per type, highest-value enemies first.
3. **Control flow** (`no`): state machines stay C for vanilla types. New types are Lua
   (`sim.object_type{…}`, MODDING.md §4.6-4.7) registered as `OBJ_MOD` with a per-placement
   tail; the object file then names a script instead of `code`:
   `"script": "scripts/sim/spinny.lua"`, with `params` declared in the file so the editor and the
   validator know the placement fields.

The object file's shape is stable across stages: each stage adds an optional section; files
written for an earlier stage stay valid.

## For AI generation

An enemy *placement* is fully authorable now: type + position + `params` (`katam describe
objects <name>` lists each type's parameters with meanings). An enemy *variant* is authorable as
stats. New enemy *behaviour* waits for stage 3; until then a model should combine existing
types and parameters.
