#!/usr/bin/env bash
#
# refresh_stubs.sh -- work out which functions are still ARM-only, and stub them.
#
# The decompilation is a moving target: functions gain C bodies while the port
# is being built.  So this list is never hand-maintained.  It is re-derived from
# a real link, every time:
#
#   1. drop any symbol the game now defines itself     (it got decompiled --
#      leaving the stub in place would be a duplicate symbol)
#   2. link, and collect what is still undefined
#   3. generate stubs, and repeat -- stubbing one function makes more code
#      reachable, which can expose further undefined symbols
#
# It converges in a handful of rounds.  A shrinking count between runs is the
# decompilation making progress.
set -uo pipefail

BUILD=${BUILD:-build}
PORT_SRC=${PORT_SRC:-$BUILD/port-src}
GENERATED=${GENERATED:-$BUILD/generated}
KATAM_DECOMP=${KATAM_DECOMP:-$HOME/Desktop/katam}
EMSDK=${EMSDK:-$HOME/emsdk}
CC=$EMSDK/upstream/emscripten/emcc
NM=$EMSDK/upstream/bin/llvm-nm

UNDEF=$BUILD/undefined.txt
STUBS=$GENERATED/stubs.c

mkdir -p "$GENERATED"

# Start from nothing every time.  Carrying the list between runs looks like an
# optimisation but is a trap: a symbol dropped from it (because something
# appeared to define it) never comes back, and stays undefined forever.  The
# loop below rediscovers the whole set in a few rounds.
: > "$UNDEF"
: > "$STUBS"

for round in $(seq 1 12); do
    # 1. Generate stubs for everything currently believed to be missing.
    python3 tools/gen_stubs.py --tree "$PORT_SRC" --symbols "$UNDEF" \
        --out "$STUBS" --asm-dir "$KATAM_DECOMP/asm" | sed 's/^/  /'

    # 2. Build, so the object files reflect those stubs.  Prune first: a
    #    stale object for a deleted source is still a definition.
    make prune >/dev/null 2>&1
    if ! make compile >"$BUILD/compile.log" 2>&1; then
        # Never link after a failed compile: the missing objects look exactly
        # like missing functions, and the loop would happily stub a file that
        # simply did not build.
        echo "compilation failed -- fix this before stubbing anything:" >&2
        grep -E "error" "$BUILD/compile.log" | head -5 >&2
        exit 1
    fi

    # 3. Drop anything the game now defines for itself.  A function that has
    #    since been decompiled would otherwise collide with its own stub --
    #    which is the normal way this list shrinks.
    objs=$(find "$BUILD/obj" -name '*.o' ! -name 'stubs.o')
    if [ -n "$objs" ]; then
        $NM --defined-only $objs 2>/dev/null | awk '{print $NF}' | sort -u > "$BUILD/defined.txt"
        if comm -12 "$UNDEF" "$BUILD/defined.txt" | grep -q .; then
            echo "  no longer missing (decompiled since):" \
                 "$(comm -12 "$UNDEF" "$BUILD/defined.txt" | tr '\n' ' ')"
            comm -23 "$UNDEF" "$BUILD/defined.txt" > "$UNDEF.tmp"
            mv "$UNDEF.tmp" "$UNDEF"
            continue    # regenerate without them before judging the link
        fi
    fi

    # 4. Link for real.  Any error at all means not done -- not just an
    #    undefined symbol.
    if $CC -sASYNCIFY $(find "$BUILD/obj" -name '*.o') -o "$BUILD/probe.js" \
            2>"$BUILD/link.log"; then
        echo "link is clean after $round round(s); $(wc -l < "$UNDEF") stubbed symbols"
        rm -f "$BUILD/probe.js" "$BUILD/probe.wasm"
        exit 0
    fi

    if ! grep -q "undefined symbol" "$BUILD/link.log"; then
        echo "link failed for a reason other than missing functions:" >&2
        grep -E "error" "$BUILD/link.log" | sort -u | head >&2
        exit 1
    fi

    # 5. Stubbing makes more code reachable, which can expose more gaps.
    grep -oE "undefined symbol: [A-Za-z_0-9]+" "$BUILD/link.log" \
        | sed 's/undefined symbol: //' >> "$UNDEF"
    sort -u -o "$UNDEF" "$UNDEF"
done

echo "still undefined after 12 rounds:" >&2
grep -oE "undefined symbol: [A-Za-z_0-9]+" "$BUILD/link.log" | sort -u >&2
exit 1
