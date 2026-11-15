#!/usr/bin/env python3
"""report.py -- the measured tables of ROOMS-REPORT.md.

    python3 tools/rooms/report.py --tmp DIR [--out FILE.md] [--prose PROSE.md]

Reads what tools/rooms/run_all.sh measured under DIR:
  DIR/final/v01/<rec>/bytes.json, frames.json   views 0 vs 1 (measure.mjs)
  DIR/final/v23/<rec>/...                       views 2 vs 3
  DIR/final/v00/<rec>/...                       views 0 vs 0 (negative control)
  DIR/final/nofix/<rec>/...                     views 0 vs 1, spawn-margin fix off
  DIR/review/*.json                             re-view tests (review_test.mjs)
  DIR/rig.json                                  the rig (rig.mjs)
  DIR/gates.txt                                 the build gates' last lines
and writes the measured sections; PROSE (the hand-written analysis) is
spliced in where it says <!-- MEASURED -->."""
import argparse
import glob
import json
import os
import sys
import re
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from symbols import Namer, ROOT  # noqa: E402
from summarize import group  # noqa: E402
from classify import classify  # noqa: E402


def meta_of(d):
    return json.load(open(os.path.join(d, 'bytes.json')))['meta']


def frames_of(d):
    return json.load(open(os.path.join(d, 'frames.json')))


def corpus_table(dirs):
    out = ['| recording | frames | players apart (frames) | room pairs | frames any byte differs | runs | gameplay digest differs | Kirbys/RNG/objects differ | objects at other addresses | task sets differ | peak tasks A/B (of 128) | wall s |',
           '|---|---|---|---|---|---|---|---|---|---|---|---|']
    for d in dirs:
        m = meta_of(d)
        fr = frames_of(d)
        v = m['views']
        apart = sum(1 for r in fr if r['rooms'][v[0]] != r['rooms'][v[1]] and r['rooms'][v[0]] not in (0, 65535)
                    and r['rooms'][v[1]] not in (0, 65535))
        pairs = len({(r['rooms'][v[0]], r['rooms'][v[1]]) for r in fr if r['rooms'][v[0]] != r['rooms'][v[1]]})
        gp = [m['firstKirbyDiffer'], m['firstRngDiffer'], m['firstObjSetDiffer'], m['firstObjOwnerDiffer']]
        gps = 'none' if all(x is None for x in gp) else 'Kirby@%s RNG@%s set@%s owner@%s' % tuple(gp)
        runs = len(json.load(open(os.path.join(d, 'bytes.json')))['runs'])
        out.append('| %s (views %d/%d) | %d | %d | %d | %d | %d | %d/%d | %s | %d | %d | %s | %.1f |' % (
            os.path.basename(d.rstrip('/')), v[0], v[1], m['frames'], apart, pairs, m['framesWithDiff'], runs,
            m.get('framesDigestDiffer', -1), m.get('digestFrames', -1), gps, m.get('framesObjPlaceDiffer', 0),
            m.get('framesTaskDiffer', 0), '/'.join(map(str, m.get('peakTasks', []))), m['wallS']))
    return '\n'.join(out)


