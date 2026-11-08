#!/usr/bin/env python3
"""
codemod_fnid.py -- canonical code IDs for every function the game stores.

    tools/codemod_fnid.py --apply      rewrite the sources (idempotent)
    tools/codemod_fnid.py --check      lint: exit 1 on an unwrapped site
    tools/codemod_fnid.py --gen        regenerate the ID table and the
                                       per-file registration blocks
    tools/codemod_fnid.py --stats      count sites, change nothing

netplay-v2 R5 (docs/design/NETPLAY-V2.md §6, docs/DETERMINISM.md "Code IDs").
The game keeps function pointers in its own memory -- Task::main,
Object2::unk78, gIntrTable, the sound engine's tables -- and a host function
pointer is a code address natively and a table index in wasm, so the same
state is spelled differently on every host and moves whenever code layout
changes.  Instead, every function-pointer *value* the game handles is a
canonical code ID: the function's GBA ROM address with the Thumb bit as the
ROM stores it (so a value copied from a ROM table is already one), or for a
function with no ROM address a name-derived value in 0x0E000001..0x0EFFFFFF.

Two rewrites, both done on libclang's AST of each file parsed with the wasm
build's flags (tools/lint_determinism.py's setup):

  * every function designator that decays to a pointer -- assigned,
    passed, initialised, compared, returned, cast, `&f` -- becomes FN(f),
    a constant expression of the same pointer type holding FNID_f;
  * every call whose callee is not a function -- through a member, a local,
    a table, a cast -- becomes CALL(callee)(args), which maps the ID back to
    the host function (platform/fnid.c).

A site inside a macro body is rewritten in the macro's definition.  Running
it again finds nothing to do: a wrapped designator only occurs inside FN's
cast type (unevaluated) and a wrapped callee is a call to PortFnHost.

--gen writes game/generated/fnid_ids.h (FNID_name for every function that is
the target of an FN anywhere, plus the ROM-only stubs), and a registration
block at the end of each file that defines one, so that file -- the only one
that can name a static function -- hands its host address to the lookup.
game/generated/fnid_tables.c lists the files.  IDs of ROM functions come from
the GBA build's katam.elf symbol table (--elf, default the decomp checkout);
without it the IDs already in fnid_ids.h are kept and new names get
name-derived IDs (with a warning).
"""
import argparse
import ctypes
import glob
import multiprocessing
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

try:
    import clang.cindex as ci
except ImportError:
    sys.exit("codemod_fnid: needs libclang's Python bindings "
             "(python3-clang / pip install libclang)")

import lint_determinism as ld   # compile_args()

K = ci.CursorKind
IDS_H = os.path.join(ROOT, "game", "generated", "fnid_ids.h")
TABLES_C = os.path.join(ROOT, "game", "generated", "fnid_tables.c")
DEFAULT_ELF = os.path.normpath(os.path.join(ROOT, "..", "decomp", "katam.elf"))
if not os.path.exists(DEFAULT_ELF):
    DEFAULT_ELF = "/bigdata/agent-tom/projects/katamport/decomp/katam.elf"

# The lookup itself; a call to it is what a wrapped callee looks like.
LOOKUP = ("PortFnHost", "PortFnHostAt")

# Platform files that are part of the simulation's code-pointer traffic and
# are rewritten the same way as the game: the sound engine's mixer half,
# which fills gMPlayJumpTable and calls the callbacks in gSoundInfo, and the
# platform files that define a function the game stores.  Other
# platform code is host code and translates by hand (CALL at the few places it
# calls a game-stored pointer).
PLATFORM_FILES = ["platform/m4a_mixer.c",
                  # MultiSioRecvBufChange, the C half of the link driver's
                  # interrupt, which the game installs as a timer handler
                  "platform/multi_sio_intr.c"]

BEGIN = "/* PORT-FNID begin:"
END = "/* PORT-FNID end */"

_spell = ci.conf.lib["clang_getSpellingLocation"]
_spell.argtypes = [ci.SourceLocation, ctypes.POINTER(ctypes.c_void_p),
                   ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                   ctypes.POINTER(ctypes.c_uint)]
_fname = ci.conf.lib["clang_getFileName"]
_fname.argtypes = [ctypes.c_void_p]
_fname.restype = ci._CXString


_floc = ci.conf.lib["clang_getFileLocation"]
_floc.argtypes = _spell.argtypes


