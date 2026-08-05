#!/usr/bin/env bash
#
# native_smoke.sh -- boot the native build with a real ROM and prove it reaches
# gameplay, with no window, no audio device and nobody watching.
#
#   bash tools/native_smoke.sh build/native/katam ~/roms/your-copy.gba
#
# This is the native counterpart of tools/headless_test.js, and it exists for
# the same reason: "does it get past GameInit" has to be answerable without a
# person.  It is also what the Windows, macOS and arm64 builds should be
# measured against -- if this passes there, that port works.
#
# What it actually checks, in order:
#
#   1. the binary starts, reserves the GBA memory map, and loads the ROM;
#   2. it survives 200 frames of boot without a fatal signal;
#   3. mashing Start and A gets it through the title screen and file select
#      into a level -- the same button pattern tools/headless_test.js uses;
#   4. it drew a real picture rather than a flat colour;
#   5. the picture kept changing;
#   6. the input got it somewhere the boot run never went.
#
# The last three are the ones that matter, and each of the first two attempts
# at them was wrong in a way worth writing down.
#
# *Colours, from the best frame of the run rather than the last one.*  The last
# frame was tried first and is actively misleading: the game fades the screen
# constantly, between the logo and the title and on every room transition, and
# a brightness fade collapses the palette to a handful of greys.  A run that
# walked Kirby across a level and happened to stop mid-transition scored 47
# colours and failed a check it should have passed.
#
# *And a low threshold.*  The obvious guess -- "a level must have far more
# colours than a menu" -- is false, and measuring said so: the title screen
# peaks at 192 distinct colours and a level frame has about 150.  It is a
# 4-bit-per-pixel console with a 512-entry palette; there is no headroom for
# the intuition.  So the colour count only answers "is anything being drawn at
# all" (a forced blank is 1, a fading logo a few dozen), and the question of
# whether the game actually got anywhere is asked separately.
#
# *Motion, and DISPCNT.*  A port that wedges after the title screen draws a
# perfectly good picture forever.  What it does not do is keep producing
# different ones, and it does not reconfigure the display.  So the play run has
# to end with the display set up differently from the boot run -- which is what
# leaving the title screen for a level means in hardware terms -- and both runs
# have to show the picture changing.
#
# Nothing here writes a ROM anywhere, and the screenshot is a PNG of the
# framebuffer, which is the port's own output and not game data.
set -uo pipefail

BIN=${1:?usage: native_smoke.sh <katam binary> <rom.gba>}
ROM=${2:?usage: native_smoke.sh <katam binary> <rom.gba>}
OUT=${OUT:-build/native-smoke}
FRAMES=${FRAMES:-1400}
MIN_COLOURS=${MIN_COLOURS:-120}
MIN_MOTION=${MIN_MOTION:-10}

[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM"; exit 2; }

mkdir -p "$OUT"

# Boot needs no input; the title screen and the file-select menu each need a
# button tapped repeatedly, because the game edge-triggers confirmation and a
# held button registers once.  17 is A (1) plus Right (16), tapped together on
# a 20-frame period: A works the menus, Right walks Kirby once a level is
# running, and tapping both rather than holding Right is what keeps the file
# cursor still while the menus are up.  It is the same pattern
# docs/DECOMP-REQUESTS.md drives the web build with.
run() {
    local name=$1; shift
    echo "--- $name"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        "$BIN" "$ROM" --no-audio --turbo "$@" 2>&1 | tee "$OUT/$name.log"
    return "${PIPESTATUS[0]}"
}

fail=0

# Two runs, because they answer different questions and a single one that
# passes both by accident would be indistinguishable from either.
#
#   boot     no input at all -- does the game get to a drawn title screen?
#   play     tap A and Right -- does it get through the menus into a level?
run boot --frames 400 --screenshot "$OUT/boot.png" || fail=1
run play --frames "$FRAMES" --mash "120:17:20" \
         --screenshot "$OUT/play.png" || fail=1

field() {   # field <log> <report line> <sed extractor>
    grep -m1 "$2" "$OUT/$1.log" 2>/dev/null | sed "$3"
}

check() {
    local name=$1
    local colours motion best

    best=$(grep -m1 'best frame:' "$OUT/$name.log" || true)
    if [ -z "$best" ]; then
        echo "FAIL $name: the run produced no frame report at all -- it did not"
        echo "     reach a clean exit.  See $OUT/$name.log"
        return 1
    fi
    colours=$(field "$name" 'best frame:'  's/.*: \([0-9]*\) distinct.*/\1/')
    motion=$( field "$name" 'motion:'      's/.*: \([0-9]*\) distinct.*/\1/')
    echo "     $name: $colours colours at best, $motion distinct pictures," \
         "final DISPCNT $(field "$name" 'final frame:' 's/.*DISPCNT=//')"

    if [ "${colours:-0}" -lt "$MIN_COLOURS" ]; then
        echo "FAIL $name: $colours distinct colours at best (< $MIN_COLOURS) --"
        echo "     the port is presenting frames but never drew anything."
        echo "     See $OUT/$name.png"
        return 1
    fi
    if [ "${motion:-0}" -lt "$MIN_MOTION" ]; then
        echo "FAIL $name: only $motion distinct pictures (< $MIN_MOTION) -- the"
        echo "     game is wedged, not running."
        return 1
    fi
    return 0
}

echo
check boot || fail=1
check play || fail=1

boot_disp=$(field boot 'final frame:' 's/.*DISPCNT=//')
play_disp=$(field play 'final frame:' 's/.*DISPCNT=//')
if [ -n "$boot_disp" ] && [ "$boot_disp" = "$play_disp" ]; then
    echo "FAIL both runs ended with DISPCNT=$boot_disp -- the input never took"
    echo "     the game anywhere the boot run did not also reach.  Either the"
    echo "     key path is broken or the mash pattern no longer fits the menus."
    fail=1
fi

# Anything the port itself called wrong.  A missing function or a DMA that
# leaves the map is not a smoke-test failure -- the web build reports a few of
# each and plays fine -- but it belongs in the output, because a *new* one is
# how a platform port announces that it broke something.
echo
grep -h 'katam-port\]' "$OUT"/*.log \
    | grep -Ei 'missing|unimplemented|leaves the map|could not|cannot|refus' \
    | sort -u | sed 's/^/     /' || true

echo
if [ "$fail" -ne 0 ]; then
    echo "SMOKE TEST FAILED"
    exit 1
fi
echo "smoke test passed -- screenshots in $OUT"
