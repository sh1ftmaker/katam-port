#!/usr/bin/env python3
"""
check_doc_sizes.py -- hold the tree to the sizes the decompilation documents.

The decomp records the true console size of a structure in a comment on its
closing brace:

    struct Unk_08353510 {
        s16 unk0; s16 unk2; s16 unk4; s16 unk6;
        u8  unk8; u8  unk9;
    }; /* size = 0xC */

That comment is console truth -- it comes from the addresses the structure is
read at -- and the member list is a reconstruction.  When the two disagree, the
port compiles the reconstruction and every `array[i]` and `++ptr` over that type
steps by the wrong stride.

This is not hypothetical.  Unk_08353510 is the warp star's animation script:
documented 0xC, compiled as 10, because four s16 and two u8 come to 10 bytes and
clang aligns the result to 2 while the GBA's compiler rounds a structure's size
up to a multiple of 4 (gcc's -mstructure-size-boundary=32, the ARM default).
`++kirby->unk114` therefore advanced 10 bytes through a table with a 12-byte
stride, the frame counter picked up a fragment of the next entry, went negative,
and the ride animation could never end.  The star reached the last state before
the level change and waited there for a condition that had already been made
unreachable.

Why platform/port/gba_layout.h did not catch it: that header is *generated from
the tree*, so it asserted `sizeof(struct Unk_08353510) == 10` -- it faithfully
recorded the bug.  It catches drift from today's tree, which is worth having,
but it cannot catch a reconstruction that was never right.  This check is the
other direction: the tree against what the decompilation says the console does.

    tools/check_doc_sizes.py [--cc CC] [--cflags FLAGS] [--tree DIR]

Exits non-zero if any documented size disagrees.  Needs no ROM.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

OPEN = re.compile(r'^(struct|union)\s+(\w+)\s*\{')
CLOSE = re.compile(r'^\};\s*/\*\s*size\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*\*/')

# Documented sizes that are known to disagree for a reason that is *not* the
# size boundary, and that no one has established the right answer for yet.
# Listed rather than silently skipped: the point of this check is that a
# mismatch is visible, and an exception with no reason attached is how a real
# one gets lost.
KNOWN = {
    'struct AnimCmd_SetIdAndVariant': (
        'documented 0xC, built 8. Not a size-boundary case -- 8 is already a '
        'multiple of 4. Its own offset comments say cmdId 0x00, animId 0x04, '
        'variant 0x08, but `u16 animId; u16 variant;` puts variant at 0x06, so '
        'the member list is missing two bytes of padding rather than the '
        'structure being rounded. Fixing it moves a member, which changes how '
        'every animation command is read; that needs to be established against '
        'the ROM first, not guessed.'),
}


def documented(inc):
    """Every top-level type whose closing brace carries a size comment.

    Brace depth, not a backwards scan for the nearest `struct X {`.  A nested
    definition --

        struct Outer {
            struct Inner { ... } in;
            ...
        }; /* size = 0xB4 */

    -- puts Inner's opener between Outer's opener and the comment, so scanning
    back finds the wrong name and attributes Outer's size to Inner.  That
    produced entries like "documented 196, built 12" on the first attempt:
    every one of them was this mistake, and taken at face value they would have
    sent someone chasing five structures that are perfectly correct.
    """
    out = {}
    for h in sorted(inc.rglob('*.h')):
        depth = 0
        top = None
        for line in h.read_text(errors='ignore').splitlines():
            # `extern "C" {` and its #ifdef __cplusplus guard wrap the whole
            # header, so counting their braces puts every top-level type at
            # depth 1 and finds nothing at all.  Preprocessor lines are skipped
            # for the same reason -- a brace inside a macro is not scope here.
            if line.startswith('#') or 'extern "C"' in line:
                continue
            m = OPEN.match(line)
            if m and depth == 0:
                top = (m.group(1), m.group(2))
            c = CLOSE.match(line)
            if c and depth == 1 and top:
                out[top] = (int(c.group(1), 0), h.name)
            depth += line.count('{') - line.count('}')
            if depth <= 0:
                depth = 0
                top = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree', default='build/port-src')
    ap.add_argument('--cc', default=os.path.expanduser(
        os.environ.get('EMSDK', '~/emsdk') + '/upstream/emscripten/emcc'))
    ap.add_argument('--cflags', default='')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    tree = Path(args.tree)
    inc = tree / 'include'
    if not inc.is_dir():
        sys.exit('%s: no such tree -- run `make sync` first' % inc)

    doc = documented(inc)
    if not doc:
        sys.exit('no documented sizes found -- has the comment style changed?')

    headers = sorted(p.relative_to(inc).as_posix() for p in inc.rglob('*.h'))
    body = ['#include "%s"' % h for h in headers]
    for (kind, name), (want, _) in sorted(doc.items()):
        body.append('_Static_assert(sizeof(%s %s) == %d, "%s %s|%d");'
                    % (kind, name, want, kind, name, want))

    cflags = args.cflags.split() or [
        '-std=gnu99', '-fgnu89-inline', '-fno-strict-aliasing', '-fwrapv',
        '-funsigned-char', '-w', '-DPORTABLE', '-DNONMATCHING', '-DMODERN=1',
        '-DPORT_GLOBAL_BASE=167772160u',
        '-I%s' % inc, '-Ibuild/generated', '-Iplatform',
        '-include', 'port/prelude.h', '-include', 'port/ram_symbols.h',
        '-include', 'port/rom_data.h']

    with tempfile.TemporaryDirectory() as td:
        probe = Path(td) / 'doc_size_probe.c'
        probe.write_text('\n'.join(body) + '\n')
        # -ferror-limit=0: the default of 20 silently truncates the report,
        # which on a check like this reads as "only 20 things are wrong".
        r = subprocess.run([args.cc] + cflags +
                           ['-ferror-limit=0', '-fsyntax-only', str(probe)],
                           capture_output=True, text=True)

    bad = re.findall(
        r'"((?:struct|union) \w+)\|(\d+)".*?\n.*?\n.*?evaluates to \'(\d+) == \d+\'',
        r.stderr)
    bad = sorted(set(bad))
    known = [b for b in bad if b[0] in KNOWN]
    bad = [b for b in bad if b[0] not in KNOWN]

    print('%d documented sizes checked against %s'
          % (len(doc), Path(args.cc).name))
    for name, want, got in known:
        print('\n  known, unresolved: %s (documented %s, built %s)\n    %s'
              % (name, want, got, KNOWN[name]))
    if not bad:
        if not known:
            print('every documented size matches')
        return 0

    print('\n%d type(s) do not have the size the decompilation documents.\n'
          'Every array index and pointer increment over these steps by the\n'
          'wrong stride -- see the header of this file for what that cost.\n'
          % len(bad))
    for name, want, got in bad:
        want, got = int(want), int(got)
        note = ''
        if (got + 3) & ~3 == want:
            note = '  (built size rounded up to a multiple of 4)'
        print('  %-38s documented %-5d built %-5d%s' % (name, want, got, note))
    print('\nThe usual cause is the GBA compiler rounding a structure size up\n'
          'to a multiple of 4 where clang aligns to the widest member.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
