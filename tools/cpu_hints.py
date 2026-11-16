#!/usr/bin/env python3
"""
cpu_hints.py -- decode the per-room hints the game ships for its CPU Kirbys.

    tools/cpu_hints.py ROM                      # survey: counts, coverage, findings
    tools/cpu_hints.py ROM --room 204           # one room in detail
    tools/cpu_hints.py ROM --json OUT.json      # everything, machine-readable
    tools/cpu_hints.py ROM --groups             # door groups of every room

Reads the user's ROM at run time and prints; nothing ROM-derived belongs in
the repository (write --json output to a scratch directory).  The formats are
described in docs/design/CPU-HINTS.md; the brain that reads them in
docs/naming/cpu-brain.md.

What it decodes, per room (gRoomProps[room].unk22 is the room's content
index, the same for every per-room table):

  gUnk_08D63C28[idx]   the nav graph + scripts (struct LevelInfo_1E8, 0x1C)
      +0x00 scripts[4]   u16 bytecode, one per ROM player row (CPU_OP_*)
      +0x10 nodeMap      RL-compressed, one node index per 16x16 cell
      +0x14 nodes        struct LevelInfo_1E8_14 (0x34 each):
                           +0x00 nextHop[32]  next node toward dest, 0xFF = none
                           +0x20 x, +0x22 y   pixels
                           +0x24 radius       pixels
                           +0x26 u16 0        (zero in every node)
                           +0x28 self         own index
                           +0x29 count        neighbours (1..7)
                           +0x2A [8]          neighbour nodes, 0xFF padded
                           +0x32 u16 0        (zero in every node)
      +0x18 numNodes
  gSolidityMaps[idx]   collision map (RL) + cell records; tag 3 = a door
                       (cell x, y -> dest room, dest cell x, y)
  gLevelObjLists[idx]  objects; OBJ_CPU_KIRBY_TRIGGER (0x7F) raises the
                       room-state slot 0xF that CPU_OP_IF_ROOM_STATE reads
  gUnk_08D6CD0C[room]  the brain's room table: area, flags, and a list of
                       (room, hops, ?, next room) used by the off-screen hops
  gUnk_08D6DCAC[area]  area exits: (room in area, room outside) pairs

Door groups.  A door cell's nav node is nodeMap[cell]; door B is reachable
from door A when nextHop[node(A)][node(B)] != 0xFF.  The tool also floods the
collision map (cells a floating Kirby can pass: not solid, not mini-only;
breakable cells passable or not) and compares the two.
"""

import argparse
import collections
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import room_export as rx  # noqa: E402

G_NAV = 0x08D63C28
G_ROOM_TABLE = 0x08D6CD0C
G_AREA_EXITS = 0x08D6DCAC
NO = 0xFF

OP = {0: 'nop', 1: 'wait', 2: 'goto', 3: 'ability', 4: 'face', 5: 'inhale',
      6: 'slide', 7: 'halt', 8: 'skip', 9: 'if_state', 0xA: 'roam',
      0xFFFF: 'end'}
OP_LEN = {0: 1, 1: 2, 2: 2, 3: 2, 4: 2, 5: 2, 6: 1, 7: 1, 8: 2, 9: 4, 0xA: 1,
          0xFFFF: 1}
CMP = ['==', '!=', '>', '<', '>=', '<=']
ABILITY = ['normal', 'fire', 'ice', 'burning', 'wheel', 'parasol', 'cutter',
           'beam', 'stone', 'bomb', 'throw', 'sleep', 'cook', 'laser', 'ufo',
           'spark', 'tornado', 'hammer', 'sword', 'cupid', 'fighter', 'magic',
           'smash', 'mini', 'crash', 'missile', 'master']

# collision attribute bits (docs/naming/metatile-attributes.md)
A_EMPTY, A_WATER, A_TOP, A_LADDER, A_SOLID = 0x1, 0x2, 0x4, 0x100, 0x200
A_MINI, A_BREAK, A_HAZARD, A_DOOR = 0x400, 0x1000, 0x2000, 0x4000
A_CHAIN = 0x40   # destroyed by a chain explosion (a fuse / bomb block sets it off)

