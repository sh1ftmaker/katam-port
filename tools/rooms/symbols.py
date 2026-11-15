"""symbols.py -- name a GBA address down to the struct field, for the rooms
measurement (tools/rooms/measure.mjs, classify.py).

Fixed RAM symbols come from game/generated/port/ram_symbols.h (via
tools/census.py); struct layouts from gdb's `ptype /o` on the native build
(the game's structs live in the GBA map, so their layout is the console's on
every build -- platform/port/gba_layout.h asserts it); heap memory is named
by the task that owns it (task table walk at the first difference) and, for
object tasks, by struct Object's fields."""
import json
import os
import re
import subprocess
import sys
import bisect

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import census  # noqa: E402

LEAF = re.compile(r'^/\*\s+(\d+)(?::\s*\d+)?\s*\|\s*(\d+)\s*\*/\s*(.*?);\s*$')
OPEN = re.compile(r'^/\*\s+(\d+)\s*\|\s*(\d+)\s*\*/\s*(?:struct|union)\b.*\{\s*$')
CLOSE = re.compile(r'^\}\s*([\w\[\]]*)\s*;')
TOTAL = re.compile(r'total size \(bytes\):\s*(\d+)')


class Node:
    def __init__(self, name, off, size):
        self.name, self.off, self.size, self.kids, self.elem = name, off, size, [], size


def parse_ptype(text):
    root = None
    stack = []
    for raw in text.splitlines():
        ln = raw.strip()
        if not ln:
            continue
        if root is None:
            m = re.search(r'type = (?:struct|union) \w+ \{', ln)
            if m:
                root = Node('', 0, 0)
                stack = [root]
            continue
        m = OPEN.match(ln)
        if m:
            n = Node('?', int(m.group(1)), int(m.group(2)))
            stack[-1].kids.append(n)
            stack.append(n)
            continue
        m = TOTAL.search(ln)
        if m:
            stack[-1].elem = int(m.group(1))
            if stack[-1] is root:
                root.size = root.elem
            continue
        m = CLOSE.match(ln)
        if m and len(stack) > 1:
            n = stack.pop()
            n.name = m.group(1).split('[')[0] or '(anon)'
            continue
        m = LEAF.match(ln)
        if m:
            decl = m.group(3)
            name = re.split(r'\s+', decl.replace('*', ' '))[-1].split('[')[0].strip(':')
            n = Node(name, int(m.group(1)), int(m.group(2)))
            elem = re.findall(r'\[(\d+)\]', decl)
            if elem:
                k = 1
                for e in elem:
                    k *= int(e)
                n.elem = max(1, n.size // max(1, k))
            stack[-1].kids.append(n)
    return root


def field_of(node, off):
    """Dotted field path of byte `off` within a struct laid out as `node`."""
    best = None
    for k in node.kids:
        if k.off <= off < k.off + k.size:
            best = k
            break
    if best is None:
        return '+%#x' % off
    rel = off - best.off
    idx = ''
    if best.elem and best.elem < best.size:
        i = rel // best.elem
        idx = '[%d]' % i
        rel -= i * best.elem
    if best.kids:
        sub = field_of(best, best.off + rel)
        return best.name + idx + ('.' + sub if not sub.startswith('+') else sub)
    return best.name + idx + ('' if rel == 0 else '+%d' % rel)


class Namer:
    def __init__(self, binary, cache):
        self.binary = binary
        self.cache_path = cache
        self.layouts = {}
        if os.path.exists(cache):
            self.layouts = json.load(open(cache))
        self.nodes = {}
        self.syms = []
        for ln in open(os.path.join(ROOT, 'game', 'generated', 'port', 'ram_symbols.h')):
            m = re.match(r'#define (\w+) \((.*)\)(0x[0-9A-Fa-f]+)\)?\s*$', ln.strip())
            if m:
                ty = re.search(r'(struct|union) (\w+)', m.group(2))
                if 'PTR32' in m.group(2) and '(*' not in m.group(2).split('PTR32')[0]:
                    ty = None          # an array of 32-bit pointers, not of structs
                self.syms.append((int(m.group(3), 16), m.group(1), ty.group(2) if ty else None))
        sized = {s[2]: (s[0], s[1]) for s in census.ram_symbols()}
        ptr = set()
        for ln in open(os.path.join(ROOT, 'game', 'generated', 'port', 'ram_symbols.h')):
            m = re.match(r'#define (\w+) \(\((.*)\*\)(0x[0-9A-Fa-f]+)\)\s*$', ln.strip())
            if m:
                ptr.add(m.group(1))
        rs = sorted(census.ram_symbols())
        self.ranges = []
        for i, (lo, hi, name) in enumerate(rs):
            ty = next((t for a, n, t in self.syms if n == name), None)
            if name in ptr and i + 1 < len(rs):
                hi = rs[i + 1][0]          # a pointer define: an array up to the next symbol
            self.ranges.append((lo, hi, name, ty))
        self.ranges.sort()
        self.starts = [r[0] for r in self.ranges]
        self.fns = census.fn_names(None)

    def layout(self, ty):
        if ty in self.nodes:
            return self.nodes[ty]
        if ty not in self.layouts:
            r = subprocess.run(['gdb', '-batch', '-ex', 'ptype /o struct %s' % ty, self.binary],
                               capture_output=True, text=True, timeout=300)
            self.layouts[ty] = r.stdout
            json.dump(self.layouts, open(self.cache_path, 'w'))
        n = parse_ptype(self.layouts[ty]) if self.layouts[ty] else None
        self.nodes[ty] = n
        return n

    def name(self, region, addr, owner=None):
        """(symbol, field) for one address; owner = (main id, base) or None."""
        if region == 'simblk':
            off = addr - 0x02040000
            n = self.layout('PortSimBlock')
            return 'gPortSim', field_of(n, off) if n else '+%#x' % off
        if region not in ('ewram', 'iwram'):
            return region, '%#x' % addr
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            lo, hi, nm, ty = self.ranges[i]
            if lo <= addr < hi and nm not in ('gEwramHeap', 'gIwramHeap'):
                if ty:
                    n = self.layout(ty)
                    if n and n.size:
                        el = n.size
                        k = (addr - lo) // el
                        f = field_of(n, (addr - lo) % el)
                        return nm, ('[%d].' % k if hi - lo > el else '') + f
                return nm, '+%#x' % (addr - lo)
        if owner and owner[0]:
            main, base = owner
            fn = self.fns.get(main, '%08X' % main)
            off = addr - base
            if fn in ('ObjectMain',) or fn.startswith('Object'):
                n = self.layout('Object')
                return 'task ' + fn, field_of(n, off) if n else '+%#x' % off
            return 'task ' + fn, '+%#x' % off
        if i >= 0:
            lo, hi, nm, ty = self.ranges[i]
            return nm + '?', '+%#x' % (addr - lo)
        return region, '%#x' % addr
