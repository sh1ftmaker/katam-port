#!/usr/bin/env bash
#
# full_check.sh -- verify a build: every suite at once, one PASS/FAIL line each.
#
#   tools/full_check.sh ROM [--old OLD_NATIVE] [--deep] [--build] [--out DIR]
#                           [--jobs N] [--only a,b] [--skip a,b] [--list]
#
# Runs the suites in the SUITES table below in parallel, inside ONE budget of
# --jobs N slots (default 12) shared by all of them: a suite starts when its
# slots are free, in table order (the longest first), and uses at most that many
# processes itself (a browser page counts as one).  Each suite has its own
# log directory (DIR/<suite>/, log DIR/<suite>.log) and -- for the browser
# suites -- its own headless Chromium (own profile, own CDP port) and own room
# server; one static server serves build/dist and the ROM (from DIR, outside
# the tree) to all of them.  Servers and browsers are started with
# `sbg start fc<pid>-...` and exactly those are stopped at the end, also on
# failure or Ctrl-C.  Builds nothing unless --build (then once: web, dist,
# node module, native).  Exit status 1 if any suite fails.
#
#   --old BIN   also run tests/layout_neutral.sh BIN build/lp64/katam (every
#               recording); without it that suite is skipped.
#   --deep      the original long versions: the rig sequential as `make
#               netproof` runs it (with R11), the browser play at --play 130,
#               rooms gates and audit as before (they are the same either way).
#   --only/--skip  suite names (or prefixes ending in '*': --only 'web-*').
#   --smoke     the one-minute check instead (tools/smoke.sh): SMOKE_SUITES
#               below -- the 4-Kirby recordings native and wasm, a 16-device
#               online session in node, host-away grace on 4 and 8 devices,
#               the wide blocks run, lint, and one browser session of 10 tabs
#               on the default module (HOST GAME, JOIN GAME by code, 8 invite
#               links, play, a pause opened and closed); --jobs 32, about
#               60 s wall with the artefacts already built.
#
# Ports: servers from 18600-18700, CDP from 19500-19600 (first free ones).
# ROM-derived output (screenshots, dumps, logs) stays in DIR -- by default
# ../netproof-eval/fast-tmp/check-<time> next to the checkout.
#
# Adding a suite is one line in SUITES:  NAME  SLOTS  NEEDS  COMMAND
#   SLOTS: processes it may run at once (capped at --jobs); the command gets
#          it as $W (pass it to the suite's own --jobs)
#   NEEDS: -    nothing
#          web  a browser and a room server: the command gets $CDP $BASE
#               $ROM_URL $RELAY
#          web2 as web, plus a second room server without the public game
#               list (--no-public), as $RELAY_OLD (the server browser's case)
#          old  --old was given: the command gets $OLD
#   Modules: the browser suites run the 4-Kirby module (?mod=4) unless the
#   line says --mod 16 or --mod default (no ?mod: the page's default, the
#   16-Kirby module) or the case names its own -- web-*-default and
#   web-menu-exits-e test the default page, web-on-eight the 16-player games (5-6 tabs).
#   In COMMAND: $ROM, $D (this suite's own directory), $ROOT (the checkout,
#   also the cwd), $W (its slots), $PLAY (browser play seconds:
#   fast 130, deep 130 -- see below), $DEEP (1 with --deep, else empty), $RIG
#   (the rig command for this mode).  CPU-bound suites (NEEDS '-' and 'old')
#   run under `nice -n 10` so the real-time browser suites keep their pace.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

