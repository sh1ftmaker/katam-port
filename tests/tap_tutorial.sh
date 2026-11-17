#!/usr/bin/env bash
#
# tap_tutorial.sh -- every action the tutorial teaches, with tap to move only
# (docs/design/TAP-TO-MOVE.md, "The tutorial, lesson by lesson").
#
#   tests/tap_tutorial.sh build/lp64/katam
#
# Run 1, the tutorial as a new game plays it: the real opening (no story
# poke, no warp), the page's default CPU AI mix (KATAM_CPU_AI=E79E79E4: the
# tutorial must stay the classic brain's), tap controls on, and only
# fingers -- the devices script's tap commands (platform/ui/tap_nav.c
# PortTapScript, a test hook, never recorded) -- except A on the
# "Using L & R" card, where the page shows its pads (no HUD, no taps):
#
#   803  the star blocks under the pillar: tapped, broken (inhaled, or a
#        helper's hit); the door at the end -> 804
#   804  a switch tapped: stood on, down (the first sliding door opens); the
#        second switch, down in its pit; the bomb block tapped: broken (its
#        chain of metal blocks goes); the heavy star block tapped: a strong
#        inhale (B held) moves it off its ledge; the door -> 805
#   805  the Sword Knight tapped: inhaled; a swipe down: swallowed (Sword);
#        three block columns tapped: broken; the big chest tapped: walked
#        into, Up, opened (the map; the metal wall goes); the mirror tapped:
#        Up -> the hub (801)
#
# The live run's recording must replay with identical hash logs.
#
# Run 2, the gestures the tutorial's cards name, in Rainbow Route 101 (story
# past the opening): a finger held on Kirby inhales (B held); a diagonal
# swipe from Kirby slides (a ground kick: he moves); a swipe up calls for
# help (R: a battery bar goes).  Run 3, the same room, nothing else first:
# two fingers held are L, held -- "hold L to start over" -- which takes him
# back to the hub (48 frames of L; a hit restarts the count, hence a run of
# its own).  Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cat >"$TMP/tutorial.dev" <<'EOF'
@1400 tapcell 44 6
@2200 tapcell 45 6
@2700 tapcell 73 3
@3300 tapswitch
@3700 tapcell 16 4
@3900 tapswitch
@4400 tapcell 27 4
@4700 tapcell 30 4
@5400 tappush
@6000 tapcell 56 2
@6450 tapenemy
@6750 swipekirby
@6800 dumpgrid
@6850 tapcell 11 7
@7300 tapcell 25 6
@7700 tapcell 26 7
@8100 tapcell 60 6
@8500 tapcell 67 6
@8900 tapcell 72 5
@9100 kb a A
@9106 kb a 0
@9300 kb a A
@9306 kb a 0
@9500 kb a A
@9506 kb a 0
EOF

cat >"$TMP/gestures.dev" <<'EOF'
@1200 poke16 203AD2C 12C
@1500 kb a A
@1503 kb a 0
@1640 dumpgrid
@1650 downkirby
@1720 upkirby
@1790 dumpgrid
@1800 kickkirby R
@1850 dumpgrid
@1900 swipeupkirby
@2000 dumpgrid
EOF

cat >"$TMP/hold_l.dev" <<'EOF'
@1200 poke16 203AD2C 12C
@1500 kb a A
@1503 kb a 0
@1650 downkirby
@1654 down2
@1800 up2
@1800 upkirby
EOF

env_run() {  # dir, then the command
    local d=$1; shift
    XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config KATAM_RL_HEADLESS=1 \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy setarch -R "$@"
}

d=$(mktemp -d -p "$TMP")
KATAM_TAP=1 KATAM_TAP_LOG=1 KATAM_CPU_AI=E79E79E4 KATAM_CPUAI_LOG=1 env_run "$d" "$BIN" --no-audio --turbo \
    --frames 9800 --mash 120:11:20:1300 --devices "$TMP/tutorial.dev" \
    --record "$TMP/live.rec" --hash-log "$TMP/live.hash" "$ROM" >"$TMP/tutorial.log" 2>&1 &
d2=$(mktemp -d -p "$TMP")
KATAM_TAP=1 KATAM_TAP_LOG=1 KATAM_CPUAI_LOG=1 env_run "$d2" "$BIN" --no-audio --turbo --frames 2600 \
    --mash 120:11:20:1300 --devices "$TMP/gestures.dev" --warp 1400:101:3:7 "$ROM" >"$TMP/gestures.log" 2>&1 &
d3=$(mktemp -d -p "$TMP")
KATAM_TAP=1 KATAM_TAP_LOG=1 KATAM_CPUAI_LOG=1 env_run "$d3" "$BIN" --no-audio --turbo --frames 2100 \
    --mash 120:11:20:1300 --devices "$TMP/hold_l.dev" --warp 1400:101:3:7 "$ROM" >"$TMP/hold_l.log" 2>&1 &
wait
d=$(mktemp -d -p "$TMP")
env_run "$d" "$BIN" --no-audio --turbo --replay "$TMP/live.rec" --hash-log "$TMP/replay.hash" \
    "$ROM" >"$TMP/replay.log" 2>&1

