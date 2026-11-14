/* PORT: the pause menu's settings page (KATAM_MENU_POC, port/menu_poc.h).
 *
 * A fourth page in the Select cycle, after the world map: the front end's
 * settings screen (platform/menu_poc.c draws both from the same functions)
 * on the pause screens' own terms -- opened and left with their page-turn
 * fade and sound (pause_help.c's PauseMenuMain changes gPauseMenus[].menuId,
 * the page sees it and fades), B or Start back to the game, Up/Down/Left/
 * Right/A its own.  The switch off, nothing reaches this file: menuId never
 * becomes MENU_PORT_SETTINGS.
 *
 * Video, while it is up (the other pages' layout, reused where it can be):
 *   BG0  the description banner, as Game Select has it (charbase 0, screen
 *        block 31, rows 21-22 scrolled up 24 px)
 *   BG1  the map pages' button hints (WorldMapPauseEnableUI: charbase 2,
 *        screen block 23), moved to the top right
 *   BG2  the backdrop: the help page's paper (charbase 1, screen block 30)
 *   OBJ  the screen and the star, from fixed tiles saved and put back
 *        around the page (PortPocPauseObjSave/Restore); palette lines 0-4,
 *        6 and 9 as the front end uses them.
 * BG VRAM is the pause screens' to use (each page clears or overwrites it,
 * and FinishInGameInterludeScreen clears it before the level reloads its own);
 * palettes come back from the source copies on the way back to the game. */

