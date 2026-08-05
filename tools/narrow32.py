"""Rewrite the decompilation's pointer *members* to four-byte ones.

The last step of the 64-bit conversion, and the one docs/SIXTYFOUR.md thought
would need a C++ front end over ~40,000 member accesses.  It does not, because
the accesses do not change: only the declarations do, and a declaration is a
line in a header.

    struct Chest {
        struct Task *task;      ->      PTR32(struct Task) task;
        u16 x;                          u16 x;
    };

PTR32 is a plain pointer in C and a four-byte handle class in C++
(platform/port/p32.h), so the ILP32 builds compile exactly what they compiled
before and the 64-bit builds get the console's layout.  `chest->task->state`,
`chest->task = t`, `if (chest->task)` and `(u32)chest->task` are all untouched
and all still work.

Only members are rewritten, never locals, parameters or return types.  A local
`struct Task *t` on a 64-bit host is an ordinary eight-byte pointer and should
stay one -- nothing reads it at a fixed address, and narrowing it would throw
away the top half of a host address for no reason.  That distinction is why
this runs over struct and union *bodies* rather than over the file.

Definition of done is not this script: it is platform/port/gba_layout.h
compiling at LP64.  The table asserts the size of all 246 types and the offset
of 2144 members, so a structure this misses is a build failure that names it.
"""

import re

# A member declaration, anchored to a whole line, which is how the
# decompilation writes them.  The leading group keeps whatever offset comment
# the decomp put there (`/* 0x08 */`), because those comments are the reason
# the file is readable and losing them would make every future diff worse.
_LEAD = r'(?P<lead>^[ \t]*(?:/\*[^*]*\*/[ \t]*)?)'
_TAIL = r'(?P<tail>[ \t]*(?:/\*.*|//.*)?)$'

# `struct Task *task;` / `const u16 *pal;` / `void *unk78;` / `u8 **pp;` /
# `const u16 *const *unk4;` -- cv-qualifiers may sit between the stars.
# The type is anything up to the run of stars; the name is a plain identifier,
# optionally followed by array dimensions.
_DATA_MEMBER = re.compile(
    _LEAD +
    r'(?P<type>(?:const[ \t]+|volatile[ \t]+|unsigned[ \t]+|signed[ \t]+)*'
    r'(?:struct[ \t]+|union[ \t]+|enum[ \t]+)?[A-Za-z_]\w*)'
    r'[ \t]*(?P<ptr>(?:\*[ \t]*|const[ \t]+|volatile[ \t]+)*\*)[ \t]*'
    r'(?P<postq>(?:const[ \t]+|volatile[ \t]+)*)'
    r'(?P<name>[A-Za-z_]\w*)'
    r'(?P<dims>(?:\[[^\];]*\])*)[ \t]*;' + _TAIL)

# `void (*unk1C)(struct Unk *);` -- a function-pointer member, and
# `void (*unkC[4])(struct Unk *);` -- an array of them, which is why the name
# group takes array dimensions.  PTR32_FN needs no special case for it: the
# name and its dimensions go through together, and both languages put them in
# the right place.  Missing this left one struct with its unk8 narrowed and its
# unkC[] not, which the layout assertions did not catch because that particular
# type is not one they cover -- the compile error was a ternary in intro.c
# complaining that the two branches had different types.
_FN_MEMBER = re.compile(
    _LEAD +
    r'(?P<ret>(?:const[ \t]+|volatile[ \t]+|unsigned[ \t]+|signed[ \t]+)*'
    r'(?:struct[ \t]+|union[ \t]+|enum[ \t]+)?[A-Za-z_]\w*[ \t*]*)'
    r'\([ \t]*\*[ \t]*(?P<name>[A-Za-z_]\w*(?:\[[^\];]*\])*)[ \t]*\)[ \t]*'
    r'(?P<args>\([^;]*\))[ \t]*;' + _TAIL)

# The brace may be on the next line -- `struct AreaDoor\n{` -- which the
# decompilation writes for a good number of types.  Matching only
# same-line braces left 64 structures unscanned and their pointer members
# eight bytes wide, which the layout assertions then reported one by one.
# An attribute may sit between the keyword and the name --
# `union __attribute__((transparent_union)) LevelInfo_1E0 {` -- and five
# unions are written that way.  Skipping them left their pointer members
# eight bytes wide, and with them every structure that embeds one, which
# is how struct Kirby came to be reported as the wrong size.