OBJ_NAMES = {0x63: '1up', 0x65: 'shard', 0x6F: 'door', 0x71: 'sliding_door',
             0x72: 'boss_door', 0x78: 'fuse', 0x79: 'star_stone_block',
             0x7D: 'big_star_stone', 0x7E: 'big_star_stone', 0x7F: 'cpu_trigger',
             0x80: 'chest', 0x81: 'big_chest', 0x88: 'big_star_stone',
             0x89: 'big_star_stone', 0x8C: 'hub_mirror', 0x8D: 'hammer_peg',
             0x91: 'triggerable_wall', 0x97: 'door_to_hub', 0x99: 'area_door'}


def attrs(rom):
    return [rom.u32(rx.G_METATILE_ATTRS + 4 * i) for i in range(256)]


def nav_entry(rom, idx):
    e = rom.ptr(G_NAV + 4 * idx)
    scripts = [rom.u32(e + 4 * i) for i in range(4)]
    node_map_addr = rom.u32(e + 0x10)
    nodes_addr = rom.u32(e + 0x14)
    n = rom.u8(e + 0x18)
    nodes = []
    for i in range(n):
        a = nodes_addr + 0x34 * i
        hop = [rom.u8(a + j) for j in range(32)]
        cnt = rom.u8(a + 0x29)
        nb = [rom.u8(a + 0x2A + j) for j in range(min(cnt, 8))]
        nodes.append({'hop': hop[:n], 'x': rom.u16(a + 0x20), 'y': rom.u16(a + 0x22),
                      'r': rom.u16(a + 0x24), 'self': rom.u8(a + 0x28), 'nb': nb})
    nm, _, _ = rx.decompress(rom, node_map_addr)
    return {'scripts': scripts, 'nodes': nodes, 'nodeMap': list(nm)}


def disasm(rom, addr, limit=96):
    """-> list of (pc, op, args).  Follows the stream linearly up to the last
    end/halt that no branch jumps over."""
    out, pc, far = [], 0, 0
    while pc < limit:
        op = rom.u16(addr + 2 * pc)
        if op not in OP:
            out.append((pc, 'bad', [op]))
            break
        n = OP_LEN[op]
        args = [rom.u16(addr + 2 * (pc + i)) for i in range(1, n)]
        out.append((pc, OP[op], args))
        if op == 8:                      # u16 pc arithmetic: wraps backwards
            far = max(far, (pc + 2 + args[0]) & 0xFFFF if args[0] < 0x8000 else 0)
        if op == 9:
            far = max(far, pc + 4 + args[2])
        pc += n
        if op in (7, 0xFFFF) and pc > far:
            break
    return out


def fmt_op(op, args):
    if op == 'ability':
        a = args[0]
        return 'ability %s' % ('drop' if a == 0xFF else ABILITY[a] if a < len(ABILITY) else a)
    if op == 'if_state':
        return 'if state %s %d skip %d' % (CMP[args[0]] if args[0] < 6 else args[0], args[1], args[2])
    if op == 'face':
        return 'face %s' % ('left' if args[0] == 1 else 'right')
    if op == 'skip':
        return 'skip %d' % (args[0] - 0x10000 if args[0] >= 0x8000 else args[0])
    return ' '.join([op] + [str(a) for a in args])


def door_records(rom, solidity_idx):
    entry = rom.ptr(rx.G_SOLIDITY_MAPS + solidity_idx * 4)
    addr = rom.u32(entry + 4)
    out = []
    if not rx.ROM_BASE <= addr < rx.ROM_BASE + len(rom.data):
        return out
    while True:
        tag, stride = rom.u8(addr), rom.u16(addr + 4)
        if stride == 0 or tag == 0:
            break
        if tag == 3:
            out.append({'x': rom.u8(addr + 2), 'y': rom.u8(addr + 3), 'col': rom.u8(addr + 1),
                        'dest': rom.u16(addr + 8), 'dx': rom.u8(addr + 0xA), 'dy': rom.u8(addr + 0xB)})
        addr += stride
    return out


