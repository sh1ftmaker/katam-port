/* The planner host (port/cpu_planner.h, docs/design/CPU-HAIKU.md).
 *
 * Event-driven, never per frame: for each CPU Kirby it plans for, it asks
 * the planner for a directive when the Kirby has none yet, when its
 * directive has ended (done, failed, timed out), or when one has run far
 * past its limit (stale).  The question is an observation -- the Kirby, its
 * room as the CPU hints see it (doors and their groups, what is open and
 * reachable, the nearest unexplored and boss routes), objects, the team,
 * the game's progress, how the last directive went -- and, when the Kirby is
 * in the room the screen shows, a picture.  The answer becomes a
 * `cpugoal` line for the next latch (PortInputLogScheduleCpuGoal): recorded,
 * so the run replays without the planner.  Until it comes the Kirby runs its
 * mode; a planner that is slow, wrong or gone costs nothing but its advice.
 *
 *   KATAM_PLANNER_SLOTS=HEX   slots to plan for (bit s = Kirby s + 1);
 *                             default every CPU slot
 *   KATAM_PLANNER_FROM=F      first frame to ask at (default 0)
 *   KATAM_PLANNER_SYNC=1      wait for each answer (tests: a reproducible
 *                             live run; real play never waits)
 *   KATAM_PLANNER_BUDGET=N    at most N questions a minute of game time
 *                             (default 30), all slots together
 *
 * Host state only (not simulation state); never runs during a replay or in
 * an online session (the host-only, sealed-event version is future work). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "treasures.h"
#include "port/port.h"
#include "port/simblock.h"
#include "port/localmp.h"
#include "port/inputlog.h"
#include "port/lmpnet.h"
#include "port/cpu_ai.h"
#include "port/cpu_planner.h"

#ifdef __cplusplus
extern "C" {
#endif

u64 PortHostMicros(void);

static const struct PortPlannerOps *sOps;

enum { P_IDLE, P_ASKED, P_SCHED, P_RUN };

struct PSlot {
    int state;
    int reqId;
    u32 nextAsk;            /* frame */
    u32 askedFrame;
    u64 askedUs;
    u32 schedFrame;         /* the cpugoal line's frame */
    int kind;               /* what it was given */
    u32 a, b, c, secs;
    int quickFails;
    const char *reason;     /* why the next question is asked */
    char last[192];         /* the previous directive and how it ended (JSON) */
};

static struct PSlot sSlots[PORT_CPUAI_SLOTS];
static int sInit, sSync, sReqSeq, sGone;
static u32 sMask = 0xFFFE, sFrom, sBudget = 30;
static u32 sWindowStart, sWindowAsks;
static u32 sShotFrame = 0xFFFFFFFF;
static char sShotPath[512];

void PortPlannerSetOps(const struct PortPlannerOps *ops)
{
    sOps = ops;
}

int PortPlannerOn(void)
{
    return sOps != NULL && !sGone;
}

static void Init(void)
{
    const char *e;
    int s;

    sInit = 1;
    if ((e = getenv("KATAM_PLANNER_SLOTS")) != NULL && *e)
        sMask = (u32)strtoul(e, NULL, 16) & 0xFFFE;
    if ((e = getenv("KATAM_PLANNER_FROM")) != NULL && *e)
        sFrom = (u32)strtoul(e, NULL, 10);
    if ((e = getenv("KATAM_PLANNER_BUDGET")) != NULL && *e)
        sBudget = (u32)strtoul(e, NULL, 10);
    sSync = (e = getenv("KATAM_PLANNER_SYNC")) != NULL && atoi(e) > 0;
    for (s = 0; s < PORT_CPUAI_SLOTS; s++) {
        sSlots[s].state = P_IDLE;
        sSlots[s].reason = "start";
        strcpy(sSlots[s].last, "null");
    }
    PortLog("[planner] on: slots %04X from f%u%s, %u questions a minute at most",
            (unsigned)sMask, (unsigned)sFrom, sSync ? ", waiting for each answer" : "",
            (unsigned)sBudget);
}

/* --- the observation -------------------------------------------------------- */

struct Buf {
    char *p;
    int len, cap;
};

