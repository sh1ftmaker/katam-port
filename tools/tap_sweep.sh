#!/usr/bin/env bash
#
# tap_sweep.sh -- how often does tap to move get there?  (docs/design/
# TAP-TO-MOVE.md, "Evidence")
#
#   ROM=/path/to/rom.gba tools/tap_sweep.sh build/lp64/katam [ROOM ...]
#
# For each room (default: the 32 of the warp_sweep recording, every area),
# a live native run: new game, story past the opening, player 1 warped in at
# frame 1400, then a tap every 900 frames on a random cell he can stand in
# and reach, 3..25 cells away (the devices script's `taprandom`,
# platform/ui/tap_nav.c).  Each tap ends as arrived, stuck (the watchdog gave
# up), left room (a door or an edge on the way), or timeout (not there when
# the next tap comes).  Prints a line per room and the totals.
#
# Known: a run can end early on a native-only crash in sir_kibble.c:530 (a
# cutter whose thrower is gone reads through NULL; the wide build guards it,
# the four-Kirby build does not).  That run's taps up to it still count.
set -uo pipefail

BIN=$(realpath "${1:?a native build}")
shift
ROM=${ROM:?set ROM=/path/to/rom.gba}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 8)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ROOMS="101:3:7 113:89:7 122:4:3 134:2:7 142:5:19 170:3:9 178:90:9 195:3:9 209:11:3 218:15:3
226:12:8 291:9:3 316:60:4 326:1:2 406:7:8 415:77:1 423:3:9 502:2:7 512:4:7 525:2:19 533:39:17
541:5:5 553:3:4 591:9:3 608:72:4 617:4:9 700:3:9 710:5:5 730:29:4 739:4:4 814:3:3 825:18:19"
if [ $# -gt 0 ]; then
    want=" $* "
    ROOMS=$(for r in $ROOMS; do case "$want" in *" ${r%%:*} "*) echo "$r" ;; esac; done)
fi

one() {  # room:x:y
    local r=${1%%:*} rest=${1#*:} f i d
    {
        echo "@1200 poke16 203AD2C 12C"
        echo "@1500 kb a A"
        echo "@1503 kb a 0"
        f=1800 i=1
        while [ $f -lt 9000 ]; do echo "@$f taprandom $((r * 100 + i))"; f=$((f + 900)); i=$((i + 1)); done
    } >"$TMP/$r.dev"
    d=$(mktemp -d -p "$TMP")
    KATAM_TAP=1 KATAM_TAP_LOG=1 KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy setarch -R "$BIN" --no-audio --turbo --frames 9000 \
        --mash 120:11:20:1300 --devices "$TMP/$r.dev" --warp "1400:$r:${rest%%:*}:${rest#*:}" "$ROM" \
        >"$TMP/$r.log" 2>&1
    rm -rf "$d"
}
export -f one
export BIN ROM TMP
printf '%s\n' $ROOMS | xargs -P "$JOBS" -I{} bash -c 'one {}' 2>/dev/null

python3 - "$TMP" <<'EOF'
import collections, glob, os, re, sys
tot = collections.Counter()
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.log')), key=lambda p: int(os.path.basename(p)[:-4])):
    c = collections.Counter(); cur = None; took = []
    for line in open(f, errors='replace'):
        m = re.match(r'\[tap\] f(\d+) (.*)', line)
        if not m:
            continue
        fr, msg = int(m.group(1)), m.group(2)
        if msg.startswith('taprandom room'):
            if cur is not None: c['timeout'] += 1
            cur = fr
        elif msg.startswith('taprandom: nothing'):
            c['no target'] += 1
        elif cur is not None:
            for key, out in (('arrived', 'arrived'), ('clear (stuck)', 'stuck'),
                             ('clear (new room)', 'left room'), ('clear (manual', 'other')):
                if msg.startswith(key):
                    c[out] += 1
                    if out == 'arrived': took.append(fr - cur)
                    cur = None
                    break
    if cur is not None: c['timeout'] += 1
    tot.update(c)
    n = sum(v for k, v in c.items() if k != 'no target')
    print('room %s: %d/%d arrived%s  %s' % (os.path.basename(f)[:-4], c['arrived'], n,
          ' (median %d frames)' % sorted(took)[len(took) // 2] if took else '',
          ' '.join('%s %d' % (k, v) for k, v in sorted(c.items()) if k != 'arrived')))
n = sum(v for k, v in tot.items() if k != 'no target')
print('TOTAL %d/%d arrived (%.0f%%)  %s' % (tot['arrived'], n, 100.0 * tot['arrived'] / max(1, n),
      ' '.join('%s %d' % (k, v) for k, v in sorted(tot.items()) if k != 'arrived')))
EOF