def room_table(rom, room):
    a = rom.u32(G_ROOM_TABLE + 4 * room)
    if not rx.ROM_BASE <= a < rx.ROM_BASE + len(rom.data):
        return None
    lst, n = rom.u32(a), rom.u8(a + 0x48)
    ent = []
    for i in range(n):
        p = rom.u32(lst + 4 * i)
        ent.append([rom.u16(p + 2 * j) for j in range(4)])
    return {'area': rom.u8(a + 0x46), 'flags': rom.u8(a + 0x47), 'roomId': rom.u16(a + 0x44),
            'routes': ent}


def passable(a, tier):
    """game/src/cpu_ai_modes.c Passable(): a cell a floating Kirby passes,
    breaking blocks up to `tier` (0 nothing, 1 level <= 1, 2 <= 2, 3 <= 3)."""
    if a & A_MINI:
        return False
    if a & A_BREAK:
        return tier > 0 and ((a >> 20) & 0xF) <= tier
    if (a & (A_SOLID | A_CHAIN)) == (A_SOLID | A_CHAIN):
        return tier >= 1
    return not a & A_SOLID


def flood(cells, w, h, at, start, tier):
    """Cells a floating Kirby reaches from `start` (game/src/cpu_ai_modes.c
    Flood()): 4-neighbour moves into passable cells (top-solid platforms
    count as passable: Kirby flies up through them, and drops or walks round
    them).  A chain block opens only once the blast reaches it: from a chain
    trigger (0x1020) the Kirby breaks, through chain cells (0x40)."""
    seen = bytearray(w * h)
    ign = bytearray(w * h)
    q = collections.deque()
    for s in start:
        if 0 <= s < w * h:
            seen[s] = 1
            q.append(s)
    while q:
        i = q.popleft()
        x, y = i % w, i // w
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if not (0 <= nx < w and 0 <= ny < h):
                continue
            j = ny * w + nx
            a = at[cells[j]]
            if a & 0x1240 == 0x240:
                if tier < 1 or not ign[i]:
                    continue
                g = 1
            else:
                if not passable(a, tier):
                    continue
                g = 1 if (a & 0x1020) == 0x1020 or (ign[i] and a & 0x40) else 0
            if not seen[j]:
                seen[j] = 1
                ign[j] = g
                q.append(j)
            elif g and not ign[j]:
                ign[j] = 1
                q.append(j)
    return seen


def groups_of(items, reach):
    """Partition `items` (0..n-1) by mutual reachability (strongly connected)."""
    n = len(items)
    gid = [-1] * n
    g = 0
    for i in range(n):
        if gid[i] >= 0:
            continue
        gid[i] = g
        for j in range(i + 1, n):
            if gid[j] < 0 and reach(i, j) and reach(j, i):
                gid[j] = g
        g += 1
    return gid


