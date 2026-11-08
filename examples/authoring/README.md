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
The rooms compile; playing them needs the runtime's room-bundle loader (AUTHORING.md §8).
