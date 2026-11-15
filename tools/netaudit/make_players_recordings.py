#!/usr/bin/env python3
"""make_players_recordings.py -- drop-in / drop-out scenarios, recorded.

Each scenario is played LIVE by the native build with fake controllers
(`--devices`, platform/native/host_sdl.c: SDL virtual game controllers that
are plugged, pressed and unplugged on cue, the keyboard sets as devices) and
written out with `--record` as tests/recordings/players_*.txt: inputs only --
player 1's keys (`k`), players 2-4's (`kp`), the slot changes (`human FRAME
SLOT ON`), warps and the session view.  Nothing from the ROM is in them.

Generating needs your ROM and a native build, because some steps wait for
the game (`when 2 dead`: until Kirby slot 2 is dead) -- the recording then
holds the frame it happened at.  Replaying needs neither the script nor the
devices:

    python3 tools/netaudit/make_players_recordings.py --rom ROM [--bin B] [--out DIR] [NAME...]
    build/lp64/katam --replay tests/recordings/players_dropin.txt ROM

Each generated recording is replayed once and its per-frame hashes compared
with the live run's (keys, sim, vid, sram): the recording reproduces the run.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve()
ROOT = HERE.parents[2]

# Player 1's boot: tools/make_recordings.py's A+Right taps from frame 120
# (--mash), to the first level by 1300.  Player 1 touches the keyboard first
# (`kb a A` at 100), so the keyboard is player 1's device and a pad's Start
# is a join.
BOOT = ['--mash', '120:17:20:1300']
P1_KB = '@100 kb a A\n+5 kb a -\n'

SCENARIOS = {}


def scenario(name, frames, script, warps=(), view='480x320', env=None, comment=''):
    SCENARIOS[name] = dict(frames=frames, script=P1_KB + script, warps=warps, view=view,
                           env=env or {}, comment=comment)


# --- players_dropin: join, leave, rejoin, unplug, reassign; doors, pause, phone
scenario('players_dropin', 3600, """
@1350 plug Xbox 360 Controller
+30 pad 1 S              # pad 1: Start -> player 2 joins (slot 1)
+5 pad 1 -
+20 pad 1 R
+60 pad 1 -
+0 shot two
+10 plug Xbox One Controller
+30 pad 2 S              # player 3
+5 pad 2 -
+20 kb b S               # keyboard set B's Start -> player 4
+5 kb b -
+10 list
+0 shot four
+20 kb a R               # player 1 takes the door 117 -> 118; everyone follows
+8 kb a -
+6 kb a U
+4 kb a -
+6 kb a U
+4 kb a -
+120 kb b R              # player 4 walks right, player 3 floats
+0 pad 2 RA
+40 shot four-118
+40 kb b -
+0 pad 2 -
+10 pad 1 S              # player 2 pauses (their own pause menu)
+5 pad 1 -
+20 shot pause-p2
+20 pad 1 s
+5 pad 1 -
+60 pad 1 B              # B quits the pause screen
+5 pad 1 -
+80 pad 2 S              # player 3 pauses
+5 pad 2 -
+40 shot pause-p3
+20 pad 2 B
+5 pad 2 -
+80 kb b S               # player 4 pauses
+5 kb b -
+40 shot pause-p4
+20 kb b B
+5 kb b -
+80 shot unpaused
+40 dropout 2            # the middle player leaves; 3 and 4 stay
+10 list
+40 shot mid-leave
+20 pad 2 r              # player 3 calls on the cell phone (the CPU in slot 1 answers)
+5 pad 2 -
+40 shot phone
+160 pad 1 S             # player 2 rejoins from the parked pad
+5 pad 1 -
+10 list
+40 shot rejoined
+10 pad 2 R               # players 3 and 4 spread out: the camera zooms out to fit
+0 kb b L
+70 shot spread
+0 pad 2 -
+0 kb b -
+20 unplug pad2          # player 3's pad unplugged: grace, then the CPU
+240 list
+10 plug Xbox One Controller
+20 pad 3 S              # plugged back in: Start rejoins as player 3
+5 pad 3 -
+10 list
+30 assign kbb 2         # keyboard set B moved from player 4 to player 2
+10 list
+30 kb a L               # player 1 takes the door back 118 -> 117
+140 kb a -
+6 kb a U
+4 kb a -
+6 kb a U
+4 kb a -
+200 dropout 4
+10 dropout 3
+10 dropout 2
+10 list
""", warps=['1300:117:44:9'],
    comment='joins by pad Start and keyboard set B, a middle player leaving while 3 and 4 '
            'stay, rejoin, pause by players 2-4, the phone, unplug + grace, replug, reassign')

# --- players_boss: Kracko with the count changing mid-fight; deaths; lives
scenario('players_boss', 14000, """
@1200 poke16 0203AD2C 300  # gAIKirbyState = NORMAL: the CPUs are partners, not the intro's
@1350 plug Xbox 360 Controller
+30 pad 1 S              # player 2 joins in the tutorial room
+5 pad 1 -
@1700 kb a U             # (warp to Kracko's arena at 1600) float up to start the fight
+200 kb a -
+0 when 0 boss
+200 plug Xbox One Controller
+30 pad 2 S              # player 3 joins mid-fight: boss HP rescaled 2 -> 3
+5 pad 2 -
+60 shot fight-3
+240 dropout 2           # player 2 leaves mid-fight: 3 -> 2
+0 when 1 dead           # the CPU in slot 1 (player 2 just left) is beaten ...
+0 assign kbb 2          # ... and becomes player 2 again while dead
+10 list
+60 shot dead-joined
+600 pad 1 S             # the pad joins as player 4
+5 pad 1 -
+10 list
""", warps=['1600:324:4:19'],
    comment="Kracko's arena: a join and a leave mid-fight (boss HP kept in proportion), "
            'a CPU taken over while dead, hands off until beaten: deaths, respawns, lives')

# --- players_rooms: joins into a Kirby in another room / mid-door
scenario('players_rooms', 3000, """
@1250 plug Xbox 360 Controller
+0 plug Xbox One Controller
+0 plug Wireless Controller
@1301 when 1 away        # (player 1 warped to 118 at 1300) the CPU in slot 1 is
+0 assign pad1 2         # still in 117: it becomes player 2 there
+10 list
+30 shot carried
+60 kb a L
+60 kb a -
+0 kb a U                # player 1 takes the door 118 -> 117 ...
+5 kb a -
+0 when 2 door           # ... and the CPU in slot 2 follows: mid-door it becomes player 3
+0 assign pad2 3
+10 list
+60 pad 2 A               # the new player 3 presses something
+5 pad 2 -
+60 pad 2 R
+30 pad 2 -
+60 shot door-joined
+200 when 3 door
+0 assign pad3 4
+300 list
""", warps=['1300:118:3:5'],
    comment='during the intro (gAIKirbyState TUTORIAL): a join of a Kirby in another room applies at once; one the intro is walking into the hub waits')

# --- players_rooms_normal: joins into a Kirby in another room / mid-door
scenario('players_rooms_normal', 3000, """
@1200 poke16 0203AD2C 300  # past the intro: the CPUs are partners
@1250 plug Xbox 360 Controller
+0 plug Xbox One Controller
+0 plug Wireless Controller
@1301 when 1 away        # (player 1 warped to 118 at 1300) the CPU in slot 1 is
+0 assign pad1 2         # still in 117: it becomes player 2 there
+10 list
+30 shot carried
+60 kb a L
+60 kb a -
+0 kb a U                # player 1 takes the door 118 -> 117 ...
+5 kb a -
+0 when 2 door           # ... and the CPU in slot 2 follows: mid-door it becomes player 3
+0 assign pad2 3
+10 list
+60 pad 2 A               # the new player 3 presses something
+5 pad 2 -
+60 pad 2 R
+30 pad 2 -
+60 shot door-joined
+200 when 3 door
+0 assign pad3 4
+300 list
""", warps=['1300:118:3:5'],
    comment='as players_rooms, with the CPUs as partners (gAIKirbyState NORMAL): joins in another room and mid-door apply at once')

# --- players_goal: three people on the goal star (it waits for every person)
scenario('players_goal', 2400, """
@1250 plug Pad A
+0 plug Pad B
@1380 pad 1 S            # players 2 and 3 join in the tutorial room
+5 pad 1 -
+20 pad 2 S
+5 pad 2 -
@1440 shot waiting       # (warp to the goal room at 1420) player 1 rides; the star waits
@1500 pad 1 RU           # players 2 and 3 float up and board
+0 pad 2 RU
+12 pad 1 U
+0 pad 2 U
+50 pad 1 -
+0 pad 2 -
+40 shot boarded
+160 shot flying
+0 kb b S                # keyboard set B joins mid-flight: the story's Kirby, held
+5 kb b -
+300 list
""", warps=['1300:117:44:9', '1420:980:7:70'],
    comment='the goal star with three people: it waits until every person in the room '
            'rides, then carries all three; a join mid-flight')

# --- players_local: MULTIPLAYER > LOCAL's start (option localmp 2), then leaves
scenario('players_local', 2400, """
@1350 plug Xbox 360 Controller   # player 2 is a person with no controller: this pad is theirs
+60 pad 1 R
+30 shot local
+30 pad 1 -
+60 dropout 2                    # player 2 leaves: the slot's CPU brain takes over
+120 kb b S                      # and rejoins with the parked keyboard set B (the pad
+5 kb b -                        # drove player 1, so it is player 1's: its Start pauses)
+120 dropout 2
+10 list
""", warps=['1300:117:44:9'], env={'KATAM_LOCAL_MP': '2'},
    comment='two humans from the start (option localmp 2: what MULTIPLAYER > LOCAL does), '
            'the pad given to player 2, player 2 leaving and coming back')


def run(bin_, rom, args, env, log):
    d = tempfile.mkdtemp(prefix='players-')
    e = dict(os.environ)
    e.update(XDG_DATA_HOME=d + '/data', XDG_CONFIG_HOME=d + '/config',
             SDL_VIDEODRIVER=e.get('SDL_VIDEODRIVER', 'dummy'), SDL_AUDIODRIVER='dummy')
    e.update(env)
    cmd = ['setarch', '-R', bin_, '--no-audio', '--turbo'] + args + [rom]
    with open(log, 'w') as f:
        subprocess.run(cmd, env=e, stdout=f, stderr=subprocess.STDOUT, check=False)


def hashes(path):
    out = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 6:
                out.append(tuple(p[:6]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--bin', default=str(ROOT / 'build' / 'lp64' / 'katam'))
    ap.add_argument('--out', default=str(ROOT / 'tests' / 'recordings'))
    ap.add_argument('--work', default=None, help='logs, hash logs and screenshots go here')
    ap.add_argument('--shots', action='store_true', help='also replay with the GPU renderer '
                    '(SDL offscreen) and write the scenario screenshots into --work')
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    work = Path(a.work or tempfile.mkdtemp(prefix='players-work-'))
    work.mkdir(parents=True, exist_ok=True)
    ok = True
    for name in a.names or SCENARIOS:
        sc = SCENARIOS[name]
        dev = work / (name + '.devices')
        dev.write_text(sc['script'])
        rec = work / (name + '.live.txt')
        args = BOOT + ['--devices', str(dev), '--record', str(rec), '--frames', str(sc['frames']),
                       '--hash-log', str(work / (name + '.live.hash'))]
        if sc['view']:
            args += ['--sim-view', sc['view']]
        for w in sc['warps']:
            args += ['--warp', w]
        run(a.bin, a.rom, args, sc['env'], work / (name + '.live.log'))
        text = rec.read_text().split('\n')
        head = ['katam-input 1',
                '# ' + name + ': ' + sc['comment'],
                '# no-regress: a scenario for tools/netaudit/ and the drop-in tests, not a golden',
                '# generated by tools/netaudit/make_players_recordings.py (live, fake controllers);',
                '# inputs only, no game data']
        body = [l for l in text[1:] if l and not l.startswith('#')]
        out = Path(a.out) / (name + '.txt')
        out.write_text('\n'.join(head + body) + '\n')
        # The recording must reproduce the live run.
        run(a.bin, a.rom, ['--replay', str(out), '--hash-log', str(work / (name + '.replay.hash'))],
            {}, work / (name + '.replay.log'))
        live = hashes(work / (name + '.live.hash'))
        rep = hashes(work / (name + '.replay.hash'))
        n = min(len(live), len(rep))
        bad = next((i for i in range(n) if live[i] != rep[i]), None)
        slots = sum(1 for l in body if l.startswith('human '))
        if bad is None and n >= sc['frames'] - 2:
            print('%-16s OK  %d frames, %d slot changes, replay == live' % (name, n, slots))
        else:
            ok = False
            print('%-16s BAD replay differs from live at frame %s (%d/%d frames)'
                  % (name, bad, len(rep), len(live)))
        # `shot LABEL` steps in the script: screenshots of the same frames,
        # replayed with the GPU renderer (SDL offscreen) at 960x640.
        shots = re.findall(r'devices-script: f(\d+) shot (\S+)',
                           (work / (name + '.live.log')).read_text())
        if a.shots and shots:
            sargs = ['--replay', str(out), '--ui', '--window', '960x640', '--frames',
                     str(max(int(f) for f, _ in shots) + 2)]
            for f, label in shots:
                sargs += ['--shot-at', '%s:%s' % (f, work / (name + '-' + label))]
            run(a.bin, a.rom, sargs, {'SDL_VIDEODRIVER': 'offscreen'}, work / (name + '.shots.log'))
    print('work files in', work)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
