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
import struct
import sys
from collections import namedtuple
from pathlib import Path


# The 64-bit builds compile these generated sources as C++ so that GBA
# structures keep 4-byte pointer members (docs/SIXTYFOUR.md, tools/cxxify.py).
# Without C linkage the function definitions mangle and the const tables become
# internal, so the game cannot see either.  A no-op in C.
EXTERN_C_OPEN = '#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
EXTERN_C_CLOSE = '\n#ifdef __cplusplus\n}\n#endif\n'

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


def parse_elf_functions(path):
    """{rom address: symbol} for every function in katam.elf's symbol table.

    The link map only lists symbols the *linker* had to resolve, which means
    globals.  A `static` function in the decompilation is invisible to it, and
    18 of the 26 unresolved entries in gUnk_08351648 were unresolved for
    exactly that reason -- OBJ_SMALL_FOOD, OBJ_MEAT, OBJ_1UP, OBJ_WADDLE_DOO,
    OBJ_PARASOL and the rest have perfectly good decompiled C, it just has
    internal linkage, so nothing named the address the ROM holds.  The ELF's
    .symtab keeps local symbols, so it names them all.

    "Not in katam.map" and "not decompiled yet" look identical from the map's
    side and are completely different statements; this is what tells them
    apart.

    Parsed here rather than shelled out to `nm`, so `make sync` keeps needing
    nothing but python.  ELF32, little-endian, which is what agbcc emits; any
    other shape is ignored rather than guessed at.
    """
    data = path.read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        return {}

    e_shoff, = struct.unpack_from('<I', data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x2E)
    if not e_shoff or not e_shnum:
        return {}

    out = {}
    SHT_SYMTAB, STT_FUNC = 2, 2
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from('<I', data, sh + 4)
        if sh_type != SHT_SYMTAB:
            continue
        sh_offset, sh_size, sh_link = struct.unpack_from('<III', data, sh + 0x10)
        st = e_shoff + sh_link * e_shentsize
        str_off, str_size = struct.unpack_from('<II', data, st + 0x10)
        strtab = data[str_off:str_off + str_size]
        for off in range(sh_offset, sh_offset + sh_size, 16):
            st_name, st_value = struct.unpack_from('<II', data, off)
            st_info = data[off + 12]
            if st_info & 0xF != STT_FUNC or not st_value:
                continue
            end = strtab.find(b'\0', st_name)
            name = strtab[st_name:end].decode('ascii', 'replace')
            # The low bit is the Thumb flag on a branch target, not part of
            # the address; ELF symbol values already have it clear.
            if name and not name.startswith(('$', '.')):
                out.setdefault(st_value & ~1, name)
    return out


def wasm_sig(ret, params):
    """(parameter count, returns a value) -- all wasm cares about here.

    Every pointer and every integer 32 bits or narrower is an i32, so
    `void f(struct A *)` and `void f(struct B *)` are the same signature and
    must not be reported.  What traps is a different number of parameters, or
    one side returning a value where the other does not."""
    params = ' '.join(params.split())
    if params in ('', 'void'):
        n = 0
    else:
        depth, n = 0, 1
        for ch in params:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            elif ch == ',' and depth == 0:
                n += 1
    return (n, ret.strip() != 'void')


def function_signatures(sources):
    """{name: (return type, parameters)} from definitions in the tree."""
    out = {}
    for cp in sources:
        text = cp.read_text(errors='replace')
        for m in re.finditer(r'^([A-Za-z_][\w \t\*]*?)\b(\w+)\s*\(([^;{)]*(?:\([^)]*\)[^;{)]*)*)\)'
                             r'\s*(?:\{|\n\s*\{)', text, re.M):
            ret = re.sub(r'\b(static|inline)\b', '', m.group(1)).strip()
            out.setdefault(m.group(2), (ret, m.group(3)))
    return out


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

