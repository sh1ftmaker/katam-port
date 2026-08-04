#!/usr/bin/env python3
"""
check_fnptrs.py -- catch wasm call_indirect signature mismatches at build time.

On the GBA this whole class of bug does not exist.  Storing

    void sub_0802FE84(struct Unk_0802E57C *a, struct Unk_0802E57C_C *b)

into a `void (*)(void)` slot and calling it through that slot is, on ARM, a
plain `bx` to an address: the two arguments the callee reads are whatever
happened to be left in r0/r1, and a return value nobody wants sits unread in
r0.  The decomp is full of this, faithfully, because the original code did it.
It costs nothing there and the compiler never says a word.

wasm does not work that way.  An indirect call carries the *declared* type of
the call site, the engine compares it against the type of the function the
table entry actually holds, and any difference -- one argument too many, a
return value the site does not expect -- traps.  So the failure mode is:

  * invisible on the original hardware, which is why it is in the source;
  * silent at link time, because wasm-ld is linking a table index, not a type;
  * useless at run time -- the trap says

        RuntimeError: call_indirect to a signature that does not match

    and names neither the function, nor the pointer, nor the call site.  Safari
    drops the wasm frames from the stack entirely, so there is not even a
    module offset to work back from.

The only good place to find these is before they ship, which is what this does:
every point where a named function's address is stored into or passed as a
function-pointer-typed slot is compared against that slot's declared type, in
wasm ABI terms rather than textually -- `void f(struct A *)` and
`void f(struct B *)` are the same wasm type and are not a bug, while
`void f(void)` and `void f(struct Task *)` are two types and are.

The tree it reads is the portified one (build/port-src, produced by
portify.py), not the decomp, because that is what actually gets compiled.
"""

import argparse
import re
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# the wasm ABI type model
# ---------------------------------------------------------------------------
#
# Only four value types exist, and the C types of a GBA decomp collapse into
# them very aggressively.  That collapse is the whole point: it is what makes
# the check report real traps instead of every place two struct tags differ.

STORAGE = ('static', 'extern', 'inline', '__inline__', 'register', 'auto',
           'NAKED', 'NOINLINE', 'UNUSED', 'EWRAM_DATA', 'IWRAM_DATA')
QUALIFIERS = ('const', 'volatile', 'restrict', '__restrict', '__restrict__')

RE_DROP_KEYWORDS = re.compile(r'\b(?:%s)\b' % '|'.join(STORAGE + QUALIFIERS))

# `u16 arg2 __attribute__((unused))`.  The attribute is part of the declarator,
# not the type, and its nested parens confuse everything downstream.
RE_ATTRIBUTE = re.compile(
    r'__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)')

# Everything 32 bits or narrower is an i32, including every pointer -- wasm32
# pointers are indices into linear memory.  The decomp's own typedefs (u8, s32,
# bool32 ...) live here alongside the C keywords.
I32_TYPES = {
    'u8', 's8', 'u16', 's16', 'u32', 's32', 'vu8', 'vs8', 'vu16', 'vs16',
    'vu32', 'vs32', 'bool8', 'bool16', 'bool32', 'int', 'char', 'short',
    'long', 'unsigned', 'signed', 'size_t', 'ptrdiff_t', 'intptr_t',
    'uintptr_t', 'uint8_t', 'int8_t', 'uint16_t', 'int16_t', 'uint32_t',
    'int32_t', 'wchar_t', 'IntrFunc',
}
I64_TYPES = {'u64', 's64', 'vu64', 'vs64', 'uint64_t', 'int64_t'}
F32_TYPES = {'float'}
F64_TYPES = {'double'}

# Words that can only ever be part of a type, never a declarator's name.  Used
# to tell `struct Foo` (an unnamed parameter) from `u32 count` (a named one).
TYPE_ONLY_WORDS = set(QUALIFIERS) | {'struct', 'union', 'enum', 'unsigned',
                                     'signed', 'static', 'extern'}


class Unparsed(Exception):
    """A type this tool declines to guess at -- reported, never assumed."""


def normalise(ctype):
    """Collapse whitespace and drop storage classes and cv-qualifiers.

    `static void` is a return type of `void`.  An earlier version of this check
    treated the whole head as the type, decided `static void` was some unknown
    32-bit thing, and declared every `static void f(struct Task *)` destructor
    in the tree to be returning a value.  Hence this being its own function.
    """
    t = RE_ATTRIBUTE.sub(' ', ctype.replace('\n', ' '))
    t = RE_DROP_KEYWORDS.sub(' ', t)
    return ' '.join(t.split())


