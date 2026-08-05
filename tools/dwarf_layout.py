#!/usr/bin/env python3
"""
dwarf_layout.py -- read struct and union layout out of an object file.

This is the measuring instrument the layout table is built with.  It is here
rather than inside gen_gba_layout.py because two things want it: the generator,
which records what the ILP32 build produces, and anyone comparing two builds to
find out what a different ABI would change.

Why DWARF from a *modern* compiler and not from katam.elf
---------------------------------------------------------
katam.elf carries .debug_info, and it is not usable.  agbcc is gcc 2.95 and
emits DWARF 1-era tags that modern tools reject outright -- gdb stops at
`unexpected tag 'DW_TAG_imported_declaration'` before it has read a single
type.  So the ELF is not the source of truth here.  The working 32-bit builds
are: wasm32, i686 and armhf all agree with the console on every layout the port
has ever been able to check, and they are what the port actually compiles.

Why DWARF and not a hand-written parse of the headers
-----------------------------------------------------
Because the compiler is the authority on what a struct's layout is, and the
question has more corners than it looks: anonymous unions, bitfields, arrays of
incomplete type, `__attribute__((packed))`, and the decompilation's habit of
declaring the same struct differently in two headers.  A regex over the headers
answers a related question, not this one.

Only enough DWARF is implemented to answer it: .debug_abbrev, the DIE tree in
.debug_info, the two string sections, and the DWARF 5 file table out of
.debug_line so a struct can name the header it came from.  Pure python, no
dependencies -- the same rule tools/gen_ram_symbols.py and tools/gen_rom_data.py
follow.
"""

import struct
import sys
from pathlib import Path

# --- DWARF constants -------------------------------------------------------

DW_TAG_structure_type = 0x13
DW_TAG_union_type = 0x17
DW_TAG_member = 0x0D
DW_TAG_typedef = 0x16
DW_TAG_pointer_type = 0x0F
DW_TAG_array_type = 0x01
DW_TAG_const_type = 0x26
DW_TAG_volatile_type = 0x35
DW_TAG_base_type = 0x24
DW_TAG_enumeration_type = 0x04
DW_TAG_subroutine_type = 0x15
DW_TAG_subrange_type = 0x21

DW_AT_name = 0x03
DW_AT_byte_size = 0x0B
DW_AT_bit_size = 0x0D
DW_AT_data_member_location = 0x38
DW_AT_data_bit_offset = 0x6B
DW_AT_type = 0x49
DW_AT_decl_file = 0x3A
DW_AT_decl_line = 0x3B
DW_AT_declaration = 0x3C
DW_AT_upper_bound = 0x2F
DW_AT_count = 0x37
DW_AT_encoding = 0x3E
DW_AT_stmt_list = 0x10
DW_AT_comp_dir = 0x1B


class Reader:
    """A cursor over bytes, with the LEB128 decoders DWARF is built from."""

    def __init__(self, data, off=0):
        self.d = data
        self.o = off

    def eof(self):
        return self.o >= len(self.d)

    def u8(self):
        v = self.d[self.o]
        self.o += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.o)[0]
        self.o += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]
        self.o += 4
        return v

    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.o)[0]
        self.o += 8
        return v

    def uleb(self):
        r, s = 0, 0
        while True:
            b = self.d[self.o]
            self.o += 1
            r |= (b & 0x7F) << s
            if not b & 0x80:
                return r
            s += 7

    def sleb(self):
        r, s = 0, 0
        while True:
            b = self.d[self.o]
            self.o += 1
            r |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                if b & 0x40:
                    r -= 1 << s
                return r

    def bytes(self, n):
        v = self.d[self.o:self.o + n]
        self.o += n
        return v

    def cstr(self):
        e = self.d.index(b'\0', self.o)
        v = self.d[self.o:e].decode('utf-8', 'replace')
        self.o = e + 1
        return v