# A member whose type is a typedef that happens to be a pointer.
#
#     typedef void (*SubGameMenuFunc)(struct SubGameMenu *);
#     SubGameMenuFunc unk154;
#
# There is no star in the declarator, so nothing above matches it, and the
# member stays eight bytes wide -- which is how struct SubGameMenu came to be
# reported as the wrong size long after every visible pointer had been dealt
# with.  The typedefs have to be collected first and the members matched
# against them by name.
_TYPEDEF_FN = re.compile(
    r'typedef\s+[A-Za-z_][\w 	*]*\(\s*\*\s*(?P<name>[A-Za-z_]\w*)\s*\)\s*\([^;]*\)\s*;')
_TYPEDEF_PTR = re.compile(
    r'typedef\s+(?:const\s+|volatile\s+|struct\s+|union\s+|enum\s+|unsigned\s+|signed\s+)*'
    r'[A-Za-z_]\w*\s*\*+\s*(?P<name>[A-Za-z_]\w*)\s*;')


def pointer_typedefs(include_dir):
    """Names of typedefs that resolve to a pointer."""
    names = set()
    for hp in sorted(include_dir.rglob('*.h')):
        txt = hp.read_text(errors='replace')
        for pat in (_TYPEDEF_FN, _TYPEDEF_PTR):
            for m in pat.finditer(txt):
                names.add(m.group('name'))
    return names


_TD_MEMBER = re.compile(
    _LEAD + r'(?P<type>[A-Za-z_]\w*)[ 	]+(?P<name>[A-Za-z_]\w*)'
    r'(?P<dims>(?:\[[^\];]*\])*)[ 	]*;' + _TAIL)

_ARR_MEMBER = re.compile(
    _LEAD +
    r'(?P<type>(?:const[ \t]+|volatile[ \t]+|unsigned[ \t]+|signed[ \t]+)*'
    r'(?:struct[ \t]+|union[ \t]+|enum[ \t]+)?[A-Za-z_]\w*)'
    r'[ \t]*\([ \t]*\*[ \t]*(?P<name>[A-Za-z_]\w*)[ \t]*\)[ \t]*'
    r'(?P<dims>(?:\[[^\];]*\])+)[ \t]*;' + _TAIL)

_TAG_OPEN = re.compile(r'\b(?:struct|union)\b'r'(?:\s*__attribute__\s*\(\(.*?\)\))?'r'(?:\s+[A-Za-z_]\w*)?\s*\{')

# Types whose members must not be narrowed.
#
# A pointer member is only constrained when the *structure* is laid out by
# something other than this compiler -- read from ROM, placed by linker.ld, or
# handed out by the game's own allocator.  docs/SIXTYFOUR.md establishes that
# the honest answer to "which ones" is all of the decompilation's, which is why
# this runs over all of them.
#
# The port's own structures are the exception and they are not in the
# decompilation's headers at all, so they are excluded by construction rather
# than by name.  This list is for anything in the game's headers that turns out
# to be host-side after all.
SKIP_TYPES = set()

# Members whose declarator cannot go through PTR32 without a typedef.
#
# PTR32(X) is `X *` in C, and that only works when the pointee can be written
# as a type on its own.  `void (*const *unkC)(struct Unk *)` -- a pointer to a
# const function pointer -- cannot: C's declarator syntax wraps the name, so
# there is no X to put in the macro.  A typedef gives it one, and then both
# languages can say it.
#
# One member in the tree.  The typedef is emitted next to the structure rather
# than in a shared header so that it stays where its only user is.
NARROW_SITES = {
    'intro.h': [
        ('    void (*const *unkC)(struct Unk_08145B64_5EC *);',
         '    PTR32(PortFnpUnk_08387348_unkC) unkC;'),
        ('struct Unk_08387348 {',
         'typedef void (*const PortFnpUnk_08387348_unkC)(struct Unk_08145B64_5EC *);\n\n'
         'struct Unk_08387348 {'),
    ],
}


