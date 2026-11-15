#!/usr/bin/env python3
"""summarize.py -- name the differing runs of tools/rooms/measure.mjs.

    python3 tools/rooms/summarize.py DIR/<rec> [...] [--bin build/lp64/katam]
        [--depth 2] [--before F] [--json OUT]

Groups every differing byte by symbol and field (to --depth levels), with
the earliest first frame, the latest last frame and the most frames any byte
of it differed, ordered by first difference.  --before F keeps only bytes
that first differed before frame F (to see what leads a cascade)."""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from symbols import Namer, ROOT  # noqa: E402


def owners_of(run):
    best = None
    for k, n in run.get('owners', {}).items():
        m, b = k.split('@')
        if best is None or n > best[2]:
            best = (int(m, 16), int(b, 16), n)
    return (best[0], best[1]) if best else None


def group(namer, dirs, depth, before=None):
    rows = {}
    for d in dirs:
        rec = os.path.basename(d.rstrip('/'))
        data = json.load(open(os.path.join(d, 'bytes.json')))
        for r in data['runs']:
            if before is not None and r['first'] >= before:
                continue
            own = owners_of(r)
            # name each byte (runs are short), group by symbol + field prefix
            for a in range(r['lo'], r['hi']):
                sym, fld = namer.name(r['region'], a, own)
                parts = fld.replace('[', '.[').split('.')
                key = (sym, '.'.join(parts[:depth]).replace('.[', '['))
                g = rows.setdefault(key, {'sym': sym, 'field': key[1], 'region': r['region'],
                                          'lo': a, 'hi': a + 1, 'bytes': 0, 'first': {}, 'last': {},
                                          'frames': 0})
                g['lo'] = min(g['lo'], a)
                g['hi'] = max(g['hi'], a + 1)
                g['bytes'] += 1
                g['first'][rec] = min(g['first'].get(rec, 1 << 30), r['first'])
                g['last'][rec] = max(g['last'].get(rec, -1), r['last'])
                g['frames'] = max(g['frames'], r['framesMax'])
    return sorted(rows.values(), key=lambda g: min(g['first'].values()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--bin', default=os.path.join(ROOT, 'build', 'lp64', 'katam'))
    ap.add_argument('--cache', default='/bigdata/agent-tom/projects/katamport/netproof-eval/rooms-tmp/layouts.json')
    ap.add_argument('--depth', type=int, default=2)
    ap.add_argument('--before', type=int)
    ap.add_argument('--json')
    a = ap.parse_args()
    namer = Namer(a.bin, a.cache)
    rows = group(namer, a.dirs, a.depth, a.before)
    for g in rows:
        print('%-28s %-34s %-6s %08X+%-5d %5dB first %-22s frames %d' % (
            g['sym'][:28], g['field'][:34], g['region'], g['lo'], g['hi'] - g['lo'], g['bytes'],
            ' '.join('%s@%d' % kv for kv in sorted(g['first'].items(), key=lambda kv: kv[1]))[:22],
            g['frames']))
    if a.json:
        json.dump(rows, open(a.json, 'w'), indent=0)


if __name__ == '__main__':
    main()
