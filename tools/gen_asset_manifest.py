#!/usr/bin/env python3
"""gen_asset_manifest.py -- write the vanilla-pack manifest (MODDING.md §2.1).

Dev-only, run once per manifest change.  Reads the decompilation's katam.elf
symbol table (addresses; a table's extent is the distance to the next label),
this tree's own headers for the record schemas (game/include/data.h, checked
against the offsets platform/port/gba_layout.h asserts) and object names
(game/include/constants/object_types.h).  It reads no ROM bytes, and the
manifest it writes holds addresses, counts, sizes, formats and names only.

    tools/gen_asset_manifest.py --elf ../decomp/katam.elf

Writes assets/manifest.json and platform/res_manifest.h (the same JSON as a C
string, so every build carries the manifest without a file to find; the
extractor and the pack compiler parse that).  --check fails if either is out
of date.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ROM_SHA1 = '274b102b6d940f46861a92b4e65f89a51815c12c'   # decomp/katam.sha1 (USA)
ROM_SIZE = 16 * 1024 * 1024

# Record schemas: (name, type, offset).  Types: u8 u16 s16 u32 ptr32 u8xN.
# Field names are the C member names, so pack files and source agree.
ROOM_PROPS = [
    ('songIdx', 'u16', 0), ('unk02', 'u16', 2), ('unk04', 'u16', 4),
    ('unkXmod_06', 's16', 6), ('unkXmod_08', 's16', 8), ('unk0A', 'u16', 10),
    ('unk0C', 'u16', 12), ('unk0E', 's16', 14), ('unk10', 's16', 16),
    ('priorityFlags', 'u16', 18), ('pixelDataIdx', 'u16', 20),
    ('paletteDataIdx', 'u16', 22), ('mapDataIdx', 'u16', 24),
    ('solidityMapIdx', 'u16', 26), ('objectList2Idx', 'u16', 28),
    ('backgroundIdx', 'u16', 30), ('objectListIdx', 'u16', 32),
    ('unk22', 'u16', 34), ('doorsIdx', 'u16', 36), ('unk26', 'u8x2', 38),
]
OBJ_DESC = [
    ('unk0', 'u8', 0), ('unk1', 'u8', 1), ('unk2', 'u16', 2), ('unk4', 's16', 4),
    ('kirbyAbility', 'u16', 6), ('unk8', 'u16', 8), ('pad0A', 'u8x2', 10),
    ('numTiles', 'u32', 12), ('unk10', 'ptr32', 16), ('unk14', 'ptr32', 20),
]
FG_PALETTE = [   # struct ForegroundPalette; `palette` is the payload
    ('palette', 'ptr32', 0), ('unk4', 'ptr32', 4), ('unk8', 'u16', 8),
    ('roomId', 'u16', 10),
]


def symbols(elf):
    out = subprocess.run(['nm', '-n', elf], capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            rows.append((int(p[0], 16), p[2]))
    return rows


def extent(rows, label):
    for i, (a, n) in enumerate(rows):
        if n == label:
            j = i + 1
            while rows[j][0] == a:
                j += 1
            return a, rows[j][0] - a
    sys.exit('label %s not in the ELF' % label)


def check_offsets(struct, fields):
    """The schema against what gba_layout.h asserts for the C struct."""
    text = open(os.path.join(ROOT, 'platform/port/gba_layout.h')).read()
    for name, _, off in fields:
        if name.startswith('pad'):
            continue
        m = re.search(r'offsetof\(struct %s, %s\) == (\d+)' % (struct, name), text)
        if not m or int(m.group(1)) != off:
            sys.exit('schema: struct %s::%s is not at %d per gba_layout.h' % (struct, name, off))


def size_of(t):
    return {'u8': 1, 'u16': 2, 's16': 2, 'u32': 4, 'ptr32': 4}.get(t) or int(t[3:])


def object_names(count):
    names = ['OBJ_%02X' % i for i in range(count)]
    for line in open(os.path.join(ROOT, 'game/include/constants/object_types.h')):
        m = re.match(r'#define (OBJ_\w+)\s+(0x[0-9A-Fa-f]+)\b', line)
        if m and int(m.group(2), 16) < count:
            names[int(m.group(2), 16)] = m.group(1)
    return names


def fields_json(fields):
    return [{'name': n, 'type': t, 'offset': o} for n, t, o in fields]


def build(elf):
    rows = symbols(elf)
    check_offsets('RoomProps', ROOM_PROPS)
    check_offsets('Unk_08351648', OBJ_DESC)
    check_offsets('ForegroundPalette', FG_PALETTE)
    kinds = []

    a, n = extent(rows, 'gRoomProps')
    stride = 0x28
    assert n % stride == 0
    kinds.append({
        'kind': 'room_props', 'realm': 'sim', 'label': 'gRoomProps',
        'addr': '0x%08X' % a, 'shape': 'array', 'count': n // stride, 'stride': stride,
        'path': 'rooms/%04u/room.json', 'fields': fields_json(ROOM_PROPS)})

    a, n = extent(rows, 'gForegroundPalettes')
    assert n % 4 == 0
    kinds.append({
        'kind': 'fg_palette', 'realm': 'presentation', 'label': 'gForegroundPalettes',
        'addr': '0x%08X' % a, 'shape': 'ptrtab', 'count': n // 4, 'stride': 12,
        'path': 'gfx/palettes/fg/%u.json', 'fields': fields_json(FG_PALETTE),
        'payload': {'field': 'palette', 'format': 'bgr555', 'banks': 6, 'colors': 16}})

    a, n = extent(rows, 'gUnk_08351648')
    stride = 0x18
    assert n % stride == 0
    kinds.append({
        'kind': 'obj_desc', 'realm': 'sim', 'label': 'gUnk_08351648',
        'addr': '0x%08X' % a, 'shape': 'array', 'count': n // stride, 'stride': stride,
        'path': 'objects/types.json', 'fields': fields_json(OBJ_DESC),
        'names': object_names(n // stride)})

    for k in kinds:
        assert sum(size_of(f['type']) for f in k['fields']) == k['stride'], k['kind']

    return {
        'manifest': 1,
        'rom': {'sha1': ROM_SHA1, 'size': ROM_SIZE, 'game_code': 'B8KE'},
        'source': 'katam.elf of the decompilation (labels and their spacing); '
                  'record schemas from game/include/data.h; names from '
                  'game/include/constants/object_types.h. No ROM bytes.',
        'kinds': kinds,
    }


def render(m):
    text = json.dumps(m, indent=2, ensure_ascii=True) + '\n'
    lines = ['/* Generated by tools/gen_asset_manifest.py from assets/manifest.json -- do not edit.',
             ' * The vanilla-pack manifest (docs/design/MODDING.md §2.1): addresses, counts,',
             ' * formats and names, no ROM bytes.  sha256 of the JSON: %s */'
             % hashlib.sha256(text.encode()).hexdigest(),
             '#ifndef GUARD_PORT_RES_MANIFEST_H', '#define GUARD_PORT_RES_MANIFEST_H', '',
             'static const char kResManifestJson[] =']
    for l in text.splitlines(True):
        lines.append('    "%s"' % l.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n'))
    lines[-1] += ';'
    lines += ['', '#endif', '']
    return text, '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=os.path.join(ROOT, '..', 'decomp', 'katam.elf'))
    ap.add_argument('--check', action='store_true',
                    help='only check that the header matches assets/manifest.json')
    args = ap.parse_args()
    jpath = os.path.join(ROOT, 'assets/manifest.json')
    hpath = os.path.join(ROOT, 'platform/res_manifest.h')
    if args.check:
        text, header = render(json.load(open(jpath)))
        ok = open(jpath).read() == text and open(hpath).read() == header
        print('manifest: %s' % ('up to date' if ok else 'OUT OF DATE -- rerun tools/gen_asset_manifest.py'))
        return 0 if ok else 1
    text, header = render(build(args.elf))
    os.makedirs(os.path.dirname(jpath), exist_ok=True)
    open(jpath, 'w').write(text)
    open(hpath, 'w').write(header)
    print('wrote %s and %s' % (os.path.relpath(jpath, ROOT), os.path.relpath(hpath, ROOT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
