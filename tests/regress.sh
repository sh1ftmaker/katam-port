#!/usr/bin/env bash
#
# regress.sh -- replay the committed input recordings and compare every frame
# against the golden hash logs.  Builds nothing.  docs/DETERMINISM.md.
#
#   tests/regress.sh                     sim mode, native build, all recordings
#   tests/regress.sh --mode pixels       compare the picture instead
#   tests/regress.sh --wasm              the node build, against the native goldens
#   tests/regress.sh --update            rewrite the goldens from this build
#   tests/regress.sh boot_to_level       just the named recording(s)
#
# Modes -- which hash-log columns must match (frame keys sim vid sram pix):
#
#   sim      keys and sim must match on every frame: the game's own state.
#            For renderer work, which must not change the simulation.  vid,
#            sram and pix are compared too and reported, but do not fail it.
#   pixels   pix must match: the picture.  Expected to move when the renderer
#            changes; this is how to see exactly where and from which frame.
#
# KATAM_ARGS adds options to every native run, e.g. KATAM_ARGS="--res-pack DIR"
# (make regress-pack) -- the goldens must still hold.
#
# --wasm compares the node build against the same native goldens: keys, vid
# and sram (and pix in pixels mode) must match.  Not sim -- a function pointer
# is a code address natively and a table index in wasm, so the two spell the
# same state differently (tools/state_diff.py classifies the difference).
#
# Each run gets a fresh XDG_DATA_HOME/XDG_CONFIG_HOME (the native host loads
# and writes a save there, and runs that share one diverge) and runs with ASLR
# off.  Needs your ROM: ROM=/path/to/rom.gba or --rom.
#
# KATAM_REGRESS_ARGS adds options to every native run and KATAM_VIDEODRIVER
# picks SDL's video driver -- the GPU renderer needs a GL context, which the
# dummy driver cannot give:
#   KATAM_VIDEODRIVER=offscreen KATAM_REGRESS_ARGS="--renderer gpu" tests/regress.sh
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
REC_DIR=$HERE/recordings
GOLD_DIR=$HERE/golden

MODE=sim
UPDATE=0
WASM=0
BIN=${KATAM_BIN:-}
NODE_MODULE=${KATAM_NODE:-$ROOT/build/katam-node.js}
ROM=${ROM:-}
NAMES=()

while [ $# -gt 0 ]; do
    case $1 in
        --mode) MODE=$2; shift ;;
        --mode=*) MODE=${1#--mode=} ;;
        --update) UPDATE=1 ;;
        --wasm) WASM=1 ;;
        --bin) BIN=$2; shift ;;
        --rom) ROM=$2; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        -*) echo "unknown option $1"; exit 2 ;;
        *) NAMES+=("$1") ;;
    esac
    shift
done

case $MODE in sim|pixels) ;; *) echo "--mode is sim or pixels"; exit 2 ;; esac
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "no ROM -- set ROM=/path/to/your.gba"; exit 2; }
if [ "$WASM" = 1 ]; then
    [ -f "$NODE_MODULE" ] || { echo "no $NODE_MODULE -- make build/katam-node.js"; exit 2; }
    [ "$UPDATE" = 0 ] || { echo "goldens come from the native build"; exit 2; }
else
    if [ -z "$BIN" ]; then
        for b in "$ROOT/build/lp64/katam" "$ROOT/build/native/katam"; do
            [ -x "$b" ] && { BIN=$b; break; }
        done
    fi
    [ -n "$BIN" ] && [ -x "$BIN" ] || { echo "no native binary -- build one or pass --bin"; exit 2; }
