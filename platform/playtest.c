/* The playtest boot: from power-on straight into a room.
 *
 * --warp on its own never reaches gameplay from a cold boot: the warp waits
 * for a level (platform/warp.c CanWarpNow), and the game does not start one
 * without input on the title, file select and intro.  So this drives the
 * keys the regression recordings use to boot (tools/make_recordings.py
 * boot(): tap A+Right on a 20-frame period from frame 120 -- with a fresh
 * save that walks into the first level by frame ~1200), schedules the warp at
 * PORT_PLAYTEST_WARP_FRAME through the input log (so a recording made of a
 * playtest replays it), and from then on leaves the keys to the player.
 *
 * tools/katam/lib/playtest.mjs writes the same boot as a katam-input
 * recording for the native `katam playtest`; the web page (web/playtest/)
 * calls PortPlaytestBoot before the ROM handshake releases main(). */

#include "global.h"
#include "port/port.h"
#include "port/inputlog.h"

#ifdef __cplusplus
extern "C" {
#endif

static struct {
    u8 active;
} sPlaytest;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE
#endif
void PortPlaytestBoot(u16 room, u8 cellX, u8 cellY)
{
    sPlaytest.active = 1;
    PortInputLogScheduleWarp(PORT_PLAYTEST_WARP_FRAME, 0, room, cellX, cellY);
    PortLog("[katam-port] playtest: boot, then warp to room %u cell (%u,%u) at frame %u",
            (unsigned)room, (unsigned)cellX, (unsigned)cellY, PORT_PLAYTEST_WARP_FRAME);
}

int PortPlaytestKeyOverride(u32 frame, u16 *keys)
{
    if (!sPlaytest.active)
        return 0;
    if (frame >= PORT_PLAYTEST_WARP_FRAME) {
        sPlaytest.active = 0;
        return 0;
    }
    *keys = (frame >= 120 && (frame - 120) % 20 < 10) ? 0x011 : 0;
    return 1;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int PortPlaytestBooting(void)
{
    return sPlaytest.active;
}

#ifdef __cplusplus
}
#endif
