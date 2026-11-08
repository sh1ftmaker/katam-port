#!/usr/bin/env python3
"""
stamp_build.py -- give each build its own URLs, so a browser cannot serve a
stale one.

The loader and the wasm keep the same filenames from one build to the next.
Combined with a long cache lifetime that is a trap: a browser that has fetched
`katam.wasm` once will keep using it, and on a phone -- where clearing the
cache is a chore -- a fresh deployment looks exactly like the old one still
crashing in the old place.  That cost a round of "it is still broken" that was
really "you are still running last week's binary".

So the published page references `katam.js?v=<id>` and `katam.wasm?v=<id>`,
where the id is the hash of the wasm itself.  Rebuild without changing the
code and the URLs stay put; change one byte and every client fetches again.
The query string leaves the filenames alone, which keeps emscripten's own
loader logic working.
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True, type=Path,
                    help='directory holding index.html, katam.js and katam.wasm')
    ap.add_argument('--label', default='',
                    help='commit and date, shown on the page (#buildlabel)')
    ap.add_argument('--sim-src', action='append', default=[], type=Path,
                    help='a directory of simulation sources for simVersion (repeatable)')
    ap.add_argument('--sim-flags', default='',
                    help='the compiler flags the simulation is built with')
    args = ap.parse_args()

    page = args.dir / 'index.html'
    loader = args.dir / 'katam.js'
    wasm = args.dir / 'katam.wasm'
    for f in (page, loader, wasm):
        if not f.exists():
            sys.exit('stamp_build: %s is missing' % f)

    build_id = hashlib.sha256(wasm.read_bytes()).hexdigest()[:12]
    # The loader gets its own hash, not the wasm's: EM_JS bodies live in the
    # glue, so a C change can alter katam.js while leaving katam.wasm
    # byte-identical -- stamped with the wasm's id, exactly that change
    # shipped under an unchanged URL and sat out the cache lifetime.
    loader_id = hashlib.sha256(loader.read_bytes()).hexdigest()[:12]

    html = page.read_text()

    # emcc minifies the shell, so the tag can be `src=katam.js`, `src='katam.js'`
    # or `src="katam.js"` -- match all three and keep the original quoting.
    tag = re.compile(r'<script\b[^>]*\bsrc=(?P<q>["\']?)katam\.js(?P=q)[^>]*>')
    m = tag.search(html)
    if not m:
        sys.exit('stamp_build: no <script src=katam.js> in index.html')

    stamped = re.sub(r'(katam\.js)', r'\1?v=%s' % loader_id, m.group(0))

    # locateFile has to be set on the Module the shell already built -- the
    # shell's `var Module = {...}` runs earlier in the page and would discard
    # anything assigned before it.  Injecting immediately ahead of the loader
    # tag is the only point that is after the shell and before the fetch.
    shim = ('<script>Module.locateFile=function(p){'
            'return p==="katam.wasm"?p+"?v=%s":p;};</script>' % build_id)

    html = html[:m.start()] + shim + stamped + html[m.end():]

    # The build a tester is on, readable on the page and by curl.
    label = (args.label + ' ' if args.label else '') + 'wasm ' + build_id
    html = re.sub(r'(<span id=(["\']?)buildlabel\2>)[^<]*(</span>)',
                  lambda mm: mm.group(1) + label + mm.group(3), html, count=1)
    html = html.replace('<head>', '<head><meta name="katam-build" content="%s">' % label, 1)

    # simVersion, the netplay version gate (netplay/session.mjs HELLO, and
    # docs/design/NETPLAY-V2.md §8): a hash of the simulation sources, the
    # flags and the boot script, so two builds of one commit agree -- plus,
    # for as long as world syncs move same-build snapshots (host function
    # pointers inside, R5), the wasm's own id.  Drop the second half when
    # netsim's portable snapshots (M2) land.
    sim = hashlib.sha256()
    for d in args.sim_src:
        for f in sorted(d.rglob('*')):
            if f.is_file() and f.suffix in ('.c', '.h', '.cpp', '.inc', '.s'):
                sim.update(str(f.relative_to(d)).encode() + b'\0')
                sim.update(f.read_bytes())
    sim.update(args.sim_flags.encode())
    for name in ('rb_boot.js', 'net_game.js'):
        f = args.dir / name
        if f.exists():
            sim.update(f.read_bytes())
    sim_version = 's' + sim.hexdigest()[:16] + '.w' + build_id[:8]
    html = html.replace('<head>', '<head><meta name="katam-sim" content="%s">'
                        '<script>window.KATAM_SIM=%r;</script>' % (sim_version, sim_version), 1)

    # The netplay scripts are separate files with the same trap -- a stale
    # cached mp_net.js against a fresh page throws on the first API the old
    # copy lacks, and the whole netplay block dies looking like "the lobby
    # cannot connect".  Same cure: content-hashed URLs, per file, so an
    # unchanged file keeps its cache and a changed one busts it.
    for name in ('mp_net.js', 'rb_net.js', 'rb_boot.js', 'net_session.js', 'net_game.js',
                 'lobby.js'):
        f = args.dir / name
        if not f.exists():
            continue
        fid = hashlib.sha256(f.read_bytes()).hexdigest()[:12]
        html = re.sub(
            r'(<script\b[^>]*\bsrc=(["\']?)%s)(\2[^>]*>)' % re.escape(name),
            r'\1?v=%s\3' % fid, html)

    page.write_text(html)
    print('  STAMP   build %s, sim %s' % (build_id, sim_version))
    return 0


if __name__ == '__main__':
    sys.exit(main())
