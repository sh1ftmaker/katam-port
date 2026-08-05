#!/usr/bin/env python3
"""
gen_stubs.py -- define the functions nobody has decompiled yet.

The decomp is ~93% done.  The remaining 7% is not missing, it is just still
ARM: asm/sprite.s holds 27 functions, asm/boss_challenge_door.s another 8,
asm/code_08032E98.s around 27 more.  On the GBA those .s files are assembled
and linked alongside the C, so the game is whole.  The port cannot assemble
ARM, so every one of those functions arrives at wasm-ld as an undefined
symbol and the link dies before anything runs.

The fix is a stub per symbol -- but a stub with the *right type*.  On the GBA
a wrong prototype is a silent bug; on wasm it is a hard link error.  wasm
function types are part of the ABI, so wasm-ld refuses to match

    call_indirect (param i32 i32) (result i32)

against a definition of `() -> ()`.  A blanket `void f(void) {}` stub would
therefore trade 60-odd undefined symbols for 60-odd signature mismatches and
get us no further.

So the signature has to be recovered rather than invented, and the game
already wrote it down: the callers include a header that declares the
function, because that is how they call it at all.  This tool reads those
declarations back out of the portified tree and turns each into a definition
with an identical prototype and a body that reports itself:

    void sub_081548A8(u16 p0, s16 p1, ..., struct BgAffineReg *p7)
    {
        PortMissingFunction("sub_081548A8");
    }

The game then links and runs, and anything that reaches un-decompiled code
says so out loud instead of trapping.  As functions get decompiled upstream
they stop being undefined, drop out of the symbol list, and the stub for them
simply stops being generated.

A few undefined symbols are data, not code (an `extern const u8 gFoo[]` whose
bytes live in an .s file).  Those get a zero-filled definition instead; the
game will read zeroes, which is wrong but is not a crash.
"""

import argparse
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# type knowledge
# ---------------------------------------------------------------------------

# Types we can produce a zero value for with a plain cast.  Everything else is
# assumed to be a struct or union, which `(T)0` is not legal for.
SCALAR_KEYWORDS = {
    'int', 'char', 'short', 'long', 'float', 'double', 'unsigned', 'signed',
    '_Bool',
}
SCALAR_TYPEDEFS = {
    'u8', 'u16', 'u32', 'u64', 's8', 's16', 's32', 's64',
    'vu8', 'vu16', 'vu32', 'vs8', 'vs16', 'vs32',
    'bool8', 'bool16', 'bool32', 'size_t', 'intptr_t', 'uintptr_t',
}

# Words that can never be a parameter's *name*, used to tell `u16` (a bare
# type) from `u16 count` (a type and a name).
TYPE_WORDS = SCALAR_KEYWORDS | {
    'const', 'volatile', 'restrict', 'register', 'void', 'struct', 'union',
    'enum',
}

# Storage classes and decorations that sit in front of a return type and are
# not part of it.
RE_STORAGE = re.compile(
    r'\b(?:static|extern|inline|__inline__|__inline|NAKED|IWRAM_CODE|'
    r'EWRAM_CODE|ARM_FUNC|THUMB_FUNC|UNUSED)\b')

# If any of these turn up in the text preceding a `name(`, we are looking at a
# call site or a control-flow statement, not a declaration.
STATEMENT_WORDS = {
    'return', 'if', 'else', 'while', 'for', 'switch', 'case', 'do', 'sizeof',
    'goto', 'break', 'continue', 'typedef',
}
BAD_PREFIX_CHARS = set('=()[]&|!+-/%<>?:,."\'')

# How long a data array gets when the declaration does not say (`extern u8 x[]`).
PLACEHOLDER_LEN = 4


# ---------------------------------------------------------------------------
# lexing helpers
# ---------------------------------------------------------------------------

def strip_comments(text):
    """Blank out comments, preserving newlines so line-anchored regexes still
    see the file's real line structure."""

    def blank(m):
        s = m.group(0)
        if s[0] in '"\'':
            return s  # a string or char literal, not a comment
        return re.sub(r'[^\n]', ' ', s)

    return re.sub(r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|'
                  r'/\*.*?\*/|//[^\n]*', blank, text, flags=re.S)


def blank_preprocessor(text):
    """Blank out `#...` lines including backslash continuations.

    Macro bodies are full of calls that look exactly like declarations once
    the `#define` scrolls off the top of a regex's view."""
    out = []
    cont = False
    for line in text.split('\n'):
        if cont or line.lstrip().startswith('#'):
            cont = line.rstrip().endswith('\\')
            out.append(re.sub(r'[^\n]', ' ', line))
        else:
            out.append(line)
    return '\n'.join(out)


