/* The view: how much of the world the page is shown, and what the game is
 * told about it.
 *
 * The GBA's answer to both questions is the same number -- 240x160 -- written
 * into a dozen unrelated places, and the whole of this file exists because
 * they are not actually the same number once the picture stops being 240 wide.
 * There are four separate things that all happen to be 240 on hardware:
 *
 *   1. how many columns the display scans          -> platform/ppu.c
 *   2. how far out an object is still drawn        -> src/sprite_2.c
 *   3. how far out an object is still alive        -> src/kirby.c
 *   4. how far out an object gets created at all   -> src/code_080023A4.c
 *   5. how close to the room's edge the camera may get -> src/code_080023A4.c
 *
 * Widening (1) alone gets you a picture of everything the game believes is
 * off-screen: no enemies, because (4) never made them; no sprites, because (2)
 * rejects them; and enemies that wink out mid-stride, because (3) kills them.
 * So this file derives all five from one description of the view, and the
 * codemod in tools/legacy-sync/portify.py replaces the literals at (2)-(5) with the
 * globals below.  Every one of them defaults to the hardware's number, and
 * PortViewApply on the default mode writes exactly those numbers back, so a
 * build that never leaves PORT_VIEW_NATIVE is the build that shipped.
 *
 * --- what actually works, and what does not -------------------------------
 *
 * docs/VIEW.md has the pictures.  In order of how surprising it was:
 *
 * The culling was never the hard part.  The game already keeps objects alive
 * 48 pixels beyond every edge of the screen, which covers a 336-wide picture
 * on its own, and the four bounds above widen cleanly with no observed
 * failure -- the cost of the widened cull was inside the noise.
 *
 * The hard part was the *tilemap*, and it very nearly ended the experiment.
 * src/bg.c refills a 32x32 screen block with the 32 map columns starting at
 * the camera, so the block is exactly full: 256 pixels for a 240-pixel screen,
 * and BG3HOFS is the low three bits of the scroll, so between 249 and 256 of
 * those pixels are valid and the rest is the room from 32 tiles ago.  There is
 * no spare VRAM to hand it a wider block -- 0x0000..0xC000 is tile data,
 * 0xC000..0x10000 the map blocks, 0x10000 up the sprite sheet.  On hardware
 * that would be the end of it.
 *
 * It is not the end of it here, because the copy has an original.  The room's
 * real tilemap is a plain array in EWRAM and the refill is a straight
 * DmaCopy16 out of it, so the software PPU can read the columns the hardware
 * had no room for.  See gPortBgSource in ppu.c.  That turns the widened view
 * from a repeat of the room into the room.
 *
 * What is left: at a room's own edge there is genuinely nothing beside it, and
 * the picture says so.  And the camera cannot always be kept far enough in for
 * that not to happen, because the clamp it obeys is not always the map's
 * extent.
 *
 * Zooming in was never in doubt: a crop of a correct picture is a correct
 * picture, it costs less than the full frame rather than more, and it is the
 * only mode here that a player would ask for by name.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "port/port.h"
#include "port/dma.h"
#include "port/backend.h"
#include "port/inputlog.h"
#include "port/mp.h"
#include "port/rollback.h"
#include "port/spawn.h"
#include "port/simblock.h"
#include "gba/gba.h"
#include "main.h"
#include "data.h"
#include "kirby.h"
#include "bg.h"
#include "task.h"

/* C linkage for the 64-bit builds.
 *
 * Those compile the game as C++ and tools/legacy-sync/cxxify.py gives it C linkage, so
 * everything on the seam has to agree.  It is applied to platform/*.c as a
 * class rather than to the files that happened to break: the failure is an
 * undefined reference to a mangled name, or -- for a `const`, which is
 * internal-linkage in C++ and external in C -- to a symbol that is plainly
 * defined a few lines away.  Neither says which file to fix, and the set of
 * files that need it changes whenever a declaration moves.  A no-op in C.
 *
 * The block opens below the includes so that SDL and the system headers stay
 * outside it. */
