#!/usr/bin/env bash
# run_all.sh -- regenerate ROOMS-REPORT.md: every measurement it cites.
#
#   tools/rooms/run_all.sh ROM TMP OUT.md [--no-build] [--no-gates] [--quick]
#
# TMP gets everything ROM-derived (dumps, logs, recordings of rig sessions):
# keep it out of the repository.  Steps and rough times (this machine):
#   build (wasm + native)                         ~2 min
#   measure: 9 recordings x views 0/1, 2/3, 0/0, and with the fix off  ~1 min (parallel)
#   re-view tests: 5 points                       ~1 min (parallel)
#   rig: apart, wander, free (40000 f), corrupt, join   ~2 min
#   gates: regress, regress-wasm (+ lint-determinism, check_sim_state)  ~3 min
#   netproof (switch off, full)                   ~7 min (--no-gates skips)
set -euo pipefail
ROM=$1; T=$2; OUT=$3; shift 3
BUILD=1; GATES=1; QUICK=
for a in "$@"; do
    case $a in --no-build) BUILD=;; --no-gates) GATES=;; --quick) QUICK=--quick;; esac
done
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
mkdir -p "$T"
if [ -n "$BUILD" ]; then
    source ~/emsdk/emsdk_env.sh >/dev/null 2>&1
    make -j24 build/katam-node.js >"$T/build-wasm.log" 2>&1
    cmake -S . -B build/lp64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKATAM_ALLOW_LP64=ON >"$T/build-native.log" 2>&1
    cmake --build build/lp64 -j 24 >>"$T/build-native.log" 2>&1
fi
python3 tools/rooms/make_rooms_recordings.py

RECS="rooms_apart rooms_wander localmp_tour players_boss players_dropin players_goal players_local players_rooms players_rooms_normal"
rm -rf "$T/final"; mkdir -p "$T/final"
M() { node tools/rooms/measure.mjs --rom "$ROM" --dump-first 0 "$@" 2>&1 | grep -E '^[a-z_]+: [0-9]+ frames' || true; }
for r in $RECS; do M --rec tests/recordings/$r.txt --out "$T/final/v01" --views 0,1 & done
M --rec tests/recordings/players_dropin.txt --out "$T/final/v23" --views 2,3 &
M --rec tests/recordings/players_goal.txt --out "$T/final/v23" --views 2,3 &
M --rec tests/recordings/rooms_apart.txt --out "$T/final/v00" --views 0,0 &
M --rec tests/recordings/rooms_apart.txt --out "$T/final/nofix" --views 0,1 --rooms-test 1 &
M --rec tests/recordings/localmp_tour.txt --out "$T/final/nofix" --views 0,1 --rooms-test 1 &
wait

rm -rf "$T/review"; mkdir -p "$T/review"
for at in 1800 3300 5900 7000; do
    node tools/rooms/review_test.mjs --rom "$ROM" --rec tests/recordings/rooms_apart.txt --at $at --frames $((at + 800)) \
        --out "$T/review/apart-$at.json" >/dev/null 2>&1 &
done
node tools/rooms/review_test.mjs --rom "$ROM" --rec tests/recordings/rooms_wander.txt --at 4000 --frames 4800 \
    --out "$T/review/wander-4000.json" >/dev/null 2>&1 &
wait

mkdir -p "$T/rec"
node tools/rooms/rig.mjs --rom "$ROM" --out "$T/rig.json" --record-dir "$T/rec" $QUICK 2>&1 \
    | grep -E '^(apart|wander|free|corrupt|join): canonical' | tee "$T/rig-summary.txt"
# the free session, replayed offline with both views (the rig's personal
# repairs, if any, are explained here)
M --rec "$T/rec/free.txt" --out "$T/final/rigfree" --views 0,1 --parts

if [ -n "$GATES" ]; then
    {
        echo "\$ make regress"; make regress ROM="$ROM" 2>&1 | grep -E "OK|DIVERGED|FAIL|check_sim_state|lint_determinism|Error|mode sim"
        echo "\$ make regress-wasm"; make regress-wasm ROM="$ROM" 2>&1 | grep -E "OK|DIVERGED|FAIL|Error|mode sim"
        echo "\$ make netproof (switch off; RIG-REPORT gates)"
        node tools/netproof.mjs --rom "$ROM" --out "$T/netproof" >"$T/netproof.log" 2>&1 || true
        grep -E '^\| R[0-9]+ \| \*\*' "$T/netproof/RIG-REPORT.md" | cut -c1-200 || echo "(netproof produced no report)"
    } >"$T/gates.txt" 2>&1 || true
fi
python3 tools/rooms/report.py --tmp "$T" --out "$OUT" --prose tools/rooms/ROOMS-PROSE.md
echo "wrote $OUT"
