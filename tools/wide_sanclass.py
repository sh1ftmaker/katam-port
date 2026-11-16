#!/usr/bin/env python3
"""wide_sanclass.py -- classify ASan/UBSan reports from wide-build runs (gate W1).

    tools/wide_sanclass.py LOG...

Every report is one of:
  known      in the default build's own sanitizer baseline (KATAM_MAX_KIRBYS
             unset, same recordings), or marked as original UB in the source;
  owner-ff   gCurLevelInfo[0xFF]: an object nobody owns, which the console
             also indexes with 0xFF (platform/native/mem.c); in the wide build
             it lands in mapped zero memory past the table, as on every host;
  NEW        anything else: an out-of-bounds access the wide build introduced.
Prints the counts and PASS when there are no NEW reports.
"""
import os
import re
import sys

KNOWN = {
    'game/src/save.c:214': 'enum load (default baseline)',
    'game/src/m4a.c:544': 'P32[1] trailing array (default baseline)',
    'game/src/rocky.c:122': 'counter >> 3 past a 16-entry table (default baseline)',
    'game/src/intro.c:3318': 'index -1 (default baseline)',
    'game/src/vert_sliding_door.c:54': 'P32[8] (default baseline)',
    # the same macro (Macro_080A4728, gUnk_02022EC0[0][slot * 8 + n]) in the
    # door's open and close states
    'game/src/vert_sliding_door.c:77': 'P32[8] (as :54, the same macro)',
    'game/src/vert_sliding_door.c:100': 'P32[8] (as :54, the same macro)',
    'game/src/object_collision.c:491': 'gCurLevelInfo[255] (default baseline)',
    'game/src/kirby.c:23868': 'marked "TODO: UB: out-of-bounds read" in the source',
    'game/src/kirby.c:23869': 'marked "TODO: UB: out-of-bounds read" in the source',
    'game/src/kirby.c:23883': 'marked "TODO: UB: out-of-bounds read" in the source',
    'game/src/kirby.c:23884': 'marked "TODO: UB: out-of-bounds read" in the source',
}
loc_re = re.compile(r'(game/src/[A-Za-z0-9_]+\.c:\d+)')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_src = {}


def marked_ub(loc):
    """The source marks the line (or the loop above it) as the console's own
    out-of-bounds read: 'TODO: UB' within the three lines above it.  Keeps the
    known list right when an edit elsewhere in the file moves the lines."""
    path, line = loc.rsplit(':', 1)
    if path not in _src:
        try:
            _src[path] = open(os.path.join(ROOT, path), errors='replace').read().split('\n')
        except OSError:
            _src[path] = []
    n = int(line)
    return any('TODO: UB' in ln for ln in _src[path][max(0, n - 4):n])


def main():
    counts = {'known': 0, 'owner-ff': 0, 'NEW': 0}
    new = {}
    for path in sys.argv[1:]:
        lines = open(path, errors='replace').read().split('\n')
        for i, ln in enumerate(lines):
            if 'runtime error:' in ln:
                m = loc_re.search(ln)
                loc = m.group(1) if m else '?'
                what = ln.split('runtime error:')[1].strip()
            elif 'ERROR: AddressSanitizer' in ln:
                loc = '?'
                for nxt in lines[i + 1:i + 16]:
                    if '#0 ' in nxt:
                        m = loc_re.search(nxt)
                        loc = m.group(1) if m else '?'
                        break
                what = ln.split('ERROR: AddressSanitizer:')[1].split(' on ')[0].strip()
            else:
                continue
            if loc in KNOWN or (loc != '?' and marked_ub(loc)):
                counts['known'] += 1
            elif re.search(r"index 255 out of bounds for type 'LevelInfo \[\d+\]'", what):
                counts['owner-ff'] += 1
            else:
                counts['NEW'] += 1
                new.setdefault('%s: %s' % (loc, what), 0)
                new['%s: %s' % (loc, what)] += 1
    for k, n in sorted(new.items(), key=lambda kv: -kv[1]):
        print('  NEW %4d  %s' % (n, k))
    ok = counts['NEW'] == 0
    print('sanitizer reports: %d known (default baseline / source-marked UB), %d gCurLevelInfo[0xFF], %d NEW -> %s'
          % (counts['known'], counts['owner-ff'], counts['NEW'], 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
