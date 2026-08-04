#!/usr/bin/env python3
"""
resolve_fnptr.py -- turn a function-pointer value into a name and a signature.

In WebAssembly a function pointer is an index into the module's table, so a
crash dump full of values like `dtor=0x918` says nothing on its own.  Every
piece of information needed to decode it is already in the .wasm: the element
section maps table slots to function indices, the type and function sections
give each function its signature, and the name section (present whenever the
build uses --profiling-funcs or -g) gives it a name.

    tools/resolve_fnptr.py build/katam-dbg.wasm 0x918 0x917

This is the missing half of every crash report the port produces.  Given
`call_indirect to a signature that does not match`, it answers both halves of
the question: what was called, and what shape is it really.
"""

import argparse
import sys
from pathlib import Path

WASM_TYPES = {0x7F: 'i32', 0x7E: 'i64', 0x7D: 'f32', 0x7C: 'f64'}


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def byte(self):
        b = self.d[self.p]
        self.p += 1
        return b

    def uleb(self):
        result = shift = 0
        while True:
            b = self.byte()
            result |= (b & 0x7F) << shift
            if not (b & 0x80):
                return result
            shift += 7

    def sleb(self):
        result = shift = 0
        while True:
            b = self.byte()
            result |= (b & 0x7F) << shift
            shift += 7
            if not (b & 0x80):
                if b & 0x40:
                    result -= 1 << shift
                return result

    def bytes(self, n):
        out = self.d[self.p:self.p + n]
        self.p += n
        return out

    def name(self):
        return self.bytes(self.uleb()).decode('utf-8', 'replace')


def parse(path):
    """Return (types, funcs, elements, names).

    types    : [(params, results)] as lists of wasm type names
    funcs    : function index -> type index (imports first, then defined)
    elements : table slot -> function index
    names    : function index -> name
    """
    data = Path(path).read_bytes()
    if data[:4] != b'\0asm':
        sys.exit('%s is not a wasm module' % path)

    r = Reader(data, 8)
    types, funcs, elements, names = [], [], {}, {}
    num_imported = 0

    while r.p < len(data):
        sid = r.byte()
        size = r.uleb()
        end = r.p + size

        if sid == 1:                                    # type
            for _ in range(r.uleb()):
                assert r.byte() == 0x60
                params = [WASM_TYPES.get(r.byte(), '?') for _ in range(r.uleb())]
                results = [WASM_TYPES.get(r.byte(), '?') for _ in range(r.uleb())]
                types.append((params, results))
        elif sid == 2:                                  # import
            for _ in range(r.uleb()):
                r.name(); r.name()
                kind = r.byte()
                if kind == 0:                           # function
                    funcs.append(r.uleb())
                    num_imported += 1
                elif kind == 1:                         # table
                    r.byte()
                    if r.byte():
                        r.uleb()
                    r.uleb()
                elif kind == 2:                         # memory
                    if r.byte():
                        r.uleb()
                    r.uleb()
                elif kind == 3:                         # global
                    r.byte(); r.byte()
        elif sid == 3:                                  # function
            for _ in range(r.uleb()):
                funcs.append(r.uleb())
        elif sid == 9:                                  # element
            for _ in range(r.uleb()):
                flags = r.uleb()
                offset = 0
                if flags in (0, 2, 4, 6):
                    if flags in (2, 6):
                        r.uleb()                        # table index
                    # constant expression: i32.const N, end
                    if r.byte() == 0x41:
                        offset = r.sleb()
                    while r.byte() != 0x0B:
                        pass
                    if flags in (2, 6):
                        r.byte()                        # element kind
                    for i in range(r.uleb()):
                        elements[offset + i] = r.uleb()
                else:
                    break                               # passive/declared: skip
        elif sid == 0:                                  # custom
            sub = Reader(data, r.p)
            if sub.name() == 'name':
                while sub.p < end:
                    kind = sub.byte()
                    sub_size = sub.uleb()
                    stop = sub.p + sub_size
                    if kind == 1:                       # function names
                        for _ in range(sub.uleb()):
                            idx = sub.uleb()
                            names[idx] = sub.name()
                    sub.p = stop

        r.p = end

    return types, funcs, elements, names


def describe(types, funcs, idx):
    if idx >= len(funcs):
        return '(function index %d is out of range)' % idx
    params, results = types[funcs[idx]]
    return '(%s) -> %s' % (', '.join(params) or 'void',
                           ', '.join(results) or 'void')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wasm', type=Path)
    ap.add_argument('values', nargs='+',
                    help='function-pointer values (table indices), decimal or 0x hex')
    ap.add_argument('--index', action='store_true',
                    help='treat the values as function indices rather than table '
                         'slots -- this is what a wasm stack trace prints, e.g. '
                         'wasm-function[1911]')
    args = ap.parse_args()

    types, funcs, elements, names = parse(args.wasm)
    print('%s: %d types, %d functions, %d table slots, %d names'
          % (args.wasm.name, len(types), len(funcs), len(elements), len(names)))
    if not names:
        print('  (no name section -- build with --profiling-funcs or -g to get names)')
    print()

    for value in args.values:
        v = int(value, 0)
        if args.index:
            print('  %-10s -> %s   %s'
                  % (value, names.get(v, 'function #%d' % v),
                     describe(types, funcs, v)))
            continue
        if v not in elements:
            print('  %-10s not in the table (0 is the null slot; a value this '
                  'large may be data, not a pointer)' % value)
            continue
        fidx = elements[v]
        print('  %-10s -> %s   %s'
              % (value, names.get(fidx, 'function #%d' % fidx),
                 describe(types, funcs, fidx)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
