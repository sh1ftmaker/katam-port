#!/usr/bin/env bash
#
# wide_abilities.sh -- every copy ability in the hands of Kirbys 4..15,
# natively, in a katam16 build: the per-player graphics and tables of the
# abilities' moves, effects and projectiles for the Kirbys past the console's
# four (docs/design/ONE-WORLD-16.md "Slot audit").
#
#   tests/wide_abilities.sh WIDE16/katam
#
# WIDE16 is a native build with KATAM_MAX_KIRBYS=16; best an ASan+UBSan one
# with -DKATAM_ROM_CHECK=ON (platform/rom_check.c), which also reports an
# animation variant past the ROM's count -- a read the sanitizers cannot see.
#
# One generated recording (inputs only): 16 local players in rooms mode, the
# story past the tutorial, Kirbys 4..15 warped into level rooms (several to a
# room), each given an ability (a recording's `ability` line, as the game
# gives one), three phases
# so that all 26 abilities are held by some Kirby 4..: tap B, hold B, B with
# each direction, jump and attack, slide, walk, then Select (the ability's
# star, which the next Kirby may swallow); some Kirbys are set to 1 HP so
# that they die and come back, and in the last phase they press R (the cell
# phone: the helpers come) and L (the warp star home).  Run twice with the
# render list built headless: no sanitizer report, no rom-check report, no
# sprite turned away by the renderer's safety net, and the two hash logs
# identical.  Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a KATAM_MAX_KIRBYS=16 native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

