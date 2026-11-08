#!/usr/bin/env bash
#
# res_roundtrip.sh -- the vanilla pack's round-trip gate (docs/design/MODDING.md
# §2.4, F0), against the player's ROM.  Builds nothing; writes only under --dir.
#
#   tests/res_roundtrip.sh --bin build/lp64/katam [--dir build/res/vanilla]
#   tests/res_roundtrip.sh ... --extract-only      just (re)extract
#
#   1. extract the ROM into DIR (pack.toml, pack.lock, the kind folders);
#   2. compile DIR and compare every compiled blob with the ROM range the
#      vanilla blob occupies: must be byte-identical (the binary exits 1 if not);
#   3. extract again into a second folder: must be identical file for file
#      (the extractor is deterministic; pack.lock covers every file);
#   4. negative control: change one field in a copy of DIR; the gate must fail.
#
# Needs ROM=/path/to/rom.gba.  The pack is ROM data: never commit or share it.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN=
DIR=$ROOT/build/res/vanilla
ONLY=0
while [ $# -gt 0 ]; do
    case $1 in
        --bin) BIN=$2; shift ;;
        --dir) DIR=$2; shift ;;
        --extract-only) ONLY=1 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done
ROM=${ROM:-}
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "no ROM -- set ROM=/path/to/your.gba"; exit 2; }
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "no native binary -- build one or pass --bin"; exit 2; }
case $DIR in /*) ;; *) DIR=$PWD/$DIR ;; esac

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
katam() {
    XDG_DATA_HOME=$TMP/data XDG_CONFIG_HOME=$TMP/config \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$BIN" --no-audio "$@" "$ROM" 2>&1 | grep '^\[res\]'
    return "${PIPESTATUS[0]}"
}

rm -rf "$DIR"
katam --res-extract "$DIR" || { echo "extract failed"; exit 1; }
[ "$ONLY" = 1 ] && exit 0

status=0
katam --res-check "$DIR" || { echo "F0: compiled pack differs from the ROM"; status=1; }

katam --res-extract "$TMP/again" >/dev/null || status=1
if diff -r "$DIR" "$TMP/again" >/dev/null; then
    echo "[res] re-extraction identical ($(wc -l < "$DIR/pack.lock") files in pack.lock)"
else
    echo "re-extraction differs"; status=1
fi

cp -r "$DIR" "$TMP/edited"
sed -i 's/"songIdx": \([0-9]*\)/"songIdx": 9\1/' "$TMP/edited/rooms/0101/room.json"
if katam --res-check "$TMP/edited" >/dev/null; then
    echo "negative control: an edited pack passed the gate"; status=1
else
    echo "[res] negative control: an edited room 101 fails the gate, as it must"
fi
[ $status = 0 ] && echo "res-roundtrip: OK"
exit $status
