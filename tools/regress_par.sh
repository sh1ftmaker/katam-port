#!/usr/bin/env bash
#
# regress_par.sh -- tests/regress.sh with every recording in its own process,
# RP_JOBS at a time (default: all), judged as below.
#
#   tools/regress_par.sh native|wasm ROM
#
# native: `make regress` (--mode sim on build/lp64/katam); every recording OK,
#         or DIVERGED in the sim column ONLY (keys match, and vid/sram/pix --
#         printed as "also:" when they differ -- match too).  The sim hash is
#         not layout-blind: aim@3318 diverges on every build, and which others
#         do depends on the build (its source path, the commit), so R11's
#         fixed "aim only" list fails builds made outside the checkout the
#         goldens came from.  tests/layout_neutral.sh proves such a run is
#         the same program; this only accepts sim-only divergence.
# wasm:   `make regress-wasm` (build/katam-node.js); every recording OK.
# Prints regress.sh's line per recording in name order; exit 1 on anything else.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODE=${1:?native or wasm}; ROM=${2:?ROM}
KNOWN='^[a-z_0-9]+ +DIVERGED +first divergent frame: sim@[0-9]+$'
args=(--mode sim)
[ "$MODE" = wasm ] && { args=(--wasm); KNOWN='^$'; }
names=()
for r in "$ROOT"/tests/recordings/*.txt; do
    grep -q '^# no-regress' "$r" || names+=("$(basename "$r" .txt)")
done
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
for n in "${names[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "${RP_JOBS:-${#names[@]}}" ]; do wait -n; done
    ( ROM=$ROM bash "$ROOT/tests/regress.sh" "${args[@]}" "$n" >"$T/$n.out" 2>&1; echo $? >"$T/$n.rc" ) &
done
wait
status=0
for n in "${names[@]}"; do
    line=$(grep -E "^$n( |:)" "$T/$n.out" | head -1)
    echo "${line:-$n: no verdict line (exit $(cat "$T/$n.rc"))}"
    if [[ "$line" =~ ^$n\ +OK\  ]]; then continue; fi
    if [[ -n "$line" && "$(echo "$line" | sed 's/ *$//')" =~ $KNOWN ]]; then echo "    (sim only: keys/vid/sram/pix match -- a layout difference)"; continue; fi
    status=1
    sed -n '2,6p' "$T/$n.out" | sed 's/^/    /'
done
echo "${#names[@]} recordings, $MODE: $([ $status = 0 ] && echo 'as baseline' || echo DIFFERS)"
exit $status
