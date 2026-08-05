#!/usr/bin/env python3
"""
gen_ram_symbols.py -- recreate the decomp's linker-placed RAM symbols.

189 of the game's globals are not defined in any C file.  They are addresses
assigned by linker.ld:

    . = 0x00020EE0; gKirbys = .;

and the C side only ever sees `extern struct Kirby gKirbys[];`.  The address is
not incidental.  Task storage is reached by arithmetic on the region base --

    #define TaskGetStructPtr(taskp) \\
        ((taskp)->flags & TASK_USE_EWRAM \\
        ? (void *)EWRAM_START + ((taskp)->structOffset << 2) \\
        : (void *)IWRAM_START + (taskp)->structOffset)

-- so a symbol that merely exists somewhere is not good enough; it has to sit
at the address linker.ld gave it, or the game and its own task allocator will
disagree about where objects are.

The port reserves the real GBA memory map inside the wasm linear memory
(docs/ARCHITECTURE.md), so those addresses are directly usable.  wasm-ld has no
--defsym, so instead of defining symbols this generator turns each one into a
macro over its address, and comments out the matching `extern` declaration in
the copied headers so the macro is the only definition in play.

The declared array extent is preserved, so `sizeof(gWinRegs)` and
friends keep working.
"""

import argparse
import re
import sys
from pathlib import Path

REGIONS = {'ewram': 0x02000000, 'iwram': 0x03000000}


def parse_linker_script(path):
    """Return {symbol: absolute address} for the ewram and iwram sections."""
    out = {}
    region = None
    dot = 0
    for line in path.read_text().splitlines():
        m = re.match(r'\s*(\w+)\s*\(NOLOAD\)', line)
        if m and m.group(1) in REGIONS:
            region, dot = m.group(1), 0
            continue
        if region is None:
            continue
        if re.match(r'\s*\}', line):
            region = None
            continue
        # `. = 0x1234;` advances the location counter
        m = re.search(r'\.\s*=\s*(0x[0-9A-Fa-f]+)', line)
        if m:
            dot = int(m.group(1), 16)
        # `name = .;` or `name = 0;`
        for name, value in re.findall(r'(\w+)\s*=\s*(\.|0x[0-9A-Fa-f]+|\d+)\s*;', line):
            if name in ('.',):
                continue
            addr = dot if value == '.' else int(value, 16 if value.startswith('0x') else 10)
            out[name] = REGIONS[region] + addr
    return out


# A whole-line `extern` declaration of one named symbol.  Trailing comments are
# common in these headers ("// SUGGESTION: gNumPlayers") and have to be matched
# too, or the declaration survives and collides with the macro.
DECL_RE_TMPL = (r'^[ \t]*extern\b[^\n;]*?\b%s\b[^\n;]*;'
                r'[ \t]*(?://[^\n]*|/\*(?:[^*]|\*(?!/))*\*/)?[ \t]*$')


def strip_attributes(text):
    """Remove `__attribute__((...))`, counting parentheses.

    A regex cannot do this: `__attribute__((aligned(4)))` has a nested pair,
    and a non-greedy match stops at the wrong one and leaves the macro
    unbalanced.
    """
    while True:
        i = text.find('__attribute__')
        if i < 0:
            return text
        j = text.find('(', i)
        if j < 0:
            return text[:i] + text[i + len('__attribute__'):]
        depth = 0
        for k in range(j, len(text)):
            if text[k] == '(':
                depth += 1
            elif text[k] == ')':
                depth -= 1
                if depth == 0:
                    text = text[:i] + text[k + 1:]
                    break
        else:
            return text[:i]


def select_declarator(body, name):
    """Reduce a multi-declarator declaration to the one declaring `name`.

    These headers declare several symbols at once:

        extern const struct AnimInfo *const gUnk_08D60FB4[], *const gUnk_08D60FDC[];

    Treating everything before the name as its type would swallow the other
    declarator, so split at top-level commas and re-attach the base type.
    """
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(body):
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        elif ch == ',' and depth == 0:
            parts.append(body[start:i])
            start = i + 1
    parts.append(body[start:])

    if len(parts) == 1:
        return body

    # The base type is whatever precedes the first declarator; a declarator
    # starts at the run of `*` / `const` immediately before its identifier.
    first = parts[0]
    m = re.search(r'\b\w+\b(?=\s*(?:\[|$))', first.strip())
    if not m:
        return None
    head = first[:first.rindex(m.group(0))]
    head = re.sub(r'(?:\s|\*|\bconst\b)+$', '', head).strip()

    for part in parts:
        if re.search(r'\b%s\b' % re.escape(name), part):
            return part if part is parts[0] else '%s %s' % (head, part.strip())
    return None


