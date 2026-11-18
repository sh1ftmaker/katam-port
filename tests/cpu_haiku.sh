#!/usr/bin/env bash
#
# cpu_haiku.sh -- planner directives for the CPU Kirbys (docs/design/CPU-HAIKU.md),
# natively, offline (the mock planner; no network, no API key).
#
#   tests/cpu_haiku.sh build/lp64/katam [OLD_BUILD]
#
# Generated recordings (inputs only) start like tests/cpu_ai.sh's: a new
# game, the story set past the opening, player 1 warped to the hub (room 801)
# at frame 1400, then an idle pad.
#
#   hub        modes E79E79E4 from 1300, then `cpugoal` lines: Kirby 2
#              goto_room 101, Kirby 3 goto_xy in the hub, Kirby 4 wait 6 s and
#              then goto_door (19,14) (to 101), Kirby 3 goto_room 2 (no such
#              room), Kirby 2 follow player 1 from another room for 5 s.  Each
#              must end as written (done / failed bad_target / timeout), and
#              two replays must give identical hash logs.
#   chambers   CLASSIC (word 0: directives on the stock brain), Kirbys 2..4
#              warped into room 709's three sealed chambers: goto_room 709
#              group 0 (already there: done), goto_room 704 (its chamber's
#              door), goto_xy in the lower chamber, later goto_door (1,3).
#   none       no directive, no modes: no directive ever runs, and a replay
#              with KATAM_PLANNER set (to a command that would fail) is
#              identical -- a replay never asks the planner.  With OLD_BUILD
#              (a build from before the planner): keys, picture, video and
#              save hashes identical to the old build's on every frame (the
#              sim column moves with code layout alone: tests/layout_neutral.sh),
#              and a mode run's [cpuai] notes identical.
#   e2e        a live run with the mock planner (tools/haiku_ai/bridge.py,
#              KATAM_PLANNER_SYNC=1): it must record its directives as
#              `cpugoal` lines, at least one must end done, and replaying the
#              recording must give the live run's hash log exactly.  A second
#              live run must record the same file (the mock is deterministic).
# And tools/haiku_ai/test_planner.py (request building, reply parsing, the
# SDK on a mock transport when tools/haiku_ai/.venv has it).
# Needs ROM=/path/to/rom.gba.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN=$(realpath "${1:?a native build}")
OLD=${2:+$(realpath "$2")}
ROM=${ROM:?set ROM=/path/to/rom.gba}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

rec() {  # name frames word [extra lines]
    {
        echo "katam-input 1"
        echo "frames $2"
        echo "poke16 1200 203AD2C 12C"      # gAIKirbyState NORMAL: past the opening
        m=0
        for f in $(seq 0 1299); do           # the power-on taps (tests/wide_blocks.sh)
            v=0; [ "$f" -ge 120 ] && [ $(( (f - 120) % 20 )) -lt 10 ] && v=11
            [ "$v" != "$m" ] && echo "k $f $v" && m=$v
        done
        [ "$m" != 0 ] && echo "k 1300 0"
        [ "$3" != 0 ] && echo "cpuai 1300 $3"
        echo "warp 1400 0 801 24 12"
        [ -n "${4:-}" ] && printf '%s\n' "$4"
        echo "end"
    } >"$TMP/$1.txt"
}
rec hub 3600 E79E79E4 "cpugoal 1700 1 goto_room 101 0 0 60
cpugoal 1700 2 goto_xy 65535 11 14 20
cpugoal 1700 3 wait 0 0 0 6
cpugoal 2100 3 goto_door 65535 19 14 40
cpugoal 2500 2 goto_room 2 0 0 10
cpugoal 2700 1 follow 0 0 0 5"
rec chambers 2700 0 "warp 1500 1 709 3 4
warp 1500 2 709 14 4
warp 1500 3 709 3 9
cpugoal 1501 1 goto_room 709 1 0 0 30
cpugoal 1501 2 goto_room 704 0 0 0 30
cpugoal 1501 3 goto_xy 65535 10 9 20
cpugoal 2400 1 goto_door 65535 1 3 20"
rec none 6000 0
rec mix 6000 E79E79E4

