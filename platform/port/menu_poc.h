/* Front-end menu proof of concept (KATAM_MENU_POC=1 natively; on the web a
 * page under .../menus/ or with ?menus=1; off by default).
 *
 * Shows that the game's own menus can be edited in place:
 *   - Start Game's own One Player / Multiplayer pop-up is kept, but
 *     MULTIPLAYER no longer leads to the link-cable lobby: it opens a
 *     MULTIPLAYER screen (LOCAL: two players on this screen,
 *     platform/localmp.c; ONLINE: shown, not available yet).  The sub-games'
 *     copy of the pop-up is still skipped (they go straight to one player);
 *   - Game Select gets a fifth entry, SETTINGS (the five rows are re-spaced
 *     so they clear the walking Kirbys), which opens a settings screen drawn
 *     in the game picture and driven by the pad: a few player-facing fields
 *     of gPortSettings, applied and saved the way the port's own settings
 *     menu applies and saves them.  Labels, rows and text are GBA sprite
 *     tiles drawn by the port from a font in platform/menu_poc.c -- no ROM
 *     data is involved.
 *
 *   - the pause menu gains a fourth page after the world map, the same
 *     settings screen (game/src/pause_settings.c; Select cycles to it from
 *     the help page where there is no map).
 *
 * With the switch off every function here is a no-op and the game runs
 * exactly as before.  The switch is a host setting and is not recorded in
 * input recordings: replay a POC recording with the same environment.
 */
#ifndef GUARD_PORT_MENU_POC_H
#define GUARD_PORT_MENU_POC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

struct Unk_08138D64;

/* Nonzero when the proof of concept is switched on (read once). */
int PortMenuPoc(void);
void PortMenuPocSet(int on);   /* hosts with no environment (node) */
/* Online (platform/lmpnet.c): the pause SETTINGS page in a session.
 * Init: the host, at its session's first frame (the edit request zeroed,
 * the room code -- 4 letters, or NULL -- into session state).
 * EditApply: a final frame's edit (PORT_SIM_LNSET) by a player of this
 * device, carried out on this device.  SlotSettings: each slot's copy of
 * its device's settings (sealed PORT_LN_EV_PSET events); Word: this
 * device's settings as such a copy. */
void PortPocSessionSettingsInit(const char *code);
void PortPocSessionEditApply(int item, int dir);
void PortPocSlotSettingsSet(int slot, u32 word, u32 ext);
int  PortPocSlotSettingsHave(int slot, u32 *word, u32 *ext);
u32  PortPocSettingsWord(void);
u32  PortPocSettingsExtWord(void);      /* the external sections' online rows */

/* --- a SETTINGS section another file keeps ------------------------------------
 *
 * Rows the file owns, drawn by the SETTINGS screen like its own (the game's
 * banner, the value between arrows, the description line).  Upper-case text
 * in the fallback font's letters (A-Z 0-9 space : . - ! / % +).
 *
 * Online, the page shows the pausing player's device and an edit is carried
 * out by that device once its frame is final -- the step function is called
 * then, on that device only, outside any frame.  A row works in a session
 * only if onlineSlot gives it one of four slots (0..3, unique across all
 * sections): its value, 0..15, travels with each device's settings copy
 * (onlineGet on the device, onlineText on every device to draw it).  Every
 * other row is dimmed in a session ("NOT DURING AN ONLINE GAME").
 *
 * Adding a section (platform/menu_poc.c):
 *   1. an item IT_SEC_<NAME> in the item enum (before IT_MAINMENU) and its
 *      kItem entry { "NAME", K_SECTION, "DESCRIPTION" };
 *   2. the item in kTopItems (before IT_MAINMENU: its place on the list);
 *   3. for rows of your own: a kItems list and SECTION(kSecName, "NAME",
 *      list, &kTopModel, place); for another file's: its struct PortPocRows
 *      in kExtRows (before the NULL) and EXT_SECTION(kSecName, &rows,
 *      ROWCOUNT, parent, place, index in kExtRows) -- parent &kTopModel, or
 *      the section it sits in (LIGHTING: &kSecExperimental, a section row
 *      K_SECTION in that section's items);
 *   4. { IT_SEC_<NAME>, &kSecName } in kSectionOf, &kSecName in kSections
 *      and at the end of kModels (the tests' model numbers). */
