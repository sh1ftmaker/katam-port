"""planner_core.py -- what every planner backend shares (docs/design/CPU-HAIKU.md).

The game (platform/cpu_planner.c) sends one `decide` request per line: an
observation of one CPU Kirby (JSON) and, when that Kirby is in the room on
screen, the path of a PNG of the frame.  A backend answers with one
directive; the bridge turns it into the reply line the game latches as a
recorded `cpugoal` input.  This module holds the directive vocabulary and its
validation, the observation's text rendering for a language model, and a
small PNG reader/writer (stdlib only) to scale the 240x160 frame up.

Nothing here talks to the network.
"""

import json
import struct
import zlib

# The directive kinds the executor (game/src/cpu_ai_modes.c) knows; "none"
# means "no order now" (the Kirby's mode runs, the game asks again later).
KINDS = ["goto_room", "goto_door", "goto_xy", "follow", "explore", "fight", "food", "wait", "auto"]
DEFAULT_SECS = {"goto_room": 90, "goto_door": 30, "goto_xy": 20, "follow": 30, "explore": 90,
                "fight": 30, "food": 20, "wait": 10, "auto": 30}
MAX_ROOM = 983          # vanilla rooms (RES_FIRST_NEW_ROOM)
SLOTS = 16

# The structured-output schema (output_config.format) the model must fill.
# Every field is required (unused ones are 0, group -1) so the reply is one
# flat, fully specified object.
DIRECTIVE_SCHEMA = {
    "type": "object",
    "properties": {
        "kind": {"type": "string", "enum": KINDS + ["none"]},
        "room": {"type": "integer"},
        "group": {"type": "integer"},
        "x": {"type": "integer"},
        "y": {"type": "integer"},
        "who": {"type": "integer"},
        "secs": {"type": "integer"},
        "why": {"type": "string"},
    },
    "required": ["kind", "room", "group", "x", "y", "who", "secs", "why"],
    "additionalProperties": False,
}


class PlannerError(Exception):
    """A backend could not produce a directive (the game's mode keeps running)."""


def normalize_directive(d, req):
    """Check a backend's directive against the vocabulary and the request.

    Returns the cleaned directive (dict with kind, room, group, x, y, who,
    secs, why) or raises PlannerError.  Lenient where the game itself will
    judge (a door that is not there fails in the game, and the planner hears
    about it); strict where a value could not be encoded at all."""
    if not isinstance(d, dict):
        raise PlannerError("directive is not an object")
    kind = d.get("kind")
    if kind == "none":
        return {"kind": "none", "why": str(d.get("why", ""))[:200]}
    if kind not in KINDS:
        raise PlannerError("unknown kind %r" % (kind,))

    def num(key, default=0):
        v = d.get(key, default)
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            raise PlannerError("%s is not a number" % key)
        return int(v)

    out = {"kind": kind, "room": 0, "group": -1, "x": 0, "y": 0, "who": 0,
           "secs": num("secs"), "why": str(d.get("why", ""))[:200]}
    if out["secs"] <= 0:
        out["secs"] = DEFAULT_SECS[kind]
    out["secs"] = min(out["secs"], 255)
    if kind == "goto_room":
        out["room"] = num("room")
        out["group"] = num("group", -1)
        if not 0 <= out["room"] < MAX_ROOM:
            raise PlannerError("room %d out of range" % out["room"])
        if out["group"] < -1 or out["group"] > 253:
            out["group"] = -1
    elif kind in ("goto_door", "goto_xy"):
        out["x"], out["y"] = num("x"), num("y")
        if not (0 <= out["x"] < 256 and 0 <= out["y"] < 256):
            raise PlannerError("tile (%d,%d) out of range" % (out["x"], out["y"]))
    elif kind == "follow":
        out["who"] = num("who")
        if not 0 <= out["who"] < SLOTS or out["who"] == req.get("slot"):
            raise PlannerError("cannot follow slot %d" % out["who"])
    return out


