#!/usr/bin/env python3
"""wide_unrelocate.py -- put a wide-build state dump back into the console's layout.

The wide build (platform/port/wide.h, KATAM_MAX_KIRBYS) moves the per-player
tables into the port sim block.  At KATAM_MAX_KIRBYS=4 it must be the same
program as the default build at a different layout (proof W0).  This rewrites
a raw dump (PORT_HASH_RAW=1 --dump-state) of the wide build into the layout of
the default build, so that tools/layout_diff.py can compare the two:

  1. each moved table's bytes are copied back to its console address;
  2. every 32-bit word in EWRAM, IWRAM, the DMA canon words and the sim block
     that points into a moved table is rewritten to the console address of the
     same byte;
  3. the sim block is cut back to the default build's length (the wide area
     is its tail) and its header's usedBytes restored.
  4. the human-slot layer's state, which the wide build keeps N wide in
     gPortSim->lmpw (port/wide.h struct PortWideLmp), is written back into
     the four-slot fields the default build keeps it in (lmp, frame, view;
     map_back_fields below), from the map's `simfield` lines.  Nothing is
     masked; a value those fields cannot hold fails the dump.

Coincidence is possible in (2) -- a game word that merely equals an address in
the wide area -- and would show up as a "real difference" afterwards, never be
hidden by this.  The two shadow-OAM buffers hold no pointers (as
platform/inputlog.c's kNoPointers) and are not rewritten.

With --explain DEFAULT.raw, one more class is explained, the one
tools/layout_diff.py calls "stale bytes" for host pointers: a freed object's
pointer into a moved table, partly overwritten by the next object to use the
memory, so that only some bytes of the old address survive (seen: the low
halfword of an effect object's `parent`, &gKirbys[3], under a CPU-Kirby
trigger in boot_to_level; the high halfword of &gKirbys[0] in play_rooms).  A word is explained when the bytes in which the
two dumps differ are, in each, the bytes of the same pointer spelled in that
build's layout; the default build's word is then copied, and the count is
reported.

    tools/wide_unrelocate.py WIDE.raw MAP ORIG_SIM_LEN OUT.raw [--explain DEFAULT.raw]

MAP is the file the wide build writes with KATAM_WIDE_MAP=FILE.
"""
import struct
import sys

from state_diff import REGIONS

SIM_BASE = 0x02040000