struct PortPocRows {
    const char *header;                         /* the screen's header      */
    const char *(*name)(int row);
    const char *(*desc)(int row);
    void (*value)(int row, char *buf, int size);/* this device's value      */
    void (*step)(int row, int dir);             /* -1 / +1 (A is +1): apply */
    int  (*available)(int row);                 /* NULL: always             */
    int  (*onlineSlot)(int row);                /* NULL / -1: not online    */
    int  (*onlineGet)(int slot);                /* this device's 0..15      */
    void (*onlineText)(int slot, int v, char *buf, int size);
};
/* Each slot's player's name for the ONLINE list (sealed PORT_LN_EV_NAME). */
void PortPocSlotNameSet(int slot, u32 lo, u32 hi);
int  PortPocSlotNameSame(int slot, const char *name);
void PortPocNameWords(const char *name, u32 *lo, u32 *hi);
/* Online: the page's host state, carried with keyframes and images. */
u32  PortPocHostStateSize(void);
void PortPocHostStateSave(u8 *dest);
void PortPocHostStateLoad(const u8 *src);
/* The same, but the session's copies (each slot's settings and name) are
 * left as they are: the pause overlay's fork (platform/lmpnet.c) keeps its
 * own screen state and shows the session's copies as they are now. */
void PortPocHostStateLoadOwn(const u8 *src);
void PortPocHostStateRebuild(void);

/* --- Game Select's fifth entry -------------------------------------------- */

/* Game Select's labels were just allocated (end of sub_081397FC): allocate
 * and draw the extra label's tiles. */
void PortMenuPocAlloc(struct Unk_08138D64 *a1);
/* Game Select's labels are being freed (sub_0813A9DC): free ours too (and the
 * settings screen's, which use the same slots). */
void PortMenuPocFree(struct Unk_08138D64 *a1);
/* Once a frame, after the menu task's state function: put the extra label
 * (and, while it is selected, its description) into OAM. */
void PortMenuPocDraw(struct Unk_08138D64 *a1);
/* The cursor moved onto the extra entry: clear the description banner's
 * text (BG0), which the game's own description art would otherwise fill. */
void PortMenuPocBlankBanner(void);

/* Game Select gains one entry: cursor 0..4, the extra one is index 4. */
#define PORT_MENU_POC_SETTINGS 4
/* Game Select's row pitch with five rows (the game's is 0x15 for four). */
#define PORT_MENU_POC_ROW_PITCH 17

/* The Start Game pop-up (restored with local multiplayer): while its
 * MULTIPLAYER row is lit, our own description line instead of the game's
 * MULTI-PAK one (the banner is blanked by the caller); 0 = none. */
void PortMenuPocPopupDesc(int multiplayer);

/* --- the MULTIPLAYER screen (LOCAL / ONLINE) ---------------------------------
 * The settings screen's functions draw it (PortMenuPocSettingsDraw, ...Input,
 * ...CursorY/X, ...Close); this opens it instead of the settings. */
int  PortMenuPocMpOpen(struct Unk_08138D64 *a1);
/* Build the settings (multiplayer = 0) or MULTIPLAYER screen if it is not
 * up yet; 0 while the sprite memory is not free yet (ask again next frame;
 * `frames` asked so far, for a log line if it never comes). */
int  PortMenuPocScreenReady(struct Unk_08138D64 *a1, int multiplayer, int frames);
/* Does A on this row of the screen up now do anything? */
int PortMenuPocRowAvailable(int row);
/* A key on the MULTIPLAYER screen for the ONLINE screens behind its ONLINE
 * row (platform/port/online_ui.h): a PORT_OUI_K_* (0: the screen's own). */
int PortMenuPocMpKey(struct Unk_08138D64 *a1, unsigned key);
/* An ONLINE screen is up: the menu's idle timeout must wait. */
int PortMenuPocMpBusy(void);
/* The fade out after PORT_OUI_K_START is over: the game proper starts. */
void PortMenuPocMpStarted(void);
/* a1->unk1 while the MULTIPLAYER screen is up. */
#define PORT_MENU_POC_MP_SCREEN 0x11
#define PORT_MENU_POC_MP_LOCAL  0
#define PORT_MENU_POC_MP_ONLINE 1
#define PORT_MENU_POC_MP_CARD   2   /* PLAYER CARD (platform/player_card.c) */

/* --- the settings screen ---------------------------------------------------- */

/* a1->unk1 while the settings screen is up (the game uses 1..10). */
#define PORT_MENU_POC_SCREEN 0x10

/* Allocate and draw the screen (row cursor a1->unk3A6 = 0); 0 if there is
 * no sprite memory for it (yet). */
int  PortMenuPocSettingsOpen(struct Unk_08138D64 *a1);
/* Put it into OAM, everything shifted right by slide pixels (0 = in place). */
void PortMenuPocSettingsDraw(struct Unk_08138D64 *a1, int slide);
/* One pad event (sub_0813B484's 0..6): PORT_POC_IN_* as PortPocScreenInput
 * (B is the caller's when this returns 0). */
