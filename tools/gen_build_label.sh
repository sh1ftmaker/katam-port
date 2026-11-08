#!/usr/bin/env bash
# gen_build_label.sh OUT.h -- the commit and date the settings menu (and the
# page) show, so a tester can tell which build they are on.  Rewritten only
# when it changes, so it does not force a rebuild every time.
set -euo pipefail
out=$1
cd "$(dirname "$0")/.."
sha=$(git rev-parse --short=9 HEAD 2>/dev/null || echo unknown)
date=$(git log -1 --format=%cd --date=format:%Y-%m-%d 2>/dev/null || echo "")
dirty=""
git diff --quiet HEAD -- 2>/dev/null || dirty="+dirty"
label="$sha$dirty $date"
text="#define KATAM_BUILD_LABEL \"$label\""
mkdir -p "$(dirname "$out")"
if [ ! -f "$out" ] || [ "$(cat "$out")" != "$text" ]; then
    printf '%s\n' "$text" > "$out"
fi