def declaration_to_macro(decl, name, addr):
    """Turn `extern u16 gWinRegs[6];` into `#define gWinRegs (*(u16 (*)[6])0x...)`.

    The result is a macro and nothing else -- deliberately.  These headers are
    force-included ahead of every translation unit, long before the game's own
    headers have defined `struct LevelInfo` and friends, so anything that named
    a type at that point would fail to parse.  A macro body is only parsed
    where it expands, by which time the type is complete.

    Keeping the array extent (rather than decaying everything to a pointer)
    is what keeps `sizeof(gBgScrollRegs)` returning the size the game expects;
    it is used as a DMA length in GameInit.
    """
    # These headers annotate declarations heavily ("// SUGGESTION: gNumPlayers",
    # "// never read"), and the comment sits after the semicolon.  Drop it
    # before anything tries to read the declaration as a type.
    body = re.sub(r'//[^\n]*', '', decl)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S).strip()
    body = re.sub(r'^extern\s+', '', body).rstrip().rstrip(';').rstrip()
    # Storage alignment is irrelevant to an address macro.
    body = strip_attributes(body).strip()

    body = select_declarator(body, name)
    if body is None:
        return None

    m = re.search(r'\b%s\b' % re.escape(name), body)
    if not m:
        return None
    ctype, suffix = body[:m.start()].strip(), body[m.end():].strip()

    # A function-pointer declarator wraps the name in parentheses --
    # `s32 (*const gTable[])(union AnimCmd, struct Sprite *)` -- so splitting at
    # the name leaves an unbalanced prefix that no cast can be built from.
    # Reject it here; the caller has a better answer for these.
    if ctype.count('(') != ctype.count(')'):
        return None

    if suffix.startswith('[]'):
        # Incomplete array: decay to a pointer to the element type, which is
        # all the game can do with it anyway.
        rest = suffix[2:].strip()
        if rest:
            return '#define %s ((%s (*)%s)0x%08X)' % (name, ctype, rest, addr)
        # An array whose *elements* are pointers needs those narrowed too:
        # `const struct TiledBg_082D7850 *const gUnk_082D7850[]` is 34 such
        # tables in ROM, and on a 64-bit host indexing one strides eight bytes
        # through data the console laid out in four.  Nothing asserts this --
        # it is a naked address, not a structure -- so the symptom is a read
        # from the wrong element, which for gUnk_082D7850 was a segfault in the
        # title logo.  PTR32 is a plain pointer in C, so the ILP32 builds get
        # the identical macro.
        mp = re.match(r'^(?P<inner>.*?)\s*\*\s*(?P<qual>const|volatile)?$', ctype)
        if mp and mp.group('inner'):
            return '#define %s ((PTR32(%s) %s*)0x%08X)' % (
                name, mp.group('inner').strip(),
                (mp.group('qual') + ' ') if mp.group('qual') else '', addr)
        return '#define %s ((%s *)0x%08X)' % (name, ctype, addr)

    if suffix.startswith('['):
        return '#define %s (*(%s (*)%s)0x%08X)' % (name, ctype, suffix, addr)

    if suffix:
        return None  # a function declaration, bitfield, or something unexpected

    return '#define %s (*(%s *)0x%08X)' % (name, ctype, addr)


# Symbols linker.ld places with `. = ALIGN(n);` immediately after an object
# file's section, rather than at a literal offset.  This script follows the
# location counter through `. = 0x...` assignments only, and has no way to know
# how long `src/m4a.o(common_data)` is -- so it carries the last literal
# forward and gives the symbol the *start* of that section.
#
# That is how gIntrTable came out as 0x03000560, which is gSoundInfo's address.
# It cost nothing for as long as the port dropped src/m4a.c and nothing ever
# wrote there.  The moment the sound engine was restored, SoundInit's CpuFill32
# zeroed the interrupt table and the first VBlank dispatched through a garbage
# entry -- a wasm trap with no obvious connection to audio at all.
#
# The address is read out of katam.map, which is the same build linker.ld
# describes.  The better fix is to teach the parser about section sizes; this
# is the only symbol in the game that needs it today.
ADDR_OVERRIDES = {
    'gIntrTable': 0x030017B0,
}