def file_loc(loc):
    """(path, offset) of loc in a file: a macro argument where it is written,
    a token from a macro body at the macro's expansion."""
    fp = ctypes.c_void_p()
    l = ctypes.c_uint()
    c = ctypes.c_uint()
    o = ctypes.c_uint()
    _floc(loc, ctypes.byref(fp), ctypes.byref(l), ctypes.byref(c), ctypes.byref(o))
    if not fp.value:
        return None, 0
    return os.path.realpath(ci._CXString.from_result(_fname(fp))), o.value


def spelling(loc):
    """(path, offset) of where the token at loc is spelled."""
    fp = ctypes.c_void_p()
    l = ctypes.c_uint()
    c = ctypes.c_uint()
    o = ctypes.c_uint()
    _spell(loc, ctypes.byref(fp), ctypes.byref(l), ctypes.byref(c), ctypes.byref(o))
    if not fp.value:
        return None, 0
    return os.path.realpath(ci._CXString.from_result(_fname(fp))), o.value


_texts = {}


def text_of(path):
    """The file as bytes: libclang's offsets are byte offsets."""
    t = _texts.get(path)
    if t is None:
        with open(path, "rb") as fh:
            t = _texts[path] = fh.read()
    return t


def _balanced(t, i):
    """t[i] is an opening bracket; index just past its match, or None."""
    pair = {ord("("): ord(")"), ord("["): ord("]")}
    stack = [pair[t[i]]]
    i += 1
    while i < len(t) and stack:
        ch = t[i]
        if ch in pair:
            stack.append(pair[ch])
        elif ch == stack[-1]:
            stack.pop()
        elif ch in (ord(")"), ord("]"), ord(";"), ord("{"), ord("}")):
            return None
        i += 1
    return i if not stack else None


def postfix_end(t, i):
    """End of the callee starting at t[i]: a primary (identifier or
    parenthesised) then [..], .m and ->m, stopping before the argument list."""
    def ws(j):
        while j < len(t) and t[j:j + 1] in (b" ", b"\t", b"\\", b"\n", b"\r"):
            j += 1
        return j
    m = re.compile(rb"[A-Za-z_]\w*")
    if t[i:i + 1] == b"(":
        j = _balanced(t, i)
    else:
        mm = m.match(t, i)
        j = mm.end() if mm else None
    if j is None:
        return None
    end = j
    while True:
        k = ws(end)
        if t[k:k + 1] == b"[":
            end = _balanced(t, k)
            if end is None:
                return None
        elif t[k:k + 1] == b"." or t[k:k + 2] == b"->":
            k = ws(k + (1 if t[k:k + 1] == b"." else 2))
            mm = m.match(t, k)
            if not mm:
                return None
            end = mm.end()
        elif t[k:k + 1] == b"(":
            return end
        else:
            return None


def game_files():
    fs = sorted(glob.glob(ROOT + "/game/src/**/*.c", recursive=True))
    fs += sorted(glob.glob(ROOT + "/game/generated/*.c"))
    fs = [f for f in fs if os.path.basename(f) != "fnid_tables.c"]
    fs += [os.path.join(ROOT, p) for p in PLATFORM_FILES]
    return fs


def editable(path):
    """Files the codemod may rewrite: the game, its headers, the generated
    sources and the listed platform files."""
    if path is None:
        return False
    rp = os.path.relpath(path, ROOT)
    if rp.startswith(".."):
        return False
    if rp.startswith("game/"):
        return not rp.startswith("game/generated/fnid_")
    return rp in PLATFORM_FILES


def compile_args():
    return ld.compile_args() + ["-DPORT_FNID_SCAN"]


def strip(c):
    while c.kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
        ch = list(c.get_children())
        if len(ch) != 1:
            break
        c = ch[0]
    return c


def unary_op(c):
    toks = [t.spelling for t in c.get_tokens()]
    return toks[0] if toks else ""


def is_fn_ref(c):
    r = c.referenced
    return c.kind == K.DECL_REF_EXPR and r is not None and r.kind == K.FUNCTION_DECL


