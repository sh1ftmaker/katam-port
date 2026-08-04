#!/usr/bin/env python3
"""
gen_rom_data.py -- resolve the game's ROM data symbols to ROM addresses.

data/*.s is 28,590 labels over `.incbin "baserom.gba", offset, size` blocks --
graphics, level layouts, tables.  The port cannot assemble ARM, and it must not
ship the data anyway (the player supplies their own ROM).

Only 162 of those labels are ever named by C code; everything else is reached
by following pointers that already live inside the ROM image.  Since the port
maps the ROM at its true address of 0x08000000, both cases are handled by the
same thing: turn each referenced label into a macro over its ROM address, and
comment out its `extern` declaration in the copied headers.

No data is copied, converted, or committed.  The addresses come from the `@`
comments the decomp writes on every data label.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from gen_ram_symbols import declaration_to_macro, DECL_RE_TMPL  # noqa: E402

# `s32 (*const gTable[])(union AnimCmd, struct Sprite *)` -- an array of
# function pointers sitting in ROM.
RE_FN_PTR_ARRAY = re.compile(
    r'^\s*extern\s+(?P<ret>[\w\s\*]+?)\s*\(\s*\*\s*(?P<qual>const\s+)?'
    r'(?P<name>\w+)\s*\[\s*\]\s*\)\s*\((?P<params>[^)]*(?:\([^)]*\)[^)]*)*)\)\s*;')


# `typedef void (*UnkCFunc2)(struct Unk_0802E57C *, struct Unk_0802E57C_C *);`
# -- the game declares most of its ROM function tables through one of these,
# so the raw `RET (*name[])(...)` spelling above only catches a few of them.
RE_FN_PTR_TYPEDEF = re.compile(
    r'typedef\s+(?P<ret>[\w\s\*]+?)\s*\(\s*\*\s*(?P<name>\w+)\s*\)\s*'
    r'\((?P<params>[^;]*)\)\s*;')

# `extern const UnkCFunc2 gUnk_082EB7D0[];`
RE_TYPEDEF_ARRAY = re.compile(
    r'^\s*extern\s+(?:const\s+)?(?P<type>\w+)\s+(?P<name>\w+)\s*\[\s*\]\s*;')


def name_parameters(params):
    """`union AnimCmd, struct Sprite *` -> `union AnimCmd p0, struct Sprite *p1`.

    A definition cannot leave its parameters unnamed, and these declarations
    always do."""
    params = params.strip()
    if not params or params == 'void':
        return 'void'
    out, depth, start, parts = [], 0, 0, []
    for i, ch in enumerate(params):
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ',' and depth == 0:
            parts.append(params[start:i])
            start = i + 1
    parts.append(params[start:])
    for i, part in enumerate(parts):
        out.append('%s p%d' % (part.strip(), i))
    return ', '.join(out)


SCALAR_SIZES = {
    'u8': 1, 's8': 1, 'bool8': 1, 'char': 1, 'vu8': 1, 'vs8': 1,
    'u16': 2, 's16': 2, 'vu16': 2, 'vs16': 2,
    'u32': 4, 's32': 4, 'vu32': 4, 'vs32': 4, 'int': 4, 'unsigned': 4,
    'bool32': 4, 'uintptr_t': 4, 'size_t': 4, 'float': 4, 'long': 4,
}

RE_STRUCT = re.compile(r'\bstruct\s+(\w+)\s*\{(?P<body>[^{}]*)\}\s*;'
                       r'(?:\s*/\*\s*size\s*=\s*(?P<size>0x[0-9A-Fa-f]+))?')


def parse_structs(texts):
    """Struct layouts, but only the ones that can be laid out with confidence.

    Enough to find function-pointer members and their byte offsets.  Anything
    with a member this cannot size -- a nested struct, an unknown typedef --
    is dropped rather than guessed at, because a wrong offset here would patch
    the wrong word of the ROM.  Where the decomp annotates `/* size = 0x18 */`
    the computed layout is checked against it and the struct is dropped if they
    disagree."""
    out = {}
    for text in texts:
        for m in RE_STRUCT.finditer(text):
            name, body = m.group(1), m.group('body')
            offset, align, members, ok = 0, 1, [], True

            for line in re.sub(r'/\*.*?\*/', '', body, flags=re.S).split(';'):
                line = line.strip()
                if not line:
                    continue

                fn = re.match(r'(?P<ret>[\w\s\*]+?)\s*\(\s*\*\s*(?P<nm>\w+)\s*\)'
                              r'\s*\((?P<params>.*)\)$', line, re.S)
                if fn:
                    size = a = 4
                    is_fn = True
                    info = (fn.group('ret').strip(), fn.group('params'))
                else:
                    is_fn, info = False, None
                    if '*' in line:
                        size = a = 4
                    else:
                        mm = re.match(r'(?:const\s+|volatile\s+)*(?:struct\s+|union\s+)?'
                                      r'(\w+)\s+\w+(\s*\[([^\]]*)\])?$', line)
                        if not mm or mm.group(1) not in SCALAR_SIZES:
                            ok = False
                            break
                        size = a = SCALAR_SIZES[mm.group(1)]
                        if mm.group(3):
                            try:
                                size *= int(mm.group(3), 0)
                            except ValueError:
                                ok = False
                                break

                offset = (offset + a - 1) // a * a
                if is_fn:
                    members.append((offset, info))
                align = max(align, a)
                offset += size

            if not ok:
                continue
            total = (offset + align - 1) // align * align
            if m.group('size') and int(m.group('size'), 16) != total:
                continue
            if members:
                out[name] = (total, members)
    return out


def parse_map(path):
    """{rom address: symbol} from the GBA build's link map."""
    out = {}
    for line in path.read_text(errors='replace').splitlines():
        m = re.match(r'\s+0x0([0-9a-fA-F]{7})\s+(\w+)\s*$', line)
        if m:
            out.setdefault(int(m.group(1), 16), m.group(2))
    return out