def reply_line(req, d):
    """The line the game reads back (platform/cpu_planner.c Answer)."""
    r = {"id": req["id"], "slot": req["slot"], "kind": d["kind"]}
    if d["kind"] != "none":
        r.update({"room": d["room"], "group": d["group"], "x": d["x"], "y": d["y"],
                  "who": d["who"], "secs": d["secs"]})
    return json.dumps(r, separators=(",", ":"), sort_keys=True)


def describe_directive(d):
    k = d.get("kind", "?")
    if k == "goto_room":
        return "goto_room %d%s" % (d.get("room", d.get("a", 0)),
                                   " group %d" % d["group"] if d.get("group", -1) >= 0 else "")
    if k in ("goto_door", "goto_xy"):
        return "%s (%d,%d)" % (k, d.get("x", d.get("b", 0)), d.get("y", d.get("c", 0)))
    if k == "follow":
        return "follow %d" % d.get("who", d.get("a", 0))
    return k


def describe_last(last):
    """The game's `last` record (cpu_planner.c Outcome) as a short phrase."""
    if not last:
        return "none"
    k = last.get("kind", "?")
    a, b, c = last.get("a", 0), last.get("b", 0), last.get("c", 0)
    if k == "goto_room":
        what = "goto_room %d%s" % (a, " group %d" % (b - 1) if b else "")
    elif k in ("goto_door", "goto_xy"):
        what = "%s (%d,%d)" % (k, b, c)
    elif k == "follow":
        what = "follow %d" % a
    else:
        what = k
    return "%s -> %s (%s) after %ss" % (what, last.get("status"), last.get("why"), last.get("took_s"))


def describe_doing(kind, args):
    """A teammate's running directive (the game's team entry: kind, [A, B, C])."""
    a, b, c = (list(args) + [0, 0, 0])[:3]
    return describe_last({"kind": kind, "a": a, "b": b, "c": c}).split(" -> ")[0]


