#ifndef GUARD_PORT_CPU_PLANNER_H
#define GUARD_PORT_CPU_PLANNER_H
/* The planner host: asks a slow planner (a Haiku agent, or the mock) for
 * CPU Kirby directives and turns the answers into recorded `cpugoal` lines
 * (docs/design/CPU-HAIKU.md, port/cpu_ai.h).
 *
 * Host-only.  Nothing here is simulation state: it reads the simulation
 * (CpuAiDirState, CpuAiView) between frames, and the only way it changes
 * anything is PortInputLogScheduleCpuGoal -- an input, latched and
 * recorded like a key press.  It never runs while a recording replays, and
 * the game never waits for it (except KATAM_PLANNER_SYNC=1, a test switch).
 *
 * The transport is the host's: one JSON object per line each way, a
 * `decide` request out (the observation) and a directive back.  Natively it
 * is a child process on a pipe (platform/native/planner_pipe.c,
 * KATAM_PLANNER=CMD); tools/haiku_ai/bridge.py is that child. */

#ifdef __cplusplus
extern "C" {
#endif

struct PortPlannerOps {
    /* One line (no newline; the transport adds it).  0 sent, -1 not now. */
    int (*send)(const char *line);
    /* One line without its newline into buf: its length; 0 none yet (after
     * waiting up to waitMs); -1 the planner is gone. */
    int (*recv)(char *buf, int cap, int waitMs);
    /* Write the picture as it is now to `path` (PNG).  1 written. */
    int (*shot)(const char *path);
    /* Where pictures go (NULL: none are taken). */
    const char *shotDir;
};

/* The host's transport (NULL: no planner, the default). */
void PortPlannerSetOps(const struct PortPlannerOps *ops);
/* Once a frame, at the latch, before the input log latches (platform/main.c). */
void PortPlannerPoll(void);
int  PortPlannerOn(void);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_CPU_PLANNER_H */
