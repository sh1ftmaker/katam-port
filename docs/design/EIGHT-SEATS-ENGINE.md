# Eight seats: the engine's side (stage 1)

Branch `eight-engine`. Stage 1 of the eight-humans plan: the human-slot layer
(`platform/localmp.c`, `port/localmp.h`) and the session engine
(`platform/lmpnet.c`, `port/lmpnet.h`) are N wide in the wide build. This note
is the contract that stage 2 (wire, hub, rig) builds on. The default build
(4 Kirbys) is unchanged: every name below equals the old constant, and the
old 4-wide functions behave as before.

## Widths

| name | default | wide build (`KATAM_MAX_KIRBYS=N`) |
|---|---|---|
| `PORT_LMP_SLOTS` (`port/wide.h`) | 4 | N |
| `PORT_LN_SLOTS` = Kirby slots a session seals | 4 | N |
| `PORT_LN_MAX_DEV` = devices (host + guests) | 4 | N |
| seats per device | 4 | 4 |
| `PORT_LN_OWN(dev, seat)` = `1 + dev*4 + seat` | ≤ 16 | ≤ 64 (a u8) |

At run time: `PortLnSlots()` and `PortLnMaxDevices()`.

**Seats the host may give: `PortLnSetSeatSlots(n)`** (host, 1..N). This is
4 until the caller raises it, and it is kept across sessions. A seat is only
granted in slots below it, and `PortLnFreeSlots` counts within it. Today's
page and rig seal k0..k3 only (`PortLnSealedGet` / `PortLnSealed4`), so a w8
module driven by them must never seat a person in slot 4..: that person's
keys would not travel, and the session diverges. The rig showed exactly that
before this limit existed (R13 churn, slot 4 given to device 3). Stage 2
calls `PortLnSetSeatSlots(PortLnSlots())` once its wire carries every slot.

**Devices.** The host is device 0, and guests are 1 .. `PORT_LN_MAX_DEV`−1.
On a w8 module, devices 4..7 are numbered and handled exactly like 1..3:
`PortLnRemoteInputSeat(f, dev, seat, keys)`, `PortLnDeviceGone(dev)`,
`PortLnSeatRequest(dev, seat, on)` and `PortLnDeviceSettings(dev, …)` all take
`dev` up to `PORT_LN_MAX_DEV`−1. The host's per-frame seat mask is bit
`(dev−1)*4 + seat`, a u64 in the wide build.

## The sealed record

**Host, read.** `PortLnSealedGetN(f, out)` fills `PORT_LN_SEALED_WORDS`
(= 10 + N − 4) u32 words. It returns 0 if frame f is not sealed.

| word | meaning |
|---|---|
| 0, 1 | keys of slots 0, 1 |
| 2 | number of events sealed with f (read them with `PortLnSealedEventGet`) |
| 3 | device 1 seat 0's key was on time |
| 4, 5, 6 | hash lo, hash hi, hashed (1/0) |
| 7, 8 | keys of slots 2, 3 |
| 9 | N (`PORT_LN_SLOTS`) |
| 10 + i | keys of slot 4 + i, for i < N − 4 |

Words 0..8 are exactly `PortLnSealedGet`'s, which is unchanged. On a w8
module, w8 needs 14 words and w16 needs 22; a 512-byte scratch buffer holds
either.

**Guest, write.** `PortLnSealedN(f, keys, n, nev)` takes `keys` as a pointer
to n u16 values in slot order (from JS: `_malloc(2*N)`, write `HEAPU16`).
Slots from n on are 0. `PortLnSealed4(f, k0..k3, nev)` is `n = 4`. As before,
a frame's events (`PortLnSealedEvent`) arrive before its record.

A slot nobody plays seals 0, so a wire that sends "slot keys follow" bits only
for non-idle pads stays sparse. The analysis's plan (flag bit 6: a mask byte
for slots 4..7 follows) is the wire's choice; the engine does not care how
the keys travel.

## Events

`PORT_LN_EV_KIRBYS = 10`, a = the Kirbys in all (4..N). This is the host's
CPU KIRBYS setting (`platform/wide.c` `PortWideSetTarget`), and the default
build ignores it. On the host, `PortWideRequestKirbys(n)` during a session
becomes `PortLnEvent(PORT_LN_EV_KIRBYS, n, …)`; on a guest it is refused. It
travels like any other sealed event: no new wire record, only a new type
value.

