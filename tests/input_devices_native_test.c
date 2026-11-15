/* input_devices_native_test.c -- the native device core (platform/input_devices.c)
 * against a fake game, the same rules tools/input_devices_test.mjs checks for
 * web/input_devices.js: joins only during play, "what player 1 used in the
 * last 3 seconds is player 1's", a fifth pad waits, hold Select to leave,
 * player 1's pad claimed once a second person plays, unplug grace, moving a
 * player's only controller makes them leave.
 *
 *   make input-devices-test      (builds and runs this, then the web module's)
 *
 * No SDL, no game: the sink is a fake that applies a join/leave request at the
 * next frame, the way the latch does. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/input_devices.h"

/* --- the fake game ------------------------------------------------------------ */
static int gSlots = 1, gQueue[8][2], gNq, gRequests, gCanRequest = 1, gStory = 1;
static u16 gKeys[4];

static int FakeIsHuman(int s) { return s == 0 ? 1 : (gSlots >> s) & 1; }
static u32 FakeRequest(int s, int on)
{
    gQueue[gNq][0] = s;
    gQueue[gNq][1] = on;
    gNq++;
    gRequests++;
    return 1;
}
static int FakeCanRequest(void) { return gCanRequest; }
static void FakeHostKeys(int p, u16 m) { gKeys[p] = m; }
static int FakeJoinable(int s) { (void)s; return 1; }
static int FakeInStory(void) { return gStory; }
static const struct PortDevSink kFake = {
    FakeIsHuman, FakeRequest, FakeCanRequest, FakeHostKeys, FakeJoinable, FakeInStory
};