def is_wrapped_callee(c):
    """CALL(p) expands to ((T)PortFnHost(...)): a cast of a lookup call."""
    c = strip(c)
    if c.kind == K.CSTYLE_CAST_EXPR:
        ch = list(c.get_children())
        if ch:
            inner = strip(ch[-1])
            if inner.kind == K.CALL_EXPR and inner.spelling in LOOKUP:
                return True
    if c.kind == K.CALL_EXPR and c.spelling in LOOKUP:
        return True
    return False


def is_direct_callee(c):
    """Callee that names a function: f, (f), *f, &f, (*f) ..."""
    c = strip(c)
    while c.kind == K.UNARY_OPERATOR and unary_op(c) in ("*", "&"):
        ch = list(c.get_children())
        if len(ch) != 1:
            return False
        c = strip(ch[0])
    return is_fn_ref(c)


class Scan:
    def __init__(self, path):
        self.path = os.path.realpath(path)
        self.decays = []     # (file, start, end, name, wrap_text_kind)
        self.calls = []      # (file, start, end, exprtext-ish)
        self.defs = {}       # name -> (file, static)
        self.errors = []
        self.odd = []        # sites we could not place
        self.extcalls = []   # (file, line, fname, callee) FN passed to non-game fn

    def site(self, c, what):
        f, line = c.location.file, c.location.line
        return "%s:%d: %s" % (os.path.relpath(f.name, ROOT) if f else "?", line, what)

    def walk(self, c, parents, uneval):
        k = c.kind
        if k == K.CXX_UNARY_EXPR:       # sizeof / alignof
            uneval = True
        if k == K.DECL_REF_EXPR and is_fn_ref(c) and not uneval:
            self.decay(c, parents)
        if k == K.CALL_EXPR and not uneval:
            ch = list(c.get_children())
            if ch and not is_direct_callee(ch[0]) and not is_wrapped_callee(ch[0]):
                self.call(c, ch[0])
        ch = list(c.get_children())
        last = len(ch) - 1
        for i, x in enumerate(ch):
            u = uneval
            # A cast's children are its type (typeof expressions included,
            # which FN and CALL put there) and then its operand.
            if k == K.CSTYLE_CAST_EXPR and i != last:
                u = True
            # Declarations: an initialiser is evaluated, a typeof in the type
            # is not.  The initialiser is the last expression child.
            if k in (K.VAR_DECL, K.FIELD_DECL, K.PARM_DECL, K.TYPEDEF_DECL) and \
                    x.kind.is_expression():
                if k != K.VAR_DECL or i != last:
                    u = True
            self.walk(x, parents + [c], u)

    def decay(self, c, parents):
        # Climb parens and implicit casts.  A call's first child is the callee.
        node = c
        i = len(parents) - 1
        while i >= 0 and parents[i].kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
            node = parents[i]
            i -= 1
        p = parents[i] if i >= 0 else None
        target = c
        if p is not None and p.kind == K.UNARY_OPERATOR and unary_op(p) == "&":
            target = p
            node = p
            i -= 1
            while i >= 0 and parents[i].kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
                node = parents[i]
                i -= 1
            p = parents[i] if i >= 0 else None
        elif p is not None and p.kind == K.UNARY_OPERATOR and unary_op(p) == "*":
            # *f is f again; a call of it is direct
            j = i - 1
            while j >= 0 and parents[j].kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
                j -= 1
            if j >= 0 and parents[j].kind == K.CALL_EXPR:
                return
        if p is not None and p.kind == K.CALL_EXPR:
            first = next(iter(p.get_children()), None)
            if first is not None and first == node:
                return          # direct call
        name = c.spelling
        sf, so = spelling(c.location)
        if sf and re.search(rb"\bPORT_FNID\(\s*$", text_of(sf)[max(0, so - 40):so]):
            return              # a registration entry: the host address on purpose
        if target is c:
            start, end = so, so + len(name)
        else:
            sf2, s2 = spelling(target.extent.start)
            if sf2 != sf:
                self.odd.append(self.site(c, "&%s spans files" % name))
                return
            start, end = s2, so + len(name)
        self.decays.append((sf, start, end, name, self.site(c, name)))
        # passed straight to something that is not game code?
        if p is not None and p.kind == K.CALL_EXPR:
            callee = strip(next(iter(p.get_children())))
            if is_fn_ref(callee):
                self.extcalls.append((self.site(c, "%s passed to %s" %
                                                (name, callee.spelling)),
                                      callee.spelling))

    def call(self, c, callee):
        x = strip(callee)
        # (*p)(args) -> CALL(p)(args)
        if x.kind == K.PAREN_EXPR:
            y = strip(x)
            if y.kind == K.UNARY_OPERATOR and unary_op(y) == "*":
                x = next(iter(y.get_children()))
        elif x.kind == K.UNARY_OPERATOR and unary_op(x) == "*":
            x = next(iter(x.get_children()))
        sf, s = spelling(x.extent.start)
        ff, fs = file_loc(x.extent.start)
        fe_f, fe = file_loc(x.extent.end)
        if sf is not None and (sf, s) != (ff, fs) and ff is not None and \
                ff == fe_f and fe > fs and postfix_end(text_of(ff), fs) == fe:
            # Written in the file, starting with an object-like macro
            # (`gUnk_08351648[i].unk10(obj)`, `gCurTask->main()`).
            self.calls.append((ff, fs, fe, self.site(c, "indirect call")))
            return
        if sf is not None and (sf, s) != (ff, fs):
            # Spelled inside a macro body: the end of a range there is not
            # reliable through libclang, so read the callee off the text.
            e = postfix_end(text_of(sf), s)
            if e is None:
                self.odd.append(self.site(c, "callee in a macro body not understood"))
                return
            self.calls.append((sf, s, e, self.site(c, "indirect call")))
            return
        ef, e = spelling(x.extent.end)
        if sf is None or sf != ef or e <= s:
            # e.g. `gUnk_08351648[i].unk10(obj)`, the table being a macro:
            # wrap it where it is written
            sf, s = file_loc(x.extent.start)
            ef, e = file_loc(x.extent.end)
        if sf is None or sf != ef or e <= s:
            self.odd.append(self.site(c, "callee spans macro boundaries"))
            return
        self.calls.append((sf, s, e, self.site(c, "indirect call")))

    def run(self):
        idx = ci.Index.create()
        tu = idx.parse(self.path, args=compile_args())
        for d in tu.diagnostics:
            if d.severity >= ci.Diagnostic.Error:
                self.errors.append("%s:%d: %s" % (
                    d.location.file, d.location.line, d.spelling))
        for c in tu.cursor.get_children():
            if c.kind == K.FUNCTION_DECL and c.is_definition() and c.location.file:
                f = os.path.realpath(c.location.file.name)
                st = c.storage_class == ci.StorageClass.STATIC
                self.defs[c.spelling] = (f, st)
            self.walk(c, [], False)
        return self


