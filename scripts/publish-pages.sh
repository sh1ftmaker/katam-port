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
set -euo pipefail

cd "$(dirname "$0")/.."

DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

BRANCH=${PAGES_BRANCH:-gh-pages}
DIST=build/dist
WORKTREE=build/gh-pages

if ! command -v node >/dev/null 2>&1; then
    for c in "$HOME"/.nvm/versions/node/*/bin; do
        [ -x "$c/node" ] && PATH="$c:$PATH" && break
    done
fi
export PATH

step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

step "build and check the publishable directory"
make dist
make check-dist

step "stage $BRANCH"
# A detached worktree keeps the branch's contents completely separate from the
# source tree -- no risk of a stray build artefact or a ROM being swept in by a
# careless `git add` on the main branch.
rm -rf "$WORKTREE"
if git show-ref --quiet "refs/heads/$BRANCH"; then
    git worktree add -q "$WORKTREE" "$BRANCH"
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
# Only that subdirectory is cleared.  Wiping the whole worktree here -- which
# is what the root publish does, and must keep doing -- would take the main
# build down every time a branch was published.
if [ -n "${PAGES_SUBDIR:-}" ]; then
    case "$PAGES_SUBDIR" in
        */*|.*|"") echo "PAGES_SUBDIR must be a single plain directory name" >&2; exit 1 ;;
    esac
    TARGET="$WORKTREE/$PAGES_SUBDIR"
    rm -rf "$TARGET"
    mkdir -p "$TARGET"
    echo "    publishing into /$PAGES_SUBDIR/ (site root left alone)"
else
    TARGET="$WORKTREE"
    find "$WORKTREE" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
fi

cp "$DIST"/index.html "$DIST"/katam.js "$DIST"/katam.wasm "$TARGET"/
cp "$DIST"/robots.txt "$TARGET"/ 2>/dev/null || true
# Pages runs Jekyll by default, which ignores files it does not understand.
touch "$WORKTREE/.nojekyll"

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

step "push $BRANCH"
git -C "$WORKTREE" push -q origin "$BRANCH"
git worktree remove --force "$WORKTREE"

step "ensure Pages is serving that branch"
repo=$(gh repo view --json nameWithOwner -q .nameWithOwner)
if ! gh api "repos/$repo/pages" >/dev/null 2>&1; then
    gh api -X POST "repos/$repo/pages" -f "source[branch]=$BRANCH" -f "source[path]=/" >/dev/null
    echo "    enabled Pages on $BRANCH"
else
    gh api -X PUT "repos/$repo/pages" -f "source[branch]=$BRANCH" -f "source[path]=/" >/dev/null
    echo "    Pages already enabled; source set to $BRANCH"
fi

url=$(gh api "repos/$repo/pages" -q .html_url 2>/dev/null || echo "")
echo
echo "    $url"
echo "    (first publish can take a minute to go live)"