def wasm_kind(ctype, index, depth=0):
    """Map a C type to 'i32' / 'i64' / 'f32' / 'f64', or None for void.

    Aggregates passed *by value* get a synthetic 'agg:Tag' kind rather than a
    real wasm type.  Their lowering depends on the struct's own layout, so two
    different tags are genuinely two different wasm signatures and comparing
    the tags is both correct and all we can do without a real front end.
    """
    t = normalise(ctype)

    # A pointer or an array parameter is an i32 no matter what it points at.
    # This is the rule that keeps `struct A *` and `struct B *` equal, and it
    # has to be tested first: `struct Foo *` starts with `struct` too.
    if '*' in t or '[' in t:
        return 'i32'
    if not t or t == 'void':
        return None

    m = re.match(r'^(?:struct|union)\s+(\w+)$', t)
    if m:
        return 'agg:%s' % m.group(1)
    if re.match(r'^enum\b', t):
        return 'i32'

    if t in F32_TYPES:
        return 'f32'
    if t in F64_TYPES:
        return 'f64'
    if t in I64_TYPES:
        return 'i64'
    if t in I32_TYPES:
        return 'i32'

    # `long long`, `unsigned long long`, `unsigned int`, ... -- the only
    # multi-word scalars that survive to here.
    words = t.split()
    if words and all(w in ('unsigned', 'signed', 'int', 'char', 'short', 'long')
                     for w in words):
        return 'i64' if words.count('long') >= 2 else 'i32'

    # A typedef.  Function-pointer typedefs are pointers; everything else is
    # followed to its underlying type.
    if t in index.fnptr_typedefs or t in index.bad_typedefs:
        return 'i32'
    if t in index.typedefs and depth < 8:
        return wasm_kind(index.typedefs[t], index, depth + 1)

    if re.match(r'^\w+$', t):
        # An unknown single-word type. Every remaining typedef in this tree is
        # a 32-bit scalar, but say so out loud rather than assume it quietly.
        index.assumed.add(t)
        return 'i32'
    raise Unparsed('cannot classify type %r' % ctype)


def format_sig(sig):
    """`(i32,i32)->void`, the form used in the report."""
    if sig is None:
        return '<unprototyped>'
    params, result = sig
    return '(%s)->%s' % (','.join(params) or '', result or 'void')


# ---------------------------------------------------------------------------
# declarator parsing
# ---------------------------------------------------------------------------

# A parameter list body.  It may not contain `;` or braces, but it does contain
# parentheses whenever a parameter is itself a function pointer, so two levels
# of nesting are spelled out here.  Newlines are allowed: plenty of the decomp's
# prototypes wrap.
PARAMS = r'(?:[^;{}()]|\((?:[^;{}()]|\([^;{}()]*\))*\))*'

# `void (*unk28)(struct A *, struct B *)`, `void (*const tbl[])(void)`,
# `s32 (*unk0[4])(union AnimCmd, struct Sprite *)`.
RE_FNPTR_DECL = re.compile(
    r'^\s*(?P<ret>[^;{}]*?)\(\s*(?P<stars>\*+)\s*(?:(?:const|volatile)\s+)*'
    r'(?P<name>\w+)?\s*(?P<arr>(?:\[[^\]]*\]\s*)*)\)\s*'
    r'\((?P<params>' + PARAMS + r')\)\s*$')


def split_top_level(text, sep=','):
    """Split at `sep` outside any (), [] or {}."""
    out, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in '([{':
            depth += 1
        elif ch in ')]}':
            depth -= 1
        elif ch == sep and depth == 0:
            out.append(text[start:i])
            start = i + 1
    out.append(text[start:])
    return out


def param_type(decl):
    """Strip a parameter's name, leaving its type.

    `struct Foo *p` -> `struct Foo *`, `u32 count` -> `u32`, but `struct Foo`
    (an unnamed parameter) must come back untouched -- its last word is the
    tag, not a name.
    """
    d = ' '.join(RE_ATTRIBUTE.sub(' ', decl).split())
    if not d:
        return d
    if '(' in d:
        return d                       # a function-pointer parameter: a pointer
    d = re.sub(r'\[[^\]]*\]', '[]', d)
    m = re.search(r'\b(\w+)\s*((?:\[\])*)$', d)
    if m:
        head = d[:m.start(1)].strip()
        words = [w for w in re.split(r'[\s*]+', head) if w]
        if words and not set(words) <= TYPE_ONLY_WORDS:
            return (head + ' ' + m.group(2)).strip()
    return d


def parse_params(text, index):
    """Parameter list text -> tuple of wasm kinds."""
    parts = [p.strip() for p in split_top_level(text)]
    if len(parts) == 1 and not parts[0]:
        # `f()` -- no prototype at all.  C says "unspecified arguments", the
        # port's compiler says whatever the first call site said, and guessing
        # is exactly how a mismatch gets missed.
        raise Unparsed('unprototyped parameter list')
    if len(parts) == 1 and normalise(parts[0]) == 'void':
        return ()
    kinds = []
    for part in parts:
        if part == '...':
            raise Unparsed('variadic')
        kind = wasm_kind(param_type(part), index)
        if kind is None:
            raise Unparsed('void parameter among others')
        kinds.append(kind)
    return tuple(kinds)


def parse_signature(ret, params, index):
    """(return type text, parameter list text) -> Sig."""
    return (parse_params(params, index), wasm_kind(ret, index))


def parse_fnptr(decl, index, stars=1):
    """A declaration text -> (name, Sig) if it declares a function pointer.

    `stars` is how many `*` the declarator must have to count.  One is the
    normal slot; two is a pointer *to* a slot, which intro.c writes through
    (`bool32 (**p)(struct Unk_08145B64 *); ... *p = f;`), and the slot it
    reaches is the same one either way.

    Returns None when the declaration is not a function pointer of that depth;
    raises Unparsed when it is one but its types cannot be pinned down.
    """
    m = RE_FNPTR_DECL.match(decl.strip().rstrip(';'))
    if not m or len(m.group('stars')) != stars:
        return None
    return m.group('name'), parse_signature(m.group('ret'), m.group('params'), index)


