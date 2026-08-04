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
    args = ap.parse_args()

    page = args.dir / 'index.html'
    loader = args.dir / 'katam.js'
    wasm = args.dir / 'katam.wasm'
    for f in (page, loader, wasm):
        if not f.exists():
            sys.exit('stamp_build: %s is missing' % f)

    build_id = hashlib.sha256(wasm.read_bytes()).hexdigest()[:12]

    html = page.read_text()

    # emcc minifies the shell, so the tag can be `src=katam.js`, `src='katam.js'`
    # or `src="katam.js"` -- match all three and keep the original quoting.
    tag = re.compile(r'<script\b[^>]*\bsrc=(?P<q>["\']?)katam\.js(?P=q)[^>]*>')
    m = tag.search(html)
    if not m:
        sys.exit('stamp_build: no <script src=katam.js> in index.html')

    stamped = re.sub(r'(katam\.js)', r'\1?v=%s' % build_id, m.group(0))

    # locateFile has to be set on the Module the shell already built -- the
    # shell's `var Module = {...}` runs earlier in the page and would discard
    # anything assigned before it.  Injecting immediately ahead of the loader
    # tag is the only point that is after the shell and before the fetch.
    shim = ('<script>Module.locateFile=function(p){'
            'return p==="katam.wasm"?p+"?v=%s":p;};</script>' % build_id)

    html = html[:m.start()] + shim + stamped + html[m.end():]
    page.write_text(html)
    print('  STAMP   build %s' % build_id)
    return 0


if __name__ == '__main__':
    sys.exit(main())