#ifdef __cplusplus
extern "C" {
#endif

/* --- the globals the codemod points the game's literals at ----------------
 *
 * Each is initialised to the constant it replaced, so the patched sources
 * behave identically until something here writes to them. */

/* src/sprite_2.c: the box outside which an object is not written to OAM. */
s32 gPortOamMinX = 0, gPortOamMaxX = 240;
s32 gPortOamMinY = 0, gPortOamMaxY = 160;

/* The spawn and despawn windows (src/code_080023A4.c, src/kirby.c) are not
 * globals any more: they are per player and per side, and come from
 * PortSpawnMarginNow below.  What the --view/--cull experiment contributes to
 * them is these two: a floor on the session view (cull "match" in a wide
 * mode), and cull "none". */
static s32 sLegacyFloorW = PORT_SCREEN_W, sLegacyFloorH = PORT_SCREEN_H;
static int sLegacyNoCull;

/* src/code_080023A4.c sub_08002DA0: extra 8.8 pixels the camera clamp keeps
 * between the view and the room's edge, so a wider view still stops inside
 * the room rather than showing the void beside it. */
s32 gPortCamPadX = 0, gPortCamPadY = 0;

/* --- the description everything else comes from -------------------------- */

u32 gPortViewMode = PORT_VIEW_NATIVE;
u32 gPortViewCull = PORT_CULL_STOCK;

/* Widescreen: extra pixels on each side.  40 makes a 320x160 picture, which
 * is 2:1 -- close enough to a phone in landscape to be the interesting case. */
s32 gPortViewPadX = 40;
s32 gPortViewPadY = 0;

/* Draw a streamed layer only where the game actually streamed it, rather than
 * where the hardware would wrap it round.  See ApplyBgLimits. */
u32 gPortClipToStreamed = 1;

/* ...and, better, read the columns it did not stream out of the room's own
 * tilemap.  This is what makes a wider view show the real room rather than a
 * repeat of it.  Costs nothing when the view is 240 wide, because no pixel
 * ever falls outside the streamed window. */
u32 gPortSynthesiseColumns = 1;

/* Zoom, as a 1.0 = 256 fixed-point factor.  Above 256 is closer in. */
#define ZOOM_ONE 256
s32 gPortZoomMin = 256;      /* never further out than the hardware picture */
s32 gPortZoomMax = 384;      /* 1.5x in: 160x106 of the screen, filling the page */
static s32 sZoom = ZOOM_ONE;
static s32 sZoomTarget = ZOOM_ONE;

/* --- the cull, derived from the view -------------------------------------
 *
 * The argument for widening the cull rather than switching it off is that the
 * game does not only use it to save time.  Objects reset by being destroyed
 * and rebuilt when the camera comes back, and the one-shot bitfield that
 * remembers which chest has been opened is written by a task's *destructor*.
 * Take destruction away and that machinery stops running.
 *
 * There is also a hard reason.  Every spawned object takes a node from
 * LevelInfo.unk1F0.nodes, which is 64 entries, and the allocator is
 *
 *     for (;;) { if (!var0->unk0C) return var0; var0++; }
 *
 * -- no bound, no failure return.  Past the 64th it hands out a pointer into
 * the fields that follow in the same struct: currentRoom, then the room's
 * flag arrays.  The struct sits at a fixed linker address with three more
 * players packed behind it, so it cannot be made bigger either.  Widening the
 * window is bounded by the view; switching the cull off is bounded by the
 * size of the room, and a big room will walk off the end of that array.
 *
 * PORT_CULL_NONE is here anyway, because "we think it would break" is not a
 * finding.  It does break; see docs/VIEW.md. */
static void ApplyCull(s32 viewW, s32 viewH)
{
    /* OAM is drawing only: the box outside which a sprite is not written.
     * The game's object windows are the session view's (PortSpawnMarginNow);
     * this experiment only sets a floor on that view, in PortSetViewMode. */
    switch (gPortViewCull) {
    case PORT_CULL_STOCK:
    default:
        gPortOamMinX = 0;   gPortOamMaxX = 240;
        gPortOamMinY = 0;   gPortOamMaxY = 160;
        break;

    case PORT_CULL_MATCH: {
        s32 padX = (viewW - PORT_SCREEN_W + 1) / 2;
        s32 padY = (viewH - PORT_SCREEN_H + 1) / 2;

        if (padX < 0) padX = 0;
        if (padY < 0) padY = 0;
        gPortOamMinX = -padX;  gPortOamMaxX = 240 + padX;
        gPortOamMinY = -padY;  gPortOamMaxY = 160 + padY;
        break;
    }

    case PORT_CULL_NONE:
        gPortOamMinX = -4096; gPortOamMaxX = 4096;
        gPortOamMinY = -4096; gPortOamMaxY = 4096;
        break;
    }
}

/* --- the dynamic part ------------------------------------------------------
 *
 * "Dynamic" has to react to something, and the candidates were Kirby's speed,
 * whether an enemy is near, and how far he is from the floor.  Speed won, for
 * a reason that only became obvious after trying the others: the zoom is a
 * change to how much of the room you can see, and the only time you *want*
 * more of the room is when you are about to be somewhere else in it.  Enemy
 * proximity zooms out during a fight, which is when the player is looking at
 * a small area very hard and a moving camera is an active nuisance.
 *
 * The signal is the camera's own speed, not Kirby's.  The platform layer does
 * not have to know what Kirby is to see how fast the world is moving past,
 * the camera is already smoothed by the game's own slew limit, and it stops
 * dead at a room edge -- which is exactly where the extra magnification is
 * welcome and where Kirby's own velocity would still be reading "running".
 *
 * Standing still zooms in to gPortZoomMax over about a second; moving at any
 * speed pulls straight back out to 1.0.  Asymmetric on purpose: arriving
 * somewhere and having the camera drift in is pleasant, and being zoomed in
 * one frame after you start running is not. */
static s32 sPrevCamX, sPrevCamY;
static s32 sCamSpeed;           /* smoothed, in 8.8 pixels per frame */
static int sHaveCam;

/* The camera's own speed limit, from sub_08001C40: unk662/unk664 are reset to
 * 0x180 every frame, which is a pixel and a half.  Getting this wrong was the
 * first version's bug -- the threshold was written in whole pixels, Kirby runs
 * faster than the camera is allowed to follow, and so the "moving" case never
 * once fired and the picture sat at full magnification through an entire
 * level.  Everything here is in the camera's own 8.8 units now. */
#define CAM_SLEW_LIMIT 0x180

/* ...and the speed at which the zoom is considered fully out.  A pixel a
 * frame rather than the slew limit itself: measured over a level, a run that
 * is being interrupted by jumps and inhales averages about that, and using
 * the limit meant the picture never quite reached 1:1 while Kirby was
 * plainly running. */
#define CAM_MAX_SPEED 0x100

static void UpdateZoom(void)
{
    s32 camX = gCurLevelInfo[gLocalPlayerId].viewportPosition.x;
    s32 camY = gCurLevelInfo[gLocalPlayerId].viewportPosition.y;
    s32 dx, dy, speed, span;

    if (!sHaveCam) {
        sPrevCamX = camX; sPrevCamY = camY; sHaveCam = 1;
    }
    dx = camX - sPrevCamX; if (dx < 0) dx = -dx;
    dy = camY - sPrevCamY; if (dy < 0) dy = -dy;
    sPrevCamX = camX; sPrevCamY = camY;

    /* A room change teleports the camera; treat the jump as "no information"
     * rather than as an enormous speed, which would slam the zoom out and
     * then crawl back in over the next second of a new room. */
    speed = (dx > dy) ? dx : dy;
    if (speed > CAM_SLEW_LIMIT * 8)
        return;

    /* Smoothed, because a camera that is following a walk cycle stops and
     * starts within it, and the zoom must not breathe in time with Kirby's
     * feet. */
    sCamSpeed += (speed - sCamSpeed) >> 3;
    if (sCamSpeed > CAM_MAX_SPEED) sCamSpeed = CAM_MAX_SPEED;

    /* Continuous rather than a switch: a two-state zoom reads as the picture
     * lurching whenever you touch the D-pad, and the whole point is that it
     * should be something you notice only afterwards. */
    span = gPortZoomMax - gPortZoomMin;
    sZoomTarget = gPortZoomMax - span * sCamSpeed / CAM_MAX_SPEED;

    /* Out fast, in slow.  Arriving somewhere and having the camera drift in
     * is pleasant; being zoomed in one frame after you start running is not.
     * 16 and 2 of 256 per frame is about a fifth of a second against a second
     * and a half. */
    if (sZoomTarget < sZoom) {
        sZoom -= 16;
        if (sZoom < sZoomTarget) sZoom = sZoomTarget;
    } else if (sZoomTarget > sZoom) {
        sZoom += 2;
        if (sZoom > sZoomTarget) sZoom = sZoomTarget;
    }
}

/* Where the crop should sit.
 *
 * Centring it on the screen is wrong at a room's edge: the camera has stopped
 * and Kirby keeps walking, so he can be 120 pixels from the middle of the
 * picture and a crop half that wide loses him entirely.  So the crop follows
 * his position on screen, clamped so it never leaves the 240x160 the display
 * actually produced -- outside that there is nothing to crop. */
static void CropAround(s32 w, s32 h, s32 *outX, s32 *outY)
{
    s32 kx = (gKirbys[gLocalPlayerId].base.x >> 8)
           - (gCurLevelInfo[gLocalPlayerId].viewportPosition.x >> 8);
    s32 ky = (gKirbys[gLocalPlayerId].base.y >> 8)
           - (gCurLevelInfo[gLocalPlayerId].viewportPosition.y >> 8);
    s32 x, y;

    /* Before a level is loaded these are zero, which would peg the crop to
     * the top-left corner of the title screen.  Centre it instead. */
    if (kx <= 0 || kx >= PORT_SCREEN_W || ky <= 0 || ky >= PORT_SCREEN_H) {
        kx = PORT_SCREEN_W / 2;
        ky = PORT_SCREEN_H / 2;
    }

    x = kx - w / 2;
    y = ky - h / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > PORT_SCREEN_W) x = PORT_SCREEN_W - w;
    if (y + h > PORT_SCREEN_H) y = PORT_SCREEN_H - h;
    *outX = x;
    *outY = y;
}