# ---------------------------------------------------------------------------
# reading the tree
# ---------------------------------------------------------------------------

def blank_comments(text):
    """Replace comment and literal bodies with spaces, preserving every offset.

    Line numbers and slice offsets stay valid against the original text, so the
    report can quote the real source line.  Blanking rather than deleting also
    stops `/* 0x28 */` field-offset comments from gluing tokens together.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            for k in range(i + 1, j - 1):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        else:
            i += 1
    return ''.join(out)


def match_brace(text, i):
    """Index just past the `}` closing the `{` at text[i]."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return j + 1
    return len(text)


# `static void sub_0800EC78(struct Task *t)` followed by `{` -- possibly on the
# next line, which is the shape half the destructors in the tree use.  `head`
# may not contain a newline, a paren or an `=`: that is what stops the regex
# from mistaking `while (x) {` (no return type) or a function-pointer table
# definition (parens before the name) for a definition.
RE_FUNC_DEF = re.compile(
    r'^(?P<head>[A-Za-z_][^;{}()\n=,]*?[\s*])(?P<name>\w+)\s*'
    r'\((?P<params>' + PARAMS + r')\)\s*'
    r'(?:__attribute__\s*\(\([^()]*(?:\([^()]*\)[^()]*)*\)\)\s*)?\{', re.M)

RE_FUNC_PROTO = re.compile(
    r'^(?P<head>(?:extern\s+)?[A-Za-z_][^;{}()\n=,]*?[\s*])(?P<name>\w+)\s*'
    r'\((?P<params>' + PARAMS + r')\)\s*;', re.M)

RE_TYPEDEF = re.compile(r'\btypedef\b(?P<body>[^;{}]*);')

RE_AGGREGATE = re.compile(r'\b(?P<kw>struct|union)\s+(?P<tag>\w+)?\s*\{')

# `extern struct Task* gCurTask;`.  Deliberately run against the *raw* text:
# gen_ram_symbols.py comments these out once it has turned them into address
# macros, and the commented-out line is still the only place the port records
# what type the symbol has.
RE_EXTERN = re.compile(r'\bextern\s+(?P<body>[^;{}]*);')


class Field:
    __slots__ = ('ctype', 'sig', 'where')

    def __init__(self, ctype, sig, where):
        self.ctype = ctype
        self.sig = sig            # None unless this field is a function pointer
        self.where = where


class Index:
    def __init__(self):
        self.typedefs = {}        # name -> underlying type text
        self.fnptr_typedefs = {}  # name -> Sig
        self.bad_typedefs = {}    # name -> reason
        self.structs = {}         # tag -> {field name: Field}
        self.funcs = {}           # name -> (Sig, 'file:line')
        self.bad_funcs = {}       # name -> reason
        self.globals = {}         # name -> type text
        self.fnptr_args = {}      # function -> {arg index: (param name, Sig)}
        self.assumed = set()      # unknown type names taken to be 32-bit
        self.bad_slots = []       # (where, text, reason)

    def signature_of(self, name):
        return self.funcs.get(name, (None, None))[0]


def collect_typedefs(index, files):
    """Two passes: gather the texts, then classify them.

    A typedef can name another typedef declared later in a different header, so
    nothing can be resolved until all of them have been seen.
    """
    raw = {}
    for where, text in files:
        for m in RE_TYPEDEF.finditer(text):
            body = ' '.join(m.group('body').split())
            fp = RE_FNPTR_DECL.match(body)
            if fp and fp.group('name'):
                raw[fp.group('name')] = ('fnptr', body, where, m.start())
                continue
            # `typedef unsigned int u32;` -- last word is the new name.
            n = re.search(r'\b(\w+)\s*((?:\[[^\]]*\])*)$', body)
            if n and n.start(1) > 0:
                raw[n.group(1)] = ('plain', body[:n.start(1)].strip(), where, m.start())

    for name, (kind, body, where, off) in raw.items():
        if kind == 'plain':
            index.typedefs[name] = body

    for name, (kind, body, where, off) in raw.items():
        if kind != 'fnptr':
            continue
        try:
            fp = RE_FNPTR_DECL.match(body)
            index.fnptr_typedefs[name] = parse_signature(
                fp.group('ret'), fp.group('params'), index)
        except Unparsed as exc:
            index.bad_typedefs[name] = str(exc)
            index.bad_slots.append((where, 'typedef %s' % name, str(exc)))


def member_slot(index, member, where):
    """A struct member text -> (name, ctype, Sig or None), or None to skip."""
    member = ' '.join(RE_ATTRIBUTE.sub(' ', member).split())
    if not member or member.startswith('#'):
        return None
    try:
        fp = parse_fnptr(member, index)
    except Unparsed as exc:
        index.bad_slots.append((where, member[:70], str(exc)))
        return None
    if fp:
        return fp[0], member, fp[1]

    # A plain member.  It still matters: `struct Sprite unk0;` is how a chain
    # like `b->unk0.unk28` gets from one struct to the next.  And if its type is
    # a function-pointer typedef, the member is a slot after all.
    m = re.search(r'\b(\w+)\s*((?:\[[^\]]*\])*)$', member)
    if not m or m.start(1) == 0:
        return None
    name = m.group(1)
    ctype = member[:m.start(1)].strip()
    base = normalise(ctype)
    sig = index.fnptr_typedefs.get(base) if '*' not in base else None
    return name, ctype, sig