SUITES=$(cat <<'EOF'
# name             slots needs  command                      (longest first)
web-on-three       3     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases three
web-on-eight       3     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases eight
web-on-menu        2     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases menu,drop --play $PLAY
rig                4     -      $RIG --jobs $W --rom "$ROM" --out "$D" --skip-r11
web-rooms-2        2     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases rooms --rooms-pages 2
web-dev-lmp        1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases lmp
audit              3     -      python3 tools/netaudit/matrix.py --rom "$ROM" --eval "$D" --jobs $W --expect tools/netaudit/expected_verdicts.json $AUDIT_DUMPS
rooms-gates        3     -      node tools/rooms/gates_parallel.mjs --rom "$ROM" --out "$D" --only two,three,four,join,local2,hostlocal2,hint --seeds 1,2,3 --long 2 --jobs $W
rooms-corrupt      2     -      node tools/rooms/gates_parallel.mjs --rom "$ROM" --out "$D" --only corrupt,pool --seeds 1 --jobs $W
web-dev-local      1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases local
web-on-relay       2     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases relay
web-rooms-3        3     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases rooms --rooms-pages 3
web-dev-hudplaces  1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases hudplaces
layout-neutral     4     old    LN_JOBS=$(( (W + 1) / 2 )) ROM="$ROM" tests/layout_neutral.sh "$OLD" build/lp64/katam
regress-wasm       3     -      RP_JOBS=$W tools/regress_par.sh wasm "$ROM"
regress            3     -      RP_JOBS=$W tools/regress_par.sh native "$ROM"
web-dev-solo       1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases solo
web-dev-touch      1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases touch
web-dev-touchset   1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases touchset
web-dev-frontend   1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases frontend
web-on-errors      1     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases errors
web-dev-gl         1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases gl
web-menu-exits-a   1     web    node tools/menu_exits_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases front,joinmsg,cards
web-menu-exits-b   1     web    node tools/menu_exits_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases pause
web-menu-exits-c   1     web    node tools/menu_exits_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases local,phone,wide
web-menu-exits-d   1     web    node tools/menu_exits_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases kirbys
web-on-exits       4     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases exits --play 30
web-on-browse      3     web2   node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --relay-old $RELAY_OLD --shots "$D" --cases browse
web-dev-picker     1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases picker
web-dev-default    1     web    node tools/devices_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --shots "$D" --cases solo,lmp,local --mod default
web-menu-exits-e   1     web    node tools/menu_exits_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases default,local --mod default
web-on-default     3     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases default
rig-r16            2     -      $RIG --jobs $W --rom "$ROM" --out "$D" --only R16 --skip-r11
rig-r16w           2     -      $RIG --jobs $W --rom "$ROM" --out "$D" --only R16 --skip-r11 --module build/w16/katam-node.js
rig-pause          2     -      node tools/netproof/pause_test.mjs --rom "$ROM"
rig-lwarp          1     -      node tools/netproof/lwarp_test.mjs --rom "$ROM"
wide-stars         2     -      tools/wide_native.sh $W && ROM="$ROM" tests/wide_stars.sh build/w16n/katam
wide-blocks        2     -      tools/wide_native.sh $W && ROM="$ROM" tests/wide_blocks.sh build/w16n/katam
wide-ghosts        1     -      tools/wide_native.sh $W && ROM="$ROM" tests/wide_ghosts.sh build/w16n/katam
cpu-ai             3     -      ROM="$ROM" tests/cpu_ai.sh build/lp64/katam
rig-hostaway       3     -      node tools/netproof/hostaway_test.mjs --rom "$ROM" --jobs $W --out "$D"
web-on-hostblip    4     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases hostblip
web-on-netq        3     web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases netq
rig-badlink        5     -      node tools/netproof/badlink_test.mjs --rom "$ROM" --jobs $W --out "$D"
paint-unit         1     -      mkdir -p "$D/home" && XDG_DATA_HOME="$D/home" XDG_CONFIG_HOME="$D/home" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KATAM_PAINT_SELFTEST=1 build/lp64/katam --no-audio --turbo --frames 600 "$ROM" > "$D/paint.log" 2>&1; grep paint-test "$D/paint.log"; grep -q "checks, 0 failed" "$D/paint.log"
lint               1     -      make -s lint-determinism && python3 tools/check_sim_state.py build/lp64
input-devices      1     -      make -s input-devices-test
netplay            1     -      cd netplay && npm test --silent
EOF
)
# --smoke (tools/smoke.sh): the one-minute check -- the highest-value paths,
# each cut short, all at once (default --jobs 32, browser play 8 s)
SMOKE_SUITES=$(cat <<'EOF'
# name             slots needs  command                      (longest first)
web-smoke          10    web    node tools/online_browser_test.mjs --cdp $CDP --base $BASE --rom-url $ROM_URL --relay $RELAY --shots "$D" --cases smoke --mod default --play $PLAY --smoke-tabs 10
rig-hostaway       2     -      node tools/netproof/hostaway_test.mjs --rom "$ROM" --jobs $W --out "$D" --only '4dev-together-cut5,8dev-freeroam-cut5'
regress-wasm       7     -      RP_JOBS=$W tools/regress_par.sh wasm "$ROM"
regress            7     -      RP_JOBS=$W tools/regress_par.sh native "$ROM"
rig-sixteen        1     -      node tools/netproof/sixteen_test.mjs --rom "$ROM" --cases freeroam --frames 900
lint               1     -      make -s lint-determinism && python3 tools/check_sim_state.py build/lp64
wide-blocks        2     -      tools/wide_native.sh $W && ROM="$ROM" tests/wide_blocks.sh build/w16n/katam
netplay            1     -      cd netplay && npm test --silent
wire               1     -      node tools/ln_wire_test.cjs
input-devices      1     -      make -s input-devices-test
EOF
)

