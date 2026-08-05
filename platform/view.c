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
 * codemod in tools/portify.py replaces the literals at (2)-(5) with the
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

#include "port/port.h"
#include "port/dma.h"
#include "gba/gba.h"
#include "main.h"
#include "data.h"
#include "kirby.h"
#include "bg.h"

/* --- the globals the codemod points the game's literals at ----------------
 *
 * Each is initialised to the constant it replaced, so the patched sources
 * behave identically until something here writes to them. */

/* src/sprite_2.c: the box outside which an object is not written to OAM. */
s32 gPortOamMinX = 0, gPortOamMaxX = 240;
s32 gPortOamMinY = 0, gPortOamMaxY = 160;

/* src/kirby.c sub_0803D6B4 and its two clones: half-extents of the box,
 * measured from the centre of the 240x160 window, outside which an object is
 * destroyed.  168 is 120 + 48; 128 is 80 + 48. */
s32 gPortCullHalfW = 168, gPortCullHalfH = 128;

/* src/code_080023A4.c: extra 16-pixel tiles of spawn window on each side. */
s32 gPortSpawnPadX = 0, gPortSpawnPadY = 0;

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
    switch (gPortViewCull) {
    case PORT_CULL_STOCK:
    default:
        gPortOamMinX = 0;   gPortOamMaxX = 240;
        gPortOamMinY = 0;   gPortOamMaxY = 160;
        gPortCullHalfW = 168; gPortCullHalfH = 128;
        gPortSpawnPadX = 0; gPortSpawnPadY = 0;
        break;

    case PORT_CULL_MATCH: {
        /* One source of truth: the view is `viewW` wide, so the object window
         * is the view plus the 48-pixel margin the game already kept. */
        s32 padX = (viewW - PORT_SCREEN_W + 1) / 2;
        s32 padY = (viewH - PORT_SCREEN_H + 1) / 2;

        if (padX < 0) padX = 0;
        if (padY < 0) padY = 0;

        gPortOamMinX = -padX;  gPortOamMaxX = 240 + padX;
        gPortOamMinY = -padY;  gPortOamMaxY = 160 + padY;
        gPortCullHalfW = 168 + padX;
        gPortCullHalfH = 128 + padY;
        /* The spawn window is counted in 16-pixel tiles and the game already
         * carries three of them beyond the screen; round up, so the object
         * exists before its first visible pixel rather than with it. */
        gPortSpawnPadX = (padX + 15) / 16;
        gPortSpawnPadY = (padY + 15) / 16;
        break;
    }

    case PORT_CULL_NONE:
        /* Big enough that no test ever fires.  The despawn box is compared
         * against an abs() of a pixel difference, so this is "never". */
        gPortOamMinX = -4096; gPortOamMaxX = 4096;
        gPortOamMinY = -4096; gPortOamMaxY = 4096;
        gPortCullHalfW = 32767; gPortCullHalfH = 32767;
        /* The spawn window is not made infinite to go with it.  It is a
         * tile-by-tile scan of the whole room's object list, and widening it
         * without limit is what actually exhausts the 64-node pool -- the
         * despawn side alone is the interesting half of the experiment. */
        gPortSpawnPadX = (gPortViewPadX + 15) / 16;
        gPortSpawnPadY = (gPortViewPadY + 15) / 16;
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
    s32 camX = gCurLevelInfo[gUnk_0203AD3C].viewportPosition.x;
    s32 camY = gCurLevelInfo[gUnk_0203AD3C].viewportPosition.y;
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
    s32 kx = (gKirbys[gUnk_0203AD3C].base.base.base.x >> 8)
           - (gCurLevelInfo[gUnk_0203AD3C].viewportPosition.x >> 8);
    s32 ky = (gKirbys[gUnk_0203AD3C].base.base.base.y >> 8)
           - (gCurLevelInfo[gUnk_0203AD3C].viewportPosition.y >> 8);
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
    struct LevelInfo *li = &gCurLevelInfo[gUnk_0203AD3C];
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
