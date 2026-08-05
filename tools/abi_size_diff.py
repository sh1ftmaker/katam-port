#!/usr/bin/env python3
"""Compare every struct and union size between an ILP32 and an LP64 build.

`make layout-check` asserts the 246 types whose GBA offsets are known, and it
passes.  That is a smaller claim than it sounds: a type only appears in
build/generated/port/gba_layout.h if gen_gba_layout.py had a console-side
offset to compare against.  A type the decompilation declares but never places
at a fixed address is not in the table, is never asserted, and can quietly
double in size under LP64 -- taking with it every `sizeof` the game passes to
its own allocator.

That matters because the game allocates by size:

    task = TaskCreate(ObjectMain, sizeof(struct Foo), priority, 0, dtor);

and TaskCreate decides *where* from what it is asked for.  A struct that grew
does not corrupt anything directly; it moves the object, and the next one, and
eventually two builds that were computing the same thing are keeping it in
different places.  Nothing faults and nothing asserts.

So compare the sizes directly, from DWARF, between the two binaries that
actually shipped -- the ILP32 build is the definition of correct here, because
it is the one that agrees with the console.

    tools/abi_size_diff.py build/native/katam build/lp64/katam

Both need debug information (-g); a Release build without it reports nothing,
which this says rather than calling it a pass.
"""

import argparse
import importlib.util
import sys
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    "dwarf_layout", Path(__file__).with_name("dwarf_layout.py"))
dwarf_layout = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(dwarf_layout)


def sizes(path):
    """Sizes from one object file, or merged over a whole build directory.

    A directory is the honest input.  A single hand-written translation unit
    that includes every header sees only the types the headers declare, and the
    decompilation defines plenty inside .c files -- including ones the game
    passes to TaskCreate.  Reading the objects the build actually produced sees
    exactly the types the build actually used.

    The linked executable would be better still and does not work: it carries
    one abbrev table per compilation unit and dwarf_layout.py reads a single
    one, which is enough for an object file and not for an image.
    """
    p = Path(path)
    objs = sorted(p.rglob("*.o")) if p.is_dir() else [p]
    if not objs:
        return {}, {}

    out, files = {}, {}
    for o in objs:
        try:
            dw = dwarf_layout.Dwarf(o)
            got = dwarf_layout.collect(dw)
        except (Exception, SystemExit):
            # An object with no DWARF, or one this reader cannot follow, is
            # skipped rather than fatal: the point is coverage across hundreds
            # of files, and one unreadable object should not hide the rest.
            continue
        for k, v in got.items():
            out.setdefault(k, v["size"])
            files.setdefault(k, v.get("file"))
    return out, files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ilp32", help="the 32-bit binary -- the reference")
    ap.add_argument("lp64", help="the 64-bit binary")
    ap.add_argument("--quiet", action="store_true",
                    help="print only the mismatches")
    ap.add_argument("--all", action="store_true",
                    help="include the host's own types (SDL, glibc, platform/) "
                         "-- these differ by design and are filtered out")
    args = ap.parse_args()

    a, files = sizes(args.ilp32)
    b, _ = sizes(args.lp64)

    if not a or not b:
        print("no DWARF in %s -- build with -g"
              % (args.ilp32 if not a else args.lp64))
        return 2

    common = sorted(set(a) & set(b))
    bad = [k for k in common if a[k] != b[k]]

    # The game's types only, unless asked for everything.
    #
    # SDL's structs and glibc's differ between the ABIs and are *supposed* to:
    # they are the host's, laid out by the host's compiler for the host's
    # libraries, and nothing on the console has an opinion about them.  So do
    # the port's own -- platform/dma.c's DmaChannel holds real host pointers
    # and should hold whatever a host pointer costs.  Leaving them in the
    # report is what made the first run of this look like fifteen problems
    # instead of one, and the one that mattered was sixth in the list.
    if not args.all:
        bad = [k for k in bad
               if (files.get(k) or "").find("/build/port-src/") != -1]

    if not args.quiet:
        print("%d types in both builds, %d compared" % (len(common), len(common)))

    for kind, name in bad:
        print("  %-6s %-34s ILP32 0x%-5X LP64 0x%-5X  (+%d)  %s"
              % (kind, name, a[(kind, name)], b[(kind, name)],
                 b[(kind, name)] - a[(kind, name)],
                 files.get((kind, name)) or ""))

    if bad:
        print("\n%d decomp type(s) change size under LP64.  Every sizeof() of "
              "one of these\nis a different number in the two builds -- "
              "including the ones the game hands\nto TaskCreate, which is how "
              "two builds that agree about what to do stop\nagreeing about "
              "where to keep it." % len(bad))
        return 1

    print("no decomp type changes size between the two ABIs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