def classes(namer, dirs):
    rows = group(namer, dirs, 4)
    by = {}
    tot = {}
    unexplained = []
    for g in rows:
        cls, writer, why = classify(g['sym'], g['field'], g['region'], g['lo'])
        k = (cls, g['sym'], writer, why)
        e = by.setdefault(k, {'bytes': 0, 'fields': set(), 'first': 1 << 30, 'frames': 0, 'lo': g['lo'], 'hi': g['hi'],
                              'recs': set(), 'region': g['region']})
        e['bytes'] += g['bytes']
        e['fields'].add(re.sub(r'\[\d+\]', '[]', g['field'] or '').split('+')[0])
        e['first'] = min(e['first'], min(g['first'].values()))
        e['frames'] = max(e['frames'], g['frames'])
        e['lo'] = min(e['lo'], g['lo'])
        e['hi'] = max(e['hi'], g['hi'])
        e['recs'] |= set(g['first'])
        tot[cls] = tot.get(cls, 0) + g['bytes']
        if cls == 'UNEXPLAINED':
            unexplained.append(g)
    order = {'P': 0, 'V': 1, 'VS': 2, 'H': 3, 'UNEXPLAINED': 4}
    out = ['| class | symbol | range | bytes (sum over recordings) | fields | first frame | most frames one byte differed | writer | why |',
           '|---|---|---|---|---|---|---|---|---|']
    for (cls, sym, writer, why), e in sorted(by.items(), key=lambda kv: (order.get(kv[0][0], 9), kv[1]['lo'])):
        f = sorted(x for x in e['fields'] if x)
        fs = ', '.join(f[:6]) + (' +%d more' % (len(f) - 6) if len(f) > 6 else '')
        rng = '%08X-%08X' % (e['lo'], e['hi']) if not sym.startswith('task ') else 'heap'
        out.append('| %s | %s | %s | %d | %s | %d | %d | %s | %s |' % (cls, sym, rng, e['bytes'], fs or '-', e['first'],
                                                                    e['frames'], writer, why))
    summ = ', '.join('%s %d B' % (k, v) for k, v in sorted(tot.items(), key=lambda kv: order.get(kv[0], 9)))
    return '\n'.join(out), summ, unexplained, tot


