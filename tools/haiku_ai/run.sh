#!/usr/bin/env bash
#
# run.sh -- play the game with CPU Kirbys directed by a planner
# (docs/design/CPU-HAIKU.md): the mock (offline, deterministic) or Claude
# Haiku 5.5.
#
#   ROM=/path/to/rom.gba tools/haiku_ai/run.sh [options]
#
#   --backend mock|haiku   who plans (default mock; haiku needs
#                          ANTHROPIC_API_KEY and the SDK, see below)
#   --frames N             game frames to run (default 7200 = 2 minutes)
#   --modes WORD           the CPU Kirbys' own modes while no directive runs
#                          (hex, port/cpu_ai.h; default E79E79E4 = Kirby 2
#                          BODYGUARD, 3 EXPLORER, 4 ADVENTURER; 0 = CLASSIC)
#   --slots HEX            which Kirbys the planner directs (bit s = slot s;
#                          default E = Kirbys 2, 3 and 4)
#   --window               show the game in a window at real speed (default:
#                          headless, as fast as the machine goes)
#   --sync / --async       wait for every answer (the game pauses while the
#                          planner thinks: reproducible, fast headless) or
#                          not (the game never waits: the real thing).
#                          Default: --sync headless, --async with --window
#   --effort low|medium    Haiku's effort (default low)
#   --no-images            send Haiku the text observation only
#   --out DIR              where everything goes (default build/haiku/run-TIME)
#   --verify               replay the recording afterwards and compare hash logs
#
# The run: a new game, the story set past the opening, player 1 warped to
# the hub (room 801) at frame 1400 and then left idle -- the same start as
# tests/cpu_ai.sh -- so the CPU Kirbys are on their own.  The planner is
# asked from frame 1500.  Out: live.txt (the recording, with every directive
# as a `cpugoal` line), live.hash, game.log, decisions.jsonl (observation ->
# directive -> outcome; tools/haiku_ai/report.py prints it), shots/ (the
# pictures the planner saw).  Nothing under build/ is committed.
#
# Haiku: python3 -m venv tools/haiku_ai/.venv &&
#        tools/haiku_ai/.venv/bin/pip install anthropic
#        export ANTHROPIC_API_KEY=...   (then --backend haiku)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BIN=${KATAM_BIN:-$ROOT/build/lp64/katam}
ROM=${ROM:?set ROM=/path/to/rom.gba}
BACKEND=mock FRAMES=7200 MODES=E79E79E4 SLOTS=E WINDOW=0 SYNC= EFFORT=low IMAGES=1 OUT= VERIFY=0

while [ $# -gt 0 ]; do
    case $1 in
        --backend) BACKEND=$2; shift ;;
        --frames) FRAMES=$2; shift ;;
        --modes) MODES=$2; shift ;;
        --slots) SLOTS=$2; shift ;;
        --window) WINDOW=1 ;;
        --sync) SYNC=1 ;;
        --async) SYNC=0 ;;
        --effort) EFFORT=$2; shift ;;
        --no-images) IMAGES=0 ;;
        --out) OUT=$2; shift ;;
        --verify) VERIFY=1 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "run.sh: unknown option $1 (--help)"; exit 2 ;;
    esac
    shift
done
[ -x "$BIN" ] || { echo "run.sh: no $BIN -- build it (docs/NATIVE.md; cmake -S . -B build/lp64 ...)"; exit 2; }
[ -n "$SYNC" ] || SYNC=$((1 - WINDOW))
OUT=${OUT:-$ROOT/build/haiku/run-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

PY=python3
[ -x "$HERE/.venv/bin/python" ] && PY=$HERE/.venv/bin/python
BRIDGE_ARGS=(--backend "$BACKEND" --log "$OUT/decisions.jsonl" --effort "$EFFORT")
[ "$IMAGES" = 1 ] || BRIDGE_ARGS+=(--no-images)
if [ "$BACKEND" = haiku ]; then
    [ -n "${ANTHROPIC_API_KEY:-}" ] || { echo "run.sh: export ANTHROPIC_API_KEY first"; exit 2; }
    "$PY" -c 'import anthropic' 2>/dev/null \
        || { echo "run.sh: no anthropic SDK for $PY -- see the header of this script"; exit 2; }
fi

echo "@1200 poke16 203AD2C 0x12C" >"$OUT/boot.dev"   # gAIKirbyState: past the opening
ARGS=(--no-audio --frames "$FRAMES" --mash 120:0x11:20:1300 --devices "$OUT/boot.dev"
      --warp 1400:801:24:12 --record "$OUT/live.txt" --hash-log "$OUT/live.hash")
PLANNER_CMD=$(printf '%q ' "$PY" "$HERE/bridge.py" "${BRIDGE_ARGS[@]}")
ENVS=(KATAM_PLANNER="$PLANNER_CMD" KATAM_PLANNER_DIR="$OUT/shots"
      KATAM_PLANNER_FROM=1500 KATAM_PLANNER_SLOTS="$SLOTS" KATAM_PLANNER_SYNC="$SYNC"
      KATAM_CPU_AI="$MODES" KATAM_CPU_AI_AT=1300 KATAM_CPUAI_LOG=1
      XDG_DATA_HOME="$OUT/xdg/data" XDG_CONFIG_HOME="$OUT/xdg/config")
if [ "$WINDOW" = 1 ]; then
    ARGS+=(--scale 3)
else
    ARGS+=(--turbo)
    ENVS+=(KATAM_RL_HEADLESS=1 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy)
fi
NORAND=()
setarch -R true 2>/dev/null && NORAND=(setarch -R)

echo "run.sh: $BACKEND planner, $FRAMES frames, modes $MODES, slots $SLOTS, sync=$SYNC -> $OUT"
env "${ENVS[@]}" "${NORAND[@]}" "$BIN" "${ARGS[@]}" "$ROM" >"$OUT/game.log" 2>&1 || true
grep -E '^\[planner\]|^bridge:' "$OUT/game.log" | tail -5 || true
echo
"$PY" "$HERE/report.py" "$OUT/decisions.jsonl" --obs 1 || true

if [ "$VERIFY" = 1 ]; then
    env XDG_DATA_HOME="$OUT/xdg2/data" XDG_CONFIG_HOME="$OUT/xdg2/config" KATAM_RL_HEADLESS=1 \
        SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "${NORAND[@]}" "$BIN" --no-audio --turbo \
        --replay "$OUT/live.txt" --hash-log "$OUT/replay.hash" "$ROM" >"$OUT/replay.log" 2>&1
    if cmp -s "$OUT/live.hash" "$OUT/replay.hash"; then
        echo "verify: the recording ($(grep -c '^cpugoal' "$OUT/live.txt") directives) replays identically, $(wc -l <"$OUT/replay.hash") frames"
    else
        echo "verify: FAILED -- live.hash and replay.hash differ"; exit 1
    fi
fi