**Seats past the live count** need no event of their own. The count in play
is max(4, setting, highest person slot + 1), computed from simulation state
(`PortLmpPeopleEnd`, which counts held joins too). `PortLnSeatRequest`
prefers a free slot whose Kirby exists, and otherwise takes the next slot. It
then seals the ordinary `PORT_LN_EV_HUMAN`. Every device then holds the join
(`lmpw.waiting`; `PortLmpSlotPerson` is already true), the count rises one
Kirby a frame while play allows, and the join is applied once the Kirby
exists. The count shrinks only from the end, and never past a person's Kirby.
I chose this over the analysis's "raise by event, then grant" for two
reasons: a raise event would leave the setting raised after the person left,
against the owner's rule count = max(4, people, setting); and a guest that
rewinds over the join replays it with nothing extra to seal.

## Join and repair images

The header is unchanged (64 bytes, same offsets; bytes 52..55 hold slots
0..3). The wide build writes version 3 and appends every slot's owner (N
bytes) after the host extras. `PortLnImageBytes()` includes it.

## Telemetry (wide build only)

The wide build adds `own4..own7` (the owners of slots 4..7; slots 8.. are not
listed) and `dHiKeyOnTime` / `dHiKeyMissing` (guest devices 4 and up)
at the end of the block. Everything else keeps its name and position.

## Still four wide, on purpose

- Seats per device (4): `lk`, `rk[dev][4]`, `PortLnSeatAsk`, `sLastLocal`.
- `PortLmpSlotIsHuman` / `PortLmpRequestHuman` in a session speak of a
  device's local seats 1..3, as before.

Stage 5 (branch `eight-hud`) widened the pause page's session copies and
names: `PORT_LN_EV_PSET` and `_NAME` are sealed for every slot
(`PORT_LN_SLOTS`), `menu_poc.c` keeps `POC_SLOTS` = `PORT_LN_SLOTS` copies
(its host state, `PocHostState`, is that wide: same build only), and
`player_card.c` names `PORT_LN_SLOTS` slots (`PortCardSlotNames` lists that
many, tab-separated).  Paint (`PORT_LN_EV_PAINT`, `kirby_paint.c`) covers
every slot (`PORT_PAINT_SLOTS`): slots 0..3 in the default layout's
`game.paint`/`paintFeet`/`paintDirty`, slots 4.. in `gPortSim->paintw`
(`struct PortWidePaint`, port/wide.h, in the padding before the wide area:
snapshotted, hashed, in images), with `PORT_PAINT_DIRTY_WIDE` in
`paintDirty` while any of those reloads is pending.  A Kirby 4..'s sprite
`palId` maps back to its slot as `(palId >> 4) * 4 + (palId & 3)`.  Kirbys
4.. have colours of their own (`PortWideDefaultColor`: the sprays 1..4 do
not wear, Snow, Carbon, Ocean, Grape first), and while more than four are in
the world the GPU renderer draws whatever GRAPHICS says (`PortWideNeedsGpu`,
platform/ui/present.c).

## Exports

`_PortLnSealedN`, `_PortLnSealedGetN`, `_PortLnSlots`, `_PortLnMaxDevices`,
`_PortLnSetSeatSlots` and `_PortLnSeatSlots` are in `PORT_EXPORTS`
(Makefile), in both modules.

## Checked (2026-11-15; scratch and logs in `netproof-eval/eight-tmp/engine/`)

- **The engine at 8 devices.** `n8.mjs` drives the API above directly, with
  no wire: a host and guests 1..7 on the w8 module, joined by image. Guests
  run 4 frames ahead, records arrive 1..3 frames late, and some pads arrive
  after the host has sealed them. Over 6,000 frames: CPU KIRBYS 4 sealed, 7
  seats (4 of them past the live count, each made and then joined), devices
  5..7 gone (the count shrinks to 5), device 7 back in its own slot 7 (the
  count rises again), CPU KIRBYS 6. All 7 guests verified 5,999 frames each
  with 0 mismatches, and the final hashes agree. On the default module the
  same harness passes with 3 guests.
- **8 local humans** (`h8.txt`, `h8rooms.txt`, `option localmp 8` with
  `human` churn and `kirbys 4`) on native w8:
  - ASan + UBSan: 0 NEW (`tools/wide_sanclass.py`);
  - the same input twice gives identical hash logs;
  - native == wasm in keys/vid/sram/pix on every frame.
- **Default module.** `tests/layout_neutral.sh` against the live binary,
  `make regress` (same result as the reference), and the rig's quick pass:
  16 gates, none FAIL.
- **The w8 module under today's 4-wide rig** (scratch copy reading the Kirby
  address from the environment): 16 gates, none FAIL.
