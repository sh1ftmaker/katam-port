#!/usr/bin/env bash
#
# wide_ghosts.sh -- Kirbys past four in another room are not drawn on the
# screen, natively, in a katam16 build (docs/design/ONE-WORLD-16.md "Slot
# audit", kirby.c sub_0803F46C).
#
#   tests/wide_ghosts.sh WIDE16/katam
#
# The HUD hides each other Kirby not in the screen's player's room (hud.c
# DrawHudOtherKirbys sets 0x80000 on its sprites), for Kirbys 0..3 only; 4..
# were drawn in every room, at their own room's coordinates against the
# screen's camera ("player 14 and 16 can see each other as if they're on the
# same map").  A generated recording (inputs only), 16 local players in rooms
# mode: player 1 and Kirbys 2 and 5 in room 102, Kirbys 1, 4, 8, 12, 13 and 15
# in room 104, the rest left in the hub.  The render list's Kirby probe
# (KATAM_RL_KIRBYPROBE) counts the Kirby sprites drawn while their Kirby is in
# another room than the screen's player: none.  Kirby 5, in player 1's room,
# is still drawn (on the screen at the end: Kirbys 1, 3 and 6).
# Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a KATAM_MAX_KIRBYS=16 native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

{
    echo "katam-input 1"
    echo "frames 2400"
    echo "option localmp 16"
    echo "option rooms 1"
    echo "kirbys 0 16"
    echo "poke16 1200 203AD2C 12C"      # gAIKirbyState NORMAL: past the tutorial
    m=0
    for f in $(seq 0 1299); do           # the power-on taps (tools/netproof/scenario.mjs bootKey)
        v=0; [ "$f" -ge 120 ] && [ $(( (f - 120) % 20 )) -lt 10 ] && v=11
        [ "$v" != "$m" ] && echo "k $f $v" && m=$v
    done
    [ "$m" != 0 ] && echo "k 1300 0"
    echo "warp 1400 0 102 28 17"
    echo "warp 1404 2 102 30 17"
    echo "warp 1408 5 102 32 17"
    i=0
    for s in 1 4 8 12 13 15; do
        echo "warp $(( 1420 + 4 * i )) $s 104 $(( 2 + i )) 9"
        i=$(( i + 1 ))
    done
    echo "end"
} >"$TMP/ghosts.txt"

d=$(mktemp -d -p "$TMP")
KATAM_RL_HEADLESS=1 KATAM_RL_KIRBYPROBE=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0} \
    "$BIN" --no-audio --turbo --replay "$TMP/ghosts.txt" "$ROM" >"$TMP/ghosts.log" 2>&1

status=0
fail() { echo "FAIL $*"; status=1; }
L=$TMP/ghosts.log
grep -q "wide: kirbys 16" "$L" || fail "not a 16-Kirby build or did not finish: $(tail -2 "$L" | tr '\n' ' ')"
w=$(grep -c "warp: player [0-9]* -> room 10[24] " "$L")
[ "$w" = 9 ] || fail "$w of 9 warps delivered"
g=$(grep -o "sprites of Kirbys in another room than the screen's player [0-9]* (Kirbys mask 0x[0-9A-F]*); on the screen the last frame: mask 0x[0-9A-F]*" "$L")
n=$(echo "$g" | sed -n 's/.*player \([0-9]*\) .*/\1/p')
if [ -z "$g" ]; then
    fail "no Kirby probe report"
elif [ "$n" = 0 ]; then
    echo "ok   no Kirby drawn while in another room than player 1 (1, 4, 8, 12, 13, 15 in room 104, the rest in the hub)"
else
    fail "Kirby sprites drawn from another room: $g"
fi
seen=$(echo "$g" | sed -n 's/.*last frame: mask \(0x[0-9A-F]*\).*/\1/p')
if [ "$seen" = 0x25 ]; then
    echo "ok   Kirbys 1, 3 and 6, in player 1's room, drawn on the screen (mask $seen)"
else
    fail "Kirbys on the screen at the end: mask ${seen:-?}, want 0x25 (Kirbys 1, 3, 6)"
fi
[ $status = 0 ] && echo "GHOSTS PASS" || echo "GHOSTS FAIL"
exit $status