def map_back_fields(sim, simb, F):
    """The human-slot layer's state (port/wide.h struct PortWideLmp, the wide
    build's gPortSim->lmpw) back into the four-slot fields the default build
    keeps it in (port/simblock.h: lmp.slots, lmp.joining with the held joins
    in its high byte, lmp.keys[3] for slots 1..3, lmp.stray[4],
    frame.lmpPressed[3], view.lmpDev two bits a slot with view.lmpDevSet,
    view.lmpGroupRoom[4]; port/localmp.h and platform/localmp.c spell both).
    The wide build leaves those fields zero.  Nothing is masked: a value the
    four-slot fields cannot hold is an error.  Not mapped (no counterpart in
    the default build, and outside its sim block's length, so never compared
    there): wideCtl (the live Kirby count) and paintw (paint for Kirbys 4..:
    at N = 4 it holds nothing, checked zero)."""
    def rd(name, i=0, size=None):
        off, sz = F[name]
        return int.from_bytes(sim[off:off + (size or sz)], 'little') if size is None else \
            int.from_bytes(sim[off + i * size:off + (i + 1) * size], 'little')

    def wr(name, value, i=0, size=None):
        off, sz = F[name]
        size = size or sz
        if off + (i + 1) * size > len(simb):
            raise ValueError(name)
        old = int.from_bytes(simb[off + i * size:off + (i + 1) * size], 'little')
        if old:
            return '%s[%d] is %#x in the wide build (expected 0)' % (name, i, old)
        simb[off + i * size:off + (i + 1) * size] = value.to_bytes(size, 'little')
        return None

    need = ['lmpw.slots', 'lmpw.joining', 'lmpw.waiting', 'lmpw.devSet', 'lmpw.keys', 'lmpw.pressed',
            'lmpw.groupRoom', 'lmpw.stray', 'lmpw.dev', 'lmp.slots', 'lmp.joining', 'lmp.keys', 'lmp.stray',
            'frame.lmpPressed', 'view.lmpDev', 'view.lmpDevSet', 'view.lmpGroupRoom']
    missing = [n for n in need if n not in F]
    if missing:
        return 'map has no simfield %s' % ', '.join(missing)
    slots, joining, waiting, devset = (rd('lmpw.' + n) for n in ('slots', 'joining', 'waiting', 'devSet'))
    keys = [rd('lmpw.keys', s, 2) for s in range(4)]
    pressed = [rd('lmpw.pressed', s, 2) for s in range(4)]
    group = [rd('lmpw.groupRoom', s, 2) for s in range(4)]
    stray = [rd('lmpw.stray', s, 1) for s in range(4)]
    dev = [rd('lmpw.dev', s, 1) for s in range(4)]
    if slots > 0xFF or joining > 0xFF or waiting > 0xFF or devset > 0xF:
        return 'lmpw masks past four slots: slots %#x joining %#x waiting %#x devSet %#x' % (slots, joining, waiting, devset)
    if keys[0] or pressed[0]:
        return 'lmpw keys/pressed of slot 0 are set (%#x, %#x): no four-slot field holds them' % (keys[0], pressed[0])
    if any(devset >> s & 1 and dev[s] > 3 for s in range(4)) or any(not devset >> s & 1 and dev[s] for s in range(4)):
        return 'lmpw.dev %s (devSet %#x) does not fit two bits a slot' % (dev, devset)
    if 'paintw' in F:
        off, sz = F['paintw']
        if any(sim[off:off + sz]):
            return 'paintw is not zero at N = 4'
    lmpdev = 0
    for s in range(4):
        if devset >> s & 1:
            lmpdev |= dev[s] << 2 * s
    for e in (wr('lmp.slots', slots), wr('lmp.joining', joining | waiting << 8),
              wr('view.lmpDevSet', devset), wr('view.lmpDev', lmpdev)):
        if e:
            return e
    for s in range(4):
        e = (wr('lmp.stray', stray[s], s, 1) or wr('view.lmpGroupRoom', group[s], s, 2)
             or (wr('lmp.keys', keys[s], s - 1, 2) if s else None)
             or (wr('frame.lmpPressed', pressed[s], s - 1, 2) if s else None))
        if e:
            return e
    return None


