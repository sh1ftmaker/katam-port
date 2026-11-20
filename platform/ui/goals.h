/* GOALS: local achievements and the run timer (platform/ui/goals.c,
 * docs/features/goals.md).  Presentation only: reads the frame shown, keeps
 * its record on this device, never writes game memory. */
#ifndef GUARD_PORT_UI_GOALS_H
#define GUARD_PORT_UI_GOALS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start keeping goals (the host, in an interactive run only): `path` the
 * native record file, NULL / "" the browser's localStorage.  Until then
 * nothing is read, drawn or stored -- scripted and test runs stay as they
 * were. */
void PortGoalsInit(const char *path);
int  PortGoalsActive(void);
/* Once per simulated frame, at the blit (present.c). */
void PortGoalsSample(double nowMs);
/* Over the presented frame: `pic` the picture (framebuffer x y w h), `scale`
 * framebuffer pixels per HUD pixel. */
void PortGoalsDraw(int fbW, int fbH, const int pic[4], double scale, int smooth);
/* The reset key (F9) and SETTINGS > GOALS > RESET TIMER. */
void PortGoalsTimerReset(void);
/* SETTINGS > GOALS (port/menu_poc.h struct PortPocRows) */
#define PORT_GOALS_ROWS 39     /* 5 + GOALS_N (goals_core.h) */
int  PortGoalsRowCount(void);
int  PortGoalsUnlockedCount(void);

#ifdef __cplusplus
}
#endif
#endif
