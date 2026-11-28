#!/usr/bin/env python3
"""bridge.py -- the CPU Kirby planner process (docs/design/CPU-HAIKU.md).

The native game starts this as KATAM_PLANNER (platform/native/planner_pipe.c):
one `decide` request per line on stdin (the observation; a picture's path),
one directive per line on stdout.  Requests are answered concurrently (a
model call takes a second or two, and each CPU Kirby asks on its own), in
whatever order they finish; the game matches answers by id and keeps playing
meanwhile.

    bridge.py --backend mock                       # deterministic, offline
    bridge.py --backend haiku [--effort low]       # Claude Haiku 5.5 (needs
                                                   #   ANTHROPIC_API_KEY)
    --log FILE    one JSON object per decision: observation text, directive,
                  latency, token usage, and (in the next decision for the same
                  Kirby) how it ended -- tools/haiku_ai/report.py reads it

stdout carries the protocol only; diagnostics go to stderr.
"""

import argparse
import collections
import json
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from planner_core import (PlannerError, describe_last, normalize_directive,  # noqa: E402
                          render_observation, reply_line)


def make_backend(args):
    if args.backend == "mock":
        from mock_planner import MockPlanner
        return MockPlanner()
    from haiku_planner import HaikuPlanner
    return HaikuPlanner(model=args.model, effort=args.effort, image_scale=args.image_scale,
                        send_images=not args.no_images)


class Bridge:
    def __init__(self, backend, out, log_path=None, workers=4):
        self.backend = backend
        self.out = out
        self.lock = threading.Lock()
        self.log = open(log_path, "a", buffering=1) if log_path else None
        self.pool = ThreadPoolExecutor(max_workers=workers) if workers > 1 else None
        self.history = collections.defaultdict(lambda: collections.deque(maxlen=4))
        self.team_fail = collections.deque(maxlen=4)     # (slot, what) of failed orders
        self.decisions = 0
        self.errors = 0

    def handle_line(self, line):
        line = line.strip()
        if not line:
            return
        try:
            req = json.loads(line)
        except ValueError:
            print("bridge: not JSON: %r" % line[:200], file=sys.stderr)
            return
        if req.get("type") != "decide":
            return
        slot = req.get("slot")
        hist = list(self.history[slot])
        notes = ["Kirby %d: %s" % (s + 1, w) for s, w in self.team_fail if s != slot]
        last = req.get("last")
        if last:
            self.history[slot].append(describe_last(last))
            if last.get("status") in ("failed", "timeout", "stale", "lost"):
                self.team_fail.append((slot, describe_last(last)))
        if self.pool is not None:
            self.pool.submit(self.decide, req, hist, notes)
        else:
            self.decide(req, hist, notes)

    def decide(self, req, hist, notes=()):
        t0 = time.monotonic()
        entry = {"ts": round(time.time(), 3), "backend": self.backend.name, "id": req.get("id"),
                 "slot": req.get("slot"), "frame": req.get("frame"), "reason": req.get("reason"),
                 "last": req.get("last"), "shot": req.get("shot")}
        try:
            entry["observation"] = render_observation(req, hist, notes)
            png = None
            if req.get("shot"):
                try:
                    with open(req["shot"], "rb") as f:
                        png = f.read()
                except OSError:
                    png = None
            d, meta = self.backend.decide(req, hist, png, notes)
            d = normalize_directive(d, req)
            entry.update(meta)
        except PlannerError as e:
            d = {"kind": "none", "why": "planner error: %s" % e}
            entry["error"] = str(e)
        except Exception as e:  # the network, the SDK: the game must not wait on it
            d = {"kind": "none", "why": "planner error"}
            entry["error"] = "%s: %s" % (type(e).__name__, e)
        entry["directive"] = d
        entry["ms"] = round((time.monotonic() - t0) * 1000, 1)
        out = reply_line(req, d)
        with self.lock:
            self.out.write(out + "\n")
            self.out.flush()
            self.decisions += 1
            if "error" in entry:
                self.errors += 1
            if self.log:
                self.log.write(json.dumps(entry, sort_keys=True) + "\n")
        print("bridge: #%s k%s f%s %s -> %s%s (%.0f ms)" % (
            req.get("id"), req.get("slot"), req.get("frame"), req.get("reason"), d["kind"],
            "" if d["kind"] == "none" else " " + json.dumps({k: d[k] for k in ("room", "group", "x", "y", "who", "secs")}),
            entry["ms"]), file=sys.stderr)

    def close(self):
        if self.pool is not None:
            self.pool.shutdown(wait=True)
        if self.log:
            self.log.close()
        print("bridge: %d decisions, %d errors" % (self.decisions, self.errors), file=sys.stderr)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=("mock", "haiku"), default="mock")
    ap.add_argument("--log")
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--model", default="claude-haiku-5-5")
    ap.add_argument("--effort", choices=("low", "medium", "high"), default="low")
    ap.add_argument("--image-scale", type=int, default=2)
    ap.add_argument("--no-images", action="store_true")
    args = ap.parse_args(argv)

    backend = make_backend(args)
    bridge = Bridge(backend, sys.stdout, args.log, args.workers)
    print("bridge: %s backend ready" % backend.name, file=sys.stderr)
    try:
        for line in sys.stdin:
            bridge.handle_line(line)
    except KeyboardInterrupt:
        pass
    bridge.close()


if __name__ == "__main__":
    main()