def prepare(text):
    return blank_preprocessor(strip_comments(text))


def match_parens(text, i):
    """Index of the `)` closing the `(` at text[i], or None."""
    depth = 0
    while i < len(text):
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None


def statement_start(text, pos):
    """Scan back from pos to the start of the statement containing it."""
    i = pos
    while i > 0 and text[i - 1] not in ';{}':
        i -= 1
    return i


# ---------------------------------------------------------------------------
# finding a declaration
# ---------------------------------------------------------------------------

class Decl:
    """One recovered declaration: either a function or a piece of data."""

    def __init__(self, kind, name, origin, ret=None, params=None, datadecl=None):
        self.kind = kind          # 'function' | 'data'
        self.name = name
        self.origin = origin      # Path the declaration was read from
        self.ret = ret            # return type, for functions
        self.params = params      # raw parameter text, for functions
        self.datadecl = datadecl  # full declaration text, for data
        self.asm_file = None      # filled in from --asm-dir, informational


def clean_return_type(prefix):
    """Turn the text in front of `name(` into a return type, or None if it is
    not one."""
    if any(c in BAD_PREFIX_CHARS for c in prefix):
        return None
    words = prefix.split()
    if any(w in STATEMENT_WORDS for w in words):
        return None
    ret = ' '.join(RE_STORAGE.sub(' ', prefix).split())
    # Tidy `struct Foo *` spacing so the emitted C reads like the original.
    ret = re.sub(r'\s+\*', ' *', ret)
    if not ret:
        return None  # implicit-int or a bare call; not worth guessing at
    return ret


def find_function(name, text, path):
    """Look for a declaration or definition of `name` as a function.

    Anchored on the exact identifier followed by `(`, so `sub_0815436C` never
    matches `sub_0815436C_helper` and a `foo(` inside `bar(foo(x))` is thrown
    out by the prefix check below."""
    for m in re.finditer(r'\b%s\b\s*\(' % re.escape(name), text):
        open_paren = text.index('(', m.end() - 1)
        close = match_parens(text, open_paren)
        if close is None:
            continue
        # A declaration ends in `;`, a definition in `{`.  A call ends in
        # anything else (`)`, `,`, `;` after an expression -- but those fail
        # the prefix test), and a function-pointer struct member has its name
        # inside parentheses, so it never reaches here with a clean prefix.
        tail = text[close + 1:].lstrip()
        if not tail or tail[0] not in ';{':
            continue
        prefix = text[statement_start(text, m.start()):m.start()]
        ret = clean_return_type(prefix)
        if ret is None:
            continue
        # Multi-line parameter lists are already handled: `params` is whatever
        # sits between the balanced parens, newlines and all.
        params = ' '.join(text[open_paren + 1:close].split())
        return Decl('function', name, path, ret=ret, params=params)
    return None


def find_data(name, text, path):
    """Look for `name` used as an object rather than a function."""
    # An `extern` declaration is the reliable form and the only one a header
    # should be using for asm-resident data.
    m = re.search(r'(?m)^[ \t]*extern\b[^;{}]*\b%s\b[^;{}]*;' % re.escape(name), text)
    if m is None:
        # Failing that, a file-scope definition (column zero, no leading
        # whitespace) in a .c file.
        m = re.search(r'(?m)^(?:const\s+|volatile\s+)?[A-Za-z_]\w*[\w \t\*]*'
                      r'\b%s\b\s*(?:\[[^\];]*\])*\s*[;=]' % re.escape(name), text)
    if m is None:
        return None
    decl = ' '.join(m.group(0).split())
    return Decl('data', name, path, datadecl=decl)


def resolve(name, groups):
    """Search each group of files in turn -- headers first, then sources."""
    for files in groups:
        for path, text in files:
            if re.search(r'\b%s\b' % re.escape(name), text) is None:
                continue  # cheap reject before the expensive scans
            decl = find_function(name, text, path)
            if decl is None:
                decl = find_data(name, text, path)
            if decl is not None:
                return decl
    return None


# ---------------------------------------------------------------------------
# rendering C
# ---------------------------------------------------------------------------

def split_params(params):
    """Split a parameter list on top-level commas."""
    out, depth, cur = [], 0, ''
    for ch in params:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


