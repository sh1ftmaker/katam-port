#!/usr/bin/env python3
"""
lint_determinism.py -- find RNG use whose order the C standard leaves open.

    tools/lint_determinism.py            check game/src, exit 1 on a finding
    tools/lint_determinism.py -v         also list the RNG-advancing functions
    tools/lint_determinism.py --json F   write the findings as JSON

The game's RNG is one global LCG, `gRngVal`, advanced by the `Rand32()` /
`Rand16()` macros (game/include/random.h).  Two RNG operations in one full
expression without a sequence point between them -- `Rand16() + Rand16()`,
`f(Rand16(), Rand16())`, `x[Rand16() & 3] = Rand16()`, or a call to a function
that advances the RNG beside a `Rand16()` -- are evaluated in an order the
compiler picks, and gcc, g++ and clang pick differently.  Every peer of a
rollback session has to compute the same numbers, so every such site is
written out as sequenced temporaries, in the order the original game (agbcc)
used, and this lint keeps new ones out.  docs/DETERMINISM.md.

How: libclang parses every game file with the wasm build's flags, with
`gRngVal` turned from the `*(u32 *)0x030068D8` macro into a plain variable so
that it is visible in the AST.  Pass 1 summarises each function -- writes
gRngVal, reads it, which functions it calls -- and closes that over the call
graph (direct calls; a call through a pointer is assumed RNG-free, and the
count of those is reported).  Pass 2 walks each full expression and applies
C's sequencing rules: `,` `&&` `||` `?:` sequence their operands; the operands
of everything else, the arguments of a call, and the elements of an
initialiser list do not.  Two unsequenced operands conflict when one writes
the RNG and the other reads or writes it.

The same check covers the allocators (SEEDS: TaskCreate/TaskDestroy, the
EWRAM/IWRAM heaps, VRAM): two allocations unsequenced against each other get
their blocks in a compiler-chosen order.  There are none today.
tests/lint/determinism_selftest.c is the checker's own test (make
lint-determinism runs both).
"""
import argparse
import glob
import json
import multiprocessing
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

try:
    import clang.cindex as ci
except ImportError:
    sys.exit("lint_determinism: needs libclang's Python bindings "
             "(python3-clang / pip install libclang)")

RNG_VAR = "gRngVal"

# Leaves of the compact trees pass 1 keeps.
W, R = "W", "R"

# Other shared state whose order matters the same way: the allocators.  Two
# allocations in one full expression -- `f(TaskCreate(..), TaskCreate(..))` --
# hand out blocks in an order the compiler picks.  Calls to these count as
# writes to the named resource; anything that (transitively) calls them too.
SEEDS = {
    "TaskCreate": "W:heap", "TaskDestroy": "W:heap",
    "EwramMalloc": "W:heap", "EwramFree": "W:heap",
    "IwramMalloc": "W:heap", "IwramFree": "W:heap",
    "VramMalloc": "W:vram", "VramFree": "W:vram",
}


def conflict(sa, sb):
    """Do two unsequenced effect sets conflict?"""
    if (W in sa and sb & {W, R}) or (W in sb and sa & {W, R}):
        return "rng"
    for t in sa & sb:
        if t.startswith("W:"):
            return t[2:]
    return None


def compile_args():
    sysroot = os.path.expanduser(
        os.environ.get("EMSDK", "~/emsdk")) + "/upstream/emscripten/cache/sysroot"
    args = ["-x", "c", "-std=gnu99", "-fgnu89-inline", "-funsigned-char",
            "-DPORTABLE", "-DNONMATCHING", "-DMODERN=1",
            "-DPORT_GLOBAL_BASE=167772160u",
            "-I" + ROOT + "/game/include", "-I" + ROOT + "/game/generated",
            "-I" + ROOT + "/platform",
            "-include", "port/prelude.h", "-include", "port/ram_symbols.h",
            "-include", "port/rom_data.h",
            "-include", os.path.join(ROOT, "tools", "lint_determinism.h"),
            "-Wno-everything"]
    if os.path.isdir(sysroot):
        args += ["--target=wasm32-unknown-emscripten", "--sysroot=" + sysroot,
                 "-D__EMSCRIPTEN__"]
    else:
        args += ["-m32"]
    return args


BK = ci.BinaryOperator if hasattr(ci, "BinaryOperator") else None
K = ci.CursorKind


def is_rng_ref(c):
    return (c.kind == K.DECL_REF_EXPR and c.spelling == RNG_VAR)


def strip(c):
    """Look through parens and implicit casts."""
    while c.kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
        ch = list(c.get_children())
        if len(ch) != 1 or ch[0].kind == K.COMPOUND_STMT:
            break
        c = ch[0]
    return c


