# Sunset Route -- a demonstration mod

The smallest useful mod for the resource layer (docs/design/MODDING.md): it
repaints the foreground palette of room 803, the room a new game starts in,
in sunset colours.  It contains only its own content -- 91 colour values in
`patches/palette.json`, each a structured edit (`"op": "set"`) on the vanilla
pack's `gfx/palettes/fg/29.json` -- and no byte of the game.

    make res-extract ROM=/path/to/your.gba          # the vanilla pack, into build/res/vanilla
    build/lp64/katam --res-pack build/res/vanilla \
                     --mod examples/mods/sunset_route your.gba

New game -> room 803: clouds, the pillar and the stars are plum and orange;
the HUD, the background and the sprites are untouched (they are not
`fg_palette` resources).

It changes the simulation hash, as it must today.  The palette is a
presentation-realm resource, but the game copies it into `gSourcePalettes` in
EWRAM (`LoadLevelPaletteEffectSourcePalettes`), which the `sim` hash covers;
replaying `boot_to_level` with the mod diverges from the goldens at frame 406,
the frame room 803 is entered.  Palette mods become per-client-safe once
NETPLAY-V2 masks palette memory out of the hash (MODDING.md §1.2, note 2).
The resolved-content hash the loader logs (`levelSetHash`) does not move,
because it covers only the sim-realm kinds.