def read_elf_sections(path):
    """{name: bytes} for every section.  32- and 64-bit little-endian ELF.

    RELA relocations against the .debug_* sections are applied on the way out,
    and that is not an optimisation.  A relocatable object records every
    reference from .debug_info into .debug_str as a relocation; on i386 those
    are REL and the addend is already sitting in the section, so an unrelocated
    read works by accident.  On x86-64 and aarch64 they are RELA, the addend
    lives in the relocation, and the section holds zero -- so every string in
    the file reads back as whichever one happens to be at offset 0.  That
    failure is entirely silent: the DIE tree parses, the sizes and offsets are
    right, and every struct is called the same thing.
    """
    d = bytearray(Path(path).read_bytes())
    if d[:4] != b'\x7fELF':
        sys.exit('dwarf_layout: %s is not an ELF file' % path)
    is64 = d[4] == 2
    if d[5] != 1:
        sys.exit('dwarf_layout: %s is big-endian; not handled' % path)
    if is64:
        e_shoff, = struct.unpack_from('<Q', d, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
        fmt, name_o, off_o, size_o = '<Q', 0x00, 0x18, 0x20
        type_o, link_o, entsz_o = 0x04, 0x28, 0x38
    else:
        e_shoff, = struct.unpack_from('<I', d, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        fmt, name_o, off_o, size_o = '<I', 0x00, 0x10, 0x14
        type_o, link_o, entsz_o = 0x04, 0x18, 0x24

    def hdr(i):
        b = e_shoff + i * e_shentsize
        nm, = struct.unpack_from('<I', d, b + name_o)
        ty, = struct.unpack_from('<I', d, b + type_o)
        of, = struct.unpack_from(fmt, d, b + off_o)
        sz, = struct.unpack_from(fmt, d, b + size_o)
        ln, = struct.unpack_from('<I', d, b + link_o)
        es, = struct.unpack_from(fmt, d, b + entsz_o)
        return nm, ty, of, sz, ln, es

    _, _, stroff, _, _, _ = hdr(e_shstrndx)

    def secname(nm):
        end = d.index(b'\0', stroff + nm)
        return d[stroff + nm:end].decode()

    heads = [hdr(i) for i in range(e_shnum)]
    names = [secname(h[0]) for h in heads]

    SHT_RELA = 4
    for i, h in enumerate(heads):
        _, ty, of, sz, link, es = h
        if ty != SHT_RELA or not names[i].startswith('.rela.debug'):
            continue
        target = names[i][len('.rela'):]
        if target not in names:
            continue
        t = heads[names.index(target)]
        tgt_off, tgt_size = t[2], t[3]
        # The symbol table this relocation section indexes, so a reference to a
        # real symbol rather than a section symbol still resolves.
        symtab = heads[link] if link < len(heads) else None
        step = 24 if is64 else 12
        for p in range(of, of + sz, step):
            if is64:
                r_off, r_info, r_add = struct.unpack_from('<QQq', d, p)
                sym_idx = r_info >> 32
            else:
                r_off, r_info, r_add = struct.unpack_from('<IIi', d, p)
                sym_idx = r_info >> 8
            base = 0
            if symtab is not None and sym_idx:
                sym_entsize = symtab[5] or (24 if is64 else 16)
                sp = symtab[2] + sym_idx * sym_entsize
                base, = struct.unpack_from('<Q' if is64 else '<I', d,
                                           sp + (0x08 if is64 else 0x04))
            if r_off + 4 > tgt_size:
                continue
            # Four bytes, always.  Every relocated field in a 32-bit-format
            # DWARF section is a four-byte offset; the one exception is
            # DW_AT_low_pc, whose value nothing here reads.
            struct.pack_into('<I', d, tgt_off + r_off, (base + r_add) & 0xFFFFFFFF)

    out = {}
    for i, h in enumerate(heads):
        out[names[i]] = bytes(d[h[2]:h[2] + h[3]])
    return out


def parse_abbrev(data, offset):
    """{code: (tag, has_children, [(attr, form, implicit_const)])}"""
    r = Reader(data, offset)
    table = {}
    while not r.eof():
        code = r.uleb()
        if code == 0:
            break
        tag = r.uleb()
        children = r.u8()
        attrs = []
        while True:
            at = r.uleb()
            form = r.uleb()
            const = r.sleb() if form == 0x21 else None  # DW_FORM_implicit_const
            if at == 0 and form == 0:
                break
            attrs.append((at, form, const))
        table[code] = (tag, children, attrs)
    return table


class Dwarf:
    def __init__(self, path):
        self.path = str(path)
        self.sec = read_elf_sections(path)
        for want in ('.debug_info', '.debug_abbrev'):
            if want not in self.sec:
                sys.exit('dwarf_layout: %s has no %s -- compile with -g'
                         % (path, want))
        self.str_ = self.sec.get('.debug_str', b'')
        self.line_str = self.sec.get('.debug_line_str', b'')
        self.str_offsets = self.sec.get('.debug_str_offsets', b'')
        self.addr_size = 4
        self.dies = {}      # offset -> die
        self.files = []     # DWARF 5 file table of the first CU
        self._parse_info()

    # -- strings ------------------------------------------------------------

    def _str_at(self, sec, off):
        end = sec.index(b'\0', off)
        return sec[off:end].decode('utf-8', 'replace')

    # -- forms --------------------------------------------------------------

    def _read_form(self, r, form, const, cu_off):
        # The set gcc and clang actually emit, plus the ones a future -gdwarf-4
        # would bring back.  Anything else is a bug rather than a fallback:
        # guessing a length here would desynchronise the whole DIE stream and
        # produce plausible-looking nonsense.
        if form == 0x01: return r.u32() if self.addr_size == 4 else r.u64()   # addr
        if form == 0x03: return r.bytes(2)                                    # block2
        if form == 0x04: return r.bytes(4)                                    # block4
        if form == 0x05: return r.u16()                                       # data2
        if form == 0x06: return r.u32()                                       # data4
        if form == 0x07: return r.u64()                                       # data8
        if form == 0x08: return r.cstr()                                      # string
        if form == 0x09: return r.bytes(r.uleb())                             # block
        if form == 0x0A: return r.bytes(r.u8())                               # block1
        if form == 0x0B: return r.u8()                                        # data1
        if form == 0x0C: return r.u8()                                        # flag
        if form == 0x0D: return r.sleb()                                      # sdata
        if form == 0x0E: return self._str_at(self.str_, r.u32())              # strp
        if form == 0x0F: return r.uleb()                                      # udata
        if form == 0x10: return r.uleb()                                      # ref_addr
        if form == 0x11: return cu_off + r.u8()                               # ref1
        if form == 0x12: return cu_off + r.u16()                              # ref2
        if form == 0x13: return cu_off + r.u32()                              # ref4
        if form == 0x14: return cu_off + r.u64()                              # ref8
        if form == 0x15: return cu_off + r.uleb()                             # ref_udata
        if form == 0x16:                                                      # indirect
            return self._read_form(r, r.uleb(), None, cu_off)
        if form == 0x17: return r.u32()                                       # sec_offset
        if form == 0x18: return r.bytes(r.uleb())                             # exprloc
        if form == 0x19: return True                                          # flag_present
        if form == 0x1A: return self._strx(r.uleb())                          # strx
        if form == 0x1B: return r.uleb()                                      # addrx
        if form == 0x1F: return self._str_at(self.line_str, r.u32())          # line_strp
        if form == 0x20: return r.u64()                                       # ref_sig8
        if form == 0x21: return const                                         # implicit_const
        if form == 0x22: return r.uleb()                                      # loclistx
        if form == 0x23: return r.uleb()                                      # rnglistx
        if form == 0x25: return self._strx(r.u8())                            # strx1
        if form == 0x26: return self._strx(r.u16())                           # strx2
        if form == 0x27: return self._strx(int.from_bytes(r.bytes(3), 'little'))
        if form == 0x28: return self._strx(r.u32())                           # strx4
        if form == 0x29: return r.u8()                                        # addrx1
        if form == 0x2A: return r.u16()                                       # addrx2
        if form == 0x2B: return int.from_bytes(r.bytes(3), 'little')          # addrx3
        if form == 0x2C: return r.u32()                                       # addrx4
        sys.exit('dwarf_layout: DW_FORM 0x%02X not handled (%s)'
                 % (form, self.path))

    def _strx(self, index):
        base = 8  # header of .debug_str_offsets for a 32-bit DWARF 5 unit
        off, = struct.unpack_from('<I', self.str_offsets, base + 4 * index)
        return self._str_at(self.str_, off)

    # -- .debug_info --------------------------------------------------------

    def _parse_info(self):
        info = self.sec['.debug_info']
        r = Reader(info)
        first_cu = True
        while r.o < len(info):
            cu_off = r.o
            unit_len = r.u32()
            if unit_len in (0xFFFFFFFF,):
                sys.exit('dwarf_layout: 64-bit DWARF format not handled')
            next_cu = r.o + unit_len
            version = r.u16()
            if version >= 5:
                r.u8()                      # unit type
                self.addr_size = r.u8()
                abbrev_off = r.u32()
            else:
                abbrev_off = r.u32()
                self.addr_size = r.u8()
            abbrevs = parse_abbrev(self.sec['.debug_abbrev'], abbrev_off)

            stack = []
            while r.o < next_cu:
                die_off = r.o
                code = r.uleb()
                if code == 0:
                    if stack:
                        stack.pop()
                    continue
                if code not in abbrevs:
                    sys.exit('dwarf_layout: abbrev %d not in table' % code)
                tag, children, attrs = abbrevs[code]
                die = {'tag': tag, 'off': die_off, 'children': [],
                       'parent': stack[-1] if stack else None}
                for at, form, const in attrs:
                    die[at] = self._read_form(r, form, const, cu_off)
                self.dies[die_off] = die
                if stack:
                    stack[-1]['children'].append(die)
                if children:
                    stack.append(die)

            if first_cu:
                first_cu = False
                root = self.dies.get(cu_off + (12 if version >= 5 else 11))
                if root is not None and DW_AT_stmt_list in root:
                    self.files = self._parse_line_files(root[DW_AT_stmt_list])
            r.o = next_cu

    # -- .debug_line, for DW_AT_decl_file -----------------------------------

    def _parse_line_files(self, offset):
        """The DWARF 5 file table.  Only the names; nothing wants the program."""
        data = self.sec.get('.debug_line')
        if not data:
            return []
        r = Reader(data, offset)
        r.u32()                              # unit length
        version = r.u16()
        if version < 5:
            return []                        # DWARF 4 file tables are a
                                             # different shape and nothing here
                                             # needs them yet
        r.u8(); r.u8()                       # address size, segment selector
        r.u32()                              # header length
        r.u8(); r.u8(); r.u8(); r.u8(); r.u8()  # min inst, max ops, default
                                             # is_stmt, line base, line range
        opcode_base = r.u8()
        for _ in range(opcode_base - 1):
            r.u8()

        def read_entries():
            fmt_count = r.u8()
            fmts = [(r.uleb(), r.uleb()) for _ in range(fmt_count)]
            count = r.uleb()
            rows = []
            for _ in range(count):
                row = {}
                for content, form in fmts:
                    row[content] = self._read_form(r, form, None, 0)
                rows.append(row)
            return rows

        dirs = [row.get(1, '') for row in read_entries()]   # DW_LNCT_path
        files = []
        for row in read_entries():
            name = row.get(1, '')
            d = row.get(2, 0)                                # DW_LNCT_directory_index
            if not name.startswith('/') and d < len(dirs):
                name = dirs[d].rstrip('/') + '/' + name
            files.append(name)
        return files

    # -- the interface anything else uses -----------------------------------

    def decl_file(self, die):
        i = die.get(DW_AT_decl_file)
        if i is None or i >= len(self.files):
            return None
        return self.files[i]

    def strip_typedefs(self, off):
        """Follow typedef/const/volatile to the type underneath."""
        seen = set()
        while off is not None and off not in seen:
            seen.add(off)
            die = self.dies.get(off)
            if die is None:
                return None
            if die['tag'] in (DW_TAG_typedef, DW_TAG_const_type,
                              DW_TAG_volatile_type):
                off = die.get(DW_AT_type)
                continue
            return die
        return None

    def has_pointer(self, off, depth=0):
        """Does this type store a pointer, directly or inside an array?

        Deliberately not transitive through a pointer: `struct A { struct B *b; }`
        has a pointer whether or not B does.
        """
        die = self.strip_typedefs(off)
        if die is None or depth > 8:
            return False
        if die['tag'] == DW_TAG_pointer_type:
            return True
        if die['tag'] == DW_TAG_array_type:
            return self.has_pointer(die.get(DW_AT_type), depth + 1)
        if die['tag'] in (DW_TAG_structure_type, DW_TAG_union_type):
            return any(self.has_pointer(m.get(DW_AT_type), depth + 1)
                       for m in die['children'] if m['tag'] == DW_TAG_member)
        return False


def collect(dw, keep_file=None):
    """Every named struct/union with a definition, as

        {('struct'|'union', name): {'size': n, 'file': path, 'members': [...]}}

    A member is (name, offset, bit_size_or_None, has_pointer, anonymous_path).
    Members of an anonymous struct or union are lifted into the parent, with
    their offsets added, because that is how the C code names them.
    """
    out = {}
    for die in dw.dies.values():
        if die['tag'] not in (DW_TAG_structure_type, DW_TAG_union_type):
            continue
        name = die.get(DW_AT_name)
        size = die.get(DW_AT_byte_size)
        if name is None or size is None or die.get(DW_AT_declaration):
            continue
        f = dw.decl_file(die)
        if keep_file is not None and not keep_file(f):
            continue
        kind = 'struct' if die['tag'] == DW_TAG_structure_type else 'union'
        key = (kind, name)
        members = []
        _walk_members(dw, die, 0, members)
        prev = out.get(key)
        if prev is not None and (prev['size'], prev['members']) != (size, members):
            # Two different definitions of the same tag in one build.  The
            # decompilation does have a few; recording either one silently
            # would make the assertion table depend on header order.
            prev['conflict'] = True
            continue
        out[key] = {'size': size, 'file': f, 'members': members,
                    'line': die.get(DW_AT_decl_line)}
    return out


def _walk_members(dw, die, base, out):
    for m in die['children']:
        if m['tag'] != DW_TAG_member:
            continue
        loc = m.get(DW_AT_data_member_location, 0)
        if isinstance(loc, (bytes, bytearray)):
            continue                    # a location expression; nothing here
                                        # produces one for a C struct
        name = m.get(DW_AT_name)
        bits = m.get(DW_AT_bit_size)
        mt = dw.strip_typedefs(m.get(DW_AT_type))
        if name is None and mt is not None and mt['tag'] in (
                DW_TAG_structure_type, DW_TAG_union_type):
            # An anonymous struct or union: its members are named directly
            # through the parent, so lift them and carry the offset.
            _walk_members(dw, mt, base + loc, out)
            continue
        if name is None:
            continue
        out.append((name, base + loc, bits,
                    dw.has_pointer(m.get(DW_AT_type))))
