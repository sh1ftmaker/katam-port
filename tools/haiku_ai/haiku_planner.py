"""haiku_planner.py -- the Claude Haiku 5.5 planner (docs/design/CPU-HAIKU.md).

One Messages API call per decision, through the official `anthropic` SDK:

  * the system prompt (vocabulary, game facts, strategy) is frozen and
    cached (cache_control on its block; it is over the 512-token minimum);
  * the user turn is the picture (base64 PNG, scaled 2x) when the Kirby is on
    screen, then the observation as text (planner_core.render_observation);
  * the reply is structured output (output_config.format, a JSON schema),
    with output_config.effort "low" -- Haiku 5.5 thinks adaptively by
    default and a planner tick wants latency more than depth;
  * stop_reason is checked before the content is read.

build_request() and parse_response() are pure, so they are unit-tested
without the network (test_planner.py).  The client reads ANTHROPIC_API_KEY
from the environment; nothing here stores or looks for keys.
"""

import base64
import json

from planner_core import (DIRECTIVE_SCHEMA, PlannerError, normalize_directive,
                          png_upscale, render_observation)

MODEL = "claude-haiku-5-5"

SYSTEM_PROMPT = """\
You are the strategic planner for one computer-controlled (CPU) Kirby in a \
port of "Kirby & The Amazing Mirror" (Game Boy Advance). A fast rule-based \
brain moves the Kirby frame by frame; you decide what it should do next. You \
are asked only when the Kirby has no order, when its last order ended (done, \
failed, timed out), or when an order ran far too long. Between your answers \
the Kirby follows its own mode, so a slow or empty answer is never fatal.

THE WORLD
- The game is a maze of numbered rooms joined by door tiles (and some by room \
edges, which the brain does not route over). Room 801 is the hub; its mirror \
doors lead to the areas and most stay closed until a big switch is pressed in \
an area. Area 0 (Rainbow Route, rooms 1xx) links to most areas.
- The goal: reach each area's boss room, beat the boss, collect its mirror \
shard (8 shards). The boss rooms are listed in the observation as pairs \
"area entrance -> boss room".
- A Kirby uses a door by walking into its tile. A door can be closed (not on \
the map now), or unreachable from where the Kirby stands.
- Door groups: in some rooms the doors fall into separate parts the Kirby \
cannot cross between. A door's group is the index of the first door of its \
group in that room. Entering a room by a door puts the Kirby in that door's \
group. Only "reachable" doors can be used now.
- Copy abilities decide which breakable blocks a Kirby passes: tier 0 none, \
1 soft blocks (most abilities), 2 also harder ones (burning, wheel, stone, \
missile), 3 also the hardest (hammer, master).
- Enemies and items only exist near a player's camera; a CPU Kirby far from \
the players meets empty rooms. CPU Kirbys revive after a defeat.

ORDERS -- reply with exactly one; fields an order does not use are 0 (group -1)
- goto_room(room, group): walk door by door to `room`; group >= 0 asks to \
arrive in that door group. Done when there. Fails at once if no door route \
is known from here.
- goto_door(x, y): walk to the door tile (x, y) of THIS room (from the doors \
list) and through it. Done when it arrives behind it.
- goto_xy(x, y): walk to tile (x, y) of this room. Done within one tile.
- follow(who): stay with Kirby slot `who` (slot 0 is player 1) for `secs`.
- explore: walk toward rooms nobody has entered; done on entering one.
- fight: attack the boss, else the enemies, in this room; done when none are \
left; fails if there are none.
- food: walk to food in this room; done when HP goes up; fails if none.
- wait: hold this spot for `secs`.
- auto: let the Kirby's own mode (CLASSIC, BODYGUARD, EXPLORER, ADVENTURER) \
run for `secs`.
- none: no order now (you will be asked again in about 10 seconds).
`secs` is the order's time limit, 1-255 (0 = a default: goto_room 90, \
goto_door 30, goto_xy 20, follow 30, explore 90, fight 30, food 20, wait 10, \
auto 30). When it runs out the order ends "timeout"; wait, auto and follow \
end "done" instead.

THE OBSERVATION
Text, sometimes with a picture of player 1's screen (only when this Kirby is \
in the room player 1 is in; the picture shows player 1's view, and the CPU \
Kirbys are the other Kirbys in it). Doors are listed as (x,y)->room \
[group; open/closed, reachable or not, visited or never entered]. "Nearest \
room worth exploring" and "nearest unbeaten boss room" are the brain's own \
door-group routes: they respect closed doors, door groups and the blocks \
this Kirby can break, so they are usually achievable. "Door moves given up" \
are room-to-room steps the brain walked at for 30 seconds without getting \
through: avoid them. Tiles are 16x16 pixels, x to the right, y down.

HOW TO CHOOSE
- Prefer orders the brain can finish: rooms from the route suggestions, \
doors marked open and reachable, tiles near listed objects.
- Never repeat an order that just failed or timed out with the same target; \
change target, or use explore or auto.
- HP at a third or less with food listed here: food.
- A live boss here: fight.
- Spread the CPU Kirbys out: if a teammate is already doing something \
(listed under Team), do something else unless helping matters.
- Keep "why" to one short sentence.
"""