def scan_one(path):
    s = Scan(path).run()
    return (s.path, s.decays, s.calls, s.defs, s.errors, s.odd, s.extcalls)


def scan_all(files, jobs):
    with multiprocessing.Pool(jobs) as pool:
        return pool.map(scan_one, files, chunksize=1)


def read(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        return fh.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)


def collect(results):
    decays, calls, defs, errors, odd, ext = {}, {}, {}, [], [], []
    for path, ds, cs, df, er, od, ex in results:
        for (f, s, e, name, where) in ds:
            decays.setdefault((f, s, e), (name, where))
        for (f, s, e, where) in cs:
            calls.setdefault((f, s, e), where)
        for n, v in df.items():
            defs.setdefault(n, set()).add(v)
        errors += er
        odd += od
        ext += ex
    ext = sorted(set(w for w, callee in ext if callee not in defs))
    return decays, calls, defs, errors, sorted(set(odd)), ext


def plan_edits(decays, calls, named=()):
    """Per file: list of (start, end, kind, name, where).  Files named on the
    command line count as editable too."""
    edits = {}
    bad = []
    named = set(named)
    editable_ = lambda f: editable(f) or f in named
    for (f, s, e), (name, where) in decays.items():
        if not editable_(f):
            bad.append(where + " (in a file the codemod does not edit)")
            continue
        edits.setdefault(f, []).append((s, e, "FN", name, where))
    for (f, s, e), where in calls.items():
        if not editable_(f):
            bad.append(where + " (in a file the codemod does not edit)")
            continue
        edits.setdefault(f, []).append((s, e, "CALL", None, where))
    return edits, bad


