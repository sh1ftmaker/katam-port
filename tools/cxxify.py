"""Make the decompilation's C acceptable to a C++ compiler.

The port needs this because of pointer width, not because anybody wanted C++.
A 64-bit build has to keep the GBA's 4-byte pointer members -- see
docs/SIXTYFOUR.md -- and C cannot express a 4-byte thing that still behaves
like a pointer at every use site.  C++ can, in about thirty lines
(platform/port/p32.h), and that turns a rewrite of ~40,000 member accesses
into a rewrite of ~285 member declarations.

So this module exists to get the game's own sources through a C++ front end.
Everything here is a transform that is **also valid C**, and that is the point:
the ILP32 builds keep compiling the same tree as C and their output must not
move, which is what makes the transforms testable.  A transform that needed
`#ifdef __cplusplus` would not be, and there are none.

The six differences that actually came up, measured across all 156 game
translation units:

  1. `template` is a parameter name in 41 files (185 uses).  Renamed.
     This one cascades: the parameter list fails to parse, so every later
     parameter is undeclared too, which is where "'a2' was not declared in
     this scope" comes from.  185 renames removed 272 errors.

  2. A struct or union tag defined inside another struct has file scope in C
     and class scope in C++.  6 definitions in 2 headers, and they accounted
     for 261 errors, because every function that names the type then declares
     a fresh incomplete one of its own.  Hoisted to file scope, which is where
     C already puts them -- this changes nothing for the C builds.

  3. `[0 ... 3] = X` is a GNU C range designator with no C++ spelling.
     19 sites, all in one array in one file.  Expanded positionally, not to
     `[0] = X`: g++ in C++ mode rejects the general designator form with
     "sorry, unimplemented: non-trivial designated initializers" and only
     accepts contiguous in-order ones, which it lowers to positional anyway.

  4. `void f();` means "unspecified parameters" in C and "no parameters" in
     C++.  Where a header already declares the function properly, the local
     redundant declaration is dropped rather than patched -- patching it to
     `(...)` would make it an *overload* in C++ rather than a redeclaration,
     which is worse than the disease.

  5. A trailing `arr[0]` member with a brace initializer is the GNU
     flexible-array idiom and C++ has no form of it.  Rewritten to a shadow
     struct plus a macro, which preserves the layout exactly; see
     expand_flex_array_objects.

  6. `enum_value++` has no built-in meaning in C++.  Not handled here: it is
     two sites and platform/port/p32.h defines the operator, so no source
     changes.  A codemod that rewrote it would have to know the enum's type.

Every transform reports through the same Report object portify.py uses, and
every one of them complains loudly if its pattern stops matching, because the
decompilation moves and a silently-skipped transform here is a build failure
several minutes later in a file that looks unrelated.
"""

import re

# ---------------------------------------------------------------------------
# 1. C++ keywords used as identifiers
# ---------------------------------------------------------------------------
# The full keyword list was swept over the tree rather than guessed at; the
# only ones that appear at all are `template` (a parameter name, 41 files),
# `asm` (only ever in comments) and `or` (inside an #error message).  So the
# rename below is the whole of it, and the sweep is worth re-running if this
# ever stops being true -- see the docstring in tools/cxxify_scan_keywords.
CXX_KEYWORD_RENAMES = {
    'template': 'template_',
}

_IDENT_USE = r'(?<![\w.>])%s\b(?!\s*:)'


def rename_cxx_keywords(text, rep):
    """Rename identifiers that are keywords in C++ but not in C.

    The lookbehind keeps `->template` and `.template` alone -- those would be
    member names, and a member named `template` would need the *declaration*
    renamed too.  None exist today; if one appears, it shows up as a compile
    error rather than as a silent miss, which is the right way round.
    """
    for kw, new in CXX_KEYWORD_RENAMES.items():
        pat = re.compile(_IDENT_USE % kw)
        text, n = pat.subn(new, text)
        if n:
            rep.bump('C++ keyword identifiers renamed', n)
    return text