int PortMenuPocSettingsInput(struct Unk_08138D64 *a1, unsigned key);
/* The star cursor's place for the current row. */
int PortMenuPocSettingsCursorY(struct Unk_08138D64 *a1);
int PortMenuPocSettingsCursorX(void);
void PortMenuPocSettingsClose(struct Unk_08138D64 *a1);

/* --- the screen, shared by the front end and the pause page ------------------
 *
 * The same rows (six on screen, seven with PLAYERS), header and description
 * line from one set of functions;
 * a caller supplies PORT_POC_SCREEN_TILES of OBJ VRAM, keeps the row cursor
 * in its own memory, and names the label (ERASE A FILE) and header
 * (SUB-GAMES) sprite variants of the game's language to build them from.
 * The description line's colours are copied each frame from the BG palette
 * line of the banner at 0x0600FD40 (BG0, screen block 31, rows 21-22). */
#define PORT_POC_SCREEN_TILES 660
void PortPocScreenBuild(u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row);
/* The same for the pause page: its top level has MAIN MENU too. */
void PortPocPauseScreenBuild(u32 tiles, u16 lblAnim, u8 lblVariant, u16 hdrAnim, u8 hdrVariant, int row);
/* Into OAM; everything shifted right by slide pixels (0 = in place). */
void PortPocScreenDraw(u32 tiles, int row, int slide, u8 layer);
/* key as sub_0813B484 (1 A, 2 B, 3 up, 4 down, 5 left, 6 right): 0 nothing
 * (B: the caller's own back), 1 the cursor moved, 2 a value changed
 * (applied and saved), 3 the screen changed in place (SETTINGS -> PLAYERS on
 * A, PLAYERS -> SETTINGS on B; the cursor row too), 4 something was done
 * (a PLAYERS change), 5 refused. */
#define PORT_POC_IN_NONE    0
#define PORT_POC_IN_CURSOR  1
#define PORT_POC_IN_VALUE   2
#define PORT_POC_IN_SCREEN  3
#define PORT_POC_IN_DONE    4
#define PORT_POC_IN_REFUSED 5
int PortPocScreenInput(u32 tiles, u8 *row, unsigned key);
/* Is the screen below SETTINGS' top level (a section, NO / YES, PLAYERS, a
 * player card), where B goes back one level instead of leaving? */
int PortPocScreenSub(void);
/* Tests: the frame the screen was last drawn on; the model it shows (0
 * SETTINGS, 1 MULTIPLAYER, 2-4 ONLINE, 5 PLAYERS, 6-7 the card, 8 the
 * session's PLAYERS, 9 the pause page's SETTINGS, 10-15 the sections
 * DISPLAY SOUND PLAYERS ONLINE GAME ADVANCED, 16 NO / YES, 17 PLAYING). */
u32 PortPocScreenLastDraw(void);
int PortPocScreenModel(void);
/* Tests: the help page's room line is up; the settings word the page shows. */
int PortPocHelpLineShown(void);
u32 PortPocShownWord(void);
int PortPocScreenCursorY(int row);
int PortPocScreenCursorX(void);

/* An animation variant's first frame as 4bpp colour indices (cw x ch,
 * cleared first); returns its width, *h its height, 0 on anything odd. */
int PortPocDecodeFrame(u16 animId, u8 variant, u8 *px, int cw, int ch, int *h);

/* --- the pause menu's settings page (game/src/pause_settings.c) -------------- */

/* Its sprite memory: the screen's tiles, then the star cursor's 16; the
 * whole range is copied away when the page opens and put back when it
 * closes, so neither gameplay's sprite tiles nor another page's are lost. */
#define PORT_POC_PAUSE_OBJ_VRAM    0x06010000u
#define PORT_POC_PAUSE_CURSOR_VRAM (PORT_POC_PAUSE_OBJ_VRAM + PORT_POC_SCREEN_TILES * 32)
#define PORT_POC_PAUSE_OBJ_BYTES   0x5600u
void PortPocPauseObjSave(void);
void PortPocPauseObjRestore(void);

/* The pause help page's line about the online game (its code, mode and
 * players): opened as the page is made (before it saves its palettes),
 * drawn every frame, closed as the page goes (its tiles put back).  Only
 * in a session; otherwise nothing. */
#define PORT_POC_HELP_BYTES 768
void PortPocHelpLineOpen(void);
void PortPocHelpLineDraw(void);
void PortPocHelpLineClose(void);

#ifdef __cplusplus
}
#endif

#endif
