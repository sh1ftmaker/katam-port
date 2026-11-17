#ifndef GUARD_PORT_PLAYER_CARD_H
#define GUARD_PORT_PLAYER_CARD_H
/* The player card: a name and a Kirby colour per player, kept with the
 * device's settings, and the in-game screens that edit it (Start Game >
 * MULTIPLAYER > PLAYER CARD).  platform/player_card.c has the design.
 *
 * Device state only.  Nothing here is simulation state or is read by the
 * simulation: a colour reaches the game as a recorded, frame-stamped paint
 * (port/kirby_paint.h) that this file schedules; a name never reaches it. */

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_CARD_PLAYERS  4
#define PORT_CARD_NAME_MAX 8        /* characters; A-Z 0-9 space - . ! */

/* How a card's colour is chosen. */
#define PORT_CARD_GAME   0          /* the game's own (spray paint / slot)     */
#define PORT_CARD_SPRAY  1          /* one of the game's fourteen              */
#define PORT_CARD_CUSTOM 2          /* by parts: a body and feet from the
                                     * presets (port/kirby_paint.h); a card
                                     * saved before the parts: its one colour
                                     * as hue / saturation / value          */

struct PortCard {
    char name[PORT_CARD_NAME_MAX + 1];  /* "" = no name (shown as P1..P4)  */
    u8   mode;                          /* PORT_CARD_*                       */
    u8   spray;                         /* 0..13 while mode is SPRAY         */
    u16  hue;                           /* 0..359 } while mode is CUSTOM and */
    u8   sat, val;                      /* 0..100 } body is 0 (an old card)  */
    u8   body;                          /* CUSTOM: 1 + preset, 0 = h/s/v     */
    u8   feet;                          /* CUSTOM: 1 + preset, 0 = as the body */
};

/* --- the cards (player p = 0..3; card 0 is this device's own profile) ----- */
const struct PortCard *PortCardGet(int p);
void PortCardSet(int p, const struct PortCard *c);  /* clamps, saves         */
/* The paint value (port/kirby_paint.h) a card stands for; 0 = the game's.
 * PortCardFeet: its feet value (PORT_PAINT_FEET_ON | BGR555; 0 = with the
 * body). */
u32  PortCardPaint(int p);
u32  PortCardFeet(int p);
/* A card's name as shown: its name, or "P1".."P4". */
const char *PortCardShownName(int p);
/* 0xRRGGBB of a custom colour (h 0..359, s and v 0..100). */
u32  PortCardHsvRgb(int h, int s, int v);

/* --- settings (platform/ui/settings.c) ------------------------------------- */
/* A settings line card0..card3 (MODE,SPRAY,HUE,SAT,VAL,NAME, as ever), or
 * card0parts..card3parts (BODY,FEET: the card's parts, 1 + preset, 0 =
 * none; written only when set, after its card line); 1 if it was ours. */
int  PortCardParseSetting(const char *key, const char *val);
/* Our lines, appended to the settings text; returns the bytes written. */
int  PortCardFormatSettings(char *buf, int size);

/* --- names on this device (presentation: the party HUD, name tags) --------
 * Slot s's (0..3) name for the screen, "" when nobody should be named (a CPU
 * Kirby).  Locally a human slot shows its card's name; online the session
 * layer says who is in which slot (PortCardSetSlotName).  Never simulation
 * state: draw it, do not feed it to the game. */
const char *PortPlayerName(int slot);
/* The colour to draw slot s's name in (0xRRGGBB): the Kirby's body fill. */
u32  PortPlayerColor(int slot);
/* Where a name tag over slot s's Kirby goes: the top of its head in world
 * pixels (*x, *y) and its room; 0 when it should not be tagged (a CPU, not
 * alive, between rooms, not in the local player's room, tags off).  The tag
 * reads PortPlayerName, or "nP" for a person without a name.  Read
 * from game memory, never written: the renderer projects it. */
int  PortPlayerTagAnchor(int slot, s32 *x, s32 *y, u16 *room);
/* A name as every screen may draw it (in place): A-Z 0-9 space - . ! (a-z
 * made capitals, anything else a space), no leading or trailing spaces, at
 * most PORT_CARD_NAME_MAX.  Names from other devices go through it. */
void PortCardCleanName(char *name);
/* Online: slot s is `name` on this device (NULL / "" clears it). */
void PortCardSetSlotName(int slot, const char *name);
/* Name tags over the Kirbys (drawn by ui/party_hud.c): the device setting
 * gPortSettings.nameTags, on by default (SETTINGS > NAME TAGS). */
int  PortCardTagsOn(void);
void PortCardSetTagsOn(int on);

/* --- online hand-off (the session layer: web/online.js, web/ln_session.js) --
 * "Send this device's profile to the host": the profile as a short string,
 * NAME then a tab then the paint as 8 hex digits, then (feet of their own)
 * a tab and the feet as 4 ("KIRBY\t02FF80C0" or "KIRBY\t01000002\tC622");
 * the page sends it and the host turns it into the two calls below. */
const char *PortCardProfileString(void);
/* Host: apply slot s's paint as a sealed event at the next frame it seals
 * (PORT_LN_EV_PAINT); returns that frame, -1 when not the host of a session
 * or the value is not a paint.  Every device then applies it at that frame. */
int  PortCardHostApplyPaint(int slot, u32 paint, u32 feet);
/* Host: the same from a profile string (paint part); also names the slot. */
int  PortCardHostApplyProfile(int slot, const char *profile);

