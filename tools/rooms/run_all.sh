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
#   gates.mjs: 2/3/4 devices, corruption, pool, join, two players on one
#     device, cost by batch size                ~3 min
#   census + cost                                 ~1 min
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

# the shipped design: the N-device rig with the canonical shadow (gates RM1-RM5)
node tools/rooms/gates.mjs --rom "$ROM" --out "$T/gates.json" $QUICK 2>&1 \
    | grep -E '^(two|three|four|corrupt|pool|join|local2|cost-[a-z0-9]+): ' | tee "$T/gates-summary.txt"
# which task types are the screen's own (the digest leaves them out)
L=tests/recordings
node tools/rooms/census.mjs --rom "$ROM" --rec $L/rooms_apart.txt,$L/rooms_wander.txt,$L/localmp_tour.txt,$L/players_boss.txt,$L/players_dropin.txt,$L/players_goal.txt,$L/players_local.txt,$L/players_rooms.txt \
    --out "$T/census.json" 2>&1 | grep -E 'frames|differs on' || true
# two wasm instances against the shadow: memory and per-frame costs
node --expose-gc tools/rooms/cost.mjs --rom "$ROM" 2>/dev/null | grep -v '^\[' > "$T/cost.json" || true
# the browser case (needs a Chromium with WebGL2, a static server and a room
# server; see tools/online_browser_test.mjs): its results are read from
# $T/browser.json when present, e.g.
#   node tools/online_browser_test.mjs --cdp PORT --base URL --rom-url URL --relay URL --shots $T/shots --cases rooms
#   cp $T/shots/online-test-results.json $T/browser.json

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