def collect_structs(index, files):
    """Field tables for every struct and union, keyed by tag."""
    for where, text in files:
        for m in RE_AGGREGATE.finditer(text):
            end = match_brace(text, text.index('{', m.start()))
            body = text[text.index('{', m.start()) + 1:end - 1]
            tags = [m.group('tag')] if m.group('tag') else []
            # `typedef struct { ... } Name;` -- the name comes after the brace.
            trailer = text[end:text.find(';', end) + 1 if ';' in text[end:end + 200] else end]
            for t in re.findall(r'\b(\w+)\b', trailer):
                if text[:m.start()].rstrip().endswith('typedef') or \
                        re.search(r'\btypedef\b[^;{}]*$', text[max(0, m.start() - 200):m.start()]):
                    tags.append(t)
            if not tags:
                continue

            fields = {}
            # Split the body at top-level `;`, skipping nested aggregates --
            # their members are merged in so that anonymous unions stay
            # transparent, which is how `data.h`'s `union { void (*ptr)(...) }`
            # is reachable at all.
            depth, start = 0, 0
            chunks = []
            for i, ch in enumerate(body):
                if ch in '{([':
                    depth += 1
                elif ch in '})]':
                    depth -= 1
                elif ch == ';' and depth == 0:
                    chunks.append(body[start:i])
                    start = i + 1
            for chunk in chunks:
                if '{' in chunk:
                    inner = chunk[chunk.index('{') + 1:chunk.rindex('}')]
                    inner_fields = {}
                    for sub in split_top_level(inner, ';'):
                        got = member_slot(index, sub, where)
                        if got:
                            inner_fields[got[0]] = Field(got[1], got[2], where)
                    name = re.search(r'\b(\w+)\s*$', chunk[chunk.rindex('}') + 1:])
                    if name:
                        # `union { u32 data; void (*ptr)(...); } unkE4;` -- the
                        # members are reached through `unkE4`, not merged in, so
                        # the inner type needs a tag of its own to point at.
                        tag = '__anon_%d' % len(index.structs)
                        index.structs[tag] = inner_fields
                        fields[name.group(1)] = Field('struct %s' % tag, None, where)
                    else:
                        fields.update(inner_fields)   # a transparent anonymous member
                    continue
                got = member_slot(index, chunk, where)
                if got:
                    fields[got[0]] = Field(got[1], got[2], where)

            for tag in tags:
                index.structs.setdefault(tag, {}).update(fields)


def line_of(text, off):
    return text.count('\n', 0, off) + 1


def collect_functions(index, files, sources):
    """Real signatures, from definitions first and prototypes only as fallback."""
    protos = {}
    for where, text in files:
        for m in RE_FUNC_PROTO.finditer(text):
            if m.group('name') in NOT_FUNCTIONS:
                continue
            try:
                sig = parse_signature(m.group('head'), m.group('params'), index)
            except Unparsed as exc:
                protos.setdefault(m.group('name'), ('bad', str(exc), where, m.start()))
                continue
            protos.setdefault(m.group('name'),
                              ('ok', sig, where, m.start()))

    for where, text in sources:
        for m in RE_FUNC_DEF.finditer(text):
            name = m.group('name')
            if name in NOT_FUNCTIONS:
                continue
            site = '%s:%d' % (where, line_of(text, m.start()))
            try:
                sig = parse_signature(m.group('head'), m.group('params'), index)
            except Unparsed as exc:
                index.bad_funcs[name] = '%s (%s)' % (exc, site)
                continue
            if name in index.funcs and index.funcs[name][0] != sig:
                # Two definitions that disagree: never guess which one links.
                index.bad_funcs[name] = (
                    'two definitions disagree: %s at %s vs %s at %s'
                    % (format_sig(index.funcs[name][0]), index.funcs[name][1],
                       format_sig(sig), site))
                continue
            index.funcs[name] = (sig, site)

    for name, entry in protos.items():
        if name in index.funcs:
            continue
        if entry[0] == 'ok':
            index.funcs[name] = (entry[1], '%s:%d (prototype)'
                                 % (entry[2], line_of_in(entry[2], entry[3], files)))
        else:
            index.bad_funcs.setdefault(name, '%s (prototype in %s)' % (entry[1], entry[2]))


def line_of_in(where, off, files):
    for w, text in files:
        if w == where:
            return line_of(text, off)
    return 0


