#!/usr/bin/env bash
#
# playtest.sh -- the room loader end to end: new rooms (ids >= 983) from the
# committed example project, played natively and in wasm.
#
#   tests/playtest.sh                 native + wasm (build/katam-node.js)
#   tests/playtest.sh --no-wasm       native only
#
# 1. `katam compile examples/authoring/sky_garden` (no ROM data involved):
#    the room bundle, rooms 983 (meadow) and 984 (tower).
# 2. A recording (tools/katam/lib/playtest.mjs): the regression boot, a warp
#    into room 983 in front of its door at frame 1300, Up held: Kirby takes
#    the meadow's door into the tower, 984, and plays on there.
# 3. Native (--rooms, fresh XDG dirs, setarch -R): the warp is delivered,
#    Kirby ends alive in room 984, both new rooms' progress words are set
#    (they live in the port-owned array, platform/res_rooms.c), and the
#    vanilla save is untouched: gVisitedDoors (EWRAM 0x02028CA0, 0x240
#    bytes) and the whole of save memory are byte-identical from the frame
#    before the warp to the end, and the sram hash column never moves.
# 4. wasm (the node build, ROOMS=bundle): keys, vid, sram and pix equal the
#    native run on every frame (regress.sh --wasm's columns; sim spells
#    pointers per platform, docs/DETERMINISM.md).
#
# Needs your ROM: ROM=/path/to/rom.gba.  Writes only to a temp dir.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
ROM=${ROM:?set ROM=/path/to/rom.gba}
BIN=${KATAM_BIN:-}
NODE_MODULE=${KATAM_NODE:-$ROOT/build/katam-node.js}
WASM=1
while [ $# -gt 0 ]; do
    case $1 in
        --no-wasm) WASM=0 ;;
        --bin) BIN=$2; shift ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
if [ -z "$BIN" ]; then
    for b in "$ROOT/build/lp64/katam" "$ROOT/build/native/katam"; do [ -x "$b" ] && { BIN=$b; break; }; done
fi
[ -x "$BIN" ] || { echo "no native binary -- build one or pass --bin"; exit 2; }

NORAND=()
command -v setarch >/dev/null && setarch -R true 2>/dev/null && NORAND=(setarch -R)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
check() { if eval "$2"; then echo "  ok    $1"; else echo "  FAIL  $1"; fail=1; fi; }

node "$ROOT/tools/katam/katam.mjs" compile "$ROOT/examples/authoring/sky_garden" -o "$TMP/out" >"$TMP/compile.json" || { cat "$TMP/compile.json"; exit 1; }
BUNDLE=$TMP/out/mod/rooms.katn
FRAMES=$(node --input-type=module -e "
import { playtestRecording } from '$ROOT/tools/katam/lib/playtest.mjs';
import fs from 'node:fs';
const r = playtestRecording({ room: 983, x: 28, y: 10, frames: 600, script: [[-60, 0x040]], note: 'tests/playtest.sh' });
fs.writeFileSync('$TMP/rec.txt', r.text);
console.log(r.warpFrame, r.totalFrames);")
read -r WARP TOTAL <<<"$FRAMES"
LAST=$((TOTAL - 2))   # the last frame a replay of TOTAL frames logs
echo "playtest: example rooms 983/984, warp at $WARP, $TOTAL frames ($(wc -c <"$BUNDLE") byte bundle)"

h=$TMP/home; mkdir -p "$h"
XDG_DATA_HOME=$h/data XDG_CONFIG_HOME=$h/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    "${NORAND[@]}" "$BIN" --no-audio --turbo --replay "$TMP/rec.txt" --rooms "$BUNDLE" \
    --hash-log "$TMP/native.hash" --dump-state "$((WARP - 1)):$TMP/before.bin" \
    --dump-state "$LAST:$TMP/after.bin" "$ROM" >"$TMP/native.log" 2>&1
rc=$?
echo "native ($BIN):"
check "exit 0" "[ $rc -eq 0 ]"
check "bundle linked (rooms up to 984)" "grep -q 'room bundles linked: .*up to 984' '$TMP/native.log'"
check "warp into room 983 delivered" "grep -q 'warp: player 0 -> room 983 ' '$TMP/native.log'"
check "took the meadow's door: alive in room 984 at the end" "grep -Eq 'at exit: player 0 in room 984, hp [1-9]' '$TMP/native.log'"
check "new rooms' progress set (port-owned): 287 visited+door, 288 visited" \
      "grep -Eq 'new-room progress.* 287:8001 288:8000' '$TMP/native.log'"
check "every frame logged" "[ \$(wc -l <'$TMP/native.hash') -eq $((TOTAL - 1)) ]"
check "sram hash constant from the warp to the end" \
      "[ \$(awk -v w=$WARP '\$1 >= w - 1 { print \$5 }' '$TMP/native.hash' | sort -u | wc -l) -eq 1 ]"
check "vanilla save untouched: gVisitedDoors + save memory byte-identical before/after" "python3 - '$TMP/before.bin' '$TMP/after.bin' <<'EOF'
import sys
a, b = (open(p, 'rb').read() for p in sys.argv[1:3])
vd = slice(0x28CA0, 0x28CA0 + 0x240)
# DumpTo (platform/inputlog.c): EWRAM, IWRAM, IO, DMA (4*7 words), 12, PLTT,
# VRAM, OAM, 64, then SRAM -- and the port sim block after it.
s0 = 0x40000 + 0x8000 + 0x400 + 4 * 7 * 4 + 12 + 0x400 + 0x18000 + 0x400 + 64
sram = slice(s0, s0 + 0x10000)
sys.exit(0 if len(a) == len(b) and a[vd] == b[vd] and a[sram] == b[sram] else 1)
EOF"

if [ "$WASM" = 1 ]; then
    if [ ! -f "$NODE_MODULE" ]; then
        echo "wasm: no $NODE_MODULE (make build/katam-node.js) -- FAIL"; fail=1
    else
        (cd "$ROOT" && REPLAY="$TMP/rec.txt" ROOMS="$BUNDLE" HASH_LOG="$TMP/wasm.hash" AUDIO=0 \
            node tools/headless_test.js "$NODE_MODULE" "$ROM" "$TOTAL") >"$TMP/wasm.log" 2>&1
        echo "wasm ($NODE_MODULE):"
        check "warp into room 983 delivered" "grep -q 'warp: player 0 -> room 983 ' '$TMP/wasm.log'"
        check "alive in room 984 at the end" "grep -Eq 'at exit: player 0 in room 984, hp [1-9]' '$TMP/wasm.log'"
        check "every frame logged" "[ \$(wc -l <'$TMP/wasm.hash') -eq $((TOTAL - 1)) ]"
        d=$(awk 'NR==FNR { k[$1] = $2 " " $4 " " $5 " " $6; next }
                 !($1 in k) || k[$1] != $2 " " $4 " " $5 " " $6 { print $1; exit }' "$TMP/native.hash" "$TMP/wasm.hash")
        check "native == wasm on every frame (keys vid sram pix)${d:+ -- first differs at frame $d}" "[ -z '$d' ]"
    fi
fi

[ "$fail" = 0 ] && echo "playtest: OK" || { echo "playtest: FAILED (logs: rerun with the temp dir kept)"; }
exit $fail
