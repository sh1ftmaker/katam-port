#!/usr/bin/env bash
#
# wide_w0.sh -- proof W0: is the wide build at four Kirbys the default program
# at a different layout?  (docs/design/ONE-WORLD-16.md)
#
#   tests/wide_w0.sh DEFAULT/katam WIDE4/katam [recording...]
#
# DEFAULT is a native build with KATAM_MAX_KIRBYS unset, WIDE4 one with
# KATAM_MAX_KIRBYS=4 (every per-player table moved into the port sim block).
# For every recording, as tests/layout_neutral.sh does for a code move:
#
#   1. keys, vid, sram and pix must match on EVERY frame (hash logs);
#   2. at the first frame whose sim hash differs and at 15 frames spread over
#      the run, a raw dump of WIDE4 is put back into the console layout
#      (tools/wide_unrelocate.py) and must then differ from DEFAULT's only in
#      words tools/layout_diff.py explains as host-pointer spellings.
#
# Prints PASS/FAIL per recording and a total.  Needs ROM=/path/to/rom.gba.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
A=$(realpath "${1:?default build}"); B=$(realpath "${2:?wide build at 4}"); shift 2
ROM=${ROM:?set ROM=/path/to/rom.gba}
NAMES=("$@")
[ ${#NAMES[@]} -eq 0 ] && for r in "$HERE"/recordings/*.txt; do NAMES+=("$(basename "$r" .txt)"); done

NORAND=()
command -v setarch >/dev/null && setarch -R true 2>/dev/null && NORAND=(setarch -R)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
MAP=$TMP/wide.map

run() {  # bin out rec [args...]
    local bin=$1 out=$2 rec=$3; shift 3
    local d; d=$(mktemp -d -p "$TMP")
    XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        KATAM_WIDE_MAP=$MAP "${NORAND[@]}" "$bin" --no-audio --turbo --replay "$HERE/recordings/$rec.txt" \
        "$@" "$ROM" >"$out.log" 2>&1
}

status=0; pass=0; fail=0; frames_total=0; dumps_total=0
for r in "${NAMES[@]}"; do
    run "$A" "$TMP/a.$r" "$r" --hash-log "$TMP/a.$r.hash" &
    run "$B" "$TMP/b.$r" "$r" --hash-log "$TMP/b.$r.hash" &
    wait
    frames=$(wc -l < "$TMP/a.$r.hash")
    other=$(paste -d' ' <(awk '{print $1,$2,$4,$5,$6}' "$TMP/a.$r.hash") \
                        <(awk '{print $1,$2,$4,$5,$6}' "$TMP/b.$r.hash") \
            | awk '{ if ($1" "$2" "$3" "$4" "$5 != $6" "$7" "$8" "$9" "$10) { print $1; exit } }')
    first=$(paste -d' ' "$TMP/a.$r.hash" "$TMP/b.$r.hash" \
            | awk '{ if ($3 != $9) { print $1; exit } }')
    [ "$(wc -l < "$TMP/b.$r.hash")" = "$frames" ] || other="length"
    if [ -n "$other" ]; then
        echo "FAIL $r: keys/vid/sram/pix differ at frame $other"
        status=1; fail=$((fail + 1)); continue
    fi
    picks=()
    [ -n "$first" ] && picks+=("$first")
    for k in $(seq 1 15); do picks+=($(( frames * k / 16 ))); done
    dargs_a=(); dargs_b=()
    for f in "${picks[@]}"; do
        dargs_a+=(--dump-state "$f:$TMP/a.$r.$f.raw")
        dargs_b+=(--dump-state "$f:$TMP/b.$r.$f.raw")
    done
    PORT_HASH_RAW=1 run "$A" "$TMP/ra.$r" "$r" "${dargs_a[@]}" &
    PORT_HASH_RAW=1 run "$B" "$TMP/rb.$r" "$r" "${dargs_b[@]}" &
    wait
    bad=0; n=0; ptrs=0; stale=0
    for f in "${picks[@]}"; do
        alen=$(stat -c %s "$TMP/a.$r.$f.raw")
        simlen=$((alen - 0x40000 - 0x8000 - 0x400 - 112 - 12 - 0x400 - 0x18000 - 0x400 - 64 - 0x10000))
        u=$(cd "$ROOT/tools" && python3 wide_unrelocate.py "$TMP/b.$r.$f.raw" "$MAP" "$simlen" "$TMP/u.$r.$f.raw" --explain "$TMP/a.$r.$f.raw") \
            || { echo "FAIL $r: frame $f: unrelocate: $u"; bad=1; continue; }
        ptrs=$((ptrs + $(echo "$u" | awk '{print $4}'))); stale=$((stale + $(echo "$u" | awk '{print $7}')))
        out=$(cd "$ROOT/tools" && python3 layout_diff.py --a-bin "$A" --b-bin "$B" \
                  "$TMP/a.$r.$f.raw" "$TMP/u.$r.$f.raw")
        if [ $? -ne 0 ]; then
            echo "FAIL $r: frame $f: $(echo "$out" | head -8)"; bad=1
        fi
        n=$((n + 1))
    done
    if [ $bad = 0 ]; then
        simnote="sim identical"
        [ -n "$first" ] && simnote="sim first differs at $first"
        printf 'PASS %-20s %6s frames keys/vid/sram/pix identical; %s; %d dumps equal after unrelocation (%d pointer words moved back, %d stale-pointer words)\n' \
            "$r" "$frames" "$simnote" "$n" "$ptrs" "$stale"
        pass=$((pass + 1)); frames_total=$((frames_total + frames)); dumps_total=$((dumps_total + n))
    else
        status=1; fail=$((fail + 1))
    fi
done
echo "W0: $pass passed, $fail failed; $frames_total frames identical, $dumps_total state dumps equal up to layout"
[ $status = 0 ] && echo "W0 PASS" || echo "W0 FAIL"
exit $status
