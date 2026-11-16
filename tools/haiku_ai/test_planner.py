#!/usr/bin/env python3
"""test_planner.py -- the planner's Python side, offline (docs/design/CPU-HAIKU.md).

    python3 tools/haiku_ai/test_planner.py              # stdlib only
    tools/haiku_ai/.venv/bin/python tools/haiku_ai/test_planner.py
                                                        # + the real SDK, on a
                                                        #   mock HTTP transport

No network, no API key: request building and reply parsing are pure
functions, and the SDK round trip goes through httpx2.MockTransport, which
answers in-process (the request never leaves the machine).  The sample
observation is synthetic (room numbers only, no game data).
"""

import base64
import io
import json
import os
import struct
import sys
import types
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import planner_core as pc  # noqa: E402
from bridge import Bridge  # noqa: E402
from haiku_planner import MODEL, SYSTEM_PROMPT, HaikuPlanner  # noqa: E402
from mock_planner import MockPlanner  # noqa: E402


def sample_request(**over):
    req = {
        "type": "decide", "id": 7, "slot": 2, "frame": 2400, "reason": "ended", "shot": None,
        "last": {"kind": "goto_room", "a": 101, "b": 0, "c": 0, "secs": 90, "status": "done",
                 "why": "ok", "took_s": 6},
        "me": {"room": 101, "area": 0, "room_flags": 0, "x": 2, "y": 6, "hp": 6, "max_hp": 6,
               "ability": "normal", "tier": 1, "mode": "EXPLORER", "came_from": 801,
               "room_entered_before": 1, "room_script": 0, "with_player": 0},
        "doors": [[83, 8, 106, 0, 1, 1, 0]], "more_doors": 0,
        "explore": {"room": 106, "door": [83, 8]},
        "boss_route": {"room": 516, "door": [83, 8]},
        "objects": {"food": [0], "shard": [0], "boss": [0], "chest": [0], "enemy": [2, 20, 8]},
        "blocked": [[108, 140]],
        "team": [{"slot": 0, "person": 1, "room": 801, "x": 23, "y": 12, "hp": 6, "ability": "normal"},
                 {"slot": 1, "person": 0, "room": 801, "x": 17, "y": 10, "hp": 5, "ability": "normal",
                  "mode": "BODYGUARD", "doing": "follow"},
                 {"slot": 2, "person": 0, "room": 101, "x": 2, "y": 6, "hp": 6, "ability": "normal",
                  "mode": "EXPLORER"}],
        "progress": {"shards": 0, "rooms_entered": 3,
                     "bosses": [[300, 324, 0], [500, 516, 0], [114, 129, 1]]},
    }
    req.update(over)
    return req


def game_png(w=240, h=160):
    """A PNG as platform/native/png.c writes it: 8-bit RGB, filter 0, stored deflate."""
    rows = b"".join(b"\x00" + bytes(v for x in range(w) for v in ((x * 7 + y) & 0xFF, x & 0xFF, y & 0xFF))
                    for y in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 0)) + chunk(b"IEND", b""))


def fake_message(text, stop="end_turn", thinking=True):
    blocks = []
    if thinking:
        blocks.append(types.SimpleNamespace(type="thinking", thinking="", signature="sig"))
    if text is not None:
        blocks.append(types.SimpleNamespace(type="text", text=text))
    usage = types.SimpleNamespace(input_tokens=900, output_tokens=120, cache_creation_input_tokens=0,
                                  cache_read_input_tokens=1100)
    return types.SimpleNamespace(stop_reason=stop, content=blocks, usage=usage)


GOOD = {"kind": "goto_room", "room": 106, "group": -1, "x": 0, "y": 0, "who": 0, "secs": 60,
        "why": "explore onward"}


class Vocabulary(unittest.TestCase):
    def test_normalize(self):
        req = sample_request()
        d = pc.normalize_directive(dict(GOOD), req)
        self.assertEqual((d["kind"], d["room"], d["group"], d["secs"]), ("goto_room", 106, -1, 60))
        d = pc.normalize_directive(dict(GOOD, secs=0), req)
        self.assertEqual(d["secs"], pc.DEFAULT_SECS["goto_room"])
        d = pc.normalize_directive(dict(GOOD, secs=999), req)
        self.assertEqual(d["secs"], 255)
        self.assertEqual(pc.normalize_directive({"kind": "none", "why": "x"}, req)["kind"], "none")
        for bad in (dict(GOOD, kind="teleport"), dict(GOOD, room=5000), dict(GOOD, room="101"),
                    {"kind": "follow", "who": 2, "secs": 10},          # itself
                    {"kind": "goto_xy", "x": 300, "y": 2, "secs": 10}, "goto_room"):
            with self.assertRaises(pc.PlannerError):
                pc.normalize_directive(bad, req)

    def test_reply_line(self):
        req = sample_request()
        line = pc.reply_line(req, pc.normalize_directive(dict(GOOD), req))
        r = json.loads(line)
        self.assertEqual((r["id"], r["slot"], r["kind"], r["room"], r["secs"]), (7, 2, "goto_room", 106, 60))
        self.assertNotIn("\n", line)
        self.assertEqual(json.loads(pc.reply_line(req, {"kind": "none"})), {"id": 7, "slot": 2, "kind": "none"})

    def test_schema_matches_vocabulary(self):
        s = pc.DIRECTIVE_SCHEMA
        self.assertFalse(s["additionalProperties"])
        self.assertEqual(sorted(s["required"]), sorted(s["properties"]))
        self.assertEqual(s["properties"]["kind"]["enum"], pc.KINDS + ["none"])
        # the C side's names (platform/cpu_ai_host.c kGoalNames) are the same words
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, "..", "..", "platform", "cpu_ai_host.c")) as f:
            src = f.read()
        for k in pc.KINDS:
            self.assertIn('"%s"' % k, src)

    def test_render_is_deterministic_and_complete(self):
        req = sample_request()
        a = pc.render_observation(req, ["wait -> done (ok) after 10s"])
        self.assertEqual(a, pc.render_observation(req, ["wait -> done (ok) after 10s"]))
        for want in ("Kirby 3", "goto_room 101 -> done (ok)", "(83,8)->room 106", "group 0",
                     "never entered", "worth exploring by door route: 106", "516", "enemy x2",
                     "108->140", "player 1", "doing follow", "Earlier orders"):
            self.assertIn(want, a)