# ---------------------------------------------------------------------------
# 2. nested struct/union tags
# ---------------------------------------------------------------------------

_TAG_OPEN = re.compile(r'(?P<kw>\b(?:struct|union))\s+(?P<name>[A-Za-z_]\w*)\s*\{')


def _match_brace(text, open_idx):
    """Index just past the '}' matching the '{' at open_idx."""
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def hoist_nested_tags(text, rep):
    """Move `struct Inner {...}` out of its enclosing struct to file scope.

    In C a tag defined inside another struct has file scope, so this is a
    no-op for the C builds by construction.  In C++ the same definition is
    scoped to the enclosing class, and a later `union Inner *p;` at function
    scope therefore declares a *new* incomplete type instead of finding it --
    which is the "invalid use of incomplete type" the measurement found 261 of.

    Innermost-first, one at a time, re-scanning after each: the nesting is two
    deep in practice (a union inside a struct, four structs inside that union)
    and doing it this way avoids having to track indices through edits.
    """
    n = 0
    while True:
        # Find the innermost tag definition that sits at depth > 0.  Innermost
        # first means the extracted block never itself contains a nested tag.
        target = None
        depth = 0
        i = 0
        top_start = 0
        while i < len(text):
            c = text[i]
            if c == '{':
                m = _TAG_OPEN.search(text, max(0, i - 200), i + 1)
                if m and m.end() == i + 1:
                    if depth == 0:
                        top_start = _stmt_start(text, m.start())
                    else:
                        # Capture top_start *with* the target: the scan carries
                        # on past this point and would otherwise leave it
                        # pointing at a later top-level struct, hoisting the
                        # definition in front of the wrong one.
                        target = (m, top_start)
                depth += 1
            elif c == '}':
                depth -= 1
            i += 1
        if target is None:
            break

        m, top_start = target
        open_idx = m.end() - 1
        close = _match_brace(text, open_idx)
        if close < 0:
            rep.unhandled.append(
                'cxxify: unbalanced braces hoisting %s %s' % (m.group('kw'), m.group('name')))
            break

        body = text[m.start():close]                 # 'struct Inner { ... }'
        text = text[:m.start()] + body[:0] + text[close:]
        # what remains at the site is ' member;' -- put the tag reference back
        text = (text[:m.start()]
                + '%s %s' % (m.group('kw'), m.group('name'))
                + text[m.start():])
        # and emit the definition ahead of the whole enclosing declaration
        text = text[:top_start] + body + ';\n\n' + text[top_start:]
        n += 1
        if n > 64:
            rep.unhandled.append('cxxify: nested-tag hoisting did not converge')
            break
    if n:
        rep.bump('nested struct/union tags hoisted to file scope', n)
    return text


def _stmt_start(text, idx):
    """Start of the declaration containing idx: just past the previous ';' or '}'."""
    j = idx
    while j > 0 and text[j - 1] not in ';}':
        j -= 1
    while j < len(text) and text[j] in ' \t\r\n':
        j += 1
    return j


# ---------------------------------------------------------------------------
# 3. GNU range designators
# ---------------------------------------------------------------------------

_RANGE = re.compile(
    r'\[\s*(?P<lo>0[xX][0-9A-Fa-f]+|\d+)\s*\.\.\.\s*(?P<hi>0[xX][0-9A-Fa-f]+|\d+)\s*\]\s*=\s*'
    r'(?P<val>[^,}]+?)\s*,')
_SINGLE = re.compile(
    r'\[\s*(?P<idx>0[xX][0-9A-Fa-f]+|\d+)\s*\]\s*=\s*(?P<val>[^,}]+?)\s*,')


