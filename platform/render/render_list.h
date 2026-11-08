/* The render list: what the GPU renderer draws, built from game state.
 *
 * This is the durable half of the renderer.  The GL backend in gl_renderer.c
 * consumes it and is replaceable; the list is the contract between the game
 * and whatever draws it.  docs/RENDERER.md has the design and the numbers.
 *
 * Three coordinate spaces, and every item says which one it is in:
 *
 *   SCREEN  GBA screen coordinates, 0..239 x 0..159 on hardware.  What the
 *           emulated PPU would have drawn there.  Menus, title screens and
 *           anything the list cannot prove is world-anchored.
 *   WORLD   GBA screen coordinates relative to the game's own camera, but
 *           *unbounded*: a world layer has real content for any x, and a
 *           world sprite carries its full-precision s32 position from before
 *           the 9-bit OAM truncation and the game's on-screen cull.  The view
 *           may be any size and may be moved off the game camera (clamping at
 *           a room edge), and world items follow it.
 *   HUD     screen space, but each item is anchored to an edge or corner of
 *           the view rather than to a 240x160 rectangle.
 *
 * What is still GBA-shaped, on purpose, for this cut: tiles and palettes are
 * referenced by VRAM address and palette RAM index (the backend keeps a texture
 * cache of both, uploaded when they change), and layer composition follows the
 * GBA rules (four priorities, two blend targets, windows).  MODDING.md M9
 * replaces the VRAM references with resource ids -- RlTileLayer.map and
 * RlSprite.tile are the fields that change; the spaces, the anchoring, the
 * effect description and the sprite order do not.
 *
 * Nothing in this file, and nothing that builds a list, writes game memory.
 * The game-side hooks (PortRl*) are write-only from the simulation's point of
 * view: they copy game state into host memory and return.  `make regress`
 * with the renderer on is the check.
 */
#ifndef GUARD_PORT_RENDER_LIST_H
#define GUARD_PORT_RENDER_LIST_H

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RL_SPACE_SCREEN 0
#define RL_SPACE_WORLD  1
#define RL_SPACE_HUD    2

#define RL_MAX_SPRITES  1024
#define RL_MAX_HUD_REGIONS 8
#define RL_LINES 160            /* per-line effect state, one row per GBA line */
#define RL_LINE_WORDS 40        /* see RL_LINE_* below; 10 RGBA texels */

/* A background layer.  The text/affine distinction and the tile fetch are the
 * GBA's (BGxCNT in the per-line table); what the list adds is where the
 * layer's content comes from outside the 32x32 hardware screen block, and how
 * it moves when the view moves. */
typedef struct RlTileLayer {
    u8 enabled;
    u8 space;                /* RL_SPACE_*                                   */
    u8 streamed;             /* WORLD: VRAM holds only a window of the map   */
    u8 wrapX;                /* outside the window: 1 repeat, 2 mirror      */
    u8 clampY;               /* ... and its edge rows extend up and down    */
    u8 pad_[3];
    /* Added to the screen coordinate of every pixel before sampling: the
     * parallax term for a view moved off the game camera,
     * (factor - 1) * viewShift.  Zero for the room layer and for SCREEN. */
    s32 shiftX, shiftY;
    /* WORLD + streamed: the room's real tilemap (row-major u16 entries,
     * exactly what bg.c DMAs into the screen block), where it sits relative
     * to the screen, and the screen-space window inside which VRAM holds the
     * game's own copy (which wins: it is what the game last wrote, including
     * any tile it changed in place). */
    const u16 *map;
    s32 mapW, mapH;          /* tiles                                       */
    s32 mapScrollX, mapScrollY;  /* Background.scrollX/Y, pixels             */
    s32 mapOffX, mapOffY;        /* Background.unk1E/unk20, tiles            */
    s32 validL, validR, validT, validB;  /* screen coords                   */
    u32 mapGen;              /* bumps when map/mapW/mapH change               */
    /* Parallax factor against the game camera, 8.8 (256 = moves with the
     * world).  Informational in this cut -- shiftX/Y already apply it. */
    s32 factorX, factorY;
} RlTileLayer;

/* One hardware-sized sprite (a GBA OBJ), front to back in the list. */
typedef struct RlSprite {
    s32 x, y;                /* top-left of the bounding box, in `space`     */
    u16 attr0, attr1, attr2; /* OBJ attributes with the position fields zero */
    u8 space;
    u8 anchor;               /* HUD: index into RlFrame.hud                  */
    s16 pa, pb, pc, pd;      /* affine matrix (identity when not affine)    */
} RlSprite;

/* A HUD region: a rectangle of the 240x160 HUD and the edge it is pinned to.
 * Every HUD pixel and HUD sprite belongs to the first region containing it. */
