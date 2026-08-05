#!/usr/bin/env python3
"""
refresh_layout_asserts.py -- recompute the values in platform/port/gba_layout.h.

gen_gba_layout.py is the real generator: it compiles a probe over the whole
tree, reads DWARF, and decides *which* types belong in the table.  It needs a
32-bit hosted toolchain (`gcc -m32`), and on a machine without gcc-multilib --
which cannot be installed without root -- neither `make layout` nor `make
layout-check` runs at all.

This tool does the smaller half, and needs no DWARF and no multilib: it asks
the compiler what each assertion in the committed table actually evaluates to,
and rewrites the ones that have moved.  It cannot add a type or notice one that
dropped out of the tree -- only gen_gba_layout can -- so it is a repair tool,
not a replacement.  Run it when a deliberate structure change has invalidated
known entries, then let the build re-check all of them.

How it reads the values: each assertion is re-asserted against a sentinel it
cannot equal, and the compiler's own diagnostic reports the truth --

    error: static assertion failed ...
    note: expression evaluates to '32 == 2147483633'

which is exact, needs no parsing of the type system, and is measured by the
same compiler that will compile the assertion afterwards.

    tools/refresh_layout_asserts.py [--check] [--cc CC]

--check reports what would change and exits non-zero, for CI.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HDR = Path('platform/port/gba_layout.h')
SENTINEL = 0x7FFFFFF1
ASSERT = re.compile(
    r'^(PORT_LAYOUT_ASSERT\()(.+?)( == )(-?\d+|0x[0-9A-Fa-f]+)(, ".*?"\);)$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--header', default=str(HDR))
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--cc', default=os.path.expanduser(
        os.environ.get('EMSDK', '~/emsdk') + '/upstream/emscripten/emcc'))
    args = ap.parse_args()

    hdr = Path(args.header)
    lines = hdr.read_text().splitlines()

    # The table's own #include block, so the probe sees exactly what it sees.
    includes = [l for l in lines if l.startswith('#include')]

    items = []                       # (line index, expression)
    for i, line in enumerate(lines):
        m = ASSERT.match(line)
        if m:
            items.append((i, m.group(2)))
    if not items:
        sys.exit('%s: no PORT_LAYOUT_ASSERT lines found' % hdr)

    body = ['#include <stddef.h>'] + includes
    for n, (_, expr) in enumerate(items):
        body.append('_Static_assert((%s) == %d, "L%d");' % (expr, SENTINEL, n))

    with tempfile.TemporaryDirectory() as td:
        probe = Path(td) / 'layout_values.c'
        probe.write_text('\n'.join(body) + '\n')
        r = subprocess.run(
            [args.cc, '-std=gnu99', '-fgnu89-inline', '-fno-strict-aliasing',
             '-fwrapv', '-funsigned-char', '-w', '-DPORTABLE', '-DNONMATCHING',
             '-DMODERN=1', '-DPORT_GLOBAL_BASE=167772160u',
             '-Ibuild/port-src/include', '-Ibuild/generated', '-Iplatform',
             '-include', 'port/prelude.h', '-include', 'port/ram_symbols.h',
             '-include', 'port/rom_data.h',
             '-ferror-limit=0', '-fsyntax-only', str(probe)],
            capture_output=True, text=True)

    got = {}
    for idx, val in re.findall(
            r'"L(\d+)".*?\n.*?\n.*?evaluates to \'(-?\d+) == %d\'' % SENTINEL,
            r.stderr):
        got[int(idx)] = int(val)

    if len(got) != len(items):
        missing = len(items) - len(got)
        print('warning: %d of %d assertions produced no value -- the probe may '
              'not have compiled cleanly' % (missing, len(items)),
              file=sys.stderr)
        if not got:
            sys.stderr.write(r.stderr[-3000:])
            return 2

    changed = []
    for n, (i, expr) in enumerate(items):
        if n not in got:
            continue
        m = ASSERT.match(lines[i])
        was = int(m.group(4), 0)
        now = got[n]
        if was != now:
            changed.append((expr, was, now))
            lines[i] = '%s%s%s%d%s' % (m.group(1), m.group(2), m.group(3),
                                       now, m.group(5))

    if not changed:
        print('%s: all %d assertions already match' % (hdr, len(items)))
        return 0

    print('%d of %d assertions have moved:\n' % (len(changed), len(items)))
    for expr, was, now in changed:
        print('  %-64s %d -> %d' % (expr, was, now))

    if args.check:
        print('\n--check: not written')
        return 1

    hdr.write_text('\n'.join(lines) + '\n')
    print('\n%s rewritten. The build re-checks every assertion, so compile '
          'before trusting this.' % hdr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
