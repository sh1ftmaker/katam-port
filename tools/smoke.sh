#!/usr/bin/env bash
#
# smoke.sh -- the one-minute check: tools/full_check.sh --smoke.
#
#   tools/smoke.sh ROM [--out DIR] [--jobs N] [--only a,b] [--skip a,b] [--list]
#
# The highest-value paths, each cut short, all at once on already-built
# artefacts (build first: `tools/full_check.sh ROM --build --only lint` or
# the build lines in full_check.sh): the 4-Kirby recordings natively and in
# wasm (regress), a 16-device online session in node (sixteen_test, free
# roam, 900 frames), host-away grace (hostaway_test, 4 and 8 devices), star
# stone blocks moved by Kirbys past four (wide_blocks), lint, the wire and
# netplay unit tests, and one browser session of 10 tabs on the default
# module (online_browser_test --cases smoke: HOST GAME, JOIN GAME by code,
# 8 invite links, 8 s of play with every frame compared, a pause opened and
# closed by a player past the eighth).  About 60 s wall on this machine.
# tools/full_check.sh is the deep check (43 suites, ~30 min).
exec "$(cd "$(dirname "$0")" && pwd)/full_check.sh" --smoke "$@"