def apply_edits(edits):
    """Edits as insertions, so nested sites (a CALL whose callee holds an FN or
    another CALL) compose: FN(f) is "FN(" before f and ")" after it, `&f` drops
    the & as well, CALL(x) is "CALL(" before and ")" after."""
    n = 0
    problems = []
    for f, es in edits.items():
        with open(f, "rb") as fh:
            text = fh.read()
        opens = {}      # pos -> [(range_len, deletion_len, text)]
        closes = {}     # pos -> count
        for (s, e, kind, name, where) in sorted(set(es)):
            seg = text[s:e].decode("utf-8", "surrogateescape")
            dele = 0
            if kind == "FN":
                if seg.startswith("&"):
                    m = re.match(r"&\s*\(?\s*", seg)
                    dele = m.end()
                    ident = seg[dele:].rstrip(") \t")
                    if seg[dele:].rstrip(" \t").endswith(")"):
                        problems.append("%s: parenthesised &(f) not handled" % where)
                        continue
                else:
                    ident = seg
                if ident != name:
                    problems.append("%s: expected %r at the site, found %r" % (where, name, seg))
                    continue
                opens.setdefault(s, []).append((e - s, dele, "FN("))
            else:
                opens.setdefault(s, []).append((e - s, 0, "CALL("))
            closes[e] = closes.get(e, 0) + 1
            n += 1
        out = text
        for pos in sorted(set(opens) | set(closes), reverse=True):
            ops = sorted(opens.get(pos, []), key=lambda x: -x[0])
            dele = max([d for _, d, _ in ops] or [0])
            ins = ")" * closes.get(pos, 0) + "".join(t for _, _, t in ops)
            out = out[:pos] + ins.encode() + out[pos + dele:]
        if out != text:
            with open(f, "wb") as fh:
                fh.write(out)
    return n, problems


# --- IDs -------------------------------------------------------------------

def fnv1a(s):
    h = 0x811C9DC5
    for b in s.encode():
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def name_id(name, salt=0):
    s = name if not salt else "%s#%d" % (name, salt)
    return 0x0E000000 | (fnv1a(s) & 0x00FFFFFE) | 1


