#!/usr/bin/env python3
"""pngdiff.py A.png B.png [OUT.png] -- pixels that differ between two
screenshots of the same size (8-bit RGB/RGBA, non-interlaced, as the port's
--screenshot writes them): count, bounding box, and optionally an image with
A on the left, B in the middle and the differing pixels in red on the right,
scaled 2x.  No dependencies."""
import struct
import sys
import zlib


def read(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'\x89PNG\r\n\x1a\n'
    o, idat, w = 8, b'', None
    while o < len(b):
        n, t = struct.unpack('>I4s', b[o:o + 8])
        d = b[o + 8:o + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', d[:10])
        elif t == b'IDAT':
            idat += d
        o += 12 + n
    bpp = {2: 3, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    rows, prev, s = [], bytearray(w * bpp), w * bpp
    for y in range(h):
        f, line = raw[y * (s + 1)], bytearray(raw[y * (s + 1) + 1:(y + 1) * (s + 1)])
        for i in range(s):
            a = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + up) & 255
            elif f == 3: line[i] = (line[i] + (a + up) // 2) & 255
            elif f == 4:
                p = a + up - c
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 255
        rows.append([tuple(line[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = line
    return w, h, rows


def write(path, w, h, px):
    raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in px)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    wa, ha, A = read(sys.argv[1])
    wb, hb, B = read(sys.argv[2])
    assert (wa, ha) == (wb, hb)
    d = [(x, y) for y in range(ha) for x in range(wa) if A[y][x] != B[y][x]]
    if d:
        xs, ys = [p[0] for p in d], [p[1] for p in d]
        print('%d pixels differ, box x %d..%d y %d..%d' % (len(d), min(xs), max(xs), min(ys), max(ys)))
    else:
        print('identical')
    if len(sys.argv) > 3:
        diff = set(d)
        out = []
        for y in range(ha):
            row = A[y] + [(255, 255, 255)] * 4 + B[y] + [(255, 255, 255)] * 4 + \
                [(255, 0, 0) if (x, y) in diff else tuple(v // 3 for v in A[y][x]) for x in range(wa)]
            row2 = [p for p in row for _ in (0, 1)]
            out += [row2, row2]
        write(sys.argv[3], len(out[0]), len(out), out)


if __name__ == '__main__':
    main()