#define RL_ANCHOR_LEFT   0
#define RL_ANCHOR_RIGHT  1
#define RL_ANCHOR_CENTER 2
#define RL_ANCHOR_TOP    0
#define RL_ANCHOR_BOTTOM 1
typedef struct RlHudRegion {
    s16 x0, y0, x1, y1;      /* in the 240x160 HUD                           */
    u8 anchorH, anchorV;
    s16 dx, dy;              /* resolved: view coord = HUD coord + (dx, dy)  */
} RlHudRegion;

/* Per-line register state, captured as the emulated display scans each line
 * (so HBlank effects are in it).  Word offsets: */
#define RL_LINE_BGOFS    0   /* 8 words: hofs0 vofs0 hofs1 vofs1 ... (u16)  */
#define RL_LINE_AFF2     8   /* refX refY pa pc for BG2                      */
#define RL_LINE_AFF3    12   /* ... and BG3                                  */
#define RL_LINE_DISPCNT 16
#define RL_LINE_BLDCNT  17
#define RL_LINE_BLDALPHA 18
#define RL_LINE_BLDY    19
#define RL_LINE_WIN0H   20
#define RL_LINE_WIN1H   21
#define RL_LINE_WIN0V   22
#define RL_LINE_WIN1V   23
#define RL_LINE_WININ   24
#define RL_LINE_WINOUT  25
#define RL_LINE_MOSAIC  26
#define RL_LINE_BGCNT   27   /* 4 words */
#define RL_LINE_AFFB    32   /* pb2 pd2 pb3 pd3 */

typedef struct RlFrame {
    u32 serial;              /* bumps every built frame                      */
    u8 world;                /* 1: gameplay framing (view != 240x160 allowed) */
    u8 objOneD;
    /* The view: its size in game pixels, and where its top-left sits in
     * SCREEN coordinates.  At 240x160 on the game camera this is (0,0). */
    s32 viewW, viewH;
    s32 viewX, viewY;
    /* The camera shift the list applied (view centre minus game centre),
     * after clamping to the room; world items move by -shift. */
    s32 shiftX, shiftY;
    /* Outside this rectangle (SCREEN coords) the view has no content: the
     * backend fills it with black. */
    s32 contentL, contentT, contentR, contentB;

    RlTileLayer layers[4];   /* by hardware BG number                        */
    u8 hudBg;                /* 0xFF: none                                   */
    u8 numHud;
    RlHudRegion hud[RL_MAX_HUD_REGIONS];

    const RlSprite *sprites; /* front (drawn on top) to back                 */
    u32 numSprites;
    u32 numObjWin;           /* sprites[numSprites .. +numObjWin): OBJ window */

    /* Texture-cache sources.  The backend uploads what changed. */
    const u8 *vram;          /* 96 KiB, with off-screen sprite tiles patched  */
    const u16 *palette;      /* 512 entries, BGR555                           */
    s32 lines[RL_LINES][RL_LINE_WORDS];

    /* Diagnostics: where the list came from. */
    u32 spritesFromHooks, spritesFromOam, spritesOffscreen, spritesOverflow;
    u8 hookSpritesUsed;      /* 0: OAM did not match the hooks this frame    */
} RlFrame;

/* --- the renderer's switches (platform/render/rl_build.c) --------------- */

extern int gPortRlEnabled;     /* hooks collect only when set                */

/* The view the player asked for: 0 means 240x160.  `autoAspect` picks the
 * width from the window at present time. */
void PortRlSetViewSize(s32 w, s32 h);
void PortRlGetViewSize(s32 *w, s32 *h);

/* Called from the PPU scanline loop for each line before it is drawn. */
void PortRlCaptureLine(int line, s32 aff2x, s32 aff2y, s32 aff3x, s32 aff3y);

/* Build the frame to present from the current console state and the game
 * state committed at the last UpdateScreen.  Never writes game memory. */
const RlFrame *PortRlBuildFrame(void);
/* KATAM_RL_POPSTAT=1: objects appearing/vanishing in the widened margin. */
void PortRlReportPopIn(void);
/* The last frame built, or NULL before the first. */
const RlFrame *PortRlCurrentFrame(void);

/* --- game-side hooks: write-only, see the header comment ----------------
 * Attached by linker wraps of DisplaySprite, sub_081564D8 and
 * DrawToOamBuffer (rl_build.c); game/ itself is unmodified. */

struct Sprite;
/* At the top of DisplaySprite (variant 0) and sub_081564D8 (variant 1):
 * record every OBJ the call would write, with its full-precision position,
 * whether or not the game's 240x160 cull or the 128-entry pool lets it
 * through. */
void PortRlEmitSprite(const struct Sprite *sprite, int variant);
/* End of DrawToOamBuffer: the emitted list becomes the committed one. */
void PortRlCommitSprites(void);
/* With DrawToOamBuffer, in UpdateScreen: snapshot camera, layers, room. */
void PortRlCommitScreen(void);

#ifdef __cplusplus
}
#endif

#endif /* GUARD_PORT_RENDER_LIST_H */