class Png(unittest.TestCase):
    def test_upscale(self):
        src = game_png()
        out = pc.png_upscale(src, 2)
        w, h, rows = pc.png_read_rgb(out)
        self.assertEqual((w, h), (480, 320))
        _, _, src_rows = pc.png_read_rgb(src)
        self.assertEqual(rows[2][6:9], src_rows[1][3:6])     # pixel (3,1) -> (6,2)
        self.assertLess(len(out), len(src))                  # recompressed
        self.assertEqual(pc.image_tokens(480, 320), 205)


class Haiku(unittest.TestCase):
    def planner(self, client=None):
        return HaikuPlanner(client=client or object())

    def test_request_shape(self):
        p = self.planner()
        req = sample_request(shot="/x.png")
        kw = p.build_request(req, [], game_png())
        self.assertEqual(kw["model"], MODEL)
        self.assertEqual(MODEL, "claude-haiku-5-5")
        self.assertEqual(kw["system"], [{"type": "text", "text": SYSTEM_PROMPT,
                                         "cache_control": {"type": "ephemeral"}}])
        self.assertEqual(kw["output_config"]["effort"], "low")
        self.assertEqual(kw["output_config"]["format"], {"type": "json_schema", "schema": pc.DIRECTIVE_SCHEMA})
        self.assertNotIn("thinking", kw)                       # adaptive by default on Haiku 5.5
        self.assertNotIn("tool_choice", kw)
        content = kw["messages"][0]["content"]
        self.assertEqual([c["type"] for c in content], ["image", "text"])
        img = base64.standard_b64decode(content[0]["source"]["data"])
        self.assertEqual(content[0]["source"]["media_type"], "image/png")
        self.assertEqual(pc.png_read_rgb(img)[:2], (480, 320))
        self.assertEqual(content[1]["text"], pc.render_observation(req, []))
        # no picture: text only
        kw2 = p.build_request(sample_request(), [], None)
        self.assertEqual([c["type"] for c in kw2["messages"][0]["content"]], ["text"])
        # the cached prefix is byte-identical between decisions
        self.assertEqual(json.dumps(kw["system"]), json.dumps(kw2["system"]))

    def test_system_prompt_is_cacheable(self):
        # Haiku 5.5 caches prefixes of 512 tokens and more; ~4 chars a token
        self.assertGreater(len(SYSTEM_PROMPT) / 4, 700)
        for k in pc.KINDS:
            self.assertIn(k, SYSTEM_PROMPT)

    def test_parse(self):
        req = sample_request()
        d = HaikuPlanner.parse_response(fake_message(json.dumps(GOOD)), req)
        self.assertEqual((d["kind"], d["room"]), ("goto_room", 106))
        d = HaikuPlanner.parse_response(fake_message(json.dumps(GOOD), thinking=False), req)
        self.assertEqual(d["room"], 106)
        for msg in (fake_message(json.dumps(GOOD), stop="max_tokens"),
                    fake_message(None, stop="refusal"),
                    fake_message("not json"),
                    fake_message(None),
                    fake_message(json.dumps(dict(GOOD, kind="fly"))),
                    fake_message(json.dumps(GOOD), stop="tool_use")):
            with self.assertRaises(pc.PlannerError):
                HaikuPlanner.parse_response(msg, req)

    def test_decide_with_fake_client(self):
        calls = []

        class Messages:
            def create(self, **kw):
                calls.append(kw)
                return fake_message(json.dumps(GOOD))
        client = types.SimpleNamespace(messages=Messages())
        d, meta = HaikuPlanner(client=client).decide(sample_request(), [], None)
        self.assertEqual(d["room"], 106)
        self.assertEqual(meta["usage"]["cache_read_input_tokens"], 1100)
        self.assertEqual(len(calls), 1)