def decode_room(rom, room, at):
    r = rx.resolve(rom, room)
    p = r['props']
    w, h = r['collision']['widthCells'], r['collision']['heightCells']
    cells = r['collision']['cells']
    nav = nav_entry(rom, p['unk22'])
    nm = nav['nodeMap']
    n = len(nav['nodes'])
    doors = door_records(rom, p['solidityMapIdx'])
    for d in doors:
        i = d['y'] * w + d['x']
        d['node'] = nm[i] if i < len(nm) else NO
    objs = []
    for o in r['objects']['objects']:
        t = o['type']
        rec = {'type': t, 'name': OBJ_NAMES.get(t, '0x%02X' % t), 'x': o['x'], 'y': o['y']}
        if t == 0x7F:
            mode = 'kirby' if not (o['unk22'] & 1) and o['unk22'] & 6 else 'block'
            rec.update({'raise': o['unk11'], 'mode': mode,
                        'who': ('people' if o['unk22'] & 2 else '') + ('+' if o['unk22'] & 6 == 6 else '')
                               + ('cpus' if o['unk22'] & 4 else ''),
                        'rect': [o['x'] - 8 + o['unk1A'], o['y'] - 8 + o['unk1C'], o['unk1E'], o['unk20']]})
        objs.append(rec)
    scripts = []
    for s in nav['scripts']:
        scripts.append({'addr': s, 'ops': [(pc, op, a) for pc, op, a in disasm(rom, s)]})

    # nav-graph reachability between nodes (directed)
    def nreach(a, b):
        return a < n and b < n and nav['nodes'][a]['hop'][b] != NO
    asym = sum(1 for a in range(n) for b in range(n) if nreach(a, b) != nreach(b, a))
    unreach = sum(1 for a in range(n) for b in range(n) if not nreach(a, b))
    # door groups: nav graph
    nav_gid = groups_of(doors, lambda i, j: nreach(doors[i]['node'], doors[j]['node']))
    # door groups: flood fill (no breaking / break anything)
    floods = {}
    for tier, f in (("solid", 0), ("break", 1), ("hammer", 3)):
        reach = []
        for d in doors:
            reach.append(flood(cells, w, h, at, [d['y'] * w + d['x']], f))
        g = groups_of(doors, lambda i, j, reach=reach: reach[i][doors[j]['y'] * w + doors[j]['x']])
        for i, d in enumerate(doors):           # a door under a block: not usable at this tier
            if not passable(at[cells[d['y'] * w + d['x']]], f):
                g[i] = -1
        floods[tier] = g
    breakables = collections.Counter((at[c] >> 20) & 0xF for c in cells if at[c] & A_BREAK)
    return {'room': room, 'idx': p['unk22'], 'w': w, 'h': h, 'nodes': nav['nodes'], 'nodeMap': nm,
            'scripts': scripts, 'doors': doors, 'objects': objs, 'navGroups': nav_gid,
            'floodGroups': floods['solid'], 'floodGroupsBreak': floods['break'],
            'floodGroupsHammer': floods['hammer'],
            'asym': asym, 'unreachPairs': unreach, 'breakables': dict(breakables),
            'table': room_table(rom, room), 'cells': cells}


def ngroups(g):
    return len(set(g))


def script_kind(ops):
    k = [op for _, op, _ in ops]
    if k == ['end'] or k == []:
        return 'empty'
    return 'script'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('rom')
    ap.add_argument('--room', type=int, action='append')
    ap.add_argument('--json')
    ap.add_argument('--groups', action='store_true')
    ap.add_argument('--map', action='store_true', help='with --room: print the node map and doors')
    a = ap.parse_args()
    rom = rx.Rom(a.rom)
    at = attrs(rom)
    rooms = rx.valid_rooms(rom)
    out = []
    for room in rooms:
        try:
            out.append(decode_room(rom, room, at))
        except rx.BadPointer as e:
            print('room %d: %s' % (room, e), file=sys.stderr)

    entrances(out, at)
    if a.room:
        for d in out:
            if d['room'] in a.room:
                print_room(d, a.map)
    elif a.groups:
        for d in out:
            if ngroups(d['navGroups']) > 1 or ngroups(d['floodGroups']) > 1:
                print('%d: nav %s flood %s flood+break %s doors %s' % (
                    d['room'], d['navGroups'], d['floodGroups'], d['floodGroupsBreak'],
                    ['%d,%d>%d' % (x['x'], x['y'], x['dest']) for x in d['doors']]))
    else:
        survey(out)
    if a.json:
        for d in out:
            d.pop('cells')
            d['entries'] = [list(e) for e in d['entries']]
        Path(a.json).write_text(json.dumps(out))


def entrances(rooms, at):
    """For every door into a room: the room's doors its arrival cell reaches
    (cpu_ai_modes.c sHintReach), at tiers 0, 1 and 3.  Stored on the room
    as d['entries'] = [(from room, arrival x, y, [mask t0, t1, t3])]."""
    by = {d['room']: d for d in rooms}
    for d in rooms:
        d['entries'] = []
    for src in rooms:
        for x in src['doors']:
            d = by.get(x['dest'])
            if d is None:
                continue
            ax, ay, w = x['dx'] - 1, x['dy'] - 1, d['w']
            masks = []
            for tier in (0, 1, 3):
                seen = flood(d['cells'], w, d['h'], at, [ay * w + ax], tier)
                masks.append(sum(1 << i for i, y in enumerate(d['doors'][:32])
                                 if seen[y['y'] * w + y['x']]))
            d['entries'].append((src['room'], ax, ay, masks))