def extend_fn_table(rom, mapping, defined, addr, count, limit=64):
    """Follow a function table past the label that nominally ends it.

    Labels in data/*.s mark where the decompilation put a name, not where a
    table ends.  `gUnk_0834BD88` is labelled as 3 entries and the game indexes
    it well past that -- on hardware the reads simply continue into the words
    of the next label, which are more function pointers of the same kind.  In C
    those became two separate arrays, so the same index ran off the end and
    called garbage.

    Keep taking words while they still look like functions: Thumb bit set, and
    an exact match for a function the port defines.  An arbitrary data word
    matching one of those exactly is vanishingly unlikely, and the walk stops
    at the first word that does not."""
    extra = 0
    while extra < limit:
        off = addr - ROM_START + (count + extra) * 4
        if off + 4 > len(rom):
            break
        word = int.from_bytes(rom[off:off + 4], 'little')
        if not (word & 1):          # not a Thumb function pointer
            break
        name = mapping.get(word & ~1)
        if name not in defined:
            break
        extra += 1
    return count + extra


def resolve_fn_table(rom, mapping, defined, addr, count):
    """Read a ROM function table and name each entry.

    The table holds ARM code addresses.  Each one is looked up in the link map
    and kept only if the port actually defines that function -- otherwise the
    entry is a function the decompilation has not reached yet, and the caller
    substitutes a stub.  The low bit is the Thumb flag, not part of the
    address."""
    entries = []
    for i in range(count):
        off = addr - ROM_START + i * 4
        if off < 0 or off + 4 > len(rom):
            entries.append(None)
            continue
        value = int.from_bytes(rom[off:off + 4], 'little') & ~1
        name = mapping.get(value)
        entries.append(name if name in defined else None)
    return entries


ROM_START = 0x08000000
ROM_END = 0x0A000000


