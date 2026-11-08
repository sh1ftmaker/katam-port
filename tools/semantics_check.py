#!/usr/bin/env python3
"""
semantics_check.py -- validate the semantics registry (assets/schema/semantics).

Checks the shape of every record file against docs/design/semantics/README.md
and, for every code citation {"loc": "path:line", "code": "..."}, that the
quoted code really is on (or within +-WINDOW lines of) the cited line.  With
--fix, citations whose code moved are re-pointed at the nearest occurrence.

    python3 tools/semantics_check.py [--fix] [--coverage] [--quiet]

Exit status 0 when everything checks, 1 otherwise.  No ROM is read.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REG = ROOT / 'assets' / 'schema' / 'semantics'
FORMAT = 'katam-semantics/1'
WINDOW = 3

CONFIDENCE = {'confirmed', 'likely', 'guess', 'unknown'}
USAGE = {'live', 'code_only', 'unused', 'padding'}
SCALAR = {'u8': 1, 's8': 1, 'u16': 2, 's16': 2, 'u32': 4, 's32': 4, 'ptr': 4}
FIELD_KEYS = {'field', 'offset', 'type', 'name', 'description', 'unit', 'range',
              'default', 'sentinel', 'enum', 'bits', 'packed', 'usage',
              'confidence', 'evidence', 'stats', 'editor', 'notes', 'undecoded', 'aliases'}

_norm = re.compile(r'\s+')
_files = {}


def norm(s):
    return _norm.sub('', s)


def lines_of(rel):
    if rel not in _files:
        p = ROOT / rel
        _files[rel] = p.read_text(errors='replace').split('\n') if p.is_file() else None
    return _files[rel]


def type_size(t):
    m = re.fullmatch(r'(\w+)\[(\d+)\]', t)
    if m:
        return SCALAR.get(m.group(1), 0) * int(m.group(2))
    return SCALAR.get(t)


class Checker:
    def __init__(self, fix):
        self.fix = fix
        self.errors = []
        self.fixed = 0
        self.cites = 0

    def err(self, where, msg):
        self.errors.append('%s: %s' % (where, msg))

    # -- evidence ---------------------------------------------------------
    def evidence(self, where, ev):
        if not isinstance(ev, list):
            self.err(where, 'evidence must be a list')
            return
        for i, e in enumerate(ev):
            w = '%s.evidence[%d]' % (where, i)
            if not isinstance(e, dict):
                self.err(w, 'not an object')
                continue
            if 'loc' in e:
                self.cite(w, e)
            elif not any(k in e for k in ('data', 'runtime', 'doc')):
                self.err(w, 'needs one of loc/data/runtime/doc')

    def cite(self, w, e):
        self.cites += 1
        m = re.fullmatch(r'([^:]+):(\d+)', e['loc'])
        if not m:
            self.err(w, 'bad loc %r (want path:line)' % e['loc'])
            return
        rel, line = m.group(1), int(m.group(2))
        code = e.get('code')
        if not code:
            self.err(w, 'loc %s has no "code" quote' % e['loc'])
            return
        ls = lines_of(rel)
        if ls is None:
            self.err(w, 'no such file %s' % rel)
            return
        needle = norm(code)
        lo, hi = max(1, line - WINDOW), min(len(ls), line + WINDOW)
        if any(needle in norm(ls[n - 1]) for n in range(lo, hi + 1)):
            return
        # also accept a quote that spans two adjacent lines
        if any(needle in norm(ls[n - 1] + ls[n]) for n in range(lo, min(hi, len(ls) - 1) + 1)):
            return
        hits = [n for n in range(1, len(ls) + 1) if needle in norm(ls[n - 1])]
        if not hits:
            self.err(w, '%s: quote %r not found anywhere in the file' % (e['loc'], code))
            return
        best = min(hits, key=lambda n: abs(n - line))
        if self.fix:
            e['loc'] = '%s:%d' % (rel, best)
            self.fixed += 1
        else:
            self.err(w, '%s: quote %r is at line %d now (%d hit(s)); --fix' % (
                e['loc'], code, best, len(hits)))

    # -- fields -------------------------------------------------------------
    def common(self, w, f, need_conf=True):
        c = f.get('confidence')
        if need_conf and c not in CONFIDENCE:
            self.err(w, 'confidence %r not in %s' % (c, sorted(CONFIDENCE)))
        if 'usage' in f and f['usage'] not in USAGE:
            self.err(w, 'usage %r not in %s' % (f['usage'], sorted(USAGE)))
        if c in ('confirmed', 'likely') and not f.get('evidence'):
            self.err(w, '%s without evidence' % c)
        if 'evidence' in f:
            self.evidence(w, f['evidence'])
        if 'enum' in f:
            if not isinstance(f['enum'], dict):
                self.err(w, 'enum must be an object')
            else:
                en = [v.get('name') for v in f['enum'].values() if isinstance(v, dict)]
                if len(en) != len(set(en)):
                    self.err(w, 'enum names are not unique')
                for k, v in f['enum'].items():
                    if not re.fullmatch(r'-?\d+', k):
                        self.err(w, 'enum key %r is not a decimal integer' % k)
                    if not isinstance(v, dict) or 'name' not in v:
                        self.err(w, 'enum[%s] needs a name' % k)
                    elif 'evidence' in v:
                        self.evidence('%s.enum[%s]' % (w, k), v['evidence'])
        for key in ('bits', 'packed'):
            for i, b in enumerate(f.get(key, [])):
                bw = '%s.%s[%d]' % (w, key, i)
                if not re.fullmatch(r'0x[0-9A-Fa-f]+', str(b.get('mask', ''))):
                    self.err(bw, 'mask must be a hex string')
                self.common(bw, b, need_conf=True)

    def unique_names(self, w, entries):
        names = {}
        for f in entries:
            for n in [f.get('name')] + list(f.get('aliases', [])):
                if n:
                    if n in names and names[n] != f.get('field'):
                        self.err(w, 'name %r used by both %s and %s' % (n, names[n], f.get('field')))
                    names[n] = f.get('field')

    def field(self, w, f, size, seen):
        unknown = set(f) - FIELD_KEYS
        if unknown:
            self.err(w, 'unknown keys %s' % sorted(unknown))
        for k in ('field', 'offset', 'type'):
            if k not in f:
                self.err(w, 'missing %s' % k)
                return
        ts = type_size(f['type'])
        if ts is None:
            self.err(w, 'bad type %r' % f['type'])
        elif size is not None and f['offset'] + ts > size:
            self.err(w, 'offset %d + %d overruns size %d' % (f['offset'], ts, size))
        if f['field'] in seen:
            self.err(w, 'duplicate field %s' % f['field'])
        seen.add(f['field'])
        self.common(w, f)

    def record(self, w, r):
        for k in ('record', 'fields'):
            if k not in r:
                self.err(w, 'missing %s' % k)
                return
        w = '%s[%s]' % (w, r['record'])
        seen = set()
        for i, f in enumerate(r['fields']):
            self.field('%s.%s' % (w, f.get('field', i)), f, r.get('size'), seen)
        self.unique_names(w, r['fields'])
        if 'evidence' in r:
            self.evidence(w, r['evidence'])

    def params(self, w, doc):
        for t, d in doc.get('types', {}).items():
            tw = '%s.types[%s]' % (w, t)
            if not re.fullmatch(r'0x[0-9A-F]{2}', t):
                self.err(tw, 'type key must be 0xNN (upper-case hex)')
            if 'ctor' in d and 'loc' in d['ctor']:
                self.cite(tw + '.ctor', d['ctor'])
            if 'evidence' in d:
                self.evidence(tw, d['evidence'])
            seen = set()
            for p in d.get('params', []):
                pw = '%s.%s' % (tw, p.get('field'))
                if 'field' not in p:
                    self.err(pw, 'param without field')
                    continue
                if p['field'] in seen:
                    self.err(pw, 'duplicate param')
                seen.add(p['field'])
                self.common(pw, p)
            self.unique_names(tw, d.get('params', []))

    def inventory(self, w, doc):
        for i, it in enumerate(doc.get('items', [])):
            iw = '%s.items[%d]' % (w, i)
            if 'evidence' in it:
                self.evidence(iw, it['evidence'])
            for j, loc in enumerate(it.get('locations', [])):
                if 'loc' in loc:
                    self.cite('%s.locations[%d]' % (iw, j), loc)

    def file(self, path):
        w = path.name
        try:
            doc = json.loads(path.read_text())
        except ValueError as e:
            self.err(w, 'invalid JSON: %s' % e)
            return None
        if doc.get('format') != FORMAT:
            self.err(w, 'format must be %r' % FORMAT)
        if 'types' in doc:
            self.params(w, doc)
        if 'items' in doc:
            self.inventory(w, doc)
        recs = doc.get('records') or ([doc] if 'fields' in doc else [])
        for r in recs:
            self.record(w, r)
        if 'values' in doc:
            vn = [v.get('name') for v in doc['values'].values()]
            if len(vn) != len(set(vn)):
                self.err(w, 'values names are not unique')
            for k, v in doc['values'].items():
                if 'evidence' in v:
                    self.evidence('%s.values[%s]' % (w, k), v['evidence'])
        return doc


def iter_fields(doc):
    recs = doc.get('records') or ([doc] if 'fields' in doc else [])
    for r in recs:
        for f in r['fields']:
            yield r['record'], f
            for key in ('bits', 'packed'):
                for b in f.get(key, []):
                    yield '%s.%s/%s' % (r['record'], f['field'], key), b
    for t, d in doc.get('types', {}).items():
        for p in d.get('params', []):
            yield 'object_params[%s %s]' % (t, d.get('const', '')), p
            for key in ('bits', 'packed'):
                for b in p.get(key, []):
                    yield 'object_params/%s' % key, b


def coverage(docs):
    tab = {}
    for doc in docs:
        for rec, f in iter_fields(doc):
            if rec.startswith('object_params['):
                rec = 'object_params'
            row = tab.setdefault(rec, {'total': 0, 'confirmed': 0, 'likely': 0, 'guess': 0, 'unknown': 0})
            row['total'] += 1
            row[f.get('confidence', 'unknown')] += 1
    print('%-32s %6s %9s %6s %6s %7s' % ('record', 'fields', 'confirmed', 'likely', 'guess', 'unknown'))
    for rec, r in tab.items():
        print('%-32s %6d %9d %6d %6d %7d' % (rec, r['total'], r['confirmed'], r['likely'], r['guess'], r['unknown']))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--fix', action='store_true', help='re-point drifted citations')
    ap.add_argument('--coverage', action='store_true', help='print per-record confidence counts')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('files', nargs='*', help='check these files instead of the whole registry')
    args = ap.parse_args()
    ck = Checker(args.fix)
    docs = []
    for path in ([Path(f) for f in args.files] or sorted(REG.glob('*.json'))):
        before = ck.fixed
        doc = ck.file(path)
        if doc is None:
            continue
        docs.append(doc)
        if args.fix and ck.fixed > before:
            path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + '\n')
    for e in ck.errors:
        print('error:', e)
    if not args.quiet:
        print('%d file(s), %d citation(s), %d error(s)%s' % (
            len(docs), ck.cites, len(ck.errors), ', %d fixed' % ck.fixed if args.fix else ''))
    if args.coverage:
        coverage(docs)
    return 1 if ck.errors else 0


if __name__ == '__main__':
    sys.exit(main())
