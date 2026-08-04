#!/usr/bin/env bash
#
# release.sh -- sync from the decompilation, rebuild, and publish.
#
# The whole chain in one command, because the steps have to happen in this
# order and skipping one produces a build that looks fine and is stale:
#
#   sync    copy the decomp and adapt it            (new decompiled functions)
#   prune   drop objects whose source is gone       (or they still get linked)
#   stubs   re-derive what is still ARM-only        (shrinks as decomp lands)
#   build   compile and link
#   dist    assemble the publishable directory
#   check   refuse to publish anything ROM-shaped
#   deploy  upload to Cloudflare Pages
#   verify  fetch the live page back and compare it to what was built
#
# Usage:
#   scripts/release.sh              full chain, publishes
#   scripts/release.sh --dry-run    everything except the upload
#   scripts/release.sh --local      stop after build; no dist, no publish
set -euo pipefail

cd "$(dirname "$0")/.."

DRY_RUN=0
LOCAL_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --local)   LOCAL_ONLY=1 ;;
        -h|--help) sed -n '3,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

# make cannot find node: nvm installs it outside the default PATH, and make's
# shell does not read the user profile.  Resolve it here and export it, so both
# `npx wrangler` below and `make test` inherit a working PATH.
if ! command -v node >/dev/null 2>&1; then
    for candidate in "$HOME"/.nvm/versions/node/*/bin; do
        [ -x "$candidate/node" ] && PATH="$candidate:$PATH" && break
    done
fi
export PATH
command -v node >/dev/null 2>&1 || { echo "node not found" >&2; exit 1; }

EMSDK=${EMSDK:-$HOME/emsdk}
[ -x "$EMSDK/upstream/emscripten/emcc" ] || { echo "no emscripten at $EMSDK" >&2; exit 1; }

PROJECT=${PAGES_PROJECT:-katam-port}
ACCOUNT=${CF_ACCOUNT_ID:-}
SITE=${SITE_URL:-https://$PROJECT.pages.dev/}

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

step "sync from the decompilation"
make sync 2>&1 | grep -E 'C files copied|merged|INCBIN|relocated|stubbed|wrappers|resolved|function tables|inside ROM structs|storage copied|SKIPPED' || true

step "prune stale objects"
make prune

step "re-derive stubs"
make stubs

step "build"
make
ls -lh web/katam.wasm | awk '{print "    wasm: " $5}'

if [ "$LOCAL_ONLY" = 1 ]; then
    step "stopping after build (--local)"
    exit 0
fi

step "assemble and check the publishable directory"
make dist
make check-dist

if [ "$DRY_RUN" = 1 ]; then
    step "dry run -- not uploading"
    exit 0
fi

step "deploy to Cloudflare Pages ($PROJECT)"
${ACCOUNT:+CLOUDFLARE_ACCOUNT_ID=$ACCOUNT} npx --yes wrangler@latest pages deploy build/dist \
    --project-name="$PROJECT" --branch=main --commit-dirty=true 2>&1 | tail -3

step "verify the live page matches what was built"
# Pages serves the new deployment through the alias within a few seconds; retry
# rather than reporting a mismatch that is really just propagation.
for attempt in 1 2 3 4 5 6; do
    live=$(curl -fsS --max-time 45 "$SITE" 2>/dev/null | md5sum | cut -d' ' -f1) || live=""
    built=$(md5sum < build/dist/index.html | cut -d' ' -f1)
    if [ "$live" = "$built" ]; then
        echo "    live page matches the build ($built)"
        exit 0
    fi
    [ "$attempt" -lt 6 ] && sleep 10
done

echo "    live page does NOT match the build yet (propagation, or a failed upload)" >&2
echo "    built: ${built:-?}" >&2
echo "    live:  ${live:-unreachable}" >&2
exit 1