RE_UNNAMED_FNPTR = re.compile(r'\(\s*\*\s*\)')
RE_ARRAY_SUFFIX = re.compile(r'^(.*?)((?:\s*\[[^\]]*\])+)$', re.S)


def name_param(param, index):
    """C requires a *definition* to name every parameter, but headers here
    declare `void sub_081548A8(u16, s16, s16, ...)`.  Invent names for the
    ones that have none."""
    p = ' '.join(param.split())
    if p in ('', 'void', '...'):
        return p
    argname = 'p%d' % index

    if '(' in p:  # function-pointer parameter: the name lives inside `(*..)`
        if RE_UNNAMED_FNPTR.search(p):
            return RE_UNNAMED_FNPTR.sub('(*%s)' % argname, p, count=1)
        return p

    m = RE_ARRAY_SUFFIX.match(p)
    base, arr = (m.group(1).rstrip(), m.group(2).strip()) if m else (p, '')

    tokens = re.findall(r'[A-Za-z_]\w*|\*', base)
    named = (len(tokens) > 1
             and tokens[-1] != '*'
             and tokens[-1] not in TYPE_WORDS
             and tokens[-2] not in ('struct', 'union', 'enum'))
    if named:
        return p
    sep = '' if base.endswith('*') else ' '
    return '%s%s%s%s' % (base, sep, argname, arr)


def format_params(params):
    parts = split_params(params)
    if not parts or (len(parts) == 1 and parts[0] in ('', 'void')):
        return 'void'
    return ', '.join(name_param(p, i) for i, p in enumerate(parts))


def is_castable(ret, ptr_typedefs):
    """Can we write `return (ret)0;`?  True for scalars and every pointer,
    false for structs and unions, which need a zeroed local instead."""
    if '*' in ret:
        return True
    tokens = [t for t in ret.split() if t not in ('const', 'volatile')]
    if not tokens:
        return False
    if tokens[0] in ('struct', 'union', 'enum'):
        return tokens[0] == 'enum'  # an enum is an integer; a struct is not
    if any(t in SCALAR_KEYWORDS for t in tokens):
        return True
    # A single typedef name: a known scalar, or a typedef we saw declared as a
    # function pointer (casting 0 to one of those is fine).
    return len(tokens) == 1 and (tokens[0] in SCALAR_TYPEDEFS
                                 or tokens[0] in ptr_typedefs)


def load_stub_returns(path):
    """{symbol: literal} from tools/stub_returns.conf.

    A stub returning 0 is harmless when the caller ignores the result and fatal
    when the caller reads it as a state.  main.c's VRAM-transfer loop is the
    live example: it treats 0 as "this worker did not finish", stops running
    game tasks until the queue drains, and so hangs forever on a stub that
    always says 0.  This file records the cases where the port knows better."""
    out = {}
    if path and Path(path).exists():
        for line in Path(path).read_text().splitlines():
            line = line.split('#')[0].strip()
            parts = line.split()
            if len(parts) == 2:
                out[parts[0]] = parts[1]
    return out


def render_function(decl, ptr_typedefs, returns=None):
    lines = []
    if decl.asm_file:
        lines.append('/* %s */' % decl.asm_file)
    sep = '' if decl.ret.endswith('*') else ' '
    lines.append('%s%s%s(%s)' % (decl.ret, sep, decl.name,
                                 format_params(decl.params)))
    lines.append('{')
    ret = decl.ret.strip()
    if ret == 'void':
        lines.append('    PortMissingFunction("%s");' % decl.name)
    elif is_castable(ret, ptr_typedefs):
        value = (returns or {}).get(decl.name, '0')
        lines.append('    PortMissingFunction("%s");' % decl.name)
        if value != '0':
            lines.append('    /* %s, not 0 -- see tools/stub_returns.conf */'
                         % value)
        lines.append('    return (%s)%s;' % (ret, value))
    else:
        # `(struct Foo)0` is not a cast anyone can write, so zero a local.
        lines.append('    %s zero = { 0 };' % ret)
        lines.append('    PortMissingFunction("%s");' % decl.name)
        lines.append('    return zero;')
    lines.append('}')
    return '\n'.join(lines)