def expand_designated_ranges(text, rep):
    """`[0 ... 3] = X,` -> `X, X, X, X,`.

    Positional, not `[0] = X, [1] = X, ...`: g++ accepts array designators in
    C++ only when they are contiguous and in order (it lowers those to
    positional itself) and answers the general form with "sorry, unimplemented:
    non-trivial designated initializers".  Emitting positional values is the
    form both compilers agree on.

    This is only correct if the designators tile the array from 0 with no gaps
    and no reordering, so that is *checked* rather than assumed, per
    initializer, and a failure is reported instead of miscompiled.  A gap would
    otherwise silently shift every element after it.
    """
    if '...' not in text:
        return text
    out = []
    pos = 0
    total = 0
    for brace in _initializer_spans(text):
        start, end = brace
        if start < pos:
            continue
        body = text[start:end]
        if '...' not in body:
            continue
        entries = []
        expect = 0
        ok = True
        scan = 0
        while scan < len(body):
            mr = _RANGE.search(body, scan)
            ms = _SINGLE.search(body, scan)
            m = min([x for x in (mr, ms) if x], key=lambda x: x.start(), default=None)
            if m is None:
                break
            if m is mr:
                lo, hi = int(m.group('lo'), 0), int(m.group('hi'), 0)
            else:
                lo = hi = int(m.group('idx'), 0)
            if lo != expect:
                ok = False
                rep.unhandled.append(
                    'cxxify: designators do not tile from 0 (expected [%d], got [%d]) '
                    '-- expanding them positionally would shift the array' % (expect, lo))
                break
            entries.append((m.start(), m.end(), m.group('val'), hi - lo + 1))
            expect = hi + 1
            scan = m.end()
        if not ok or not entries:
            continue
        rebuilt = []
        last = 0
        for s, e, val, count in entries:
            rebuilt.append(body[last:s])
            rebuilt.append(', '.join([val.strip()] * count) + ',')
            last = e
        rebuilt.append(body[last:])
        out.append((start, end, ''.join(rebuilt)))
        total += sum(c for _, _, _, c in entries)
        pos = end
    for start, end, new in reversed(out):
        text = text[:start] + new + text[end:]
    if out:
        rep.bump('GNU range designators expanded positionally', total)
    return text


def _initializer_spans(text):
    """Spans of top-level `= { ... };` initializers, outermost only."""
    for m in re.finditer(r'=\s*\{', text):
        open_idx = m.end() - 1
        close = _match_brace(text, open_idx)
        if close > 0:
            yield (open_idx, close)


# ---------------------------------------------------------------------------
# 4. empty parameter lists that a header already declares properly
# ---------------------------------------------------------------------------

_EMPTY_DECL = re.compile(
    r'^[ \t]*(?:extern[ \t]+)?[A-Za-z_]\w*(?:[ \t]+\w+)*[ \t*]+(?P<name>\w+)[ \t]*\([ \t]*\)[ \t]*;[ \t]*\n',
    re.M)


def fix_unprototyped_decls(text, header_fns, rep):
    """Replace `void f();` with the real prototype a header declares.

    `void f()` is an unprototyped declaration in C and a zero-parameter one in
    C++, so in C++ it contradicts the header and every call becomes "too many
    arguments".  Patching it to `void f(...)` would be worse: that is a
    distinct signature in C++, so it would quietly become an overload.

    Substituting the header's own prototype is valid C too, and unlike simply
    deleting the line it does not depend on the file actually including that
    header.  star_platform.c is exactly that case -- it forward-declares
    sub_08089864 itself and never includes code_0806F780.h, so deleting the
    declaration traded one error for another.
    """
    def repl(m):
        proto = header_fns.get(m.group('name'))
        if proto:
            rep.bump('unprototyped declarations given their real prototype')
            return proto + '\n'
        return m.group(0)
    return _EMPTY_DECL.sub(repl, text)