def apply_narrow_sites(text, name, rep):
    for old, new in NARROW_SITES.get(name, ()):
        if old in text:
            text = text.replace(old, new, 1)
            rep.bump('members narrowed via a generated typedef')
        else:
            rep.unhandled.append(
                '%s: a NARROW_SITES pattern no longer matches -- the member is '
                'left eight bytes wide and the layout assertions will say so: '
                '%s' % (name, ' '.join(old.split())[:70]))
    return text


def _matching_brace(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
    return -1


def narrow_members(text, rep, skip=SKIP_TYPES, name='', typedefs=frozenset()):
    """Rewrite pointer members of every struct/union body in `text`."""
    if name:
        text = apply_narrow_sites(text, name, rep)
    out = []
    pos = 0
    data_n = fn_n = td_n = arr_n = 0

    for m in _TAG_OPEN.finditer(text):
        if m.start() < pos:
            continue
        open_idx = m.end() - 1
        close = _matching_brace(text, open_idx)
        if close < 0:
            rep.unhandled.append('narrow32: unbalanced braces in a struct body')
            continue
        tag = re.search(r'\b(?:struct|union)[ \t]+([A-Za-z_]\w*)', m.group(0))
        if tag and tag.group(1) in skip:
            pos = close
            continue

        body = text[open_idx + 1:close]
        lines = body.splitlines(keepends=True)
        depth = 0
        for i, line in enumerate(lines):
            # Nested bodies included, deliberately.  tools/cxxify.py has
            # already hoisted every *named* nested tag to file scope, so what
            # is left inside is anonymous -- and an anonymous union's members
            # are part of this structure's layout, not a separate type's.
            # Skipping them left struct ForegroundPalette_4pp holding a
            # `u16 **u16pp` inside an anonymous union, eight bytes wide.
            if True:
                am = _ARR_MEMBER.match(line.rstrip('\n'))
                if am:
                    lines[i] = ('%sPTR32_ARR(%s, %s, %s);%s\n'
                                % (am.group('lead'), am.group('type'),
                                   am.group('name'), am.group('dims'),
                                   am.group('tail')))
                    arr_n += 1
                    depth += line.count('{') - line.count('}')
                    continue
                fm = _FN_MEMBER.match(line.rstrip('\n'))
                if fm:
                    lines[i] = ('%sPTR32_FN(%s, %s, %s);%s\n'
                                % (fm.group('lead'), fm.group('ret').strip(),
                                   fm.group('name'), fm.group('args'),
                                   fm.group('tail')))
                    fn_n += 1
                else:
                    tdm = _TD_MEMBER.match(line.rstrip('\n'))
                    if tdm and tdm.group('type') in typedefs:
                        lines[i] = ('%sPTR32_TD(%s) %s%s;%s\n'
                                    % (tdm.group('lead'), tdm.group('type'),
                                       tdm.group('name'), tdm.group('dims'),
                                       tdm.group('tail')))
                        td_n += 1
                        depth += line.count('{') - line.count('}')
                        continue
                    dm = _DATA_MEMBER.match(line.rstrip('\n'))
                    if dm:
                        # Everything up to the *last* star is the pointee.
                        # `const u16 *const *unk4` is a pointer to a const
                        # pointer to const u16, so the pointee is
                        # `const u16 *const` -- the qualifiers sit between the
                        # stars and have to travel with them.
                        inner = dm.group('type') + ' ' + dm.group('ptr')[:-1]
                        # A qualifier after the last star qualifies the
                        # *pointer*, not the pointee -- `const u8 *volatile
                        # srcp` -- so it stays on the member, outside PTR32.
                        lines[i] = ('%sPTR32(%s) %s%s%s;%s\n'
                                    % (dm.group('lead'), inner.strip(),
                                       dm.group('postq'),
                                       dm.group('name'), dm.group('dims'),
                                       dm.group('tail')))
                        data_n += 1
            depth += line.count('{') - line.count('}')

        out.append((open_idx + 1, close, ''.join(lines)))
        pos = close

    for start, end, new in reversed(out):
        text = text[:start] + new + text[end:]

    if data_n:
        rep.bump('pointer members narrowed to PTR32', data_n)
    if fn_n:
        rep.bump('function-pointer members narrowed to PTR32_FN', fn_n)
    if td_n:
        rep.bump('typedef-typed pointer members narrowed to PTR32_TD', td_n)
    if arr_n:
        rep.bump('pointer-to-array members narrowed to PTR32_ARR', arr_n)
    return text
