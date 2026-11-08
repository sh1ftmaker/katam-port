#!/usr/bin/env python3
"""unbake_incbin.py -- take the game assets back out of a portified tree.

portify.py expands every INCBIN_xx("graphics/...") in the decompilation into a
C initialiser holding the asset's bytes, read out of the decomp checkout (where
they were extracted from a ROM).  That was harmless while the tree lived in
build/, which is never committed.  The fork commits the tree, so before it does
every one of those arrays is replaced by a reference to the same bytes in the
ROM image the player supplies at run time -- the same treatment
gen_rom_data.py gives every other ROM data label:

    static const u32 sFoo[] = INCBIN_U32("graphics/foo.4bpp.lz");
 -> #define sFoo (*(const u32 (*)[N])0x08ABCDEF)

The array type, and so sizeof() and ARRAY_COUNT(), are kept.  A file-scope
static gets its macro in place in the .c file; a global is moved to
port/rom_data.h, which every translation unit force-includes, and its extern
declaration in a header is commented out the way gen_rom_data.py does it.

Addresses come from katam.elf's symbol table (it names file-local objects;
katam.map does not), and every one is *verified*: the asset file's bytes must
equal the ROM's bytes at that address, or the script refuses.  Needs the decomp
checkout, its katam.elf, arm-none-eabi-nm and a ROM -- run once, at fork time.

    python3 tools/legacy-sync/unbake_incbin.py --decomp D --rom R \
        --tree game --rom-data game/generated/port/rom_data.h
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

RE_DEF = re.compile(
    r'^(?P<static>static\s+)?const\s+(?P<type>[us](?:8|16|32))\s+(?P<name>\w+)\s*'
    r'\[(?P<size>[^\]]*)\]\s*(?:UNUSED\s*|__attribute__\(\(aligned\(4\)\)\)\s*)?'
    r'=\s*INCBIN_(?P<kind>[US](?:8|16|32))\(\s*"(?P<path>[^"]+)"\s*\)\s*;', re.M)


def elf_symbols(elf):
    out = subprocess.run(['arm-none-eabi-nm', '-S', str(elf)], check=True,
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, _, name = parts
            syms.setdefault(name, []).append((int(addr, 16), int(size, 16)))
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decomp', required=True, type=Path)
    ap.add_argument('--rom', required=True, type=Path)
    ap.add_argument('--tree', required=True, type=Path)
    ap.add_argument('--rom-data', required=True, type=Path)
    a = ap.parse_args()

    rom = a.rom.read_bytes()
    syms = elf_symbols(a.decomp / 'katam.elf')
    globals_out = []
    total = 0
    count = 0
    for src in sorted((a.decomp / 'src').rglob('*.c')):
        rel = src.relative_to(a.decomp)
        defs = list(RE_DEF.finditer(src.read_text()))
        if not defs:
            continue
        dst = a.tree / rel
        text = dst.read_text()
        for d in defs:
            name, ctype, width = d['name'], d['type'], int(d['kind'][1:]) // 8
            data = (a.decomp / d['path']).read_bytes()
            cands = [s for s in syms.get(name, ()) if 0x08000000 <= s[0] < 0x0A000000]
            if len(cands) != 1:
                sys.exit('%s: %s has %d ROM symbols' % (rel, name, len(cands)))
            addr, symsize = cands[0]
            off = addr - 0x08000000
            if rom[off:off + len(data)] != data:
                sys.exit('%s: %s: ROM bytes at %#x differ from %s'
                         % (rel, name, addr, d['path']))
            if d['size'].strip():
                n = int(d['size'], 0)
            else:
                n = (len(data) + width - 1) // width
            if symsize and symsize != n * width:
                sys.exit('%s: %s: symbol size %d, array %d' % (rel, name, symsize, n * width))
            macro = ('#define %s (*(const %s (*)[%#x])0x%08X)'
                     % (name, ctype, n, addr))
            what = 'INCBIN_%s("%s"), %d bytes, read from the ROM' % (
                d['kind'], d['path'], len(data))
            note = ' /* %s */' % what
            # The portified definition: the same line, with the bytes pasted in.
            pat = re.compile(r'^[^\n]*\b%s\s*\[[^\n]*=\s*\{[^\n]*\};[^\n]*$' % re.escape(name), re.M)
            hits = pat.findall(text)
            if len(hits) != 1:
                sys.exit('%s: %s: %d expanded definitions in the tree' % (rel, name, len(hits)))
            if d['static']:
                text = pat.sub(lambda m: macro + note, text)
            else:
                text = pat.sub(lambda m: '/* PORT-FORK: %s -- moved to port/rom_data.h, %s */' % (name, what), text)
                globals_out.append((name, macro + note))
            total += len(data)
            count += 1
        dst.write_text(text)

    # Globals: the macro goes where every translation unit sees it first, and
    # the header declarations that would now expand into nonsense go.
    if globals_out:
        names = {n for n, _ in globals_out}
        files = sorted((a.tree / 'include').rglob('*.h')) + sorted((a.tree / 'src').rglob('*.c'))
        for h in files:
            t = h.read_text()
            new = re.sub(r'^([ \t]*extern\s+(?:const\s+)?[us](?:8|16|32)\s+(?:const\s+)?(\w+)\s*\[[^\]\n]*\]\s*;)',
                         lambda m: ('/* PORT: replaced by port/rom_data.h -- %s */' % m.group(1).strip())
                         if m.group(2) in names else m.group(0), t, flags=re.M)
            if new != t:
                h.write_text(new)
        rd = a.rom_data.read_text()
        block = ['', '/* Assets the decompilation INCBINs; see tools/legacy-sync/unbake_incbin.py. */']
        block += [m for _, m in sorted(globals_out)]
        rd = rd.replace('\n#endif', '\n'.join(block) + '\n\n#endif', 1) if rd.rstrip().endswith('#endif') else None
        if rd is None:
            sys.exit('rom_data.h does not end in #endif')
        # replace only the last #endif
        a.rom_data.write_text(rd)
    print('%d INCBIN arrays (%d bytes) now read from the ROM, %d of them global'
          % (count, total, len(globals_out)))


if __name__ == '__main__':
    main()