status=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; status=1; }
T=$TMP/tutorial.log
G=$TMP/gestures.log
has() {  # label file pattern [min count]
    local n; n=$(grep -c -- "$3" "$2")
    [ "$n" -ge "${4:-1}" ] && ok "$1" || fail "$1 (\"$3\": $n)"
}
frame_of() { grep -m1 -- "$2" "$1" | sed -n 's/.* f\([0-9]*\) .*/\1/p'; }

# 803
d803=$(frame_of "$T" "door taken: room 803 -> 804")
n=$(awk -v e="${d803:-0}" '/\[tap\] f[0-9]+ block [0-9]+,[0-9]+ broken/ { f = substr($2, 2) + 0; if (f < e) n++ } END { print n + 0 }' "$T")
[ "$n" -ge 1 ] && ok "803: the star blocks under the pillar, tapped: $n broken by Kirby" \
    || fail "803: no tapped block broke before the door"
[ -n "$d803" ] && ok "803: the door -> 804 (f$d803)" || fail "803: never took the door to 804"
# 804
has "804: two floor switches tapped, stood on, down" "$T" "switch down (type 0x6D, stood on)" 2
has "804: the bomb block tapped, broken" "$T" "block 30,4 broken"
has "804: the heavy star block tapped, moved by a strong inhale" "$T" "moved the block"
has "804: the door -> 805" "$T" "door taken: room 804 -> 805"
# 805
has "805: the Sword Knight tapped, inhaled" "$T" "enemy gone (inhaled)"
ab=$(grep "\[tap\] grid room 805" "$T" | head -1 | sed -n 's/.* ability \([0-9]*\).*/\1/p')
[ -n "$ab" ] && [ "$ab" != 0 ] && ok "805: swipe down, swallowed (ability $ab)" \
    || fail "805: no ability after the swipe (${ab:-?})"
n=$(grep "\[tap\] f[0-9]* block [0-9]*,[0-9]* broken" "$T" | awk -v s="$(frame_of "$T" "door taken: room 804 -> 805")" '{ if (substr($2, 2) + 0 > s) n++ } END { print n + 0 }')
[ "$n" -ge 3 ] && ok "805: three block columns tapped, broken ($n)" || fail "805: $n blocks broken (want 3)"
has "805: the big chest tapped: Up, opened" "$T" "chest opened"
has "805: the mirror tapped: Up -> the hub" "$T" "door taken: room 805 -> 801"
hub=$(frame_of "$T" "door taken: room 805 -> 801")
n=$(awk -v e="${hub:-999999}" '/\[cpuai\] f[0-9]+ k[0-9]/ { if (substr($2, 2) + 0 < e) n++ } END { print n + 0 }' "$T")
[ "$n" = 0 ] && ok "the CPU AI mix asked for, and the tutorial was the classic brain's (no mode ran before the hub)" \
    || fail "a CPU AI mode ran in the tutorial ($n brain notes)"
if [ -s "$TMP/live.hash" ] && cmp -s "$TMP/live.hash" "$TMP/replay.hash"; then
    ok "replay: $(wc -l <"$TMP/live.hash") frames, the live run's hash log replays identically"
else
    fail "replay: the hash logs differ (or none)"
fi

# the gestures
has "hold on Kirby: inhale (B held)" "$G" "kirby: hold (inhale)"
x0=$(grep "\[tap\] grid room" "$G" | sed -n 2p | sed -n 's/.* kirby \([0-9]*\),.*/\1/p')
x1=$(grep "\[tap\] grid room" "$G" | sed -n 3p | sed -n 's/.* kirby \([0-9]*\),.*/\1/p')
[ -n "$x0" ] && [ -n "$x1" ] && [ $((x1 - x0)) -ge 24 ] && ok "diagonal swipe: a slide (a ground kick), $x0 -> $x1" \
    || fail "diagonal swipe: no slide (${x0:-?} -> ${x1:-?})"
b0=$(grep "\[tap\] grid room" "$G" | sed -n 3p | sed -n 's/.* battery \([0-9]*\).*/\1/p')
b1=$(grep "\[tap\] grid room" "$G" | sed -n 4p | sed -n 's/.* battery \([0-9]*\).*/\1/p')
[ -n "$b0" ] && [ -n "$b1" ] && [ "$b1" -lt "$b0" ] && ok "swipe up: called for help (R; battery $b0 -> $b1)" \
    || fail "swipe up: no call (battery ${b0:-?} -> ${b1:-?})"
L=$TMP/hold_l.log
has "two fingers held: L" "$L" "two fingers held: L"
r=$(grep "\[cpuai-obs\] f[0-9]* k0 room" "$L" | tail -1 | awk '{print $NF}')
[ "$r" = 801 ] && ok "two fingers held: start over (back in the hub, $r)" || fail "two fingers held: in room ${r:-?}, not 801"

[ $status = 0 ] && echo "TAP TUTORIAL PASS" || { echo "TAP TUTORIAL FAIL"; grep "\[tap\] f" "$T" | tail -30; }
exit $status
