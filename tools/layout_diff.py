#!/usr/bin/env python3
"""layout_diff.py -- are two native builds' states equal up to code layout?

The `sim` column of the hash log (docs/DETERMINISM.md) is not layout-blind,
and cannot be made so by looking at values alone.  Two ways a change that
alters no behaviour still moves it, both seen when the resource seam turned
every ROM table macro into a load (docs/design/MODDING.md §1.5, R0):

  coincidence   a game word that is *exactly* some symbol's start becomes a
                name token (platform/native/symtab.c).  0x10030000 is game
                data in IWRAM; in one build sub_08005DE0 sat at 0x10030000.
  stale bytes   a destroyed task's callback pointer is left in the IWRAM
                heap, the next task's struct is written over it byte-wise,
                and two bytes of the old host code address survive as game
                state (0x0F4DEA10 = 0x0F | 4DEA of sub_08146CE4+0x... | 10).

So this compares RAW dumps (PORT_HASH_RAW=1 --dump-state) of two builds, and
explains each differing EWRAM/IWRAM word by the two executables' symbol
tables:

  pointer       both words name the same symbol + offset;
  stale         the differing bytes are, in each build, the corresponding
                bytes of the same symbol + offset's address;

and anything else is a real difference.  Every other region (I/O, DMA,
frame state, palette, VRAM, OAM, PPU latches, save) must be byte-identical.

    tools/layout_diff.py A.raw B.raw --a-bin A/katam --b-bin B/katam

Exit status 0 when every difference is explained.  tests/layout_neutral.sh
drives it over the regression recordings.
"""
import argparse
import bisect
import struct
import subprocess
import sys

from state_diff import REGIONS


class Syms:
    def __init__(self, path):
        out = subprocess.run(['nm', '-nS', path], capture_output=True, text=True).stdout
        rows = []
        for line in out.splitlines():
            p = line.split()
            if len(p) == 4 and p[2] in 'tTwWbBdDrR':
                rows.append((int(p[0], 16), int(p[1], 16), p[3]))
            elif len(p) == 3 and p[1] in 'tTwW':
                rows.append((int(p[0], 16), 0, p[2]))
        rows.sort()
        self.addr = [r[0] for r in rows]
        self.rows = rows
        self.by_name = {}
        for a, s, n in rows:
            self.by_name.setdefault(n, a)
        self.lo = rows[0][0] if rows else 0
        self.hi = max(a + max(s, 1) for a, s, _ in rows) if rows else 0

    def locate(self, w):
        """(name, offset) of the symbol containing w, or None."""
        if not (self.lo <= w < self.hi):
            return None
        i = bisect.bisect_right(self.addr, w) - 1
        if i < 0:
            return None
        a, s, n = self.rows[i]
        nxt = self.addr[i + 1] if i + 1 < len(self.addr) else self.hi
        if w - a < max(s, nxt - a):
            return (n, w - a)
        return None


def explain(x, y, sa, sb):
    la, lb = sa.locate(x), sb.locate(y)
    if la is not None and la == lb:
        return 'pointer %s+%#x' % la
    # Stale bytes: the bytes that differ are bytes of one symbol's address
    # in each build.  Rebuild the candidate address in A from the bytes of x
    # that differ plus each possible value of the bytes that do not.
    diff = [k for k in range(4) if (x >> 8 * k) & 0xFF != (y >> 8 * k) & 0xFF]
    # Most stale words are a dead callback: a symbol's entry point with some
    # of its bytes overwritten by the next owner of the memory.  Try every
    # symbol at offset 0 first, with any bytes outside `diff` overwritten.
    mask = sum(0xFF << 8 * k for k in diff)
    for name, a in sa.by_name.items():
        b = sb.by_name.get(name)
        if b is not None and a & mask == x & mask and b & mask == y & mask:
            return 'stale bytes %s of %s' % (diff, name)
    tops = sorted({a >> 24 for a in sa.addr})
    opts = []
    for k in range(4):
        if k in diff:
            opts.append([(x >> 8 * k) & 0xFF])
        elif k == 3:
            opts.append(tops)              # overwritten: any top byte of the image
        elif k == 0:
            opts.append(range(256))        # overwritten: any low byte
        else:
            opts.append([(x >> 8 * k) & 0xFF])
    for b3 in opts[3]:
        for b0 in opts[0]:
            cand = (b3 << 24) | (opts[2][0] << 16) | (opts[1][0] << 8) | b0
            loc = sa.locate(cand)
            if loc is None or loc[0] not in sb.by_name:
                continue
            cb = sb.by_name[loc[0]] + loc[1]
            if all((cb >> 8 * k) & 0xFF == (y >> 8 * k) & 0xFF for k in diff):
                return 'stale bytes %s of %s+%#x' % (diff, loc[0], loc[1])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--a-bin', required=True); ap.add_argument('--b-bin', required=True)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    A = open(args.a, 'rb').read(); B = open(args.b, 'rb').read()
    if len(A) != len(B):
        print('dumps differ in size'); return 1
    sa, sb = Syms(args.a_bin), Syms(args.b_bin)
    off = 0
    counts = {}
    real = []
    for name, base, size in REGIONS:
        for i in range(0, size, 4):
            x = struct.unpack_from('<I', A, off + i)[0]
            y = struct.unpack_from('<I', B, off + i)[0]
            if x == y:
                continue
            why = explain(x, y, sa, sb) if name in ('ewram', 'iwram') else None
            if why is None:
                real.append((name, base + i, x, y))
            else:
                kind = why.split()[0]
                counts[kind] = counts.get(kind, 0) + 1
                if args.verbose:
                    print('  %-6s %08X  %08X %08X  %s' % (name, base + i, x, y, why))
        off += size
    print('layout-explained: %s; real differences: %d'
          % (', '.join('%s %d' % kv for kv in sorted(counts.items())) or 'none', len(real)))
    for r in real[:30]:
        print('  %-6s %08X  %08X %08X' % r)
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
