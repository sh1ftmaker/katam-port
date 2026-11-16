#!/usr/bin/env python3
"""make_localmp_recordings.py -- the local-multiplayer scenario recordings for
the determinism audit (tools/netaudit/), in tests/recordings/localmp_*.txt.

Inputs only, like tools/make_recordings.py: key masks per frame for player 1
(`k`) and player 2 (`kp 1`), warps by room and cell, the session view and
`option localmp 2`.  Nothing comes from the ROM; the door cells and arrival
cells below were found by playing (KATAM_LOCAL_MP_DOORS=1 lists a room's door
cells) and are written out as plain numbers.

    python3 tools/netaudit/make_localmp_recordings.py [--out tests/recordings]

The scenarios:

  localmp_tour     two humans from power-on (option localmp 2), menus off:
                   - boot into the tutorial room with player 2 idle, so player 2
                     is carried to player 1 by the straggler rule;
                   - real doors in two areas, led by each player in turn
                     (117 -> 118 by P1, 118 -> 117 by P2, 203 -> 205 by P2,
                     205 -> 203 by P1), the other following;
                   - play in two level rooms: floating, inhaling, swallowing
                     (abilities), enemies, damage; player 2 mirrors player 1
                     a quarter second late;
                   - the pause screen opened by player 1 and by player 2, with
                     Select cycling its pages;
                   - the cell phone (R) called by both players;
                   - Kracko's arena (a boss room) hands-off until the Kirbys
                     are beaten: damage, death and respawn with the group.
  localmp_menus    the in-game menus (needs KATAM_MENU_POC=1 natively, a page
                   with a location -- any browser -- on the web; see the
                   comment line it carries): Game Select's SETTINGS screen
                   with VOLUME and CAMERA changed; then START GAME >
                   MULTIPLAYER > LOCAL, which starts the file with two humans
                   from the menu (no `option localmp`); then the pause menu,
                   which opens on its SETTINGS page, by player 1 (Select
                   round all four pages, back to SETTINGS) and by player 2,
                   each changing a value there.

Grammar of the scripts below (as local-mp-eval's drive.py): `@F` jump to frame
F; `+N` advance N; a key name taps it (held 4 frames, advances 10); `hN:K`
holds K for N frames (advances N+6); `warp:ROOM:X:Y` (player 1's script only).
Keys: A B s(elect) S(tart) R(ight) L(eft) U D r (R button) l (L button).
"""
import argparse
from pathlib import Path

M = {'A': 1, 'B': 2, 's': 4, 'S': 8, 'R': 16, 'L': 32, 'U': 64, 'D': 128, 'r': 256, 'l': 512}


def mask(s):
    m = 0
    for ch in s:
        m |= M[ch]
    return m


def parse(script, f=0):
    """-> (key events [(frame, mask)], warps [(frame, room, x, y)], end frame)"""
    ks, ws = [], []
    for tok in script.split():
        if tok.startswith('@'):
            f = int(tok[1:])
        elif tok.startswith('+'):
            f += int(tok[1:])
        elif tok.startswith('warp:'):
            _, r, x, y = tok.split(':')
            ws.append((f, int(r), int(x), int(y)))
            f += 1
        elif tok.startswith('h') and ':' in tok:
            n, k = tok[1:].split(':')
            n = int(n)
            ks.append((f, mask(k)))
            ks.append((f + n, 0))
            f += n + 6
        else:
            ks.append((f, mask(tok)))
            ks.append((f + 4, 0))
            f += 10
    return ks, ws, f


def boot_keys(upto=1300):
    """tools/make_recordings.py boot(): A+Right on a 20-frame period from 120."""
    out, prev = [], None
    for f in range(upto):
        k = (1 | 16) if f >= 120 and (f - 120) % 20 < 10 else 0
        if k != prev:
            out.append((f, k))
            prev = k
    out.append((upto - 1, 0))
    return out


def play_keys(start, end, delay=0):
    """make_recordings.py play(): float right, Up for doors, A, B to inhale."""
    out, prev = [], None
    for f in range(start, end):
        p = (f - start) % 40
        m = 16
        if p < 4:
            m = 64
        elif 10 <= p < 24:
            m |= 1
        if (f - start) % 200 in range(150, 156):
            m = 2
        if m != prev:
            out.append((f + delay, m))
            prev = m
    out.append((end + delay, 0))
    return out