static void Put(struct Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void Put(struct Buf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->len >= b->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, (size_t)(b->cap - b->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        b->len = b->len + n < b->cap - 1 ? b->len + n : b->cap - 1;
}

static const char *AbilityName(int a)
{
    static const char *const kNames[] = {
        "normal", "fire", "ice", "burning", "wheel", "parasol", "cutter", "beam", "stone",
        "bomb", "throw", "sleep", "cook", "laser", "ufo", "spark", "tornado", "hammer",
        "sword", "cupid", "fighter", "magic", "smash", "mini", "crash", "missile", "master",
    };

    return a >= 0 && a < (int)(sizeof kNames / sizeof kNames[0]) ? kNames[a] : "?";
}

static int Playable(u32 slot)
{
    u16 room = gCurLevelInfo[slot].currentRoom;

    if (slot >= gNumKirbys || PortIsHuman(slot) || (gUnk_0203AD10 & 0x10))
        return 0;
    if (room == 0xFFFF || room >= 983 || gKirbys[slot].hp <= 0)
        return 0;
    if (room >= 0x323 && room <= 0x325)  /* the opening: the people's (CPU-AI.md) */
        return 0;
    return 1;
}

static void Team(struct Buf *b)
{
    u32 i;

    Put(b, "\"team\":[");
    for (i = 0; i < gNumKirbys && i < PORT_CPUAI_SLOTS; i++) {
        struct PortCpuGoalState ds;

        CpuAiDirState(i, &ds);
        Put(b, "%s{\"slot\":%u,\"person\":%d,\"room\":%u,\"x\":%d,\"y\":%d,\"hp\":%d,\"ability\":\"%s\"",
            i ? "," : "", (unsigned)i, PortIsHuman(i) ? 1 : 0, (unsigned)gCurLevelInfo[i].currentRoom,
            (int)(gKirbys[i].base.x >> 12), (int)(gKirbys[i].base.y >> 12), (int)gKirbys[i].hp,
            AbilityName(gKirbys[i].ability));
        if (!PortIsHuman(i)) {
            Put(b, ",\"mode\":\"%s\"", PortCpuAiName(PortCpuAiWordMode(PortCpuAiWord(), i)));
            if (ds.status == PORT_CPUGOAL_ST_RUNNING)
                Put(b, ",\"doing\":\"%s\",\"args\":[%u,%u,%u]", PortCpuGoalName(ds.kind),
                    (unsigned)ds.a, (unsigned)ds.b, (unsigned)ds.c);
        }
        Put(b, "}");
    }
    Put(b, "]");
}

static void Observe(struct Buf *b, u32 slot, const char *reason, const char *shot)
{
    struct PortCpuAiView v;
    struct Kirby *k = &gKirbys[slot];
    static const char *const kObj[5] = { "food", "shard", "boss", "chest", "enemy" };
    int i;

    CpuAiView(slot, &v);
    Put(b, "{\"type\":\"decide\",\"id\":%d,\"slot\":%u,\"frame\":%u,\"reason\":\"%s\",\"shot\":",
        sSlots[slot].reqId, (unsigned)slot, (unsigned)gPortSim->frame.frameCount, reason);
    if (shot != NULL)
        Put(b, "\"%s\"", shot);
    else
        Put(b, "null");
    Put(b, ",\"last\":%s", sSlots[slot].last);
    Put(b, ",\"me\":{\"room\":%u,\"area\":%u,\"room_flags\":%u,\"x\":%d,\"y\":%d,\"hp\":%d,\"max_hp\":%d,"
           "\"ability\":\"%s\",\"tier\":%u,\"mode\":\"%s\",\"came_from\":%d,\"room_entered_before\":%u,"
           "\"room_script\":%u,\"with_player\":%d}",
        (unsigned)v.room, (unsigned)v.area, (unsigned)v.flags, (int)(k->base.x >> 12), (int)(k->base.y >> 12),
        (int)k->hp, (int)k->maxHp, AbilityName(k->ability), (unsigned)v.tier,
        PortCpuAiName(PortCpuAiWordMode(PortCpuAiWord(), slot)),
        v.prevRoom == 0xFFFF ? -1 : (int)v.prevRoom, (unsigned)v.entered, (unsigned)v.script,
        gCurLevelInfo[0].currentRoom == v.room ? 1 : 0);
    /* doors: [x, y, to, group, open, reach, entered] */
    Put(b, ",\"doors\":[");
    for (i = 0; i < v.nDoors; i++)
        Put(b, "%s[%u,%u,%u,%u,%u,%u,%u]", i ? "," : "", (unsigned)v.doors[i].x, (unsigned)v.doors[i].y,
            (unsigned)v.doors[i].to, (unsigned)v.doors[i].group, (unsigned)v.doors[i].open,
            (unsigned)v.doors[i].reach, (unsigned)v.doors[i].entered);
    Put(b, "],\"more_doors\":%u", (unsigned)v.moreDoors);
    if (v.exploreRoom != 0xFFFF)
        Put(b, ",\"explore\":{\"room\":%u,\"door\":[%u,%u]}", (unsigned)v.exploreRoom,
            (unsigned)v.exploreX, (unsigned)v.exploreY);
    else
        Put(b, ",\"explore\":null");
    if (v.bossRoom != 0xFFFF)
        Put(b, ",\"boss_route\":{\"room\":%u,\"door\":[%u,%u]}", (unsigned)v.bossRoom,
            (unsigned)v.bossX, (unsigned)v.bossY);
    else
        Put(b, ",\"boss_route\":null");
    Put(b, ",\"objects\":{");
    for (i = 0; i < 5; i++) {
        if (v.count[i])
            Put(b, "%s\"%s\":[%u,%u,%u]", i ? "," : "", kObj[i], (unsigned)v.count[i],
                (unsigned)v.nearX[i], (unsigned)v.nearY[i]);
        else
            Put(b, "%s\"%s\":[0]", i ? "," : "", kObj[i]);
    }
    Put(b, "},\"blocked\":[");
    for (i = 0; i < v.nBlocked; i++)
        Put(b, "%s[%u,%u]", i ? "," : "", (unsigned)v.blockedFrom[i], (unsigned)v.blockedTo[i]);
    Put(b, "],");
    Team(b);
    Put(b, ",\"progress\":{\"shards\":%u,\"rooms_entered\":%u,\"bosses\":[", (unsigned)NumShardsCollected(),
        (unsigned)v.roomsEntered);
    for (i = 0; i < 8; i++)
        Put(b, "%s[%u,%u,%u]", i ? "," : "", (unsigned)v.bossRooms[i][0], (unsigned)v.bossRooms[i][1],
            (unsigned)((v.bossBeaten >> i) & 1));
    Put(b, "]}}");
}

/* --- asking and answering ---------------------------------------------------- */

static void Ask(u32 slot, const char *reason)
{
    struct PSlot *ps = &sSlots[slot];
    char line[4000];
    struct Buf b = { line, 0, (int)sizeof line };
    const char *shot = NULL;
    u32 now = gPortSim->frame.frameCount;

    ps->reqId = ++sReqSeq;
    /* the picture shows the local player's camera: only worth sending when
     * this Kirby is in that room; one file per frame for every slot */
    if (sOps->shot != NULL && sOps->shotDir != NULL
        && gCurLevelInfo[slot].currentRoom == gCurLevelInfo[0].currentRoom) {
        if (sShotFrame != now) {
            snprintf(sShotPath, sizeof sShotPath, "%s/f%07u.png", sOps->shotDir, (unsigned)now);
            sShotFrame = sOps->shot(sShotPath) ? now : 0xFFFFFFFF;
        }
        if (sShotFrame == now)
            shot = sShotPath;
    }
    Observe(&b, slot, reason, shot);
    if (b.len >= b.cap - 1) {
        PortError("[planner] observation too long for k%u; not asked", (unsigned)slot);
        ps->nextAsk = now + 600;
        return;
    }
    if (sOps->send(line) != 0) {
        ps->nextAsk = now + 60;         /* the pipe is full: later */
        return;
    }
    ps->state = P_ASKED;
    ps->askedFrame = now;
    ps->askedUs = PortHostMicros();
    sWindowAsks++;
    PortLog("[planner] f%u k%u ask #%d (%s)", (unsigned)now, (unsigned)slot, ps->reqId, reason);
}

/* "key": value in a flat JSON object; numbers only (strings via JStr). */
static int JNum(const char *s, const char *key, long *out)
{
    char pat[40];
    const char *p;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    if ((p = strstr(s, pat)) == NULL)
        return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (!(*p == '-' || (*p >= '0' && *p <= '9')))
        return 0;
    *out = strtol(p, NULL, 10);
    return 1;
}

static int JStr(const char *s, const char *key, char *out, int cap)
{
    char pat[40];
    const char *p;
    int n = 0;

    snprintf(pat, sizeof pat, "\"%s\"", key);
    if ((p = strstr(s, pat)) == NULL)
        return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    if (*p++ != '"')
        return 0;
    while (*p && *p != '"' && n < cap - 1)
        out[n++] = *p++;
    out[n] = '\0';
    return 1;
}

static void Answer(const char *line)
{
    long id = -1, slot = -1, v;
    char kind[24];
    int k;
    u32 a = 0, b = 0, c = 0, secs = 0, now = gPortSim->frame.frameCount, f;
    struct PSlot *ps;

    if (!JNum(line, "id", &id) || !JNum(line, "slot", &slot) || slot < 1 || slot >= PORT_CPUAI_SLOTS)
        return;
    ps = &sSlots[slot];
    if (ps->state != P_ASKED || ps->reqId != id)
        return;                         /* late: that question was dropped */
    if (!JStr(line, "kind", kind, sizeof kind) || (k = PortCpuGoalKindOf(kind)) <= PORT_CPUGOAL_NONE) {
        /* "none", or nonsense: the mode runs; ask again in 10 s */
        ps->state = P_IDLE;
        ps->nextAsk = now + 600;
        ps->reason = "declined";
        PortLog("[planner] f%u k%ld #%ld: no directive (%s)", (unsigned)now, slot, id, line);
        return;
    }
    if (JNum(line, "secs", &v) && v > 0)
        secs = (u32)(v > 255 ? 255 : v);
    switch (k) {
    case PORT_CPUGOAL_ROOM:
        a = JNum(line, "room", &v) && v >= 0 && v < 983 ? (u32)v : 0;
        b = JNum(line, "group", &v) && v >= 0 && v < 0xFE ? (u32)v + 1 : 0;
        break;
    case PORT_CPUGOAL_DOOR: case PORT_CPUGOAL_XY:
        a = PORT_CPUGOAL_HERE;
        b = JNum(line, "x", &v) && v >= 0 && v < 256 ? (u32)v : 0;
        c = JNum(line, "y", &v) && v >= 0 && v < 256 ? (u32)v : 0;
        break;
    case PORT_CPUGOAL_FOLLOW:
        a = JNum(line, "who", &v) && v >= 0 && v < PORT_CPUAI_SLOTS ? (u32)v : 0;
        break;
    default:
        break;
    }
    f = PortInputLogScheduleCpuGoal((u8)slot, k, a, b, c, secs);
    ps->state = P_SCHED;
    ps->schedFrame = f;
    ps->kind = k;
    ps->a = a;
    ps->b = b;
    ps->c = c;
    ps->secs = secs;
    PortLog("[planner] f%u k%ld #%ld -> %s %u %u %u %us at f%u (%.0f ms)", (unsigned)now, slot, id,
            PortCpuGoalName(k), (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)secs, (unsigned)f,
            (double)(PortHostMicros() - ps->askedUs) / 1000.0);
}

static void Drain(int waitMs)
{
    char line[2048];
    int n;

    while ((n = sOps->recv(line, sizeof line, waitMs)) != 0) {
        if (n < 0) {
            if (!sGone)
                PortError("[planner] the planner has gone; the Kirbys keep their modes");
            sGone = 1;
            return;
        }
        Answer(line);
        waitMs = 0;
    }
}

/* The directive that just ended, for the next question. */
static void Outcome(struct PSlot *ps, const struct PortCpuGoalState *ds, u32 now, const char *status)
{
    snprintf(ps->last, sizeof ps->last,
             "{\"kind\":\"%s\",\"a\":%u,\"b\":%u,\"c\":%u,\"secs\":%u,\"status\":\"%s\",\"why\":\"%s\","
             "\"took_s\":%u}",
             PortCpuGoalName(ps->kind), (unsigned)ps->a, (unsigned)ps->b, (unsigned)ps->c,
             (unsigned)ps->secs, status, ds ? PortCpuGoalWhyName(ds->why) : "lost",
             (unsigned)((now - ps->schedFrame) / 60));
}

void PortPlannerPoll(void)
{
    u32 now, s, n;

    if (sOps == NULL || sGone)
        return;
    /* inputs only: a replay applies its own cpugoal lines, and an online
     * session's would have to be the host's sealed events (future work) */
    if (PortInputLogReplaying() || gPortLnOn)
        return;
    if (!sInit)
        Init();
    now = gPortSim->frame.frameCount;
    if (now - sWindowStart >= 3600) {
        sWindowStart = now;
        sWindowAsks = 0;
    }
    Drain(0);
    n = gNumKirbys < PORT_CPUAI_SLOTS ? gNumKirbys : PORT_CPUAI_SLOTS;
    for (s = 1; s < n && !sGone; s++) {
        struct PSlot *ps = &sSlots[s];
        struct PortCpuGoalState ds;

        if (!(sMask & (1u << s)))
            continue;
        CpuAiDirState(s, &ds);
        switch (ps->state) {
        case P_SCHED:
            if (now > ps->schedFrame)   /* latched last frame */
                ps->state = P_RUN;
            break;
        case P_RUN:
            if (ds.status != PORT_CPUGOAL_ST_RUNNING || ds.kind != ps->kind) {
                int lost = ds.status == PORT_CPUGOAL_ST_NONE || ds.kind != ps->kind;
                int quick = ds.status == PORT_CPUGOAL_ST_FAILED && now - ps->schedFrame < 120;

                Outcome(ps, lost ? NULL : &ds, now, lost ? "lost" : PortCpuGoalStatusName(ds.status));
                PortLog("[planner] f%u k%u %s %s (%s) after %us", (unsigned)now, (unsigned)s,
                        PortCpuGoalName(ps->kind), lost ? "lost" : PortCpuGoalStatusName(ds.status),
                        lost ? "-" : PortCpuGoalWhyName(ds.why), (unsigned)((now - ps->schedFrame) / 60));
                ps->quickFails = quick ? ps->quickFails + 1 : 0;
                /* a planner that keeps asking for the impossible: back off */
                ps->nextAsk = now + (ps->quickFails ? (60u << (ps->quickFails < 5 ? ps->quickFails : 5)) : 0);
                ps->reason = lost ? "lost" : "ended";
                ps->state = P_IDLE;
            } else if (now - ps->schedFrame > (ps->secs ? ps->secs : 90) * 60 + 1800) {
                Outcome(ps, &ds, now, "stale");
                ps->nextAsk = now;
                ps->reason = "stale";
                ps->state = P_IDLE;
            }
            break;
        case P_ASKED:
            if (!sSync && PortHostMicros() - ps->askedUs > 30000000u) {
                PortLog("[planner] f%u k%u #%d: no answer in 30 s; the mode runs", (unsigned)now,
                        (unsigned)s, ps->reqId);
                ps->state = P_IDLE;
                ps->nextAsk = now + 600;
                ps->reason = "no_answer";
            }
            break;
        default:                        /* P_IDLE */
            if (now < ps->nextAsk || now < sFrom || !Playable(s))
                break;
            if (sBudget && sWindowAsks >= sBudget)
                break;
            Ask(s, ps->reason);
            if (sSync && ps->state == P_ASKED) {
                u64 t0 = PortHostMicros();

                while (ps->state == P_ASKED && !sGone && PortHostMicros() - t0 < 60000000u)
                    Drain(1000);
                if (ps->state == P_ASKED) {
                    PortError("[planner] k%u #%d: no answer in 60 s", (unsigned)s, ps->reqId);
                    ps->state = P_IDLE;
                    ps->nextAsk = now + 600;
                    ps->reason = "no_answer";
                }
            }
            break;
        }
    }
}

#ifdef __cplusplus
}
#endif
