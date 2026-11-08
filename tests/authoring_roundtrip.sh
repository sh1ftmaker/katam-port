#!/usr/bin/env bash
#
# authoring_roundtrip.sh -- the authoring layer's gate (docs/design/AUTHORING.md
# "Gates"), against the player's ROM.  Builds nothing; writes only under --dir.
#
#   tests/authoring_roundtrip.sh --bin build/lp64/katam [--dir build/authoring] [--no-regress]
#
#   1. extract the ROM's pack (--res-extract);
#   2. katam roundtrip: decompile ROM + pack into authoring files, compile them
#      back, compare every room/tileset/background/palette/object blob with the
#      ROM, diff the compiled pack against the extracted one, --res-check it;
#   3. compile the example mod (examples/authoring/sky_garden) on top;
#   4. replay the regression recordings with the game served from the compiled
#      vanilla pack, then from the pack with the example rooms added: the
#      goldens must hold (tests/regress.sh --mode sim).
#
# Needs ROM=/path/to/rom.gba.  Everything under --dir is ROM-derived: never
# commit or share it.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN=
DIR=$ROOT/build/authoring
REGRESS=1
while [ $# -gt 0 ]; do
    case $1 in
        --bin) BIN=$2; shift ;;
        --dir) DIR=$2; shift ;;
        --no-regress) REGRESS=0 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
ROM=${ROM:-}
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "no ROM -- set ROM=/path/to/your.gba"; exit 2; }
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "no native binary -- build one or pass --bin"; exit 2; }
command -v node >/dev/null || { echo "node (>= 18) is required"; exit 2; }
case $DIR in /*) ;; *) DIR=$PWD/$DIR ;; esac
KATAM="node $ROOT/tools/katam/katam.mjs"
status=0

ROM=$ROM bash "$HERE/res_roundtrip.sh" --bin "$BIN" --dir "$DIR/pack" --extract-only || { echo "extract failed"; exit 1; }
$KATAM roundtrip --rom "$ROM" --pack "$DIR/pack" --work "$DIR/rt" --bin "$BIN" || status=1
$KATAM compile "$ROOT/examples/authoring/sky_garden" --base "$DIR/rt/vanilla" -o "$DIR/sky_garden" >/dev/null || { echo "example compile failed"; status=1; }

if [ "$REGRESS" = 1 ] && [ $status = 0 ]; then
    echo "regress: game served from the compiled vanilla pack"
    ROM=$ROM KATAM_ARGS="--res-pack $DIR/rt/out/pack" bash "$HERE/regress.sh" --mode sim --bin "$BIN" || status=1
    echo "regress: game served from the compiled pack + example rooms"
    ROM=$ROM KATAM_ARGS="--res-pack $DIR/sky_garden/pack" bash "$HERE/regress.sh" --mode sim --bin "$BIN" || status=1
fi
[ $status = 0 ] && echo "authoring-roundtrip: OK" || echo "authoring-roundtrip: FAILED"
exit $status
