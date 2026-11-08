# Bosses

Status: **designed, not implemented** beyond what object types give (boss descriptors are
`obj_desc` entries, authorable as `object:<boss>.object.json`; a boss placement is a room
object).

## What exists

Bosses are object types (`kracko`, `king_golem`, `moley`, `mega_titan`, `gobbler`, `wiz`,
`dark_meta_knight`, `master_hand`, `crazy_hand_*`, `dark_mind_*`, plus mid-bosses), each a
hand-written state machine in `game/src/<boss>.c`. From the semantics tuning inventory:

| item | kind | data candidate |
|---|---|---|
| `boss.hp` — starting hp by number of human players (1-4) | table | yes |
| `boss.hp.dark_mind` — hp per encounter and player count | table | yes |
| `boss.music` — song when the fight starts | table | yes |
| `boss.hud_scale` — hp-bar scale | derived | yes (derive from hp) |
| `boss.phase_threshold` — phase changes at 1/2 (some 1/4, 3/4) hp | derived | partial |
| `boss.attack_damage` | literal | partial |
| `boss.attack_timers` — attack odds, wind-up/cooldown timers, speeds | literal | no |
| `boss.defeat_reward` — mirror shard drop, positions | literal | no |

## The authoring file (planned): `bosses/<name>.boss.json`

A boss is an object type plus a fight:

```json
{"format": "katam-boss/1", "id": "boss:king_golem", "object": "object:king_golem",
 "music": "mus_boss",
 "hp": {"players1": 60, "players2": 80, "players3": 100, "players4": 120},
 "phases": [{"at": 0.5, "name": "enraged"}],
 "attacks": {
   "rock_drop": {"weight": 3, "windup": 30, "cooldown": 60, "count": 3, "damage": 2},
   "gordo_spawn": {"weight": 1, "windup": 45, "cooldown": 90}
 },
 "reward": {"drop": "object:mirror_shard"}}
```

(Illustrative numbers.) `attacks` are named after the boss's own moves; `weight`, timers in
frames and counts are the knobs a designer — or a model — turns.

## Migration path

1. **Tables** (`yes`): hp by player count (`gUnk_08351530`, s16[27][4], types 0x38-0x52 —
   `obj_desc.unk4` is ignored for these, so the object file's `hp` stat is not a boss's hp),
   Dark Mind's per-encounter hp, music → manifest kinds and the `hp` / `music` sections. The
   HUD bar scale `gUnk_08351458` (u16[27][4]) is **derived, never authored**: every vanilla
   entry is `floor(0x4000 / hp)` for the same cell (semantics registry, `boss.hud_scale`), so the
   compiler writes both tables from `hp` — an edited hp with a stale scale draws a wrong bar.
   The round trip will prove the derivation on all 108 cells. Pure data, gated by the round trip +
   `regress-pack`.
2. **Attack parameters** (`partial` / `no`): per boss, lift the literals of its attack selection
   (odds, timers, counts, damage) into `gBossParams[boss]`, vanilla values = the old literals.
   One boss at a time, highest value first (the bosses players fight most: King Golem, Kracko,
   Moley). Gates: round trip, `regress-pack`, layout-neutrality.
3. **New fights**: MODDING.md §4 — a boss as a Lua state machine (`sim.object_type{category =
   "boss", states = {…}}`) whose numbers come from the same `bosses/<name>.boss.json` attack
   records, read through `o.params`/`o.attacks`. The data file is the AI-facing part (tune or
   compose attacks); the Lua is the behaviour (written by people, or generated against the API
   with the data file as its contract).

The file format is the same for a vanilla boss after stage 2 and a Lua boss after stage 3, so
the editor and the validator treat them alike.

## For AI generation

Today: place bosses in rooms (with the room sized for the arena, doors sealed — the
`boss_challenge_door`, `scroll_lock` and cutscene-trigger object types carry the arena
mechanics). After stage 1-2: tune fights with named numbers.
