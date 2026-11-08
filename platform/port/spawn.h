#ifndef GUARD_PORT_SPAWN_H
#define GUARD_PORT_SPAWN_H
/* The game's spawn and despawn windows, sized for the session's view.
 *
 * Included by the game sources that hold the windows (src/code_080023A4.c,
 * src/kirby.c) and implemented in platform/view.c.  At the default 240x160
 * view every value here is the constant the game was built with, so the
 * patched code behaves exactly as it shipped.  docs/VIEW.md, "Spawn margin". */
#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct PortSpawnMargin {
    /* Extra 16-pixel object cells of spawn window beyond the stock one, per
     * side (left, right, top, bottom).  0 at the default view. */
    s32 padL, padR, padT, padB;
    /* Despawn bounds: how far an object's position may sit from the centre
     * of the 240x160 window before it is destroyed, per side.  168 (120+48)
     * and 128 (80+48) at the default view; 16 per pad cell more otherwise,
     * so the despawn box always contains the spawn window. */
    s32 cullL, cullR, cullT, cullB;
};

/* Player `player`'s windows for its camera now, for this frame's view. */
void PortSpawnMarginNow(u8 player, struct PortSpawnMargin *m);
/* The same for the camera the frame started with (viewportPositionUnmodified)
 * and the previous frame's view: the window the game had already scanned. */
void PortSpawnMarginPrev(u8 player, struct PortSpawnMargin *m);
/* Non-zero when the session view is the stock one (nothing widened). */
int PortSpawnStock(void);
/* Whether the room walks may create one more object for `player`: the
 * object-node pool (64) and the task pool (128) have no failure path in the
 * game, so a widened window keeps a reserve of both.  Always 1 at the stock
 * view.  (x, y) is the spawn point in world pixels and `edge` says which walk
 * asks (0 the whole-window walk, 1 a scroll-in edge) -- for the diagnostics
 * only (KATAM_SPAWNSTAT); neither changes the answer. */
int PortSpawnHeadroom(u8 player, s32 x, s32 y, int edge);
/* Diagnostics only: a level object (struct Object) is being destroyed by
 * the despawn test at world pixel (x, y). */
void PortSpawnNoteDespawn(u8 player, s32 x, s32 y);

#ifdef __cplusplus
}
#endif
#endif