/* --- how far each layer actually reaches ---------------------------------
 *
 * This is the finding that decides whether widescreen is worth anything, and
 * it is not the culling.
 *
 * src/bg.c's sub_08153184 refills a background's screen block whenever the
 * camera crosses a tile boundary, and what it writes is `unk26 + 1` map
 * columns starting at the camera's own column, by `unk28 + 1` rows.  For every
 * gameplay layer that is 32 by 22 -- exactly the 32x32 screen block, exactly
 * full.  The scroll register is then set to `scrollX & 7`, so the first
 * written column lands at screen x -(scrollX & 7) and the last valid pixel is
 * at 256 - (scrollX & 7) - 1: somewhere between 248 and 255 depending on where
 * in the tile the camera is.  Past that the hardware wraps to column 0, which
 * holds the map column 32 tiles behind.  There is no spare VRAM to hand the
 * layer a 64-wide block -- 0x0000-0xC000 is tile data, 0xC000-0x10000 is the
 * four map blocks in use, and 0x10000 up is the sprite sheet.
 *
 * So the honest margin on the room layer is between 8 and 16 pixels on the
 * right and none at all on the left, and a 320-wide picture is asking for
 * eighty pixels of world the game never wrote down.
 *
 * A layer whose whole map is 32x32 or smaller is exempt: sub_08153184 copies
 * it once and clears bit 0x20 of unk2E, the game writes the full scroll value
 * to the register instead of the low three bits, and the hardware's wrap is
 * then the map genuinely tiling.  Those are the parallax skies, and they are
 * why the widened picture looks as good as it does -- everything that extends
 * cleanly into the margin is a layer that was never streamed.
 */