def header_function_prototypes(include_dir):
    """{name: full declaration} for functions declared with a parameter list."""
    protos = {}
    decl = re.compile(r'^[ \t]*(?!return\b)([A-Za-z_][\w \t*]*?\b(\w+)\s*\([^;{)]*\)\s*;)', re.M)
    for hp in sorted(include_dir.rglob('*.h')):
        for m in decl.finditer(hp.read_text(errors='replace')):
            body = m.group(1)
            if '(void)' in body.replace(' ', '') or '()' in body.replace(' ', ''):
                continue
            protos.setdefault(m.group(2), ' '.join(body.split()))
    return protos


# ---------------------------------------------------------------------------
# 6. sites where C's type rules are looser than C++'s
# ---------------------------------------------------------------------------
# Three constructs that the port's `-w` hides in C and that C++ rejects outright
# whatever the warning flags say.  They are listed as sites rather than handled
# by a transform because each needs to know what the code means, and because
# there are three of them -- a general rewriter for "C++ is stricter here" would
# be far more dangerous than the disease.
#
# GCC's transparent_union is the interesting one: it is a C-only attribute, so
# g++ accepts the declaration (with a warning) and then refuses every call that
# passes a member type instead of the union.  Five transparent unions exist in
# the decompilation and two call sites pass a member today.  A new one appears
# as a compile error naming the file and line, not as bad code generation.
CXX_SITES = {
    'multi_boot_util.c': [
        # end - start on `const void *`: pointer arithmetic on void is a GNU C
        # extension with no C++ spelling.
        ('u32 size = end - start;',
         'u32 size = (const u8 *)end - (const u8 *)start;'),
    ],
    'sprite.c': [
        # gUnk_08D6081C takes `union AnimCmd` (transparent); cmd is its first
        # member `words`.  The member is named because C picks it by type and
        # C++ would otherwise take the first one.
        ('ret = gUnk_08D6081C[~*cmd](cmd, s);',
         'ret = gUnk_08D6081C[~*cmd](PORT_TRANSPARENT(AnimCmd, words, cmd), s);'),
    ],
    'unknown_75.c': [
        # sub_08001408 takes `union LevelInfo_1E0` (transparent).
        ('sub_08001408(roomId, &p->unk0, NULL, NULL);',
         'sub_08001408(roomId, PORT_TRANSPARENT(LevelInfo_1E0, pat1, &p->unk0), NULL, NULL);'),
    ],
}


def apply_cxx_sites(text, name, rep):
    for old, new in CXX_SITES.get(name, ()):
        if old in text:
            text = text.replace(old, new)
            rep.bump('C++ strictness sites patched')
        else:
            rep.unhandled.append(
                '%s: a CXX_SITES pattern no longer matches -- this file has '
                'changed upstream and the C++ build will fail on it: %s'
                % (name, ' '.join(old.split())[:70]))
    return text


# ---------------------------------------------------------------------------
# 5. trailing `arr[0]` members with initializers
# ---------------------------------------------------------------------------

_FLEX_MEMBER = re.compile(
    r'\b(?P<type>struct\s+\w+|union\s+\w+|\w+)\s+(?P<member>\w+)\s*\[\s*0\s*\]\s*;')


def flex_array_types(include_dir):
    """{struct name: (member, element type)} for trailing `x[0]` members."""
    out = {}
    for hp in sorted(include_dir.rglob('*.h')):
        txt = hp.read_text(errors='replace')
        for m in _TAG_OPEN.finditer(txt):
            close = _match_brace(txt, m.end() - 1)
            if close < 0:
                continue
            body = txt[m.end():close - 1]
            fm = None
            for fm in _FLEX_MEMBER.finditer(body):
                pass                      # keep the last: it must be trailing
            # Comments have to go before the "is it last?" test.  The array
            # that documents its own layout in a trailing comment --
            # `u16 data[0]; // one single entry ...` in struct Unk_08128E28_0
            # -- is precisely the one this is for, and leaving the comment in
            # made the check say no.
            if fm and not _strip_comments(body[fm.end():]).strip().strip(';'):
                out[m.group('name')] = (fm.group('member'), fm.group('type'))
    return out


