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
#   groups   door groups (docs/design/CPU-HINTS.md): all EXPLORER, Kirbys 2..4
#            warped at frame 1500 into room 709's three sealed chambers (the
#            arrival cells of its doors from 707, 704 and 702).  Each must
#            choose, and walk through within 10 s, the one door of its own
#            chamber (before door groups all three aimed at 707's door).
# Needs ROM=/path/to/rom.gba.
set -uo pipefail

BIN=$(realpath "${1:?a native build}")
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rec() {  # name word [extra lines]
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
        [ -n "${3:-}" ] && printf '%s\n' "$3"
        echo "end"
    } >"$TMP/$1.txt"
}
rec mix E79E79E4
rec classic 0
rec groups AAAAAAA8 "warp 1500 1 709 3 4
warp 1500 2 709 14 4
warp 1500 3 709 3 9"
sed -i 's/^frames 12000/frames 2200/' "$TMP/groups.txt"

run() {  # rec out
    local d; d=$(mktemp -d -p "$TMP")
    KATAM_CPUAI_LOG=1 KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        setarch -R "$BIN" --no-audio --turbo --replay "$TMP/$1.txt" --hash-log "$TMP/$2.hash" "$ROM" >"$TMP/$2.log" 2>&1
}
run mix mix & run mix mix2 & run classic classic & run groups groups &
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
for kd in 1:707 2:704 3:702; do
    k=${kd%:*} d=${kd#*:}
    first=$(grep "\[cpuai\] f[0-9]* k$k door 709 " "$TMP/groups.log" | head -1 | awk '{print $6}')
    thr=$(grep -c "\[cpuai\] f1[5-9][0-9][0-9] k$k through 709 $d " "$TMP/groups.log")
    [ "$first" = "$d" ] && [ "$thr" -ge 1 ] && ok "groups: Kirby $((k + 1)) in 709 took its own chamber's door (to $d)" \
        || fail "groups: Kirby $((k + 1)) in 709 chose ${first:-nothing} (want $d), through by f1999: $thr"
done
[ $status = 0 ] && echo "CPU AI PASS" || echo "CPU AI FAIL"
exit $status
