/* GOALS: the list, the record, the run (platform/ui/goals_core.h). */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/goals_core.h"

const GoalInfo gGoalInfo[GOALS_N] = {
    [GOAL_KRACKO]          = { "KRACKO",            "BEAT KRACKO" },
    [GOAL_KING_GOLEM]      = { "KING GOLEM",        "BEAT KING GOLEM" },
    [GOAL_MOLEY]           = { "MOLEY",             "BEAT MOLEY" },
    [GOAL_MEGA_TITAN]      = { "MEGA TITAN",        "BEAT MEGA TITAN" },
    [GOAL_GOBBLER]         = { "GOBBLER",           "BEAT GOBBLER" },
    [GOAL_WIZ]             = { "WIZ",               "BEAT WIZ" },
    [GOAL_HANDS]           = { "CRAZY HAND",        "BEAT MASTER AND CRAZY HAND" },
    [GOAL_DARK_META_KNIGHT]= { "DARK META KNIGHT",  "BEAT DARK META KNIGHT" },
    [GOAL_DARK_MIND]       = { "DARK MIND",         "BEAT DARK MIND" },
    [GOAL_MIDBOSS]         = { "MID-BOSS",          "BEAT A MID-BOSS" },
    [GOAL_ALL_MIDBOSSES]   = { "EVERY MID-BOSS",    "BEAT ALL 8 MID-BOSSES" },
    [GOAL_SHARD1]          = { "FIRST SHARD",       "FIND A MIRROR SHARD" },
    [GOAL_SHARD4]          = { "HALF A MIRROR",     "FIND 4 MIRROR SHARDS" },
    [GOAL_SHARD8]          = { "MIRROR MENDED",     "FIND ALL 8 MIRROR SHARDS" },
    [GOAL_ALL_AREAS]       = { "WORLD TRAVELLER",   "VISIT ALL 9 AREAS" },
    [GOAL_AREA_DONE]       = { "AREA COMPLETE",     "EVERY ROOM OF AN AREA DONE" },
    [GOAL_MAP50]           = { "HALF THE MAP",      "50% OF THE ROOMS DONE" },
    [GOAL_MAP100]          = { "100% MAP",          "EVERY ROOM DONE" },
    [GOAL_ALL_MAPS]        = { "CARTOGRAPHER",      "FIND ALL 9 AREA MAPS" },
    [GOAL_COPY1]           = { "COPYCAT",           "COPY AN ABILITY" },
    [GOAL_COPY10]          = { "TEN TALENTS",       "COPY 10 ABILITIES" },
    [GOAL_COPY_ALL]        = { "MASTER OF ALL",     "COPY ALL 26 ABILITIES" },
    [GOAL_MASTER]          = { "MASTER SWORD",      "BECOME MASTER KIRBY" },
    [GOAL_CHEST]           = { "TREASURE",          "OPEN A CHEST" },
    [GOAL_VITALITY]        = { "FULL HEALTH",       "FIND ALL 4 VITALITIES" },
    [GOAL_STREAK10]        = { "CAREFUL",           "10 ROOMS IN A ROW NO MISS" },
    [GOAL_STREAK50]        = { "UNTOUCHABLE",       "50 ROOMS IN A ROW NO MISS" },
    [GOAL_BOSS_NO_MISS]    = { "FLAWLESS",          "A BOSS ROOM WITH NO MISS" },
    [GOAL_ONLINE]          = { "ONLINE",            "PLAY AN ONLINE GAME" },
    [GOAL_ONLINE4]         = { "FULL TEAM",         "4 PLAYERS ONLINE" },
    [GOAL_ONLINE8]         = { "PARTY OF 8",        "8 PLAYERS ONLINE" },
    [GOAL_ONLINE_BOSS]     = { "TEAMWORK",          "BEAT A BOSS ONLINE" },
    [GOAL_ONLINE_SHARD]    = { "SHARED SHARD",      "FIND A SHARD ONLINE" },
    [GOAL_HOUR]            = { "ONE HOUR",          "PLAY FOR AN HOUR" },
};

void GoalsSaveDefaults(GoalsSave *s)
{
    memset(s, 0, sizeof(*s));
    s->splitsOn = 1;
}

static unsigned long long Num(const char *v)
{
    return strtoull(v, NULL, 0);
}