def render_data(decl):
    """Turn `extern const u8 gFoo[];` into a real, zero-filled definition."""
    body = decl.datadecl.rstrip(';').strip()
    body = re.sub(r'^extern\s+', '', body)
    body = re.sub(r'\s*=.*$', '', body)  # drop any initialiser we copied
    guessed = False
    if re.search(r'\[\s*\]', body):
        # Size unknown -- the real size is in the .s file we cannot read.
        body = re.sub(r'\[\s*\]', '[%d]' % PLACEHOLDER_LEN, body, count=1)
        guessed = True
    lines = []
    if decl.asm_file:
        lines.append('/* %s */' % decl.asm_file)
    if guessed:
        lines.append('/* PORT: size unknown, %d-element placeholder. */' % PLACEHOLDER_LEN)
    # `extern` because these are usually const, and a const object at
    # namespace scope has internal linkage in C++ and external linkage in C.
    # The 64-bit builds compile this file as C++ (docs/SIXTYFOUR.md), where
    # without it the stub is defined here and invisible to the game that needs
    # it.  In C an extern declaration with an initialiser is a definition with
    # external linkage, which is what it already had.
    lines.append('extern %s;' % body)
    lines.append('%s = { 0 };' % body)
    return '\n'.join(lines), guessed


# ---------------------------------------------------------------------------
# inputs
# ---------------------------------------------------------------------------

RE_UNDEF_LINE = re.compile(r'undefined symbol:\s*(\S+)')
RE_BARE_NAME = re.compile(r'^[A-Za-z_]\w*$')


def read_symbols(path):
    """Accept a bare list of names, or linker output pasted verbatim."""
    names, ignored = [], 0
    seen = set()
    for line in path.read_text(errors='replace').splitlines():
        line = line.strip()
        if not line:
            continue
        m = RE_UNDEF_LINE.search(line)
        name = m.group(1) if m else (line if RE_BARE_NAME.match(line) else None)
        if name is None:
            ignored += 1  # `>>> referenced by ...` and other linker noise
            continue
        if name not in seen:
            seen.add(name)
            names.append(name)
    return names, ignored


def collect_pointer_typedefs(files):
    """Names of function-pointer typedefs (`typedef void (*TaskMain)(void);`).

    Worth knowing because the return type `TaskMain` looks like a struct to a
    plain word test, but `(TaskMain)0` is perfectly legal."""
    out = set()
    for _, text in files:
        out.update(re.findall(r'typedef\b[^;]*?\(\s*\*\s*(\w+)\s*\)\s*\(', text))
    return out


RE_TAG = re.compile(r'\b(struct|union)\s+(\w+)')


def forward_declarations(decls):
    """`struct Foo;` for every tag a prototype mentions.

    Some prototypes come out of a .c file whose struct is declared in that .c
    and nowhere else, so including the symbol's home header is not always
    enough.  Without a file-scope tag, C scopes the one in the parameter list
    to the prototype itself -- harmless for the wasm signature, since it is a
    pointer either way, but it is a warning per stub and it is trivial to
    avoid.  A duplicate of a tag a header already declared is legal C."""
    tags = set()
    for decl in decls:
        for kw, tag in RE_TAG.findall('%s %s' % (decl.ret, decl.params)):
            tags.add((kw, tag))
    return ['%s %s;' % (kw, tag) for kw, tag in sorted(tags)]


def scan_asm(asm_dir):
    """{symbol: 'asm/sprite.s'} -- purely so each stub can say where the real
    implementation still lives."""
    out = {}
    for path in sorted(asm_dir.rglob('*.s')):
        text = path.read_text(errors='replace')
        for name in re.findall(r'(?:thumb|arm)_func_start\s+(\w+)', text):
            out.setdefault(name, 'asm/%s' % path.name)
    return out


def load(paths):
    return [(p, prepare(p.read_text(errors='replace'))) for p in paths]


# ---------------------------------------------------------------------------


# The 64-bit builds compile every source, including this generated one, through
# a C++ front end so that GBA structures keep 4-byte pointer members; see
# docs/SIXTYFOUR.md and tools/cxxify.py.  C++ would mangle these definitions and
# give the const tables internal linkage, so the file declares C linkage the way
# tools/cxxify.py does for the game's own sources.  It is a no-op in C.
EXTERN_C_OPEN = '#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
EXTERN_C_CLOSE = '\n#ifdef __cplusplus\n}\n#endif\n'

