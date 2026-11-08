#!/usr/bin/env python3
"""census.py -- name the blocks a divergence census found (platform/census.c).

    tools/census.py OUT.txt [OUT2.txt ...] --bin build/lp64/katam
                    [--masks tests/census/masks.txt] [--unmasked]

Maps each differing 64-byte block to a symbol: the fixed RAM symbols of
game/generated/port/ram_symbols.h (sizes from platform/port/gba_layout.h),
the task heaps by the owning task's main function (from the census's own task
walk, resolved with nm), the port sim block by field.  Merges runs of blocks
with one name and prints them in order of first divergence, with the mask
class when --masks names it.  Several census files (one per recording) are
merged: a block's first frame is per file, so the table says which recording.
"""
import argparse
import bisect
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PRIM = {'u8': 1, 's8': 1, 'bool8': 1, 'u16': 2, 's16': 2, 'u32': 4, 's32': 4,
        'bool32': 4, 'int': 4, 'enum': 4, 'void': 4}


def layout_sizes():
    sz = {}
    for ln in open(os.path.join(ROOT, 'platform', 'port', 'gba_layout.h')):
        m = re.search(r'sizeof\((struct|union) (\w+)\) == (\d+)', ln)
        if m:
            sz[m.group(2)] = int(m.group(3))
    return sz


def ram_symbols():
    sz = layout_sizes()
    out = []
    for ln in open(os.path.join(ROOT, 'game', 'generated', 'port', 'ram_symbols.h')):
        m = re.match(r'#define (\w+) \((.*)\)(0x[0-9A-Fa-f]+)\)?\s*$', ln.strip())
        if not m:
            continue
        name, ty, addr = m.group(1), m.group(2), int(m.group(3), 16)
        base = re.search(r'(struct|union|enum)?\s*(\w+)', ty.lstrip('*( '))
        el = 4
        if base:
            word = base.group(2)
            if base.group(1) in ('struct', 'union'):
                el = sz.get(word, 4)
            elif base.group(1) == 'enum':
                el = 4
            else:
                el = PRIM.get(word, 4)
            if 'PTR32' in ty:
                el = 4
        n = 1
        for d in re.findall(r'\[([^\]]+)\]', ty):
            try:
                n *= int(eval(d.replace('NUM_KEYS', '10'), {}))
            except Exception:
                n *= 1
        out.append((addr, addr + max(el * n, 1), name))
    out.sort()
    return out


SIMBLK = [(0x00, 0x20, 'simblk.hdr'), (0x20, 0x30, 'simblk.frame'),
          (0x30, 0x40, 'simblk.spare'), (0x40, 0x50, 'simblk.warp'),
          (0x50, 0x70, 'simblk.view'), (0x70, 0xB0, 'simblk.game'),
          (0xB0, 0xC0, 'simblk.session'), (0xC0, 0x40000, 'simblk.rando+')]


def fn_names(binary):
    # Task::main holds canonical code IDs (platform/port/fnid.h): name them
    # from the generated table first, then fall back to the binary.
    names = {}
    ids = os.path.join(ROOT, 'game', 'generated', 'fnid_ids.h')
    if os.path.exists(ids):
        for ln in open(ids):
            m = re.match(r'#define FNID_(\w+) (0x[0-9A-Fa-f]+)u', ln)
            if m:
                names[int(m.group(2), 16)] = m.group(1)
    if not binary:
        return names
    txt = subprocess.run(['nm', '-C', binary], capture_output=True, text=True).stdout
    for ln in txt.splitlines():
        p = ln.split(None, 2)
        if len(p) == 3 and p[1] in 'tTwW':
            names[int(p[0], 16) & 0xFFFFFFFF] = p[2]
    return names


def load_masks(path):
    out = []
    if not path:
        return out
    for ln in open(path):
        ln = ln.split('#', 1)[0].split()
        if len(ln) >= 3:
            out.append((int(ln[0], 16), int(ln[1], 16), ln[2], ' '.join(ln[3:])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--bin')
    ap.add_argument('--masks')
    ap.add_argument('--unmasked', action='store_true')
    a = ap.parse_args()
    syms = ram_symbols()
    starts = [s[0] for s in syms]
    fns = fn_names(a.bin)
    masks = load_masks(a.masks)

    def name_of(region, addr, owner, obase):
        if region == 'simblk':
            off = addr - 0x02040000
            for lo, hi, n in SIMBLK:
                if lo <= off < hi:
                    return n
        if region in ('dma', 'ppu'):
            return region + ' (host state)'
        if region not in ('ewram', 'iwram'):
            return '%s+%#x' % (region, addr & 0xFFFFFF)
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and syms[i][0] <= addr < syms[i][1] and syms[i][2] not in ('gEwramHeap', 'gIwramHeap'):
            return syms[i][2]
        if owner:
            return 'task %s' % fns.get(owner, '%08X' % owner)
        if region == 'ewram' and addr < 0x02020080:
            return 'gEwramHeap (no task)'
        if region == 'iwram' and 0x03003A20 <= addr < 0x03003A20 + 0x2604:
            return 'gIwramHeap (no task)'
        if i >= 0:
            return '%s+%#x?' % (syms[i][2], addr - syms[i][0])
        return region

    def mask_of(addr):
        for lo, hi, cls, why in masks:
            if addr + 64 > lo and addr < hi:
                return cls
        return ''

    rows = {}
    for f in a.files:
        rec = os.path.basename(f).rsplit('.', 1)[0]
        for ln in open(f):
            p = ln.split()
            if not p or p[0] != 'block':
                continue
            region, addr = p[1], int(p[2], 16)
            first, last, frames = int(p[3]), int(p[4]), int(p[5])
            seats, unm, owner, obase = int(p[6], 16), int(p[7]), int(p[8], 16), int(p[9], 16)
            bits, fbits = int(p[10], 16), int(p[11], 16)
            n = name_of(region, addr, owner, obase)
            r = rows.setdefault(n, {'lo': addr, 'hi': addr + 64, 'first': {},
                                    'frames': 0, 'seats': 0, 'blocks': 0,
                                    'region': region, 'unm': 0, 'bytes': set(),
                                    'fbytes': {}})
            r['unm'] = max(r['unm'], unm)
            for k in range(64):
                if bits >> k & 1:
                    r['bytes'].add(addr + k)
            r['lo'] = min(r['lo'], addr); r['hi'] = max(r['hi'], addr + 64)
            r['first'][rec] = min(r['first'].get(rec, 1 << 30), first)
            r['frames'] = max(r['frames'], frames)
            r['seats'] |= seats
            r['blocks'] += 1
    order = sorted(rows.items(), key=lambda kv: min(kv[1]['first'].values()))
    print('%-40s %-6s %-19s %6s %6s %-5s %-6s %s' % (
        'symbol', 'region', 'range', 'blocks', 'frames', 'seats', 'mask', 'first divergence'))
    for n, r in order:
        m = 'yes' if r['unm'] == 0 else ('part' if mask_of(r['lo']) else '')
        if a.unmasked and r['unm'] == 0:
            continue
        firsts = ' '.join('%s@%d' % kv for kv in sorted(r['first'].items(), key=lambda kv: kv[1]))
        bs = sorted(r['bytes'])
        span = ('%08X..%08X(%d)' % (bs[0], bs[-1], len(bs))) if bs else ''
        print('%-40s %-6s %08X-%08X %6d %6d %-5X %-6s %s  bytes %s' % (
            n[:40], r['region'], r['lo'], r['hi'], r['blocks'], r['frames'],
            r['seats'], m or '-', firsts, span))
    return 0


if __name__ == '__main__':
    sys.exit(main())
