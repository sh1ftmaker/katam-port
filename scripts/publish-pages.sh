#!/usr/bin/env bash
#
# publish-pages.sh -- publish the built page to GitHub Pages.
#
# The build cannot run in CI: it needs the decompilation checkout, and it reads
# the player's ROM to resolve the ARM addresses held in the game's function
# tables.  Neither belongs in a GitHub Action.  So the artefacts are built here
# and pushed to a `gh-pages` branch, which Pages serves as-is.
#
# The branch holds exactly what `make dist` produced: the page, the loader and
# the wasm.  No ROM -- `make check-dist` runs first and refuses otherwise.
#
# Usage: scripts/publish-pages.sh [--dry-run]
#
#   PAGES_REMOTE=github       push to this remote (default origin)
#   PAGES_SUBDIR=next         publish into /next/ and leave the rest alone
#   PAGES_SUBDIR=next/playtest  nested subdirectories are fine
#   PAGES_PAGE=playtest       publish the room playtest page (make
#                             dist-playtest, web/playtest/) instead of the game
#
# The staged tree starts from the remote's own gh-pages tip, in a detached
# worktree, and the push is a plain fast-forward -- never forced -- so a
# publish cannot drop anything someone else put on the branch.
set -euo pipefail

cd "$(dirname "$0")/.."

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

BRANCH=${PAGES_BRANCH:-gh-pages}
REMOTE=${PAGES_REMOTE:-origin}
DIST=build/dist
DIST_TARGET=dist
case "${PAGES_PAGE:-game}" in
    game) ;;
    playtest) DIST=build/dist-playtest; DIST_TARGET=dist-playtest ;;
    *) echo "PAGES_PAGE is game or playtest" >&2; exit 1 ;;
esac
WORKTREE=build/gh-pages

if ! command -v node >/dev/null 2>&1; then
    for c in "$HOME"/.nvm/versions/node/*/bin; do
        [ -x "$c/node" ] && PATH="$c:$PATH" && break
    done
fi
export PATH

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

step "build and check the publishable directory"
make $DIST_TARGET
make check-dist DIST=$DIST

step "stage $BRANCH"
# A detached worktree keeps the branch's contents completely separate from the
# source tree -- no risk of a stray build artefact or a ROM being swept in by a
# careless `git add` on the main branch.
git worktree remove --force "$WORKTREE" 2>/dev/null || true
rm -rf "$WORKTREE"
if git fetch -q "$REMOTE" "$BRANCH" 2>/dev/null; then
    # The remote's tip, not a local branch that may be stale.
    git worktree add -q --detach "$WORKTREE" FETCH_HEAD
elif [ -n "${PAGES_SUBDIR:-}" ]; then
    echo "cannot fetch $REMOTE/$BRANCH -- refusing to publish a subdirectory onto an empty branch" >&2
    exit 1
else
    git worktree add -q --detach "$WORKTREE"
    git -C "$WORKTREE" checkout -q --orphan "$BRANCH"
    git -C "$WORKTREE" rm -rqf . 2>/dev/null || true
fi

# PAGES_SUBDIR publishes into a subdirectory instead of the site root, so an
# experimental branch can be looked at side by side with the real thing:
#
#   PAGES_SUBDIR=qol scripts/publish-pages.sh   ->  .../katam-port/qol/
#
# Only that subdirectory is cleared; the root publish likewise clears only
# the root's own files, never another page's directory (see below).
if [ -n "${PAGES_SUBDIR:-}" ]; then
    # One or more plain segments: next, next/playtest.  No dots, no empty
    # segments, nothing that could climb out of the worktree.
    if ! printf '%s' "$PAGES_SUBDIR" | grep -Eq '^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*$'; then
        echo "PAGES_SUBDIR must be plain directory names joined by / (e.g. next/playtest)" >&2; exit 1
    fi
    TARGET="$WORKTREE/$PAGES_SUBDIR"
    echo "    publishing into /$PAGES_SUBDIR/ (site root left alone)"
else
    TARGET="$WORKTREE"
    echo "    publishing the site root (pages in subdirectories left alone)"
fi
# Clear what this publish owns, but keep nested publishes: a child directory
# with its own index.html (next/ under the root, next/playtest/ under next/,
# the old mp/ page) is another page, and publishing the parent must not take
# it down.  Child directories without a page are this publish's to clear.
mkdir -p "$TARGET"
find "$TARGET" -mindepth 1 -maxdepth 1 \( -type f -o -type l \) ! -name .git -exec rm -f {} +
for d in "$TARGET"/*/; do
    [ -d "$d" ] || continue
    [ -f "$d/index.html" ] && [ ! -e "$DIST/$(basename "$d")/index.html" ] && continue
    rm -rf "$d"
