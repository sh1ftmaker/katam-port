#ifndef GUARD_PORT_STATES_H
#define GUARD_PORT_STATES_H
/* Single-player save states, rewind and fast-forward / slow motion.
 * See docs/features/states.md and platform/states.c.
 *
 * Everything here is local to this device and switched off while any online
 * or seated session is running (PortStAllowed): a state load, a rewind or a
 * change of speed would desync a room or hand one player an edge. */
#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

#define PORT_ST_SLOTS 9

/* 1 when save states, rewind and speed changes may be used right now. */
int  PortStAllowed(void);

/* Top of PortPresentFrame (platform/main.c), after the session hooks: runs
 * a queued save or load, steps the rewind, or records the rewind ring. */
void PortStFrame(void);

/* The page (or the menu) asks; the work happens at the next frame top,
 * never while the SETTINGS screen is up. */
void PortStRequestSave(int slot);           /* 1..9 */
void PortStRequestLoad(int slot);           /* 1..9: the page hands the blob */
/* The page's blob for a load: `buf` from malloc, ownership passes here. */
int  PortStLoadBlob(int slot, u8 *buf, u32 len);
void PortStSetRewindHeld(int held);
/* The page's settings, so the menu can show and change them. */
void PortStSetPrefs(int enabled, int rewindOn, int fast, int slow, int slot);
void PortStSetHave(u32 mask);               /* bit n-1: slot n has a state */

/* For the menu rows (platform/ui/states_rows.c). */
int  PortStSlot(void);
int  PortStRewindOn(void);
int  PortStFast(void);                      /* 2..8 */
int  PortStSlow(void);                      /* 2..4: 1/n speed */
u32  PortStHave(void);
void PortStStepPref(int which, int dir);    /* 0 slot 1 rewind 2 fast 3 slow */

/* Tests: frames held in the ring. */
int  PortStRingFrames(void);

#ifdef __cplusplus
}
#endif

#endif
