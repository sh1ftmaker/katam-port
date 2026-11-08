#!/usr/bin/env python3
"""
symbolize_diff.py -- turn a RAM diff into a list of named globals.

`DIFF=a:b node tools/headless_test.js ...` reports which bytes of EWRAM and
IWRAM changed between two frames.  That is the right measurement and the wrong
output: "0x02020F40 changed" is not evidence about anything.  This attaches
names to it.

Two sources of names, and the difference between them matters:

  game/generated/port/ram_symbols.h   184 globals the GBA build's linker
                                       placed at fixed addresses.  Exact, and
                                       generated from linker.ld, so it cannot
                                       drift from what the game actually uses.

  the decompilation's linker.ld        the same list plus everything the port
                                       does not happen to reference.

A run that lands inside a known symbol is reported as `gKirbys+96`.  A run that
lands between two symbols is reported as an offset from the one before it, with
the gap size, because a global the port never references is invisible to the
first source and common in the second.

**An unnamed run is a finding, not a failure.**  It means something moved in a
region no symbol covers, which is exactly where undocumented state lives.

Usage:
    python3 tools/symbolize_diff.py build/ram-diff.json
    python3 tools/symbolize_diff.py build/ram-diff.json --min 4 --top 40
"""

import argparse
import json
import re
from pathlib import Path

RE_SYM = re.compile(r'#define\s+(\w+)\s+\(\*\([^)]*\)(0x[0-9A-Fa-f]+)\)')
RE_LD = re.compile(r'\.\s*=\s*(0x[0-9A-Fa-f]+)\s*;\s*(\w+)\s*=\s*\.\s*;')

# linker.ld places EWRAM and IWRAM symbols as offsets from each region's base.
LD_REGIONS = ((0x02000000, 0x02040000), (0x03000000, 0x03008000))


def load_symbols(port_header, linker_script):
    """-> sorted [(addr, name, source)].

    Both sources are read and merged rather than one preferred, because they
    fail in opposite directions: the header is exact but only covers what the
    port references, and the linker script covers everything but states
    addresses as region offsets that have to be reconstructed."""
    syms = {}

    if port_header and Path(port_header).exists():
        for line in Path(port_header).read_text().splitlines():
            m = RE_SYM.search(line)
            if m:
                syms[int(m.group(2), 16)] = (m.group(1), 'port')

    if linker_script and Path(linker_script).exists():
        for m in RE_LD.finditer(Path(linker_script).read_text()):
            off, name = int(m.group(1), 16), m.group(2)
            # A bare offset like 0x00020EE0 is relative to whichever region it
            # is declared in; try each and keep the one that lands in range.
            for base, end in LD_REGIONS:
                addr = base + off
                if base <= addr < end and addr not in syms:
                    syms[addr] = (name, 'linker.ld')
                    break

    return sorted((a, n, s) for a, (n, s) in syms.items())


def locate(syms, addr):
    """Nearest symbol at or before addr -> (name, offset, gap_to_next)."""
    lo, hi = 0, len(syms) - 1
    best = -1
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best, lo = mid, mid + 1
        else:
            hi = mid - 1
    if best < 0:
        return None, 0, 0
    a, name, _ = syms[best]
    nxt = syms[best + 1][0] if best + 1 < len(syms) else None
    return name, addr - a, (nxt - a) if nxt else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('diff', help='the DIFF_OUT json')
    ap.add_argument('--minus', metavar='BASELINE',
                    help='a second diff from an identical run in which the '
                         'thing under test did NOT happen. Runs that appear '
                         'in both are dropped. This is what separates "the '
                         'game was running" from "the game did the thing".')
    ap.add_argument('--symbols',
                    default='game/generated/port/ram_symbols.h')
    ap.add_argument('--linker',
                    default=str(Path.home() / 'Desktop/katam/linker.ld'))
    ap.add_argument('--min', type=int, default=1,
                    help='ignore runs shorter than this many bytes')
    ap.add_argument('--top', type=int, default=30)
    args = ap.parse_args()

    d = json.loads(Path(args.diff).read_text())
    syms = load_symbols(args.symbols, args.linker)
    if not syms:
        print('no symbols loaded -- addresses will be bare')

    runs = [r for r in d['runs'] if r['length'] >= args.min]

    dropped = 0
    if args.minus:
        # Anything that moved in the control run moved because the game was
        # running, not because of the thing under test.  Matching on address
        # alone is deliberately strict: a counter that ticks in both runs is
        # noise even when it ticks to different values, and keeping it because
        # the bytes differ would defeat the point of having a control.
        base = json.loads(Path(args.minus).read_text())
        noisy = {r['addr'] for r in base['runs']}
        keep = [r for r in runs if r['addr'] not in noisy]
        dropped = len(runs) - len(keep)
        runs = keep

    print('frames %d..%d: %d bytes changed in %d runs (%d at >= %d bytes)'
          % (d['frameA'], d['frameB'], d['changedBytes'], d['runCount'],
             len(runs) + dropped, args.min))
    if args.minus:
        print('minus %s: %d runs also moved in the control and were dropped, '
              '%d left' % (args.minus, dropped, len(runs)))
    print('%d symbols loaded\n' % len(syms))

    named = unnamed = 0
    print('%-34s %6s  %s' % ('where', 'bytes', 'before -> after'))
    print('-' * 78)
    for r in runs[:args.top]:
        name, off, span = locate(syms, r['addr'])
        # "Inside" means the run starts before the next symbol does.  Without
        # that check every address in RAM belongs to the last symbol below it,
        # including the ones in genuinely uncovered gaps.
        #
        # The last symbol in each region has no next symbol to bound it, so it
        # gets a fixed cap instead.  Without one, everything above the highest
        # symbol is attributed to it -- which is how this tool first reported
        # `iwram_end+103858`, a name for an address a hundred kilobytes past
        # the end of IWRAM.
        LAST_SYMBOL_CAP = 4096
        if name and (off < span if span else off < LAST_SYMBOL_CAP):
            where = '%s+%d' % (name, off) if off else name
            named += 1
        else:
            where = '0x%08X (after %s+%d)' % (r['addr'], name or '?', off)
            unnamed += 1
        print('%-34s %6d  %s -> %s'
              % (where[:34], r['length'], r['before'][:16], r['after'][:16]))

    if len(runs) > args.top:
        print('... and %d more runs' % (len(runs) - args.top))
    print('\n%d runs inside a known symbol, %d in uncovered memory'
          % (named, unnamed))
    if unnamed:
        print('The uncovered ones are where undocumented state lives; they are '
              'the interesting half.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
