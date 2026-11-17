#!/usr/bin/env bash
#
# tap_nav.sh -- tap to move (docs/design/TAP-TO-MOVE.md), natively.
#
#   tests/tap_nav.sh build/lp64/katam
#
# A live run, not a replay: the tap controls on (KATAM_TAP=1), the boot from
# --mash (the power-on taps tests/cpu_ai.sh records), the story set past the
# opening, player 1 warped to the hub (room 801) at frame 1400, and then
# fingers on the world through the devices script's tap commands
# (platform/ui/tap_nav.c PortTapScript -- a test hook, never recorded):
#
#   hub 801      the ground below (12,18); the high platform (15,4), which
#                needs a flight; the mirror door (19,14) -> Rainbow Route 101
#   101          a point (12,8); the nearest enemy (a Waddle Doo): inhaled;
#                a swipe down on Kirby: swallowed, an ability; the far door
#                (83,8) across the hill, the column and both pits -> 106
#   106          the lower door (76,7), across the room -> 107
#
# Then the live run's own recording is replayed, without and with the tap
# controls on: all three hash logs must be identical (the autopilot's keys
# are recorded as keys; a replay needs nothing of it), and the recording
# holds keys only.  Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FRAMES=6000

cat >"$TMP/taps.dev" <<'EOF'
@1200 poke16 203AD2C 12C
@1500 kb a A
@1503 kb a 0
@1600 tapcell 12 18
@1900 tapcell 15 4
@2300 tapcell 19 14
@2600 tapcell 12 8
@2800 tapenemy
@2900 swipekirby
@2950 dumpgrid
@3000 tapcell 83 8
@4500 tapcell 76 7
EOF

env_run() {  # dir, then the command
    local d=$1; shift
    XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config KATAM_RL_HEADLESS=1 \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy setarch -R "$@"
}

d=$(mktemp -d -p "$TMP")
KATAM_TAP=1 KATAM_TAP_LOG=1 env_run "$d" "$BIN" --no-audio --turbo --frames $FRAMES \
    --mash 120:11:20:1300 --devices "$TMP/taps.dev" --warp 1400:801:24:12 \
    --record "$TMP/live.rec" --hash-log "$TMP/live.hash" "$ROM" >"$TMP/live.log" 2>&1
d=$(mktemp -d -p "$TMP")
env_run "$d" "$BIN" --no-audio --turbo --replay "$TMP/live.rec" --hash-log "$TMP/replay.hash" \
    "$ROM" >"$TMP/replay.log" 2>&1
d=$(mktemp -d -p "$TMP")
KATAM_TAP=1 env_run "$d" "$BIN" --no-audio --turbo --replay "$TMP/live.rec" --hash-log "$TMP/replay-tap.hash" \
    "$ROM" >"$TMP/replay-tap.log" 2>&1

status=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; status=1; }
L=$TMP/live.log

# arrived ROOM TX TY TOL: an arrival in ROOM within TOL px of the target
arrived() {
    awk -v r="$1" -v tx="$2" -v ty="$3" -v tol="$4" '
        $3 == "arrived" && $5 == r {
            split($7, k, ","); dx = k[1] - tx; dy = k[2] - ty
            if (dx < 0) dx = -dx; if (dy < 0) dy = -dy
            if (dx <= tol && dy <= tol) { print $2, $7, $(NF-1); found = 1; exit }
        }
        END { exit !found }' "$L"
}
check_arrival() {  # label room tx ty
    local a
    if a=$(arrived "$2" "$3" "$4" 8); then ok "$1: Kirby at the target ($a frames)"
    else fail "$1: never within 8 px of $3,$4 in room $2"; fi
}
check_door() {  # from to
    local f
    f=$(grep -m1 "\[tap\] f[0-9]* door taken: room $1 -> $2" "$L" | awk '{print $2}')
    [ -n "$f" ] && ok "door: room $1 -> $2 ($f)" || fail "door: never went from room $1 to $2"
}

check_arrival "hub: the ground below" 801 200 296
check_arrival "hub: the high platform (a flight)" 801 248 72
check_door 801 101
check_arrival "101: a point along the ground" 101 200 136
grep -q "\[tap\] f[0-9]* enemy gone (inhaled)" "$L" && ok "101: the marked enemy was inhaled" \
    || fail "101: the marked enemy was not inhaled"
ab=$(grep "\[tap\] grid room 101" "$L" | head -1 | sed -n 's/.* ability \([0-9]*\).*/\1/p')
[ -n "$ab" ] && [ "$ab" != 0 ] && ok "101: swipe down swallowed it (ability $ab)" \
    || fail "101: no ability after the swipe (${ab:-?})"
check_door 101 106
check_door 106 107

if [ -s "$TMP/live.hash" ] && cmp -s "$TMP/live.hash" "$TMP/replay.hash" && cmp -s "$TMP/live.hash" "$TMP/replay-tap.hash"; then
    ok "replay: $(wc -l <"$TMP/live.hash") frames, the live run's hash log replays identically (tap controls off and on)"
else
    fail "replay: the hash logs differ (or none): $(wc -l <"$TMP/live.hash" 2>/dev/null) / $(wc -l <"$TMP/replay.hash" 2>/dev/null) / $(wc -l <"$TMP/replay-tap.hash" 2>/dev/null) lines"
fi
if grep -qi "tap" "$TMP/live.rec"; then
    fail "recording: mentions taps (it should hold keys only)"
else
    ok "recording: keys only ($(grep -c '^k ' "$TMP/live.rec") key lines, no taps)"
fi
[ $status = 0 ] && echo "TAP NAV PASS" || { echo "TAP NAV FAIL"; grep "\[tap\]" "$L" | grep -v "^\[tap\] *[0-9]* " | tail -40; }
exit $status
