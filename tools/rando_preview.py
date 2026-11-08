#!/usr/bin/env python3
"""
rando_preview.py -- what the randomizer does to a whole ROM, without playing it.

platform/rando.c rewrites one byte per level-object record as a room is
loaded.  Two things about that are worth knowing before a player finds them
out: how much of the game actually changes, and whether any room ends up
asking for more video memory than the room it replaced.  Neither question can
be answered by playing, because the answer lives in 287 object lists and a
run touches a handful of them.

So this is a second implementation of the same roll, in Python, over the
player's own ROM.

**The point of a second implementation is that it can disagree**, and a
disagreement is meaningless unless the two are pinned together first, so
`--check` reproduces a run of the real thing:

    RANDO=0x11:1 HOLD=0x10 MASH=600:1:6 node tools/headless_test.js ... 1300

took room 803 from `00 . . 0f 32 00` at indices 7, 9, 10, 11 to
`0b . . 32 10 23`, and `--check` asserts exactly that.  If the C changes and
this does not, the assertion fails and the numbers below stop being claimed.

No data from the ROM is written anywhere; this only prints.
"""

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

ROM_BASE = 0x08000000

# ROM addresses, from katam.map by way of the decompilation's data labels.
# They are constants of the retail cartridge, like every other address the
# port resolves; gen_rom_data.py is where the same numbers come from for the
# build itself.
G_LEVEL_OBJ_LISTS = 0x08D637AC   # 287 pointers to struct LevelInfo_1E4
G_ROOM_PROPS      = 0x089331AC   # struct RoomProps[], 0x28 bytes each
G_OBJ_DESC        = 0x08351648   # struct Unk_08351648[], 0x18 bytes each

ROOM_PROPS_SIZE   = 0x28
ROOM_PROPS_OBJLIST = 0x20        # RoomProps.objectListIdx
NUM_ROOM_PROPS    = 983          # (0x0893CB44 - 0x089331AC) / 40
OBJ_DESC_SIZE     = 0x18
OBJ_DESC_ABILITY  = 0x06         # Unk_08351648.kirbyAbility
OBJ_DESC_TILES    = 0x0C         # Unk_08351648.numTiles
OBJECT_TYPE_OFF   = 0x0C         # struct Object.type

# --- the pools, in the order platform/rando.c lists them ------------------
# The order matters: the roll picks the Nth *candidate*, so a pool in a
# different order is a different game for the same seed.
ENEMY_POOL = [
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2D, 0x2F, 0x32, 0x33, 0x34, 0x35,
    0x37,
]
MIDBOSS_POOL = [0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F]
BOSS_POOL    = [0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B]

RANDO_ENEMIES      = 0x01
RANDO_MIDBOSSES    = 0x02
RANDO_BOSSES       = 0x04
RANDO_ANY_SIZE     = 0x08
RANDO_KEEP_ABILITY = 0x10

U32 = 0xFFFFFFFF


class Rom:
    def __init__(self, path):
        self.data = Path(path).read_bytes()

    def u8(self, a):
        return self.data[a - ROM_BASE]

    def u16(self, a):
        return struct.unpack_from('<H', self.data, a - ROM_BASE)[0]

    def u32(self, a):
        return struct.unpack_from('<I', self.data, a - ROM_BASE)[0]

    def tiles(self, t):
        return self.u32(G_OBJ_DESC + t * OBJ_DESC_SIZE + OBJ_DESC_TILES)

    def ability(self, t):
        return self.u16(G_OBJ_DESC + t * OBJ_DESC_SIZE + OBJ_DESC_ABILITY)


def mix(x):
    """The 32-bit finalizer in platform/rando.c, in 32-bit arithmetic."""
    x &= U32
    x ^= x >> 16
    x = (x * 0x7FEB352D) & U32
    x ^= x >> 15
    x = (x * 0x846CA68B) & U32
    x ^= x >> 16
    return x


def size_class(rom, t):
    tiles = rom.tiles(t)
    if tiles == 0xFFFFFFFF:
        return 0
    if tiles <= 12:
        return 0
    if tiles <= 25:
        return 1
    if tiles <= 36:
        return 2
    return 3


def pool_of(t):
    if t in ENEMY_POOL:
        return ENEMY_POOL, RANDO_ENEMIES
    if t in MIDBOSS_POOL:
        return MIDBOSS_POOL, RANDO_MIDBOSSES
    if t in BOSS_POOL:
        return BOSS_POOL, RANDO_BOSSES
    return None, 0


def roll(rom, t, pool, room, index, flags, seed):
    want_class = size_class(rom, t)
    want_ability = rom.ability(t) != 0

    def fits(c):
        if not (flags & RANDO_ANY_SIZE) and size_class(rom, c) != want_class:
            return False
        if (flags & RANDO_KEEP_ABILITY) \
                and (rom.ability(c) != 0) != want_ability:
            return False
        return True

    cands = [c for c in pool if fits(c)]
    if not cands:
        return t
    r = mix(seed ^ mix(((room * 0x9E3779B9) & U32) + index))
    return cands[r % len(cands)]


def walk_list(rom, addr):
    """(index, record address, type) for every record in one object list."""
    out, i = [], 0
    while True:
        stride = rom.u8(addr + 1)
        if stride == 0:
            return out
        out.append((i, addr, rom.u8(addr + OBJECT_TYPE_OFF)))
        addr += stride
        i += 1