static void ClearBgLimits(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        gPortBgValidL[i] = gPortBgValidT[i] = -0x40000000;
        gPortBgValidR[i] = gPortBgValidB[i] =  0x40000000;
        gPortBgSource[i].map = NULL;
    }
}

/* Is this source map safe to read every pixel of?
 *
 * It has to be asked, and asking it cost a crash to learn.  `unkC0[]` is live
 * game state that outlives the level it describes: on a menu, or between
 * rooms, `unk10` still holds whatever the last room left there while `unk14`
 * and `unk16` have already been replaced, and the product of the two indexes
 * a long way past the end of anything.  The renderer reads this array a
 * hundred and sixty times a frame with no bounds check of its own -- putting
 * one in the inner loop would tax the normal view for the sake of the
 * experiment -- so the whole map is proved to lie inside one region here,
 * once, and the loop can then trust it.
 *
 * EWRAM for the room foreground, which is decompressed there at load; ROM for
 * the two parallax layers, which are read in place.  Anything else -- a map
 * inside VRAM, which one late-game room really does use -- is declined. */
static int SourceMapUsable(const u16 *map, s32 w, s32 h)
{
    uintptr_t a = (uintptr_t)map;
    uintptr_t bytes;

    if (!map || w <= 0 || h <= 0 || w > 0x1000 || h > 0x1000)
        return 0;
    bytes = (uintptr_t)w * (uintptr_t)h * 2;
    if (a >= GBA_EWRAM_BASE && a + bytes <= GBA_EWRAM_BASE + GBA_EWRAM_SIZE)
        return 1;
    if (a >= GBA_ROM_BASE && a + bytes <= GBA_ROM_BASE + gPortRomSize)
        return 1;
    return 0;
}

static void ApplyBgLimits(void)
{
    struct LevelInfo *li = &gCurLevelInfo[gLocalPlayerId];
    int i;

    ClearBgLimits();
    if (!gPortClipToStreamed)
        return;
    /* No room, no layers worth describing -- and `unkC0[]` still full of the
     * last one's. */
    if (li->currentRoom == 0xFFFF)
        return;

    for (i = 0; i < 3; i++) {
        const struct Background *bg = &li->unkC0[i];
        int hw = bg->unk2E & 3;
        s32 offX, offY;

        /* Bit 0x20 is the game's own "this layer is streamed" flag.  Without
         * it the map tiles for real and there is nothing to clip. */
        if (!(bg->unk2E & 0x20))
            continue;
        offX = *(vu16 *)(GBA_IO_BASE + REG_OFFSET_BG0HOFS + hw * 4) & 7;
        offY = *(vu16 *)(GBA_IO_BASE + REG_OFFSET_BG0VOFS + hw * 4) & 7;
        gPortBgValidL[hw] = -offX;
        gPortBgValidR[hw] = (bg->unk26 + 1) * 8 - offX;
        gPortBgValidT[hw] = -offY;
        gPortBgValidB[hw] = (bg->unk28 + 1) * 8 - offY;

        /* And where the columns beyond that window came from.  Only offered
         * for the plain case: 16-bit entries, no mirroring, and a source map
         * that is not itself inside VRAM.  The mirrored variants exist in
         * bg.c and index their source differently -- they ignore the scroll
         * displacement entirely -- and room 918 puts its map at 0x0600D800
         * and switches the layer to 8-bit affine.  Neither is reachable on a
         * normal gameplay layer, and neither is worth reproducing on the
         * chance that it becomes so. */
        if (gPortSynthesiseColumns
            && !(bg->unk2E & (0x40 | 0x80 | 0x100))
            && SourceMapUsable(bg->unk10, bg->unk14, bg->unk16)) {
            gPortBgSource[hw].map = bg->unk10;
            gPortBgSource[hw].widthTiles = bg->unk14;
            gPortBgSource[hw].heightTiles = bg->unk16;
            gPortBgSource[hw].scrollX = bg->scrollX;
            gPortBgSource[hw].scrollY = bg->scrollY;
            gPortBgSource[hw].offX = bg->unk1E;
            gPortBgSource[hw].offY = bg->unk20;
        }
    }
}

