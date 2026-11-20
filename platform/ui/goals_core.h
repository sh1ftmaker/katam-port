/* GOALS: local achievements and the run timer -- the part that knows nothing
 * of the game (platform/ui/goals.c reads the game; docs/features/goals.md).
 * Pure data: the list, the saved record and its text form, the timer's
 * clock and splits, the time format.  Compiled on its own by the unit test
 * (make goals-test). */
#ifndef GUARD_PORT_UI_GOALS_CORE_H
#define GUARD_PORT_UI_GOALS_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* bosses (beaten while this device watched: the object's health ran
     * out, or it went while its room stayed) */
    GOAL_KRACKO, GOAL_KING_GOLEM, GOAL_MOLEY, GOAL_MEGA_TITAN, GOAL_GOBBLER, GOAL_WIZ,
    GOAL_HANDS, GOAL_DARK_META_KNIGHT, GOAL_DARK_MIND,
    /* mid-bosses */
    GOAL_MIDBOSS, GOAL_ALL_MIDBOSSES,
    /* mirror shards (the save's) */
    GOAL_SHARD1, GOAL_SHARD4, GOAL_SHARD8,
    /* the map */
    GOAL_ALL_AREAS, GOAL_AREA_DONE, GOAL_MAP50, GOAL_MAP100, GOAL_ALL_MAPS,
    /* copy abilities */
    GOAL_COPY1, GOAL_COPY10, GOAL_COPY_ALL, GOAL_MASTER,
    /* treasure */
    GOAL_CHEST, GOAL_VITALITY,
    /* no misses */
    GOAL_STREAK10, GOAL_STREAK50, GOAL_BOSS_NO_MISS,
    /* online */
    GOAL_ONLINE, GOAL_ONLINE4, GOAL_ONLINE8, GOAL_ONLINE_BOSS, GOAL_ONLINE_SHARD,
    /* time */
    GOAL_HOUR,
    GOALS_N
};

/* 26 copy abilities (KIRBY_ABILITY_FIRE..MASTER) */
#define GOALS_ABILITIES 26
/* the run timer's splits: a shard's area (1..9), then 10 the end (Dark Mind) */
#define GOALS_SPLITS 11
#define GOALS_SPLIT_END 10

typedef struct {
    const char *name;   /* upper case, the menu's letters, <= 22 */
    const char *desc;   /* <= 28 */
} GoalInfo;
extern const GoalInfo gGoalInfo[GOALS_N];

typedef struct {
    uint64_t unlocked;          /* bit per GOAL_*                          */
    uint32_t abilities;         /* bit per ability copied (1 << ability)   */
    uint32_t midbosses;         /* bit per mid-boss type - 0x38            */
    uint32_t bosses;            /* bit per boss goal                       */
    uint32_t bestStreak;        /* rooms in a row without a miss           */
    uint32_t playFrames;        /* in-game frames, all runs                */
    int timerOn, splitsOn;
    /* the best time to each split: real time (ms), in-game (frames); 0 none */
    uint32_t bestRta[GOALS_SPLITS], bestIgt[GOALS_SPLITS];
} GoalsSave;

void GoalsSaveDefaults(GoalsSave *s);
/* "key=value" lines; unknown keys ignored, missing ones left at default */
void GoalsSaveParse(GoalsSave *s, const char *text);
int  GoalsSaveFormat(const GoalsSave *s, char *buf, int size);
/* 1 if it was new */
int  GoalsUnlock(GoalsSave *s, int goal);
int  GoalsCount(const GoalsSave *s);

/* The run. */
typedef struct {
    int started, finished;
    double rtaMs;               /* real time, from the first frame in play */
    uint32_t igt;               /* frames in play                          */
    uint32_t splitRta[GOALS_SPLITS], splitIgt[GOALS_SPLITS];  /* 0 none */
    int lastSplit;              /* -1 none */
    double lastSplitAt;         /* rtaMs when it was taken                 */
} GoalsRun;

void GoalsRunReset(GoalsRun *r);
/* One step: `dtMs` real time since the last, `inPlay` a frame in play.  The
 * run starts at its first frame in play. */
void GoalsRunTick(GoalsRun *r, double dtMs, int inPlay);
/* Take split k (once a run); updates the best.  1 if it was a new best. */
int  GoalsRunSplit(GoalsRun *r, GoalsSave *s, int k);

/* "1:02:03.4" / "2:03.4" from ms; frames at 59.7275 Hz */
void GoalsFormatMs(double ms, char *buf, int size);
double GoalsFramesMs(uint32_t frames);
/* "+0:01.2" / "-0:01.2": a split against its best */
void GoalsFormatDelta(double ms, char *buf, int size);

#ifdef __cplusplus
}
#endif
#endif
