#!/usr/bin/env bash
#
# layout_neutral.sh -- is build B the same program as build A, up to code layout?
#
#   tests/layout_neutral.sh A/katam B/katam [recording...]
#
# The proof a behaviour-neutral refactor owes when it moves code (and so moves
# the `sim` column of the hash log, which is not layout-blind: see
# tools/layout_diff.py for the two mechanisms).  For every recording:
#
#   1. keys, vid, sram and pix must match on EVERY frame (hash logs);
#   2. at the first frame whose sim hash differs, and at up to 15 frames
#      spread over the run, raw state dumps (PORT_HASH_RAW=1) of the two
#      builds must differ only in words tools/layout_diff.py explains as the
#      same host pointer, or stale bytes of one, spelled at two layouts.
#
# Both are native LP64/ILP32 builds of this tree (nm must read them).  Needs
# ROM=/path/to/rom.gba.  Fresh XDG dirs and setarch -R per run, as regress.sh.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
A=$(realpath "${1:?build A}"); B=$(realpath "${2:?build B}"); shift 2
ROM=${ROM:?set ROM=/path/to/rom.gba}
NAMES=("$@")
[ ${#NAMES[@]} -eq 0 ] && for r in "$HERE"/recordings/*.txt; do NAMES+=("$(basename "$r" .txt)"); done

NORAND=()
command -v setarch >/dev/null && setarch -R true 2>/dev/null && NORAND=(setarch -R)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

run() {  # bin out rec [args...]
    local bin=$1 out=$2 rec=$3; shift 3
    local d; d=$(mktemp -d -p "$TMP")
    XDG_DATA_HOME=$d/data XDG_CONFIG_HOME=$d/config SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "${NORAND[@]}" "$bin" --no-audio --turbo --replay "$HERE/recordings/$rec.txt" \
        "$@" "$ROM" >"$out.log" 2>&1
}

status=0
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
        echo "$r: keys/vid/sram/pix differ at frame $other -- NOT layout-neutral"
        status=1
        continue
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
    bad=0; explained=0
    for f in "${picks[@]}"; do
        out=$(cd "$ROOT/tools" && python3 layout_diff.py --a-bin "$A" --b-bin "$B" \
                  "$TMP/a.$r.$f.raw" "$TMP/b.$r.$f.raw")
        if [ $? -ne 0 ]; then
            echo "$r: frame $f: $(echo "$out" | head -8)"; bad=1
        fi
        explained=$((explained + 1))
    done
    if [ $bad = 0 ]; then
        simnote="identical"
        [ -n "$first" ] && simnote="first differs at frame $first"
        printf '%-14s layout-neutral: %s frames keys/vid/sram/pix identical; sim %s; %d dumps explained\n' \
            "$r" "$frames" "$simnote" "$explained"
    else
        status=1
    fi
done
exit $status
