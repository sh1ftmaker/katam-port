#!/bin/sh
# room_sweep.sh -- visit rooms, and record what the game did in each.
#
# The game has 286 rooms and a scripted run reaches a handful.  33 of them have
# no incoming door edge at all, so they cannot be reached by playing.  With
# PortWarp they can all be entered directly, which turns "what does this room
# contain" and "what moves when the game is in it" from questions that need a
# speedrun into questions that need a shell loop.
#
# For each room it produces, under $OUT:
#
#   room-<id>.json    the live room state -- dimensions, slot, collision grid,
#                     the LevelInfo tail for all four players
#   room-<id>.ppm     the last rendered frame
#   room-<id>.log     the port's diagnostics for that run
#
# and then checks each room's collision grid against the static reconstruction
# in tools/room_export.py.  That check is the point of doing it this way: it is
# 286 independent comparisons between what the ROM says a room is and what the
# game builds when it goes there, and any disagreement is a real finding about
# either the format or the port.
#
# Usage:
#   tools/room_sweep.sh <rom.gba> [room ...]      # named rooms, or all of them
#
# Environment:
#   OUT=build/sweep     where to write
#   FRAMES=1500         how long to run; the warp is issued at WARP_AT
#   WARP_AT=1320        the frame the warp is requested on.  Requesting early
#                       is safe -- platform/warp.c holds it until the game is
#                       actually in a level -- but the run still has to be long
#                       enough for the room to load and settle afterwards.
set -eu

ROM=${1:?usage: room_sweep.sh <rom.gba> [room ...]}
shift

OUT=${OUT:-build/sweep}
FRAMES=${FRAMES:-1500}
WARP_AT=${WARP_AT:-1320}
MODULE=${MODULE:-build/katam-node.js}

mkdir -p "$OUT"

# No room list given: sweep every room the ROM actually defines, straight from
# the table rather than from a hard-coded range.  Room ids are not contiguous
# and do not start at zero.
if [ $# -eq 0 ]; then
    set -- $(python3 tools/room_export.py --rom "$ROM" --list \
             | awk '{print $2}')
fi

total=0; ok=0; mismatch=0; missing=0; diags=0

# Where to put Kirby in each room.  This is not a detail: warping to an
# arbitrary cell HANGS THE GAME -- room 804 cell (3,5) hangs, cell (2,5) works,
# and the difference is that (2,5) is a cell one of the game's own doors
# arrives at.  So spawn points come from inverting the door table rather than
# from a default.  See arrival_cells() in tools/room_export.py.
SPAWNS=$(python3 tools/room_export.py --rom "$ROM" --spawn)

hung=0

for room in "$@"; do
    total=$((total + 1))
    json="$OUT/room-$room.json"
    log="$OUT/room-$room.log"

    cell=$(printf '%s\n' "$SPAWNS" | awk -v r="$room" '$1 == r {print $2":"$3; exit}')
    src=$(printf '%s\n' "$SPAWNS" | awk -v r="$room" '$1 == r {print $4; exit}')
    if [ -z "$cell" ] || [ "$src" = "none" ]; then
        missing=$((missing + 1))
        printf 'room %-4s NO SPAWN CELL -- nothing arrives here and no floor found\n' "$room"
        continue
    fi

    # A hang is the expected failure mode for a bad spawn, so every run is
    # bounded.  Without this one bad room stalls a 286-room sweep forever.
    DUMP_ROOM="$json" WARP="$WARP_AT:$room:${cell%%:*}:${cell##*:}" \
        HOLD=0x10 MASH=600:1:6 \
        timeout "${RUN_TIMEOUT:-240}" \
        node tools/headless_test.js "$MODULE" "$ROM" "$FRAMES" >"$log" 2>&1 \
        || { hung=$((hung + 1)); printf 'room %-4s HUNG or crashed (spawn %s from %s)\n' "$room" "$cell" "$src"; }

    [ -f build/frame.ppm ] && mv build/frame.ppm "$OUT/room-$room.ppm"

    # The port reports its own diagnostics; a room that produces any is more
    # interesting than one that does not, so they are counted separately from
    # the collision check rather than folded into it.
    if grep -q 'port diagnostics ([1-9]' "$log" 2>/dev/null; then
        diags=$((diags + 1))
        printf 'room %-4s DIAGNOSTICS\n' "$room"
    fi

    if [ ! -f "$json" ]; then
        missing=$((missing + 1))
        printf 'room %-4s NOT REACHED -- the warp was never delivered\n' "$room"
        continue
    fi

    if python3 tools/room_export.py --rom "$ROM" --check "$json" >"$log.check" 2>&1; then
        ok=$((ok + 1))
    else
        mismatch=$((mismatch + 1))
        printf 'room %-4s MISMATCH\n' "$room"
        sed 's/^/         /' "$log.check"
    fi
done

echo
echo "swept $total room(s) -> $OUT"
echo "  $ok matched the static reconstruction"
echo "  $mismatch disagreed"
echo "  $missing never loaded or had no spawn cell"
echo "  $hung hung or crashed"
echo "  $diags produced port diagnostics"

[ "$mismatch" -eq 0 ] && [ "$missing" -eq 0 ] && [ "$hung" -eq 0 ]