/* Called once a frame, from PortPresentFrame, before the PPU runs. */
void PortViewUpdate(void)
{
    s32 x = 0, y = 0, w = PORT_SCREEN_W, h = PORT_SCREEN_H;

    switch (gPortViewMode) {
    case PORT_VIEW_NATIVE:
    default:
        break;

    case PORT_VIEW_WIDE:
        x = -gPortViewPadX;
        y = -gPortViewPadY;
        w = PORT_SCREEN_W + gPortViewPadX * 2;
        h = PORT_SCREEN_H + gPortViewPadY * 2;
        break;

    case PORT_VIEW_ZOOM:
        UpdateZoom();
        w = PORT_SCREEN_W * ZOOM_ONE / sZoom;
        h = PORT_SCREEN_H * ZOOM_ONE / sZoom;
        /* Even widths only.  An odd crop changes width by one pixel as the
         * zoom creeps, and the page re-fits the canvas every time it does --
         * which reads as a picture that will not stop shivering. */
        w &= ~1; h &= ~1;
        if (w > PORT_SCREEN_W) w = PORT_SCREEN_W;
        if (h > PORT_SCREEN_H) h = PORT_SCREEN_H;
        CropAround(w, h, &x, &y);
        break;

    case PORT_VIEW_WIDE_ZOOM:
        UpdateZoom();
        w = (PORT_SCREEN_W + gPortViewPadX * 2) * ZOOM_ONE / sZoom;
        h = (PORT_SCREEN_H + gPortViewPadY * 2) * ZOOM_ONE / sZoom;
        w &= ~1; h &= ~1;
        x = -gPortViewPadX + (PORT_SCREEN_W + gPortViewPadX * 2 - w) / 2;
        y = -gPortViewPadY + (PORT_SCREEN_H + gPortViewPadY * 2 - h) / 2;
        break;
    }

    PortSetView(x, y, w, h);
    ApplyCull(w, h);

    /* The camera clamp only needs padding when the view is genuinely wider
     * than the screen; a crop is inside it and wants the stock behaviour. */
    gPortCamPadX = (w > PORT_SCREEN_W) ? (w - PORT_SCREEN_W) / 2 * 256 : 0;
    gPortCamPadY = (h > PORT_SCREEN_H) ? (h - PORT_SCREEN_H) / 2 * 256 : 0;

    /* The HUD is BG1 during gameplay: screen block 28, priority 0, both
     * scroll registers held at zero for the whole level (sub_080338B4 in
     * src/code_08032E98.c).  Menus put other things on other blocks and
     * scroll them, so the pin is only claimed when the picture is not 240
     * wide -- at which point the only thing that can be hurt is a menu
     * someone opened in widescreen, which is already not a picture the
     * hardware could produce. */
    PortSetScreenSpaceBgs(w != PORT_SCREEN_W || h != PORT_SCREEN_H ? (1u << 1) : 0,
                          gPortPinScreenSpace);

    if (w > PORT_SCREEN_W || h > PORT_SCREEN_H)
        ApplyBgLimits();
    else
        ClearBgLimits();
}

/* The page's single entry point.  Everything it can change is here, so a
 * setting can never be half-applied. */
void PortSetViewMode(u32 mode, s32 padX, s32 padY, u32 cull, u32 pinHud,
                     u32 tilemap)
{
    /* One control, three settings, because they are three points on one
     * scale of honesty and offering them separately invites the combination
     * that makes no sense (synthesise, but also draw the wrap).
     *   0 -- draw the hardware wrap, which is what a naive widening shows
     *   1 -- draw nothing outside the streamed window
     *   2 -- read the room's own tilemap for what is outside it */
    gPortClipToStreamed = (tilemap >= 1);
    gPortSynthesiseColumns = (tilemap >= 2);
    if (mode > PORT_VIEW_WIDE_ZOOM) mode = PORT_VIEW_NATIVE;
    if (padX < 0) padX = 0;
    if (padY < 0) padY = 0;
    /* The OAM X field is nine bits and Y is eight, so a sprite can only say
     * where it is within -256..255 and -128..127.  Past that the game cannot
     * express the position however wide the picture gets. */
    if (padX > 96) padX = 96;
    if (padY > 48) padY = 48;

    gPortViewMode = mode;
    gPortViewPadX = padX;
    gPortViewPadY = padY;
    gPortViewCull = cull > PORT_CULL_NONE ? PORT_CULL_STOCK : cull;
    gPortPinScreenSpace = pinHud;

    /* What the experiment asks of the game's object windows.  Set here, from
     * the settings alone, rather than from the per-frame view: the windows
     * are simulation and must not follow the zoom's easing (the widest the
     * picture gets in a wide mode is 240+2*padX, zoom only crops it). */
    sLegacyFloorW = PORT_SCREEN_W;
    sLegacyFloorH = PORT_SCREEN_H;
    sLegacyNoCull = 0;
    if (mode == PORT_VIEW_WIDE || mode == PORT_VIEW_WIDE_ZOOM) {
        if (gPortViewCull != PORT_CULL_STOCK) {
            sLegacyFloorW = PORT_SCREEN_W + padX * 2;
            sLegacyFloorH = PORT_SCREEN_H + padY * 2;
        }
        sLegacyNoCull = gPortViewCull == PORT_CULL_NONE;
    }

    if (mode == PORT_VIEW_NATIVE) {
        /* Return the game to the hardware's numbers immediately rather than
         * waiting for the next frame's PortViewUpdate: the toggle is meant to
         * be a fair comparison, and a frame of half-reverted cull bounds in
         * the middle of it is not one. */
        sZoom = sZoomTarget = ZOOM_ONE;
        gPortViewCull = PORT_CULL_STOCK;
        PortSetView(0, 0, PORT_SCREEN_W, PORT_SCREEN_H);
        ApplyCull(PORT_SCREEN_W, PORT_SCREEN_H);
        gPortCamPadX = gPortCamPadY = 0;
        PortSetScreenSpaceBgs(0, pinHud);
        ClearBgLimits();
    }
}