def rig_table(path):
    if not os.path.exists(path):
        return '(rig not run)', {}
    R = json.load(open(path))
    runs = {r['name']: r for r in R['runs']}
    out = ['| run | game min | canonical final frames identical to host | personal digest equal to canonical | personal repairs asked / images | personal re-views | screen on player 2 (checks wrong/all) | players apart (frames) | meetings | rooms P1 / P2 | guest C images | step ms p50/p99: guest C, personal | wall s |',
           '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for n, r in runs.items():
        pc = r['personal']['compare']
        gs, ps_ = r['guest']['stepMs'], r['personal']['stepMs']
        steps = '%.2f/%.2f, %.2f/%.2f' % (gs['p50'], gs['p99'], ps_['p50'], ps_['p99'])
        out.append('| %s | %.2f | %d/%d | %d/%d | %d / %d | %d | %d/%d | %d/%d | %d | %d / %d | %d | %s | %.1f |' % (
            n, r['frames']['gameMinutes'], r['compare']['equal'], r['compare']['compared'], pc['equal'], pc['compared'],
            pc['repairsAsked'], len(r['personal']['images']), r['personal']['reviews'], pc['viewWrong'], pc['viewChecks'],
            r['apart']['frames'], r['apart']['total'], r['apart']['meetings'], len(r['apart']['rooms1']), len(r['apart']['rooms2']),
            r['guest']['images'], steps, r['wallSec']))
    return '\n'.join(out), runs


def corrupt_table(runs):
    r = runs.get('corrupt')
    if not r:
        return '(no corruption run)'
    out = ['| injected | instance | detected | detect ms | repaired ms | how |', '|---|---|---|---|---|---|']
    for c in r['corrupt']:
        how = ('gameplay digest vs canonical; host image for P + re-view' if c['personal']
               else 'whole-image hash vs host; host image')
        out.append('| %s | %s | %s | %s | %s | %s |' % (c['what'].replace('personal: ', ''), 'personal' if c['personal'] else 'canonical',
                                                      c['detectMs'] is not None, c['detectMs'], c['repairMs'], how))
    return '\n'.join(out)


def review_table(paths):
    out = ['| recording | re-view at | rooms (P1, P2) then | frames until the picture equals the always-P2 device | frames equal / after | gameplay ever differs |',
           '|---|---|---|---|---|---|']
    for p in sorted(paths):
        d = json.load(open(p))
        o = d['out']
        for r in o['res']:
            out.append('| %s | %d | %s | %s | %d/%d | %s |' % (os.path.basename(o['rec']), r['at'], r['rooms'],
                                                              r['framesToSamePicture'], r['framesSameAfter'], r['framesAfter'],
                                                              'no' if o['gameplayDiffer'] is None else 'at %d' % o['gameplayDiffer']))
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tmp', required=True)
    ap.add_argument('--out')
    ap.add_argument('--prose')
    ap.add_argument('--bin', default=os.path.join(ROOT, 'build', 'lp64', 'katam'))
    a = ap.parse_args()
    T = a.tmp
    namer = Namer(a.bin, os.path.join(T, 'layouts.json'))
    v01 = sorted(glob.glob(os.path.join(T, 'final', 'v01', '*/')))
    v23 = sorted(glob.glob(os.path.join(T, 'final', 'v23', '*/')))
    v00 = sorted(glob.glob(os.path.join(T, 'final', 'v00', '*/')))
    nofix = sorted(glob.glob(os.path.join(T, 'final', 'nofix', '*/')))
    parts = {}
    rigfree = sorted(glob.glob(os.path.join(T, 'final', 'rigfree', '*/')))
    parts['corpus'] = corpus_table(v01 + v23 + rigfree)
    parts['control'] = corpus_table(v00) if v00 else '(not run)'
    parts['nofix'] = corpus_table(nofix) if nofix else '(not run)'
    parts['classes'], parts['class_sum'], unexplained, tot = classes(namer, v01 + v23)
    parts['unexplained'] = '\n'.join('- %s %s %08X+%d first %s' % (g['sym'], g['field'], g['lo'], g['hi'] - g['lo'], g['first'])
                                     for g in unexplained[:40]) or 'none'
    parts['rig'], runs = rig_table(os.path.join(T, 'rig.json'))
    parts['corrupt'] = corrupt_table(runs)
    parts['review'] = review_table(glob.glob(os.path.join(T, 'review', '*.json')))
    gates = os.path.join(T, 'gates.txt')
    parts['gates'] = open(gates).read() if os.path.exists(gates) else '(gates not run)'
    md = []
    md.append('## Measured: the corpus (views 0/1 = player 1\'s and player 2\'s screen; 2/3 = players 3 and 4)\n')
    md.append(parts['corpus'] + '\n')
    md.append('Negative control (both devices on player 1\'s screen):\n\n' + parts['control'] + '\n')
    md.append('Before the one fix (platform/view.c spawn margin, rooms mode reads the room size), same corpus:\n\n' + parts['nofix'] + '\n')
    md.append('## Measured: every differing byte, classified\n')
    md.append('Totals: ' + parts['class_sum'] + '.  Unexplained bytes: ' + str(tot.get('UNEXPLAINED', 0)) + '.\n')
    md.append(parts['classes'] + '\n')
    md.append('Unexplained ranges: ' + parts['unexplained'] + '\n')
    md.append('## Measured: the re-view (a device told it is player 2\'s at frame F, against one that was all along)\n')
    md.append(parts['review'] + '\n')
    md.append('## Measured: the rig (design (c))\n')
    md.append(parts['rig'] + '\n')
    md.append('Corruption:\n\n' + parts['corrupt'] + '\n')
    md.append('## Gates (switch off)\n\n```\n' + parts['gates'] + '\n```\n')
    measured = '\n'.join(md)
    head = '<!-- generated by tools/rooms/run_all.sh on %s; edit tools/rooms/ROOMS-PROSE.md, not this file -->\n' % time.strftime('%Y-%m-%d %H:%M')
    if a.prose and os.path.exists(a.prose):
        prose = open(a.prose).read()
        body = prose.replace('<!-- MEASURED -->', measured)
    else:
        body = measured
    text = head + body
    if a.out:
        open(a.out, 'w').write(text)
    else:
        print(text)
    json.dump({'class_totals': tot, 'unexplained': len(unexplained)}, open(os.path.join(T, 'report-summary.json'), 'w'))


if __name__ == '__main__':
    main()