fi
if [ ${#NAMES[@]} -eq 0 ]; then
    for r in "$REC_DIR"/*.txt; do NAMES+=("$(basename "$r" .txt)"); done
fi

NORAND=()
command -v setarch >/dev/null && setarch -R true 2>/dev/null && NORAND=(setarch -R)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$GOLD_DIR"

run_native() {   # name out.hash -> exit status
    local h=$TMP/home-$1
    mkdir -p "$h"
    XDG_DATA_HOME=$h/data XDG_CONFIG_HOME=$h/config \
    SDL_VIDEODRIVER=${KATAM_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=dummy \
        "${NORAND[@]}" "$BIN" --no-audio --turbo --replay "$REC_DIR/$1.txt" \
        --hash-log "$2" ${KATAM_ARGS:-} ${KATAM_REGRESS_ARGS:-} "$ROM" >"$TMP/$1.log" 2>&1
}

run_wasm() {
    local n
    n=$(awk '/^frames/{print $2; exit}' "$REC_DIR/$1.txt")
    (cd "$ROOT" && REPLAY="$REC_DIR/$1.txt" HASH_LOG="$2" AUDIO=0 \
        node tools/headless_test.js "$NODE_MODULE" "$ROM" "$n") >"$TMP/$1.log" 2>&1
}

# first_diff golden got column -> "frame" of the first differing line, or ""
first_diff() {
    awk -v c="$3" 'NR==FNR { g[$1] = $c; n++; next }
                   ($1 in g) && g[$1] != $c { print $1; exit }' "$1" "$2"
}

status=0
start=$(date +%s)
for name in "${NAMES[@]}"; do
    [ -f "$REC_DIR/$name.txt" ] || { echo "$name: no such recording"; status=1; continue; }
    out=$TMP/$name.hash
    t0=$(date +%s)
    if [ "$WASM" = 1 ]; then run_wasm "$name" "$out"; else run_native "$name" "$out"; fi
    rc=$?
    secs=$(( $(date +%s) - t0 ))
    got=$(wc -l < "$out" 2>/dev/null || echo 0)

    if [ "$UPDATE" = 1 ]; then
        if [ $rc -ne 0 ]; then
            echo "$name: run failed (exit $rc after $got frames) -- golden NOT updated"
            tail -3 "$TMP/$name.log" | sed 's/^/    /'
            status=1
        else
            cp "$out" "$GOLD_DIR/$name.hash"
            echo "$name: golden written ($got frames, ${secs}s)"
        fi
        continue
    fi

    gold=$GOLD_DIR/$name.hash
    [ -f "$gold" ] || { echo "$name: no golden -- run with --update"; status=1; continue; }
    want=$(wc -l < "$gold")

    if [ "$WASM" = 1 ]; then
        must="2:keys 4:vid 5:sram"; info="6:pix"
        [ "$MODE" = pixels ] && { must="2:keys 6:pix"; info="4:vid 5:sram"; }
    elif [ "$MODE" = sim ]; then
        must="2:keys 3:sim"; info="4:vid 5:sram 6:pix"
    else
        must="2:keys 6:pix"; info="3:sim 4:vid 5:sram"
    fi

    verdict=OK
    detail=""
    for spec in $must; do
        f=$(first_diff "$gold" "$out" "${spec%%:*}")
        if [ -n "$f" ]; then
            verdict=DIVERGED
            detail="$detail ${spec#*:}@$f"
        fi
    done
    notes=""
    for spec in $info; do
        f=$(first_diff "$gold" "$out" "${spec%%:*}")
        [ -n "$f" ] && notes="$notes ${spec#*:}@$f"
    done
    if [ "$got" -lt "$want" ]; then
        verdict=DIVERGED
        detail="$detail ended-at-frame-$got-of-$want(exit-$rc)"
    elif [ $rc -ne 0 ]; then
        verdict=DIVERGED
        detail="$detail exit-$rc"
    fi

    if [ "$verdict" = OK ]; then
        printf '%-15s OK        %5d frames  %3ds%s\n' "$name" "$got" "$secs" \
            "${notes:+   (differs, not checked in this mode:$notes)}"
    else
        status=1
        printf '%-15s DIVERGED  first divergent frame:%s%s\n' "$name" "$detail" \
            "${notes:+   (also:$notes)}"
        [ $rc -ne 0 ] && tail -3 "$TMP/$name.log" | sed 's/^/    /'
    fi
done
echo "($(( $(date +%s) - start ))s, mode $MODE$([ "$WASM" = 1 ] && echo ', wasm'))"
exit $status
