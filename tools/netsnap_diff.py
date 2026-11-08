#!/usr/bin/env python3
"""netsnap_diff.py -- compare two full netplay snapshot blobs (platform/netsim.c).

    tools/netsnap_diff.py A.snp B.snp [--show 40]

Decompresses both (Python 3.14's compression.zstd), splits the canonical
image into its parts (netsim.c kParts; the sim block's size from its header)
and lists the differing words by GBA address.  Exit 0 when identical.
"""
import struct
import sys
from compression import zstd

PARTS = [('ewram', 0x02000000, 0x40000), ('iwram', 0x03000000, 0x8000),
         ('simblk', 0x02040000, None), ('io', 0x04000000, 0x400), ('dma', 0, 112),
         ('ppu', 0, 16), ('pltt', 0x05000000, 0x400), ('vram', 0x06000000, 0x18000),
         ('oam', 0x07000000, 0x400), ('sram', 0x09000000, 0x10000)]


def image(path):
    b = open(path, 'rb').read()
    magic, ver, kind, frame, base, raw, payload = struct.unpack_from('<IBxBxIIII', b, 0)
    assert magic == 0x504E534B and kind == 0, 'not a full snapshot'
    img = zstd.decompress(b[40:40 + payload])
    assert len(img) == raw
    return frame, img, b[24:32], b[32:40]


def parts(img):
    off = 0
    out = []
    for name, base, size in PARTS:
        if size is None:
            size = struct.unpack_from('<I', img, off + 8)[0]   # hdr.usedBytes
        out.append((name, base, off, size))
        off += size
    return out


def main():
    a, b = sys.argv[1], sys.argv[2]
    show = int(sys.argv[sys.argv.index('--show') + 1]) if '--show' in sys.argv else 40
    fa, A, va, ha = image(a)
    fb, B, vb, hb = image(b)
    print('frames %d / %d, simVersion %s / %s, hash %s / %s'
          % (fa, fb, va.hex(), vb.hex(), ha.hex(), hb.hex()))
    diffs = []
    for name, base, off, size in parts(A):
        for i in range(0, size, 4):
            x, y = A[off + i:off + i + 4], B[off + i:off + i + 4]
            if x != y:
                diffs.append((name, (base + i) if base else i, x[::-1].hex(), y[::-1].hex()))
    by = {}
    for d in diffs:
        by[d[0]] = by.get(d[0], 0) + 1
    print('differing words: %d %s' % (len(diffs), by))
    for d in diffs[:show]:
        print('  %-6s %08X  %s %s' % d)
    return 1 if diffs else 0


if __name__ == '__main__':
    sys.exit(main())