# One function pointer found inside a ROM structure.  `index` and `off` are
# what identify it to a reader -- gUnk_08351648[0x5E] is OBJ_SMALL_FOOD, and
# knowing that is the difference between a usable report and "something in
# a ROM struct".
Patch = namedtuple('Patch', 'at sym ret params table index off value')


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
    ap.add_argument('--elf', type=Path,
                    help="the GBA build's katam.elf.  Its symbol table names "
                         'the file-local functions the link map leaves out, '
                         'which is most of what is otherwise unresolvable')
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
    # The map is left authoritative and the ELF only fills its gaps, so adding
    # this cannot move an address that already resolved.
    from_elf = {}
    if args.elf and args.elf.exists():
        for addr, sym in parse_elf_functions(args.elf).items():
            if addr not in mapping:
                mapping[addr] = sym
                from_elf[addr] = sym
    rom = args.rom.read_bytes() if args.rom and args.rom.exists() else None
    signatures = {}
    defined = set()
    # The platform layer counts too -- it defines replacements for things the
    # decomp has in ROM (the SRAM library's version string, for one), and those
    # must not be turned into address macros either.
    sources = list((args.tree / 'src').rglob('*.c'))
    sources += sorted(Path('platform').rglob('*.c'))
    signatures = function_signatures(sources)
    for cp in sources:
        text = cp.read_text(errors='replace')
        # Functions.
        for m in re.finditer(r'^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;]*?\)\s*\{',
                             text, re.M):
            defined.add(m.group(1))
        # File-scope data.  These matter as much as functions here: a symbol the
        # port defines itself must never be turned into an address macro, or its
        # own definition stops parsing.  Anchoring at column 0 keeps locals out.
        #
        # This set only ever *excludes* symbols from being macro'd, so a false
        # positive costs nothing and a miss breaks the build -- hence the loose
        # rule: on any line starting a declaration with an initialiser, take the
        # last identifier before the '[' or '='.  Declarators here come in every
        # shape, `struct RoomTiledBG *const gUnk_082D8D74[] =` among them.
        for line in text.splitlines():
            if not line[:1].isalpha() or '=' not in line:
                continue
            head = line.split('=', 1)[0].split('[', 1)[0]
            names = re.findall(r'\b(\w+)\b', head)
            if names:
                defined.add(names[-1])

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


    # data/*.s is not the only place ROM symbols live.  The sound tables are
    # assembled from sound/, so `gSongTable` never appears as a data label --
    # and without it the port stubbed the table as four zero entries.  That is
    # far worse than it sounds: PlaySfxInternal does
    #
    #     gUnk_08D60FA4[gSongTable[num].ms]->unk4
    #
    # so any real sound id read past the stub, took a garbage `.ms` as an index
    # into a ROM pointer table, and dereferenced whatever came back.  It is
    # what was killing the port a few seconds into gameplay.
    #
    # The link map knows where all of these live.  Any symbol a header declares
    # that the port does not define, and that the map places in ROM, becomes an
    # address macro like every other piece of ROM data.
    from_map = []
    if mapping:
        resolved_names = {n for n, _, _ in resolved} | set(typedef_tables)
        for addr, sym in sorted(mapping.items()):
            if sym in resolved_names or sym in labels or sym in defined:
                continue
            if not (ROM_START <= addr < ROM_END) or sym not in present:
                continue
            decl = None
            for hp in headers:
                m = re.search(DECL_RE_TMPL % re.escape(sym), header_text[hp], re.M)
                if m:
                    decl = m.group(0)
                    break
            if decl is None:
                continue
            macro = declaration_to_macro(decl, sym, addr)
            if macro is None:
                continue
            resolved.append((sym, addr, macro))
            from_map.append(sym)

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
        # The label's own extent, and nothing more.  An earlier version walked
        # forward while the following words still resolved to known functions;
        # that is unsound exactly where two function tables abut, because the
        # successor's entries resolve perfectly.  It read gUnk_0834BD88 as 12
        # entries when it is 3, and the 9 extra entries had a different
        # signature -- which is a trap waiting at the first dispatch.
        count = max(1, extent.get(name, 4) // 4)
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
            f.write(EXTERN_C_OPEN)

            for name, addr, size in copies:
                f.write('__attribute__((aligned(4))) u8 %s[%d];\n' % (name, size))
            f.write('\nvoid PortLoadRomDataCopies(void)\n{\n')
            for name, addr, size in copies:
                f.write('    memcpy(%s, (const void *)0x%08Xu, %d);\n'
                        % (name, addr, size))
            f.write('}\n')
            f.write(EXTERN_C_CLOSE)

    # Function pointers sitting *inside* ROM structs.  `gUnk_08351648` is an
    # array of 219 object descriptors, each with a `void (*unk10)(struct
    # Object2 *)` at +0x10 -- not the constructor, but the per-type setup each
    # object runs at the end of its own Create function, from ~20 call sites.
    # So this is what stands between the menus and actually loading a room.
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
                    patches.append(Patch(at, sym if sym in defined else None,
                                         ret, params, name, i, off, value))

    wired = missing = 0
    sig_rejects = []
    referenced = set()   # every function rom_fn_tables.c ends up naming
    if args.out_tables is not None:
        args.out_tables.parent.mkdir(parents=True, exist_ok=True)
        with args.out_tables.open('w') as f:
            f.write('/* Generated by tools/gen_rom_data.py -- do not edit.\n'
                    ' *\n'
                    " * Function-pointer tables that live in ROM.  Their entries are ARM\n"
                    ' * code addresses, and a WebAssembly function pointer is a table\n'
                    ' * index, so those values mean nothing here -- calling one traps.\n'
                    " * Each entry is looked up in the GBA build's link map, then in\n"
                    " * katam.elf's symbol table (which is the only one of the two that\n"
                    ' * names file-local functions), and rebuilt as a reference to the\n'
                    ' * decompiled C function of the same name.\n'
                    ' *\n'
                    ' * An entry whose function is still ARM-only gets a stub of the right\n'
                    ' * signature, which reports itself at error level, naming the table,\n'
                    ' * the index and the ARM address, and returns.  Deliberately not\n'
                    ' * fatal: these are per-type setup routines that run *after* the\n'
                    ' * object is built, so the object exists either way, and the eight\n'
                    ' * that remain belong to Dark Mind and the boss challenge door --\n'
                    ' * halting there would take down a room the port otherwise plays.\n'
                    ' * The report names the entry precisely so that a crash somewhere\n'
                    ' * else can be traced back to it. */\n\n')
            wanted = {h for _, _, _, _, h in fn_tables if h}
            for name, ret, params, count, _ in fn_tables:
                ents = (resolve_fn_table(rom, mapping, defined, labels[name], count)
                        if rom and mapping else [])
                wanted |= {declared[e] for e in ents if e and e in declared}
            wanted |= {declared[p.sym] for p in patches
                       if p.sym and p.sym in declared}

            f.write('#include "port/port.h"\n')
            for hdr in sorted(wanted):
                f.write('#include "%s"\n' % hdr)
            f.write('\n')
            f.write(EXTERN_C_OPEN)

            # One extern per symbol for the whole file.  The same function can
            # sit in two tables whose element types differ, and declaring it
            # twice with two signatures is a hard error -- so declare it once
            # and let the cast at each use site reconcile them, exactly as the
            # ROM does by storing one address for both.
            emitted = set()

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
                # A cast makes any function fit the table's element type at
                # compile time and traps at the first dispatch: wasm checks the
                # callee's real type against the call site's.  So an entry whose
                # signature does not match is not wired at all -- it gets the
                # reporting stub, and is named here at build time.
                want = wasm_sig(ret, params)
                for i, fn in enumerate(entries):
                    if not fn:
                        continue
                    have = signatures.get(fn)
                    if have is None:
                        continue
                    if wasm_sig(have[0], have[1]) != want:
                        sig_rejects.append((name, i, fn,
                                            '%s(%s)' % (have[0], ' '.join(have[1].split())),
                                            '%s(%s)' % (ret, ' '.join(params.split()))))
                        entries[i] = None

                referenced |= {e for e in entries if e}
                for fn in sorted({e for e in entries if e}):
                    if fn not in declared and fn not in emitted:
                        emitted.add(fn)
                        f.write('extern %s %s(%s);\n' % (ret, fn, params))
                f.write('static %s PortRomFn_%s(%s)\n{\n'
                        % (ret, name, name_parameters(params)))
                f.write('    PortMissingFunction("%s[] (ROM function table)");\n'
                        % name)
                if ret != 'void':
                    f.write('    return (%s)0;\n' % ret)
                f.write('}\n\n')
                # `extern` is load-bearing, and only on the 64-bit builds.
                # These are const arrays, and a const object at namespace scope
                # has internal linkage in C++ where it has external linkage in
                # C.  extern "C" does not fix that -- it sets *language*
                # linkage, not storage linkage -- so without this the table is
                # defined here, invisible everywhere else, and the game's
                # reference to it fails to link with no other symptom.
                f.write('extern %s (*const %s[%d])(%s);\n' % (ret, name, count, params))
                f.write('%s (*const %s[%d])(%s) = {\n' % (ret, name, count, params))
                for entry in entries:
                    if entry:
                        f.write('    (%s (*)(%s))%s,\n' % (ret, params, entry))
                    else:
                        f.write('    PortRomFn_%s,\n' % name)
                f.write('};\n\n')

            if patches:
                # One stub per unresolved *entry*, not one per signature.
                # A shared stub can only name the table it belongs to, and
                # "a function pointer inside a ROM struct (gUnk_08351648)" in a
                # crash report says nothing about which of 219 object types
                # spawned without its per-type setup.  The index is the object
                # type constant, so the reader can look it up.
                stub = {}
                for p in patches:
                    if p.sym or p.at in stub:
                        continue
                    stub[p.at] = 'PortRomStructFn_%08X' % p.at
                    f.write('static %s %s(%s)\n{\n'
                            % (p.ret, stub[p.at], name_parameters(p.params)))
                    f.write('    PortMissingFunction("%s[0x%X]+0x%X (ROM struct '
                            'function pointer, ARM address 0x%08X)");\n'
                            % (p.table, p.index, p.off, p.value))
                    if p.ret != 'void':
                        f.write('    return (%s)0;\n' % p.ret)
                    f.write('}\n\n')

                for sym in sorted({p.sym for p in patches if p.sym}):
                    if sym in declared or sym in emitted:
                        continue
                    emitted.add(sym)
                    p = next(q for q in patches if q.sym == sym)
                    f.write('extern %s %s(%s);\n' % (p.ret, sym, p.params))
                f.write('\n/* %d function pointers inside ROM structs; %d resolved. */\n'
                        % (len(patches), sum(1 for p in patches if p.sym)))
                f.write('static const struct { u32 at; void *fn; } sRomStructFns[] = {\n')
                for p in patches:
                    f.write('    { 0x%08Xu, (void *)%s },\n'
                            % (p.at, p.sym or stub[p.at]))
                f.write('};\n\n')
                f.write('void PortPatchRomFunctionPointers(void)\n{\n'
                        '    u32 i;\n\n'
                        '    for (i = 0; i < sizeof(sRomStructFns) / sizeof(sRomStructFns[0]); i++)\n'
                        '        *(void **)sRomStructFns[i].at = sRomStructFns[i].fn;\n'
                        '}\n')
            else:
                f.write('void PortPatchRomFunctionPointers(void) { }\n')

            f.write(EXTERN_C_CLOSE)

    # Anything rom_fn_tables.c names has to be linkable from another
    # translation unit, and half of what the ELF adds is `static`.  Dropping
    # the keyword is the whole change -- the function is otherwise untouched,
    # and the decompilation's own build is not involved, since this edits the
    # copy in build/port-src.  Only names defined exactly once in the tree
    # qualify: two files could each have a static of the same name, and giving
    # both external linkage would be a duplicate symbol.
    unstatic = []
    static_defs = {}
    for cp in sources:
        for m in re.finditer(r'^static\s+[A-Za-z_][\w \t\*]*?\b(\w+)\s*'
                             r'\([^;{]*\)\s*\{',
                             cp.read_text(errors='replace'), re.M):
            static_defs.setdefault(m.group(1), set()).add(cp)
    for sym in sorted(referenced | {p.sym for p in patches if p.sym}):
        where = static_defs.get(sym)
        if not where or len(where) != 1:
            continue
        cp = next(iter(where))
        text = cp.read_text(errors='replace')
        new, n = re.subn(r'^static(\s+[A-Za-z_][\w \t\*]*?\b%s\s*\()'
                         % re.escape(sym),
                         r'/* PORT: was static; a ROM table holds its address */\1',
                         text, flags=re.M)
        if n:
            cp.write_text(new)
            unstatic.append((sym, cp.name))

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
    if from_map:
        print('  %d further ROM symbols resolved from katam.map: %s'
              % (len(from_map), ', '.join(from_map[:8])
                 + (' ...' if len(from_map) > 8 else '')))
    if from_elf:
        print('  %d ROM addresses named only by katam.elf (file-local '
              'functions the link map does not list)' % len(from_elf))
    if unstatic:
        print('  %d given external linkage so a ROM table can hold their '
              'address: %s'
              % (len(unstatic),
                 ', '.join('%s (%s)' % u for u in sorted(set(unstatic)))))
    if patches:
        print('  %d function pointers inside ROM structs, %d resolved'
              % (len(patches), sum(1 for p in patches if p.sym)))
        still = sorted({(p.table, p.index, p.value) for p in patches if not p.sym})
        if still:
            print('      still unresolved (the object type spawns without its '
                  'per-type setup):')
            for table, index, value in still:
                print('        %s[0x%02X]  ARM 0x%08X  %s'
                      % (table, index, value,
                         'no function there at all' if not value
                         else 'still ARM assembly'))
    if sig_rejects:
        print('  %d table entries REJECTED -- the function\'s signature does not '
              'match the table, so calling it would trap:' % len(sig_rejects))
        for tbl, idx, fn, have, want in sig_rejects:
            print('      %s[%d] = %s' % (tbl, idx, fn))
            print('          is   %s' % have)
            print('          want %s' % want)
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