def collect_fnptr_args(index, files):
    """Which parameters of which functions are function-pointer slots.

    `TaskCreate(TaskMain, u16, u16, u16, TaskDestructor)` is the one that
    matters -- almost every task in the game is created through it, and both of
    its function-pointer parameters take a bare function name at the call site.
    """
    for where, text in files:
        for m in list(RE_FUNC_PROTO.finditer(text)) + list(RE_FUNC_DEF.finditer(text)):
            name = m.group('name')
            if name in index.fnptr_args or name in NOT_FUNCTIONS:
                continue
            slots = {}
            for i, part in enumerate(split_top_level(m.group('params'))):
                part = ' '.join(part.split())
                if not part:
                    continue
                try:
                    fp = parse_fnptr(part, index)
                except Unparsed as exc:
                    index.bad_slots.append((where, '%s arg %d' % (name, i), str(exc)))
                    continue
                if fp:
                    slots[i] = (fp[0] or 'arg%d' % i, fp[1])
                    continue
                base = normalise(param_type(part))
                if base in index.fnptr_typedefs:
                    pname = re.search(r'\b(\w+)\s*$', part)
                    slots[i] = ('%s %s' % (base, pname.group(1)) if pname and
                                pname.group(1) != base else base,
                                index.fnptr_typedefs[base])
                elif base in index.bad_typedefs:
                    index.bad_slots.append(
                        (where, '%s arg %d (%s)' % (name, i, base),
                         index.bad_typedefs[base]))
            if slots:
                index.fnptr_args[name] = slots


def collect_globals(index, files):
    """File-scope variable types, so `gCurTask->main` can be resolved."""
    for where, text in files:
        for m in RE_EXTERN.finditer(text):
            body = ' '.join(m.group('body').split())
            if '(' in body:
                fp = RE_FNPTR_DECL.match(body)
                if fp and fp.group('name'):
                    index.globals.setdefault(fp.group('name'), body)
                continue
            for part in split_top_level(body):
                n = re.search(r'\b(\w+)\s*((?:\[[^\]]*\])*)$', part)
                if n and n.start(1) > 0:
                    index.globals.setdefault(
                        n.group(1), part[:n.start(1)].strip() + ' ' + n.group(2))


# ---------------------------------------------------------------------------
# resolving what a piece of source is assigning to
# ---------------------------------------------------------------------------

# One declaration statement inside a function body, used to type local
# variables.  Only the shapes the decomp actually writes are handled.
# The `\s*(?=\*)` alternative is not cosmetic: the decomp writes
# `struct Object2* obj = ...` with the star glued to the tag, and requiring
# whitespace after the type made the regex fall back to its `\w+` branch,
# read the type as `struct`, and hand `struct *` to the struct lookup.
RE_LOCAL_DECL = re.compile(
    r'^[ \t]*(?P<type>(?:(?:const|volatile|static|register|unsigned|signed)\s+)*'
    r'(?:(?:struct|union|enum)\s+\w+|\w+))(?:\s+|\s*(?=\*))'
    r'(?P<rest>\**\s*\w[^;{}]*?)\s*;', re.M)

RE_CONTROL = re.compile(r'^(?:if|for|while|switch|return|do|else|case|goto)$')

# Things that look like a declaration but are macros the decomp writes at the
# start of one -- `ALIGNED(4) static const u8 sTiles[] = ...`.
NOT_FUNCTIONS = {'ALIGNED', 'sizeof', 'if', 'for', 'while', 'switch', 'return',
                 'do', 'else', 'defined', '__attribute__'}


class Function:
    """A definition, plus the local variable types its body declares."""

    def __init__(self, name, start, end, params, text):
        self.name = name
        self.start = start
        self.end = end
        self._text = text
        self._params = params
        self._locals = None

    @property
    def locals(self):
        if self._locals is None:
            self._locals = {}
            for part in split_top_level(RE_ATTRIBUTE.sub(' ', self._params)):
                part = ' '.join(part.split())
                n = re.search(r'\b(\w+)\s*((?:\[[^\]]*\])*)$', part)
                if n and n.start(1) > 0 and '(' not in part:
                    self._locals[n.group(1)] = part[:n.start(1)].strip()
                elif '(' in part:
                    fp = RE_FNPTR_DECL.match(part)
                    if fp and fp.group('name'):
                        self._locals[fp.group('name')] = part
            body = self._text[self.start:self.end]
            for m in RE_LOCAL_DECL.finditer(body):
                base = m.group('type')
                if RE_CONTROL.match(base.split()[-1]):
                    continue
                # `struct Object2 *obj2 = TaskGetStructPtr(t), *obj = obj2;`
                # declares two variables, and the second one is the one every
                # `obj->field = fn` in the function then goes through.  Each
                # declarator is cut at its own initialiser.
                for part in split_top_level(m.group('rest')):
                    part = split_top_level(part, '=')[0].strip()
                    n = re.search(r'\b(\w+)\s*((?:\[[^\]]*\])*)$', part)
                    if n:
                        stars = part[:n.start(1)].count('*')
                        self._locals.setdefault(
                            n.group(1), base + ' ' + '*' * stars + n.group(2))
            # Locally declared function pointers.  A trailing macro call is
            # trimmed first: the decomp pins these to registers
            # (`bool32 (**p)(struct X *) ASM_PIN("r1");`) and the pin sits
            # after the declarator, where it looks like a parameter list.
            for m in re.finditer(r'^[ \t]*(?P<decl>[^;{}\n]*\([^;{}\n]*\)[^;{}\n]*);',
                                 body, re.M):
                decl = ' '.join(m.group('decl').split())
                decl = re.sub(r'\s*\b\w+\s*\([^()]*\)\s*$', '', decl) \
                    if not RE_FNPTR_DECL.match(decl) else decl
                fp = RE_FNPTR_DECL.match(decl)
                if fp and fp.group('name'):
                    self._locals.setdefault(fp.group('name'), decl)
        return self._locals