#ifdef __cplusplus
extern "C" {
#endif

#include "global.h"
#include "data.h"
#include "main.h"
#include "palette.h"
#include "palette_effects.h"
#include "pause_area_map.h"
#include "pause_fade.h"
#include "pause_help.h"
#include "pause_settings.h"
#include "pause_world_map.h"
#include "in_game_interlude.h"
#include "sprite.h"
#include "subgames.h"
#include "task.h"
#include "constants/songs.h"
#include "port/menu_poc.h"

struct PauseSettings {
    /* 0x00 */ struct Sprite cursor;
    /* 0x28 */ u8 row;
    /* 0x29 */ s8 toGameCounter;
    /* 0x2A */ u8 nextMenuId;
    /* 0x2B */ u8 open;
    /* 0x2C */ u8 repeatIntervals[NUM_KEYS];
};

static void PauseSettingsMain(void);
static void PauseSettingsToNextMenu(void);
static void PauseSettingsToGame(void);
static void PauseSettingsDestructor(struct Task *);

#define TILES ((u32)PORT_POC_PAUSE_OBJ_VRAM)

/* The help page's frame picture (pause_help.c: sHelpMenuUnkTiledBGsIndices
 * [*][0], the same in every language), only its outer paper: the tile its
 * top-left corner shows, over the whole screen. */
static void PauseSettingsBackdrop(void) {
    const struct TiledBg_082D7850 *frame = gUnk_082D7850[0];
    u16 *map = (u16 *)BG_SCREEN_ADDR(30);
    u16 paper;
    s32 i;

    LZ77UnCompVram(frame->tileset, (u16 *)BG_CHAR_ADDR(1));
    paper = frame->tilemap[0];
    for (i = 0; i < 0x400; i++)
        map[i] = paper;
    if (gMainFlags & MAIN_FLAG_BG_PALETTE_TRANSFORMATION_ENABLE) {
        LoadBgPaletteWithTransformation(gHelpMenuPalette, 0, 0x70);
    }
    else {
        DmaCopy16(3, gHelpMenuPalette, gBgPalette, 0x70 * sizeof(u16));
        gMainFlags |= MAIN_FLAG_BG_PALETTE_SYNC_ENABLE;
    }
}

/* The world map's hint strip (SELECT SWITCH at the left of rows 18-19, B QUIT
 * at the right; WorldMapPauseEnableUI blanks B QUIT for a player who cannot
 * quit) moved up beside the header tab, right-aligned, one line above the
 * other -- the description banner owns the bottom rows here.  Two layers on
 * the same tiles, so the lines can sit 12 px apart: BG1 (screen block 23)
 * SELECT SWITCH, BG3 (screen block 22) B QUIT. */
#define HINT_EMPTY 0x7080
static void PauseSettingsHints(void) {
    u16 *map1 = (u16 *)BG_SCREEN_ADDR(23), *map3 = (u16 *)BG_SCREEN_ADDR(22);
    u16 strip[2][32];
    s32 x, y;

    WorldMapPauseEnableUI();
    for (y = 0; y < 2; y++)
        for (x = 0; x < 32; x++)
            strip[y][x] = map1[(18 + y) * 32 + x];
    for (x = 0; x < 0x400; x++) {
        map1[x] = HINT_EMPTY;
        map3[x] = HINT_EMPTY;
    }
    for (y = 0; y < 2; y++) {
        for (x = 0; x < 12; x++)        /* SELECT SWITCH: x 9..95 of the strip */
            map1[y * 32 + 18 + x] = strip[y][x];
        for (x = 22; x < 30; x++)       /* B QUIT: x 185..228 */
            map3[y * 32 + x] = strip[y][x];
    }
    gBgCntRegs[3] = (gBgCntRegs[1] & ~BGCNT_SCREENBASE(31)) | BGCNT_SCREENBASE(22);
    /* ink rows 145..156 of the strip: lines at y 0..11 and 12..23, both
     * ending at x 235 */
    gBgScrollRegs[1][0] = 4;
    gBgScrollRegs[1][1] = 1;
    gBgScrollRegs[3][0] = -7;
    gBgScrollRegs[3][1] = -11;
}

void CreatePauseSettings(void) {
    struct Task *task;
    struct PauseSettings *tmp, *ps;
    u16 lblAnim, hdrAnim;
    u8 lblVariant, hdrVariant;
    u16 white = RGB_WHITE;
    s32 i;

    LoadBgPaletteAndPaletteEffectSource(&white, 0, 1);
    CpuFill32(0, (u32 *)BG_VRAM, BG_VRAM_SIZE);
    for (i = 0; i < 4; i++) {
        gBgScrollRegs[i][0] = 0;
        gBgScrollRegs[i][1] = 0;
    }
    gDispCnt = DISPCNT_MODE_0 | DISPCNT_OBJ_1D_MAP | DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON | DISPCNT_OBJ_ON;
    gBgCntRegs[0] = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0) | BGCNT_16COLOR | BGCNT_SCREENBASE(31) | BGCNT_TXT256x256;
    gBgCntRegs[2] = BGCNT_PRIORITY(3) | BGCNT_CHARBASE(1) | BGCNT_16COLOR | BGCNT_SCREENBASE(30) | BGCNT_TXT256x256;
    gBgScrollRegs[0][1] = 0x18;

    task = TaskCreate(FN(PauseSettingsMain), sizeof(struct PauseSettings), 0x1000, TASK_x0004 | TASK_USE_IWRAM,
                      FN(PauseSettingsDestructor));
    ps = tmp = TaskGetStructPtr(task);
    CpuFill32(0, ps, sizeof(*ps));

    PauseSettingsBackdrop();
    PauseSettingsHints();       /* sets BG1 */

    PortPocPauseObjSave();
    ps->open = 1;
    PortPocPauseLoadMenuColours(PORT_POC_PAUSE_CURSOR_VRAM);
    PortPocPauseMenuArt(&lblAnim, &lblVariant, &hdrAnim, &hdrVariant);
    ps->row = 0;
    PortPocScreenBuild(TILES, lblAnim, lblVariant, hdrAnim, hdrVariant, ps->row);
    PortMenuPocBlankBanner();
    /* the star, as Game Select's (sub_08139344: unk28C) */
    SpriteInitNoPointer2(&ps->cursor, PORT_POC_PAUSE_CURSOR_VRAM, 0x280, 0x355, 2, 0, 0xFF, 0x10, 1,
                         PortPocScreenCursorX(), PortPocScreenCursorY(ps->row), 0);

    /* the front end's key repeat (sub_08138B44) while the page is up */
    for (i = 0; i < NUM_KEYS; i++) {
        ps->repeatIntervals[i] = gKeysContinuedRepeatIntervals[i];
        gKeysContinuedRepeatIntervals[i] = 6;
    }

    SaveObjPaletteColors(0x00, 0xA0);
    SaveBgPaletteColors(0x00, 0xff);
}

/* Put back what the page borrowed; once, before the next page or the game. */
static void PauseSettingsClose(struct PauseSettings *ps) {
    s32 i;

    if (!ps->open)
        return;
    ps->open = 0;
    PortPocPauseObjRestore();
    for (i = 0; i < NUM_KEYS; i++)
        gKeysContinuedRepeatIntervals[i] = ps->repeatIntervals[i];
}

static void PauseSettingsDestructor(struct Task *task) {
    PauseSettingsClose(TaskGetStructPtr(task));
}