HEADER = '''\
/* Generated by tools/gen_stubs.py -- do not edit.
 *
 * One definition per symbol that only exists as ARM assembly in the decomp.
 * Each prototype was copied from the game's own declaration, because wasm-ld
 * matches function signatures and will reject a stub whose type differs from
 * the type the callers use.  Calling any of these reports itself and returns
 * zero.  Regenerate rather than editing: as functions get decompiled they
 * stop being undefined and drop out of this file by themselves.
 */

'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tree', required=True, type=Path,
                    help='portified source tree (headers in include/, C in src/)')
    ap.add_argument('--symbols', required=True, type=Path,
                    help='one undefined symbol per line, or raw wasm-ld output')
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--asm-dir', type=Path,
                    help="decomp's asm/ directory; used only to note in a "
                         'comment which .s file each symbol comes from')
    args = ap.parse_args()
    returns = load_stub_returns(Path(__file__).parent / 'stub_returns.conf')

    if not (args.tree / 'include').is_dir():
        sys.exit('error: %s has no include/ -- is it a portified tree?' % args.tree)

    names, ignored = read_symbols(args.symbols)
    headers = load(sorted((args.tree / 'include').rglob('*.h')))
    sources = load(sorted((args.tree / 'src').rglob('*.c'))
                   + sorted((args.tree / 'src').rglob('*.h')))
    ptr_typedefs = collect_pointer_typedefs(headers + sources)
    asm_origin = scan_asm(args.asm_dir) if args.asm_dir else {}

    functions, data, unresolved, guessed_sizes = [], [], [], []
    includes = set()

    for name in names:
        decl = resolve(name, (headers, sources))
        if decl is None:
            unresolved.append(name)
            continue
        decl.asm_file = asm_origin.get(name)

        # Pull in whatever header declared the symbol, so the struct types in
        # its prototype are the real ones and not fresh incomplete types
        # invented inside our parameter list.
        inc = header_include(decl.origin, args.tree)
        if inc:
            includes.add(inc)

        if decl.kind == 'function':
            functions.append(decl)
        else:
            data.append(decl)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('w') as f:
        f.write(HEADER)
        f.write('#include "global.h"\n#include "port/port.h"\n')
        for inc in sorted(includes):
            f.write('#include "%s"\n' % inc)
        f.write('\n')
        f.write(EXTERN_C_OPEN)

        if data:
            f.write('/* ---- data that lives in assembly -------------------'
                    '-------------------- */\n\n')
            for decl in data:
                text, guessed = render_data(decl)
                if guessed:
                    guessed_sizes.append(decl.name)
                f.write(text + '\n\n')

        if functions:
            fwd = forward_declarations(functions)
            if fwd:
                f.write('\n'.join(fwd) + '\n\n')
            f.write('/* ---- functions that are still ARM only -------------'
                    '-------------------- */\n\n')
            for decl in functions:
                f.write(render_function(decl, ptr_typedefs, returns) + '\n\n')

        if unresolved:
            # Deliberately not stubbed: a stub with a guessed signature would
            # link and then be wrong at run time, which is worse than a link
            # error that names the problem.
            f.write('/* UNRESOLVED -- no declaration found anywhere in the '
                    'tree, so no stub\n')
            f.write(' * could be written with a signature we trust:\n')
            for name in unresolved:
                f.write(' *     %s\n' % name)
            f.write(' */\n')

        f.write(EXTERN_C_CLOSE)

    print('gen_stubs: %d symbols requested' % len(names))
    if ignored:
        print('  %d input lines ignored (linker noise)' % ignored)
    print('  %d function stubs' % len(functions))
    print('  %d data definitions' % len(data))
    if guessed_sizes:
        print('      %d of them with a placeholder size: %s'
              % (len(guessed_sizes), ', '.join(guessed_sizes)))
    print('  %d unresolved' % len(unresolved))
    if unresolved:
        print()
        print('UNRESOLVED -- declared nowhere in %s, no stub emitted:' % args.tree)
        for name in unresolved:
            print('    %-32s %s' % (name, asm_origin.get(name, '')))
        print()
        print('  Each needs a prototype before it can be stubbed: find a')
        print('  caller, or read the .s file, and declare it in a header.')
    print('  -> %s' % args.out)
    return 0


def header_include(origin, tree):
    """Path to #include a declaration's home header by, or None for a .c."""
    incdir = tree / 'include'
    try:
        return str(origin.relative_to(incdir))
    except ValueError:
        pass
    # A declaration found in src/foo.c: the companion header, if there is one.
    companion = incdir / (origin.stem + '.h')
    if origin.suffix == '.c' and companion.exists():
        return str(companion.relative_to(incdir))
    return None


if __name__ == '__main__':
    sys.exit(main())