def walk_keys(start, end, delay=0):
    """Walk right on the ground: inhale (B held) every 80 frames, swallow
    (Down), a jump now and then -- enemies, abilities, damage."""
    out, prev = [], None
    for f in range(start, end):
        p = (f - start) % 80
        m = 16
        if 40 <= p < 64:
            m = 2
        elif 64 <= p < 68:
            m = 128
        elif p < 6 and ((f - start) // 80) % 3 == 2:
            m |= 1
        if m != prev:
            out.append((f + delay, m))
            prev = m
    out.append((end + delay, 0))
    return out


def write(path, frames, p1, p2, warps=(), views=(), options=(), comments=()):
    def collapse(events):
        d = {}
        for f, m in sorted(events, key=lambda e: e[0]):
            d[f] = m            # a later event at the same frame wins
        out, prev = [], None
        for f in sorted(d):
            if d[f] != prev:
                out.append((f, d[f]))
                prev = d[f]
        return out
    lines = ['katam-input 1']
    lines += ['# ' + c for c in comments]
    lines.append('# no-regress: a scenario for tools/netaudit/, not a tests/regress.sh golden')
    lines.append('# generated by tools/netaudit/make_localmp_recordings.py; inputs only, no game data')
    lines.append('frames %d' % frames)
    lines += list(options)
    for f, w, h in views:
        lines.append('view %d %d %d' % (f, w, h))
    for f, r, x, y in warps:
        lines.append('warp %d 0 %d %d %d' % (f, r, x, y))
    p1 = collapse(p1)
    if not p1 or p1[0][0] != 0:
        p1 = [(0, 0)] + p1
    for f, m in p1:
        lines.append('k %d %03X' % (f, m))
    for f, m in collapse(p2):
        lines.append('kp 1 %d %03X' % (f, m))
    lines.append('end')
    Path(path).write_text('\n'.join(lines) + '\n')


# --- localmp_tour ---------------------------------------------------------------

# Doors (local-mp-eval doors3.sh, which verified them): area 1 is rooms
# 117 <-> 118, area 2 rooms 203 <-> 205.
TOUR_P1_DOORS = """
@1300 warp:117:44:9 @1340 h8:R U U U U
@1440 h140:R @1590 h140:L
@1950 warp:203:60:5 @2060 h30:L U U U U
"""
TOUR_P2_DOORS = "@1750 h30:L U U U U @1990 h10:R U U U U @2150 h120:L"

PLAY = [(2400, 101, 3, 7), (3000, 104, 2, 9)]       # 600 frames each
PAUSE_AT = 3600         # in room 104
BOSS = (4300, 324, 4, 19)                          # Kracko's arena
TOUR_END = 7300


def tour(out):
    p1, w1, _ = parse(TOUR_P1_DOORS)
    p2, _, _ = parse(TOUR_P2_DOORS)
    p1 = boot_keys() + p1
    warps = list(w1)
    for i, (f, room, x, y) in enumerate(PLAY):
        warps.append((f, room, x, y))
        keys = play_keys if i == 0 else walk_keys
        p1 += keys(f + 1, f + 590)
        p2 += keys(f + 1, f + 590, delay=15)
    # The pause screen: player 1 opens it, cycles the pages with Select and
    # closes it; then player 2 does the same.  Then the cell phone, each.
    f = PAUSE_AT
    k, _, f = parse('S +40 s +30 s +30 s +30 S +60', f)
    p1 += k
    k, _, f = parse('S +40 s +30 s +30 s +30 S +60', f)
    p2 += k
    k, _, f = parse('r +120', f)
    p1 += k
    k, _, f = parse('r +120', f)
    p2 += k
    # The boss: float up into Kracko's arena to start the fight, then hands
    # off until the Kirbys have been beaten and come back.
    warps.append(BOSS)
    p1 += play_keys(BOSS[0] + 100, BOSS[0] + 500)
    p2 += play_keys(BOSS[0] + 100, BOSS[0] + 500, delay=15)
    write(out / 'localmp_tour.txt', TOUR_END, p1, p2, warps,
          views=[(0, 480, 320)], options=['option localmp 2'],
          comments=['two players from power-on: straggler, doors led by each player, play,',
                    'pause by each player, cell phone, a boss room with death and respawn'])


# --- localmp_menus -------------------------------------------------------------

MENUS_P1 = """
@400 D D D D A
@520 D D D D D L U U R +20 B
@700 U U U U A +40 D +20 A +60 A
@1300 warp:117:44:9
@1400 S +60 s +60 s +60 s +60 s +80 D D D D D L +40 B
@2450 h60:R h30:L
"""
MENUS_P2 = """
@1200 h40:R
@1950 S +100 D D D R +40 B
@2450 h60:L h30:R
"""
MENUS_END = 2600


def menus(out):
    p1 = boot_keys(282)
    k, w, _ = parse(MENUS_P1)
    p1 += k
    p2, _, _ = parse(MENUS_P2)
    write(out / 'localmp_menus.txt', MENUS_END, p1, p2, w,
          comments=['REQUIRES the in-game menus: KATAM_MENU_POC=1 natively; on the web a page',
                    'with a location (any browser; not ?menus=0).  Not recorded in this file.',
                    'Game Select SETTINGS screen, START GAME > MULTIPLAYER > LOCAL, then the',
                    'pause SETTINGS page by player 1 and by player 2, each changing a value'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=str(Path(__file__).resolve().parents[2] / 'tests' / 'recordings'))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    tour(out)
    menus(out)


if __name__ == '__main__':
    main()