class Sdk(unittest.TestCase):
    """The real anthropic SDK, answered in-process by a mock transport."""

    def setUp(self):
        try:
            import anthropic
            import httpx2
        except ImportError:
            self.skipTest("anthropic SDK not installed (use tools/haiku_ai/.venv)")
        self.anthropic, self.httpx2 = anthropic, httpx2

    def client(self, handler):
        return self.anthropic.Anthropic(
            api_key="test-key-not-real", max_retries=0,
            http_client=self.anthropic.DefaultHttpxClient(transport=self.httpx2.MockTransport(handler)))

    def test_round_trip(self):
        seen = {}

        def handler(request):
            seen["url"] = str(request.url)
            seen["key"] = request.headers.get("x-api-key") or request.headers.get("authorization", "")
            seen["body"] = json.loads(request.content)
            body = {"id": "msg_test", "type": "message", "role": "assistant", "model": MODEL,
                    "content": [{"type": "thinking", "thinking": "", "signature": "sig"},
                                {"type": "text", "text": json.dumps(GOOD)}],
                    "stop_reason": "end_turn", "stop_sequence": None,
                    "usage": {"input_tokens": 912, "output_tokens": 131,
                              "cache_creation_input_tokens": 0, "cache_read_input_tokens": 1180}}
            return self.httpx2.Response(200, json=body, headers={"request-id": "req_test"})

        p = HaikuPlanner(client=self.client(handler))
        d, meta = p.decide(sample_request(shot="/x.png"), ["wait -> done (ok) after 10s"], game_png())
        self.assertEqual((d["kind"], d["room"], d["secs"]), ("goto_room", 106, 60))
        self.assertTrue(seen["url"].endswith("/v1/messages"))
        self.assertIn("test-key-not-real", seen["key"])         # the client's key, sent as given
        b = seen["body"]
        self.assertEqual(b["model"], "claude-haiku-5-5")
        self.assertEqual(b["output_config"]["effort"], "low")
        self.assertEqual(b["output_config"]["format"]["type"], "json_schema")
        self.assertEqual(b["system"][0]["cache_control"], {"type": "ephemeral"})
        self.assertEqual(b["messages"][0]["content"][0]["type"], "image")
        self.assertEqual(meta["usage"]["cache_read_input_tokens"], 1180)
        self.assertEqual(meta["request_id"], "req_test")
        self.assertTrue(meta["image"])

    def test_api_error_is_reported_not_raised_by_bridge(self):
        def handler(request):
            return self.httpx2.Response(529, json={"type": "error", "error": {"type": "overloaded_error",
                                                                               "message": "busy"}})
        b, out = self.bridge(HaikuPlanner(client=self.client(handler)))
        b.handle_line(json.dumps(sample_request()))
        b.close()
        self.assertEqual(json.loads(out.getvalue()), {"id": 7, "slot": 2, "kind": "none"})

    def bridge(self, backend):
        out = io.StringIO()
        return Bridge(backend, out, None, workers=1), out


class MockAndBridge(unittest.TestCase):
    def test_mock_is_deterministic_and_sensible(self):
        m = MockPlanner()
        req = sample_request()
        a, _ = m.decide(req)
        b, _ = m.decide(json.loads(json.dumps(req)))
        self.assertEqual(a, b)
        self.assertEqual((a["kind"], a["room"]), ("goto_room", 106))       # scout: explore route
        # it does not repeat what just failed
        failed = sample_request(last={"kind": "goto_room", "a": 106, "b": 0, "c": 0, "secs": 90,
                                      "status": "failed", "why": "no_route", "took_s": 0})
        c, _ = m.decide(failed)
        self.assertNotEqual((c["kind"], c.get("room")), ("goto_room", 106))
        # low HP and food: eat
        hurt = sample_request(me=dict(sample_request()["me"], hp=2), objects=dict(
            sample_request()["objects"], food=[1, 5, 6]))
        self.assertEqual(m.decide(hurt)[0]["kind"], "food")
        # the guard persona (slot 1) stays with player 1
        g, _ = m.decide(sample_request(slot=1, me=dict(sample_request()["me"], room=801)))
        self.assertEqual((g["kind"], g["who"]), ("follow", 0))
        for d in (a, c, g):
            pc.normalize_directive(d, sample_request(slot=1 if d is g else 2))

    def test_bridge_round_trip_and_log(self):
        out = io.StringIO()
        log = os.path.join(os.environ.get("TMPDIR", "/tmp"), "haiku_test_%d.jsonl" % os.getpid())
        try:
            b = Bridge(MockPlanner(), out, log, workers=1)
            b.handle_line(json.dumps(sample_request()))
            b.handle_line("not json")
            b.handle_line(json.dumps({"type": "other"}))
            b.close()
            r = json.loads(out.getvalue())
            self.assertEqual((r["id"], r["kind"], r["room"]), (7, "goto_room", 106))
            with open(log) as f:
                e = json.loads(f.read().splitlines()[0])
            for k in ("observation", "directive", "ms", "last", "frame", "slot"):
                self.assertIn(k, e)
        finally:
            if os.path.exists(log):
                os.unlink(log)


if __name__ == "__main__":
    unittest.main(verbosity=2)
