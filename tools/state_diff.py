#!/usr/bin/env python3
"""state_diff.py -- diff two --dump-state / DUMP_STATE checkpoint dumps.

The per-frame hash log (docs/DETERMINISM.md) says *that* two runs differ and
from which frame.  This says *where*, word by word, and classifies each
difference.

Function pointers are no longer one of the classes that differ: the game
stores canonical code IDs (platform/port/fnid.h, netplay-v2 R5) -- the GBA ROM
address, or a name-derived 0x0Exxxxxx -- which are the same bytes in every
build, so a raw native dump and a wasm dump agree on them.  A word that is a
code ID is labelled with its function's name (game/generated/fnid_ids.h).
Dumps from before that change spelled a function as

  native   the code address of the function (this executable's symbol table)
  wasm     an index into the module's function table

and giving each dump the binary it came from still translates those back to
names.  What the binaries are still needed for: pointers into the port's own
static data (the game's C-defined const tables; netplay-v2 R6), which differ
natively vs wasm and between layouts.

    tools/state_diff.py A.bin B.bin [--a-bin build/lp64/katam]
                                    [--b-bin build/katam-node.wasm]

Dump layout (platform/inputlog.c, DumpTo): EWRAM, IWRAM, IO (DMA address
registers zeroed), DMA channels canonical, frame state, PLTT, VRAM, OAM, the
PPU's affine latches, SRAM.
Exit status 0 when nothing but pointer spelling differs.
"""
import argparse
import os
import re
import struct
import subprocess
import sys

REGIONS = [('ewram', 0x02000000, 0x40000), ('iwram', 0x03000000, 0x8000),
           ('io', 0x04000000, 0x400), ('dma', 0, 112), ('frame', 0, 12),
           ('pltt', 0x05000000, 0x400), ('vram', 0x06000000, 0x18000),
           ('oam', 0x07000000, 0x400), ('ppu', 0, 64),
           ('sram', 0x09000000, 0x10000)]


def leb(b, i):
    r = s = 0
    while True:
        x = b[i]; i += 1
        r |= (x & 0x7f) << s; s += 7
        if not x & 0x80:
            return r, i


def wasm_table_names(path):
    """{table index: function name} from the element and name sections."""
    b = open(path, 'rb').read()
    assert b[:4] == b'\0asm'
    i = 8
    n_imports = 0
    elems = {}
    names = {}
    while i < len(b):
        sid = b[i]; i += 1
        size, i = leb(b, i)
        end = i + size
        if sid == 2:                                  # imports
            n, j = leb(b, i)
            for _ in range(n):
                l, j = leb(b, j); j += l
                l, j = leb(b, j); j += l
                kind = b[j]; j += 1
                if kind == 0:
                    _, j = leb(b, j); n_imports += 1
                elif kind == 1:
                    j += 1; fl = b[j]; j += 1; _, j = leb(b, j)
                    if fl & 1: _, j = leb(b, j)
                elif kind == 2:
                    fl = b[j]; j += 1; _, j = leb(b, j)
                    if fl & 1: _, j = leb(b, j)
                elif kind == 3:
                    j += 2
        elif sid == 9:                                # elements
            n, j = leb(b, i)
            for _ in range(n):
                flags, j = leb(b, j)
                if flags not in (0, 2):
                    break
                if flags == 2:
                    _, j = leb(b, j)
                assert b[j] == 0x41; j += 1           # i32.const
                off, j = leb(b, j)                    # small, positive
                assert b[j] == 0x0b; j += 1
                if flags == 2:
                    j += 1                            # elemkind
                cnt, j = leb(b, j)
                for k in range(cnt):
                    f, j = leb(b, j)
                    elems[off + k] = f
        elif sid == 0:                                # custom
            l, j = leb(b, i)
            nm = b[j:j + l]; j += l
            if nm == b'name':
                while j < end:
                    sub = b[j]; j += 1
                    sl, j = leb(b, j)
                    se = j + sl
                    if sub == 1:
                        cnt, j = leb(b, j)
                        for _ in range(cnt):
                            idx, j = leb(b, j)
                            l2, j = leb(b, j)
                            names[idx] = b[j:j + l2].decode('utf-8', 'replace'); j += l2
                    j = se
        i = end
    return {t: names.get(f, 'func%d' % f) for t, f in elems.items()}