/* The core's other references (the real game sink), never called here. */
int PortLmpSlotIsHuman(int s) { (void)s; abort(); }
u32 PortLmpRequestHuman(int s, int on) { (void)s; (void)on; abort(); }
int PortLmpCanRequest(void) { abort(); }
void PortLmpSetHostKeys(int p, u16 m) { (void)p; (void)m; abort(); }
int PortLmpSlotJoinable(int s) { (void)s; abort(); }
int PortLmpInStory(void) { abort(); }
void PortLog(const char *fmt, ...)
{
    if (getenv("DEV_TEST_LOG")) {
        va_list ap;

        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}

static void Latch(void)
{
    int i;

    for (i = 0; i < gNq; i++)
        gSlots = gQueue[i][1] ? gSlots | (1 << gQueue[i][0]) : gSlots & ~(1 << gQueue[i][0]);
    gNq = 0;
}

/* --- harness ------------------------------------------------------------------ */
#define A_BTN  0x001
#define B_BTN  0x002
#define SELECT 0x004
#define START  0x008
#define RIGHT  0x010

static int sFail, sPass;
#define CHECK(c, ...) do { if (c) sPass++; else { sFail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Reset(void)
{
    PortDevReset();
    PortDevSetSink(&kFake);
    PortDevSetVerbose(getenv("DEV_TEST_LOG") != NULL);
    gSlots = 1; gNq = 0; gRequests = 0; gCanRequest = 1; gStory = 1;
    memset(gKeys, 0, sizeof gKeys);
}

/* One frame: the devices' keys, the core, the latch.  Returns player 1's. */
static u16 Step(void)
{
    u16 p1 = PortDevFrame();

    Latch();
    return p1;
}

static u16 Steps(int n)
{
    u16 p1 = 0;

    while (n-- > 0)
        p1 = Step();
    return p1;
}

static int PlayerOf(int id)
{
    struct PortDevInfo l[PORT_DEV_MAX];
    int i, n = PortDevList(l, PORT_DEV_MAX);

    for (i = 0; i < n; i++)
        if (l[i].id == id)
            return l[i].player;
    return -99;
}

/* Tap: down one frame, up one frame. */
static u16 Tap(int id, u16 m)
{
    u16 p1;

    PortDevSetKeys(id, m);
    p1 = Step();
    PortDevSetKeys(id, 0);
    Step();
    return p1;
}

#define TEST(name) printf("%s\n", name); Reset();

int main(void)
{
    int kbA, kbB, p1, p2, p3, p4, p5;
    u16 out;

    TEST("default: every unclaimed device drives player 1") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        p1 = PortDevAdd(PORT_DEV_PAD, "pad-1", "Pad 1");
        PortDevSetKeys(kbA, RIGHT);
        PortDevSetKeys(p1, A_BTN);
        out = Step();
        CHECK(out == (RIGHT | A_BTN), "player 1 got %03x", out);
        CHECK(gRequests == 0, "no join");
    }

    TEST("a lone pad's first Start is player 1's") {
        p1 = PortDevAdd(PORT_DEV_PAD, "pad-1", "Pad 1");
        out = Tap(p1, START);
        CHECK(out & START, "Start reached player 1");
        CHECK(gRequests == 0 && gSlots == 1, "nobody joined");
    }

    TEST("joins only during play: on the title a second pad's Start is player 1's") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        p2 = PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2");
        Tap(kbA, A_BTN);
        gStory = 0;
        out = Tap(p2, START);
        CHECK(out & START, "the title's Start");
        CHECK(gSlots == 1 && gRequests == 0, "no player 2 (slots %x)", gSlots);
        gStory = 1;
        Tap(kbA, A_BTN);            /* player 1 plays on the keyboard */
        Steps(PORT_DEV_JOIN_QUIET + 2);
        out = Tap(p2, START);
        CHECK(!(out & START), "in play, the pad's Start joins and is swallowed");
        CHECK(gSlots == 3 && PlayerOf(p2) == 1, "player 2 (slots %x, pad on %d)", gSlots, PlayerOf(p2));
    }

    TEST("what player 1 used in the last 3 seconds is player 1's") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        p1 = PortDevAdd(PORT_DEV_PAD, "pad-1", "Pad 1");
        Tap(kbA, A_BTN);
        Tap(p1, RIGHT);             /* player 1 picks the pad up */
        Tap(kbA, A_BTN);            /* ... and goes back to the keyboard */
        Steps(30);
        out = Tap(p1, START);
        CHECK(out & START, "Start on the pad they just used pauses");
        CHECK(gSlots == 1, "no join (slots %x)", gSlots);
        Tap(kbA, A_BTN);
        Steps(PORT_DEV_JOIN_QUIET + 2);
        out = Tap(p1, START);
        CHECK(!(out & START) && gSlots == 3, "put down three seconds: its Start joins (slots %x)", gSlots);
    }

    TEST("keyboard set B: player 1's until put down; then its Start joins") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        kbB = PortDevAdd(PORT_DEV_KEYS_B, "kb-b", "Keyboard (arrows)");
        Tap(kbB, RIGHT);
        out = Tap(kbB, START);
        CHECK(out & START && gSlots == 1, "set B in use by player 1: Start pauses");
        Tap(kbA, A_BTN);
        Steps(PORT_DEV_JOIN_QUIET + 2);
        out = Tap(kbB, START);
        CHECK(!(out & START) && gSlots == 3, "set B left alone: Start joins (slots %x)", gSlots);
    }

    TEST("four players; player 1's pad is theirs; a fifth pad waits") {
        p1 = PortDevAdd(PORT_DEV_PAD, "pad-1", "Pad 1");
        p2 = PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2");
        p3 = PortDevAdd(PORT_DEV_PAD, "pad-3", "Pad 3");
        p4 = PortDevAdd(PORT_DEV_PAD, "pad-4", "Pad 4");
        p5 = PortDevAdd(PORT_DEV_PAD, "pad-5", "Pad 5");
        Tap(p1, A_BTN);
        Tap(p2, START);
        CHECK(gSlots == 3 && PlayerOf(p2) == 1, "pad 2 joins as player 2");
        CHECK(PlayerOf(p1) == 0, "pad 1 is claimed by player 1 (%d)", PlayerOf(p1));
        Tap(p3, START);
        Tap(p4, START);
        CHECK(gSlots == 15, "four players (slots %x)", gSlots);
        PortDevSetKeys(p5, START);
        out = Step();
        CHECK(!(out & START), "a fifth pad's Start does not pause player 1");
        PortDevSetKeys(p5, RIGHT);
        out = Step();
        CHECK(!(out & RIGHT), "nor does its d-pad move player 1: it waits");
        PortDevSetKeys(p5, 0);
        Step();
        CHECK(PlayerOf(p5) == PORT_DEV_FOLLOW, "pad 5 still unclaimed");
        PortDevSetKeys(p2, RIGHT);
        Step();
        CHECK(gKeys[1] == RIGHT, "pad 2 drives player 2 (%03x)", gKeys[1]);
    }

    TEST("hold Select to leave; Start rejoins on the same slot") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        p2 = PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2");
        Tap(kbA, A_BTN);
        Tap(p2, START);
        CHECK(gSlots == 3, "player 2 joined");
        PortDevSetKeys(p2, SELECT);
        Steps(PORT_DEV_HOLD_LEAVE_FRAMES - 2);
        CHECK(gSlots == 3, "not yet");
        Steps(3);
        CHECK(gSlots == 1 && PlayerOf(p2) == PORT_DEV_PARKED, "left after %d frames (slots %x, %d)",
              PORT_DEV_HOLD_LEAVE_FRAMES, gSlots, PlayerOf(p2));
        PortDevSetKeys(p2, 0);
        Step();
        Tap(p2, START);
        CHECK(gSlots == 3 && PlayerOf(p2) == 1, "a parked pad's Start rejoins as player 2");
    }

    TEST("unplugged: back within the grace period carries on, later parked") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        p2 = PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2");
        Tap(kbA, A_BTN);
        Tap(p2, START);
        PortDevRemove(p2);
        Steps(PORT_DEV_GRACE_FRAMES - 10);
        CHECK(gSlots == 3, "still player 2 in the grace period");
        CHECK(PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2") == p2, "the same pad comes back");
        Steps(20);
        CHECK(gSlots == 3 && PlayerOf(p2) == 1, "carries on");
        PortDevRemove(p2);
        Steps(PORT_DEV_GRACE_FRAMES + 2);
        CHECK(gSlots == 1, "gone for good: the CPU (slots %x)", gSlots);
        PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2");
        CHECK(PlayerOf(p2) == PORT_DEV_PARKED, "parked when back");
        Tap(p2, START);
        CHECK(gSlots == 3, "Start rejoins");
    }

    TEST("moving a player's only controller to another player makes them leave") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        p2 = PortDevAdd(PORT_DEV_PAD, "pad-2", "Pad 2");
        p3 = PortDevAdd(PORT_DEV_PAD, "pad-3", "Pad 3");
        Tap(kbA, A_BTN);
        Tap(p2, START);
        Tap(p3, START);
        CHECK(gSlots == 7, "players 2 and 3");
        PortDevAssign(p2, 2);       /* pad 2 to player 3 */
        Steps(2);
        CHECK(PlayerOf(p2) == 2 && gSlots == 5, "player 2 left (slots %x)", gSlots);
    }

    TEST("the game's LOCAL start: a person with no controller gets a spare one") {
        kbA = PortDevAdd(PORT_DEV_KEYS_A, "kb-a", "Keyboard");
        kbB = PortDevAdd(PORT_DEV_KEYS_B, "kb-b", "Keyboard (arrows)");
        Tap(kbA, A_BTN);
        gSlots = 3;
        Step();
        CHECK(PlayerOf(kbB) == 1, "keyboard set B is player 2's (%d)", PlayerOf(kbB));
    }

    printf("%d passed, %d failed\n", sPass, sFail);
    return sFail ? 1 : 0;
}