/* --- the simulation's view: spawn and despawn windows -----------------------
 *
 * The game creates an object when its spawn point comes within a window
 * about 48 pixels (three 16-pixel object cells) past the 240x160 screen and
 * destroys it 48 pixels past the screen's edge.  A renderer that shows more
 * of the world than 240x160 (the GPU path zoomed out, or wider than 3:2) sees
 * past both, and objects visibly pop in and out at the edges of the picture.
 *
 * So the windows are sized for the view the player is shown, and that makes
 * them simulation.  Everything below follows from that:
 *
 *  - The size is a *recorded input*, not something read off the window.  The
 *    host asks for the widest world view it may show (PortSimViewRequest);
 *    the request becomes a frame-stamped `view FRAME W H` event in the input
 *    log, and the latch applies the event list's value for its frame
 *    (PortSimViewLatch).  A replay -- native or wasm -- reproduces every change
 *    at its frame, whatever window it runs in.  The value at a frame is a pure
 *    lookup, so a rollback that re-latches a frame gets the same answer.
 *  - Requests are integers rounded *up* to whole 16-pixel cells a side (32 in
 *    each dimension), clamped to the render list's 1024x512 first (so at
 *    most 1040x512).  The float rounding the host does to turn a window into
 *    a view only decides what it asks for; nothing window-dependent reaches
 *    the game except through the log.
 *  - Where the extra margin goes is computed from game state alone, with the
 *    placement rule platform/render/rl_build.c uses for the GPU view: centred
 *    on the camera, kept inside the camera rectangle (+8 px left, right and
 *    top, none at the bottom) or the map when the rectangle is narrower than
 *    the screen, never so far that the 240x160 the game composed leaves the
 *    view.  At a room's edge the view is pushed inwards, so the margin is
 *    asymmetric: little on the side of the wall, up to W-240 on the other.
 *    And behind a scroll lock the picture stops at the lock, so the margin
 *    does too -- nothing past a boss arena's wall is woken up.
 *  - Never smaller than the original on any side.  At 240x160 (or anything
 *    narrower: a zoom in) every number is the stock one and the patched code
 *    is the code that shipped.
 *  - Netplay: every peer must use the same value.  While the game's link
 *    (platform/mp.c) or a rollback session (platform/rollback.c) is active,
 *    or a netplay page has pinned it (PortSimViewPinNative), the view is held
 *    at 240x160 -- a common value -- and requests are dropped.  The seam for
 *    netplay v2 is the event list: a session would carry each peer's request
 *    as a timeline event and feed the max of the peers' latest requests into
 *    PortInputLogViewAt's place (docs/design/NETPLAY-V2.md).
 */
#define SIM_VIEW_MAX_W 1024    /* the render list's own ceiling */
#define SIM_VIEW_MAX_H 512
#define CELL 16                 /* the object list's grid */

/* Everything the latch decides, in one place.  Recomputed at every latch
 * from the event list and the frame number, so it needs no snapshot of its
 * own; the netsim work's sim block (platform/port/simblock.h) can hold it
 * as-is if it moves there. */
/* It lives in the port sim block (port/simblock.h, struct PortSimView): the
 * game reads it every frame, and between latches -- a snapshot taken at the
 * frame boundary and restored elsewhere -- it is simulation state like any
 * other.  PortSimBlockInit (platform/main.c) sets the power-on value,
 * 240x160 and stock. */
#define sSim (gPortSim->view)
static int sSimPinned;
static s32 sReqW = PORT_SCREEN_W, sReqH = PORT_SCREEN_H;

/* Diagnostics only (KATAM_SPAWNSTAT=1): never read by the game. */
static u32 sStatChanges, sStatSkips, sStatMaxNodes, sStatMaxTasks;
static s32 sStatMaxPad[4];

static s32 RoundView(s32 v, s32 screen, s32 max)
{
    if (v <= screen)
        return screen;
    if (v > max)
        v = max;
    return screen + ((v - screen + 2 * CELL - 1) / (2 * CELL)) * (2 * CELL);
}

static int SimLocked(void)
{
    return sSimPinned || PortRbActive() || PortMpCurrent() != NULL;
}

void PortSimViewPinNative(void)
{
    sSimPinned = 1;
}

int PortSimViewLocked(void)
{
    return SimLocked();
}

void PortSimViewRequest(s32 w, s32 h)
{
    w = RoundView(w, PORT_SCREEN_W, SIM_VIEW_MAX_W);
    h = RoundView(h, PORT_SCREEN_H, SIM_VIEW_MAX_H);
    if (SimLocked() || PortInputLogReplaying())
        return;
    if (w == sReqW && h == sReqH)
        return;
    sReqW = w;
    sReqH = h;
    PortInputLogScheduleView(PortFrameNumber() + 1, w, h);
}

int PortSimViewCovers(s32 w, s32 h)
{
    if (SimLocked())
        return w <= PORT_SCREEN_W && h <= PORT_SCREEN_H;
    return w <= SIM_VIEW_MAX_W && h <= SIM_VIEW_MAX_H;
}

void PortSimViewGet(s32 *w, s32 *h)
{
    *w = sSim.viewW;
    *h = sSim.viewH;
}