class HaikuPlanner:
    name = "haiku"

    def __init__(self, client=None, model=MODEL, effort="low", max_tokens=2048, image_scale=2,
                 send_images=True):
        if client is None:
            import anthropic  # the official SDK; reads ANTHROPIC_API_KEY
            client = anthropic.Anthropic(max_retries=2, timeout=30.0)
        self.client = client
        self.model = model
        self.effort = effort
        self.max_tokens = max_tokens
        self.image_scale = image_scale
        self.send_images = send_images

    def build_request(self, req, history=(), png=None):
        """The keyword arguments of client.messages.create for one decision."""
        content = []
        if png is not None and self.send_images:
            data = png_upscale(png, self.image_scale) if self.image_scale > 1 else png
            content.append({"type": "image",
                            "source": {"type": "base64", "media_type": "image/png",
                                       "data": base64.standard_b64encode(data).decode("ascii")}})
        content.append({"type": "text", "text": render_observation(req, history)})
        return {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "system": [{"type": "text", "text": SYSTEM_PROMPT, "cache_control": {"type": "ephemeral"}}],
            "output_config": {"effort": self.effort,
                              "format": {"type": "json_schema", "schema": DIRECTIVE_SCHEMA}},
            "messages": [{"role": "user", "content": content}],
        }

    @staticmethod
    def parse_response(resp, req):
        """A Message (SDK object, or anything shaped like one) -> directive."""
        stop = getattr(resp, "stop_reason", None)
        if stop == "refusal":
            raise PlannerError("refused")
        if stop == "max_tokens":
            raise PlannerError("hit max_tokens before the directive was complete")
        if stop not in ("end_turn", "stop_sequence"):
            raise PlannerError("unexpected stop_reason %r" % (stop,))
        text = next((b.text for b in resp.content if getattr(b, "type", None) == "text"), None)
        if text is None:
            raise PlannerError("no text block in the reply")
        try:
            d = json.loads(text)
        except ValueError as e:
            raise PlannerError("reply is not JSON: %s" % e)
        return normalize_directive(d, req)

    @staticmethod
    def usage_of(resp):
        u = getattr(resp, "usage", None)
        if u is None:
            return {}
        out = {}
        for k in ("input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens"):
            v = getattr(u, k, None)
            if v is not None:
                out[k] = v
        return out

    def decide(self, req, history=(), png=None):
        """(directive, meta): meta has the token usage and the request id."""
        params = self.build_request(req, history, png)
        resp = self.client.messages.create(**params)
        meta = {"usage": self.usage_of(resp), "request_id": getattr(resp, "_request_id", None),
                "image": any(c["type"] == "image" for c in params["messages"][0]["content"])}
        return self.parse_response(resp, req), meta
