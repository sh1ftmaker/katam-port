#!/usr/bin/env python3
"""objdiff.py -- the live objects of two rooms-measurement dumps, side by side.

    python3 tools/rooms/objdiff.py DIR/<rec> FRAME

Lists every struct Object (kind 1) in A.FRAME.bin and B.FRAME.bin (written by
measure.mjs --dump-at) as type/x/y/room/hp/owner, and the ones in only one."""
import struct
import sys

REG = [('ewram', 0x02000000, 0x40000), ('iwram', 0x03000000, 0x8000), ('io', 0x04000000, 0x400),
       ('pltt', 0x05000000, 0x400), ('vram', 0x06000000, 0x18000), ('oam', 0x07000000, 0x400),
       ('simblk', 0x02040000, 0x1000)]
OBJMAIN = 0x08099B81


class Img:
    def __init__(self, path):
        b = open(path, 'rb').read()
        self.parts, o = [], 0
        for n, base, sz in REG:
            self.parts.append((base, b[o:o + sz]))
            o += sz

    def rd(self, a, n):
        for base, b in self.parts:
            if base <= a < base + len(b):
                return b[a - base:a - base + n]
        raise KeyError(hex(a))

    def u8(self, a): return self.rd(a, 1)[0]
    def u16(self, a): return struct.unpack('<H', self.rd(a, 2))[0]
    def s16(self, a): return struct.unpack('<h', self.rd(a, 2))[0]
    def u32(self, a): return struct.unpack('<I', self.rd(a, 4))[0]
    def s32(self, a): return struct.unpack('<i', self.rd(a, 4))[0]

    def objects(self):
        out = []
        for i in range(0x80):
            t = 0x030019F0 + i * 0x14
            if self.u32(t + 8) != OBJMAIN:
                continue
            off, fl = self.u16(t + 6), self.u16(t + 0x12)
            o = 0x02000000 + (off << 2) if fl & 0x10 else 0x03000000 + off
            if self.u8(o) != 1:
                continue
            out.append(dict(task=i, addr=o, type=self.u8(o + 130), x=self.s32(o + 64) >> 8, y=self.s32(o + 68) >> 8,
                            room=self.u16(o + 96), hp=self.s16(o + 128), owner=self.u8(o + 86),
                            flags=self.u32(o + 8), prio=self.u16(t + 0x10)))
        return out


def main():
    d, f = sys.argv[1], int(sys.argv[2])
    A, B = Img('%s/A.%d.bin' % (d, f)), Img('%s/B.%d.bin' % (d, f))
    oa, ob = A.objects(), B.objects()
    key = lambda o: (o['type'], o['x'], o['y'], o['room'], o['hp'])
    ka, kb = [key(o) for o in oa], [key(o) for o in ob]
    for tag, mine, other, objs in (('A only', ka, kb, oa), ('B only', kb, ka, ob)):
        rest = list(other)
        for k, o in zip(mine, objs):
            if k in rest:
                rest.remove(k)
            else:
                print(tag, o)
    print('owners differ:')
    for o in oa:
        m = [p for p in ob if key(p) == key(o)]
        if m and all(p['owner'] != o['owner'] for p in m):
            print('  type %(type)d at %(x)d,%(y)d room %(room)d' % o, 'A owner', o['owner'], 'B owner', m[0]['owner'])
    for i in range(4):
        k = 0x02020EE0 + i * 424
        print('kirby', i, 'A', A.s32(k + 64) >> 8, A.s32(k + 68) >> 8, A.u16(k + 96), 'B', B.s32(k + 64) >> 8, B.s32(k + 68) >> 8, B.u16(k + 96))
    for i in range(4):
        li = 0x02023530 + i * 0x668
        print('levelinfo', i, 'room A', A.u16(li + 0x65E - 0x65E + 0), 'B', B.u16(li))


if __name__ == '__main__':
    main()