def main():
    src, mapf, orig_len, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    explain = sys.argv[6] if len(sys.argv) > 6 and sys.argv[5] == '--explain' else None
    b = bytearray(open(src, 'rb').read())
    lines = open(mapf).read().split('\n')
    head = lines[0].split()
    kirbys = int(head[1])
    if kirbys != 4:
        print('wide_unrelocate: only N = 4 has a console layout (map says %d)' % kirbys)
        return 2
    relocs = []
    fields = {}
    for ln in lines[1:]:
        p = ln.split()
        if len(p) == 6:
            relocs.append((p[0], int(p[1], 16), int(p[2], 16), int(p[3], 16), int(p[4], 16)))
        elif len(p) == 4 and p[0] == 'simfield':
            fields[p[1]] = (int(p[2], 16), int(p[3], 16))

    off = {}
    o = 0
    for name, base, size in REGIONS:
        off[name] = (o, base, size)
        o += size
    fixed = o
    sim = b[fixed:]

    def view(addr):
        """(buffer, index) of a console address in the dump, or None."""
        for name in ('ewram', 'iwram'):
            o, base, size = off[name]
            if base <= addr < base + size:
                return o + addr - base
        return None

    # 1. copy back
    for name, orig, osz, now, nsz in relocs:
        i = view(orig)
        if i is None:
            continue                      # the ROM table: not RAM on the console
        s = now - SIM_BASE
        b[i:i + osz] = sim[s:s + osz]
    # the old homes of the moved tables were zero in the wide build; now filled.

    # 2. pointers
    def tr(w):
        for name, orig, osz, now, nsz in relocs:
            if now <= w < now + nsz:
                d = w - now
                return orig + d if d < osz else w
        return w

    # Only words that differ from the default build are candidates: a word
    # that merely lies in the wide area's range (a coordinate, two packed
    # u16s -- both seen in play_rooms) is equal in the two dumps and is left.
    a = open(explain, 'rb').read() if explain else None
    nfix = 0
    spans = [off['ewram'][:1] + (off['ewram'][2],), off['iwram'][:1] + (off['iwram'][2],),
             (off['dma'][0], off['dma'][2])]
    noptr = [(0x030031C0, 0x030031C0 + 0x400), (0x030060B0, 0x030060B0 + 0x300)]
    nopi = [(view(lo), view(lo) + hi - lo) for lo, hi in noptr]

    def fix(buf, i, ref):
        w = struct.unpack_from('<I', buf, i)[0]
        if ref is not None and struct.unpack_from('<I', ref, i)[0] == w:
            return 0
        t = tr(w)
        if t != w and (ref is None or struct.unpack_from('<I', ref, i)[0] == t):
            struct.pack_into('<I', buf, i, t)
            return 1
        return 0

    for start, size in spans:
        for i in range(start, start + size, 4):
            if any(lo <= i < hi for lo, hi in nopi):
                continue
            nfix += fix(b, i, a)
    simb = bytearray(sim[:orig_len])
    aref = a[fixed:fixed + orig_len] if a is not None else None
    for i in range(0, len(simb) - 3, 4):
        nfix += fix(simb, i, aref)
    # 3. header: usedBytes at +8
    struct.pack_into('<I', simb, 8, orig_len)

    # 3b. the sim block's moved fields (the map's `simfield` lines)
    if fields:
        err = map_back_fields(sim, simb, fields)
        if err:
            print('wide_unrelocate: %s' % err)
            return 3

    # 4. stale bytes of a moved pointer (--explain)
    nstale = 0
    if explain:
        for start, size in spans[:2]:
            for i in range(start, start + size, 4):
                x = struct.unpack_from('<I', a, i)[0]
                y = struct.unpack_from('<I', b, i)[0]
                if x == y:
                    continue
                mask = [k for k in range(4) if ((x >> 8 * k) & 0xFF) != ((y >> 8 * k) & 0xFF)]
                yb = y.to_bytes(4, 'little')
                ok = False
                for name, orig, osz, now, nsz in relocs:
                    if all(k < 2 for k in mask):
                        # the low halfword survived: p = hi16 | y.lo16
                        for hi in range(now >> 16, ((now + osz - 1) >> 16) + 1):
                            p = (hi << 16) | (y & 0xFFFF)
                            if now <= p < now + osz:
                                q = orig + p - now
                                if (q & 0xFFFF) == (x & 0xFFFF):
                                    ok = True
                    elif all(k >= 2 for k in mask):
                        # the high halfword survived: p in [y.hi16 << 16, +64K)
                        lo = max(now, (y >> 16) << 16); hi = min(now + osz, ((y >> 16) + 1) << 16)
                        if lo < hi:
                            h1 = (orig + lo - now) >> 16; h2 = (orig + hi - 1 - now) >> 16
                            ok = h1 <= (x >> 16) <= h2
                    else:
                        # some middle bytes survived (seen: bytes 1-2 of
                        # &gKirbys[0].base.sprite under a sprite word): try
                        # every address of the table
                        xb = x.to_bytes(4, 'little')
                        for p in range(now, now + osz):
                            pb = p.to_bytes(4, 'little')
                            if all(pb[k] == yb[k] for k in mask):
                                qb = (orig + p - now).to_bytes(4, 'little')
                                if all(qb[k] == xb[k] for k in mask):
                                    ok = True
                                    break
                    if ok:
                        break
                if ok:
                    struct.pack_into('<I', b, i, x); nstale += 1
    open(out, 'wb').write(bytes(b[:fixed]) + bytes(simb))
    print('unrelocated %d tables, %d pointer words, %d stale-pointer words' % (len(relocs), nfix, nstale))
    return 0


if __name__ == '__main__':
    sys.exit(main())
