#!/usr/bin/env python3
"""
room_export.py -- read the game's rooms out of a ROM.

A KATAM room is a 40-byte `struct RoomProps` record and nine indices into nine
other tables.  Every one of those tables is at a known address and every field
of the record has a name, so a room can be resolved statically -- no emulator,
no playing, no guessing.  This does that resolution and prints it as JSON.

The resolution chain, established by reading the game's own room loader
(`FillLevelInfo` in the decompilation's wip/code.c) rather than by inference:

    gRoomProps[roomId]                          40 bytes, 983 entries
      .mapDataIdx      -> gForegroundTilemaps[] -> struct RoomTiledBG  (0x20)
                          THIS LAYER DEFINES THE ROOM'S SIZE:
                          roomWidth  = width  << 3   (px)
                          roomHeight = height << 3   (px)
      .backgroundIdx   -> gRoomTiledBGs[]       -> struct RoomTiledBG
      .objectList2Idx  -> gLevelObjLists[]      -> struct RoomTiledBG  (!)
      .objectListIdx   -> gLevelObjLists[]      -> object records      (!)
      .solidityMapIdx  -> gSolidityMaps[]       -> RL-compressed collision
      .doorsIdx        -> gUnk_08D640A4[]       -> door table
      .pixelDataIdx    -> gForegroundInfo[]
      .paletteDataIdx  -> gForegroundPalettes[]
      .songIdx            music

The two lines marked (!) are the same table read two different ways.
`gLevelObjLists` is declared as a transparent union of `RoomTiledBG *` and
`LevelInfo_1E4 *` (include/data.h:853): `objectListIdx` selects the object
reading, `objectList2Idx` selects the tilemap reading.  Anything that assumes
one meaning for that table will silently produce nonsense for half its uses.

Compression, both standard BIOS formats:
  - the collision map is RLUnCompWram   (wip/code.c:431)
  - the tilemaps are LZ77UnCompWram     (wip/code.c:433)

**Collision is the useful part.**  One u8 per 16x16 pixel cell, row-major,
stride `roomWidth / 16`, decompressed into a 1950-byte buffer.  Each byte is
an index into the metatile attribute table `gUnk_082D88B8` (0x082D88B8), whose
u32 entries are the terrain bitfields the physics reads.

No data is written back to the ROM and nothing here needs the ROM to be
writable; this only reads and prints.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

ROM_BASE = 0x08000000

# ROM addresses, taken from katam.map by way of the decompilation's data
# labels -- the same source gen_rom_data.py uses for the build itself.
G_ROOM_PROPS          = 0x089331AC   # struct RoomProps[983]
G_LEVEL_OBJ_LISTS     = 0x08D637AC   # union { RoomTiledBG*; LevelInfo_1E4*; }[287]
G_SOLIDITY_MAPS       = 0x08D63330   # struct SolidityMap *const []
G_FOREGROUND_TILEMAPS = 0x08D64520   # struct RoomTiledBG *const []
G_ROOM_TILED_BGS      = 0x08D64A24   # struct RoomTiledBG *const []
G_FOREGROUND_INFO     = 0x08D6499C   # struct ForegroundInfo *const []
G_FOREGROUND_PALETTES = 0x08D63288   # struct ForegroundPalette *const []
G_DOORS               = 0x08D640A4   # struct Unk_08930E5C *const []
G_METATILE_ATTRS      = 0x082D88B8   # const u32 [] -- terrain bitfields

NUM_ROOMS      = 983        # (0x0893CB44 - 0x089331AC) / 40
NUM_OBJ_LISTS  = 287
ROOM_PROPS_SIZE = 0x28
OBJECT_SIZE     = 0x24      # struct Object
COLLISION_CAP   = 1950      # sizeof gUnk_02024ED0[n] -- a hard cap, see validate()

# struct RoomProps, in order.  Every name here is the decompilation's own; the
# `unk` ones are genuinely still unknown and are reported as-is rather than
# guessed at.
ROOM_PROPS_FIELDS = [
    ('songIdx',        'H'), ('unk02',          'H'),
    ('unk04',          'H'), ('unkXmod_06',     'h'),
    ('unkXmod_08',     'h'), ('unk0A',          'H'),
    ('unk0C',          'H'), ('unk0E',          'h'),
    ('unk10',          'h'), ('priorityFlags',  'H'),
    ('pixelDataIdx',   'H'), ('paletteDataIdx', 'H'),
    ('mapDataIdx',     'H'), ('solidityMapIdx', 'H'),
    ('objectList2Idx', 'H'), ('backgroundIdx',  'H'),
    ('objectListIdx',  'H'), ('unk22',          'H'),
    ('doorsIdx',       'H'),
]

# struct Object, 36 bytes.  Byte 12 (`type`) is the one the randomizer rewrites.
OBJECT_FIELDS = [
    ('spawnTable', 'B'), ('unk1', 'B'), ('unk2', 'B'), ('unk3', 'B'),
    ('unk4', 'B'), ('unk5', 'B'), ('x', 'h'), ('y', 'h'),
    ('unkA', 'B'), ('unkB', 'B'), ('type', 'B'), ('unkD', 'B'),
    ('subtype1', 'B'), ('unkF', 'B'), ('subtype2', 'B'), ('unk11', 'B'),
    ('unk12', 'H'), ('unk14', 'H'), ('unk16', 'H'), ('unk18', 'H'),
    ('unk1A', 'h'), ('unk1C', 'h'), ('unk1E', 'H'), ('unk20', 'H'),
    ('unk22', 'H'),
]


class BadPointer(Exception):
    """A table entry that does not point into the ROM.

    Raised rather than returned because every caller's correct response is the
    same -- record the room as unresolvable and move on -- and a sentinel
    return would get silently formatted into the JSON as though it were data.
    """


class Rom:
    def __init__(self, path):
        self.data = Path(path).read_bytes()

    def _off(self, addr):
        off = addr - ROM_BASE
        if not 0 <= off < len(self.data):
            raise BadPointer('0x%08X is outside the ROM' % addr)
        return off

    def u8(self, a):
        return self.data[self._off(a)]

    def u16(self, a):
        return struct.unpack_from('<H', self.data, self._off(a))[0]

    def u32(self, a):
        return struct.unpack_from('<I', self.data, self._off(a))[0]

    def ptr(self, a):
        """Read a pointer and check it lands in the ROM before returning it."""
        p = self.u32(a)
        if not ROM_BASE <= p < ROM_BASE + len(self.data):
            raise BadPointer('0x%08X does not point into the ROM' % p)
        return p

    def unpack(self, addr, fields):
        out, a = {}, addr
        for name, fmt in fields:
            out[name] = struct.unpack_from('<' + fmt, self.data, self._off(a))[0]
            a += struct.calcsize(fmt)
        return out


# --- the two BIOS decompressors -------------------------------------------
# Both are the documented GBA formats.  The header is one u32: the low nibble
# is the type and the top 24 bits are the decompressed size.  Trusting that
# size is what makes the validation in validate() meaningful -- a room whose
# collision map decompresses to exactly (width/2 * height/2) bytes is a room
# whose geometry we have read correctly.

def decompress(rom, addr):
    """-> (bytes, declared_size, type_nibble).  Raises on a malformed stream.

    The type is the HIGH nibble of byte 0 -- 0x10 is LZ77 and 0x30 is
    run-length.  The low nibble is reserved and is 0 in every stream in this
    ROM.  Reading the low nibble instead gets you type 0 for everything, which
    looks exactly like a wrong table address."""
    header = rom.u32(addr)
    kind, size = (header >> 4) & 0xF, header >> 8
    off = rom._off(addr) + 4
    data = rom.data
    out = bytearray()

    if kind == 1:                                    # LZ77
        while len(out) < size:
            flags = data[off]; off += 1
            for bit in range(8):
                if len(out) >= size:
                    break
                if flags & (0x80 >> bit):
                    b0, b1 = data[off], data[off + 1]; off += 2
                    length = (b0 >> 4) + 3
                    disp = (((b0 & 0xF) << 8) | b1) + 1
                    if disp > len(out):
                        raise BadPointer('LZ77 back-reference before the start')
                    for _ in range(length):
                        out.append(out[len(out) - disp])
                else:
                    out.append(data[off]); off += 1

    elif kind == 3:                                  # run-length
        while len(out) < size:
            flag = data[off]; off += 1
            if flag & 0x80:
                out.extend(bytes([data[off]]) * ((flag & 0x7F) + 3)); off += 1
            else:
                n = (flag & 0x7F) + 1
                out.extend(data[off:off + n]); off += n

    else:
        raise BadPointer('compression type %d is not LZ77 or RLE' % kind)

    return bytes(out[:size]), size, kind


def tiled_bg(rom, addr):
    """struct RoomTiledBG, 0x20 bytes.  width/height are in 8px tiles."""
    return {
        'addr':          '0x%08X' % addr,
        'width':         rom.u16(addr + 0x00),
        'height':        rom.u16(addr + 0x02),
        'unk4':          rom.u16(addr + 0x04),
        'unk6':          rom.u8(addr + 0x06),
        'unk7':          rom.u8(addr + 0x07),
        'tileset':       '0x%08X' % rom.u32(addr + 0x08),
        'tilesetSize':   rom.u32(addr + 0x0C),
        'palette':       '0x%08X' % rom.u32(addr + 0x10),
        'paletteOffset': rom.u16(addr + 0x14),
        'paletteSize':   rom.u16(addr + 0x16),
        'tilemap':       '0x%08X' % rom.u32(addr + 0x18),
        'unk1C':         rom.u32(addr + 0x1C),
    }


def object_list(rom, idx):
    """The room's objects.

    `gLevelObjLists[idx]` read as `LevelInfo_1E4 *`, whose first word points at
    the records.  Termination is a zero stride byte, not a count: the game
    walks with `addr += record[1]`, so a record can in principle be a size
    other than 0x24 even though every one in the retail ROM is 0x24."""
    header = rom.ptr(G_LEVEL_OBJ_LISTS + idx * 4)
    addr = rom.ptr(header)
    out = []
    while True:
        stride = rom.u8(addr + 1)
        if stride == 0:
            return {'headerAddr': '0x%08X' % header,
                    'recordsAddr': '0x%08X' % rom.u32(header),
                    'count': len(out), 'objects': out}
        rec = rom.unpack(addr, OBJECT_FIELDS)
        rec['_addr'] = '0x%08X' % addr
        rec['_stride'] = stride
        out.append(rec)
        addr += stride
        if len(out) > 256:
            raise BadPointer('object list did not terminate within 256 records')


def collision(rom, props, width_cells, height_cells):
    """The room's collision grid: one u8 per 16x16px cell, row-major.

    Returns the grid plus the decompressor's own declared size, because the
    two disagreeing is the single most informative failure this tool can
    report -- it means the geometry came from somewhere other than where we
    think it does."""
    entry = rom.ptr(G_SOLIDITY_MAPS + props['solidityMapIdx'] * 4)
    blob = rom.ptr(entry + 0x00)          # struct SolidityMap.unk0
    cells, declared, kind = decompress(rom, blob)
    expected = width_cells * height_cells
    return {
        'entryAddr':      '0x%08X' % entry,
        'compressedAddr': '0x%08X' % blob,
        'compression':    {1: 'lz77', 3: 'rle'}.get(kind, kind),
        'declaredSize':   declared,
        'expectedSize':   expected,
        'matchesGeometry': declared == expected,
        'widthCells':     width_cells,
        'heightCells':    height_cells,
        'cells':          list(cells),
    }


def resolve(rom, room_id, want_cells=True):
    props = rom.unpack(G_ROOM_PROPS + room_id * ROOM_PROPS_SIZE,
                       ROOM_PROPS_FIELDS)
    out = {'roomId': room_id, 'props': props}

    fg = rom.ptr(G_FOREGROUND_TILEMAPS + props['mapDataIdx'] * 4)
    out['foreground'] = tiled_bg(rom, fg)

    # roomWidth = width << 3 (wip/code.c:256).  Collision cells are 16px, so
    # the cell stride is width/2 -- NOT width, and not roomWidth/8.
    w, h = out['foreground']['width'], out['foreground']['height']
    out['roomWidthPx'], out['roomHeightPx'] = w << 3, h << 3
    wc, hc = (w << 3) // 16, (h << 3) // 16

    out['background'] = tiled_bg(rom, rom.ptr(G_ROOM_TILED_BGS
                                              + props['backgroundIdx'] * 4))

    if props['objectList2Idx'] != 0xFFFF:
        out['extraLayer'] = tiled_bg(
            rom, rom.ptr(G_LEVEL_OBJ_LISTS + props['objectList2Idx'] * 4))
    else:
        out['extraLayer'] = None

    out['objects'] = object_list(rom, props['objectListIdx'])
    out['collision'] = collision(rom, props, wc, hc)
    if not want_cells:
        out['collision'].pop('cells')

    out['doors'] = {'tableAddr': '0x%08X' % rom.ptr(G_DOORS
                                                    + props['doorsIdx'] * 4),
                    'note': 'layout not yet decoded -- see docs/naming/doors.md'}
    return out


def arrival_cells(rom):
    """{room id: [(cellX, cellY), ...]} -- where the game's own doors deliver.

    This exists because warping Kirby to an arbitrary cell **hangs the game**.
    Cell (3,5) of room 804 hangs; cell (2,5) works, and the difference is that
    (2,5) is a cell a real door arrives at.  Guessing a spawn point is
    therefore not a heuristic that is sometimes worse -- it is a hang.

    The door records in the solidity map (tag 3) each carry a destination room
    and a destination cell, so inverting that table gives, for every room, the
    exact set of places the game itself considers a legal arrival.  268 of the
    286 rooms have at least one; see spawn_cell for the other 18."""
    out = {}
    for rid in valid_rooms(rom):
        idx = rom.u16(G_ROOM_PROPS + rid * ROOM_PROPS_SIZE + 0x1A)
        try:
            entry = rom.ptr(G_SOLIDITY_MAPS + idx * 4)
            addr = rom.u32(entry + 4)
            if not ROM_BASE <= addr < ROM_BASE + len(rom.data):
                continue
        except BadPointer:
            continue
        while True:
            tag, stride = rom.u8(addr), rom.u16(addr + 4)
            if stride == 0 or tag == 0:
                break
            if tag == 3:
                out.setdefault(rom.u16(addr + 8), []).append(
                    (rom.u8(addr + 0xA), rom.u8(addr + 0xB)))
            addr += stride
    return out


def spawn_cell(rom, room_id, arrivals=None):
    """A cell it is safe to warp into, and where it came from.

    -> ((cellX, cellY), source) with source 'door' or 'floor' or None."""
    if arrivals is None:
        arrivals = arrival_cells(rom)
    cells = arrivals.get(room_id)
    if cells:
        # Deterministic order, nothing more.  Every cell in this list is one
        # the game itself delivers players to, so there is no reason to prefer
        # one over another, and inventing a reason ("prefer the lowest") only
        # invites a rule that reads backwards -- y grows downward here.
        return min(cells, key=lambda c: (c[1], c[0])), 'door'

    # The 18 rooms nothing arrives at.  Fall back to standing on a floor:
    # an empty cell with a solid cell directly beneath it.  This is a guess and
    # is reported as one -- it is not known to be as safe as a real arrival.
    r = resolve(rom, room_id)
    c = r['collision']
    w, h, cells = c['widthCells'], c['heightCells'], c['cells']
    for y in range(h - 1):
        for x in range(w):
            if cells[y * w + x] == 0 and cells[(y + 1) * w + x] != 0:
                return (x, y), 'floor'
    return None, None


def valid_rooms(rom):
    """Room ids that resolve.

    gRoomProps is not packed: several hundred entries are 0xFFFF filler,
    because a room id is an id and not an index into a list of real rooms.
    Skipping them is correct; stopping at the first one is not."""
    out = []
    for n in range(NUM_ROOMS):
        idx = rom.u16(G_ROOM_PROPS + n * ROOM_PROPS_SIZE + 0x20)
        if idx < NUM_OBJ_LISTS:
            out.append(n)
    return out


def ascii_map(room):
    """The collision grid as text, so it can be eyeballed against the game.

    Metatile 0 is empty in every room in the ROM; that is an observation from
    the sweep below, not an assumption the format guarantees."""
    c = room['collision']
    w, cells = c['widthCells'], c['cells']
    rows = []
    for y in range(c['heightCells']):
        row = cells[y * w:(y + 1) * w]
        rows.append(''.join('.' if v == 0 else
                            ('#' if v < 0x40 else
                             ('%' if v < 0x80 else '@')) for v in row))
    return rows


def validate(rom):
    """Sweep every room and report where the model breaks.

    The point is the failures.  A tool that resolves 900 rooms and says nothing
    about the other 83 has not been checked, it has been run."""
    ok = bad = 0
    mismatches, errors = [], []
    widest = tallest = biggest = 0
    over_cap = []
    metatiles = set()

    for rid in valid_rooms(rom):
        try:
            r = resolve(rom, rid)
        except (BadPointer, struct.error, IndexError) as e:
            bad += 1
            errors.append((rid, str(e)))
            continue
        c = r['collision']
        if not c['matchesGeometry']:
            mismatches.append((rid, c['expectedSize'], c['declaredSize']))
        else:
            ok += 1
        widest = max(widest, c['widthCells'])
        tallest = max(tallest, c['heightCells'])
        biggest = max(biggest, c['declaredSize'])
        if c['declaredSize'] > COLLISION_CAP:
            over_cap.append((rid, c['declaredSize']))
        metatiles.update(c['cells'])

    print('rooms with a valid object list index: %d' % len(valid_rooms(rom)))
    print('  %4d resolved, collision size == width/16 * height/16' % ok)
    print('  %4d resolved, but the sizes disagree' % len(mismatches))
    print('  %4d failed to resolve at all' % bad)
    print()
    print('geometry:')
    print('  widest room      %d cells (%d px)' % (widest, widest * 16))
    print('  tallest room     %d cells (%d px)' % (tallest, tallest * 16))
    print('  largest grid     %d bytes' % biggest)
    print('  gUnk_02024ED0    %d bytes per slot -> %s'
          % (COLLISION_CAP,
             'fits, %d bytes spare' % (COLLISION_CAP - biggest) if biggest <= COLLISION_CAP
             else 'DOES NOT FIT, %d rooms over' % len(over_cap)))
    print()
    print('metatile indices actually used: %d distinct, range 0x%02X-0x%02X'
          % (len(metatiles), min(metatiles), max(metatiles)))
    print('  gUnk_082D88B8 must therefore have at least %d entries'
          % (max(metatiles) + 1))

    if mismatches:
        print('\nsize disagreements (room, expected, declared):')
        for m in mismatches[:12]:
            print('  room %-4d %6d != %6d' % m)
        if len(mismatches) > 12:
            print('  ... and %d more' % (len(mismatches) - 12))
    if errors:
        print('\nunresolvable:')
        for rid, msg in errors[:12]:
            print('  room %-4d %s' % (rid, msg))
        if len(errors) > 12:
            print('  ... and %d more' % (len(errors) - 12))

    return 0 if not errors and not mismatches else 1


def check(rom, dump_path):
    """Compare the reconstruction against a live dump of the running game.

    Produce the dump with:

        DUMP_ROOM=build/room-live.json HOLD=0x10 MASH=600:1:6 \\
            node tools/headless_test.js build/katam-node.js <rom> 1300

    This is the assertion that makes everything else here worth printing.
    Reading a compressed blob out of a ROM and calling the result a collision
    map is a claim; 988 bytes that match what the game is actually indexing is
    evidence."""
    live = json.loads(Path(dump_path).read_text())
    room = resolve(rom, live['room'])
    mine = room['collision']

    fails = []
    for field, got, want in (
        ('room width (px)',  room['roomWidthPx'],   live['roomWidthPx']),
        ('room height (px)', room['roomHeightPx'],  live['roomHeightPx']),
        ('width in cells',   mine['widthCells'],    live['widthCells']),
        ('height in cells',  mine['heightCells'],   live['heightCells']),
    ):
        if got != want:
            fails.append('%s: reconstructed %r, game has %r' % (field, got, want))

    if not fails:
        diffs = [(i, a, b) for i, (a, b)
                 in enumerate(zip(mine['cells'], live['cells'])) if a != b]
        if len(mine['cells']) != len(live['cells']):
            fails.append('cell count: reconstructed %d, game has %d'
                         % (len(mine['cells']), len(live['cells'])))
        elif diffs:
            fails.append('%d of %d cells differ, first at index %d '
                         '(reconstructed 0x%02X, game 0x%02X)'
                         % (len(diffs), len(mine['cells']),
                            diffs[0][0], diffs[0][1], diffs[0][2]))

    if fails:
        print('MISMATCH against the live game, room %d' % live['room'])
        for f in fails:
            print('  ' + f)
        return 1

    print('check: room %d, %dx%d cells -- all %d collision bytes match the '
          'running game' % (live['room'], mine['widthCells'],
                            mine['heightCells'], len(mine['cells'])))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--room', type=int, help='resolve one room and print JSON')
    ap.add_argument('--map', action='store_true',
                    help='with --room, draw the collision grid as text')
    ap.add_argument('--list', action='store_true',
                    help='one line per room: id, size, object count')
    ap.add_argument('--validate', action='store_true',
                    help='sweep every room and report where the model breaks')
    ap.add_argument('--spawn', action='store_true',
                    help='with --room, print a safe warp cell for it; alone, '
                         'print one per room as "roomId cellX cellY source"')
    ap.add_argument('--check', metavar='DUMP',
                    help='compare against a DUMP_ROOM= dump of the live game')
    args = ap.parse_args()

    rom = Rom(args.rom)

    if args.check:
        return check(rom, args.check)

    if args.validate:
        return validate(rom)

    if args.spawn:
        arrivals = arrival_cells(rom)
        rooms = [args.room] if args.room is not None else valid_rooms(rom)
        for rid in rooms:
            try:
                cell, src = spawn_cell(rom, rid, arrivals)
            except (BadPointer, struct.error, IndexError):
                cell, src = None, None
            if cell is None:
                print('%d - - none' % rid)
            else:
                print('%d %d %d %s' % (rid, cell[0], cell[1], src))
        return 0

    if args.list:
        for rid in valid_rooms(rom):
            try:
                r = resolve(rom, rid, want_cells=False)
            except (BadPointer, struct.error, IndexError) as e:
                print('room %-4d  UNRESOLVED  %s' % (rid, e))
                continue
            print('room %-4d  %4dx%-4d px  %3d objects  song %-4d  %s'
                  % (rid, r['roomWidthPx'], r['roomHeightPx'],
                     r['objects']['count'], r['props']['songIdx'],
                     '' if r['collision']['matchesGeometry'] else 'SIZE MISMATCH'))
        return 0

    if args.room is None:
        ap.error('one of --room, --list or --validate is required')

    room = resolve(rom, args.room)
    if args.map:
        print('room %d: %dx%d px, %dx%d cells, %d objects'
              % (args.room, room['roomWidthPx'], room['roomHeightPx'],
                 room['collision']['widthCells'], room['collision']['heightCells'],
                 room['objects']['count']))
        for line in ascii_map(room):
            print(line)
        return 0

    json.dump(room, sys.stdout, indent=2)
    print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