def loc(c):
    f, line, _, _ = c.extent.start.get_expansion_location() \
        if hasattr(c.extent.start, "get_expansion_location") else \
        (c.extent.start.file, c.extent.start.line, 0, 0)
    return (f.name if f else "?", line)


class TU:
    """Pass 1 over one file."""

    def __init__(self, path):
        self.path = path
        self.funcs = {}         # name -> {"W": bool, "R": bool, "calls": set}
        self.exprs = []         # (func, file, line, tree)
        self.indirect = 0
        self.errors = []

    # Compact tree: ("seq", [..]) | ("unseq", [..]) | ("call", name, node)
    #               | W | R | None
    def expr(self, c, fn):
        k = c.kind
        if is_rng_ref(c):
            fn["R"] = True
            return R
        if k == K.COMPOUND_STMT:        # a GNU statement expression
            return ("seq", self.stmts(c, fn))
        ch = list(c.get_children())
        if k == K.BINARY_OPERATOR or k == K.COMPOUND_ASSIGNMENT_OPERATOR:
            op = c.binary_operator if BK else None
            name = op.name if op is not None else ""
            if name in ("LAnd", "LOr", "Comma"):
                return mk("seq", [self.expr(x, fn) for x in ch])
            if name == "Assign" or k == K.COMPOUND_ASSIGNMENT_OPERATOR:
                lhs = strip(ch[0])
                if is_rng_ref(lhs):
                    fn["W"] = True
                    rd = [R] if k == K.COMPOUND_ASSIGNMENT_OPERATOR else []
                    return mk("seq", [self.expr(ch[1], fn)] + rd + [W])
            return mk("unseq", [self.expr(x, fn) for x in ch])
        if k == K.UNARY_OPERATOR and ch and is_rng_ref(strip(ch[0])):
            toks = [t.spelling for t in c.get_tokens()]
            if "++" in toks or "--" in toks:
                fn["R"] = fn["W"] = True
                return mk("seq", [R, W])
        if k == K.CONDITIONAL_OPERATOR:
            # cond is sequenced before the arm taken; the arms are exclusive.
            return mk("seq", [self.expr(x, fn) for x in ch])
        if k == K.CALL_EXPR:
            ref = c.referenced
            name = ref.spelling if ref is not None and \
                ref.kind == K.FUNCTION_DECL else None
            if name is None:
                self.indirect += 1
            else:
                fn["calls"].add(name)
            # callee designator and arguments are unsequenced with each other;
            # the call itself happens after all of them.
            args = mk("unseq", [self.expr(x, fn) for x in ch])
            return ("call", name, args)
        if k in (K.UNEXPOSED_EXPR,) and ch and ch[0].kind == K.COMPOUND_STMT:
            return ("seq", self.stmts(ch[0], fn))
        if k == K.CXX_UNARY_EXPR or k == K.TYPE_REF:     # sizeof: unevaluated
            return None
        return mk("unseq", [self.expr(x, fn) for x in ch])

    def full(self, c, fn, fname):
        t = self.expr(c, fn)
        if t is not None:
            f, line = loc(c)
            self.exprs.append((fname, f, line, t))
        return t

    def stmts(self, c, fn):
        """Walk statements; record every full expression; return the trees
        in order (for a statement expression's summary)."""
        out = []
        for x in c.get_children():
            k = x.kind
            if k.is_expression():
                out.append(self.full(x, fn, fn["name"]))
            elif k == K.DECL_STMT:
                for d in x.get_children():
                    for e in d.get_children():
                        if e.kind.is_expression():
                            out.append(self.full(e, fn, fn["name"]))
            elif k.is_statement() or k == K.VAR_DECL:
                if k == K.VAR_DECL:
                    for e in x.get_children():
                        if e.kind.is_expression():
                            out.append(self.full(e, fn, fn["name"]))
                else:
                    out.append(mk("seq", self.stmts(x, fn)))
        return [o for o in out if o is not None]

    def run(self):
        idx = ci.Index.create()
        tu = idx.parse(self.path, args=compile_args(),
                       options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES * 0)
        for d in tu.diagnostics:
            if d.severity >= ci.Diagnostic.Error:
                self.errors.append("%s:%d: %s" % (
                    d.location.file, d.location.line, d.spelling))
        for c in tu.cursor.get_children():
            if c.kind == K.FUNCTION_DECL and c.is_definition() and \
                    c.location.file and \
                    os.path.realpath(c.location.file.name) == \
                    os.path.realpath(self.path):
                fn = {"name": c.spelling, "W": False, "R": False,
                      "calls": set()}
                for b in c.get_children():
                    if b.kind == K.COMPOUND_STMT:
                        self.stmts(b, fn)
                prev = self.funcs.get(c.spelling)
                self.funcs[c.spelling] = fn
        return self


