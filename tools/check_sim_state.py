#!/usr/bin/env python3
"""check_sim_state.py -- no simulation state outside the GBA map and the port
sim block (NETPLAY-V2 R7, M1; platform/port/simblock.h).

A snapshot, a hash and a state transfer cover the GBA map and the port sim
block at 0x02040000, and nothing else.  So any writable object a simulation
translation unit defines in host .data/.bss is either

  * not simulation state -- a constant table C++ initialises at startup, a
    diagnostic counter, a host setting the recording carries as an input --
    and listed in tests/sim_state_allowlist.txt with the reason, or
  * a bug: state a snapshot restore silently leaves behind (rando's list
    parity was one: rando + rollback desynced).

    tools/check_sim_state.py [BUILD_DIR]      (default build/lp64)

Reads the native LP64 objects with objdump -t (the section names tell .data /
.bss apart from .rodata and .data.rel.ro, which nm's letters do not).  Checks
every object under game/ and the platform files that run inside the
simulation (SIM_PLATFORM below).  Exit status 1 on an unlisted symbol, and on
an allowlist entry that no longer matches anything (so the list cannot rot).
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ALLOW = os.path.join(ROOT, 'tests', 'sim_state_allowlist.txt')

# Platform files whose code the game calls, or that the frame loop runs, as
# part of a simulated frame.  main.c is not here: it holds the host's own
# diagnostics, and its frame state moved into the sim block.
SIM_PLATFORM = ['rando', 'warp', 'bios', 'sram', 'dma', 'coop']

SEC = re.compile(r'\s(\.data|\.bss|\.tbss|\.tdata)(\.[^\s]*)?\s')


def objects(build):
    base = os.path.join(build, 'CMakeFiles', 'katam.dir')
    out = []
    for dp, _, fs in os.walk(os.path.join(base, 'game')):
        out += [os.path.join(dp, f) for f in fs if f.endswith('.o')]
    for f in SIM_PLATFORM:
        out.append(os.path.join(base, 'platform', f + '.c.o'))
    return sorted(out)


def writable(obj):
    txt = subprocess.run(['objdump', '-t', obj], capture_output=True, text=True,
                         check=True).stdout
    syms = []
    for line in txt.splitlines():
        m = SEC.search(line)
        if not m or '.data.rel.ro' in line:
            continue
        name = line.split()[-1]
        if name.startswith('.'):
            continue
        syms.append(name)
    return syms


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'lp64')
    allow = {}
    for ln in open(ALLOW):
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        parts = ln.split(None, 3)
        allow[(parts[0], parts[1])] = parts[2]
    found = set()
    bad = []
    objs = objects(build)
    if not objs or not os.path.exists(objs[0]):
        print('check_sim_state: no objects under %s -- build build/lp64 first' % build)
        return 2
    for o in objs:
        unit = os.path.basename(o)[:-len('.c.o')]
        for s in writable(o):
            if (unit, s) in allow:
                found.add((unit, s))
            else:
                bad.append((unit, s))
    stale = sorted(set(allow) - found)
    for u, s in bad:
        print('%s: %s is writable host memory outside the GBA map and the sim '
              'block -- move it into port/simblock.h or allowlist it with a '
              'reason (tests/sim_state_allowlist.txt)' % (u, s))
    for u, s in stale:
        print('allowlist entry %s %s matches nothing -- remove it' % (u, s))
    print('check_sim_state: %d objects, %d allowlisted symbols, %d unlisted, '
          '%d stale' % (len(objs), len(found), len(bad), len(stale)))
    return 1 if bad or stale else 0


if __name__ == '__main__':
    sys.exit(main())