void GoalsSaveParse(GoalsSave *s, const char *text)
{
    const char *p = text;

    while (p && *p) {
        char line[96], *eq;
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);

        if (n >= sizeof line)
            n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = nl ? nl + 1 : NULL;
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';
        if (!strcmp(line, "unlocked")) s->unlocked = Num(eq) & ((1ull << GOALS_N) - 1);
        else if (!strcmp(line, "abilities")) s->abilities = (uint32_t)Num(eq);
        else if (!strcmp(line, "midbosses")) s->midbosses = (uint32_t)Num(eq) & 0xFF;
        else if (!strcmp(line, "bosses")) s->bosses = (uint32_t)Num(eq);
        else if (!strcmp(line, "streak")) s->bestStreak = (uint32_t)Num(eq);
        else if (!strcmp(line, "frames")) s->playFrames = (uint32_t)Num(eq);
        else if (!strcmp(line, "timer")) s->timerOn = Num(eq) != 0;
        else if (!strcmp(line, "splits")) s->splitsOn = Num(eq) != 0;
        else if (!strncmp(line, "best", 4)) {
            int k = atoi(line + 4);
            char *c = strchr(eq, ',');

            if (k >= 0 && k < GOALS_SPLITS && c) {
                s->bestRta[k] = (uint32_t)strtoul(eq, NULL, 10);
                s->bestIgt[k] = (uint32_t)strtoul(c + 1, NULL, 10);
            }
        }
    }
}

int GoalsSaveFormat(const GoalsSave *s, char *buf, int size)
{
    int n, k;

    n = snprintf(buf, size, "unlocked=0x%llx\nabilities=0x%x\nmidbosses=0x%x\nbosses=0x%x\n"
                 "streak=%u\nframes=%u\ntimer=%d\nsplits=%d\n",
                 (unsigned long long)s->unlocked, (unsigned)s->abilities, (unsigned)s->midbosses,
                 (unsigned)s->bosses, (unsigned)s->bestStreak, (unsigned)s->playFrames,
                 s->timerOn, s->splitsOn);
    for (k = 0; k < GOALS_SPLITS && n > 0 && n < size; k++)
        if (s->bestRta[k] || s->bestIgt[k])
            n += snprintf(buf + n, size - n, "best%d=%u,%u\n", k, (unsigned)s->bestRta[k],
                          (unsigned)s->bestIgt[k]);
    return n;
}

int GoalsUnlock(GoalsSave *s, int goal)
{
    uint64_t b;

    if (goal < 0 || goal >= GOALS_N)
        return 0;
    b = 1ull << goal;
    if (s->unlocked & b)
        return 0;
    s->unlocked |= b;
    return 1;
}

int GoalsCount(const GoalsSave *s)
{
    int i, n = 0;

    for (i = 0; i < GOALS_N; i++)
        n += (int)((s->unlocked >> i) & 1);
    return n;
}

void GoalsRunReset(GoalsRun *r)
{
    memset(r, 0, sizeof(*r));
    r->lastSplit = -1;
}

void GoalsRunTick(GoalsRun *r, double dtMs, int inPlay)
{
    if (r->finished)
        return;
    if (!r->started) {
        if (!inPlay)
            return;
        r->started = 1;
        dtMs = 0.0;         /* the clock starts with this frame */
    }
    if (dtMs > 0.0 && dtMs < 1000.0)    /* a hidden tab, a stall: not counted */
        r->rtaMs += dtMs;
    if (inPlay)
        r->igt++;
}

int GoalsRunSplit(GoalsRun *r, GoalsSave *s, int k)
{
    uint32_t rta, igt;
    int best = 0;

    if (k < 0 || k >= GOALS_SPLITS || !r->started || r->finished || r->splitRta[k])
        return 0;
    rta = (uint32_t)(r->rtaMs + 0.5);
    if (rta == 0)
        rta = 1;
    igt = r->igt ? r->igt : 1;
    r->splitRta[k] = rta;
    r->splitIgt[k] = igt;
    r->lastSplit = k;
    r->lastSplitAt = r->rtaMs;
    if (!s->bestRta[k] || rta < s->bestRta[k]) {
        s->bestRta[k] = rta;
        best = 1;
    }
    if (!s->bestIgt[k] || igt < s->bestIgt[k]) {
        s->bestIgt[k] = igt;
        best = 1;
    }
    if (k == GOALS_SPLIT_END)
        r->finished = 1;
    return best;
}

double GoalsFramesMs(uint32_t frames)
{
    return frames * (1000.0 / 59.7275);
}

void GoalsFormatMs(double ms, char *buf, int size)
{
    unsigned long t = ms > 0 ? (unsigned long)(ms / 100.0) : 0;    /* tenths */
    unsigned long h = t / 36000, m = t / 600 % 60, sec = t / 10 % 60, d = t % 10;

    if (h)
        snprintf(buf, size, "%lu:%02lu:%02lu.%lu", h, m, sec, d);
    else
        snprintf(buf, size, "%lu:%02lu.%lu", m, sec, d);
}

void GoalsFormatDelta(double ms, char *buf, int size)
{
    char t[24];

    GoalsFormatMs(fabs(ms), t, sizeof t);
    snprintf(buf, size, "%c%s", ms < 0 ? '-' : '+', t);
}