def _strip_comments(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    return re.sub(r'//[^\n]*', '', s)


def expand_flex_array_objects(text, flex, rep):
    """Give a `[0]`-terminated struct object its elements via a shadow struct.

    C++ has no flexible array member in any spelling, so

        static const struct T g = { .a = 1, .arr = { {..}, {..} } };

    becomes

        static const struct { struct T head; ElemT tail[2]; } g_storage = {
            { .a = 1 }, { {..}, {..} } };
        #define g (*(const struct T *)&g_storage)

    which is layout-identical: `head` occupies sizeof(struct T) bytes and the
    elements follow at exactly the offset `arr` had, because that offset *is*
    sizeof(struct T) for a trailing zero-length array.  Every `&g` and `g.a`
    at the use sites keeps working through the macro, so no use site changes.

    The alternative -- sizing the array for real -- would change
    sizeof(struct T), which platform/port/gba_layout.h asserts, so the build
    would fail.  That is the correct behaviour and it is why this shape was
    chosen instead.
    """
    if not flex:
        return text
    n = 0
    for tag, (member, elemtype) in flex.items():
        pat = re.compile(
            r'(?P<lead>static\s+const\s+|const\s+static\s+|static\s+|const\s+|)'
            r'struct\s+' + re.escape(tag) + r'\s+(?P<name>\w+)\s*=\s*\{')
        while True:
            m = pat.search(text)
            if not m:
                break
            close = _match_brace(text, m.end() - 1)
            if close < 0:
                rep.unhandled.append('cxxify: unbalanced initializer for %s' % m.group('name'))
                break
            body = text[m.end():close - 1]
            dm = re.search(r'\.\s*' + re.escape(member) + r'\s*=\s*\{', body)
            if not dm:
                break                      # no elements supplied: already legal
            arr_close = _match_brace(body, dm.end() - 1)
            elems = body[dm.end():arr_close - 1]
            count = _count_top_level(elems)
            head = (body[:dm.start()].rstrip().rstrip(',')
                    + body[arr_close:].rstrip().rstrip(','))
            name = m.group('name')
            new = ('%sstruct { struct %s head; %s tail[%d]; } %s__storage = {\n'
                   '    { %s },\n    { %s }\n};\n'
                   '#define %s (*(const struct %s *)&%s__storage)\n'
                   % (m.group('lead'), tag, elemtype, count, name,
                      head.strip(), elems.strip(), name, tag, name))
            end = close
            while end < len(text) and text[end] in ' \t':
                end += 1
            if end < len(text) and text[end] == ';':
                end += 1
            text = text[:m.start()] + new + text[end:]
            n += 1
    if n:
        rep.bump('flexible-array objects given shadow storage', n)
    return text


def _count_top_level(s):
    """Number of comma-separated elements at brace depth 0."""
    depth = 0
    count = 0
    seen = False
    for c in s:
        if c in '{[(':
            depth += 1
            seen = True
        elif c in '}])':
            depth -= 1
        elif c == ',' and depth == 0:
            count += 1
            seen = False
        elif not c.isspace():
            seen = True
    return count + (1 if seen else 0)


# ---------------------------------------------------------------------------

def rewrite_header(text, rep):
    text = rename_cxx_keywords(text, rep)
    text = hoist_nested_tags(text, rep)
    return text


def rewrite_source(text, rep, name='', header_fns=None, flex=None):
    text = rename_cxx_keywords(text, rep)
    text = expand_designated_ranges(text, rep)
    if header_fns:
        text = fix_unprototyped_decls(text, header_fns, rep)
    if flex:
        text = expand_flex_array_objects(text, flex, rep)
    if name:
        text = apply_cxx_sites(text, name, rep)
    return text