def struct_tag(ctype, index):
    """The struct/union tag a (possibly pointer, possibly typedef'd) type names."""
    t = normalise(ctype).replace('*', ' ').replace('[', ' ').replace(']', ' ')
    t = ' '.join(t.split())
    m = re.match(r'^(?:struct|union)\s+(\w+)$', t)
    if m:
        return m.group(1)
    for _ in range(8):
        if t in index.structs:
            return t
        if t in index.typedefs:
            t = normalise(index.typedefs[t]).replace('*', ' ').strip()
            m = re.match(r'^(?:struct|union)\s+(\w+)$', t)
            if m:
                return m.group(1)
            continue
        break
    return None


def resolve_slot(lhs, func, index):
    """An assignment target -> (description, Sig), or (None, reason).

    Handles `f`, `b->unk28`, `b->unk0.unk28`, `s->unk0[2]` and `gCurTask->main`.
    """
    # `((struct Unk_08039E04 *)t)->unk0 = f;` -- the cast *is* the type, and it
    # is the only type available: `t` is a `void *` from TaskGetStructPtr.
    derefs = len(lhs) - len(lhs.lstrip('* '))
    derefs = lhs[:derefs].count('*')
    lhs = lhs.lstrip('* ')

    cast = re.match(r'^\(\s*\((?P<type>[^()]*)\)\s*[A-Za-z_]\w*\s*\)(?P<rest>.*)$',
                    lhs)
    if cast:
        ctype, tail = cast.group('type'), cast.group('rest')
        parts = ['<cast>'] + [p.strip() for p in re.split(r'->|\.', tail) if p.strip()]
    else:
        parts = [p.strip() for p in re.split(r'->|\.', lhs) if p.strip()]
        if not parts:
            return None, 'empty target'
        base = re.sub(r'\[.*$', '', parts[0]).strip()

        ctype = None
        if func is not None:
            ctype = func.locals.get(base)
        if ctype is None:
            ctype = index.globals.get(base)
        if ctype is None:
            return None, 'cannot type `%s`' % base

    if len(parts) == 1:
        base_type = normalise(ctype)
        if base_type in index.fnptr_typedefs and not derefs:
            return '%s %s' % (base_type, base), index.fnptr_typedefs[base_type]
        try:
            fp = parse_fnptr(ctype, index, stars=derefs + 1)
        except Unparsed as exc:
            return None, str(exc)
        if fp:
            return '%s%s (local)' % ('*' * derefs, base), fp[1]
        return None, None            # not a function-pointer slot at all

    tag = None
    for field in parts[1:]:
        field = re.sub(r'\[.*$', '', field).strip()
        tag = struct_tag(ctype, index)
        if tag is None or tag not in index.structs:
            return None, 'unknown struct for `%s`' % ctype.strip()
        info = index.structs[tag].get(field)
        if info is None:
            return None, 'struct %s has no field `%s`' % (tag, field)
        ctype, sig = info.ctype, info.sig

    if sig is None:
        return None, None            # a real field, just not a function pointer
    return 'struct %s::%s' % (tag, parts[-1].split('[')[0]), sig


# ---------------------------------------------------------------------------
# the scan
# ---------------------------------------------------------------------------

# `x = fn;`, `b->unk28 = sub_0802FE84;`, `t->dtor = &Foo;`, `p = (TaskMain)f,`.
# The lookbehind is what keeps `==`, `!=`, `<=`, `+=` and friends out.
RE_ASSIGN = re.compile(
    r'(?<![.\w>])'
    r'(?P<lhs>\**(?:\(\s*\([^()]*\)\s*[A-Za-z_]\w*\s*\)|[A-Za-z_]\w*)'
    r'(?:\s*(?:->|\.)\s*\w+|\s*\[[^\[\]]*\])*)'
    r'\s*(?<![=!<>+\-*/%&|^])=(?!=)\s*'
    r'(?P<cast>\(\s*[A-Za-z_][^()]*\)\s*)?&?\s*'
    r'(?P<rhs>[A-Za-z_]\w*)\s*(?=[;,)}])')

RE_CALL = re.compile(r'\b(?P<name>\w+)\s*\((?P<args>' + PARAMS + r')\)')

# `void (*const gUnk_082DDEF0[])(struct Unk_02038590 *) = {`,
# `static const TaskMain sTable[] = {`, and struct tables whose members are
# filled in by name (`.unk8 = sub_08146D80,`), which is how intro.c builds its
# state tables.
RE_TABLE_DEF = re.compile(r'^[ \t]*(?P<decl>[A-Za-z_][^;{}=]*?)\s*=\s*\{', re.M)

RE_DESIGNATED = re.compile(
    r'\.\s*(?P<field>\w+)\s*=\s*(?:\(\s*[A-Za-z_][^()]*\)\s*)?&?\s*'
    r'(?P<value>[A-Za-z_]\w*)\s*(?=[,}])')

