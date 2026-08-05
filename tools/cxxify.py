"""Make the decompilation's C acceptable to a C++ compiler.

The port needs this because of pointer width, not because anybody wanted C++.
A 64-bit build has to keep the GBA's 4-byte pointer members -- see
docs/SIXTYFOUR.md -- and C cannot express a 4-byte thing that still behaves
like a pointer at every use site.  C++ can (platform/port/p32.h), and that
turns a rewrite of ~40,000 member accesses into a rewrite of ~285 member
declarations.

Almost every transform here is also valid C, and that is the point: the ILP32
builds compile the identical tree and their output must not move, which is what
makes the whole thing testable.  Two are necessarily C++-only and say so with
an #ifdef -- the transparent-union constructors and the linkage wrappers.

The measurement, over all 156 game translation units: 107 compiled as C++
unchanged, and the other 49 produced 698 errors from six causes.  Fixing those
got every file to compile.  Linking took four more, and those were the
interesting ones, because none of them produces a diagnostic anywhere.

Compile:

  1. `template` is a parameter name in 41 files (185 uses).  Renamed.  It
     cascades: the parameter list fails to parse, so every later parameter is
     undeclared too, which is where 79 "'a2' was not declared" came from.

  2. A struct or union tag defined inside another struct has file scope in C
     and class scope in C++.  5 definitions in 2 headers, 261 errors, because
     every function naming the type then declares a fresh incomplete one.
     Hoisted to file scope, which is where C already puts them.

  3. `[0 ... 3] = X` is a GNU range designator with no C++ spelling.  Expanded
     positionally, not to `[0] = X`: g++ answers the general designator form
     with "sorry, unimplemented" and takes only contiguous in-order ones.

  4. `void f();` is unprototyped in C and zero-parameter in C++.  Replaced with
     the header's real prototype -- not deleted, because star_platform.c
     declares sub_08089864 itself and never includes the header.

  5. A trailing `arr[0]` with a brace initializer is the GNU flexible-array
     idiom, which C++ has in no form.  Given shadow storage that preserves the
     layout exactly; sizing the array for real would change a sizeof that
     platform/port/gba_layout.h asserts.

  6. Transparent unions.  GCC's attribute is C-only, so g++ takes the
     declaration and rejects every call passing a member.  Given a converting
     constructor per member type -- after a site table proved untenable: three
     sites turned up by three separate routes, the third from a file a
     full-tree sweep had compiled clean minutes earlier.

Link:

  7. `const` at file scope has external linkage in C and internal linkage in
     C++.  Every ROM data table in the decompilation is a const array, so under
     C++ they silently become static.  420 definitions given an explicit
     `extern`, which is what C already meant.

  8. C matches functions by name; C++ matches by name and parameter types.  The
     game is therefore given C linkage throughout -- headers and sources -- so
     the C++ build links by the same rules the ILP32 builds do.  Anything else
     would be a second port, which is the risk docs/SIXTYFOUR.md names.

  9. extern "C" is *language* linkage, not storage linkage: a const inside an
     extern "C" block is still internal.  Both 7 and 8 are needed, and finding
     that out cost several rounds of "the symbol is right there in the source".

 10. One genuine defect, which only this build can see: functions.h declares
     sub_08002888 taking u32 and code_080023A4.c defines it taking an enum.  C
     links that by name; with C linkage under C++ the declarations are still
     compared, so it is reported.  Worth carrying upstream.

Enum increment is not handled here at all -- platform/port/cxx_compat.h defines
the operator, which also covers the sites the decompilation has not written yet.

Every transform reports through the same Report object portify.py uses, and the
site tables complain loudly when a pattern stops matching, because the
decompilation moves and a silently skipped transform here surfaces minutes
later in a file that looks unrelated.
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
        best_depth = 0
        depth = 0
        i = 0
        top_start = 0
        while i < len(text):
            c = text[i]
            if c == '{':
                # finditer, not search: search returns the *first* match in
                # the window, which for a brace preceded by another tag within
                # 200 characters is the wrong one -- it ends somewhere else, so
                # the test below rejects it and this brace is treated as not
                # opening a tag at all.  That is what left
                # Unk_08145B64_5EC_24_Pat2 nested inside its union.
                m = None
                for cand in _TAG_OPEN.finditer(text, max(0, i - 200), i + 1):
                    if cand.end() == i + 1:
                        m = cand
                if m is not None:
                    if depth == 0:
                        top_start = _stmt_start(text, m.start())
                    elif depth > best_depth:
                        # The *innermost* candidate, not the last one seen.
                        # Taking the last leaves a tag stranded: with a union at
                        # depth 1 holding four structs at depth 2, the last
                        # candidate in a linear scan is whichever comes latest
                        # in the text, and once the union itself is hoisted its
                        # remaining child is never picked.  That left
                        # Unk_08145B64_5EC_24_Pat2 nested and every assertion
                        # about it failing on an incomplete type.
                        #
                        # top_start is captured with the target because the scan
                        # carries on and would otherwise point at a later
                        # top-level struct.
                        best_depth = depth
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
    'intro.c': [
        # A ternary whose branches are a narrowed member and a raw function
        # pointer.  Not "cannot convert" -- *ambiguous*: PTR32 converts to a
        # pointer and a pointer converts to a PTR32, so neither branch wins and
        # `?:` has no common type.  Casting one branch settles it, and the cast
        # is a no-op in C.
        ('v4->unk1C = gUnk_08387348[a2->unk4].unk8 ? gUnk_08387348[a2->unk4].unk8 '
         ': gUnk_08387348[a2->unk4].unkC[a2->unkE];',
         'v4->unk1C = gUnk_08387348[a2->unk4].unk8 '
         '? (void (*)(struct Unk_08145B64_5EC *))gUnk_08387348[a2->unk4].unk8 '
         ': (void (*)(struct Unk_08145B64_5EC *))gUnk_08387348[a2->unk4].unkC[a2->unkE];'),
    ],
    'phan_phan.c': [
        # `const struct Kirby_110 **kirby110 = &phanPhan->kirby3->unk110;`
        #
        # The one shape a four-byte pointer member genuinely cannot serve
        # unchanged: taking the *address* of a narrowed member gives a
        # PTR32(T)*, not a T**, because the member no longer is a T.  There is
        # no way for platform/port/p32.h to paper over it -- the object being
        # pointed at really has a different type now.
        #
        # So the local changes to match, which is valid C as well: PTR32
        # expands to a plain pointer there, and the ILP32 builds compile the
        # same declaration they always did.  One site in the tree; if the
        # decompilation grows another, it is a compile error naming the file.
        ('const struct Kirby_110 **kirby110;',
         'PTR32(const struct Kirby_110) *kirby110;'),
    ],
    'multi_boot_util.c': [
        # end - start on `const void *`: pointer arithmetic on void is a GNU C
        # extension with no C++ spelling.
        ('u32 size = end - start;',
         'u32 size = (const u8 *)end - (const u8 *)start;'),
    ],
}

# The same, for headers.
#
# This one is not a C-versus-C++ difference at all: it is a place where the
# decompilation's declaration and its definition genuinely disagree, and C
# links them anyway because C matches functions by name alone.  Giving the game
# C linkage under C++ keeps that matching *and* gets the declarations compared,
# so the C++ build reports it and no other build can.  It is the only one in
# the tree, and it is worth carrying upstream -- see docs/DECOMP-REQUESTS.md.
#
# functions.h already includes data.h, so the enum is in scope at the
# declaration; the definition is the correct one and the header is simply
# behind it.
CXX_HEADER_SITES = {
    'm4a.h': [
        # gMPlayJumpTable is an array at a fixed address with a fixed extent,
        # so its entries have to stay four bytes -- see prelude.h.  The one
        # function that takes the table by pointer has to agree about the
        # element type.  PTR32_TD is a plain MPlayFunc in C.
        ('void MPlayJumpTableCopy(MPlayFunc *);',
         'void MPlayJumpTableCopy(PTR32_TD(MPlayFunc) *);'),
    ],
    'functions.h': [
        ('u32 *sub_08002888(u32 arg0, u8 index, u8 subindex);',
         'u32 *sub_08002888(enum SUB_08002888_ENUM arg0, u8 index, u8 subindex);'),
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

_TU_OPEN = re.compile(
    r'union\s+__attribute__\s*\(\s*\(\s*transparent_union\s*\)\s*\)\s+(?P<name>\w+)\s*\{')
_TU_MEMBER = re.compile(r'^\s*(?P<type>[A-Za-z_][\w \t]*?[\w \t*]*?)\s*(?P<name>\w+)\s*;\s*$', re.M)


def add_transparent_union_ctors(text, rep):
    """Let a named transparent union be constructed from any member's type.

    GCC's transparent_union is a C-only attribute: in C a function taking such
    a union may be called with any member's type directly, and g++ parses the
    attribute, warns that it is ignoring it, and rejects every one of those
    calls.

    This started as a table of the call sites that broke.  That was wrong, and
    measurably so: three sites turned up by three different routes -- two in a
    sweep of every translation unit, and a third only once build/generated had
    been regenerated, from a file the sweep had compiled clean minutes earlier.
    A construct the decompilation uses freely needs a fix at the type, not at
    the call.

    So give the union a converting constructor per distinct member type, which
    is what the C rule amounts to.  The default constructor stays defaulted, so
    the union remains trivially default-constructible and every structure
    holding one -- struct Kirby does -- keeps its own triviality.

    This is the one transform here that is C++-only, and it says so with an
    #ifdef rather than pretending otherwise.  The C builds see the union
    exactly as the decompilation wrote it.
    """
    out = []
    pos = 0
    n = 0
    for m in _TU_OPEN.finditer(text):
        if m.start() < pos:
            continue
        close = _match_brace(text, m.end() - 1)
        if close < 0:
            rep.unhandled.append(
                'cxxify: unbalanced braces in transparent union %s' % m.group('name'))
            continue
        name = m.group('name')
        body = _strip_comments(text[m.end():close - 1])
        seen = []
        for mem in _TU_MEMBER.finditer(body):
            full = _decl_type(mem)
            if full and full not in seen:
                seen.append(full)
        if not seen:
            continue
        ctors = ['\n#ifdef __cplusplus',
                 '    /* Generated by tools/cxxify.py: C lets a transparent union be',
                 '     * built from any member type, and C++ has no such rule. */',
                 '    %s() = default;' % name]
        for i, t in enumerate(seen):
            ctors.append('    %s(%s v__) { %s = v__; }'
                         % (name, t, _member_of_type(text[m.end():close - 1], t)))
        ctors.append('#endif\n')
        out.append((close - 1, '\n'.join(ctors)))
        n += 1
        pos = close
    for at, ins in reversed(out):
        text = text[:at] + ins + text[at:]
    if n:
        rep.bump('transparent unions given C++ converting constructors', n)
    return text


def _decl_type(mem):
    """The declared type of a matched member, stars and all.

    The type group of _TU_MEMBER can absorb the `*` in `struct Foo *pat1;`, so
    the stars are stripped out of it and re-attached from a count over the
    whole declaration.  Doing it any other way produced `struct Foo * *`.
    """
    base = ' '.join(mem.group('type').replace('*', ' ').split())
    stars = '*' * mem.group(0).count('*')
    return (base + ' ' + stars).strip()


def _member_of_type(body, wanted):
    """First member of `body` whose declared type is `wanted`."""
    for mem in _TU_MEMBER.finditer(_strip_comments(body)):
        if _decl_type(mem) == wanted:
            return mem.group('name')
    return ''


# ---------------------------------------------------------------------------
# 7. linkage
# ---------------------------------------------------------------------------
# The two differences that only appear at link time, and they are the ones that
# matter most, because neither produces a diagnostic in any translation unit.
#
#   - `const` at file scope has external linkage in C and *internal* linkage in
#     C++.  Every ROM data table the decompilation defines is a const array, so
#     under C++ they all quietly become static and disappear from the link.
#     18 symbols, including gUnk_082D8D74 -- the room tilemap table.
#
#   - C matches functions by name; C++ matches them by name and parameter
#     types.  The decompilation has declarations that disagree with their
#     definitions -- functions.h says `u32 *sub_08002888(u32, u8, u8)` and
#     code_080023A4.c defines it taking `enum SUB_08002888_ENUM` -- which C
#     links happily and C++ does not.  29 symbols.
#
# Both go away by giving the game C linkage, and that is the right answer for
# a better reason than convenience: the whole point is for the 64-bit build to
# behave the same as the ILP32 builds.  C linkage *is* part of how those builds
# behave.  Adopting C++ linkage would make the 64-bit build differ from them in
# a way that has nothing to do with pointer width, which is the "two ports"
# risk docs/SIXTYFOUR.md warns about.
#
# The wrapper goes just inside the include guard rather than after the
# includes, and that is safe here specifically because platform/port/prelude.h
# is force-included ahead of every translation unit and has already pulled in
# every system header the game uses -- so a nested <stdlib.h> inside the block
# is a no-op against its own include guard.  prelude.h says that is what it is
# for.  p32.h is included from there too, so the template is declared long
# before any extern "C" block opens; a template cannot have C linkage.

_GUARD = re.compile(r'^[ \t]*#\s*define[ \t]+(?P<name>GUARD_\w+)[ \t]*$', re.M)


def wrap_header_extern_c(text, rep):
    """Give a game header C linkage when a C++ front end parses it."""
    m = _GUARD.search(text)
    if not m:
        return text
    # The matching #endif is the last one in the file.
    end = text.rfind('#endif')
    if end < 0 or end < m.end():
        rep.unhandled.append('cxxify: no closing #endif to place extern "C" before')
        return text
    text = (text[:m.end()]
            + '\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
            + text[m.end():end]
            + '#ifdef __cplusplus\n}\n#endif\n\n'
            + text[end:])
    rep.bump('headers given C linkage for the C++ builds')
    return text


def hoist_nested_tags_fully(text, rep):
    """hoist_nested_tags until nothing moves.

    One call does not reach a fixed point: hoisting rewrites the text under the
    scan that found the target, and the scan keeps only the last candidate it
    saw, so a pass can end with tags still nested.  On intro.h a single call
    moved five and left eight -- including the innermost Pat2, which then made
    every assertion about it in gba_layout.h fail on an incomplete type.

    Repeating until the text stops changing is the honest fix and costs a few
    passes over one header.
    """
    for _ in range(32):
        new = hoist_nested_tags(text, rep)
        if new == text:
            return text
        text = new
    rep.unhandled.append('cxxify: nested-tag hoisting did not reach a fixed point')
    return text


def rewrite_header(text, rep, name=''):
    for old, new in CXX_HEADER_SITES.get(name, ()):
        if old in text:
            text = text.replace(old, new)
            rep.bump('conflicting declarations corrected for C++')
        else:
            rep.unhandled.append(
                '%s: a CXX_HEADER_SITES pattern no longer matches -- if the '
                'decompilation has fixed it upstream, drop the entry; if not, '
                'the C++ build will fail on it: %s'
                % (name, ' '.join(old.split())[:70]))
    text = rename_cxx_keywords(text, rep)
    text = hoist_nested_tags_fully(text, rep)
    text = add_transparent_union_ctors(text, rep)
    text = wrap_header_extern_c(text, rep)
    return text


_CONST_DEF = re.compile(
    r'^(?!\s*(?:static|extern|typedef|return|case)\b)'      # not already, not a statement
    r'(?=[^=;]*\bconst\b)'                                   # const somewhere in the declarator
    r'([A-Za-z_][^=;{}()]*?\b\w+\s*(?:\[[^\]]*\]\s*)*)=',    # ... name [dims] =
    re.M)


def externalise_const_definitions(text, rep):
    """Give file-scope `const` definitions explicit external linkage.

    A const object at namespace scope has *internal* linkage in C++ and
    external linkage in C.  The decompilation's ROM tables are all const
    arrays, so under C++ every one of them quietly becomes static: defined in
    its own translation unit, invisible from every other, and the only symptom
    is an undefined reference at link time naming a symbol that is plainly
    right there in the source.

    extern "C" does not help with this, which is worth stating because it looks
    as though it should: it sets *language* linkage, not storage linkage, and a
    const at namespace scope inside an extern "C" block is still internal.

    Writing `extern` on the definition is valid C -- an extern declaration with
    an initialiser is a definition with external linkage, which is exactly what
    C gives it by default -- so the ILP32 builds are unaffected.

    Only at brace depth 0, so that a const local inside a function is left
    alone.
    """
    n = 0
    lines = text.splitlines(keepends=True)
    depth = 0
    for i, line in enumerate(lines):
        if depth == 0:
            m = _CONST_DEF.match(line)
            if m:
                lines[i] = 'extern ' + line
                n += 1
        depth += line.count('{') - line.count('}')
        if depth < 0:
            depth = 0
    if n:
        rep.bump('const definitions given explicit external linkage', n)
    return ''.join(lines)


def wrap_source_extern_c(text, rep):
    """Give a game source file C linkage when a C++ front end parses it.

    Needed as well as the header wrapper, not instead of it: a symbol declared
    in a header inherits that header's linkage at its definition, but the
    decompilation also defines things no header declares.  gUnk_082D8D74 is
    defined in code_080023A4.c and declared, as an `extern` local to the file,
    in code.c -- so nothing about it passes through a header at all, and under
    C++ its `const` would make it internal and unreachable from the other file.

    The block opens after the *leading* run of #include lines rather than after
    the last #include in the file.  Six sources include an .inc.c fragment part
    way down, and anchoring to the last one would leave every definition above
    it outside the block.  A later include ending up inside the block is
    harmless -- they are all game headers or fragments, and the system headers
    were pulled in by prelude.h long before.
    """
    # The whole file, from the very first line, includes and all.
    #
    # Anchoring after the include block looks tidier and is wrong: portify.py
    # prepends declarations *above* the includes -- the WILD_READS guard and
    # the FNPTR adapter forward declarations -- so an anchor below them leaves
    # exactly the cross-seam declarations outside the block, which is the one
    # place it matters.  The symptom is an undefined reference to a mangled
    # PortMain_* from a file that plainly declares it.
    #
    # Wrapping the includes too is safe here for a specific reason rather than
    # a general one: platform/port/prelude.h is force-included ahead of every
    # translation unit and has already brought in every system header the game
    # uses.  Three sources include <limits.h> or <math.h> directly; both are in
    # prelude.h's list, so those directives are no-ops against their own
    # include guards and no system declaration is ever parsed inside an
    # extern "C" block.
    lines = text.splitlines(keepends=True)
    anchor = 0
    lines.insert(anchor, '\n#ifdef __cplusplus\nextern "C" {\n#endif\n\n')
    lines.append('\n#ifdef __cplusplus\n}\n#endif\n')
    rep.bump('sources given C linkage for the C++ builds')
    return ''.join(lines)


def rewrite_source(text, rep, name='', header_fns=None, flex=None):
    text = rename_cxx_keywords(text, rep)
    text = expand_designated_ranges(text, rep)
    if header_fns:
        text = fix_unprototyped_decls(text, header_fns, rep)
    if flex:
        text = expand_flex_array_objects(text, flex, rep)
    if name:
        text = apply_cxx_sites(text, name, rep)
    text = externalise_const_definitions(text, rep)
    # Last: the linkage wrapper has to enclose everything above it.
    text = wrap_source_extern_c(text, rep)
    return text
