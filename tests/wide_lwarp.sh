#!/usr/bin/env bash
#
# wide_lwarp.sh -- the cell phone's warp star home (L held in a level) for
# Kirbys past the console's four, natively, all sixteen of a katam16 build.
#
#   tests/wide_lwarp.sh WIDE16/katam
#
# WIDE16 is a native build with KATAM_MAX_KIRBYS=16 (an ASan build finds the
# first bad read).  The ride's sprites take a per-player variant and the ROM
# has four (kirby.c sub_0803F790): before the fix a Kirby 4.. with an ability,
# and any Kirby 8.., read past its animation's data.  The online side, 8
# devices, is tools/netproof/lwarp_test.mjs; this one reaches the slots no
# online room has (8..15) with local players.
#
# One generated recording (inputs only): 16 local players, the story past
# the tutorial, riders warped into level rooms of their own, some given an
# ability (a poke16 into the wide area), each holding L.  Run twice with the
# GPU renderer's render list built headless (KATAM_RL_HEADLESS, as the
# page's ENHANCED renderer): every rider must end in the hub (room 801), no
# sprite turned away by the renderer's safety net, no sanitizer report, and
# the two runs' hash logs identical.  Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a KATAM_MAX_KIRBYS=16 native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# slot ability room x y  (ability 0: none; 1 fire, 2 ice, 9 the six-variant one)
RIDERS="4 1 104 2 9
9 0 202 3 4
12 1 101 3 7
15 9 402 4 4
6 2 502 2 7"
KIRBY=$(( 0x02040000 + (((0xB4 + 4130 * 16) + 0xFFF) & ~0xFFF) ))   # port/wide.h WIDE_BASE: gKirbys
REC=$TMP/lwarp.txt
{
    echo "katam-input 1"
    echo "frames 3200"
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
    i=0
    while read -r s a room x y; do
        at=$(( 1300 + 40 * i )); hold=$(( 1600 + 120 * i ))
        echo "warp $at $s $room $x $y"
        [ "$a" != 0 ] && printf 'poke16 %d %X %X\n' $(( at + 50 )) $(( KIRBY + s * 0x1A8 + 0x102 )) $(( (a << 8) | 2 ))
        echo "kp $s $hold 200"; echo "kp $s $(( hold + 300 )) 0"
        i=$(( i + 1 ))
    done <<<"$RIDERS"
    echo "end"
} >"$REC"

run() {  # out
    local d; d=$(mktemp -d -p "$TMP")
    KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0} \
        "$BIN" --no-audio --turbo --replay "$REC" --hash-log "$1.hash" "$ROM" >"$1.log" 2>&1
}
run "$TMP/a" & run "$TMP/b" & wait

status=0
fail() { echo "FAIL $*"; status=1; }
for r in a b; do
    grep -q "wide: kirbys 16" "$TMP/$r.log" || fail "run $r: not a 16-Kirby build or did not finish: $(tail -2 "$TMP/$r.log" | tr '\n' ' ')"
    grep -q "render list built headless" "$TMP/$r.log" || fail "run $r: the render list was not built"
    grep -qE "ERROR: AddressSanitizer|runtime error:|Segmentation" "$TMP/$r.log" && fail "run $r: $(grep -m1 -E 'ERROR: AddressSanitizer|runtime error:|Segmentation' "$TMP/$r.log")"
    grep -q " not drawn -- " "$TMP/$r.log" && fail "run $r: $(grep -m1 ' not drawn -- ' "$TMP/$r.log")"
done
while read -r s a room x y; do
    line=$(grep "at exit: player $s in room" "$TMP/a.log")
    case $line in
        *"in room 801,"*) echo "ok   slot $s (ability $a) from room $room: home in the hub" ;;
        *) fail "slot $s (ability $a) from room $room: ${line:-no exit report}" ;;
    esac
done <<<"$RIDERS"
if cmp -s "$TMP/a.hash" "$TMP/b.hash" && [ -s "$TMP/a.hash" ]; then
    echo "ok   two runs, $(wc -l <"$TMP/a.hash") frames, hash logs identical"
else
    fail "the two runs differ (or no hash log)"
fi
[ $status = 0 ] && echo "LWARP PASS" || echo "LWARP FAIL"
exit $status
