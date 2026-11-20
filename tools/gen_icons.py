#!/usr/bin/env python3
"""
gen_icons.py -- the installable page's icons, drawn from scratch.

Nothing here comes from the ROM: the picture is a handful of circles and a
star described below, rasterised with 4x4 supersampling by plain Python (no
PIL on the build machines).  `make dist` runs it; the PNGs are build output
(*.png is ignored by git) and web/icon.svg is the same design for browsers
that take SVG icons.

    tools/gen_icons.py OUTDIR      -> icon-192.png icon-512.png icon-maskable-512.png
"""

import math
import struct
import sys
import zlib
from pathlib import Path

BG = (16, 18, 22)          # the page's #101216
PINK = (255, 154, 208)     # the first Kirby's colour in the page's notices
CHEEK = (255, 105, 160)
EYE = (24, 24, 40)
WHITE = (255, 255, 255)
STAR = (255, 224, 102)


def star_poly(cx, cy, r_out, r_in, rot=-math.pi / 2):
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        a = rot + i * math.pi / 5
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def in_poly(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def shapes(maskable):
    """The design on a 1.0 x 1.0 canvas, back to front: (test, colour)."""
    s = 0.80 if maskable else 1.0          # maskable icons keep a safe zone
    c = 0.5

    def t(v):
        return c + (v - c) * s

    body_r = 0.36 * s
    out = []
    out.append((lambda x, y: (x - t(0.5)) ** 2 + (y - t(0.52)) ** 2 <= body_r ** 2, PINK))
    for ex in (0.42, 0.58):
        out.append((lambda x, y, ex=ex: ((x - t(ex)) / (0.035 * s)) ** 2 +
                    ((y - t(0.45)) / (0.085 * s)) ** 2 <= 1, EYE))
        out.append((lambda x, y, ex=ex: ((x - t(ex)) / (0.02 * s)) ** 2 +
                    ((y - t(0.41)) / (0.035 * s)) ** 2 <= 1, WHITE))
    for cx in (0.33, 0.67):
        out.append((lambda x, y, cx=cx: ((x - t(cx)) / (0.05 * s)) ** 2 +
                    ((y - t(0.58)) / (0.025 * s)) ** 2 <= 1, CHEEK))
    pts = [(t(px), t(py)) for px, py in star_poly(0.80, 0.20, 0.13, 0.055)]
    out.append((lambda x, y: in_poly(x, y, pts), STAR))
    return out


def render(size, maskable):
    ss = 4
    layers = shapes(maskable)

    def colour_at(x, y):
        col = BG
        for test, c in layers:
            if test(x, y):
                col = c
        return col

    rows = []
    for py in range(size):
        row = bytearray([0])               # PNG filter: none
        for px in range(size):
            # Flat pixels (all four corners agree) skip the supersampling,
            # which is nearly all of them; only edges pay for 16 samples.
            corners = {colour_at((px + dx) / size, (py + dy) / size)
                       for dx in (0.01, 0.99) for dy in (0.01, 0.99)}
            if len(corners) == 1:
                row += bytes(corners.pop())
                continue
            acc = [0, 0, 0]
            for sy in range(ss):
                y = (py + (sy + 0.5) / ss) / size
                for sx in range(ss):
                    col = colour_at((px + (sx + 0.5) / ss) / size, y)
                    acc[0] += col[0]
                    acc[1] += col[1]
                    acc[2] += col[2]
            n = ss * ss
            row += bytes((acc[0] // n, acc[1] // n, acc[2] // n))
        rows.append(bytes(row))
    return png(size, size, b''.join(rows))


def png(w, h, raw):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) +
            chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, size, mask in (('icon-192.png', 192, False), ('icon-512.png', 512, False),
                             ('icon-maskable-512.png', 512, True)):
        (out / name).write_bytes(render(size, mask))
    print('  ICONS   %s' % out)


if __name__ == '__main__':
    main()
