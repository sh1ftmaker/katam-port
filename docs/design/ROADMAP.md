# Roadmap

The port no longer aims at GBA fidelity. The game source lives in `game/` and
is edited directly (docs/FORK.md). Everything is checked by the regression
harness (docs/DETERMINISM.md, `make regress`): simulation hashes must not move
unless a change is *meant* to change gameplay, and then the goldens are
re-recorded in the same commit with the reason stated.

Three tracks, detailed in their own documents:

| Track | Document | First milestone |
|---|---|---|
| Renderer: a GPU renderer driven by a world-space render list, replacing the emulated PPU | RENDERER.md (../RENDERER.md) | world-space BG + sprites, switchable against the PPU |
| Online co-op: deterministic rollback, input-only sync, snapshot joins | NETPLAY-V2.md | **Shelved 2026-11-08, hidden behind `?online=1`**: room mode desyncs and rubberbands. Hand-off and next steps: ../NETPLAY-HANDOFF.md |
| Modding + level editor: resource layer, vanilla pack, Lua, web editor | MODDING.md | M0 resource seam R0 |

## Settled between the two design documents

- **Save memory is at `0x09000000`**, not `0x0E000000`: the sync relocated it
  (`tools/legacy-sync/portify.py`, `SRAM_RELOC`), and `game/` carries the
  relocated address. ARCHITECTURE.md's map table is stale on this point.
  MODDING.md's resource arena at `0x09100000` does not overlap it.
- **The port simulation block is at `0x02040000`** (NETPLAY-V2 M1): mod store,
  port-owned sim state (rando parity, warp state), and later the grown room
  pool. MODDING.md's XRAM is this block.
- **Content hash:** NETPLAY-V2 §11's HELLO shape; MODDING.md's `levelSetHash`
  is the hash of resolved simulation content. Presentation content is hashed
  separately and never enforced.
- **Sim Lua is stateless between calls**, with persistent state in task
  structs or the mod store, so snapshot, hash and transfer stay a memcpy.

Still open (MODDING.md §9): the sim block's size budget, the content-build
step in the join sequence, editor hot-reload in a netplay session (proposed:
refused), and reporting a script out-of-memory error to all peers.

## Known determinism debt (docs/DETERMINISM.md)

- 42 lines call `Rand16()` twice in one unsequenced expression; GCC and clang
  order them differently. `make regress-wasm` is red on warp_sweep (frame
  3132) until this is fixed.
- `ArcTan2` uses libm `atan2`; glibc and emscripten agree today, other libms
  are not guaranteed to.
- lp64 crashes: Mega Titan arm setup (room 738), attract demo frame 6744.
  wasm-only trap in `sub_080788AC` -> `sub_08155128` in long sweeps.