done

if [ "$DIST_TARGET" = dist-playtest ]; then
    # The whole directory: the page, the game, the compiler library, data.json.
    cp -r "$DIST"/. "$TARGET"/
fi
cp "$DIST"/index.html "$DIST"/katam.js "$DIST"/katam.wasm "$TARGET"/
cp "$DIST"/mp_net.js "$DIST"/rb_net.js "$DIST"/rb_boot.js "$TARGET"/ 2>/dev/null || true
cp "$DIST"/net_session.js "$DIST"/net_game.js "$DIST"/lobby.js "$TARGET"/ 2>/dev/null || true
cp "$DIST"/robots.txt "$TARGET"/ 2>/dev/null || true
# Pages runs Jekyll by default, which ignores files it does not understand.
touch "$WORKTREE/.nojekyll"

# check-dist guarded build/dist; this guards what is actually staged.
bad=$(find "$TARGET" -type f \( -iname '*.gba' -o -iname '*.gb' -o -iname '*.gbc' \
        -o -iname '*.bin' -o -iname '*.sav' -o -iname '*.srm' -o -iname '*.pack' -o -iname '*.katn' \
        -o \( -size +8M ! -name 'katam.wasm' \) \) -not -path '*/.git/*')
if [ -n "$bad" ]; then
    echo "REFUSING TO PUBLISH -- staged files look like game data:" >&2
    echo "$bad" >&2
    exit 1
fi

git -C "$WORKTREE" add -A
if git -C "$WORKTREE" diff --cached --quiet; then
    echo "    no change to publish"
else
    git -C "$WORKTREE" -c user.email=shiftmaker@gmail.com -c user.name=sh1ftmaker \
        commit -q -m "publish $(date -u +%Y-%m-%dT%H:%M:%SZ)"
fi

if [ "$DRY_RUN" = 1 ]; then
    step "dry run -- not pushing"
    git -C "$WORKTREE" log --oneline -1
    exit 0
fi

step "push $REMOTE $BRANCH (fast-forward only)"
git -C "$WORKTREE" push -q "$REMOTE" "HEAD:refs/heads/$BRANCH"
git worktree remove --force "$WORKTREE"

if [ -n "${PAGES_SUBDIR:-}" ]; then
    # A subdirectory rides on the site that is already being served; the
    # Pages configuration is the root publish's business.
    echo
    echo "    pushed /$PAGES_SUBDIR/ -- give Pages a minute to rebuild"
    exit 0
fi

step "ensure Pages is serving that branch"
repo=$(gh repo view --json nameWithOwner -q .nameWithOwner)
if ! gh api "repos/$repo/pages" >/dev/null 2>&1; then
    gh api -X POST "repos/$repo/pages" -f "source[branch]=$BRANCH" -f "source[path]=/" >/dev/null
    echo "    enabled Pages on $BRANCH"
else
    echo "    Pages already enabled; configuration left as it is"
fi

url=$(gh api "repos/$repo/pages" -q .html_url 2>/dev/null || echo "")
echo
echo "    $url"
echo "    (first publish can take a minute to go live)"