def name_hash(s):
    """platform/native/symtab.c's NameHash, without the tag bits."""
    h = 2166136261
    for c in s.encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h & 0x3FFFFFFF


def elf_func_names(path):
    """{word: function name} for a native build.  Both spellings: the raw code
    address (a PORT_HASH_RAW=1 dump) and the token platform/native/symtab.c
    replaces it with in an ordinary one."""
    out = subprocess.run(['nm', path], capture_output=True, text=True).stdout
    m = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwW':
            m.setdefault(int(p[0], 16), p[2])
            m.setdefault(0xC0000000 | name_hash(p[2]), p[2])
    return m


def elf_object_tokens(path):
    out = subprocess.run(['nm', path], capture_output=True, text=True).stdout
    return {0x80000000 | name_hash(l.split()[2]): l.split()[2]
            for l in out.splitlines()
            if len(l.split()) == 3 and l.split()[1] in 'bBdDrRgGsS'}


def namer(path):
    if path is None:
        return {}
    if path.endswith('.wasm'):
        return wasm_table_names(path)
    return elf_func_names(path)


def elf_data_range(path):
    """[lo, hi) of this executable's image, for telling a stored pointer into
    the port's own static data (the game's C-defined const tables live there)
    from an integer."""
    out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
    addrs = [int(l.split()[0], 16) for l in out.splitlines()
             if len(l.split()) == 3 and int(l.split()[0], 16) >= 0x10000000]
    return (min(addrs), max(addrs) + 1) if addrs else (0, 0)


def data_test(path):
    """A predicate: is this word a pointer into the port's own static data?"""
    if path is None:
        return lambda w: False
    if path.endswith('.wasm'):
        return lambda w: 0x0A000000 <= w < 0x0C000000   # GLOBAL_BASE..INITIAL_MEMORY
    lo, hi = elf_data_range(path)
    toks = elf_object_tokens(path)
    return lambda w: lo <= w < hi or w in toks


def code_ids():
    """{code ID: function name}, from the generated table."""
    p = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     'game', 'generated', 'fnid_ids.h')
    try:
        t = open(p).read()
    except OSError:
        return {}
    return {int(v, 16): n for n, v in re.findall(r'#define FNID_(\w+) 0x([0-9A-Fa-f]+)u', t)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('b')
    ap.add_argument('--a-bin'); ap.add_argument('--b-bin')
    ap.add_argument('--show', type=int, default=30)
    args = ap.parse_args()
    A = open(args.a, 'rb').read(); B = open(args.b, 'rb').read()
    na, nb = namer(args.a_bin), namer(args.b_bin)
    ids = code_ids()
    da, db = data_test(args.a_bin), data_test(args.b_bin)
    off = 0
    fnptr = folded = dataptr = 0
    real = []
    for name, base, size in REGIONS:
        for i in range(0, size, 4):
            x = struct.unpack_from('<I', A, off + i)[0]
            y = struct.unpack_from('<I', B, off + i)[0]
            if x == y:
                continue
            fx, fy = na.get(x), nb.get(y)
            if name in ('ewram', 'iwram') and fx is not None and fy is not None:
                if fx == fy:
                    fnptr += 1
                else:
                    # Both are function pointers but the names differ: the
                    # linker folded identical bodies (nullsub_103 and
                    # nullsub_12 are both `{}`) and one table slot or one
                    # address serves several names.
                    folded += 1
                continue
            if name in ('ewram', 'iwram') and da(x) and db(y):
                dataptr += 1
                continue
            real.append((name, base + i if base else i, x, y, fx, fy))
        off += size
    print('function pointers spelled differently (same function): %d' % fnptr)
    print('function pointers to identical-code-folded functions: %d' % folded)
    print("pointers into the port's own static data: %d" % dataptr)
    print('real differences: %d words' % len(real))
    by = {}
    for r in real:
        by[r[0]] = by.get(r[0], 0) + 1
    for k, v in by.items():
        print('  %-6s %d' % (k, v))
    for r in real[:args.show]:
        print('  %-6s %08X  %08X %s   %08X %s' % (
            r[0], r[1], r[2], r[4] or ('=' + ids[r[2]] if r[2] in ids else ''),
            r[3], r[5] or ('=' + ids[r[3]] if r[3] in ids else '')))
    return 1 if real else 0


if __name__ == '__main__':
    sys.exit(main())