KIRBY=$(( 0x02040000 + (((0xB4 + 4130 * 16) + 0xFFF) & ~0xFFF) ))   # port/wide.h WIDE_BASE: gKirbys
ROOMS=(101:3:7 104:2:9 202:3:4 402:4:4 502:2:7 101:6:7)              # room:x:y, two or so Kirbys each
PHASE=1500                                                           # frames per ability
# SLOTS / PLAYERS: a control run of the same moves by the console's own
# Kirbys on a default build (SLOTS="1 2 3" PLAYERS=4).
# AOFF shifts which abilities they get (Kirby number i of SLOTS has 1 + (i +
# AOFF + 12 * phase) % 26).
SLOTS=${SLOTS:-$(seq 4 15)}
PLAYERS=${PLAYERS:-16}
AOFF=${AOFF:-0}
ROFF=${ROFF:-0}                                                     # and which rooms
[ "$PLAYERS" -gt 4 ] || KIRBY=$(( 0x02020EE0 ))                     # the console's gKirbys
START=1400
REC=$TMP/abilities.txt
{
    echo "katam-input 1"
    echo "frames $(( START + 3 * PHASE + 200 ))"
    echo "option localmp $PLAYERS"
    echo "option rooms 1"
    [ "$PLAYERS" -gt 4 ] && echo "kirbys 0 $PLAYERS"
    echo "poke16 1200 203AD2C 12C"          # gAIKirbyState NORMAL: past the tutorial
    m=0
    for f in $(seq 0 1299); do               # the power-on taps (tools/netproof/scenario.mjs bootKey)
        v=0; [ "$f" -ge 120 ] && [ $(( (f - 120) % 20 )) -lt 10 ] && v=11
        [ "$v" != "$m" ] && echo "k $f $v" && m=$v
    done
    [ "$m" != 0 ] && echo "k 1300 0"
    i=0
    for s in $SLOTS; do
        IFS=: read -r room x y <<<"${ROOMS[$(( (i + ROFF) % ${#ROOMS[@]} ))]}"
        echo "warp $(( 1300 + 4 * i )) $s $room $x $y"
        i=$(( i + 1 ))
    done
    for p in 0 1 2; do
        i=0
        for s in $SLOTS; do
            at=$(( START + p * PHASE + 3 * i ))
            a=$(( 1 + (i + AOFF + 12 * p) % 26 ))
            i=$(( i + 1 ))
            echo "ability $at $s $a"
            # some Kirbys on 1 HP (hp, maxHp at 0x100): the next hit is their last
            # (a recording holds 16 pokes: 4 per phase)
            if [ $(( s % 3 )) = 0 ]; then
                printf 'poke16 %d %X %X\n' $(( at + 1 )) $(( KIRBY + s * 0x1A8 + 0x100 )) 0x0601
            fi
            f=$(( at + 10 ))
            seg() { echo "kp $s $f $1"; f=$(( f + $2 )); }
            for r in 1 2 3; do seg 002 8; seg 000 8; done   # tap B
            seg 002 90; seg 000 6                            # hold B
            seg 042 40; seg 000 6                            # up + B
            seg 082 40; seg 000 6                            # down + B
            seg 012 40; seg 000 6                            # right + B
            seg 022 40; seg 000 6                            # left + B
            seg 001 12; seg 003 30; seg 000 10               # jump, attack in the air
            seg 081 20; seg 000 10                           # slide
            seg 010 70; seg 020 70; seg 000 6                # walk
            seg 041 30; seg 001 30; seg 000 20               # fly
            for r in 1 2 3 4; do seg 012 10; seg 010 10; done
            seg 002 120; seg 000 10                          # hold B again (inhale, charge)
            if [ $p = 2 ] && [ $(( s % 4 )) = 1 ]; then
                seg 100 8; seg 000 120                       # R: the cell phone
            fi
            seg 004 8; seg 000 30                            # Select: the ability's star
            seg 002 60; seg 000 10                           # swallow what is near
            if [ $p = 2 ] && [ $(( s % 4 )) = 3 ]; then
                seg 200 200; seg 000 10                      # L: the warp star home
            fi
            echo "kp $s $f 000"
        done
    done
    echo "end"
} >"$REC"
[ -n "${KEEP_REC:-}" ] && cp "$REC" "$KEEP_REC"

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
    grep -q "calls into missing functions" "$TMP/$r.log" || fail "run $r: did not finish: $(tail -2 "$TMP/$r.log" | tr '\n' ' ')"
    grep -q "render list built headless" "$TMP/$r.log" || fail "run $r: the render list was not built"
    grep -q "Segmentation\|SEGV" "$TMP/$r.log" && fail "run $r: $(grep -m1 'Segmentation\|SEGV' "$TMP/$r.log")"
    if grep -qE "ERROR: AddressSanitizer|runtime error:" "$TMP/$r.log"; then   # the console's own UB is known
        c=$(python3 "$(dirname "$0")/../tools/wide_sanclass.py" "$TMP/$r.log") || { fail "run $r: sanitizer: $(echo "$c" | head -3 | tr '\n' ' ')"; cp "$TMP/$r.log" "${KEEP_LOG:-/dev/null}.$r.bad"; }
    fi
    grep -q " not drawn -- " "$TMP/$r.log" && fail "run $r: $(grep -m1 ' not drawn -- ' "$TMP/$r.log")"
    # rom-check reports, less the one the console's own Kirbys 1..3 make on a
    # default build with the same moves (SLOTS="1 2 3" PLAYERS=4 AOFF=9): the
    # stone's row 0x34 variant (kirby.c, (counter >> 5) + 3) on the normal
    # Kirby's table, animation 202 -- not a slot's
    bad=$(grep "rom-check: animation" "$TMP/$r.log" | grep -v "rom-check: animation 202 variant [3-9] ")
    [ -n "$bad" ] && fail "run $r: $(echo "$bad" | head -1)"
done
[ -n "${KEEP_LOG:-}" ] && cp "$TMP/a.log" "$KEEP_LOG"
grep -q "rom-check: .* animations counted" "$TMP/a.log" && echo "ok   animation variants checked (KATAM_ROM_CHECK)"
for s in $SLOTS; do
    line=$(grep "at exit: player $s in room" "$TMP/a.log")
    [ -n "$line" ] || fail "slot $s: no exit report"
done
if cmp -s "$TMP/a.hash" "$TMP/b.hash" && [ -s "$TMP/a.hash" ]; then
    echo "ok   two runs, $(wc -l <"$TMP/a.hash") frames, hash logs identical"
else
    fail "the two runs differ (or no hash log)"
fi
[ $status = 0 ] && echo "ABILITIES PASS" || echo "ABILITIES FAIL"
exit $status