def print_room(d, show_map):
    t = d['table']
    print('room %d (content %d) %dx%d cells, %d nodes, area %s flags 0x%02X' % (
        d['room'], d['idx'], d['w'], d['h'], len(d['nodes']), t and t['area'], t and t['flags'] or 0))
    for i, nd in enumerate(d['nodes']):
        reach = [j for j, v in enumerate(nd['hop']) if v != NO]
        print('  node %2d (%4d,%4d) r%d nb %s reaches %d/%d' % (i, nd['x'], nd['y'], nd['r'], nd['nb'],
                                                              len(reach), len(d['nodes'])))
    for i, x in enumerate(d['doors']):
        print('  door (%d,%d) -> room %d (%d,%d) node %d  nav group %d flood group %d (+break %d)' % (
            x['x'], x['y'], x['dest'], x['dx'], x['dy'], x['node'], d['navGroups'][i],
            d['floodGroups'][i], d['floodGroupsBreak'][i]))
    for src, ax, ay, m in d.get('entries', []):
        print('  entered from %d at (%d,%d): reaches doors %s (breaking nothing), %s (levels 0-1, chains), %s (to level 3)' % (
            src, ax, ay, *[[i for i in range(len(d['doors'])) if mm >> i & 1] for mm in m]))
    for o in d['objects']:
        if o['type'] == 0x7F:
            print('  cpu trigger at (%d,%d) %s %s raise state to %d rect %s' % (
                o['x'], o['y'], o['mode'], o['who'], o['raise'], o['rect']))
        elif o['name'][0] != '0':
            print('  %s at (%d,%d) px' % (o['name'], o['x'], o['y']))
    for s, sc in enumerate(d['scripts']):
        print('  script row %d: %s' % (s, '; '.join('%d:%s' % (pc, fmt_op(op, ar)) for pc, op, ar in sc['ops'])))
    if t:
        print('  room table: %d routes, e.g. %s' % (len(t['routes']), t['routes'][:6]))
    print('  breakable cells by level: %s; nav one-way pairs %d' % (d['breakables'], d['asym']))
    if show_map:
        w = d['w']
        doorcells = {(x['x'], x['y']) for x in d['doors']}
        for y in range(d['h']):
            row = ''
            for x in range(w):
                v = d['nodeMap'][y * w + x]
                row += 'D ' if (x, y) in doorcells else ('%2s' % (format(v, 'x') if v != NO else '..'))
            print('   ' + row)