def rooms(rom):
    """{room id: object list address}.

    Rooms are what the randomizer is keyed on and lists are what it rewrites.
    An earlier version of this comment said several rooms share a list; that
    was wrong, and tools/room_export.py measured it: the 286 real rooms use
    286 distinct object lists, so the mapping is one to one (list 4 is the only
    one nothing references).  Keying on the room id is still right -- it is the
    stable identifier the game hands the loader -- but it is right because it
    is the natural key, not because a list needed disambiguating."""
    out = {}
    # The table is 983 entries: gRoomProps at 0x089331AC to the next data
    # label at 0x0893CB44, over sizeof(struct RoomProps) == 40 (frozen in
    # platform/port/gba_layout.h).  Its first several hundred entries are
    # 0xFFFF filler -- a room id is not an index into a packed list -- so
    # unused entries are skipped rather than treated as the end.
    for n in range(NUM_ROOM_PROPS):
        idx = rom.u16(G_ROOM_PROPS + n * ROOM_PROPS_SIZE + ROOM_PROPS_OBJLIST)
        if idx >= 287:
            continue
        ptr = rom.u32(G_LEVEL_OBJ_LISTS + idx * 4)
        if not (ROM_BASE <= ptr < ROM_BASE + len(rom.data)):
            continue
        objs = rom.u32(ptr)
        if not (ROM_BASE <= objs < ROM_BASE + len(rom.data)):
            continue
        out[n] = objs
    return out


def names():
    hdr = (Path(__file__).parent.parent
           / 'game' / 'include' / 'constants' / 'object_types.h')
    out = {}
    if hdr.exists():
        for line in hdr.read_text().splitlines():
            if line.startswith('#define OBJ_'):
                p = line.split()
                if len(p) >= 3 and p[2].startswith('0x'):
                    out[int(p[2], 16)] = p[1][4:]
    return out


def check(rom, flags, seed):
    """Pin this implementation to the C one.  See the module docstring."""
    expected = {7: 0x0B, 9: 0x32, 10: 0x10, 11: 0x23}
    room = 803
    addr = rooms(rom)[room]
    got = {}
    for i, _, t in walk_list(rom, addr):
        pool, bit = pool_of(t)
        if pool and (flags & bit):
            r = roll(rom, t, pool, room, i, flags, seed)
            if r != t:
                got[i] = r
    if got != expected:
        print('MISMATCH against the recorded run of platform/rando.c')
        print('  expected %s' % {k: hex(v) for k, v in sorted(expected.items())})
        print('  got      %s' % {k: hex(v) for k, v in sorted(got.items())})
        return 1
    print('check: room 803 at flags=0x%x seed=%d matches platform/rando.c '
          '(%d swaps)' % (flags, seed, len(got)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--flags', default='0x11')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--check', action='store_true',
                    help='verify against the recorded run of the C, and stop')
    ap.add_argument('--room', type=int, help='print one room in full')
    args = ap.parse_args()

    rom = Rom(args.rom)
    flags = int(args.flags, 0)
    nm = names()

    if args.check:
        return check(rom, flags, args.seed)

    roomtab = rooms(rom)

    if args.room is not None:
        addr = roomtab[args.room]
        print('room %d, object list at 0x%08X' % (args.room, addr))
        for i, ra, t in walk_list(rom, addr):
            pool, bit = pool_of(t)
            r = roll(rom, t, pool, args.room, i, flags, args.seed) \
                if pool and (flags & bit) else t
            mark = '' if r == t else '  <-- %s' % nm.get(r, '%02X' % r)
            print('  %2d  %-28s %s' % (i, nm.get(t, '%02X' % t), mark))
        return 0

    changed = same = untouched = 0
    became = Counter()
    worse_rooms = []
    for room, addr in sorted(roomtab.items()):
        stock = new = 0
        for i, ra, t in walk_list(rom, addr):
            pool, bit = pool_of(t)
            if not pool or not (flags & bit):
                untouched += 1
                continue
            r = roll(rom, t, pool, room, i, flags, args.seed)
            if r == t:
                same += 1
            else:
                changed += 1
                became[r] += 1
            st = rom.tiles(t)
            rt = rom.tiles(r)
            stock += 0 if st == 0xFFFFFFFF else st
            new += 0 if rt == 0xFFFFFFFF else rt
        if new > stock:
            worse_rooms.append((new - stock, room, stock, new))

    print('flags=0x%x seed=%d over %d rooms' % (flags, args.seed, len(roomtab)))
    print('  %5d placements replaced' % changed)
    print('  %5d rolled back to themselves' % same)
    print('  %5d outside every pool, untouched' % untouched)
    print()
    print('  tile demand per room, replaced set only:')
    print('    %d room(s) ask for more than they shipped with' % len(worse_rooms))
    for extra, room, stock, new in sorted(worse_rooms, reverse=True)[:8]:
        print('      room %-4d %4d -> %4d tiles (+%d)' % (room, stock, new, extra))
    print()
    print('  most common replacements:')
    for t, n in became.most_common(8):
        print('    %-28s %d' % (nm.get(t, '%02X' % t), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
