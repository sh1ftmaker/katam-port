#!/usr/bin/env python3
"""matrix.py -- the online-multiplayer determinism audit: what, besides the
inputs, can make two devices' game memory differ?

    python3 tools/netaudit/matrix.py [--rom ROM] [--eval DIR] [--jobs N]
                                     [--only ID,ID] [--no-build-check]

Every run replays one recording through one "device" and logs, per frame, the
raw whole-image hashes platform/audit.c takes at the top of the frame (the
rollback snapshot point: EWRAM, IWRAM, IO, palette, VRAM, OAM, the port sim
block -- unmasked, uncanonicalised), plus the save memory and the out-of-map
host state as separate columns.  A factor flips ONE device-side thing against
a baseline device and reports SAME or DIFFERS, the first differing frame, and
-- from raw dumps of both runs at that frame -- the differing byte ranges by
symbol.  Writes DIR/AUDIT-REPORT.md and DIR/audit-report.json.

The wasm runs use build/katam-node.js (tools/netaudit/run_node.js); a few
factors that only the native build can show (the GL renderer, the window)
use build/lp64/katam.  Build both first (docs/DETERMINISM.md).  ROM-derived
output (dumps, logs) stays under DIR/audit-tmp/matrix.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COLS = ['image', 'ewram', 'iwram', 'io', 'pltt', 'vram', 'oam', 'sim',
        'sram', 'iocanon', 'dmahost', 'ppuhost']
IMAGE_COLS = COLS[:8]

# --- running ------------------------------------------------------------------


class Ctx:
    pass


C = Ctx()


def run_node(name, cfg, dumps=()):
    d = C.tmp / name
    d.mkdir(parents=True, exist_ok=True)
    full = {'module': str(ROOT / 'build' / 'katam-node.js'), 'rom': str(C.rom),
            'out': str(d / 'hash.txt'), 'dumpDir': str(d / 'dumps')}
    full.update(cfg)
    full['replay'] = str(cfg['replay'])
    if dumps:
        full['dumps'] = sorted(set(dumps))
    (d / 'cfg.json').write_text(json.dumps(full, indent=1))
    t = time.time()
    r = subprocess.run(['node', str(ROOT / 'tools' / 'netaudit' / 'run_node.js'), str(d / 'cfg.json')],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    (d / 'stderr.txt').write_text(r.stdout + r.stderr)
    return d, time.time() - t, r.returncode


def run_native(name, spec, dumps=()):
    d = C.tmp / name
    if d.exists():
        shutil.rmtree(d)
    (d / 'home').mkdir(parents=True)
    env = dict(os.environ, XDG_DATA_HOME=str(d / 'home' / 'd'), XDG_CONFIG_HOME=str(d / 'home' / 'c'),
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PORT_AUDIT_LOG=str(d / 'hash.txt'))
    env.update(spec.get('env', {}))
    if spec.get('ini'):
        cd = d / 'home' / 'd' / 'katam-port' / 'katam-port'
        cd.mkdir(parents=True, exist_ok=True)
        (cd / 'settings.ini').write_text(spec['ini'])
    if dumps:
        (d / 'dumps').mkdir(exist_ok=True)
        env['PORT_AUDIT_DUMP'] = ','.join('%d:%s' % (f, d / 'dumps' / ('%d.kaud' % f)) for f in sorted(set(dumps)))
    args = spec.get('args', ['--no-audio'])
    cmd = ['setarch', '-R', str(ROOT / 'build' / 'lp64' / 'katam'), '--turbo',
           '--replay', str(spec['replay'])] + args + [str(C.rom)]
    t = time.time()
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, timeout=1800)
    (d / 'stderr.txt').write_text(r.stdout + r.stderr)
    return d, time.time() - t, r.returncode


def run(name, spec, dumps=()):
    if spec.get('native'):
        return run_native(name, spec, dumps)
    cfg = {k: v for k, v in spec.items() if k not in ('native',)}
    return run_node(name, cfg, dumps)

# --- comparing -----------------------------------------------------------------


def load(path):
    rows = {}
    if not path.exists():
        return rows
    for ln in path.read_text().splitlines():
        if not ln or ln[0] == '#':
            continue
        p = ln.split()
        rows[int(p[0])] = p[1:]
    return rows


def compare(a, b):
    frames = sorted(set(a) & set(b))
    res = {'frames': len(frames), 'missing': len(set(a) ^ set(b))}
    for i, c in enumerate(COLS):
        first, n = None, 0
        for f in frames:
            if a[f][i] != b[f][i]:
                n += 1
                if first is None:
                    first = f
        res[c] = {'first': first, 'frames': n}
    return res


# --- dumps and symbols -------------------------------------------------------------

def read_kaud(path):
    b = path.read_bytes()
    assert b[:4] == b'KAUD'
    _, frame, n = struct.unpack_from('<III', b, 4)
    parts, off = [], 16
    for i in range(n):
        name = b[off:off + 8].rstrip(b'\0').decode()
        base, size = struct.unpack_from('<II', b, off + 8)
        parts.append((name, base, size))
        off += 16
    out = {}
    for name, base, size in parts:
        out[name] = (base, b[off:off + size])
        off += size
    return frame, out


SIM_FIELDS = [(0x00, 0x20, 'sim.hdr'), (0x20, 0x24, 'sim.frame.frameCount'), (0x24, 0x28, 'sim.frame.vblankBudget'),
              (0x28, 0x2A, 'sim.frame.keysDown'), (0x2A, 0x30, 'sim.frame.lmpPressed'),
              (0x30, 0x40, 'sim.lmp'), (0x40, 0x50, 'sim.warp'), (0x50, 0x70, 'sim.view'),
              (0x70, 0xB0, 'sim.game'), (0xB0, 0xB8, 'sim.session'), (0xB8, 0x10000, 'sim.rando')]
IO_REGS = [(0x00, 0x02, 'DISPCNT'), (0x04, 0x06, 'DISPSTAT'), (0x06, 0x08, 'VCOUNT'), (0x08, 0x60, 'BG/window/blend regs'),
           (0x60, 0xB0, 'SOUND regs'), (0xB0, 0xE0, 'DMA0-3 SAD/DAD/CNT'), (0x100, 0x110, 'TIMERS'),
           (0x120, 0x130, 'SIO'), (0x130, 0x134, 'KEYINPUT/KEYCNT'), (0x134, 0x200, 'SIO/RCNT'),
           (0x200, 0x20C, 'IE/IF/WAITCNT/IME'), (0x20C, 0x400, 'IO other')]


# Game state a player would see diverge (positions, HP, the room, enemies'
# AI, the RNG): if any of these differ, the difference reached gameplay.
GAMEPLAY = {'gKirbys', 'gCurLevelInfo', 'gRngVal', 'gUnk_02038590'}


def elf_symbols():
    elf = C.elf
    syms = []
    if not elf or not Path(elf).exists():
        return syms
    try:
        txt = subprocess.run(['nm', '-S', '-n', str(elf)], capture_output=True, text=True).stdout
    except OSError:
        return syms
    for ln in txt.splitlines():
        p = ln.split()
        if len(p) < 3:
            continue
        try:
            addr = int(p[0], 16)
        except ValueError:
            continue
        if not (0x02000000 <= addr < 0x02040000 or 0x03000000 <= addr < 0x03008000):
            continue
        name = p[-1]
        if name.startswith('$') or name.endswith('_START') or name.endswith('_END') or p[-2] in ('a', 'A'):
            continue
        size = int(p[1], 16) if len(p) == 4 else 0
        syms.append([addr, size, name])
    syms.sort()
    for i, s in enumerate(syms):
        if s[1] == 0:
            nxt = next((t[0] for t in syms[i + 1:] if t[0] > s[0]), s[0] + 4)
            s[1] = min(nxt - s[0], 0x10000)
    return syms


def name_of(part, base, off):
    a = base + off
    if part == 'sim':
        for lo, hi, n in SIM_FIELDS:
            if lo <= off < hi:
                return '%s+0x%X' % (n, off - lo)
    if part == 'io':
        for lo, hi, n in IO_REGS:
            if lo <= off < hi:
                return 'IO+0x%03X %s' % (off, n)
    if part == 'vram':
        return 'VRAM %s +0x%05X' % ('BG' if off < 0x10000 else 'OBJ tiles', off)
    if part == 'pltt':
        return 'PLTT %s' % ('BG' if off < 0x200 else 'OBJ')
    if part == 'oam':
        return 'OAM #%d' % (off // 8)
    if part in ('ewram', 'iwram'):
        best = None
        for s in C.syms:
            if s[0] <= a < s[0] + s[1]:
                best = s
            if s[0] > a:
                break
        if best:
            return '%s+0x%X' % (best[2], a - best[0])
        return '0x%08X' % a
    return '%s+0x%X' % (part, off)


def diff_dumps(pa, pb, limit=30):
    _, A = read_kaud(pa)
    _, B = read_kaud(pb)
    ranges = []
    for part in A:
        base, x = A[part]
        y = B[part][1] if part in B else b''
        n = min(len(x), len(y))
        i = 0
        cur = None
        while i < n:
            if x[i] != y[i]:
                if cur and i - cur[1] <= 16:
                    cur[1] = i + 1
                    cur[2] += 1
                else:
                    if cur:
                        ranges.append(cur)
                    cur = [i, i + 1, 1, part, base]
            i += 1
        if cur:
            ranges.append(cur)
    by_part, by_sym = {}, {}
    for lo, hi, cnt, part, base in ranges:
        by_part[part] = by_part.get(part, 0) + cnt
        nm = name_of(part, base, lo)
        key = nm.split('+')[0] if part in ('ewram', 'iwram', 'sim') else nm.split(' +')[0]
        by_sym[key] = by_sym.get(key, 0) + cnt
    gameplay = sorted(k for k in by_sym if k in GAMEPLAY)
    out = []
    for lo, hi, cnt, part, base in sorted(ranges, key=lambda r: -r[2])[:limit]:
        bx = A[part][1][lo:min(hi, lo + 8)].hex()
        by = B[part][1][lo:min(hi, lo + 8)].hex()
        addr = base + lo if base else None
        out.append({'part': part, 'addr': ('0x%08X' % addr) if addr is not None else None,
                    'len': hi - lo, 'bytes': cnt, 'name': name_of(part, base, lo), 'a': bx, 'b': by})
    top = sorted(by_sym.items(), key=lambda kv: -kv[1])[:15]
    return {'bytes_by_part': by_part, 'by_symbol': top, 'gameplay': gameplay, 'ranges': out}

# --- the factors --------------------------------------------------------------------


def factors(R):
    tour = R / 'localmp_tour.txt'
    menus = R / 'localmp_menus.txt'
    T = {'replay': tour}
    Mn = {'replay': menus, 'menus': True}
    desk = {'rate': 48000, 'quantum': 128}
    phone = {'rate': 44100, 'quantum': 1024, 'jitter': 0.5, 'seed': 7}
    F = []

    def f(fid, group, title, base, var, note='', base_name=None):
        F.append({'id': fid, 'group': group, 'title': title, 'base': base, 'var': var,
                  'note': note, 'base_name': base_name})
    # baselines: same build, two processes
    f('baseline-tour', 'baseline', 'tour: two processes, identical device', T, dict(T))
    f('baseline-menus', 'baseline', 'menus recording, menus on: two processes', Mn, dict(Mn))
    f('baseline-native', 'baseline', 'native build: two processes (diagnostic aid)',
      {'native': True, 'replay': tour}, {'native': True, 'replay': tour})
    # audio
    f('audio-on-off', 'audio', 'audio device 48 kHz vs none', T, dict(T, audio=desk))
    f('audio-rate', 'audio', 'audio 44.1 kHz vs 48 kHz (both on)', dict(T, audio=desk),
      dict(T, audio=dict(desk, rate=44100)))
    f('audio-block', 'audio', 'same rate, phone-like device (1024-frame callbacks, jitter) vs desktop (128)',
      dict(T, audio=desk), dict(T, audio=dict(phone, rate=48000)))
    f('audio-live', 'audio', 'AudioContext running (AudioTopUp path) vs suspended, same device',
      dict(T, audio=desk), dict(T, audio=dict(desk, live=True)))
    f('audio-live-30hz', 'audio', 'live audio at 30 Hz pacing vs live audio at 60 Hz',
      dict(T, audio=dict(desk, live=True)), dict(T, audio=dict(desk, live=True), pace={'dts': [33.49, 0]}))
    # pacing / rendering
    f('pace-30hz', 'pacing', '30 Hz device (two frames per vsync) vs 60 Hz, no audio', T,
      dict(T, pace={'dts': [33.49, 0]}))
    f('pace-burst', 'pacing', 'catch-up bursts (8 frames at once every 60) vs paced, no audio', T,
      dict(T, pace={'dts': [0] * 7 + [16.74 * 8] + [16.74] * 52}))
    f('render-off-all', 'pacing', 'PortSetRenderEnabled(0) on every frame vs rendering', T,
      dict(T, renderOff='all'))
    f('render-off-burst', 'pacing', 'rendering off for 8 of every 60 frames (replayed frames) vs on', T,
      dict(T, renderOff={'burst': 8, 'period': 60}))
    f('pace-burst-audio', 'pacing', 'catch-up bursts with render off, live audio vs paced live audio',
      dict(T, audio=dict(desk, live=True)),
      dict(T, audio=dict(desk, live=True), pace={'dts': [0] * 7 + [16.74 * 8] + [16.74] * 52},
           renderOff={'burst': 8, 'period': 60}))
    # host environment
    f('clock', 'host', 'wall clock 10^9 ms later, 1% fast', T, dict(T, clock={'offsetMs': 1e9, 'rate': 1.01}))
    f('random', 'host', 'Math.random seeded differently', dict(T, random=1), dict(T, random=99))
    f('malloc-shift', 'host', 'every malloc address shifted (64 KiB + 12 B allocated first)', T,
      dict(T, mallocShift=65548))
    f('heap-junk', 'host', 'freed heap pre-filled with 0xA5 (uninitialised memory)', T, dict(T, heapJunk=0xA5))
    # menus / URL / input devices
    f('menus-switch', 'switch', 'in-game menus on (a browser) vs off (node) on the tour recording', T,
      dict(T, menus=True))
    f('menus-param0', 'switch', '?menus=0 vs no location (both off)', T, dict(T, menus='0'))
    f('p2-input', 'input', 'touch-only device (PortLmpSetP2Input(0)) vs keyboard/gamepad, menus recording',
      Mn, dict(Mn, p2Input=0))
    f('p2-input-tour', 'input', 'touch-only vs keyboard on the tour (no menus)', T, dict(T, p2Input=0))
    # stored settings / saves
    stg = {'katam.settings': 'renderer=1\nzoom=0.500\naspect=5\nscaling=2\nvolume=40\nmute=0\nsmoothcam2=1\n'}
    f('settings-menus', 'storage', 'different stored settings (localStorage katam.settings), menus recording',
      Mn, dict(Mn, storage=stg))
    f('settings-tour', 'storage', 'different stored settings, tour (no menus)', T, dict(T, storage=stg))
    f('sram', 'storage', 'device has a played save vs a blank one', T, dict(T, sram=str(C.save)))
    # the view
    f('view-follow', 'view', 'sim view follows the display: 746x320 (phone 21:9, zoomed) vs the recorded 480x320',
      T, dict(T, replay='@view746'))
    f('view-pin-native', 'view', 'PortSimViewPinNative() vs unpinned', T, dict(T, viewPin='native'))
    f('view-pin-session', 'view', 'PortSimViewUseSession() (session view events) vs unpinned, same events',
      T, dict(T, viewPin='session'))
    f('view-mode', 'view', 'page view experiment: widescreen (PortSetViewMode 1,40) from frame 1500', T,
      dict(T, viewMode=[1500, 1, 40, 0, 0, 0, 2]))
    # joining: a device that receives only the host's image at frame F
    IMG, FULL = 0x7F, 0x3FF
    def join(fid, title, base, F, rec, mask, note=''):
        v = dict(base, replay=rec, join={'frame': F, 'mask': mask, 'donor': fid})
        f(fid, 'join', title, base, v, note)
        F_[fid] = F
    F_ = {}
    join('join-tour-image', 'join at 3300 (mid-play) with the image only', T, 3300, '@join_tour3300', IMG)
    join('join-tour-image-lmp', 'join at 3300: image + the local-MP setting (option localmp 2)', T, 3300,
         '@join_tour3300lmp', IMG)
    join('join-tour-full', 'join at 3300: image + setting + SRAM + DMA table + PPU latches', T, 3300,
         '@join_tour3300lmp', FULL)
    join('join-menus-image', 'join at 1720 (P1 on the pause SETTINGS page; LOCAL chosen in the menu): image only',
         Mn, 1720, '@join_menus1720', IMG)
    join('join-menus-full', 'join at 1720: image + SRAM + DMA + PPU + localmp setting', Mn, 1720,
         '@join_menus1720lmp', FULL)
    for x in F:
        if x['id'] in F_:
            x['from'] = F_[x['id']]
    # --- the same device factors with an online session on (platform/lmpnet.c,
    # platform/lnsession.c): every device in a session must hold the same raw
    # image whatever its audio, pacing, display or storage.  Host-vs-host
    # rows: both devices start a host session at frame 1 and the recording
    # drives both pads.  Guest rows: a guest-session device with its own
    # storage receives the host device's image at frame 2 (as a joiner does),
    # compared from there.  Replay rows: a guest forced to rewind 20 frames
    # every 37, so a third of its frames are replayed (muted, render off).
    HS = {'role': 1, 'at': 0}
    GS = {'role': 2, 'at': 0}
    GRW = {'role': 2, 'at': 0, 'forceRewind': {'every': 37, 'depth': 20}}
    S = dict(T, session=HS)
    SM = dict(Mn, session=HS)
    live = dict(desk, live=True)
    phone_live = dict(phone, live=True)
    burst = {'dts': [0] * 7 + [16.74 * 8] + [16.74] * 52}
    f('s-baseline', 'session', 'in a session: two processes, identical device', S, dict(S))
    f('s-audio-on-off', 'session', 'in a session: audio device 48 kHz vs none', S, dict(S, audio=desk))
    f('s-audio-rate', 'session', 'in a session: audio 44.1 kHz vs 48 kHz', dict(S, audio=desk),
      dict(S, audio=dict(desk, rate=44100)))
    f('s-audio-block', 'session', 'in a session: phone-like device (1024-frame callbacks, jitter) vs desktop (128)',
      dict(S, audio=desk), dict(S, audio=dict(phone, rate=48000)))
    f('s-audio-phone', 'session', 'in a session: phone 44.1 kHz, 1024, jitter, live vs no audio', S,
      dict(S, audio=phone_live))
    f('s-audio-live-30hz', 'session', 'in a session: live audio at 30 Hz pacing (queue starves) vs live at 60 Hz',
      dict(S, audio=live), dict(S, audio=live, pace={'dts': [33.49, 0]}))
    f('s-pace-burst-audio', 'session', 'in a session: catch-up bursts with render off, live audio vs paced live audio',
      dict(S, audio=live), dict(S, audio=live, pace=burst, renderOff={'burst': 8, 'period': 60}))
    f('s-replays-audio', 'session', 'in a session: guest replaying 1/3 of its frames, phone audio live vs host, no audio',
      S, dict(T, session=GRW, audio=phone_live))
    f('s-replays-noaudio', 'session', 'in a session: guest replaying 1/3 of its frames, no audio vs host, desktop audio',
      dict(S, audio=desk), dict(T, session=GRW))
    f('s-view-follow', 'session', 'in a session: the display asks for 746x320 (PortSimViewRequest) vs not',
      S, dict(S, viewRequestAt=[1, 746, 320]))
    f('s-view-mode', 'session', 'in a session: page view experiment widescreen (PortSetViewMode 1,40) from 1500',
      S, dict(S, viewMode=[1500, 1, 40, 0, 0, 0, 2]))
    f('s-p2-input', 'session', 'in a session, menus recording: touch-only device vs keyboard/gamepad', SM,
      dict(SM, p2Input=0))
    f('s-settings-tour', 'session', 'in a session: different stored settings, tour', S, dict(S, storage=stg))
    f('s-settings-host', 'session', 'in a session, menus recording: the HOST has different stored settings', SM,
      dict(SM, storage=stg))

    def sjoin(fid, title, base, extra, Fj=2, role=None):
        v = dict(base, **extra)
        v['session'] = dict(role or GS)
        v['join'] = {'frame': Fj, 'mask': 0x3FF, 'donor': fid}
        f(fid, 'session', title, base, v)
        # From the frame after the load: at the load frame itself the IO DMA
        # source mirror is the donor run's (a stack address that its dump
        # call moved: class d, dma.c:311; iocanon equal), not the base's.
        F_[fid] = Fj + 1
    sjoin('s-guest-settings', 'in a session, menus recording: a guest with different stored settings joins (image at 2)',
          SM, {'storage': stg})
    sjoin('s-guest-sram', 'in a session: a guest with its own played save joins (image at 2)', S,
          {'sram': str(C.save)})
    sjoin('s-replays-menus', 'in a session, menus recording: a phone guest joins (image at 2) and replays 1/3 of its frames',
          SM, {'audio': phone_live}, role=GRW)
    sjoin('s-guest-all', 'in a session, menus recording: a phone guest (44.1 kHz, touch-only, own settings and save) joins',
          SM, {'storage': stg, 'sram': str(C.save), 'p2Input': 0, 'audio': phone_live})
    for x in F:
        if x['id'] in F_:
            x['from'] = F_[x['id']]
        elif x['group'] == 'session':
            # The session starts at the present of frame 0; the image at the
            # top of frame 0 is the device's own, before it.
            x['from'] = 1
    # native-only
    N = {'native': True, 'replay': tour}
    f('native-gpu', 'display', 'native: GPU renderer (offscreen GL) vs PPU', N,
      dict(N, args=['--no-audio', '--renderer', 'gpu', '--window', '960x640'],
           env={'SDL_VIDEODRIVER': 'offscreen'}),
      base_name=None)
    f('native-window', 'display', 'native: GPU renderer, 2400x1080 window vs 960x640',
      dict(N, args=['--no-audio', '--renderer', 'gpu', '--window', '960x640'], env={'SDL_VIDEODRIVER': 'offscreen'}),
      dict(N, args=['--no-audio', '--renderer', 'gpu', '--window', '2400x1080'], env={'SDL_VIDEODRIVER': 'offscreen'}))
    f('native-ini', 'display', 'native: GPU, settings.ini zoom 0.5, 21:9, smooth camera, smooth pixels, volume 40 vs defaults',
      dict(N, args=['--no-audio', '--renderer', 'gpu', '--window', '960x640'], env={'SDL_VIDEODRIVER': 'offscreen'}),
      dict(N, args=['--no-audio', '--renderer', 'gpu', '--window', '960x640'], env={'SDL_VIDEODRIVER': 'offscreen'},
           ini='zoom=0.5\naspect=5\nsmoothcam2=1\nscaling=2\nvolume=40\ninterp=1\nhud=1\n'))
    f('native-audio', 'audio', 'native: SDL audio device (dummy driver) vs --no-audio', N,
      dict(N, args=[]))
    return F


def strip_before(text, F, keep_localmp, add_localmp=False):
    """The recording as a device that joins at frame F sees it: nothing
    before F (no keys, no warps), the same from F on.  `view` lines are the
    session's and are kept."""
    out, k1, kp = [], 0, {}
    lines = text.splitlines()
    body = []
    for ln in lines:
        p = ln.split()
        if not p:
            continue
        if p[0] == 'k' and int(p[1]) < F:
            k1 = int(p[2], 16)
            continue
        if p[0] == 'kp' and int(p[2]) < F:
            kp[p[1]] = int(p[3], 16)
            continue
        if p[0] == 'warp' and int(p[1]) < F:
            continue
        if ln.startswith('option localmp') and not keep_localmp:
            continue
        if p[0] == 'end':
            continue
        body.append(ln)
    hdr = [l for l in body if not l.startswith(('k ', 'kp '))]
    keys = [l for l in body if l.startswith(('k ', 'kp '))]
    if add_localmp and not any(l.startswith('option localmp') for l in hdr):
        hdr.insert(hdr.index(next(l for l in hdr if l.startswith('frames'))) + 1, 'option localmp 2')
    pre = ['k 0 000', 'k %d %03X' % (F, k1)] + ['kp %s %d %03X' % (q, F, m) for q, m in kp.items()]
    return '\n'.join(hdr + pre + keys + ['end']) + '\n'


