#!/usr/bin/env bash
#
# cpu_ai.sh -- the CPU AI modes (docs/design/CPU-AI.md), natively.
#
#   tests/cpu_ai.sh build/lp64/katam
#
# Generated recordings (inputs only): a new game, the story set past the
# opening, player 1 warped to the hub (room 801) at frame 1400, then an
# idle pad for 12000 frames.
#   mix      `cpuai 1300 E79E79E4`: Kirby 2 BODYGUARD, 3 EXPLORER,
#            4 ADVENTURER.  EXPLORER and ADVENTURER must each walk through
#            at least two doors on their own; the BODYGUARD must be in the
#            player's room at the end.  Run twice: identical hash logs.
#   classic  the same without the line: no mode ever runs (no brain notes)
#            -- the stock brain, as every older recording.
# Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rec() {  # name word
    {
        echo "katam-input 1"
        echo "frames 12000"
        echo "poke16 1200 203AD2C 12C"      # gAIKirbyState NORMAL: past the opening
        m=0
        for f in $(seq 0 1299); do           # the power-on taps (tests/wide_blocks.sh)
            v=0; [ "$f" -ge 120 ] && [ $(( (f - 120) % 20 )) -lt 10 ] && v=11
            [ "$v" != "$m" ] && echo "k $f $v" && m=$v
        done
        [ "$m" != 0 ] && echo "k 1300 0"
        [ "$2" != 0 ] && echo "cpuai 1300 $2"
        echo "warp 1400 0 801 24 12"
        echo "end"
    } >"$TMP/$1.txt"
}
rec mix E79E79E4
rec classic 0

run() {  # rec out
    local d; d=$(mktemp -d -p "$TMP")
    KATAM_CPUAI_LOG=1 KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        setarch -R "$BIN" --no-audio --turbo --replay "$TMP/$1.txt" --hash-log "$TMP/$2.hash" "$ROM" >"$TMP/$2.log" 2>&1
}
run mix mix & run mix mix2 & run classic classic &
wait

status=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; status=1; }
for k in 2 3; do
    n=$(grep -c "\[cpuai\] f[0-9]* k$k through" "$TMP/mix.log")
    [ "$n" -ge 2 ] && ok "mix: Kirby $((k + 1)) walked through $n doors" \
                   || fail "mix: Kirby $((k + 1)) walked through $n doors (want >= 2)"
done
p=$(grep "\[cpuai-obs\] f[0-9]* k0 room" "$TMP/mix.log" | tail -1 | awk '{print $NF}')
b=$(grep "\[cpuai-obs\] f[0-9]* k1 room" "$TMP/mix.log" | tail -1 | awk '{print $NF}')
[ -n "$p" ] && [ "$p" = "$b" ] && ok "mix: the BODYGUARD ends in the player's room ($b)" \
                               || fail "mix: BODYGUARD in room ${b:-?}, player in ${p:-?}"
if cmp -s "$TMP/mix.hash" "$TMP/mix2.hash" && [ -s "$TMP/mix.hash" ]; then
    ok "mix twice: $(wc -l <"$TMP/mix.hash") frames, hash logs identical"
else
    fail "mix: the two runs differ (or no hash log)"
fi
n=$(grep -c "\[cpuai\] f[0-9]* k[0-9]" "$TMP/classic.log")
[ "$n" = 0 ] && ok "classic: no mode ran" || fail "classic: $n brain notes (a mode ran)"
grep -q "\[cpuai\] f[0-9]* modes" "$TMP/classic.log" && fail "classic: the modes word changed"
[ $status = 0 ] && echo "CPU AI PASS" || echo "CPU AI FAIL"
exit $status
