#!/usr/bin/env bash
#
# coop_selftest.sh -- the online co-op puppet (platform/coop.c), with no
# network: replay a recording twice with slot 1 a puppet, once kept absent
# (the control) and once fed poses -- next to the local Kirby, in another
# room, absent, and left alone -- and check
#
#   1. the puppet sat where it was told, never moved on its own, never had
#      roomId 0xFFFF or a live LevelInfo ("[coop-test] FAIL" lines: none);
#   2. the local player's trajectory and state hash are identical on every
#      frame of the two runs (a puppet affects nothing it touches);
#   3. both runs reached the last frame (no crash), across the recording's
#      room changes and a visit to the pause screen's area map.
#
#   ROM=/path/to/rom.gba tests/coop_selftest.sh [--bin katam] [--shots DIR]
#   ROM=/path/to/rom.gba tests/coop_selftest.sh --wasm build/katam-node.js
#
# --shots DIR also writes DIR/coop-near-ppu.png, the puppet beside the local
# Kirby, and DIR/coop-map-ppu.png, the area map with the puppet on it.  A third run gives the slot back to the CPU mid-way and checks the
# slot gets a live room again.  Builds nothing.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN=${KATAM_BIN:-$ROOT/build/lp64/katam}
REC=$HERE/recordings/play_rooms.txt
ROM=${ROM:-}
SHOTS=
NODE_MODULE=
START=1250
LEAVE=2700
PAUSE=1700      # pause screen + area map while the puppet is in another room

while [ $# -gt 0 ]; do
    case $1 in
        --bin) BIN=$2; shift ;;
        --rom) ROM=$2; shift ;;
        --shots) SHOTS=$2; shift ;;
        --wasm) NODE_MODULE=$(realpath "$2"); shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "no ROM -- set ROM=/path/to/your.gba"; exit 2; }
if [ -n "$NODE_MODULE" ]; then
    [ -f "$NODE_MODULE" ] || { echo "no $NODE_MODULE"; exit 2; }
    [ -z "$SHOTS" ] || { echo "--shots needs the native build"; exit 2; }
else
    [ -x "$BIN" ] || { echo "no native binary at $BIN"; exit 2; }
fi

NORAND=()
command -v setarch >/dev/null && setarch -R true 2>/dev/null && NORAND=(setarch -R)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FRAMES=$(awk '/^frames/{print $2; exit}' "$REC")

run() {  # name selftest-spec [args...]
    local name=$1 spec=$2; shift 2
    local d=$TMP/home-$name
    mkdir -p "$d"
    if [ -n "$NODE_MODULE" ]; then
        (cd "$ROOT" && PORT_COOP_SELFTEST=$spec REPLAY="$REC" AUDIO=0 \
            node tools/headless_test.js "$NODE_MODULE" "$ROM" "$FRAMES") \
            >"$TMP/$name.log" 2>&1
        echo $? >"$TMP/$name.rc"
        return
    fi
    PORT_COOP_SELFTEST=$spec XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "${NORAND[@]}" "$BIN" --no-audio --turbo --replay "$REC" "$@" "$ROM" \
        >"$TMP/$name.log" 2>&1
    echo $? >"$TMP/$name.rc"
}

shot=()
[ -n "$SHOTS" ] && { mkdir -p "$SHOTS"
                     shot=(--shot-at "1550:$SHOTS/coop-near" --shot-at "1830:$SHOTS/coop-map"); }
run absent "$START:absent:-1:$PAUSE" &
run mix "$START:mix:-1:$PAUSE" "${shot[@]}" &
run leave "$START:mix:$LEAVE:$PAUSE" &
wait

status=0
for n in absent mix leave; do
    rc=$(cat "$TMP/$n.rc")
    last=$(grep -c '^\[coop-test\] L ' "$TMP/$n.log")
    fails=$(grep -c '^\[coop-test\] FAIL' "$TMP/$n.log")
    res=$(grep '^\[coop-test\] result' "$TMP/$n.log" | tail -1)
    printf '%-7s exit %s, %s frames logged, %s FAIL lines; %s\n' "$n" "$rc" "$last" "$fails" "${res#*] }"
    grep '^\[coop-test\] FAIL' "$TMP/$n.log" | head -5 | sed 's/^/    /'
    [ "$rc" = 0 ] && [ "$fails" = 0 ] && [ "$last" -ge $((FRAMES - 2)) ] || status=1
    grep -q '^\[coop-test\] start' "$TMP/$n.log" || { echo "    never started"; status=1; }
done

d=$(diff <(grep '^\[coop-test\] L ' "$TMP/absent.log") <(grep '^\[coop-test\] L ' "$TMP/mix.log") | head -3)
if [ -n "$d" ]; then
    echo "local trajectory differs between the absent and the mix run:"; echo "$d" | sed 's/^/    /'
    status=1
else
    echo "local trajectory + state hash identical on all frames (absent vs mix)"
fi
if grep -q 'given back to the CPU' "$TMP/leave.log" \
   && grep '^\[coop-test\] cpu ' "$TMP/leave.log" | tail -1 | grep -qv 'level 65535'; then
    echo "slot 1 given back to the CPU at frame $LEAVE: $(grep '^\[coop-test\] cpu ' "$TMP/leave.log" | tail -1 | cut -d' ' -f3-)"
else
    echo "slot 1 was not given back to the CPU cleanly"; status=1
fi
[ -n "$SHOTS" ] && ls "$SHOTS"/coop-near-ppu.png "$SHOTS"/coop-map-ppu.png 2>/dev/null
exit $status
