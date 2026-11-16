#!/usr/bin/env bash
#
# wide_blocks.sh -- star stone blocks moved by Kirbys past four, natively, in
# a katam16 build (docs/design/ONE-WORLD-16.md "Slot audit").
#
#   tests/wide_blocks.sh WIDE16/katam
#
# A block counts the Kirbys in contact with it from obj.unk90, 2 bits a Kirby
# in a u8 (Kirbys 0..3); the wide build keeps 4..'s beside it
# (star_stone_block.h portContactHi).  The contact is Kirby's inhale (one
# push, two once the inhale is strong); the block slides toward the inhaler,
# opposite sides cancel, and it stops where a contacting Kirby is in the way
# (unkE3 0x10/0x20).  The block of room 102 (Radish Ruins, at 488,279; a
# Kirby at cell 28,17 or 27,17 is left of it, at 33,17 right of it).
# Generated recordings (inputs only), 16 local players in rooms mode; each
# case is a run of its own, and each with a Kirby 4.. must end exactly as the
# same case with a Kirby 1..3 does:
#   left    one Kirby inhales from the left: the block comes to 469
#           (slot 1, slot 4; before the fix slot 4 left it at 488)
#   right   one Kirby inhales from the right: the block comes to 498 and
#           stops beside it (slot 1, slot 15; before: 488)
#   cancel  slot 1 from the left and one from the right at once: the block
#           stays at 488 (slot 2, slot 5; before: slot 5 did not count and
#           the block went to 469)
# The "left" case with slot 4 runs twice: identical hash logs.  No sanitizer
# report past the console's own (tools/wide_sanclass.py), no rom-check report.
# Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a KATAM_MAX_KIRBYS=16 native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pull() {  # slot cell facing-key: warp in, face the block, inhale for 10 s
    echo "warp 1400 $1 102 $2 17"
    echo "kp $1 1600 $3"; echo "kp $1 1604 000"
    echo "kp $1 1650 002"; echo "kp $1 2250 000"
}
rec() {  # name, then the case's lines on stdin
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
        cat
        echo "end"
    } >"$TMP/$1.txt"
}
pull 1 28 010                   | rec left1
pull 4 28 010                   | rec left4
pull 1 33 020                   | rec right1
pull 15 33 020                  | rec right15
{ pull 1 28 010; pull 2 33 020; } | rec cancel2
{ pull 1 28 010; pull 5 33 020; } | rec cancel5

run() {  # rec out
    local d; d=$(mktemp -d -p "$TMP")
    KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=0} UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1} \
        "$BIN" --no-audio --turbo --replay "$TMP/$1.txt" --hash-log "$TMP/$2.hash" "$ROM" >"$TMP/$2.log" 2>&1
}
for c in left1 left4 right1 right15 cancel2 cancel5; do run $c $c & done
run left4 left4b &
wait

status=0
fail() { echo "FAIL $*"; status=1; }
for r in left1 left4 right1 right15 cancel2 cancel5 left4b; do
    grep -q "wide: kirbys 16" "$TMP/$r.log" || fail "$r: not a 16-Kirby build or did not finish: $(tail -2 "$TMP/$r.log" | tr '\n' ' ')"
    grep -q "Segmentation\|SEGV" "$TMP/$r.log" && fail "$r: $(grep -m1 'Segmentation\|SEGV' "$TMP/$r.log")"
    if grep -qE "ERROR: AddressSanitizer|runtime error:" "$TMP/$r.log"; then
        c=$(python3 "$HERE/../tools/wide_sanclass.py" "$TMP/$r.log") || fail "$r: sanitizer: $(echo "$c" | head -3 | tr '\n' ' ')"
    fi
    grep -q "rom-check: animation" "$TMP/$r.log" && fail "$r: $(grep -m1 'rom-check: animation' "$TMP/$r.log")"
done
block() { grep -o "at exit: star stone block 79 in room 102 at [0-9-]*,[0-9-]*" "$TMP/$1.log" | sed 's/.* at //'; }
check() {  # case low-slot-run high-slot-run expected-x what
    local a b; a=$(block "$2"); b=$(block "$3")
    if [ "${a%%,*}" = "$4" ] && [ "$a" = "$b" ]; then
        echo "ok   $1: $5 (block at $b both times)"
    else
        fail "$1 ($5): Kirby 1..3 run left the block at ${a:-?}, Kirby 4.. run at ${b:-?}, want x $4"
    fi
}
check left   left1   left4   469 "slot 4 inhales from the left, as slot 1"
check right  right1  right15 498 "slot 15 inhales from the right and the block stops beside it, as slot 1"
check cancel cancel2 cancel5 488 "slots 1 and 5 inhale from opposite sides and cancel, as 1 and 2"
if cmp -s "$TMP/left4.hash" "$TMP/left4b.hash" && [ -s "$TMP/left4.hash" ]; then
    echo "ok   two runs, $(wc -l <"$TMP/left4.hash") frames, hash logs identical"
else
    fail "the two runs differ (or no hash log)"
fi
[ $status = 0 ] && echo "BLOCKS PASS" || echo "BLOCKS FAIL"
exit $status