static void SimViewAt(u32 frame, s32 *w, s32 *h)
{
    PortInputLogViewAt(frame, w, h);
    *w = RoundView(*w, PORT_SCREEN_W, SIM_VIEW_MAX_W);
    *h = RoundView(*h, PORT_SCREEN_H, SIM_VIEW_MAX_H);
    if (SimLocked()) {
        *w = PORT_SCREEN_W;
        *h = PORT_SCREEN_H;
    }
    if (*w < sLegacyFloorW) *w = sLegacyFloorW;
    if (*h < sLegacyFloorH) *h = sLegacyFloorH;
}

void PortSimViewLatch(u32 frame)
{
    s32 w, h;

    SimViewAt(frame, &w, &h);
    if (frame > 0)
        SimViewAt(frame - 1, &sSim.prevW, &sSim.prevH);
    else
        sSim.prevW = w, sSim.prevH = h;
    if (w != sSim.viewW || h != sSim.viewH)
        sStatChanges++;
    sSim.viewW = w;
    sSim.viewH = h;
    sSim.stock = w == PORT_SCREEN_W && h == PORT_SCREEN_H
             && sSim.prevW == PORT_SCREEN_W && sSim.prevH == PORT_SCREEN_H
             && !sLegacyNoCull;
}

int PortSpawnStock(void)
{
    return sSim.stock;
}