def mk(kind, kids):
    kids = [x for x in kids if x is not None]
    if not kids:
        return None
    if len(kids) == 1:
        return kids[0]
    return (kind, kids)


def parse_one(path):
    t = TU(path).run()
    funcs = {n: {"W": f["W"], "R": f["R"], "calls": sorted(f["calls"])}
             for n, f in t.funcs.items()}
    return path, funcs, t.exprs, t.indirect, t.errors


def effects(t, summ, conflicts, ctx):
    """Set of RNG effects of tree t; appends unsequenced conflicts."""
    if t is None:
        return frozenset()
    if t == W or t == R:
        return frozenset([t])
    kind = t[0]
    if kind == "call":
        e = effects(t[2], summ, conflicts, ctx)
        s = summ.get(t[1], frozenset())
        return e | s
    sets = [effects(x, summ, conflicts, ctx) for x in t[1]]
    if kind == "unseq":
        live = [(i, s) for i, s in enumerate(sets) if s]
        for a in range(len(live)):
            for b in range(a + 1, len(live)):
                c = conflict(live[a][1], live[b][1])
                if c:
                    conflicts.append(c)
    out = frozenset()
    for s in sets:
        out |= s
    return out


def count_rng_ops(t, summ):
    """How many RNG advances a tree performs (for the report)."""
    if t is None or t == R:
        return 0
    if t == W:
        return 1
    if t[0] == "call":
        return count_rng_ops(t[2], summ) + (1 if W in summ.get(t[1], ()) else 0)
    return sum(count_rng_ops(x, summ) for x in t[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("files", nargs="*")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()

    files = a.files or sorted(glob.glob(ROOT + "/game/src/**/*.c", recursive=True))
    # every file feeds the call graph, even when only some are reported
    allfiles = sorted(set(files) | set(glob.glob(ROOT + "/game/src/**/*.c",
                                                 recursive=True)))
    with multiprocessing.Pool(a.jobs) as pool:
        results = pool.map(parse_one, allfiles, chunksize=1)

    funcs, exprs, indirect, errors = {}, [], 0, []
    report = set(os.path.realpath(f) for f in files)
    for path, fs, es, ind, errs in results:
        for n, f in fs.items():
            g = funcs.setdefault(n, {"W": False, "R": False, "calls": set()})
            g["W"] |= f["W"]
            g["R"] |= f["R"]
            g["calls"] |= set(f["calls"])
        if os.path.realpath(path) in report:
            exprs += es
        indirect += ind
        errors += errs

    # close over the call graph
    summ = {n: set(k for k in (W, R) if f[k]) for n, f in funcs.items()}
    for n, tok in SEEDS.items():
        summ.setdefault(n, set()).add(tok)
        funcs.setdefault(n, {"W": False, "R": False, "calls": set()})
    changed = True
    while changed:
        changed = False
        for n, f in funcs.items():
            for c in f["calls"]:
                add = summ.get(c, set()) - summ[n]
                if add:
                    summ[n] |= add
                    changed = True
    summ = {n: frozenset(s) for n, s in summ.items()}

    findings = {}
    for fname, f, line, t in exprs:
        conf = []
        effects(t, summ, conf, None)
        if conf:
            key = (os.path.relpath(f, ROOT), line)
            findings.setdefault(key, {"file": key[0], "line": line,
                                      "function": fname,
                                      "resource": sorted(set(conf)),
                                      "rng_ops": count_rng_ops(t, summ)})

    if errors:
        print("lint_determinism: %d parse errors (first: %s)" %
              (len(errors), errors[0]), file=sys.stderr)
    adv = sorted(n for n, s in summ.items() if W in s)
    if a.verbose:
        print("RNG-advancing functions (%d): %s" % (len(adv), " ".join(adv)))
        print("calls through a pointer, assumed RNG-free: %d" % indirect)
    for k in sorted(findings):
        f = findings[k]
        src = ""
        try:
            with open(os.path.join(ROOT, f["file"])) as fh:
                src = fh.read().split("\n")[f["line"] - 1].strip()
        except Exception:
            pass
        what = "RNG use" if f["resource"] == ["rng"] else \
            "use of " + "/".join(f["resource"])
        print("%s:%d: unsequenced %s in %s: %s" %
              (f["file"], f["line"], what, f["function"], src))
    if a.json:
        with open(a.json, "w") as fh:
            json.dump({"findings": list(findings.values()),
                       "rng_functions": adv, "indirect_calls": indirect,
                       "parse_errors": errors}, fh, indent=1)
    print("lint_determinism: %d unsequenced RNG/allocator site(s) in %d file(s); "
          "%d functions advance the RNG" %
          (len(findings), len(report), len(adv)), file=sys.stderr)
    return 1 if findings or errors else 0


if __name__ == "__main__":
    sys.exit(main())
