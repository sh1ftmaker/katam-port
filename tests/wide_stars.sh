#!/usr/bin/env bash
#
# wide_stars.sh -- the level warp star for Kirbys past eight, natively, in a
# katam16 build (docs/design/ONE-WORLD-16.md "Slot audit").
#
#   tests/wide_stars.sh WIDE16/katam
#
# The star keeps its riders in a u8, a bit a Kirby (warp_star.c STAR_MASK):
# a Kirby 8.. that boarded was never released and stayed in the star's room.
# One generated recording (inputs only): 16 local players in rooms mode, the
# story past the tutorial; Kirbys 5, 7, 9, 12 and 15 are put one after the
# other on the warp star of room 142 (cell 25,12), 15 holding the Master
# sword (its hat rows past 54, code_0806F780.c MASTER_HAT_ROW, are the ride's);
# Kirby 10, also with the Master sword, takes the cell phone's star home (L).
# Run twice with the render list built headless: each rider must end in room
# 195 (where the star goes) and 10 in the hub (801); no sanitizer report past
# the console's own (tools/wide_sanclass.py), no rom-check report, no sprite
# turned away, the two hash logs identical.  Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a KATAM_MAX_KIRBYS=16 native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

RIDERS="5 7 9 12 15"
REC=$TMP/stars.txt
{
    echo "katam-input 1"
    echo "frames 3600"
    echo "option localmp 16"
    echo "option rooms 1"
    echo "kirbys 0 16"
    echo "poke16 1200 203AD2C 12C"          # gAIKirbyState NORMAL: past the tutorial
    m=0
    for f in $(seq 0 1299); do               # the power-on taps (tools/netproof/scenario.mjs bootKey)
        v=0; [ "$f" -ge 120 ] && [ $(( (f - 120) % 20 )) -lt 10 ] && v=11
        [ "$v" != "$m" ] && echo "k $f $v" && m=$v
    done
    [ "$m" != 0 ] && echo "k 1300 0"
    echo "warp 1300 10 104 2 9"
    echo "ability 1340 10 26"
    echo "kp 10 1500 200"; echo "kp 10 1800 000"   # L: the warp star home
    echo "ability 1340 15 26"
    i=0
    for s in $RIDERS; do                     # one at a time: each boards a star of its own
        echo "warp $(( 1400 + 300 * i )) $s 142 25 12"
        i=$(( i + 1 ))
    done
    echo "end"
} >"$REC"

run() {  # out
    local d; d=$(mktemp -d -p "$TMP")
    KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1} \
        "$BIN" --no-audio --turbo --replay "$REC" --hash-log "$1.hash" "$ROM" >"$1.log" 2>&1
}
run "$TMP/a" & run "$TMP/b" & wait

status=0
fail() { echo "FAIL $*"; status=1; }
for r in a b; do
    grep -q "wide: kirbys 16" "$TMP/$r.log" || fail "run $r: not a 16-Kirby build or did not finish: $(tail -2 "$TMP/$r.log" | tr '\n' ' ')"
    grep -q "Segmentation\|SEGV" "$TMP/$r.log" && fail "run $r: $(grep -m1 'Segmentation\|SEGV' "$TMP/$r.log")"
    if grep -qE "ERROR: AddressSanitizer|runtime error:" "$TMP/$r.log"; then
        c=$(python3 "$HERE/../tools/wide_sanclass.py" "$TMP/$r.log") || fail "run $r: sanitizer: $(echo "$c" | head -3 | tr '\n' ' ')"
    fi
    grep -q "rom-check: animation" "$TMP/$r.log" && fail "run $r: $(grep -m1 'rom-check: animation' "$TMP/$r.log")"
    grep -q " not drawn -- " "$TMP/$r.log" && fail "run $r: $(grep -m1 ' not drawn -- ' "$TMP/$r.log")"
done
check() {  # slot room what
    local line; line=$(grep "at exit: player $1 in room" "$TMP/a.log")
    case $line in
        *"in room $2,"*) echo "ok   slot $1: $3" ;;
        *) fail "slot $1 ($3): ${line:-no exit report}" ;;
    esac
}
for s in $RIDERS; do check "$s" 195 "rode the warp star of room 142 to room 195"; done
check 10 801 "Master sword, the cell phone's star home"
if cmp -s "$TMP/a.hash" "$TMP/b.hash" && [ -s "$TMP/a.hash" ]; then
    echo "ok   two runs, $(wc -l <"$TMP/a.hash") frames, hash logs identical"
else
    fail "the two runs differ (or no hash log)"
fi
[ $status = 0 ] && echo "STARS PASS" || echo "STARS FAIL"
exit $status