static s32 ClampS(s32 v, s32 lo, s32 hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* How far past the 240x160 screen, on each side of one axis, the view
 * reaches -- rl_build.c's placement, in integers.  `cam` is the world
 * position of screen pixel 0, [rectLo, rectHi) the camera rectangle, `map`
 * the room's extent. */
static void Reach(s32 view, s32 screen, s32 cam, s32 rectLo, s32 rectHi,
                  s32 padLo, s32 padHi, s32 map, s32 *lo, s32 *hi)
{
    s32 rl, rr, v;

    if (view <= screen) {
        *lo = *hi = 0;
        return;
    }
    rl = rectLo - padLo - cam;
    rr = rectHi + padHi - cam;
    if (rr - rl < screen || rl > 0 || rr < screen) {
        rl = -cam;
        rr = map - cam;
    }
    if (rl < -cam) rl = -cam;
    if (rr > map - cam) rr = map - cam;
    v = -(view - screen) / 2;
    v = (rr - rl >= view) ? ClampS(v, rl, rr - view) : (rl + rr - view) / 2;
    v = ClampS(v, screen - view, 0);
    *lo = -v;
    *hi = v + view - screen;
}

static s32 Cells(s32 px)
{
    return (px + CELL - 1) / CELL;
}

static void Margin(u8 player, s32 camX, s32 camY, s32 w, s32 h,
                   struct PortSpawnMargin *m)
{
    const struct LevelInfo *li = &gCurLevelInfo[player & 3];
    s32 lo, hi;

    m->padL = m->padR = m->padT = m->padB = 0;
    if (w > PORT_SCREEN_W) {
        Reach(w, PORT_SCREEN_W, camX, li->unk_S32Vec2_6C.x >> 8,
              li->unk_S32Vec2_74.x >> 8, 8, 8, li->unkC0[1].unk14 * 8, &lo, &hi);
        m->padL = Cells(lo);
        m->padR = Cells(hi);
    }
    if (h > PORT_SCREEN_H) {
        Reach(h, PORT_SCREEN_H, camY, li->unk_S32Vec2_6C.y >> 8,
              li->unk_S32Vec2_74.y >> 8, 8, 0, li->unkC0[1].unk16 * 8, &lo, &hi);
        m->padT = Cells(lo);
        m->padB = Cells(hi);
    }
    m->cullL = 168 + m->padL * CELL;
    m->cullR = 168 + m->padR * CELL;
    m->cullT = 128 + m->padT * CELL;
    m->cullB = 128 + m->padB * CELL;
    if (sLegacyNoCull)
        m->cullL = m->cullR = m->cullT = m->cullB = 32767;
}

static void Stock(struct PortSpawnMargin *m)
{
    m->padL = m->padR = m->padT = m->padB = 0;
    m->cullL = m->cullR = 168;
    m->cullT = m->cullB = 128;
}

void PortSpawnMarginNow(u8 player, struct PortSpawnMargin *m)
{
    const struct LevelInfo *li = &gCurLevelInfo[player & 3];

    if (sSim.stock) {
        Stock(m);
        return;
    }
    Margin(player, li->viewportPosition.x >> 8, li->viewportPosition.y >> 8,
           sSim.viewW, sSim.viewH, m);
    if (m->padL > sStatMaxPad[0]) sStatMaxPad[0] = m->padL;
    if (m->padR > sStatMaxPad[1]) sStatMaxPad[1] = m->padR;
    if (m->padT > sStatMaxPad[2]) sStatMaxPad[2] = m->padT;
    if (m->padB > sStatMaxPad[3]) sStatMaxPad[3] = m->padB;
}

void PortSpawnMarginPrev(u8 player, struct PortSpawnMargin *m)
{
    const struct LevelInfo *li = &gCurLevelInfo[player & 3];

    if (sSim.stock) {
        Stock(m);
        return;
    }
    Margin(player, li->viewportPositionUnmodified.x >> 8,
           li->viewportPositionUnmodified.y >> 8, sSim.prevW, sSim.prevH, m);
}

/* The reserve a widened window keeps.  The object-node pool is 64 with an
 * allocator that walks off its end when full (see ApplyCull's history in
 * docs/VIEW.md); the task pool is 128 and hands out a shared dummy when full.
 * Scripted spawns (a Boxy's present, a Prank's item) and every projectile and
 * effect draw on the same pools, so the room walks leave them room. */
#define NODE_RESERVE 8
#define TASK_RESERVE 24

/* The diagnostics' idea of "visible": the rectangle a view of
 * KATAM_SPAWNSTAT=WxH (default: the session view) covers around player's
 * camera, placed as the renderer places it.  0 off it, 1 on the 240x160
 * screen, 2 in the view past the screen. */
static int sStatOn = -1;
static s32 sStatW, sStatH;
static u32 sStatSpawn[2][3], sStatDespawn[3];

static int StatOn(void)
{
    if (sStatOn < 0) {
        const char *e = getenv("KATAM_SPAWNSTAT");
        int w = 0, h = 0;

        sStatOn = e != NULL && *e != '\0' && *e != '0';
        if (sStatOn && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            sStatW = w;
            sStatH = h;
        }
    }
    return sStatOn;
}

static int StatWhere(u8 player, s32 x, s32 y)
{
    const struct LevelInfo *li = &gCurLevelInfo[player & 3];
    s32 camX = li->viewportPosition.x >> 8, camY = li->viewportPosition.y >> 8;
    s32 w = sStatW ? sStatW : sSim.viewW, h = sStatH ? sStatH : sSim.viewH;
    s32 l = 0, r = 0, t = 0, b = 0;

    x -= camX;
    y -= camY;
    if (x >= 0 && x < PORT_SCREEN_W && y >= 0 && y < PORT_SCREEN_H)
        return 1;
    if (w > PORT_SCREEN_W)
        Reach(w, PORT_SCREEN_W, camX, li->unk_S32Vec2_6C.x >> 8,
              li->unk_S32Vec2_74.x >> 8, 8, 8, li->unkC0[1].unk14 * 8, &l, &r);
    if (h > PORT_SCREEN_H)
        Reach(h, PORT_SCREEN_H, camY, li->unk_S32Vec2_6C.y >> 8,
              li->unk_S32Vec2_74.y >> 8, 8, 0, li->unkC0[1].unk16 * 8, &t, &b);
    return (x >= -l && x < PORT_SCREEN_W + r && y >= -t && y < PORT_SCREEN_H + b) ? 2 : 0;
}

void PortSpawnNoteDespawn(u8 player, s32 x, s32 y)
{
    if (StatOn() && player < 4)
        sStatDespawn[StatWhere(player, x, y)]++;
}

int PortSpawnHeadroom(u8 player, s32 x, s32 y, int edge)
{
    const struct LevelInfo *li = &gCurLevelInfo[player & 3];
    u32 used = 0, i;

    if (StatOn())
        sStatSpawn[edge != 0][StatWhere(player, x, y)]++;

    for (i = 0; i < 0x40; i++)
        used += li->unk1F0.nodes[i].unk0C != 0;
    if (used > sStatMaxNodes) sStatMaxNodes = used;
    if ((u32)gNumTasks > sStatMaxTasks) sStatMaxTasks = (u32)gNumTasks;
    if (sSim.stock)
        return 1;
    if (used + NODE_RESERVE >= 0x40 || gNumTasks + TASK_RESERVE >= MAX_TASK_NUM) {
        sStatSkips++;
        return 0;
    }
    return 1;
}

void PortSimViewReport(void)
{
    const char *e = getenv("KATAM_SPAWNSTAT");

    if (e == NULL || *e == '\0' || *e == '0')
        return;
    PortLog("[katam-port] spawn margin: view now %dx%d, %u changes; widest pad "
            "(cells) L%d R%d T%d B%d; peak %u/64 object nodes, %u/%d tasks; "
            "%u spawns deferred for headroom", (int)sSim.viewW, (int)sSim.viewH,
            (unsigned)sStatChanges, (int)sStatMaxPad[0], (int)sStatMaxPad[1],
            (int)sStatMaxPad[2], (int)sStatMaxPad[3], (unsigned)sStatMaxNodes,
            (unsigned)sStatMaxTasks, MAX_TASK_NUM, (unsigned)sStatSkips);
    PortLog("[katam-port] spawn margin: judged against a %dx%d view -- "
            "scroll-in spawns %u in view past 240x160, %u on screen, %u unseen; "
            "despawns %u in view past 240x160, %u on screen, %u unseen; "
            "whole-window walks (room entry, camera jumps) %u/%u/%u",
            (int)(sStatW ? sStatW : sSim.viewW), (int)(sStatH ? sStatH : sSim.viewH),
            (unsigned)sStatSpawn[1][2], (unsigned)sStatSpawn[1][1],
            (unsigned)sStatSpawn[1][0], (unsigned)sStatDespawn[2],
            (unsigned)sStatDespawn[1], (unsigned)sStatDespawn[0],
            (unsigned)sStatSpawn[0][2], (unsigned)sStatSpawn[0][1],
            (unsigned)sStatSpawn[0][0]);
}

#ifdef __cplusplus
}
#endif