def check_overrides_against_map(map_path, report):
    """Fail loudly if a hardcoded override no longer matches the link map.

    A hardcoded address that quietly goes stale is exactly how this symbol
    caused trouble in the first place: the wrong value cost nothing for weeks,
    then took the game down in a way that pointed nowhere near the cause.  The
    map is the authority and it is already in the tree, so there is no reason
    to let the two drift in silence.
    """
    if not map_path or not Path(map_path).exists():
        return
    text = Path(map_path).read_text(errors='replace')
    for name, addr in ADDR_OVERRIDES.items():
        m = re.search(r'^\s*0x([0-9a-fA-F]+)\s+%s\s*=' % re.escape(name),
                      text, re.M)
        if not m:
            report.append('%s: not found in the link map -- the override '
                          'cannot be checked' % name)
            continue
        actual = int(m.group(1), 16)
        if actual != addr:
            sys.exit('gen_ram_symbols: %s is 0x%08X in %s but the override '
                     'says 0x%08X.\n'
                     '  Update ADDR_OVERRIDES.  Leaving this wrong puts the '
                     'symbol on top of another one,\n'
                     '  which fails far away from here and looks like '
                     'anything but an address problem.'
                     % (name, actual, map_path, addr))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--linker-script', required=True, type=Path)
    ap.add_argument('--tree', required=True, type=Path,
                    help='portified source tree (its headers are edited in place)')
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--map', type=Path,
                    help='katam.map, used to verify ADDR_OVERRIDES still hold')
    args = ap.parse_args()

    syms = parse_linker_script(args.linker_script)
    override_notes = []
    check_overrides_against_map(args.map, override_notes)
    for note in override_notes:
        print('  NOTE %s' % note)
    syms.update(ADDR_OVERRIDES)
    # .c files re-declare these symbols too, and a declaration left
    # standing would expand the macro into a declarator.
    headers = sorted(list((args.tree / 'include').rglob('*.h'))
                     + list((args.tree / 'src').rglob('*.c'))
                     + list((args.tree / 'src').rglob('*.h')))

    resolved, skipped, unreferenced = [], [], []

    # Read every file once.  With ~190 symbols across ~350 files, re-reading
    # per symbol turns a two-second job into a several-minute one.
    text_of = {hp: hp.read_text(errors='replace') for hp in headers}
    dirty = set()

    done = set()
    for name in sorted(syms):
        if name in done:
            continue
        addr = syms[name]
        pattern = re.compile(DECL_RE_TMPL % re.escape(name), re.M)
        hit = None
        for hp in headers:
            m = pattern.search(text_of[hp])
            if m:
                hit = m.group(0)
                break
        if hit is None:
            unreferenced.append(name)
            continue

        # One declaration can declare several of these symbols at once.  All of
        # them have to be resolved here, because commenting the line out for the
        # first would hide the declaration the others still need.
        group = [n for n in syms if n != name and re.search(r'\b%s\b' % re.escape(n), hit)]
        for other in group:
            macro = declaration_to_macro(hit, other, syms[other])
            if macro is None:
                skipped.append((other, hit.strip()))
            else:
                resolved.append((other, syms[other], macro))
            done.add(other)

        macro = declaration_to_macro(hit, name, addr)
        if macro is None:
            skipped.append((name, hit.strip()))
            continue
        resolved.append((name, addr, macro))

        # Comment out every declaration of this symbol, everywhere.

    # One removal pass at the end, over every symbol that got a macro.
    # Doing this inline while resolving missed any symbol that was resolved as
    # part of another declaration's group: its own declaration was never
    # visited, survived, and then expanded the macro into a declarator.
    for name in sorted({n for n, _, _ in resolved}):
        pattern = re.compile(DECL_RE_TMPL % re.escape(name), re.M)
        for hp in headers:
            new = pattern.sub(
                lambda m: '/* PORT: replaced by port/ram_symbols.h -- %s */'
                          % m.group(0).strip(),
                text_of[hp])
            if new != text_of[hp]:
                text_of[hp] = new
                dirty.add(hp)

    for hp in dirty:
        hp.write_text(text_of[hp])

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('w') as f:
        f.write('/* Generated by tools/gen_ram_symbols.py -- do not edit. */\n')
        f.write('#ifndef GUARD_PORT_RAM_SYMBOLS_H\n#define GUARD_PORT_RAM_SYMBOLS_H\n\n')
        f.write('#include "global.h"\n\n')
        for name, addr, define in resolved:
            f.write('%s\n' % define)
        f.write('#endif\n')

    print('gen_ram_symbols: %d symbols in linker.ld' % len(syms))
    print('  %d resolved to address macros' % len(resolved))
    print('  %d declared in no header (asm-only, ignored)' % len(unreferenced))
    if skipped:
        print('  %d SKIPPED -- declaration shape not understood:' % len(skipped))
        for name, decl in skipped:
            print('      %-24s %s' % (name, decl[:70]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
