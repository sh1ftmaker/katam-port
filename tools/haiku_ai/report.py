#!/usr/bin/env python3
"""report.py -- read a planner decision log (bridge.py --log) and print the
chain observation -> directive -> outcome per decision, and the totals.

    tools/haiku_ai/report.py build/haiku/RUN/decisions.jsonl [--obs N]

A directive's outcome is the `last` record of the next question the same
Kirby asked (the game reports how the previous order ended).  Cost uses
Claude Haiku 5.5's list prices: $0.10 / $0.50 per million input / output
tokens, cache reads at 0.1x and 5-minute cache writes at 1.25x the input
price.
"""

import argparse
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from planner_core import describe_directive, describe_last  # noqa: E402

PRICE_IN, PRICE_OUT = 0.10e-6, 0.50e-6


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--obs", type=int, default=0, help="print the full observation of the first N decisions")
    args = ap.parse_args(argv)
    rows = load(args.log)
    rows.sort(key=lambda e: (e.get("frame") or 0, e.get("id") or 0))
    by_slot = collections.defaultdict(list)
    for e in rows:
        by_slot[e["slot"]].append(e)
    outcome = {}
    for slot, es in by_slot.items():
        for a, b in zip(es, es[1:]):
            outcome[a["id"]] = describe_last(b.get("last")) if b.get("last") else "?"
    statuses = collections.Counter()
    kinds = collections.Counter()
    tok = collections.Counter()
    ms = []
    shown = 0
    print("%5s %4s %7s %-8s  %-28s  %s" % ("id", "kirby", "frame", "asked", "directive", "outcome"))
    for e in rows:
        d = e.get("directive", {})
        kinds[d.get("kind")] += 1
        o = outcome.get(e["id"], "(still running at the end)")
        for s in ("done", "failed", "timeout", "stale", "lost"):
            if "-> %s " % s in o:
                statuses[s] += 1
        for k, v in (e.get("usage") or {}).items():
            tok[k] += v
        ms.append(e.get("ms", 0))
        err = "  [error: %s]" % e["error"] if e.get("error") else ""
        print("%5s %4d %7s %-8s  %-28s  %s%s" % (e["id"], e["slot"] + 1, e.get("frame"), e.get("reason"),
                                                  describe_directive(d), o, err))
        if shown < args.obs:
            shown += 1
            print("      observation:\n        " + e.get("observation", "").replace("\n", "\n        "))
            if e.get("shot"):
                print("      picture: %s" % e["shot"])
            print("      why: %s" % d.get("why", ""))
    n = len(rows)
    print()
    print("%d decisions; directives %s" % (n, dict(kinds)))
    print("outcomes: %s" % dict(statuses))
    if ms:
        ms.sort()
        print("latency ms: median %.0f, max %.0f" % (ms[len(ms) // 2], ms[-1]))
    if tok:
        cost = (tok["input_tokens"] * PRICE_IN + tok["cache_read_input_tokens"] * PRICE_IN * 0.1
                + tok["cache_creation_input_tokens"] * PRICE_IN * 1.25 + tok["output_tokens"] * PRICE_OUT)
        print("tokens: %s; cost $%.4f ($%.5f a decision)" % (dict(tok), cost, cost / max(n, 1)))


if __name__ == "__main__":
    main()