def render_observation(req, history=(), team_notes=()):
    """The observation as compact text for a language model (the image, if
    any, goes alongside).  Deterministic: the same request renders the same.
    `history`: this Kirby's earlier orders and how they ended; `team_notes`:
    the other Kirbys' recent failures (the bridge keeps both)."""
    me = req.get("me", {})
    slot = req.get("slot")
    lines = []
    lines.append("Kirby %d (slot %d), game frame %d (%.0f s). Asked because: %s." % (
        slot + 1, slot, req.get("frame", 0), req.get("frame", 0) / 60.0, req.get("reason")))
    lines.append("Last order: %s." % describe_last(req.get("last")))
    if history:
        lines.append("Earlier orders (oldest first): %s." % "; ".join(history))
    lines.append(
        "You: room %d (area %d%s), tile (%d,%d), HP %d/%d, ability %s (breaks blocks up to tier %d), "
        "own mode %s, came from room %s. Room entered before: %s. Room has its own CPU script (not just roaming): %s. "
        "Player 1 is in this room: %s%s." % (
            me.get("room", -1), me.get("area", 0),
            ", boss room" if me.get("room_flags", 0) & 1 else
            ", area entrance" if me.get("room_flags", 0) & 8 else "",
            me.get("x", 0), me.get("y", 0), me.get("hp", 0), me.get("max_hp", 0),
            me.get("ability", "?"), me.get("tier", 0), me.get("mode", "?"),
            me.get("came_from") if me.get("came_from", -1) >= 0 else "?",
            "yes" if me.get("room_entered_before") else "no",
            "yes" if me.get("room_script") else "no",
            "yes" if me.get("with_player") else "no",
            " (picture attached)" if req.get("shot") else ""))
    doors = req.get("doors", [])
    if doors:
        parts = []
        for x, y, to, grp, opn, reach, ent in doors:
            st = "open" if opn else "closed"
            st += ", reachable" if reach else ", not reachable from here"
            st += ", visited" if ent else ", never entered"
            parts.append("(%d,%d)->room %d [group %d; %s]" % (x, y, to, grp, st))
        lines.append("Doors here: " + "; ".join(parts) +
                     (" (+%d more)" % req["more_doors"] if req.get("more_doors") else "") + ".")
    else:
        lines.append("Doors here: none known (rooms can also join by their edges).")
    ex, bo = req.get("explore"), req.get("boss_route")
    lines.append("Nearest room worth exploring by door route: %s." % (
        "%d, first door (%d,%d)" % (ex["room"], ex["door"][0], ex["door"][1]) if ex else "none"))
    lines.append("Nearest unbeaten boss room by door route: %s." % (
        "%d, first door (%d,%d)" % (bo["room"], bo["door"][0], bo["door"][1]) if bo else "none known"))
    objs = []
    for name, v in sorted(req.get("objects", {}).items()):
        if v and v[0]:
            objs.append("%s x%d (nearest at %d,%d)" % (name, v[0], v[1], v[2]))
    lines.append("Objects here: %s." % ("; ".join(objs) if objs else "none seen"))
    if req.get("blocked"):
        lines.append("Door moves given up recently (room->room): %s." % ", ".join(
            "%d->%d" % (a, b) for a, b in req["blocked"]))
    team = []
    for t in req.get("team", []):
        if t["slot"] == slot:
            continue
        who = "player %d" % (t["slot"] + 1) if t.get("person") else "CPU Kirby %d" % (t["slot"] + 1)
        extra = (", doing %s" % describe_doing(t["doing"], t.get("args", ()))) if t.get("doing") else ""
        team.append("slot %d %s: room %d (%d,%d) HP %d %s%s" % (
            t["slot"], who, t["room"], t["x"], t["y"], t["hp"], t.get("ability", ""), extra))
    lines.append("Team: %s." % ("; ".join(team) if team else "nobody else"))
    if team_notes:
        lines.append("Teammates' recent failures: %s." % "; ".join(team_notes))
    pr = req.get("progress", {})
    bosses = pr.get("bosses", [])
    beaten = sum(1 for b in bosses if b[2])
    lines.append("Progress: shards %d/8, rooms entered %d, bosses beaten %d/8; boss rooms (entrance->boss): %s." % (
        pr.get("shards", 0), pr.get("rooms_entered", 0), beaten,
        ", ".join("%d->%d%s" % (b[0], b[1], " beaten" if b[2] else "") for b in bosses)))
    lines.append("Give one order for Kirby %d." % (slot + 1))
    return "\n".join(lines)


# --- PNG (stdlib only): the game writes 8-bit RGB, filter 0, stored deflate --

def _chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    while pos + 8 <= len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        yield typ, data[pos + 8:pos + 8 + n]
        pos += 12 + n


def png_read_rgb(data):
    """(width, height, rows) of an 8-bit RGB PNG with filter 0 rows."""
    ihdr, idat = None, b""
    for typ, body in _chunks(data):
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
    if ihdr is None:
        raise ValueError("no IHDR")
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype != 2 or interlace:
        raise ValueError("not 8-bit RGB")
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    rows = []
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        if row[0] != 0:
            raise ValueError("filtered rows not supported")
        rows.append(row[1:])
    return w, h, rows


def png_write_rgb(w, h, rows):
    def chunk(typ, body):
        return struct.pack(">I", len(body)) + typ + body + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def png_upscale(data, factor):
    """Nearest-neighbour upscale and recompress (the game's PNGs are stored,
    not compressed).  factor 1 just recompresses."""
    w, h, rows = png_read_rgb(data)
    out = []
    for r in rows:
        if factor > 1:
            px = [r[i:i + 3] for i in range(0, len(r), 3)]
            r = b"".join(p * factor for p in px)
        out.extend([r] * factor)
    return png_write_rgb(w * factor, h * factor, out)


def image_tokens(w, h):
    """The documented estimate for an image's input tokens."""
    return (w * h + 749) // 750