static void PauseSettingsDraw(struct PauseSettings *ps) {
    if (!ps->open)
        return;
    PortPocScreenDraw(TILES, ps->row, 0, 0x10);
    if (!sub_08155128(&ps->cursor))
        ps->cursor.unk1B = 0xFF;
    DisplaySprite(&ps->cursor);
}

/* The pad as Game Select's sub_0813B484 reads it, A only (Start is the pause
 * screen's), and only the local player's, when the pause screen lets it. */
static u8 PauseSettingsKey(void) {
    struct PauseMenu *menu = &gPauseMenus[gLocalPlayerId];

    if (menu->flags & (MENU_FLAG_DISABLE_INPUT | MENU_FLAG_AI))
        return 0;
    if (menu->pressedKeys & A_BUTTON)
        return 1;
    if (gRepeatedKeys & DPAD_UP)
        return 3;
    if (gRepeatedKeys & DPAD_DOWN)
        return 4;
    if (gRepeatedKeys & DPAD_LEFT)
        return 5;
    if (gRepeatedKeys & DPAD_RIGHT)
        return 6;
    return 0;
}

static void PauseSettingsMain(void) {
    struct PauseSettings *tmp = TaskGetStructPtr(gCurTask), *ps = tmp;
    u8 playerId, key;

    if (gPauseMenus[0].flags & MENU_FLAG_BACK_TO_GAME || gPauseMenus[1].flags & MENU_FLAG_BACK_TO_GAME ||
        gPauseMenus[2].flags & MENU_FLAG_BACK_TO_GAME || gPauseMenus[3].flags & MENU_FLAG_BACK_TO_GAME) {
        m4aSongNumStart(SE_PAUSE_MENU_ACTIVATE);
        sub_08124EC8();
        gCurTask->main = FN(PauseSettingsToGame);
        PauseSettingsDraw(ps);
        return;
    }

    for (playerId = 0; playerId < 4; playerId++) {
        if (gPauseMenus[playerId].flags & MENU_FLAG_CURRENT_PLAYER &&
            gPauseMenus[playerId].menuId != MENU_PORT_SETTINGS) {
            ps->nextMenuId = gPauseMenus[playerId].menuId;
            gCurTask->main = FN(PauseSettingsToNextMenu);
            CreatePauseFade(0x20, 1);
            PauseSettingsDraw(ps);
            return;
        }
    }

    key = PauseSettingsKey();
    switch (PortPocScreenInput(TILES, &ps->row, key)) {
    case 1:
        m4aSongNumStart(SE_MAIN_MENU_CURSOR);
        ps->cursor.y = PortPocScreenCursorY(ps->row);
        sub_08155128(&ps->cursor);
        break;
    case 2:
        m4aSongNumStart(key == 1 ? SE_MAIN_MENU_SELECT : SE_MAIN_MENU_CURSOR);
        break;
    }
    PauseSettingsDraw(ps);
}

static void PauseSettingsToNextMenu(void) {
    struct PauseSettings *tmp = TaskGetStructPtr(gCurTask), *ps = tmp;

    if (!sub_0812A304()) {
        PauseSettingsClose(ps);
        if (ps->nextMenuId == MENU_WORLDMAP)
            CreateWorldMap(WORLDMAP_NO_UNLOCK);
        else if (ps->nextMenuId == MENU_AREAMAP)
            CreateAreaMap();
        else
            CreateHelpMenu();
        CreatePauseFade(-0x20, 1);
        TaskDestroy(gCurTask);
        return;
    }
    PauseSettingsDraw(ps);
}

static void PauseSettingsToGame(void) {
    struct PauseSettings *tmp = TaskGetStructPtr(gCurTask), *ps = tmp;

    if (ps->toGameCounter++ > 18) {
        PauseSettingsClose(ps);
        TaskDestroy(gPauseMenus[gLocalPlayerId].mainTask);
        FinishInGameInterludeScreen();
        TaskDestroy(gCurTask);
    }
    else {
        PauseSettingsDraw(ps);
    }
}

#ifdef __cplusplus
}
#endif

/* PORT-FNID begin: this file's functions the game stores as code
 * IDs, so the lookup can name statics (tools/codemod_fnid.py
 * --gen rewrites this block). */
PORT_FNID_TABLE(game_src_pause_settings) {
    PORT_FNID(PauseSettingsToNextMenu),
    PORT_FNID(PauseSettingsToGame),
    PORT_FNID(PauseSettingsDestructor),
    PORT_FNID(PauseSettingsMain),
    PORT_FNID_END
};
/* PORT-FNID end */
