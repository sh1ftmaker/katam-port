#!/usr/bin/env bash
#
# netsim_xplat.sh -- portable snapshots, native LP64 <-> wasm (NETPLAY-V2 M2).
#
#   ROM=rom.gba tests/netsim_xplat.sh [recording...]     (default: all)
#
# Per recording, with a session from frame F0 (platform/netsim.c test driver):
#   1. native and node each save a full snapshot at frame S; the two images
#      must be byte-identical (tools/netsnap_diff.py);
#   2. node loads the NATIVE snapshot at frame L (early in boot, a different
#      state) and plays on from S; native loads the NODE snapshot likewise;
#   3. every frame from S on, keys/sim/vid/sram/pix of the run that loaded the
#      other platform's snapshot must equal the same platform's own unbroken
#      run (sim is compared within one platform; across platforms, step 1).
# Needs build/lp64/katam and build/katam-node.js.  Fresh XDG dirs, setarch -R.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$HERE")
ROM=${ROM:?set ROM=/path/to/rom.gba}
BIN=${KATAM_BIN:-$ROOT/build/lp64/katam}; NODE_MODULE=${KATAM_NODE:-$ROOT/build/katam-node.js}
F0=${F0:-50}; S=${S:-3000}; L=${L:-200}
NAMES=("$@"); [ ${#NAMES[@]} -eq 0 ] && for r in "$HERE"/recordings/*.txt; do NAMES+=("$(basename "$r" .txt)"); done
NORAND=(); command -v setarch >/dev/null && setarch -R true 2>/dev/null && NORAND=(setarch -R)
TMP=${KEEP_TMP:-$(mktemp -d)}; mkdir -p "$TMP"; [ -n "${KEEP_TMP:-}" ] || trap 'rm -rf "$TMP"' EXIT
nat() { local d; d=$(mktemp -d -p "$TMP"); XDG_DATA_HOME=$d/a XDG_CONFIG_HOME=$d/b SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "${NORAND[@]}" "$BIN" --no-audio --turbo "$@" "$ROM"; }
web() { (cd "$ROOT" && AUDIO=0 node tools/headless_test.js "$NODE_MODULE" "$ROM" "$1"); }
# the last line per frame (a load replays frames), frames >= S
tailfrom() { awk -v s="$S" '$1 >= s { l[$1] = $0 } END { for (f in l) print l[f] }' "$1" | sort -n; }
status=0
for r in "${NAMES[@]}"; do
    rec=$HERE/recordings/$r.txt; n=$(awk '/^frames/{print $2; exit}' "$rec")
    [ "$n" -gt $((S + 60)) ] || { echo "$r: shorter than S=$S, skipped"; continue; }
    nat --replay "$rec" --hash-log "$TMP/n.$r" --net-test "session:$F0,save:$S:$TMP/n.$r.snp" >"$TMP/n.$r.log" 2>&1 &
    REPLAY=$rec HASH_LOG=$TMP/w.$r NET_TEST=session:$F0 NET_SAVE=$S:$TMP/w.$r.snp web "$n" >"$TMP/w.$r.log" 2>&1 &
    wait
    if python3 "$ROOT/tools/netsnap_diff.py" "$TMP/n.$r.snp" "$TMP/w.$r.snp" >"$TMP/d.$r" 2>&1; then same=identical
    else same="DIFFER ($(sed -n 2p "$TMP/d.$r"))"; status=1; fi
    nat --replay "$rec" --hash-log "$TMP/nl.$r" --net-test "session:$F0,load:$L:$TMP/w.$r.snp" >"$TMP/nl.$r.log" 2>&1 &
    REPLAY=$rec HASH_LOG=$TMP/wl.$r NET_TEST=session:$F0 NET_LOAD=$L:$TMP/n.$r.snp web "$n" >"$TMP/wl.$r.log" 2>&1 &
    wait
    a=$(diff <(tailfrom "$TMP/n.$r") <(tailfrom "$TMP/nl.$r") | grep -c '^<')
    b=$(diff <(tailfrom "$TMP/w.$r") <(tailfrom "$TMP/wl.$r") | grep -c '^<')
    k=$(tailfrom "$TMP/n.$r" | wc -l)
    # frames both runs have (a loaded run is S-L frames longer: the host
    # counts its own frames, and the load moved the game's on)
    c=$(awk 'NR==FNR { g[$1] = $2" "$4" "$5" "$6; next } ($1 in g) && g[$1] != $2" "$4" "$5" "$6 { n++ } END { print n+0 }' <(tailfrom "$TMP/n.$r") <(tailfrom "$TMP/wl.$r"))
    printf '%-14s snapshot@%d %s (%s bytes native); native<-wasm: %d/%d frames differ; wasm<-native: %d/%d; native vs wasm<-native keys/vid/sram/pix: %d\n' \
        "$r" "$S" "$same" "$(stat -c %s "$TMP/n.$r.snp")" "$a" "$k" "$b" "$k" "$c"
    [ "$a" = 0 ] && [ "$b" = 0 ] && [ "$c" = 0 ] || status=1
done
exit $status