def parse_data_labels(datadir):
    """Return {label: absolute ROM address}.

    Most labels carry the address in a trailing comment, which the decomp
    writes on every one it generates:

        gUnk_0815BDD8:: @ 0815BDD8
            .incbin "baserom.gba", 0x15BDD8, 0x0000200

    A few do not -- the `_End` markers in multi_boot_images.s, which sit right
    after a block rather than at the start of one.  For those the address is
    tracked forward through the .incbin sizes since the last known label.
    """
    out, sizes = {}, {}
    for path in sorted(Path(datadir).rglob('*.s')):
        cursor = None
        pending = None      # the label whose extent is still being measured

        def close(pending, cursor):
            if pending is not None and cursor is not None:
                sizes[pending] = cursor - out[pending]

        for line in path.read_text(errors='replace').splitlines():
            m = re.match(r'^(\w+)::?\s*(?:@\s*([0-9A-Fa-f]{6,8}))?\s*$', line)
            if m:
                close(pending, cursor)
                if m.group(2):
                    cursor = int(m.group(2), 16)
                if cursor is not None and ROM_START <= cursor < ROM_END:
                    out[m.group(1)] = cursor
                    pending = m.group(1)
                continue

            inc = re.match(r'\s*\.incbin\s+"[^"]*"\s*,\s*(0x[0-9A-Fa-f]+)\s*,'
                           r'\s*(0x[0-9A-Fa-f]+)', line)
            if inc and cursor is not None:
                cursor += int(inc.group(2), 16)
        close(pending, cursor)
    return out, sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-dir', required=True, type=Path)
    ap.add_argument('--tree', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--map', type=Path,
                    help="the GBA build's katam.map, used to turn the ROM "
                         'addresses inside function tables back into names')
    ap.add_argument('--rom', type=Path,
                    help='a ROM to read function-table contents from; without '
                         'it those tables can only be stubbed')
    ap.add_argument('--out-tables', type=Path,
                    help='C file for ROM function tables rebuilt with real '
                         'function symbols.  Separate from --out-copies '
                         'because it has to include the game headers, and '
                         'those redeclare what --out-copies defines')
    ap.add_argument('--out-copies', type=Path,
                    help='C file for symbols whose declaration cannot be '
                         'turned into a macro; they become storage filled '
                         'from the ROM at startup')
    args = ap.parse_args()

    labels, label_sizes = parse_data_labels(args.data_dir)
    # .c files re-declare these symbols too, and a declaration left
    # standing would expand the macro into a declarator.
    headers = sorted(list((args.tree / 'include').rglob('*.h'))
                     + list((args.tree / 'src').rglob('*.c'))
                     + list((args.tree / 'src').rglob('*.h')))
    header_text = {hp: hp.read_text(errors='replace') for hp in headers}

    # Taken before the removal passes start commenting declarations out.
    struct_arrays = {}
    for hp in headers:
        for m in re.finditer(r'^[ \t]*extern[^;\n]*\bstruct\s+(\w+)\s+(\w+)\s*\[\s*\]',
                             header_text[hp], re.M):
            struct_arrays.setdefault(m.group(2), m.group(1))

    resolved, skipped = [], []
    dirty = set()
    decl_source = {}
    typedef_tables = {}

    # Which typedefs are function pointers, and what they expand to.
    fn_typedefs = {}
    for hp in headers:
        for m in RE_FN_PTR_TYPEDEF.finditer(header_text[hp]):
            fn_typedefs[m.group('name')] = (m.group('ret').strip(),
                                            m.group('params'))

    structs = parse_structs(header_text.values())

    # Symbols the game's own headers already declare.  Re-declaring one with
    # the table's element signature is a hard error when the header disagrees
    # -- and it often does, because a ROM table stores whatever fits in r0 and
    # the C declarations were recovered per-function.  Where a declaration
    # exists, defer to it and cast at the point of use instead, which is what
    # the ROM is doing anyway.
    declared = {}
    for hp in headers:
        if hp.suffix != '.h':
            continue
        try:
            rel = str(hp.relative_to(args.tree / 'include'))
        except ValueError:
            continue
        for m in re.finditer(r'^[\w \t\*]*?\b(\w+)\s*\([^;{]*\)\s*;', header_text[hp], re.M):
            declared.setdefault(m.group(1), rel)
    mapping = parse_map(args.map) if args.map and args.map.exists() else {}
    rom = args.rom.read_bytes() if args.rom and args.rom.exists() else None
    defined = set()
    for cp in (args.tree / 'src').rglob('*.c'):
        for m in re.finditer(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;]*?\)\s*\{',
                             cp.read_text(errors='replace'), re.M):
            defined.add(m.group(1))

    # Extents, so an unparseable symbol can be given storage of the right size.
    # The .incbin length is authoritative; the gap to the next label is only a
    # fallback, and it over-measures badly when a label is followed by others
    # the decomp has not written sizes for.
    by_addr = sorted(set(labels.values()))
    index = {a: i for i, a in enumerate(by_addr)}
    extent = {}
    for name, addr in labels.items():
        if label_sizes.get(name):
            extent[name] = label_sizes[name]
            continue
        i = index[addr]
        extent[name] = (by_addr[i + 1] - addr) if i + 1 < len(by_addr) else 0

    # A declaration whose type is an inline struct/union definition spans
    # several lines and never matches the single-line pattern.
    multiline = {}
    for hp in headers:
        text = header_text[hp]
        for m in re.finditer(r'^[ \t]*extern\b[^\n]*\{', text, re.M):
            # The body has semicolons of its own, so scan braces rather than
            # trying to bound the match with a regex.
            depth, i = 0, m.end() - 1
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            tail = re.match(r'\}\s*(\w+)\s*(?:\[[^\]]*\])?\s*;', text[i:])
            if tail:
                multiline.setdefault(tail.group(1), (hp, text[m.start():i]))

    # 28k labels against 350 files is far too many searches to do blindly.
    # Only labels that appear as a whole word somewhere can possibly match.
    corpus = '\n'.join(header_text.values())
    present = set(re.findall(r'\b\w+\b', corpus))

    done = set()
    for name in sorted(labels):
        if name not in present or name in done:
            continue
        pattern = re.compile(DECL_RE_TMPL % re.escape(name), re.M)
        decl = None
        for hp in headers:
            m = pattern.search(header_text[hp])
            if m:
                decl = m.group(0)
                try:
                    decl_source[name] = str(hp.relative_to(args.tree / 'include'))
                except ValueError:
                    decl_source[name] = None
                break
        if decl is None:
            continue  # not declared by C -- reached through ROM pointers instead

        # Resolve every label this one declaration declares, before the line
        # gets commented out and the rest become invisible.
        for other in [n for n in labels
                      if n != name and n in present
                      and re.search(r'\b%s\b' % re.escape(n), decl)]:
            m2 = declaration_to_macro(decl, other, labels[other])
            if m2 is None:
                skipped.append((other, decl.strip()))
            else:
                resolved.append((other, labels[other], m2))
            done.add(other)

        # A table of function pointers must not become an address macro: the
        # values in ROM are ARM code addresses, and calling one traps.
        mt = RE_TYPEDEF_ARRAY.match(decl.strip())
        if mt and mt.group('name') == name and mt.group('type') in fn_typedefs:
            typedef_tables[name] = mt.group('type')
            skipped.append((name, decl.strip()))
            continue

        macro = declaration_to_macro(decl, name, labels[name])
        if macro is None:
            # Some declarations cannot become a macro at all -- gLevelObjLists
            # is an array of an anonymous transparent_union spread over four
            # lines, and there is no type name to cast to.  Those get real
            # storage instead, filled from the ROM at startup.  The bytes are
            # copied verbatim, so the ROM pointers inside them stay valid.
            skipped.append((name, decl.strip()))
            continue
        resolved.append((name, labels[name], macro))


    # Referenced symbols that no macro can express get storage + a ROM copy.
    copies = []
    for name in sorted(set(multiline) | {n for n, _ in skipped}):
        if name not in labels or name in {n for n, _, _ in resolved}:
            continue
        size = extent.get(name, 0)
        if size <= 0 or size > 0x100000:
            continue
        copies.append((name, labels[name], size))

    # One removal pass at the end, over every symbol that got a macro.
    # Doing this inline while resolving missed any symbol that was resolved as
    # part of another declaration's group: its own declaration was never
    # visited, survived, and then expanded the macro into a declarator.
    for name in sorted({n for n, _, _ in resolved}):
        pattern = re.compile(DECL_RE_TMPL % re.escape(name), re.M)
        for hp in headers:
            new = pattern.sub(
                lambda m: '/* PORT: replaced by port/rom_data.h -- %s */'
                          % m.group(0).strip(),
                header_text[hp])
            if new != header_text[hp]:
                header_text[hp] = new
                dirty.add(hp)

    for hp in dirty:
        hp.write_text(header_text[hp])

    # Function-pointer tables in ROM hold ARM code addresses.  A wasm function
    # pointer is a table index, so those values are meaningless here and an
    # indirect call through one traps immediately.  Rather than let the game
    # die on its first animation command, fill the table with a stub of the
    # right signature that reports itself -- the call still does nothing, but
    # it does nothing survivably.
    fn_tables = []
    for name, decl in skipped:
        if name not in labels:
            continue
        m = RE_FN_PTR_ARRAY.match(decl)
        if m:
            ret, params = m.group('ret').strip(), m.group('params')
        elif name in typedef_tables:
            ret, params = fn_typedefs[typedef_tables[name]]
        else:
            continue
        count = max(1, extent.get(name, 4) // 4)
        if rom and mapping:
            count = extend_fn_table(rom, mapping, defined, labels[name], count)
        fn_tables.append((name, ret, params, count, decl_source.get(name)))
    fn_names = {n for n, _, _, _, _ in fn_tables}
    copies = [c for c in copies if c[0] not in fn_names]

    if args.out_copies is not None:
        args.out_copies.parent.mkdir(parents=True, exist_ok=True)
        with args.out_copies.open('w') as f:
            f.write('/* Generated by tools/gen_rom_data.py -- do not edit.\n'
                    ' *\n'
                    ' * Storage for ROM data symbols whose declaration cannot be\n'
                    ' * rewritten as an address macro (an array of an anonymous\n'
                    ' * union, for instance).  The contents are copied out of the\n'
                    ' * player\'s ROM at startup, byte for byte, so any ROM pointers\n'
                    ' * inside them keep pointing at the mapped ROM image. */\n\n')
            # No game headers here on purpose: this file defines storage for
            # symbols the headers also declare, with a type it cannot name.
            f.write('#include <string.h>\n#include "port/port.h"\n\n')

            for name, addr, size in copies:
                f.write('__attribute__((aligned(4))) u8 %s[%d];\n' % (name, size))
            f.write('\nvoid PortLoadRomDataCopies(void)\n{\n')
            for name, addr, size in copies:
                f.write('    memcpy(%s, (const void *)0x%08Xu, %d);\n'
                        % (name, addr, size))
            f.write('}\n')

    # Function pointers sitting *inside* ROM structs.  `gUnk_08351648` is an
    # array of 219 object descriptors, each with a constructor at +0x10, and
    # `CreateLevelObjects` calls through it for every object in a level -- so
    # this is what stands between the menus and actually loading a room.
    # The values are ARM addresses like everything else in ROM, so they are
    # rewritten in place at startup, once the ROM is mapped.
    patches = []
    if rom and mapping:
        for name, addr in sorted(labels.items()):
            if name not in present:
                continue
            decl = struct_arrays.get(name)
            if decl is None or decl not in structs:
                continue
            stride, members = structs[decl]
            span = extent.get(name, 0)
            if not stride or not span or span % stride:
                continue
            for i in range(span // stride):
                for off, (ret, params) in members:
                    at = addr + i * stride + off
                    romoff = at - ROM_START
                    if romoff + 4 > len(rom):
                        continue
                    value = int.from_bytes(rom[romoff:romoff + 4], 'little') & ~1
                    sym = mapping.get(value)
                    patches.append((at, sym if sym in defined else None,
                                    ret, params, name))

    wired = missing = 0
    if args.out_tables is not None:
        args.out_tables.parent.mkdir(parents=True, exist_ok=True)
        with args.out_tables.open('w') as f:
            f.write('/* Generated by tools/gen_rom_data.py -- do not edit.\n'
                    ' *\n'
                    " * Function-pointer tables that live in ROM.  Their entries are ARM\n"
                    ' * code addresses, and a WebAssembly function pointer is a table\n'
                    ' * index, so those values mean nothing here -- calling one traps.\n'
                    " * Each entry is looked up in the GBA build's link map and rebuilt as\n"
                    ' * a reference to the decompiled C function of the same name.  Entries\n'
                    ' * whose function is still ARM-only get a stub of the right signature,\n'
                    ' * so the call reports itself instead of taking the game down. */\n\n')
            wanted = {h for _, _, _, _, h in fn_tables if h}
            for name, ret, params, count, _ in fn_tables:
                ents = (resolve_fn_table(rom, mapping, defined, labels[name], count)
                        if rom and mapping else [])
                wanted |= {declared[e] for e in ents if e and e in declared}
            wanted |= {declared[s] for _, s, _, _, _ in patches
                       if s and s in declared}

            f.write('#include "port/port.h"\n')
            for hdr in sorted(wanted):
                f.write('#include "%s"\n' % hdr)
            f.write('\n')

            for name, ret, params, count, _ in fn_tables:
                entries = (resolve_fn_table(rom, mapping, defined,
                                            labels[name], count)
                           if rom and mapping else [None] * count)
                have = sum(1 for e in entries if e)
                wired += have
                missing += count - have
                f.write('/* %s: %d entries; %d resolved to decompiled C, '
                        '%d still ARM-only. */\n' % (name, count, have,
                                                     count - have))
                for fn in sorted({e for e in entries if e}):
                    if fn not in declared:
                        f.write('extern %s %s(%s);\n' % (ret, fn, params))
                f.write('static %s PortRomFn_%s(%s)\n{\n'
                        % (ret, name, name_parameters(params)))
                f.write('    PortMissingFunction("%s[] (ROM function table)");\n'
                        % name)
                if ret != 'void':
                    f.write('    return (%s)0;\n' % ret)
                f.write('}\n\n')
                f.write('%s (*const %s[%d])(%s) = {\n' % (ret, name, count, params))
                for entry in entries:
                    if entry:
                        f.write('    (%s (*)(%s))%s,\n' % (ret, params, entry))
                    else:
                        f.write('    PortRomFn_%s,\n' % name)
                f.write('};\n\n')

            if patches:
                sigs = {}
                for _, sym, ret, params, table in patches:
                    sigs.setdefault((ret, params), set()).add(table)
                for n_, (ret, params) in enumerate(sorted(sigs)):
                    f.write('static %s PortRomStructFn%d(%s)\n{\n'
                            % (ret, n_, name_parameters(params)))
                    f.write('    PortMissingFunction("a function pointer inside '
                            'a ROM struct (%s)");\n' % ', '.join(sorted(sigs[(ret, params)])))
                    if ret != 'void':
                        f.write('    return (%s)0;\n' % ret)
                    f.write('}\n\n')
                sig_index = {s: i for i, s in enumerate(sorted(sigs))}

                for sym in sorted({s for _, s, _, _, _ in patches if s}):
                    if sym in declared:
                        continue
                    ret, params = next((r, p) for _, s, r, p, _ in patches if s == sym)
                    f.write('extern %s %s(%s);\n' % (ret, sym, params))
                f.write('\n/* %d function pointers inside ROM structs; %d resolved. */\n'
                        % (len(patches), sum(1 for _, s, _, _, _ in patches if s)))
                f.write('static const struct { u32 at; void *fn; } sRomStructFns[] = {\n')
                for at, sym, ret, params, _ in patches:
                    target = sym or ('PortRomStructFn%d' % sig_index[(ret, params)])
                    f.write('    { 0x%08Xu, (void *)%s },\n' % (at, target))
                f.write('};\n\n')
                f.write('void PortPatchRomFunctionPointers(void)\n{\n'
                        '    u32 i;\n\n'
                        '    for (i = 0; i < sizeof(sRomStructFns) / sizeof(sRomStructFns[0]); i++)\n'
                        '        *(void **)sRomStructFns[i].at = sRomStructFns[i].fn;\n'
                        '}\n')
            else:
                f.write('void PortPatchRomFunctionPointers(void) { }\n')

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('w') as f:
        f.write('/* Generated by tools/gen_rom_data.py -- do not edit.\n'
                ' * Addresses point into the ROM image the player supplies at run time. */\n')
        f.write('#ifndef GUARD_PORT_ROM_DATA_H\n#define GUARD_PORT_ROM_DATA_H\n\n')
        f.write('#include "global.h"\n\n')
        for name, addr, define in resolved:
            f.write('%s\n' % define)
        f.write('#endif\n')

    print('gen_rom_data: %d data labels, %d referenced by C and resolved'
          % (len(labels), len(resolved)))
    if patches:
        print('  %d function pointers inside ROM structs, %d resolved'
              % (len(patches), sum(1 for _, s, _, _, _ in patches if s)))
    if fn_tables:
        print('  %d ROM function tables: %d entries wired to decompiled C, '
              '%d stubbed' % (len(fn_tables), wired, missing))
    if args.out_copies is not None:
        print('  %d given storage copied from the ROM at startup: %s'
              % (len(copies), ', '.join(n for n, _, _ in copies) or '-'))
    if skipped:
        print('  %d SKIPPED -- declaration shape not understood:' % len(skipped))
        for name, decl in skipped:
            print('      %-28s %s' % (name, decl[:64]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