# `(TaskDestructor)sub_08002E3C` as an argument.  The cast silences the C
# compiler and changes nothing at all for wasm: the table still holds a
# function of the wrong type, and TaskDestroy still calls it with one argument.
# Casts are therefore stripped and the argument checked as if bare.
RE_ARGUMENT = re.compile(r'^(?:\(\s*[A-Za-z_][^()]*\)\s*)?&?\s*([A-Za-z_]\w*)$')


class Report:
    def __init__(self):
        self.mismatches = []      # (site, slot, slot sig, function, fn sig, kind)
        self.unresolved = []      # (site, text, reason)
        self.checked = 0

    def check(self, site, slot, slot_sig, fname, index, context):
        """Compare one address-taken function against one slot."""
        self.checked += 1
        if fname in index.bad_funcs:
            self.unresolved.append(
                (site, '%s -> %s' % (context, fname), index.bad_funcs[fname]))
            return
        real = index.signature_of(fname)
        if real is None:
            self.unresolved.append(
                (site, '%s -> %s' % (context, fname),
                 'no definition or prototype for %s' % fname))
            return
        if real != slot_sig:
            self.mismatches.append(
                (site, slot, format_sig(slot_sig), fname, format_sig(real)))


def index_functions(text, where):
    out = []
    for m in RE_FUNC_DEF.finditer(text):
        brace = text.index('{', m.end() - 1) if text[m.end() - 1] != '{' else m.end() - 1
        out.append(Function(m.group('name'), brace, match_brace(text, brace),
                            m.group('params'), text))
    return out


def scan_source(where, text, index, report):
    """Every address-take in one .c file."""
    funcs = index_functions(text, where)

    def enclosing(off):
        for f in funcs:
            if f.start <= off < f.end:
                return f
        return None

    # --- assignments -------------------------------------------------------
    for m in RE_ASSIGN.finditer(text):
        rhs = m.group('rhs')
        if rhs not in index.funcs and rhs not in index.bad_funcs:
            continue
        func = enclosing(m.start())
        # A local variable that happens to share a function's name is not an
        # address-take.  (`f = sub_0802F77C; b->unk28 = f;` -- the second line
        # is copying a variable, and only the first stores an address.)
        if func is not None and rhs in func.locals:
            continue
        lhs = ' '.join(m.group('lhs').split())
        site = '%s:%d' % (where, line_of(text, m.start()))
        slot, sig = resolve_slot(lhs, func, index)
        if slot is None:
            if sig is not None:
                report.unresolved.append((site, '%s = %s' % (lhs, rhs), sig))
            continue
        report.check(site, slot, sig, rhs, index, lhs)

    # --- arguments ---------------------------------------------------------
    for m in RE_CALL.finditer(text):
        slots = index.fnptr_args.get(m.group('name'))
        if not slots:
            continue
        func = enclosing(m.start())
        for i, arg in enumerate(split_top_level(m.group('args'))):
            if i not in slots:
                continue
            arg = arg.strip()
            a = RE_ARGUMENT.match(arg)
            if not a:
                continue
            name = a.group(1)
            if func is not None and name in func.locals:
                continue
            if name not in index.funcs and name not in index.bad_funcs:
                continue
            pname, sig = slots[i]
            report.check('%s:%d' % (where, line_of(text, m.start())),
                         '%s(%s)' % (m.group('name'), pname), sig, name, index,
                         '%s arg %d' % (m.group('name'), i))

    # --- tables ------------------------------------------------------------
    for m in RE_TABLE_DEF.finditer(text):
        decl = ' '.join(m.group('decl').split())
        try:
            fp = parse_fnptr(decl, index)
        except Unparsed as exc:
            report.unresolved.append(
                ('%s:%d' % (where, line_of(text, m.start())), decl[:70], str(exc)))
            continue
        n = re.search(r'\b(\w+)\s*((?:\[[^\]]*\])*)$', decl)
        if fp:
            name, sig = fp
        elif n and n.start(1) > 0 and normalise(decl[:n.start(1)]) in index.fnptr_typedefs:
            name = n.group(1)
            sig = index.fnptr_typedefs[normalise(decl[:n.start(1)])]
        else:
            # Not a table of function pointers.  It may still be a table of
            # structs that have function-pointer members, filled in by name.
            if n and n.start(1) > 0:
                scan_designated(where, text, m.start(),
                                struct_tag(decl[:n.start(1)], index), index, report)
            continue

        open_brace = text.index('{', m.start())
        body = text[open_brace + 1:match_brace(text, open_brace) - 1]
        for element in re.finditer(r'(?<![\w.>])&?\s*([A-Za-z_]\w*)\s*(?=[,}]|$)', body):
            fname = element.group(1)
            if fname not in index.funcs and fname not in index.bad_funcs:
                continue
            report.check('%s:%d' % (where, line_of(text, open_brace + element.start())),
                         '%s[]' % name, sig, fname, index, name)


