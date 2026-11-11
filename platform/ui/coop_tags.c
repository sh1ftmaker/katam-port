/* Co-op name tags: where the other players' Kirbys are on the page.
 *
 * The page draws the names (web/coop.js, HTML over the canvas: sharp text at
 * any size, any script); this says where.  For each remote player drawn in
 * the local room it gives the top of their Kirby, as a fraction of the
 * canvas, taken from the frame actually on screen -- the render list's
 * presented frame on the GPU path, so the widened view, zoom, the sub-pixel
 * camera and interpolation are all in it, and the tag sits on the Kirby
 * rather than a frame behind.  A remote Kirby in the room but not drawn (off
 * the view) gets a position worked out from the local Kirby's, flagged so
 * the page can pin it to the edge.
 *
 * Tags are keyed by a draw id, not a Kirby slot: today the ids are the
 * puppet slots 1..3, and players drawn outside gKirbys (more than four,
 * docs/COOP-HANDOFF.md) will take ids from PORT_COOP_TAG_EXTRA up.  The page
 * maps ids to players.
 *
 * Presentation only: reads game and renderer state, writes nothing.
 */

#include "global.h"
#include "kirby.h"
#include "port/port.h"
#include "port/coop.h"
#include "render/render_list.h"
#include "ui/ui.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define TAGS_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define TAGS_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_COOP_TAG_EXTRA 16   /* first id for players drawn outside gKirbys */
#define TAG_DRAWN     1          /* on the frame on screen                      */
#define TAG_OFFVIEW   2          /* in the room, not drawn: a worked-out place  */
#define HEAD_GUESS    (20 * 256) /* anchor to the top of a Kirby, when unknown  */

/* Where a Kirby's top and centre are in view pixels (1/256), and whether the
 * frame on screen drew it. */
static int Place(const struct Kirby *k, int world, s32 *x, s32 *top)
{
    s32 ax, ay;

    if (world)
        return PortRlPresentedSprite(&k->base.sprite, &k->sprites[1], x, &ay, top);
    /* The PPU's 240x160 picture: the sprite's anchor as the game drew it. */
    if (k->base.flags & 0x400)
        return 0;
    ax = k->base.sprite.x * 256;
    ay = k->base.sprite.y * 256;
    *x = ax;
    *top = ay - HEAD_GUESS;
    return ax >= 0 && ax < 240 * 256 && ay >= 0 && ay < 176 * 256;
}

/* out: x0 y0 x1 y1 (the picture's rectangle, fractions of the canvas), then
 * per tag: id, x, y (the top of the Kirby), flags.  Returns the number of
 * tags, 0 when none should show (co-op off, not in play, the menu open). */
TAGS_EXPORT int PortCoopTags(float *out, int max)
{
    const struct Kirby *me = &gKirbys[gLocalPlayerId & 3];
    int dst[4] = { 0, 0, 240, 160 }, fbW = 240, fbH = 160, world = 0, n = 0, slot;
    s32 vw = 240, vh = 160, lx, ltop, head = HEAD_GUESS;
    int haveMe;

    if (out == NULL || max < 1 || !PortCoopReady() || !gPortCoopPuppets || PortUiIsOpen())
        return 0;
    if (PortShowPicture(dst, &fbW, &fbH, &world) && PortShowUsingGpu()) {
        /* The GPU path: only over a world frame (not the pause screens). */
        if (!world || !PortRlShownView(&vw, &vh))
            return 0;
    } else {
        /* The PPU's picture, through the GL presenter or the page's 2D
         * canvas (then the canvas is the picture). */
        world = 0;
        if (me->base.flags & 0x400)
            return 0;
    }
    out[0] = (float)dst[0] / fbW;
    out[1] = (float)dst[1] / fbH;
    out[2] = (float)(dst[0] + dst[2]) / fbW;
    out[3] = (float)(dst[1] + dst[3]) / fbH;
    haveMe = Place(me, world, &lx, &ltop);
    if (haveMe) {
        s32 ly;

        if (world) {
            s32 ax;
            PortRlPresentedSprite(&me->base.sprite, NULL, &ax, &ly, NULL);
        } else {
            ly = me->base.sprite.y * 256;
        }
        if (ly - ltop > 8 * 256 && ly - ltop < 48 * 256)
            head = ly - ltop;
        ltop = ly;                       /* from here on: the local anchor */
    }
    for (slot = 1; slot < PORT_COOP_MAX_SLOTS && n < max; slot++) {
        const struct Kirby *k = &gKirbys[slot];
        s32 x, top;
        int flags;

        if (!PORT_COOP_IS_PUPPET(slot) || PortCoopPuppetAbsent((u32)slot)
            || k->base.roomId != me->base.roomId || (k->base.flags & 0x400))
            continue;
        if (Place(k, world, &x, &top)) {
            flags = TAG_DRAWN;
        } else if (haveMe) {
            /* Off the view: from the local Kirby, by world distance. */
            x = lx + (k->base.x - me->base.x);
            top = ltop + (k->base.y - me->base.y) - head;
            flags = TAG_OFFVIEW;
        } else {
            continue;
        }
        out[4 + n * 4 + 0] = (float)slot;
        out[4 + n * 4 + 1] = (dst[0] + (float)x / 256.0f * dst[2] / vw) / fbW;
        out[4 + n * 4 + 2] = (dst[1] + (float)top / 256.0f * dst[3] / vh) / fbH;
        out[4 + n * 4 + 3] = (float)flags;
        n++;
    }
    return n;
}

#ifdef __cplusplus
}
#endif
