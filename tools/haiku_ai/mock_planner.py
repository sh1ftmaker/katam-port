"""mock_planner.py -- a deterministic rule-of-thumb planner behind the same
interface as the Haiku one (docs/design/CPU-HAIKU.md).

Same request in, same directive out: no clock, no randomness, no network.
It exists so the whole loop -- observation, directive, latch, record, replay
-- runs and is tested offline, and as the baseline a model has to beat.

Per slot a persona (slot % 3): 1 guard (stays with player 1), 2 scout
(rooms nobody has entered), 0 adventurer (boss routes, fights).  Common
rules first: eat when at a third of HP or below and food is here; never
repeat an order that just failed or timed out with the same target.
"""

from planner_core import DEFAULT_SECS


def _failed_same(last, kind, a=None, b=None, c=None):
    if not last or last.get("status") not in ("failed", "timeout", "stale", "lost"):
        return False
    if last.get("kind") != kind:
        return False
    return (a is None or last.get("a") == a) and (b is None or last.get("b") == b) \
        and (c is None or last.get("c") == c)


def _d(kind, why, room=0, group=-1, x=0, y=0, who=0, secs=0):
    return {"kind": kind, "room": room, "group": group, "x": x, "y": y, "who": who,
            "secs": secs or DEFAULT_SECS[kind], "why": why}


class MockPlanner:
    name = "mock"

    def decide(self, req, history=(), png=None, team_notes=()):
        """(directive, meta) -- the same interface as HaikuPlanner.decide."""
        return self._decide(req), {}

    def _decide(self, req):
        me = req.get("me", {})
        objs = req.get("objects", {})
        last = req.get("last")
        slot = req["slot"]
        persona = slot % 3
        people = [t for t in req.get("team", []) if t.get("person")]
        player = people[0] if people else None

        if me.get("max_hp", 0) > 0 and me.get("hp", 0) * 3 <= me["max_hp"] \
                and objs.get("food", [0])[0] and not _failed_same(last, "food"):
            return _d("food", "low HP and food here")
        if objs.get("boss", [0])[0] and not _failed_same(last, "fight"):
            return _d("fight", "a boss is here", secs=60)

        if persona == 1 and player is not None:
            if player["room"] == me.get("room") and not _failed_same(last, "follow"):
                return _d("follow", "guard player 1", who=player["slot"], secs=30)
            if not _failed_same(last, "goto_room", a=player["room"]):
                return _d("goto_room", "rejoin player 1", room=player["room"], secs=90)
            return _d("auto", "cannot reach player 1; own mode for a while", secs=30)

        if persona == 0:
            br = req.get("boss_route")
            if br and not _failed_same(last, "goto_room", a=br["room"]):
                return _d("goto_room", "head for the nearest unbeaten boss", room=br["room"], secs=120)
            if me.get("with_player") and objs.get("enemy", [0])[0] and not _failed_same(last, "fight"):
                return _d("fight", "clear the enemies near the player", secs=20)

        ex = req.get("explore")
        if ex and not _failed_same(last, "goto_room", a=ex["room"]):
            return _d("goto_room", "nearest room worth exploring", room=ex["room"], secs=90)
        # a door here that leads somewhere never entered
        for x, y, to, grp, opn, reach, ent in req.get("doors", []):
            if opn and reach and not ent and not _failed_same(last, "goto_door", b=x, c=y):
                return _d("goto_door", "an open door to a room never entered", x=x, y=y, secs=40)
        if not _failed_same(last, "explore"):
            return _d("explore", "look for new rooms", secs=60)
        return _d("auto", "nothing better: own mode", secs=30)
