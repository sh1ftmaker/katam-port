# Authoring examples

Hand-written content in the authoring format (docs/design/AUTHORING.md). Original, not derived
from the ROM, CC0.

- `sky_garden/` — two rooms on top of the vanilla game: a meadow and a tower, linked by doors
  both ways, the tower's top door leaving to the hub (room 803).

```sh
node tools/katam/katam.mjs validate examples/authoring/sky_garden                       # no ROM needed
node tools/katam/katam.mjs validate examples/authoring/sky_garden --base VANILLA       # + references into the game
node tools/katam/katam.mjs render room:sky_garden/meadow --project examples/authoring/sky_garden \
    --base VANILLA -o /tmp/meadow.png --overlay doors,objects --scale 2
node tools/katam/katam.mjs compile examples/authoring/sky_garden --base VANILLA -o /tmp/sky_out
```

`VANILLA` is the folder `katam decompile` wrote from your ROM (docs/design/authoring/AI-GUIDE.md §0).
The rooms are ids 983 (meadow) and 984 (tower). Play them:

```sh
node tools/katam/katam.mjs playtest examples/authoring/sky_garden --rom baserom.gba -o /tmp/pt
build/lp64/katam --rooms /tmp/pt/build/mod/rooms.katn --warp 1300:983:28:10 ...   # by hand
```

or on the web page `/next/playtest/` (pick "Example: Sky Garden"). `tests/playtest.sh` plays
them on every build (native and wasm, every frame equal).