def scan_designated(where, text, start, tag, index, report):
    """`.unk8 = sub_08146D80,` inside a braced initialiser of struct type."""
    open_brace = text.index('{', start)
    body = text[open_brace + 1:match_brace(text, open_brace) - 1]
    if not any(f.sig for f in index.structs.get(tag, {}).values()):
        # No function-pointer member anywhere in this struct, so any bare name
        # in the initialiser is data, not an address-take.  Nested structs get
        # their own pass when the initialiser names them.
        nested = [struct_tag(f.ctype, index)
                  for f in index.structs.get(tag, {}).values()]
        if not any(any(g.sig for g in index.structs.get(t, {}).values())
                   for t in nested if t):
            return
    for m in RE_DESIGNATED.finditer(body):
        fname = m.group('value')
        if fname not in index.funcs and fname not in index.bad_funcs:
            continue
        site = '%s:%d' % (where, line_of(text, open_brace + m.start()))
        info = index.structs.get(tag, {}).get(m.group('field'))
        if info is None:
            # A member of a struct nested one level down, named directly in a
            # nested brace pair.
            for f in index.structs.get(tag, {}).values():
                sub = index.structs.get(struct_tag(f.ctype, index) or '', {})
                if m.group('field') in sub:
                    info = sub[m.group('field')]
                    break
        if info is None:
            report.unresolved.append(
                (site, '.%s = %s' % (m.group('field'), fname),
                 'struct %s has no field `%s`' % (tag, m.group('field'))))
            continue
        if info.sig is None:
            report.unresolved.append(
                (site, '.%s = %s' % (m.group('field'), fname),
                 'field `%s` is `%s`, not a function pointer'
                 % (m.group('field'), ' '.join(info.ctype.split())[:40])))
            continue
        report.check(site, 'struct %s::%s' % (tag, m.group('field')), info.sig,
                     fname, index, '.%s' % m.group('field'))


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Find wasm call_indirect signature mismatches statically.')
    ap.add_argument('--tree', type=Path,
                    default=Path(__file__).resolve().parent.parent / 'build' / 'port-src',
                    help='portified source tree (default: build/port-src)')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='list every unresolved slot and assumed type')
    args = ap.parse_args()

    if not (args.tree / 'include').is_dir():
        sys.exit('error: %s does not look like a portified tree (run `make sync`)'
                 % args.tree)

    root = args.tree
    sources = sorted(list((root / 'src').rglob('*.c'))
                     + list((root / 'src').rglob('*.inc.c')))
    headers = sorted(list((root / 'include').rglob('*.h'))
                     + list((root / 'src').rglob('*.h')))

    # Read once.  Comments are blanked rather than removed so that offsets --
    # and therefore reported line numbers -- match the file on disk.
    def load(paths):
        out = []
        for p in paths:
            raw = p.read_text(errors='replace')
            out.append((str(p.relative_to(root)), blank_comments(raw), raw))
        return out

    src = load(sources)
    hdr = load(headers)
    stripped = [(w, t) for w, t, _ in hdr + src]
    src_stripped = [(w, t) for w, t, _ in src]

    index = Index()
    collect_typedefs(index, stripped)
    collect_structs(index, stripped)
    collect_functions(index, stripped, src_stripped)
    collect_fnptr_args(index, stripped)
    # Globals come from the raw text: gen_ram_symbols.py comments out the
    # `extern` line once it has an address macro, and that comment is still the
    # only record of the symbol's type.
    collect_globals(index, [(w, raw) for w, _, raw in hdr + src])

    report = Report()
    for where, text in src_stripped:
        scan_source(where, text, index, report)

    slots = (len(index.fnptr_typedefs)
             + sum(1 for f in index.structs.values() for x in f.values() if x.sig)
             + sum(len(v) for v in index.fnptr_args.values()))

    print('check_fnptrs: %s' % root)
    print('  %6d function-pointer slots inspected '
          '(%d typedefs, %d struct fields, %d parameters)'
          % (slots, len(index.fnptr_typedefs),
             sum(1 for f in index.structs.values() for x in f.values() if x.sig),
             sum(len(v) for v in index.fnptr_args.values())))
    print('  %6d function signatures known' % len(index.funcs))
    print('  %6d address-takes checked' % report.checked)
    print('  %6d MISMATCHES' % len(report.mismatches))
    print('  %6d unresolved' % (len(report.unresolved) + len(index.bad_slots)))

    if report.mismatches:
        print()
        print('%-42s %-34s %-22s %-26s %s'
              % ('SITE', 'SLOT', 'SLOT SIGNATURE', 'FUNCTION', 'ITS SIGNATURE'))
        print('-' * 150)
        for site, slot, slot_sig, fname, fn_sig in sorted(report.mismatches):
            print('%-42s %-34s %-22s %-26s %s'
                  % (site, slot, slot_sig, fname, fn_sig))

    unresolved = ([('%s' % w, t, r) for w, t, r in index.bad_slots]
                  + report.unresolved)
    if unresolved:
        print()
        print('unresolved (%d) -- neither cleared nor reported as a mismatch:'
              % len(unresolved))
        shown = unresolved if args.verbose else unresolved[:20]
        for site, what, why in shown:
            print('    %-46s %-40s %s' % (site, what[:40], why))
        if len(shown) < len(unresolved):
            print('    ... %d more (-v for all)' % (len(unresolved) - len(shown)))

    if index.assumed and args.verbose:
        print()
        print('type names assumed to be 32-bit (%d): %s'
              % (len(index.assumed), ', '.join(sorted(index.assumed))))

    return 1 if report.mismatches else 0


if __name__ == '__main__':
    sys.exit(main())