run() {  # bin rec out [env...]
    local bin=$1 r=$2 out=$3 d; shift 3
    d=$(mktemp -d -p "$TMP")
    env "$@" KATAM_CPUAI_LOG=1 KATAM_RL_HEADLESS=1 XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        setarch -R "$bin" --no-audio --turbo --replay "$TMP/$r.txt" --hash-log "$TMP/$out.hash" "$ROM" \
        >"$TMP/$out.log" 2>&1
}
live() {  # out -- a live run with the mock planner, recorded
    local out=$1 d
    d=$(mktemp -d -p "$TMP")
    echo "@1200 poke16 203AD2C 0x12C" >"$d/boot.dev"
    env KATAM_PLANNER="python3 $ROOT/tools/haiku_ai/bridge.py --backend mock --workers 1 --log $TMP/$out.jsonl" \
        KATAM_PLANNER_DIR="$TMP/$out-shots" KATAM_PLANNER_SYNC=1 KATAM_PLANNER_FROM=1500 \
        KATAM_CPU_AI=E79E79E4 KATAM_CPU_AI_AT=1300 KATAM_CPUAI_LOG=1 KATAM_RL_HEADLESS=1 \
        XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        setarch -R "$BIN" --no-audio --turbo --frames 7200 --mash 120:0x11:20:1300 --devices "$d/boot.dev" \
        --warp 1400:801:24:12 --record "$TMP/$out.txt" --hash-log "$TMP/$out.hash" "$ROM" >"$TMP/$out.log" 2>&1
}

run "$BIN" hub hub & run "$BIN" hub hub2 & run "$BIN" chambers chambers & run "$BIN" chambers chambers2 &
run "$BIN" none none & run "$BIN" none none-p KATAM_PLANNER="false" KATAM_PLANNER_SYNC=1 &
run "$BIN" mix mix &
[ -n "$OLD" ] && { run "$OLD" none none-old & run "$OLD" mix mix-old & }
live e2e & live e2e2 &
wait

status=0
ok()   { echo "ok   $*"; }
fail() { echo "FAIL $*"; status=1; }

# the planner's Python side (no network): with the SDK if its venv is there
PY=python3
[ -x "$ROOT/tools/haiku_ai/.venv/bin/python" ] && PY=$ROOT/tools/haiku_ai/.venv/bin/python
if out=$("$PY" "$ROOT/tools/haiku_ai/test_planner.py" 2>&1); then
    ok "planner unit tests: $(echo "$out" | grep -E '^Ran ' | head -1)$(echo "$out" | grep -oE 'skipped=[0-9]+' | sed 's/^/, /')"
else
    fail "planner unit tests"; echo "$out" | tail -20
fi
same() {  # a b what
    if [ -s "$TMP/$1.hash" ] && cmp -s "$TMP/$1.hash" "$TMP/$2.hash"; then
        ok "$3: $(wc -l <"$TMP/$1.hash") frames, hash logs identical"
    else
        fail "$3: hash logs differ (or none)"
    fi
}
ended() {  # log slot kind status why what -- a `goal-end` note (port/cpu_ai.h numbers)
    if grep -q "\[cpuai\] f[0-9]* k$2 goal-end $3 $4 $5\$" "$TMP/$1.log"; then
        ok "$1: $6 ($(grep -m1 "\[cpuai\] f[0-9]* k$2 goal-end $3 $4 $5\$" "$TMP/$1.log" | awk '{print $2}'))"
    else
        fail "$1: $6 -- $(grep "k$2 goal-end $3 " "$TMP/$1.log" | head -1)"
    fi
}

# kinds: 1 goto_room 2 goto_door 3 goto_xy 4 follow 8 wait; status 2 done 3 failed 4 timeout;
# why 0 ok 1 time 7 bad_target
ended hub 1 1 2 0 "Kirby 2 goto_room 101: done"
ended hub 2 3 2 0 "Kirby 3 goto_xy (11,14) in the hub: done"
ended hub 3 8 2 0 "Kirby 4 wait 6 s: done"
ended hub 3 2 2 0 "Kirby 4 goto_door (19,14): done (through to 101)"
ended hub 2 1 3 7 "Kirby 3 goto_room 2: failed, bad_target"
ended hub 1 4 4 1 "Kirby 2 follow player 1 from room 101 for 5 s: timeout"
grep -q "\[cpuai-obs\] f[0-9]* k3 room 101" "$TMP/hub.log" && ok "hub: Kirby 4 is in room 101 after its door" \
    || fail "hub: Kirby 4 never reached room 101"