def survey(rooms):
    n = len(rooms)
    print('rooms decoded: %d' % n)
    nodes = [len(d['nodes']) for d in rooms]
    print('nav graph: every room has one: %d; nodes per room min %d max %d mean %.1f' % (
        sum(1 for x in nodes if x), min(nodes), max(nodes), sum(nodes) / n))
    radii = collections.Counter(nd['r'] for d in rooms for nd in d['nodes'])
    print('  node radius values: %s' % dict(radii))
    print('  rooms with unreachable node pairs: %d; with one-way pairs: %d' % (
        sum(1 for d in rooms if d['unreachPairs']), sum(1 for d in rooms if d['asym'])))
    nbok = sum(1 for d in rooms for i, nd in enumerate(d['nodes']) if nd['self'] == i)
    print('  node self-index in the tail bytes matches: %d/%d' % (nbok, sum(nodes)))
    # is the neighbour list = the set of direct next hops?
    agree = tot = 0
    for d in rooms:
        for i, nd in enumerate(d['nodes']):
            direct = sorted({h for j, h in enumerate(nd['hop']) if h != NO and h != i})
            tot += 1
            agree += sorted(nd['nb']) == direct
    print('  neighbour list == set of first hops: %d/%d nodes' % (agree, tot))
    # scripts
    ops = collections.Counter()
    nonempty = 0
    per_area = collections.defaultdict(lambda: [0, 0])
    abil = collections.Counter()
    shapes = collections.Counter()
    for d in rooms:
        has = False
        for sc in d['scripts']:
            k = [op for _, op, _ in sc['ops']]
            ops.update(k)
            if script_kind(sc['ops']) != 'empty':
                has = True
            for _, op, ar in sc['ops']:
                if op == 'ability':
                    abil[ar[0]] += 1
            shapes[' '.join(k)] += 1
        nonempty += has
        area = d['table']['area'] if d['table'] else -1
        per_area[area][0] += 1
        per_area[area][1] += has
    print('scripts: rooms with a non-empty script (any row): %d/%d' % (nonempty, n))
    print('  opcode counts: %s' % dict(ops))
    print('  ability operands: %s' % {('drop' if k == 0xFF else ABILITY[k] if k < len(ABILITY) else k): v
                                      for k, v in abil.items()})
    print('  most common script shapes:')
    for s, c in shapes.most_common(12):
        print('    %4d  %s' % (c, s))
    print('  per area (rooms, with script): %s' % dict(sorted(per_area.items())))
    trig = [(d['room'], o) for d in rooms for o in d['objects'] if o['type'] == 0x7F]
    print('cpu triggers: %d in %d rooms; modes %s; who %s; raise values %s' % (
        len(trig), len({r for r, _ in trig}), dict(collections.Counter(o['mode'] for _, o in trig)),
        dict(collections.Counter(o['who'] for _, o in trig)), dict(collections.Counter(o['raise'] for _, o in trig))))
    ifs = sum(1 for d in rooms if any(op == 'if_state' for sc in d['scripts'] for _, op, _ in sc['ops']))
    print('  rooms whose scripts branch on the room state: %d' % ifs)
    # door groups
    multi_nav = [d for d in rooms if ngroups(d['navGroups']) > 1]
    multi_fl = [d for d in rooms if ngroups(d['floodGroups']) > 1]
    multi_flb = [d for d in rooms if ngroups(d['floodGroupsBreak']) > 1]
    multi_flh = [d for d in rooms if ngroups(d['floodGroupsHammer']) > 1]
    print('door groups (mutual): rooms with doors %d; >1 group by nav graph %d, by flood breaking nothing %d, '
          'levels 0-1 + chains %d, to level 3 %d' % (sum(1 for d in rooms if d['doors']), len(multi_nav),
                                                    len(multi_fl), len(multi_flb), len(multi_flh)))
    for k, name in ((0, 'breaking nothing'), (1, 'breaking levels 0-1 and chains'), (2, 'breaking to level 3')):
        rs = [d for d in rooms if any(m[k] != (1 << min(len(d['doors']), 32)) - 1 for _, _, _, m in d['entries'])]
        ent = sum(1 for d in rooms for _, _, _, m in d['entries'] if m[k] != (1 << min(len(d['doors']), 32)) - 1)
        print('  entrances that do not reach every door of the room (%s): %d in %d rooms' % (name, ent, len(rs)))
    same = sum(1 for d in rooms if d['doors'] and same_partition(d['navGroups'], d['floodGroups']))
    sameb = sum(1 for d in rooms if d['doors'] and same_partition(d['navGroups'], d['floodGroupsBreak']))
    print('  nav partition == flood partition: %d (no breaking), %d (breaking all) of %d rooms with doors' % (
        same, sameb, sum(1 for d in rooms if d['doors'])))
    dn = collections.Counter('ok' if x['node'] != NO else 'no node' for d in rooms for x in d['doors'])
    print('  door cells with a nav node: %s' % dict(dn))
    brk = sum(1 for d in rooms if d['breakables'])
    print('breakable cells: rooms with any %d; by level %s' % (
        brk, dict(sum((collections.Counter(d['breakables']) for d in rooms), collections.Counter()))))
    tbl = [d for d in rooms if d['table']]
    print('room table (gUnk_08D6CD0C): %d rooms; flags %s' % (
        len(tbl), dict(collections.Counter(d['table']['flags'] for d in tbl))))


def same_partition(a, b):
    return all((a[i] == a[j]) == (b[i] == b[j]) for i in range(len(a)) for j in range(len(a)))


if __name__ == '__main__':
    main()
