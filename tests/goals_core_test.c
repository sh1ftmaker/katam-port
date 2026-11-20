/* make goals-test: GOALS' record, run timer and splits (platform/ui/goals_core.c),
 * without the game.  Prints "goals-test: N checks, F failed". */
#include <stdio.h>
#include <string.h>

#include "ui/goals_core.h"

static int sN, sF;
#define CHECK(c) do { sN++; if (!(c)) { sF++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    GoalsSave s, t;
    GoalsRun r;
    char buf[1024];
    int i;

    /* the list: every goal named, in the menu's letters, short enough */
    for (i = 0; i < GOALS_N; i++) {
        const char *p;

        CHECK(gGoalInfo[i].name && gGoalInfo[i].desc);
        CHECK(strlen(gGoalInfo[i].name) <= 22 && strlen(gGoalInfo[i].desc) <= 28);
        for (p = gGoalInfo[i].name; *p; p++)
            CHECK(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :.-!/%+", *p) != NULL);
        for (p = gGoalInfo[i].desc; *p; p++)
            CHECK(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :.-!/%+", *p) != NULL);
    }
    CHECK(GOALS_N + 5 == 39);   /* PORT_GOALS_ROWS (goals.h) */

    /* the record round trips */
    GoalsSaveDefaults(&s);
    CHECK(GoalsCount(&s) == 0 && s.splitsOn == 1 && s.timerOn == 0);
    CHECK(GoalsUnlock(&s, GOAL_KRACKO) == 1);
    CHECK(GoalsUnlock(&s, GOAL_KRACKO) == 0);
    CHECK(GoalsUnlock(&s, GOAL_HOUR) == 1);
    CHECK(GoalsUnlock(&s, GOALS_N) == 0 && GoalsUnlock(&s, -1) == 0);
    CHECK(GoalsCount(&s) == 2);
    s.abilities = 0x6;
    s.midbosses = 0x81;
    s.bestStreak = 12;
    s.playFrames = 123456;
    s.timerOn = 1;
    s.bestRta[3] = 61234;
    s.bestIgt[3] = 3600;
    GoalsSaveFormat(&s, buf, sizeof buf);
    GoalsSaveDefaults(&t);
    GoalsSaveParse(&t, buf);
    CHECK(memcmp(&s, &t, sizeof s) == 0);
    /* junk and unknown keys are ignored */
    GoalsSaveDefaults(&t);
    GoalsSaveParse(&t, "nonsense\nfoo=1\nbest99=1,2\nunlocked=0xffffffffffffffff\n");
    CHECK(t.unlocked == (1ull << GOALS_N) - 1);
    GoalsSaveParse(&t, "");
    GoalsSaveParse(&t, NULL);

    /* the run: starts at its first frame in play, real time skips stalls */
    GoalsRunReset(&r);
    GoalsRunTick(&r, 16.7, 0);
    CHECK(!r.started && r.rtaMs == 0.0 && r.igt == 0);
    GoalsRunTick(&r, 16.7, 1);
    CHECK(r.started && r.rtaMs == 0.0 && r.igt == 1);
    GoalsRunTick(&r, 20.0, 1);
    GoalsRunTick(&r, 20.0, 0);          /* a menu: real time only */
    GoalsRunTick(&r, 5000.0, 1);        /* a hidden tab */
    CHECK(r.rtaMs == 40.0 && r.igt == 3);

    /* splits: a new best is kept, a slower one is not, once a run */
    GoalsSaveDefaults(&s);
    CHECK(GoalsRunSplit(&r, &s, 2) == 1 && s.bestRta[2] == 40 && s.bestIgt[2] == 3);
    CHECK(GoalsRunSplit(&r, &s, 2) == 0 && r.lastSplit == 2);
    GoalsRunReset(&r);
    GoalsRunTick(&r, 0, 1);
    for (i = 0; i < 10; i++) GoalsRunTick(&r, 10.0, 1);
    CHECK(GoalsRunSplit(&r, &s, 2) == 0 && s.bestRta[2] == 40 && r.splitRta[2] == 100);
    CHECK(GoalsRunSplit(&r, &s, GOALS_SPLIT_END) == 1 && r.finished);
    GoalsRunTick(&r, 10.0, 1);
    CHECK(r.rtaMs == 100.0);            /* stopped at the end */
    CHECK(GoalsRunSplit(&r, &s, GOALS_SPLITS) == 0);

    /* the time format */
    GoalsFormatMs(0, buf, sizeof buf);          CHECK(!strcmp(buf, "0:00.0"));
    GoalsFormatMs(61234, buf, sizeof buf);      CHECK(!strcmp(buf, "1:01.2"));
    GoalsFormatMs(3723400, buf, sizeof buf);    CHECK(!strcmp(buf, "1:02:03.4"));
    GoalsFormatDelta(-1500, buf, sizeof buf);   CHECK(!strcmp(buf, "-0:01.5"));
    GoalsFormatDelta(2500, buf, sizeof buf);    CHECK(!strcmp(buf, "+0:02.5"));
    CHECK(GoalsFramesMs(59727) > 999990.0 && GoalsFramesMs(59727) < 1000010.0);

    printf("goals-test: %d checks, %d failed\n", sN, sF);
    return sF != 0;
}