same hub hub2 "hub twice"
ended chambers 1 1 2 0 "Kirby 2 goto_room 709 group 0 (its own chamber): done"
ended chambers 2 1 2 0 "Kirby 3 goto_room 704 (its chamber's door): done"
ended chambers 3 3 2 0 "Kirby 4 goto_xy (10,9) in the lower chamber: done"
ended chambers 1 2 2 0 "Kirby 2 goto_door (1,3): done"
same chambers chambers2 "chambers twice"

n=$(grep -c "goal\|cpugoal" "$TMP/none.log")
[ "$n" = 0 ] && ok "none: no directive ran" || fail "none: $n directive lines"
n=$(grep -c "\[cpuai\] f[0-9]* k[0-9]" "$TMP/none.log")
[ "$n" = 0 ] && ok "none: no mode ran" || fail "none: $n brain notes"
same none none-p "none, replayed with KATAM_PLANNER set"
grep -q "\[planner\] replaying" "$TMP/none-p.log" && ! grep -q "\[planner\] .*ask" "$TMP/none-p.log" \
    && ok "none: the replay never asked the planner" || fail "none: the planner was consulted in a replay"
if [ -n "$OLD" ]; then
    # keys, vid, sram, pix: layout-blind, so they must match on every frame;
    # the sim column also holds host pointers the code layout spells
    # (docs/DETERMINISM.md) -- tests/layout_neutral.sh is the proof for it
    if [ -s "$TMP/none.hash" ] && cmp -s <(cut -d' ' -f1,2,4-6 "$TMP/none.hash") <(cut -d' ' -f1,2,4-6 "$TMP/none-old.hash"); then
        f=$(paste -d' ' "$TMP/none.hash" "$TMP/none-old.hash" | awk '$3 != $9 { print $1; exit }')
        if [ -n "$f" ]; then f="differs from frame $f (code layout: tests/layout_neutral.sh)"; else f=identical; fi
        ok "none vs the old build: keys/vid/sram/pix identical on all $(wc -l <"$TMP/none.hash") frames; sim $f"
    else
        fail "none vs the old build: the game differs"
    fi
    if diff <(grep "\[cpuai" "$TMP/mix.log") <(grep "\[cpuai" "$TMP/mix-old.log") >/dev/null; then
        ok "mix (modes, no directive): the brain's notes are those of the old build ($(grep -c "\[cpuai" "$TMP/mix.log") lines)"
    else
        fail "mix: the modes behave differently from the old build"
    fi
fi

n=$(grep -c "^cpugoal" "$TMP/e2e.txt" 2>/dev/null || echo 0)
j=$(wc -l <"$TMP/e2e.jsonl" 2>/dev/null || echo 0)
[ "$n" -ge 3 ] && [ "$n" = "$j" ] && ok "e2e: the live run recorded $n directives ($j decisions)" \
    || fail "e2e: $n cpugoal lines, $j decisions"
d=$(grep -c "\[cpuai\] f[0-9]* k[0-9] goal-end [0-9] 2 0" "$TMP/e2e.log")
[ "$d" -ge 1 ] && ok "e2e: $d directives ended done" || fail "e2e: no directive ended done"
ls "$TMP"/e2e-shots/*.png >/dev/null 2>&1 && ok "e2e: the planner got pictures ($(ls "$TMP"/e2e-shots | wc -l))" \
    || fail "e2e: no pictures"
run "$BIN" e2e e2e-replay
same e2e e2e-replay "e2e: the recording replayed"
cmp -s "$TMP/e2e.txt" "$TMP/e2e2.txt" && ok "e2e: a second live run recorded the same file" \
    || fail "e2e: the two live runs recorded different files"

[ $status = 0 ] && echo "CPU HAIKU PASS" || echo "CPU HAIKU FAIL"
exit $status
