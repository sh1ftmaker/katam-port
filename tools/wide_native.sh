#!/usr/bin/env bash
#
# wide_native.sh [JOBS] -- make sure build/w16n/katam, a native katam16 build
# (KATAM_MAX_KIRBYS=16, RelWithDebInfo; for tests/wide_*.sh), exists and is
# up to date.  Prints nothing when there is nothing to do.
set -euo pipefail
cd "$(dirname "$0")/.."
J=${1:-4}
if [ ! -f build/w16n/CMakeCache.txt ]; then
    cmake -S . -B build/w16n -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKATAM_ALLOW_LP64=ON \
          -DKATAM_MAX_KIRBYS=16 >/dev/null
fi
cmake --build build/w16n -j "$J" >/dev/null
