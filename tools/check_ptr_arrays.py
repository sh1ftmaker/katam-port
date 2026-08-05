#!/usr/bin/env python3
"""Find file-scope arrays of pointers whose element stride the game relies on.

The bug this exists to catch, stated once:

    On the GBA every pointer is four bytes, so *every* array of pointers has a
    stride of four and the game may freely alias one to another.  The 64-bit
    builds narrow pointer *members of structures* (tools/narrow32.py) because
    those structures are laid out by the ROM, the linker script or the game's
    own allocator.  A file-scope array of pointers has none of those
    constraints -- nothing outside the compiler decides where its elements go
    -- so narrow32 leaves it alone, and under LP64 its stride becomes eight.

    That is invisible and harmless right up until something reads the array
    through a *different* type.  src/code.c does exactly that:

        extern struct RoomTiledBG *const gUnk_082D8D74[];   /* stride 8 */
        gUnk_03002E60 = (const union Unk_03002E60 *)gUnk_082D8D74;

    `union Unk_03002E60` holds PTR32 members, so it is four bytes, and every
    subsequent gUnk_03002E60[i] reads element i/2 of the real array -- a valid,
    mapped, entirely plausible pointer to the wrong record.  The background
    layer of every level was drawn from the wrong descriptor.  Nothing faulted,
    no assertion fired, and the layout checks passed, because the type that was
    wrong is not a type any of them describe.

So: report every file-scope array of pointers, and mark the ones whose name
also appears under a cast somewhere in the tree.  Those are the ones where the
stride is observable and the array has to be narrowed to PTR32.

A clean report is not a proof -- an array can be aliased through a variable, or
passed as void * and cast at the far end, and this sees neither.  It is a list
of the cases that can be found by looking, which is how the one above was
found once its shape was known.

    tools/check_ptr_arrays.py build/port-src
"""

import os
import re
import sys

# File scope only -- the declaration has to start in column zero.  A local is
# not interesting: nothing aliases a stack array across a type.
#
# `struct Foo *const gBar[] = {`, `const u16 *gBaz[4];`
DATA_ARRAY = re.compile(
    r'^(?P<pre>(?:extern\s+|static\s+|const\s+|volatile\s+)*'
    r'(?:struct\s+|union\s+|enum\s+)?[A-Za-z_]\w*\s*)'
    r'(?P<stars>\*+)\s*(?:const\s+|volatile\s+)*'
    r'(?P<name>[A-Za-z_]\w*)\s*\[',
    re.MULTILINE)

# `void (*const gQux[])(void);`
FN_ARRAY = re.compile(
    r'^(?P<ret>(?:extern\s+|static\s+|const\s+)*[A-Za-z_][\w\s*]*?)'
    r'\(\s*\*\s*(?:const\s+)?(?P<name>[A-Za-z_]\w*)\s*\[',
    re.MULTILINE)

# Already-narrowed forms, so that a second run over a fixed tree still sees the
# array and can report it as handled rather than losing sight of it.
PTR32_ARRAY = re.compile(
    r'^(?:extern\s+|static\s+|const\s+)*'
    r'PTR32(?:_TD)?\s*\((?P<inner>[^)]*)\)\s*(?:const\s+)?'
    r'(?P<name>[A-Za-z_]\w*)\s*\[',
    re.MULTILINE)


def sources(root):
    for base, _, files in os.walk(root):
        for f in files:
            if f.endswith((".c", ".h")):
                yield os.path.join(base, f)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "build/port-src"

    text = {}
    for path in sources(root):
        with open(path, encoding="utf-8", errors="replace") as fh:
            text[path] = fh.read()

    arrays = {}          # name -> (path, kind)
    narrowed = set()
    for path, body in text.items():
        for m in DATA_ARRAY.finditer(body):
            arrays.setdefault(m.group("name"), (path, "data"))
        for m in FN_ARRAY.finditer(body):
            arrays.setdefault(m.group("name"), (path, "function"))
        for m in PTR32_ARRAY.finditer(body):
            arrays.setdefault(m.group("name"), (path, "data"))
            narrowed.add(m.group("name"))

    # Cast anywhere?  `(some type *)name` with name not followed by `[`, i.e.
    # the array itself rather than one of its elements.
    aliased = {}
    for name in arrays:
        pat = re.compile(r'\(\s*(?:const\s+)?[^)（]{0,60}?\*\s*\)\s*'
                         + re.escape(name) + r'\b(?!\s*\[)')
        for path, body in text.items():
            for m in pat.finditer(body):
                line = body.count("\n", 0, m.start()) + 1
                aliased.setdefault(name, []).append(
                    (path, line, body[m.start():m.end() + 40].split("\n")[0]))

    risky = sorted(n for n in aliased if n not in narrowed)

    print("%d file-scope pointer arrays, %d already narrowed to PTR32"
          % (len(arrays), len(narrowed)))
    print("%d are read through a cast; %d of those are NOT narrowed:\n"
          % (len(aliased), len(risky)))

    for name in risky:
        path, kind = arrays[name]
        print("  %-24s (%s array, declared in %s)"
              % (name, kind, os.path.relpath(path, root)))
        for path, line, snippet in aliased[name][:4]:
            print("      %s:%d  %s"
                  % (os.path.relpath(path, root), line, snippet.strip()))

    return 1 if risky else 0


if __name__ == "__main__":
    sys.exit(main())
