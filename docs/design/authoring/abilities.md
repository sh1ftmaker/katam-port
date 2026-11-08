# Copy abilities ("powers")

Status: **designed, not implemented.** No ability data is a resource yet.

## What exists

- 27 abilities (`game/include/constants/kirby.h`, `KIRBY_ABILITY_NORMAL` … `MASTER`); the
  ability lives in 5 bits (`transitioningAbility & 0x1F`, `kirby.c`), so 32 slots exist.
- Which ability an enemy gives is data: `obj_desc.kirbyAbility` → `object:<name>.ability`,
  authorable today (e.g. make Waddle Doo give `fire`).
- Ability statues in rooms (`ability_statue_1..4`, `ability_statue_random`) carry their ability
  in placement parameters (registry `object_params.json`).
- Everything else is code: per-ability dispatch tables (`kirby.c`), attack objects
  (`code_0806F780.c`: damage `unk63`, attack-kind bits `unk68`, speeds, lifetimes),
  animations. The semantics tuning inventory lists them (`ability.attack_damage`,
  `ability.projectile_speed`, `ability.dispatch`).

## The authoring file (planned): `abilities/<name>.ability.json`

```json
{"format": "katam-ability/1", "id": "ability:fire", "bind": {"ability": 1},
 "name": "Fire", "hat": "sprite:kirby_fire_hat",
 "attacks": {
   "breath": {"damage": 2, "kind": ["fire"], "speed": 512, "lifetime": 24},
   "dash":   {"damage": 3, "kind": ["fire"], "speed": 768}
 }}
```

Each attack is a named record with units (damage in hp, speed in 8.8 px/frame, lifetime in
frames), keys named after the ability's own moves. `kind` is the attack-kind bitset the
element-modifier table of enemies (enemies.md stage 1) is indexed by, so "fire beats ice" is
data on both sides.

## Migration path

1. **Damage and kinds** (`partial` in the inventory): each attack object's constructor sets
   `unk63`/`unk68` from literals; replace them with reads from `gAbilityParams[ability].attack[i]`
   whose vanilla values are those literals. Mechanical, one ability at a time, gated by the
   round trip plus `regress-pack` (and layout-neutrality, since the game binary changes). This
   is the highest-value step: balance mods.
2. **Speeds and lifetimes** (`no`: spread through control flow): extract the constants a move
   uses into its parameter record where they are plain literals; leave the rest.
3. **New abilities**: out of scope for data alone (MODDING.md §4.6: the ability code is the
   largest state machine in the game). The plan is MODDING.md's: `on_ability_gain` /
   `ability_swallow` hooks can veto or replace with a *vanilla* ability, and per-ability
   parameter hooks; a fully custom ability is Lua over a vanilla ability's slot.

## For AI generation

Today: choose which ability enemies and statues give (object files, room placements). After
stage 1: rebalance attacks by editing numbers with units and ranges in the schema.
