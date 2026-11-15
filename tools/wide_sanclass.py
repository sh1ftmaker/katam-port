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
import re
import sys

KNOWN = {
    'game/src/save.c:214': 'enum load (default baseline)',
    'game/src/m4a.c:544': 'P32[1] trailing array (default baseline)',
    'game/src/rocky.c:122': 'counter >> 3 past a 16-entry table (default baseline)',
    'game/src/intro.c:3318': 'index -1 (default baseline)',
    'game/src/vert_sliding_door.c:54': 'P32[8] (default baseline)',
    'game/src/object_collision.c:491': 'gCurLevelInfo[255] (default baseline)',
    'game/src/kirby.c:23859': 'marked "TODO: UB: out-of-bounds read" in the source',
    'game/src/kirby.c:23860': 'marked "TODO: UB: out-of-bounds read" in the source',
    'game/src/kirby.c:23874': 'marked "TODO: UB: out-of-bounds read" in the source',
    'game/src/kirby.c:23875': 'marked "TODO: UB: out-of-bounds read" in the source',
}
loc_re = re.compile(r'(game/src/[a-z0-9_]+\.c:\d+)')


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
                for nxt in lines[i + 1:i + 6]:
                    if '#0 ' in nxt:
                        m = loc_re.search(nxt)
                        loc = m.group(1) if m else '?'
                        break
                what = ln.split('ERROR: AddressSanitizer:')[1].split(' on ')[0].strip()
            else:
                continue
            if loc in KNOWN:
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