def elf_functions(path):
    """name -> st_value for STT_FUNC symbols (Thumb bit included)."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise SystemExit("codemod_fnid: %s is not a 32-bit ELF" % path)
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(e_shnum):
        secs.append(struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize))
    out = {}
    multi = {}
    for s in secs:
        if s[1] != 2:      # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for j in range(s[5] // 16):
            nm, val, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", data, s[4] + j * 16)
            if info & 0xF != 2 or shndx == 0:
                continue
            o = strtab[4] + nm
            name = data[o:data.index(b"\0", o)].decode()
            multi.setdefault(name, set()).add(val)
            out[name] = val
    return out, multi


def read_ids():
    ids = {}
    if os.path.exists(IDS_H):
        for m in re.finditer(r"^#define FNID_(\w+) 0x([0-9A-Fa-f]+)u", read(IDS_H), re.M):
            ids[m.group(1)] = int(m.group(2), 16)
    return ids


def wanted_names(results_text_files):
    """Every FN(name) target in the editable sources, plus the ROM stubs."""
    names = set()
    pat = re.compile(r"\bFN\(\s*(\w+)\s*\)")
    for f in results_text_files:
        for m in pat.finditer(read(f)):
            names.add(m.group(1))
    return names


def assign_ids(names, elf_path, extra):
    old = read_ids()
    elf, multi = ({}, {})
    if elf_path and os.path.exists(elf_path):
        elf, multi = elf_functions(elf_path)
    else:
        print("codemod_fnid: no katam.elf (%s); keeping the committed IDs, "
              "new names get name-derived IDs" % elf_path, file=sys.stderr)
    ids = {}
    src = {}
    for n in sorted(names):
        if n in extra:
            ids[n] = extra[n]
            src[n] = "rom"
        elif n in elf and len(multi[n]) == 1 and 0x08000000 <= elf[n] < 0x0A000000:
            ids[n] = elf[n]
            src[n] = "rom"
        elif not elf and n in old:
            ids[n] = old[n]
            src[n] = "rom" if old[n] < 0x0E000000 else "name"
    # aliases: two names at one ROM address keep the address for the first
    # (alphabetical) and the rest fall back to name IDs
    by = {}
    for n in sorted(ids):
        by.setdefault(ids[n], []).append(n)
    for v, ns in by.items():
        for n in ns[1:]:
            del ids[n]
    used = set(ids.values())
    for n in sorted(names):
        if n in ids:
            continue
        salt = 0
        v = name_id(n)
        while v in used:
            salt += 1
            v = name_id(n, salt)
        ids[n] = v
        src[n] = "name"
        used.add(v)
    return ids, src


def c_ident(path):
    rp = os.path.relpath(path, ROOT)
    return re.sub(r"\W", "_", os.path.splitext(rp)[0])


def write_ids(ids, src):
    lines = ["/* Generated by tools/codemod_fnid.py --gen -- edit by rerunning it.",
             " *",
             " * Canonical code IDs (netplay-v2 R5, docs/DETERMINISM.md \"Code IDs\").",
             " * A function-pointer value in game memory is one of these, never a host",
             " * address: the function's GBA ROM address with the Thumb bit as the ROM",
             " * stores it (from katam.elf), or, for a function the ROM does not have,",
             " * 0x0E000001 | (fnv1a(name) & 0xFFFFFE).  FN(f) is FNID_f as f's pointer",
             " * type; CALL(p) maps one back to the host function (platform/fnid.c). */",
             "#ifndef GUARD_FNID_IDS_H", "#define GUARD_FNID_IDS_H", "",
             "#define PORT_FNID_COUNT %du" % len(ids), ""]
    for n in sorted(ids, key=lambda n: (ids[n], n)):
        lines.append("#define FNID_%s 0x%08Xu%s" % (
            n, ids[n], "" if src.get(n) == "rom" else "  /* name */"))
    lines += ["", "#endif", ""]
    write(IDS_H, "\n".join(lines))


def write_blocks(ids, defs, files):
    """Registration block at the end of each defining file; the table list."""
    owners = {}
    problems = []
    for n in sorted(ids):
        d = defs.get(n)
        if not d:
            problems.append("FN(%s): no definition in the scanned files" % n)
            continue
        fs = sorted(set(f for f, st in d if editable(f) and f.endswith(".c")))
        if len(fs) != 1:
            problems.append("FN(%s): defined in %s" % (n, ", ".join(
                os.path.relpath(f, ROOT) for f, _ in d) or "nothing"))
            continue
        owners.setdefault(fs[0], []).append(n)
    tus = []
    for f in sorted(set(files)):
        text = read(f)
        b = text.find(BEGIN)
        if b >= 0:
            e = text.index(END, b) + len(END)
            text = text[:b].rstrip("\n") + "\n" + text[e:].lstrip("\n")
        ns = owners.get(f)
        if ns:
            tu = c_ident(f)
            tus.append((tu, len(ns)))
            blk = [BEGIN + " this file's functions the game stores as code",
                   " * IDs, so the lookup can name statics (tools/codemod_fnid.py",
                   " * --gen rewrites this block). */",
                   "PORT_FNID_TABLE(%s) {" % tu]
            for n in sorted(ns, key=lambda n: ids[n]):
                blk.append("    PORT_FNID(%s)," % n)
            blk += ["    PORT_FNID_END", "};", END]
            text = text.rstrip("\n") + "\n\n" + "\n".join(blk) + "\n"
        if text != read(f):
            write(f, text)
    out = ["/* Generated by tools/codemod_fnid.py --gen -- edit by rerunning it.",
           " *",
           " * Every file's code-ID registration block (PORT_FNID_TABLE), for",
           " * platform/fnid.c to build its ID -> host function lookup from. */",
           "", "#include \"port/fnid.h\"", "",
           "#ifdef __cplusplus", "extern \"C\" {", "#endif", ""]
    for tu, n in tus:
        out.append("extern const struct PortFnIdEntry gPortFnIds_%s[];" % tu)
    out += ["", "/* extern spelled out: a const at namespace scope is internal in C++. */",
            "extern const struct PortFnIdEntry *const gPortFnIdTables[];",
            "const struct PortFnIdEntry *const gPortFnIdTables[] = {"]
    for tu, n in tus:
        out.append("    gPortFnIds_%s," % tu)
    out += ["    0,", "};", "", "#ifdef __cplusplus", "}", "#endif", ""]
    write(TABLES_C, "\n".join(out))
    return problems


# --- ROM struct entries (rom_fn_tables.c) ------------------------------------

def rom_extra(files):
    """IDs fixed by the console rather than by katam.elf's symbol for the
    name: a stub rom_fn_tables.c registers for ROM fields whose function the
    port does not have, under the value the ROM holds; MultiSioIntr, under the
    IWRAM address it ran from.  Declared next to the function, in a comment:
    `PORT_FNID_ROM(name, 0x...u)`."""
    out = {}
    for p in files:
        for m in re.finditer(r"PORT_FNID_ROM\((\w+), *0x([0-9A-Fa-f]+)u?\)", read(p)):
            out[m.group(1)] = int(m.group(2), 16)
    return out


def header_texts(files):
    return list(files) + sorted(glob.glob(ROOT + "/game/include/**/*.h", recursive=True))


def check_tables(files):
    """Is the generated part current: every FN target has an ID and a
    registration entry, and every registration names an ID?"""
    names = wanted_names(header_texts(files)) | set(rom_extra(files))
    ids = read_ids()
    reg = set()
    for f in files:
        reg |= set(re.findall(r"^\s*PORT_FNID\((\w+)\),", read(f), re.M))
    out = []
    for n in sorted(names - set(ids)):
        out.append("FN(%s) has no ID in game/generated/fnid_ids.h" % n)
    for n in sorted(names - reg):
        out.append("FN(%s) is not in any file's PORT_FNID_TABLE" % n)
    for n in sorted(reg - names):
        out.append("PORT_FNID(%s) is registered but nothing takes FN(%s)" % (n, n))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--gen", action="store_true")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count())
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    if not (a.apply or a.check or a.gen or a.stats):
        a.check = True

    files = [os.path.realpath(f) for f in (a.files or game_files())]
    results = scan_all(files, a.jobs)
    decays, calls, defs, errors, odd, ext = collect(results)
    edits, bad = plan_edits(decays, calls, files if a.files else ())
    nd = sum(1 for es in edits.values() for x in es if x[2] == "FN")
    nc = sum(1 for es in edits.values() for x in es if x[2] == "CALL")

    status = 0
    if a.stats:
        print("unwrapped function decays: %d, unwrapped indirect calls: %d, "
              "files: %d" % (nd, nc, len(edits)))
        for o in odd:
            print("odd: " + o)
        for b in bad:
            print("not editable: " + b)
        for x in ext:
            print("to non-game: " + x)
    if a.check:
        for f, es in sorted(edits.items()):
            for (s, e, kind, name, where) in sorted(es):
                print("%s: %s" % (where.split(": ")[0],
                      "function %s decays to a host pointer; write FN(%s)" % (name, name)
                      if kind == "FN" else "indirect call; write CALL(callee)(args)"))
        for o in odd + bad:
            print(o)
        print("codemod_fnid: %d unwrapped decay(s), %d unwrapped indirect call(s) "
              "in %d file(s)" % (nd, nc, len(files)), file=sys.stderr)
        if nd or nc or odd or bad:
            status = 1
        if not a.files:
            stale = check_tables(files)
            for x in stale:
                print("codemod_fnid: %s -- rerun tools/codemod_fnid.py --gen" % x)
            if stale:
                status = 1
    if a.apply:
        n, problems = apply_edits(edits)
        for p in problems + odd + bad:
            print("codemod_fnid: " + p, file=sys.stderr)
        print("codemod_fnid: %d edit(s) (%d FN, %d CALL) in %d file(s)" %
              (n, nd, nc, len(edits)), file=sys.stderr)
        if problems:
            status = 1
    if a.gen:
        texts = header_texts(files)
        extra = rom_extra(files)
        names = wanted_names(texts) | set(extra)
        ids, src = assign_ids(names, a.elf, extra)
        write_ids(ids, src)
        if any("FNID_" in e for e in errors):
            # the IDs just written were missing from the scan: once more,
            # so the definitions in the files that failed are seen
            decays, calls, defs, errors, odd, ext = collect(scan_all(files, a.jobs))
        problems = write_blocks(ids, defs, files)
        for p in problems:
            print("codemod_fnid: " + p, file=sys.stderr)
        nrom = sum(1 for n in ids if src.get(n) == "rom")
        print("codemod_fnid: %d IDs (%d ROM addresses, %d name-derived)" %
              (len(ids), nrom, len(ids) - nrom), file=sys.stderr)
        if problems:
            status = 1
    if errors and not a.files:
        print("codemod_fnid: %d parse errors (first: %s)" % (len(errors), errors[0]),
              file=sys.stderr)
        status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