def prepare_recordings(R, out):
    """Variant recordings: the tour with its view line as a phone would ask."""
    src = (R / 'localmp_tour.txt').read_text()
    v = re.sub(r'^view 0 \d+ \d+$', 'view 0 746 320', src, flags=re.M)
    p = out / 'localmp_tour_view746.txt'
    p.write_text(v)
    al = {'@view746': p}
    m = (R / 'localmp_menus.txt').read_text()
    for name, text, F, keep, add in (('tour3300', src, 3300, False, False),
                                     ('tour3300lmp', src, 3300, True, False),
                                     ('menus1720', m, 1720, False, False),
                                     ('menus1720lmp', m, 1720, False, True)):
        q = out / ('join_%s.txt' % name)
        q.write_text(strip_before(text, F, keep, add))
        al['@join_' + name] = q
    return al


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=str(ROOT.parent / 'decomp' / 'baserom.gba'))
    ap.add_argument('--elf', default=str(ROOT.parent / 'decomp' / 'katam.elf'))
    ap.add_argument('--eval', default=str(ROOT.parent / 'netproof-eval'))
    ap.add_argument('--jobs', type=int, default=max(2, min(12, (os.cpu_count() or 4) // 2)))
    ap.add_argument('--only', default='')
    ap.add_argument('--save', default='', help='a played 64 KiB save for the sram factor')
    a = ap.parse_args()
    t0 = time.time()
    C.rom = Path(a.rom)
    C.elf = a.elf
    ev = Path(a.eval)
    C.tmp = ev / 'audit-tmp' / 'matrix'
    C.tmp.mkdir(parents=True, exist_ok=True)
    C.syms = elf_symbols()
    R = ROOT / 'tests' / 'recordings'
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'netaudit' / 'make_localmp_recordings.py')], check=True)
    alias = prepare_recordings(R, C.tmp)
    C.save = Path(a.save) if a.save else ev / 'audit-tmp' / 'save_after_tour.sav'
    if not C.save.exists():
        # Make one: the tour natively, keep the save it writes.
        d, _, _ = run_native('make-save', {'replay': R / 'localmp_tour.txt'})
        found = list((d / 'home').rglob('*.sav'))
        if found:
            shutil.copy(found[0], C.save)
    F = factors(R)
    if a.only:
        keep = set(a.only.split(','))
        F = [x for x in F if x['id'] in keep]

    def fix(spec):
        s = dict(spec)
        if isinstance(s.get('replay'), str) and s['replay'] in alias:
            s['replay'] = alias[s['replay']]
        return s

    # Donors: the baseline device's image at each join frame.
    donors = {}
    for x in F:
        j = x['var'].get('join')
        if j:
            key = (json.dumps(fix(x['base']), sort_keys=True, default=str), j['frame'])
            donors.setdefault(key, []).append(x)
    for i, ((bk, fr), xs) in enumerate(donors.items()):
        name = 'donor%02d-%d' % (i, fr)
        run(name, fix(xs[0]['base']), [fr])
        img = C.tmp / name / 'dumps' / ('%d.kaud' % fr)
        for x in xs:
            j = dict(x['var']['join'])
            j.pop('donor', None)
            j['image'] = str(img)
            x['var'] = dict(x['var'], join=j)

    # Pass 1: every distinct device once (baselines are shared by spec).
    keyed = {}
    jobs = []
    for x in F:
        for side in ('base', 'var'):
            spec = fix(x[side])
            k = json.dumps(spec, sort_keys=True, default=str)
            if side == 'var' and x['group'] == 'baseline':
                k += '#2'          # a second process of the same device
            if k not in keyed:
                keyed[k] = 'dev%02d' % len(keyed)
                jobs.append((keyed[k], spec))
            x[side + '_run'] = keyed[k]
            x[side + '_spec'] = spec
    timings = {}
    with cf.ThreadPoolExecutor(a.jobs) as pool:
        for (name, spec), (d, secs, rc) in zip(jobs, pool.map(lambda j: run(*j), jobs)):
            timings[name] = (secs, rc)
            print('ran %s %.1fs rc %d' % (name, secs, rc), flush=True)

    # Compare; pass 2 dumps at the first differing frame.
    redo = []
    for x in F:
        A = load(C.tmp / x['base_run'] / 'hash.txt')
        B = load(C.tmp / x['var_run'] / 'hash.txt')
        if x.get('from'):
            A = {k: v for k, v in A.items() if k >= x['from']}
            B = {k: v for k, v in B.items() if k >= x['from']}
        x['cmp'] = compare(A, B)
        x['rc'] = [timings[x['base_run']][1], timings[x['var_run']][1]]
        first = x['cmp']['image']['first']
        x['verdict'] = 'SAME' if first is None and x['cmp']['missing'] == 0 else 'DIFFERS'
        if not A or not B:
            x['verdict'] = 'FAILED'
        if first is not None:
            fr = [first]
            if x['cmp']['sram']['first'] is not None:
                fr.append(x['cmp']['sram']['first'])
            last = max(set(A) & set(B))
            if last not in fr:
                fr.append(last)
            x['dump_frames'] = fr
            redo.append(x)
    dumpjobs = {}
    for x in redo:
        for side in ('base', 'var'):
            n = x[side + '_run'] + '-d' + '-'.join(map(str, x['dump_frames']))
            dumpjobs[n] = (x[side + '_spec'], x['dump_frames'])
            x[side + '_dump'] = n
    with cf.ThreadPoolExecutor(a.jobs) as pool:
        list(pool.map(lambda kv: run(kv[0], kv[1][0], kv[1][1]), dumpjobs.items()))
    for x in redo:
        x['diffs'] = {}
        for fr in x['dump_frames']:
            pa = C.tmp / x['base_dump'] / 'dumps' / ('%d.kaud' % fr)
            pb = C.tmp / x['var_dump'] / 'dumps' / ('%d.kaud' % fr)
            if pa.exists() and pb.exists():
                x['diffs'][fr] = diff_dumps(pa, pb)
    meta = {}
    for name, _ in jobs:
        m = C.tmp / name / 'hash.meta.json'
        if m.exists():
            meta[name] = json.loads(m.read_text())
    # Live (not a replay): does the window's shape become a `view` event?
    probe = {}
    for w in ('960x640', '2400x1080'):
        d = C.tmp / ('live-' + w)
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        env = dict(os.environ, XDG_DATA_HOME=str(d), XDG_CONFIG_HOME=str(d), SDL_VIDEODRIVER='offscreen')
        subprocess.run(['setarch', '-R', str(ROOT / 'build' / 'lp64' / 'katam'), '--no-audio', '--turbo',
                        '--renderer', 'gpu', '--gpu-view', 'auto', '--window', w, '--frames', '200',
                        '--record', str(d / 'rec.txt'), str(C.rom)], env=env, capture_output=True, timeout=600)
        rec = d / 'rec.txt'
        probe[w] = [l for l in rec.read_text().splitlines() if l.startswith('view')] if rec.exists() else None
    meta['live_view_probe'] = probe
    secs = time.time() - t0
    import report
    report.write(ev, F, meta, secs, ROOT)
    print('wrote %s/AUDIT-REPORT.md in %.0f s' % (ev, secs))


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    main()