/* --- online, every slot (web/ln_session.js carries these) -------------------
 * This device's four seats' profiles, one per line (seat s wears card s):
 * every device sends them to the host whenever they change and after every
 * image.  The host keeps them by (device, seat); PortCardHostFrame then seals
 * each slot's paint from its owner (PortLnSlotOwner): the host's own seats
 * from its cards, a guest's from its profiles, a CPU slot (or one whose
 * owner's profile has not arrived) the game's own colour.  The host names
 * every slot likewise (PortCardSlotNames: a name per Kirby slot,
 * PORT_LN_SLOTS of them -- four; the wide build N -- tab-separated) and the
 * page relays them; a guest applies them with PortCardSetSlotNames. */
const char *PortCardSeatProfiles(void);
void PortCardHostSetProfiles(int dev, const char *profiles);   /* dev 1..3 */
void PortCardHostDropDevice(int dev);
const char *PortCardSlotNames(void);
void PortCardSetSlotNames(const char *names);
/* A session ended: the slots' names and the profiles are forgotten. */
void PortCardSessionEnd(void);
/* Tests / console: card p from a settings value "MODE,SPRAY,HUE,SAT,VAL,NAME",
 * optionally followed by "|BODY,FEET" (its parts line) (not saved). */
int  PortCardSetJs(int p, const char *val);

/* --- the host's frame hook (platform/main.c, before the latch) -------------
 * Local play: a human slot wears its card (slot 0 from the start, the
 * others from the frame they join), a CPU slot the game's colour -- scheduled
 * as `paint` lines at the next latch whenever the simulation's paint
 * differs.  Online host: every slot from its owner's card, as session
 * events (a card edited on a pause overlay goes on live).  Nothing while
 * replaying, while a guest, under rollback / netsim / seats, or -- locally --
 * while a card is being edited (the change is put on when the card closes).
 * A card edited on the pause overlay is saved here, at the live frame. */
void PortCardHostFrame(void);

/* --- the card screens (the front end's MULTIPLAYER screen draws them; a
 * players page or the pause menu can too) --------------------------------- */
#define PORT_CARD_SUB_NONE 0
#define PORT_CARD_SUB_CARD 1        /* PLAYER / NAME / COLOR                  */
#define PORT_CARD_SUB_PICK 2        /* BODY / FEET, and the grid of presets   */
#define PORT_CARD_ROWS     3        /* the CARD screen's                      */
#define PORT_CARD_PICK_ROWS 2       /* the PICK screen's                      */

/* Open player p's card (0..3); `fixed` hides the PLAYER row's choice (a
 * caller that already knows whose card it is).  The screens below are then
 * PortCardSub() != NONE until a B on the CARD screen closes them. */
void PortPlayerCardOpen(int p, int fixed);
/* An invite link's first screen (platform/online_ui.c): this device's own
 * card (0), its first row JOIN with the game's code (4 letters) in place of
 * PLAYER; A on it closes the card (kept) and PortCardKey says
 * PORT_CARD_K_JOIN.  The cursor starts on JOIN: one press goes on. */
void PortPlayerCardOpenInvite(const char code[4]);
int  PortCardInvite(void);              /* the card open is an invite's   */
void PortPlayerCardClose(void);
int  PortCardSub(void);
int  PortCardPlayer(void);
u32  PortCardGen(void);                 /* changes when anything shown does */
const char *PortCardHeader(int sub);
const char *PortCardRowName(int sub, int row);
void PortCardRowValue(int sub, int row, int selected, char *buf, int size);
const char *PortCardRowDesc(int sub, int row);
/* One key (sub_0813B484's: 1 A, 2 B, 3 up, 4 down, 5 left, 6 right) on a
 * card screen; *row the cursor.  PORT_CARD_K_*; PortCardSub() may change. */
#define PORT_CARD_K_NONE   0
#define PORT_CARD_K_QUIET  1
#define PORT_CARD_K_CURSOR 2
#define PORT_CARD_K_SELECT 3
#define PORT_CARD_K_BACK   4
#define PORT_CARD_K_CLOSE  5        /* B on the CARD screen: it is closed     */
#define PORT_CARD_K_JOIN   6        /* an invite's card: A on JOIN -- closed,
                                     * kept, the caller joins the game        */
int  PortCardKey(int *row, unsigned key);
#define PORT_CARD_KEY_START 7   /* Start without A (PortCardKey works it out) */
/* The edited card's body palette (16 BGR555), for a preview. */
int  PortCardPreviewPalette(u16 out[16]);
/* The PICK screen's grid: the presets as BGR555 into out (PORT_PAINT_PRESETS,
 * PORT_PAINT_PRESET_COLS a row), and the cell to frame: the cursor while
 * the grid is open (*open = 1), else the row's part's own preset (-1 when
 * it is none: a feet as the body, an old card's colour). */
int  PortCardGrid(int row, u16 out[], int *open);
/* The colour a PICK row's part shows now (BGR555), for its swatch. */
u16  PortCardPartColor(int row);
/* The edited name with the cursor: is the NAME row taking typed letters
 * (the web page sends a keyboard's or a phone keyboard's letters here). */
int  PortCardWantsText(void);
/* The screen's row cursor, as the screen's code moved it (for the above). */
void PortCardNoteRow(int row);
/* The NAME row's typing state (A on the row starts it; Start, Enter,
 * Escape, Up/Down end it): only then does the page send letters. */
int  PortCardTyping(void);
void PortCardTypingEnd(void);
void PortCardTypeChar(int c);           /* a letter; 8 = backspace (typing only) */
void PortCardSetNameJs(const char *name);
const char *PortCardNameJs(void);

/* Native debugging: KATAM_PAINT_SHEET=out.png writes a sheet of the derived
 * palettes and a standing Kirby in each (ROM-derived: keep it out of the
 * repository). */
void PortCardDebugSheet(void);

#ifdef __cplusplus
}
#endif
#endif /* GUARD_PORT_PLAYER_CARD_H */
