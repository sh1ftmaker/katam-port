#!/usr/bin/env python3
"""Turn either host's `[stack]` lines into the same list of C function names.

The port can be diffed against itself across ABIs because every build reserves
the GBA map at the same true addresses -- but that only works for *data*.  Code
addresses are the one thing the two builds cannot agree on: a function pointer
is a table index in WebAssembly and a text address natively, and a return
address on the stack is the same problem one level up.

So normalise before comparing.  Both hosts can name a frame; they just do it at
different times.

    web       emscripten_get_callstack already names the frame, because
              --profiling-funcs keeps the wasm name section.  The line arrives
              as `at katam.wasm.sub_08152A34 (...:wasm-function[123]:0x4f2a1)`
              and all this has to do is pull the name out.

    native    backtrace() gives a return address and nothing else.  The 64-bit
              builds link -no-pie with -Ttext-segment=0x10000000 (see
              CMakeLists.txt), so the address is absolute and constant across
              runs, and `nm` on the very binary that produced the log resolves
              it exactly.  No -rdynamic, no debug info, no separate build.

What comes out is a list of decompiled names -- `sub_08152A34`, `bg.c`'s
`TileUpload`, whatever the decomp called it -- for both hosts, so the two can
be diffed with `diff`.  That is the symbol-normalising comparison the state
hasher wanted, applied to control flow instead of to memory.  It is far cheaper
there: a stack is twenty frames, while EWRAM is 256 KiB of which any word might
be a function pointer, and the port has no way to know which ones are.

Usage:
    tools/symbolize_stack.py --nm build/lp64/katam  < native.log
    tools/symbolize_stack.py                        < web.log

`--nm` is what selects native mode; without it the input is assumed to already
carry names.  Both modes read the whole log and emit one block per stack, so
the output of two runs can be diffed directly.
"""

import argparse
import bisect
import re
import subprocess
import sys

# `[stack] dma 0x10011d350` (native) or `[stack] dma at katam.wasm.Foo (...)`.
STACK = re.compile(r"^\[stack\]\s+(\S+)\s+(.*)$")
# `[dma] n=27263 f=1099 ch=3 src=...` -- the line a stack belongs to.
DMA = re.compile(r"^\[dma\]\s+n=(\d+)\s")

# emscripten spells a frame `at katam.wasm.NAME (wasm://...)` or, when it has
# no name for it, `at wasm-function[1234]`.  Keep both: an unnamed frame is
# still a frame, and dropping it silently would misalign the two stacks.
WASM_FRAME = re.compile(r"^\s*at\s+(?:\S+?\.wasm\.)?([^\s(]+)")

HEX = re.compile(r"^0x([0-9a-fA-F]+)$")


def nm_table(binary):
    """[(addr, name)] for every text symbol, sorted, for bisect."""
    out = subprocess.run(
        ["nm", "--defined-only", "-C", binary],
        capture_output=True, text=True, check=True,
    ).stdout

    syms = []
    for line in out.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) != 3:
            continue
        addr, kind, name = parts
        # t/T is text.  Anything else is data and cannot hold a return address;
        # including it would let a nearby variable claim a code frame.
        if kind not in ("t", "T", "w", "W"):
            continue
        try:
            syms.append((int(addr, 16), name))
        except ValueError:
            continue

    syms.sort()
    return syms


def resolve(syms, addr):
    """The symbol containing addr, with its offset.

    A return address points *after* the call, so the frame belongs to the
    symbol at or below it -- bisect_right minus one, not bisect_left.
    """
    i = bisect.bisect_right(syms, (addr, "\xff")) - 1
    if i < 0:
        return "0x%x" % addr
    base, name = syms[i]
    return "%s+0x%x" % (name, addr - base)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", metavar="BINARY",
                    help="resolve raw addresses against this binary's symbols")
    ap.add_argument("--tag", default=None,
                    help="only stacks with this tag (default: all)")
    args = ap.parse_args()

    syms = nm_table(args.nm) if args.nm else None
    current = None  # the `[dma] n=` a stack is hanging off, if any

    for line in sys.stdin:
        line = line.rstrip("\n")

        m = DMA.match(line)
        if m:
            current = m.group(1)
            continue

        m = STACK.match(line)
        if not m:
            continue
        tag, body = m.group(1), m.group(2)
        if args.tag is not None and tag != args.tag:
            continue

        if current is not None:
            print("--- %s n=%s" % (tag, current))
            current = None

        h = HEX.match(body.strip())
        if h and syms is not None:
            print("  " + resolve(syms, int(h.group(1), 16)))
        elif h:
            # A raw address with no symbol table is not comparable to anything.
            # Say so rather than printing a number that looks like a result.
            print("  <unresolved %s -- pass --nm>" % body.strip())
        else:
            w = WASM_FRAME.match(body)
            print("  " + (w.group(1) if w else body.strip()))


if __name__ == "__main__":
    main()