usage() { sed -n '3,47p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
ROM=""; OLD=""; DEEP=""; BUILD=""; OUT=""; ONLY=""; SKIP=""; LIST=""; JOBS=""; SMOKE=""
while [ $# -gt 0 ]; do
    case $1 in
        --smoke) SMOKE=1 ;;
        --old) OLD=$(realpath "$2"); shift ;;
        --deep) DEEP=1 ;;
        --jobs) JOBS=$2; shift ;;
        --build) BUILD=1 ;;
        --out) OUT=$2; shift ;;
        --only) ONLY=$2; shift ;;
        --skip) SKIP=$2; shift ;;
        --list) LIST=1 ;;
        -h|--help) usage ;;
        -*) echo "unknown option $1"; usage ;;
        *) [ -z "$ROM" ] && ROM=$(realpath "$1") || usage ;;
    esac
    shift
done
if [ -n "$SMOKE" ]; then SUITES=$SMOKE_SUITES; JOBS=${JOBS:-32}; else JOBS=${JOBS:-12}; fi
if [ -n "$LIST" ]; then echo "$SUITES" | grep -v '^#' | awk '{print $1, $2, $3}'; exit 0; fi
[ -n "$ROM" ] && [ -f "$ROM" ] || usage
OUT=$(realpath -m "${OUT:-$ROOT/../netproof-eval/fast-tmp/$([ -n "$SMOKE" ] && echo smoke || echo check)-$(date +%Y%m%d-%H%M%S)}")
case $OUT in "$ROOT"/*) echo "--out must be outside the tree (ROM-derived output)"; exit 2 ;; esac
mkdir -p "$OUT/status"

# --- the mode -------------------------------------------------------------
[ "$JOBS" -ge 1 ] 2>/dev/null || { echo "--jobs N, N >= 1"; exit 2; }
PLAY=130
[ -n "$SMOKE" ] && PLAY=8
if [ -n "$DEEP" ]; then
    RIG="node tools/netproof.mjs"                 # as `make netproof`: one process (ignores --jobs)
    AUDIT_DUMPS=""                                # the report's byte-range columns too
else
    RIG="node tools/netproof/parallel.mjs"
    AUDIT_DUMPS="--no-dumps"                      # verdicts only (they are what is checked)
fi
export ROM OLD DEEP PLAY RIG ROOT AUDIT_DUMPS

# --- which suites -----------------------------------------------------------
match() {  # name list -> 0 if name is in the comma list (entries may end in *)
    local n=$1 IFS=, p
    for p in $2; do [[ $n == $p ]] && return 0; done
    return 1
}
NAMES=(); NEEDS=(); CMDS=(); SLOTS=()
while read -r name slots needs cmd; do
    [ -z "$name" ] || [[ $name == \#* ]] && continue
    [ -n "$ONLY" ] && ! match "$name" "$ONLY" && continue
    [ -n "$SKIP" ] && match "$name" "$SKIP" && continue
    if [ "$needs" = old ] && [ -z "$OLD" ]; then echo "SKIP $name (no --old)" >"$OUT/status/$name.skip"; continue; fi
    [ "$slots" -gt "$JOBS" ] && slots=$JOBS
    NAMES+=("$name"); NEEDS+=("$needs"); CMDS+=("$cmd"); SLOTS+=("$slots")
done <<<"$SUITES"
[ ${#NAMES[@]} -gt 0 ] || { echo "no suites selected"; exit 2; }

# --- servers and browsers: sbg, by name, stopped on the way out ------------
TAG=fc$$
STARTED="$OUT/status/.sbg"
: >"$STARTED"
SBG_LOCK="$OUT/status/.sbg.lock"
sbg_start() {  # name mem cmd... ; records the name for the cleanup
    local n=$TAG-$1 mem=$2; shift 2
    echo "$n" >>"$STARTED"
    sbg start "$n" --mem "$mem" -- "$@" >/dev/null
}
sbg_stop() { sbg stop "$TAG-$1" >/dev/null 2>&1 || true; }
killtree() { local c; for c in $(pgrep -P "$1"); do killtree "$c"; done; kill "$1" 2>/dev/null; }
cleanup() {
    trap - EXIT INT TERM
    local p
    for p in $(jobs -pr); do killtree "$p"; done
    while read -r n; do [ -n "$n" ] && sbg stop "$n" >/dev/null 2>&1; done <"$STARTED"
    wait 2>/dev/null
}
trap cleanup EXIT
trap 'echo "interrupted"; exit 130' INT TERM

# Ports handed out are kept machine-wide (two checks at once -- smoke.sh and
# full_check.sh, or two worktrees -- must not pick the same port in the gap
# before their servers listen): "port pid" lines, a dead pid's ignored.
PORTS_TAKEN=/tmp/katam-check-ports.$(id -u)
PORTS_LOCK=$PORTS_TAKEN.lock
port_free() {  # lo hi -> the first port nobody listens on and no live check has
    local p
    (
        flock 9
        touch "$PORTS_TAKEN"
        awk '{ if (system("kill -0 " $2 " 2>/dev/null") == 0) print }' "$PORTS_TAKEN" >"$PORTS_TAKEN.new" 2>/dev/null
        mv -f "$PORTS_TAKEN.new" "$PORTS_TAKEN"
        for p in $(seq "$1" "$2"); do
            grep -q "^$p " "$PORTS_TAKEN" && continue
            ss -ltnH "sport = :$p" 2>/dev/null | grep -q . && continue
            echo "$p $$" >>"$PORTS_TAKEN"; echo "$p"; exit 0
        done
        exit 1
    ) 9>"$PORTS_LOCK"
}
wait_http() {  # url seconds
    local i
    for i in $(seq 1 $(( $2 * 10 ))); do curl -sf -o /dev/null "$1" && return 0; sleep 0.1; done
    return 1
}

# --- build ------------------------------------------------------------------
T0=$(date +%s%N)
if [ -n "$BUILD" ]; then
    echo "building (log $OUT/build.log)"
    if ! ( set -e
           source ~/emsdk/emsdk_env.sh >/dev/null 2>&1
           make -j16 && make dist && make -j16 build/katam-node.js
           make -j16 BUILD=build/w16 KATAM_MAX_KIRBYS=16 build/w16/katam-node.js   # rig-pause's and rig-lwarp's 16-Kirby module
           cmake -S . -B build/lp64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKATAM_ALLOW_LP64=ON
           cmake --build build/lp64 -j16 ) >"$OUT/build.log" 2>&1; then
        echo "FAIL build ($(tail -3 "$OUT/build.log" | tr '\n' ' '))"; exit 1
    fi
fi
for f in build/dist/index.html build/dist/katam.wasm build/katam-node.js build/lp64/katam; do
    [ -e "$f" ] || { echo "missing $f -- build first (or --build)"; exit 1; }
done

# --- the static server (one, read-only, for every browser suite) -----------
if printf '%s\n' "${NEEDS[@]}" | grep -qxE 'web2?'; then
    mkdir -p "$OUT/www"
    ln -sfn "$ROOT/build/dist" "$OUT/www/dist"
    ln -sfn "$ROM" "$OUT/www/rom.gba"
    WWW_PORT=$(port_free 18600 18700) || { echo "no free server port"; exit 1; }
    sbg_start www 2G python3 -m http.server "$WWW_PORT" --bind 127.0.0.1 --directory "$OUT/www"
    wait_http "http://127.0.0.1:$WWW_PORT/dist/index.html" 20 || { echo "FAIL static server did not come up"; exit 1; }
    # the served wasm must be this build's
    cmp -s <(curl -sf "http://127.0.0.1:$WWW_PORT/dist/katam.wasm") build/dist/katam.wasm \
        || { echo "FAIL the static server does not serve this build's katam.wasm"; exit 1; }
    export BASE="http://127.0.0.1:$WWW_PORT/dist/" ROM_URL="http://127.0.0.1:$WWW_PORT/rom.gba"
fi

# --- one suite ---------------------------------------------------------------
run_suite() {  # index
    local i=$1 name=${NAMES[$1]} needs=${NEEDS[$1]} cmd=${CMDS[$1]} W=${SLOTS[$1]}
    export W
    local D="$OUT/$name" log="$OUT/$name.log" t0 rc=0 pre=()
    mkdir -p "$D"
    t0=$(date +%s%N)
    if [ "$needs" = web ] || [ "$needs" = web2 ]; then
        local cdp relay relay2=""
        cdp=$(port_free 19500 19600) && relay=$(port_free 18600 18700) || { echo "no free port" >"$log"; rc=1; }
        if [ $rc = 0 ] && [ "$needs" = web2 ]; then
            relay2=$(port_free $((relay + 1)) 18700) || { echo "no free port" >"$log"; rc=1; }
            [ $rc = 0 ] && sbg_start "$name-relay2" 2G node netplay/dev-relay.mjs "$relay2" --no-public
        fi
        if [ $rc = 0 ]; then
            mkdir -p "$D/profile"
            sbg_start "$name-relay" 2G node netplay/dev-relay.mjs "$relay"
            sbg_start "$name-chrome" 6G chromium --headless=new --ignore-gpu-blocklist --use-angle=gl-egl \
                --remote-debugging-port="$cdp" --user-data-dir="$D/profile" \
                --autoplay-policy=no-user-gesture-required --no-first-run --no-default-browser-check about:blank
            if ! wait_http "http://127.0.0.1:$cdp/json/version" 30; then echo "chromium on $cdp did not come up" >"$log"; rc=1; fi
            if [ $rc = 0 ] && ! (for k in $(seq 1 100); do (exec 3<>/dev/tcp/127.0.0.1/$relay) 2>/dev/null && exit 0; sleep 0.1; done; exit 1); then
                echo "room server on $relay did not come up" >"$log"; rc=1
            fi
        fi
        if [ $rc = 0 ]; then
            ( export CDP=$cdp RELAY="http://127.0.0.1:$relay" RELAY_OLD="http://127.0.0.1:$relay2" D; eval "$cmd" ) >"$log" 2>&1 </dev/null
            rc=$?
        fi
        sbg_stop "$name-chrome"; sbg_stop "$name-relay"
        [ -n "$relay2" ] && sbg_stop "$name-relay2"
        rm -rf "$D/profile"
    else
        ( export D; eval "nice -n 10 bash -c '$cmd'" ) >"$log" 2>&1 </dev/null
        rc=$?
    fi
    echo "$rc $(( ($(date +%s%N) - t0) / 1000000 ))" >"$OUT/status/$name"
}

echo "full_check: ${#NAMES[@]} suites, $JOBS slots$([ -n "$DEEP" ] && echo ' (deep)'), logs in $OUT"
# the scheduler: start suites in table order while their slots fit; a line when each ends
# (in table order; a suite that does not fit yet lets a smaller one behind it start)
free=$JOBS; done_n=0; STARTED_I=()
while [ $done_n -lt ${#NAMES[@]} ]; do
    for i in "${!NAMES[@]}"; do
        [ -n "${STARTED_I[$i]:-}" ] && continue
        [ "${SLOTS[$i]}" -le "$free" ] || continue
        STARTED_I[$i]=1; free=$(( free - SLOTS[i] ))
        printf '  [%4ds] %-18s start (%d slots, %d free)\n' $(( ($(date +%s%N) - T0) / 1000000000 )) "${NAMES[$i]}" "${SLOTS[$i]}" "$free"
        run_suite "$i" &
    done
    sleep 1
    for i in "${!NAMES[@]}"; do
        name=${NAMES[$i]}
        [ -f "$OUT/status/$name" ] && [ ! -f "$OUT/status/$name.seen" ] || continue
        touch "$OUT/status/$name.seen"; done_n=$((done_n + 1)); free=$(( free + SLOTS[i] ))
        read -r rc ms <"$OUT/status/$name"
        printf '  [%4ds] %-18s %s\n' $(( ($(date +%s%N) - T0) / 1000000000 )) "$name" "$([ "$rc" = 0 ] && echo done || echo "FAILED (rc $rc)")"
    done
done
wait

# --- the summary ---------------------------------------------------------------
summary() {  # name -> a short line from its log
    local log="$OUT/$1.log"
    grep -E '^[0-9]+ (ok|passed), [0-9]+ failed|gates, |runs, [0-9]+ failing|factors, |recordings, |tests? passed|^# (pass|fail)|layout-neutral|NOT layout' "$log" 2>/dev/null | tail -1 | cut -c1-90
}
fails=0
echo
for name in "${NAMES[@]}"; do
    read -r rc ms <"$OUT/status/$name"
    if [ "$rc" = 0 ]; then v=PASS; else v=FAIL; fails=$((fails + 1)); fi
    printf '%s %-18s %6.1fs  %s\n' "$v" "$name" "$(echo "$ms" | awk '{printf "%.1f", $1/1000}')" "$(summary "$name")"
    if [ "$rc" != 0 ]; then grep -E 'FAIL|UNEXPECTED|DIVERGED|Error|error' "$OUT/$name.log" | grep -v '^\s*ok' | head -4 | cut -c1-160 | sed 's/^/       /'; fi
done
for f in "$OUT"/status/*.skip; do [ -e "$f" ] && cat "$f"; done
total=$(( ($(date +%s%N) - T0) / 1000000 ))
printf '%s %d suites, %d failed, %.1fs wall\n' "$([ $fails = 0 ] && echo PASS || echo FAIL)" ${#NAMES[@]} $fails "$(echo $total | awk '{printf "%.1f", $1/1000}')"
[ $fails = 0 ]
