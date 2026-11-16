/* The player card: platform/port/player_card.h says what.
 *
 * --- the cards ----------------------------------------------------------------
 *
 * Four per device: card 0 is the device's own profile (what an online game
 * hears about this player), cards 1..3 are the people who play on this
 * screen as players 2..4.  A card is a name (up to PORT_CARD_NAME_MAX of the
 * characters every font the screens use can draw: A-Z, 0-9, space, - . !)
 * and a colour: the game's own, one of the game's fourteen spray colours, or
 * a custom colour picked as hue / saturation / brightness.  They are kept in
 * the settings text (settings.ini natively, localStorage on the web) as
 * `card0=` .. `card3=` lines, and saved on every change the way the settings
 * screen saves.  Defaults: no name (shown as P1..P4), the game's own colour --
 * so a device nobody touched plays exactly as before.
 *
 * --- a colour reaches the game as an input -------------------------------------
 *
 * The simulation never reads a card.  PortCardHostFrame (once a frame, from
 * the host's side of the frame boundary) compares what each slot should wear
 * with what the simulation has (PortPaintOfSlot) and, when they differ,
 * schedules a paint at the next latch: a `paint` line in the recording
 * (local play), or a sealed session event (online, the host's own slot).  A
 * replay applies the recorded lines and asks nothing of the cards.
 *
 *   local:   slot 0 wears card 0 from the start; slot s (1..3) wears card s
 *            from the frame it becomes a person, and the game's colour again
 *            when the CPU takes it back.
 *   online:  the session layer carries profiles (PortCardSeatProfiles on
 *            every device, PortCardHostSetProfiles on the host) and the host
 *            seals every slot's paint (PORT_LN_EV_PAINT) and name
 *            (PORT_LN_EV_NAME, with the menus); a guest names its Kirbys
 *            from the sealed names (else the host's relayed ones).  Names
 *            from another device are cleaned on arrival (PortCardCleanName).
 *
 * Locally, while a card is open for editing nothing is scheduled (the game
 * is paused under it anyway); the result goes in when the card closes.
 * Online a card is edited live -- SETTINGS > PLAYERS > PLAYER CARD on this
 * device's pause overlay, which only ever opens the overlay seat's own card:
 * every step that changes it (a colour step, a letter; not the grid's cursor,
 * which is kept only on A) reaches the host as a profile and is sealed, for
 * every device at the same frame, while the player is still on the card.
 *
 * An invite link's first screen is this device's card, with JOIN on top
 * (PortPlayerCardOpenInvite; platform/online_ui.c brings the front end
 * there).
 *
 * --- names --------------------------------------------------------------------
 *
 * Device state for the presentation layer (PortPlayerName): never written into
 * game memory, never sent to the simulation.
 *
 * --- the screens ----------------------------------------------------------------
 *
 * Two, drawn by platform/menu_poc.c's list screen exactly as the ONLINE screens
 * are (platform/online_ui.c is the pattern): CARD (PLAYER, NAME, COLOR) and,
 * behind COLOR's A, PICK: a row per part the game's palette has (BODY, and
 * FEET -- the game draws the cheeks with the feet's colour; kirby_paint.c
 * has the study) over a grid of the 32 presets, with a live preview of the
 * Kirby.  On a row Left/Right step its part through the presets (FEET's
 * first step is BODY: shaded with the body, as the game's own paints are),
 * A opens the grid on it: the d-pad moves the cursor and the Kirby wears
 * the cell under it, A keeps it, B puts back what it was.  B on the rows:
 * back to the card.  A spray chosen on the CARD screen is the whole Kirby,
 * feet and all.  This file is their model: text, keys, the edited card.
 * Host memory, front end only. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/simblock.h"
#include "port/kirby_paint.h"
#include "port/player_card.h"
#include "port/inputlog.h"
#include "port/lmpnet.h"
#include "port/localmp.h"
#include "port/rollback.h"
#include "port/netsim.h"
#include "port/seats.h"
#include "port/menu_poc.h"
#include "ui/ui.h"              /* PortSettingsChanged */
#include "ui/settings.h"        /* gPortSettings.nameTags */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define CARD_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CARD_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Names are per Kirby slot (PORT_LN_SLOTS: four, the wide build one per
 * slot); cards are per seat of this device (PORT_CARD_PLAYERS, four); paint
 * is per slot of the simulation's paint table (kirby_paint.c
 * PORT_PAINT_SLOTS: the same). */
#define CARD_SLOTS PORT_LN_SLOTS
#define PAINT_SLOTS PORT_PAINT_SLOTS

static struct PortCard sCards[PORT_CARD_PLAYERS];
static char sSlotName[CARD_SLOTS][PORT_CARD_NAME_MAX + 1];
static int sSlotNamed[CARD_SLOTS];

/* The characters a name may hold, in the order Left / Right step through
 * them (space first: it ends a name). */
static const char kNameChars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.!";
#define NNAME ((int)sizeof(kNameChars) - 1)

/* The game's names for its spray paints (the collection room's). */
static const char *const kSprayName[PORT_PAINT_COLORS] = {
    "PINK", "YELLOW", "RED", "GREEN", "SNOW", "CARBON", "OCEAN",
    "SAPPHIRE", "GRAPE", "EMERALD", "ORANGE", "CHOCOLATE", "CHERRY", "CHALK",
};

static int NameCharOk(int c)
{
    return c != '\0' && strchr(kNameChars, c) != NULL;
}

static void CleanName(char *n)
{
    int i, len;

    for (i = 0; n[i]; i++) {
        int c = n[i];

        if (c >= 'a' && c <= 'z')
            c -= 32;
        n[i] = (char)(NameCharOk(c) ? c : ' ');
    }
    /* no leading or trailing spaces */
    while (n[0] == ' ')
        memmove(n, n + 1, strlen(n));
    len = (int)strlen(n);
    while (len > 0 && n[len - 1] == ' ')
        n[--len] = '\0';
}

CARD_EXPORT void PortCardCleanName(char *name)
{
    if (name == NULL)
        return;
    if (strlen(name) > PORT_CARD_NAME_MAX)
        name[PORT_CARD_NAME_MAX] = '\0';
    CleanName(name);
}

static void Clamp(struct PortCard *c)
{
    c->name[PORT_CARD_NAME_MAX] = '\0';
    CleanName(c->name);
    if (c->mode > PORT_CARD_CUSTOM)
        c->mode = PORT_CARD_GAME;
    if (c->spray >= PORT_PAINT_COLORS)
        c->spray = 0;
    c->hue %= 360;
    if (c->sat > 100) c->sat = 100;
    if (c->val > 100) c->val = 100;
    if (c->body > PORT_PAINT_PRESETS) c->body = 0;
    if (c->feet > PORT_PAINT_PRESETS) c->feet = 0;
}

const struct PortCard *PortCardGet(int p)
{
    return &sCards[p >= 0 && p < PORT_CARD_PLAYERS ? p : 0];
}

/* An edit on the pause overlay (platform/lmpnet.c "the pause overlay") is
 * made while the overlay's private copy of the game is in memory: it is saved
 * at the next live frame (PortCardHostFrame), as the page's other edits are
 * carried out there (PortLnOverlayEdit). */
static int sSavePending;

static void Save(void)
{
    if (PortLnOverlayRunning()) {
        sSavePending = 1;
        return;
    }
    sSavePending = 0;
    PortSettingsChanged();      /* formats the settings, ours included, and saves */
}

void PortCardSet(int p, const struct PortCard *c)
{
    if (p < 0 || p >= PORT_CARD_PLAYERS || c == NULL)
        return;
    sCards[p] = *c;
    Clamp(&sCards[p]);
    Save();
}

/* --- colours ------------------------------------------------------------------ */

static void Hsv8(int h1536, int s, int v, int *r, int *g, int *b)
{
    int sector, f, p, q, t;

    if (s <= 0) {
        *r = *g = *b = v;
        return;
    }
    h1536 = (h1536 % 1536 + 1536) % 1536;
    sector = h1536 >> 8;
    f = h1536 & 255;
    p = v * (255 - s) / 255;
    q = v * (255 * 256 - s * f) / (255 * 256);
    t = v * (255 * 256 - s * (256 - f)) / (255 * 256);
    switch (sector) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

u32 PortCardHsvRgb(int h, int s, int v)
{
    int r, g, b;

    Hsv8(((h % 360 + 360) % 360) * 1536 / 360, s * 255 / 100, v * 255 / 100, &r, &g, &b);
    return (u32)((r << 16) | (g << 8) | b);
}

static u16 Bgr555(u32 rgb)
{
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;

    return (u16)(((r * 31 + 127) / 255) | (((g * 31 + 127) / 255) << 5) | (((b * 31 + 127) / 255) << 10));
}

static u32 CardPaintOf(const struct PortCard *c)
{
    switch (c->mode) {
    case PORT_CARD_SPRAY:  return PortPaintSpray(c->spray);
    case PORT_CARD_CUSTOM:
        if (c->body)
            return PortPaintPresetBody(c->body - 1);
        return PortPaintCustom(PortCardHsvRgb(c->hue, c->sat, c->val));
    default:               return PORT_PAINT_NONE;
    }
}

/* The feet go with a card by parts only: a spray (or the game's) is the
 * whole Kirby. */
static u32 CardFeetOf(const struct PortCard *c)
{
    s32 rgb;

    if (c->mode != PORT_CARD_CUSTOM || !c->feet || (rgb = PortPaintPresetRgb(c->feet - 1)) < 0)
        return 0;
    return PortPaintFeetOfRgb((u32)rgb);
}

u32 PortCardPaint(int p)
{
    return CardPaintOf(PortCardGet(p));
}

u32 PortCardFeet(int p)
{
    return CardFeetOf(PortCardGet(p));
}

const char *PortCardShownName(int p)
{
    static char buf[PORT_CARD_PLAYERS][PORT_CARD_NAME_MAX + 1];
    const struct PortCard *c = PortCardGet(p);

    if (c->name[0])
        return c->name;
    p = p >= 0 && p < PORT_CARD_PLAYERS ? p : 0;
    snprintf(buf[p], sizeof(buf[p]), "P%d", p + 1);
    return buf[p];
}

/* --- settings ---------------------------------------------------------------- */

/* cardN=MODE,SPRAY,HUE,SAT,VAL,NAME */
int PortCardParseSetting(const char *key, const char *val)
{
    struct PortCard c;
    unsigned mode, spray, hue, sat, v;
    int p, n = 0;

    if (strncmp(key, "card", 4) != 0 || key[4] < '0' || key[4] > '3')
        return 0;
    p = key[4] - '0';
    if (strcmp(key + 5, "parts") == 0) {
        /* cardNparts=BODY,FEET: after its card line */
        unsigned b = 0, f = 0;

        if (sscanf(val, "%u,%u", &b, &f) == 2) {
            sCards[p].body = (u8)(b <= PORT_PAINT_PRESETS ? b : 0);
            sCards[p].feet = (u8)(f <= PORT_PAINT_PRESETS ? f : 0);
        }
        return 1;
    }
    if (key[5] != '\0')
        return 0;
    memset(&c, 0, sizeof c);
    if (sscanf(val, "%u,%u,%u,%u,%u,%n", &mode, &spray, &hue, &sat, &v, &n) < 5)
        return 1;
    c.mode = (u8)mode;
    c.spray = (u8)spray;
    c.hue = (u16)hue;
    c.sat = (u8)(sat > 100 ? 100 : sat);
    c.val = (u8)(v > 100 ? 100 : v);
    if (n > 0)
        snprintf(c.name, sizeof(c.name), "%s", val + n);
    Clamp(&c);
    sCards[p] = c;
    return 1;
}

int PortCardFormatSettings(char *buf, int size)
{
    int p, n = 0;

    for (p = 0; p < PORT_CARD_PLAYERS && n < size; p++) {
        const struct PortCard *c = &sCards[p];
        int w;

        if (c->mode == PORT_CARD_GAME && !c->name[0] && !c->hue && !c->sat && !c->val && !c->body
            && !c->feet)
            continue;   /* untouched: nothing to keep */
        w = snprintf(buf + n, (size_t)(size - n), "card%d=%u,%u,%u,%u,%u,%s\n", p,
                     (unsigned)c->mode, (unsigned)c->spray, (unsigned)c->hue,
                     (unsigned)c->sat, (unsigned)c->val, c->name);
        if (w < 0 || w >= size - n)
            break;
        n += w;
        if (!c->body && !c->feet)
            continue;   /* a card from before the parts reads as it always did */
        w = snprintf(buf + n, (size_t)(size - n), "card%dparts=%u,%u\n", p, (unsigned)c->body,
                     (unsigned)c->feet);
        if (w < 0 || w >= size - n)
            break;
        n += w;
    }
    return n;
}

/* --- names ----------------------------------------------------------------------- */

CARD_EXPORT const char *PortPlayerName(int slot)
{
    if (slot < 0 || slot >= CARD_SLOTS)
        return "";
    /* online, a guest with the menus: the names the host sealed
     * (PORT_LN_EV_NAME), so every device shows a name from the same frame
     * on, through rewinds and images; else (none sealed: a host without
     * the menus) the host's relayed names */
    if (gPortLnOn && PortLnRole() == PORT_LN_GUEST && PortMenuPoc() && PortPocSlotName(slot)[0])
        return PortPocSlotName(slot);
    if (sSlotNamed[slot])
        return sSlotName[slot];
    if (gPortLnOn)
        return "";          /* online: the session names its players */
    if (slot >= PORT_CARD_PLAYERS || !PortLmpSlotIsHuman(slot))
        return "";          /* (a fifth person on one screen has no card) */
    return sCards[slot].name;   /* "" when the card has none */
}

CARD_EXPORT u32 PortPlayerColor(int slot)
{
    u32 v;
    s32 rgb;

    if (slot < 0 || slot >= CARD_SLOTS)
        return 0xFFFFFF;
    v = PortPaintOfSlot(slot);
    if ((v & PORT_PAINT_KIND_MASK) == PORT_PAINT_CUSTOM)
        return v & 0xFFFFFFu;
    rgb = PortPaintSprayRgb((v & PORT_PAINT_KIND_MASK) == PORT_PAINT_SPRAY ? (int)(v & 0xFF)
                                                                           : gKirbys[slot].color);
    return rgb >= 0 ? (u32)rgb : 0xFFFFFF;
}

CARD_EXPORT void PortCardSetSlotName(int slot, const char *name)
{
    if (slot < 0 || slot >= CARD_SLOTS)
        return;
    if (name == NULL || !name[0]) {
        sSlotNamed[slot] = 0;
        sSlotName[slot][0] = '\0';
        return;
    }
    snprintf(sSlotName[slot], sizeof(sSlotName[slot]), "%s", name);
    CleanName(sSlotName[slot]);
    sSlotNamed[slot] = 1;
}

CARD_EXPORT int PortPlayerTagAnchor(int slot, s32 *x, s32 *y, u16 *room)
{
    const struct Kirby *k;

    if (!gPortSettings.nameTags || slot < 0 || slot >= CARD_SLOTS || !PortLmpSlotPerson(slot))
        return 0;
    k = &gKirbys[slot];
    if (k->hp <= 0 || k->base.roomId == 0xFFFF || (k->base.flags & 0x100)
        || k->base.roomId != gKirbys[gLocalPlayerId].base.roomId)
        return 0;
    if (x) *x = k->base.x >> 8;
    if (y) *y = (k->base.y >> 8) - 11;     /* base.y is its middle: just over the head */
    if (room) *room = k->base.roomId;
    return 1;
}

/* The device setting (SETTINGS > NAME TAGS, ui/settings.h nameTags). */
CARD_EXPORT int PortCardTagsOn(void) { return gPortSettings.nameTags; }
CARD_EXPORT void PortCardSetTagsOn(int on)
{
    if (gPortSettings.nameTags == (on != 0))
        return;
    gPortSettings.nameTags = on != 0;
    PortSettingsChanged();
}

/* --- online hand-off ------------------------------------------------------------ */

/* Card p's profile: "NAME\tPAINT8", and "\tFEET4" when it has feet. */
static int ProfileOf(char *buf, size_t size, int p)
{
    u32 feet = PortCardFeet(p);

    if (feet)
        return snprintf(buf, size, "%s\t%08X\t%04X", sCards[p].name, (unsigned)PortCardPaint(p),
                        (unsigned)feet);
    return snprintf(buf, size, "%s\t%08X", sCards[p].name, (unsigned)PortCardPaint(p));
}

CARD_EXPORT const char *PortCardProfileString(void)
{
    static char buf[PORT_CARD_NAME_MAX + 24];

    ProfileOf(buf, sizeof buf, 0);
    return buf;
}

CARD_EXPORT int PortCardHostApplyPaint(int slot, u32 paint, u32 feet)
{
    if (!gPortLnOn || PortLnRole() != PORT_LN_HOST || slot < 0 || slot >= PAINT_SLOTS || !PortPaintValid(paint)
        || !PortPaintFeetValid(feet))
        return -1;
    return PortLnEvent(PORT_LN_EV_PAINT, slot, (int)paint, (int)feet, 0);
}

CARD_EXPORT int PortCardHostApplyProfile(int slot, const char *profile)
{
    char name[PORT_CARD_NAME_MAX + 1];
    const char *tab;
    unsigned paint = 0, feet = 0;
    size_t n;

    if (profile == NULL || (tab = strchr(profile, '\t')) == NULL)
        return -1;
    n = (size_t)(tab - profile);
    if (n > PORT_CARD_NAME_MAX)
        n = PORT_CARD_NAME_MAX;
    memcpy(name, profile, n);
    name[n] = '\0';
    if (sscanf(tab + 1, "%x\t%x", &paint, &feet) < 1)
        return -1;
    PortCardSetSlotName(slot, name);
    return PortCardHostApplyPaint(slot, paint, feet);
}

/* --- online: every slot's card, through the host ----------------------------------
 *
 * Each device sends the host the profiles of its four local seats (seat s
 * wears card s: seat 0 the device's own, seats 1..3 the people who play on
 * that screen) -- PortCardSeatProfiles, whenever it changes and after every
 * image.  The host keeps them by (device, seat) (PortCardHostSetProfiles,
 * device state on the host), and PortCardHostFrame seals the paint of every
 * slot from whoever owns it (PortLnSlotOwner): a CPU slot, or one whose
 * owner's profile has not arrived, wears the game's own.  The host names the
 * slots the same way, and the page relays the names (PortCardSlotNames on the
 * host, PortCardSetSlotNames on each guest).  Names never reach game memory. */

static void Changed(void);
static void EditFollows(int p);

#define PROF_OWNERS (PORT_LN_MAX_DEV * 4)
static char sProfName[PROF_OWNERS][PORT_CARD_NAME_MAX + 1];
static u32 sProfPaint[PROF_OWNERS], sProfFeet[PROF_OWNERS];

/* One profile ("NAME\tPAINT8[\tFEET4]"), the paint 0 if it is not one. */
static void ParseProfile(const char *pr, char name[PORT_CARD_NAME_MAX + 1], u32 *paint, u32 *feet)
{
    const char *tab = pr ? strchr(pr, '\t') : NULL;
    unsigned v = 0, f = 0;
    size_t n;

    name[0] = '\0';
    *paint = PORT_PAINT_NONE;
    *feet = 0;
    if (tab == NULL)
        return;
    n = (size_t)(tab - pr);
    if (n > PORT_CARD_NAME_MAX)
        n = PORT_CARD_NAME_MAX;
    memcpy(name, pr, n);
    name[n] = '\0';
    CleanName(name);
    if (sscanf(tab + 1, "%8x\t%4x", &v, &f) >= 1 && PortPaintValid(v)) {
        *paint = v;
        *feet = v != PORT_PAINT_NONE && PortPaintFeetValid(f) ? f : 0;
    }
}

CARD_EXPORT const char *PortCardSeatProfiles(void)
{
    static char buf[4 * (PORT_CARD_NAME_MAX + 18)];
    int s, n = 0;

    for (s = 0; s < 4; s++) {
        if (s)
            buf[n++] = '\n';
        n += ProfileOf(buf + n, sizeof(buf) - (size_t)n, s);
    }
    return buf;
}

CARD_EXPORT void PortCardHostSetProfiles(int dev, const char *profiles)
{
    const char *p = profiles;
    int s;

    if (dev < 1 || dev >= PORT_LN_MAX_DEV || profiles == NULL)
        return;
    for (s = 0; s < 4; s++) {
        char one[PORT_CARD_NAME_MAX + 24];
        const char *nl = p ? strchr(p, '\n') : NULL;
        size_t n = p ? (nl ? (size_t)(nl - p) : strlen(p)) : 0;

        if (n >= sizeof(one))
            n = sizeof(one) - 1;
        if (p)
            memcpy(one, p, n);
        one[n] = '\0';
        ParseProfile(one, sProfName[dev * 4 + s], &sProfPaint[dev * 4 + s], &sProfFeet[dev * 4 + s]);
        p = nl ? nl + 1 : NULL;
    }
}

CARD_EXPORT void PortCardHostDropDevice(int dev)
{
    if (dev < 1 || dev >= PORT_LN_MAX_DEV)
        return;
    memset(sProfName[dev * 4], 0, sizeof(sProfName[0]) * 4);
    memset(&sProfPaint[dev * 4], 0, sizeof(sProfPaint[0]) * 4);
    memset(&sProfFeet[dev * 4], 0, sizeof(sProfFeet[0]) * 4);
}

/* Host: what slot s's owner's card says (the host's own seats: its cards). */
static u32 OwnerPaint(int own, const char **name, u32 *feet)
{
    *feet = 0;
    if (own < 0) {
        *name = "";
        return PORT_PAINT_NONE;
    }
    if (own < 4) {
        *name = sCards[own].name;
        *feet = PortCardFeet(own);
        return PortCardPaint(own);
    }
    if (own >= PROF_OWNERS) {
        *name = "";
        return PORT_PAINT_NONE;
    }
    *name = sProfName[own];
    *feet = sProfFeet[own];
    return sProfPaint[own];
}

CARD_EXPORT const char *PortCardSlotNames(void)
{
    static char buf[CARD_SLOTS * (PORT_CARD_NAME_MAX + 1) + 4];
    int s, n = 0;

    for (s = 0; s < CARD_SLOTS; s++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s", s ? "\t" : "", PortPlayerName(s));
    return buf;
}

CARD_EXPORT void PortCardSetSlotNames(const char *names)
{
    const char *p = names;
    int s;

    for (s = 0; s < CARD_SLOTS; s++) {
        char one[PORT_CARD_NAME_MAX + 1];
        const char *tab = p ? strchr(p, '\t') : NULL;
        size_t n = p ? (tab ? (size_t)(tab - p) : strlen(p)) : 0;

        if (n > PORT_CARD_NAME_MAX)
            n = PORT_CARD_NAME_MAX;
        if (p)
            memcpy(one, p, n);
        one[n] = '\0';
        PortCardSetSlotName(s, one);
        p = tab ? tab + 1 : NULL;
    }
}

/* The end of a session: no slot is named by it any more, no profiles kept. */
CARD_EXPORT void PortCardSessionEnd(void)
{
    int s;

    for (s = 0; s < CARD_SLOTS; s++)
        PortCardSetSlotName(s, NULL);
    memset(sProfName, 0, sizeof(sProfName));
    memset(sProfPaint, 0, sizeof(sProfPaint));
    memset(sProfFeet, 0, sizeof(sProfFeet));
}

/* Tests: the derivation's unit checks (kirby_paint.c), failures. */
CARD_EXPORT int PortCardPaintSelfTestJs(void) { return PortPaintSelfTest(); }

/* Tests: slot s's paint and feet as the simulation has them. */
CARD_EXPORT u32 PortCardSlotPaintJs(int s) { return PortPaintOfSlot(s); }
CARD_EXPORT u32 PortCardSlotFeetJs(int s) { return PortPaintFeetOfSlot(s); }

/* Tests and the console: set card p from a settings value
 * ("MODE,SPRAY,HUE,SAT,VAL,NAME", then "|BODY,FEET" for its parts), not
 * saved. */
CARD_EXPORT int PortCardSetJs(int p, const char *val)
{
    char key[11] = "card0", one[64];
    const char *bar;
    size_t n;

    if (p < 0 || p >= PORT_CARD_PLAYERS || val == NULL)
        return 0;
    key[4] = (char)('0' + p);
    bar = strchr(val, '|');
    n = bar ? (size_t)(bar - val) : strlen(val);
    if (n >= sizeof one)
        n = sizeof one - 1;
    memcpy(one, val, n);
    one[n] = '\0';
    PortCardParseSetting(key, one);
    if (bar) {
        snprintf(key + 5, sizeof key - 5, "parts");
        PortCardParseSetting(key, bar + 1);
    }
    EditFollows(p);
    Changed();
    return 1;
}

/* --- the host's frame hook ----------------------------------------------------- */

static int sEditing;
/* What was asked of the simulation and when, so a paint waiting for its
 * latch is not asked for again every frame. */
static u32 sAsked[PAINT_SLOTS], sAskedFeet[PAINT_SLOTS], sAskedAt[PAINT_SLOTS];
static int sAskedValid[PAINT_SLOTS];

static void Ask(int s, u32 want, u32 feet)
{
    u32 f;

    if (want == PORT_PAINT_NONE)
        feet = 0;
    if (gPortLnOn) {
        int r = PortLnEvent(PORT_LN_EV_PAINT, s, (int)want, (int)feet, 0);

        if (r < 0)
            return;
        f = (u32)r;
    } else {
        f = PortInputLogSchedulePaint((u8)s, want, feet);
        if (f == 0 && PortFrameNumber() > 1)
            return;
    }
    sAsked[s] = want;
    sAskedFeet[s] = feet;
    sAskedAt[s] = f;
    sAskedValid[s] = 1;
}

/* Does slot s wear (want, feet) already, or is it on its way? */
static int Worn(int s, u32 want, u32 feet)
{
    if (want == PORT_PAINT_NONE)
        feet = 0;
    if (PortPaintOfSlot(s) == want && PortPaintFeetOfSlot(s) == feet) {
        sAskedValid[s] = 0;
        return 1;
    }
    return sAskedValid[s] && sAsked[s] == want && sAskedFeet[s] == feet
        && PortFrameNumber() <= sAskedAt[s] + 2;
}

void PortCardHostFrame(void)
{
    int s, n;

    {
        static int sheet = -1;

        if (sheet < 0)
            sheet = getenv("KATAM_PAINT_SHEET") != NULL;
        if (sheet)
            PortCardDebugSheet();
    }
    {
        /* KATAM_PAINT_SELFTEST=1: the derivation's unit checks once the ROM
         * is up, then exit (status = failures) */
        static int test = -1;

        if (test < 0)
            test = getenv("KATAM_PAINT_SELFTEST") != NULL;
        if (test && gSpriteTables != NULL) {
            int fails = PortPaintSelfTest();

            exit(fails ? 1 : 0);
        }
    }
    if (sSavePending && !PortLnOverlayRunning())
        Save();                 /* an edit on the pause overlay, saved now */
    if ((sEditing && !gPortLnOn) || PortInputLogReplaying() || PortRbActive() || PortNetActive() || PortSeatActive())
        return;
    if (gPortLnOn) {
        /* online: every slot from its owner's card (the host's own seats
         * from its cards, the guests' from the profiles they sent) */
        if (PortLnRole() != PORT_LN_HOST || PortLnCatchingUp())
            return;
        for (s = 0; s < CARD_SLOTS; s++) {
            const char *name;
            int own = PortLnSlotOwner(s);
            u32 feet, want = OwnerPaint(own, &name, &feet);

            if (strcmp(PortPlayerName(s), name) != 0)
                PortCardSetSlotName(s, name);
            if (s >= PAINT_SLOTS)
                continue;       /* names only: paint is four slots wide */
            /* (a card being edited on the host's pause overlay goes on
             * live, a step at a time, as a guest's does: each step that
             * changes the card is one sealed event) */
            if (!Worn(s, want, feet))
                Ask(s, want, feet);
        }
        return;
    }
    n = 4;
    for (s = 0; s < n; s++) {
        int person = s == 0 || PortLmpSlotIsHuman(s);
        u32 want = person ? PortCardPaint(s) : PORT_PAINT_NONE;
        u32 feet = person ? PortCardFeet(s) : 0;

        if (!Worn(s, want, feet))
            Ask(s, want, feet);
    }
}

/* --- the screens' model ------------------------------------------------------- */

enum { R_PLAYER, R_NAME, R_COLOR };
enum { R_BODY, R_FEET };

static int sSub, sPlayer, sFixed, sNamePos;
/* An invite link's card (PortPlayerCardOpenInvite): row 0 is JOIN. */
static int sInvite;
static char sInviteCode[5];
/* The NAME row's typing state: entered with A on the row, left with Start
 * (Enter on a keyboard), Escape, B at the first letter, Up/Down, or A past
 * the last letter.  Only while it is on do Left/Right pick letters and does
 * the web page send a keyboard's letters here; otherwise the row is a row
 * like the others and every key is the game's. */
static int sTyping;
static char sTypedFrom[PORT_CARD_NAME_MAX + 1];   /* the name as typing began */
static struct PortCard sEdit;       /* the card being edited                */
static u32 sGen = 1;

/* The PICK screen's grid: open (the d-pad is its cursor), the cell under
 * the cursor, and the part as it was when the grid opened (B puts it back). */
static int sGridOpen, sCell;
static struct PortCard sGridFrom;

static void Changed(void)
{
    sGen++;
}

/* Card p was set from outside the screens (PortCardSetJs): if it is the one
 * open, the screen shows -- and on closing keeps -- the new card. */
static void EditFollows(int p)
{
    if (sSub != PORT_CARD_SUB_NONE && sPlayer == p)
        sEdit = sCards[p];
}

static void Load(int p)
{
    sTyping = 0;
    sGridOpen = 0;
    sPlayer = p;
    sEdit = sCards[p];
    sNamePos = (int)strlen(sEdit.name);
    if (sNamePos >= PORT_CARD_NAME_MAX)
        sNamePos = PORT_CARD_NAME_MAX - 1;
    if (sEdit.mode != PORT_CARD_CUSTOM && sEdit.sat == 0 && sEdit.val == 0) {
        sEdit.hue = 330; sEdit.sat = 35; sEdit.val = 100;   /* Kirby pink-ish */
    }
}

/* Every edit lands at once: the card is saved, so a reload keeps it. */
static void Store(void)
{
    struct PortCard c = sEdit;

    Clamp(&c);
    sCards[sPlayer] = c;
    Save();
    Changed();
}

CARD_EXPORT void PortPlayerCardOpen(int p, int fixed)
{
    if (p < 0 || p >= PORT_CARD_PLAYERS)
        p = 0;
    Load(p);
    sFixed = fixed != 0;
    sSub = PORT_CARD_SUB_CARD;
    sEditing = 1;
    Changed();
}

/* The invite's card: this device's own, the cursor on JOIN (row 0). */
void PortPlayerCardOpenInvite(const char code[4])
{
    PortPlayerCardOpen(0, 1);
    memcpy(sInviteCode, code, 4);
    sInviteCode[4] = '\0';
    sInvite = 1;
}

int PortCardInvite(void) { return sSub != PORT_CARD_SUB_NONE && sInvite; }

CARD_EXPORT void PortPlayerCardClose(void)
{
    if (sGridOpen)
        sEdit = sGridFrom;      /* a cell only looked at is not kept */
    sGridOpen = 0;
    if (sSub != PORT_CARD_SUB_NONE)
        Store();
    sSub = PORT_CARD_SUB_NONE;
    sEditing = 0;
    sTyping = 0;
    sInvite = 0;
    Changed();
}

CARD_EXPORT int PortCardSub(void) { return sSub; }
int PortCardPlayer(void) { return sPlayer; }
u32 PortCardGen(void) { return sGen; }

const char *PortCardHeader(int sub)
{
    return sub == PORT_CARD_SUB_PICK ? "COLOR" : "PLAYER CARD";
}

/* The preset a card's part is (0..31), -1 for none: a feet as the body,
 * an old card's own colour. */
static int PartPreset(const struct PortCard *c, int row)
{
    if (row == R_FEET)
        return c->feet ? c->feet - 1 : -1;
    return c->body ? c->body - 1 : -1;
}

const char *PortCardRowName(int sub, int row)
{
    if (sub == PORT_CARD_SUB_PICK)
        return row == R_BODY ? "BODY" : "FEET";
    if (row == R_PLAYER && sInvite)
        return "JOIN GAME";
    return row == R_PLAYER ? "PLAYER" : row == R_NAME ? "NAME" : "COLOR";
}

static const char *ColorName(const struct PortCard *c)
{
    switch (c->mode) {
    case PORT_CARD_SPRAY:  return kSprayName[c->spray % PORT_PAINT_COLORS];
    case PORT_CARD_CUSTOM: return "CUSTOM";
    default:               return "GAME";
    }
}

void PortCardRowValue(int sub, int row, int selected, char *buf, int size)
{
    buf[0] = '\0';
    if (sub == PORT_CARD_SUB_PICK) {
        int k = PartPreset(&sEdit, row);
        const char *n = k >= 0 ? PortPaintPresetName(k) : row == R_FEET ? "BODY" : "OWN";

        snprintf(buf, size, selected && !sGridOpen ? "<%s>" : "%s", n);
        return;
    }
    switch (row) {
    case R_PLAYER:
        if (sInvite)
            snprintf(buf, size, "%s", sInviteCode);     /* the game's code */
        else
            snprintf(buf, size, selected && !sFixed ? "<P%d>" : "P%d", sPlayer + 1);
        break;
    case R_NAME: {
        /* the letters so far, the one being picked between arrows */
        int i, o = 0, len = (int)strlen(sEdit.name);

        for (i = 0; i < PORT_CARD_NAME_MAX && o < size - 4; i++) {
            char c = i < len ? sEdit.name[i] : ' ';

            if (selected && sTyping && i == sNamePos) {
                buf[o++] = '<';
                buf[o++] = c == ' ' ? '-' : c;
                buf[o++] = '>';
            } else if (i < len) {
                buf[o++] = c == ' ' ? '-' : c;
            }
        }
        buf[o] = '\0';
        if (o == 0)
            snprintf(buf, size, "%s", PortCardShownName(sPlayer));
        break;
    }
    case R_COLOR:
        snprintf(buf, size, selected ? "<%s>" : "%s", ColorName(&sEdit));
        break;
    }
}

/* Description lines: letters the game's description pictures have where
 * possible (no J, Q, Z, digits); others are drawn in the fallback font. */
const char *PortCardRowDesc(int sub, int row)
{
    if (sub == PORT_CARD_SUB_PICK) {
        int odd = (gPortSim->frame.frameCount / 120) & 1;

        if (sGridOpen)
            return odd ? "B PUTS IT BACK" : "PICK A COLOR - A KEEPS IT";
        if (row == R_BODY)
            return odd ? "LEFT RIGHT STEP THROUGH" : "A PICKS FROM THE COLORS";
        return odd ? "BODY - SHADED LIKE THE BODY" : "THE FEET AND THE CHEEKS";
    }
    switch (row) {
    case R_PLAYER:
        if (sInvite)    /* alternate: what A does, and what the card is for */
            return (gPortSim->frame.frameCount / 120) & 1 ? "B - NOT NOW" : "A JOINS - YOUR CARD BELOW";
        return sFixed ? "WHOSE CARD THIS IS" : "LEFT RIGHT PICK A PLAYER";
    case R_NAME:
        if (!sTyping)
            return "A TO TYPE A NAME";
        /* alternate, about every two seconds: how to finish, how to cancel */
        return (gPortSim->frame.frameCount / 120) & 1 ? "B ON FIRST LETTER CANCELS"
                                                      : "TYPE OR PICK - START DONE";
    default:
        return sEdit.mode == PORT_CARD_GAME ? "THE GAMES OWN SPRAY PAINT"
             : "A PICKS BODY AND FEET";
    }
}

static void NameStep(int dir)
{
    char *n = sEdit.name;
    int len = (int)strlen(n), c, i;

    while (len < sNamePos) {            /* fill a gap with spaces */
        n[len++] = ' ';
        n[len] = '\0';
    }
    c = sNamePos < len ? n[sNamePos] : ' ';
    for (i = 0; i < NNAME && kNameChars[i] != c; i++)
        ;
    if (i >= NNAME)
        i = 0;
    i = (i + dir + NNAME) % NNAME;
    if (sNamePos >= len) {
        n[sNamePos] = kNameChars[i];
        n[sNamePos + 1] = '\0';
    } else {
        n[sNamePos] = kNameChars[i];
    }
    /* trailing spaces end the name */
    len = (int)strlen(n);
    while (len > 0 && n[len - 1] == ' ')
        n[--len] = '\0';
}

static void ColorStep(int dir)
{
    /* GAME, the fourteen, CUSTOM, round */
    int k = sEdit.mode == PORT_CARD_GAME ? 0 : sEdit.mode == PORT_CARD_SPRAY ? 1 + sEdit.spray
          : 1 + PORT_PAINT_COLORS;

    k = (k + dir + PORT_PAINT_COLORS + 2) % (PORT_PAINT_COLORS + 2);
    if (k == 0) {
        sEdit.mode = PORT_CARD_GAME;
    } else if (k <= PORT_PAINT_COLORS) {
        sEdit.mode = PORT_CARD_SPRAY;
        sEdit.spray = (u8)(k - 1);
    } else {
        sEdit.mode = PORT_CARD_CUSTOM;
    }
}

/* The preset whose colour is nearest rgb (for a cursor with no cell of its
 * own to start on). */
static int NearestPreset(u32 rgb)
{
    int i, best = 0;
    long bestD = -1;

    for (i = 0; i < PORT_PAINT_PRESETS; i++) {
        s32 c = PortPaintPresetRgb(i);
        long dr, dg, db, d;

        if (c < 0)
            continue;
        dr = (long)((c >> 16) & 255) - (long)((rgb >> 16) & 255);
        dg = (long)((c >> 8) & 255) - (long)((rgb >> 8) & 255);
        db = (long)(c & 255) - (long)(rgb & 255);
        d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (bestD < 0 || d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

/* The preset that is spray k (every spray has one). */
static int SprayPreset(int k)
{
    int i;

    for (i = 0; i < PORT_PAINT_PRESETS; i++)
        if (PortPaintPresetBody(i) == PortPaintSpray(k))
            return i;
    return 0;
}

/* The picker opens on the colour shown now: a spray (or the game's own,
 * the slot's spray) is its preset, feet and all; a card by parts as it is. */
static void PickFromCurrent(void)
{
    if (sEdit.mode == PORT_CARD_SPRAY || sEdit.mode == PORT_CARD_GAME) {
        sEdit.body = (u8)(1 + SprayPreset(sEdit.mode == PORT_CARD_SPRAY ? sEdit.spray
                                                                       : sPlayer % PORT_PAINT_COLORS));
        sEdit.feet = 0;
    }
    sEdit.mode = PORT_CARD_CUSTOM;
}

/* Row `row`'s part steps through its choices: the presets, and for the
 * feet first "as the body". */
static void PartStep(int row, int dir)
{
    if (row == R_FEET) {
        sEdit.feet = (u8)((sEdit.feet + dir + PORT_PAINT_PRESETS + 1) % (PORT_PAINT_PRESETS + 1));
    } else if (sEdit.body == 0) {
        /* an old card's own colour: the first step is to the preset nearest it */
        sEdit.body = (u8)(1 + NearestPreset(CardPaintOf(&sEdit) & 0xFFFFFFu));
    } else {
        sEdit.body = (u8)(1 + (sEdit.body - 1 + dir + PORT_PAINT_PRESETS) % PORT_PAINT_PRESETS);
    }
    sEdit.mode = PORT_CARD_CUSTOM;
}

static void GridPut(int row)
{
    if (row == R_FEET)
        sEdit.feet = (u8)(1 + sCell);
    else
        sEdit.body = (u8)(1 + sCell);
    sEdit.mode = PORT_CARD_CUSTOM;
}

static void GridOpen(int row)
{
    int k = PartPreset(&sEdit, row);

    if (k < 0)      /* no cell of its own: start on the body's, or the nearest */
        k = sEdit.body ? sEdit.body - 1 : NearestPreset(CardPaintOf(&sEdit) & 0xFFFFFFu);
    sCell = k;
    sGridFrom = sEdit;
    sGridOpen = 1;
}

CARD_EXPORT int PortCardKey(int *row, unsigned key)
{
    /* The screens give A and Start as one key (1); Start alone is the
     * typing state's "done". */
    if (key == 1 && (gPressedKeys & START_BUTTON) && !(gPressedKeys & A_BUTTON))
        key = PORT_CARD_KEY_START;
    switch (sSub) {
    case PORT_CARD_SUB_CARD:
        if (key == 3 || key == 4) {
            sTyping = 0;
            *row = (*row + (key == 4 ? 1 : PORT_CARD_ROWS - 1)) % PORT_CARD_ROWS;
            Changed();
            return PORT_CARD_K_CURSOR;
        }
        if (*row == R_NAME && sTyping && key == PORT_CARD_KEY_START) {
            sTyping = 0;            /* done: the row is a row again */
            Changed();
            return PORT_CARD_K_SELECT;
        }
        if (key == PORT_CARD_KEY_START)
            key = 1;
        if (key == 2) {
            if (*row == R_NAME && sTyping) {
                if (sNamePos > 0) {
                    sNamePos--;
                } else {
                    /* B at the first letter: cancel -- the name as it was */
                    snprintf(sEdit.name, sizeof sEdit.name, "%s", sTypedFrom);
                    sTyping = 0;
                    Store();
                }
                Changed();
                return PORT_CARD_K_CURSOR;
            }
            PortPlayerCardClose();
            return PORT_CARD_K_CLOSE;
        }
        switch (*row) {
        case R_PLAYER:
            if (sInvite) {
                if (key != 1)
                    return PORT_CARD_K_QUIET;
                PortPlayerCardClose();  /* kept (saved): then the join */
                return PORT_CARD_K_JOIN;
            }
            if (sFixed || (key != 5 && key != 6 && key != 1))
                return key == 1 ? PORT_CARD_K_BACK : PORT_CARD_K_QUIET;
            Store();
            Load((sPlayer + (key == 5 ? PORT_CARD_PLAYERS - 1 : 1)) % PORT_CARD_PLAYERS);
            Changed();
            return PORT_CARD_K_CURSOR;
        case R_NAME:
            if (!sTyping) {
                if (key != 1)
                    return PORT_CARD_K_NONE;
                sTyping = 1;            /* A: type (pick letters) from here */
                snprintf(sTypedFrom, sizeof sTypedFrom, "%s", sEdit.name);
                sNamePos = (int)strlen(sEdit.name);
                if (sNamePos >= PORT_CARD_NAME_MAX)
                    sNamePos = PORT_CARD_NAME_MAX - 1;
                Changed();
                return PORT_CARD_K_SELECT;
            }
            if (key == 5 || key == 6) {
                NameStep(key == 6 ? 1 : -1);
                Store();
                return PORT_CARD_K_CURSOR;
            }
            if (key == 1) {
                if (sNamePos < PORT_CARD_NAME_MAX - 1 && sNamePos < (int)strlen(sEdit.name)) {
                    sNamePos++;
                    Changed();
                    return PORT_CARD_K_CURSOR;
                }
                sTyping = 0;
                *row = R_COLOR;          /* the name is done */
                Changed();
                return PORT_CARD_K_SELECT;
            }
            return PORT_CARD_K_NONE;
        case R_COLOR:
            if (key == 5 || key == 6) {
                ColorStep(key == 6 ? 1 : -1);
                Store();
                return PORT_CARD_K_CURSOR;
            }
            if (key == 1) {
                if (sEdit.mode != PORT_CARD_CUSTOM)
                    PickFromCurrent();
                Store();
                sSub = PORT_CARD_SUB_PICK;
                sGridOpen = 0;
                *row = R_BODY;
                Changed();
                return PORT_CARD_K_SELECT;
            }
            return PORT_CARD_K_NONE;
        }
        return PORT_CARD_K_NONE;

    case PORT_CARD_SUB_PICK:
        if (key == PORT_CARD_KEY_START)
            key = 1;
        if (sGridOpen) {
            int c = sCell % PORT_PAINT_PRESET_COLS, r = sCell / PORT_PAINT_PRESET_COLS;
            const int rows = PORT_PAINT_PRESETS / PORT_PAINT_PRESET_COLS;

            switch (key) {
            case 1:             /* keep it */
                sGridOpen = 0;
                Store();
                return PORT_CARD_K_SELECT;
            case 2:             /* put back what it was */
                sEdit = sGridFrom;
                sGridOpen = 0;
                Store();
                return PORT_CARD_K_BACK;
            case 3: r = (r + rows - 1) % rows; break;
            case 4: r = (r + 1) % rows; break;
            case 5: c = (c + PORT_PAINT_PRESET_COLS - 1) % PORT_PAINT_PRESET_COLS; break;
            case 6: c = (c + 1) % PORT_PAINT_PRESET_COLS; break;
            default: return PORT_CARD_K_NONE;
            }
            sCell = r * PORT_PAINT_PRESET_COLS + c;
            GridPut(*row);
            Changed();          /* the preview wears it; saved when kept */
            return PORT_CARD_K_CURSOR;
        }
        if (key == 3 || key == 4) {
            *row = (*row + (key == 4 ? 1 : PORT_CARD_PICK_ROWS - 1)) % PORT_CARD_PICK_ROWS;
            Changed();
            return PORT_CARD_K_CURSOR;
        }
        if (key == 2) {
            Store();
            sSub = PORT_CARD_SUB_CARD;
            *row = R_COLOR;
            Changed();
            return PORT_CARD_K_BACK;
        }
        if (key == 1) {
            GridOpen(*row);
            Changed();
            return PORT_CARD_K_SELECT;
        }
        if (key == 5 || key == 6) {
            PartStep(*row, key == 6 ? 1 : -1);
            Store();
            return PORT_CARD_K_CURSOR;
        }
        return PORT_CARD_K_NONE;
    }
    return PORT_CARD_K_NONE;
}

int PortCardPreviewPalette(u16 out[16])
{
    return PortPaintBodyPalette(CardPaintOf(&sEdit), CardFeetOf(&sEdit), sPlayer, out);
}

static u16 Bgr555Of(s32 rgb)
{
    return rgb < 0 ? 0 : Bgr555((u32)rgb);
}

int PortCardGrid(int row, u16 out[], int *open)
{
    int i;

    for (i = 0; i < PORT_PAINT_PRESETS; i++)
        out[i] = Bgr555Of(PortPaintPresetRgb(i));
    if (open)
        *open = sGridOpen;
    return sGridOpen ? sCell : PartPreset(&sEdit, row);
}

u16 PortCardPartColor(int row)
{
    u16 pal[16];

    if (!PortCardPreviewPalette(pal))
        return 0;
    return row == R_FEET ? pal[9] : pal[3];     /* the feet's light shade, the body's fill */
}

/* Tests: the card being edited -- 0 its paint, 1 its feet, 2 the grid open,
 * 3 the grid's cursor. */
CARD_EXPORT u32 PortCardEditJs(int what)
{
    switch (what) {
    case 0:  return CardPaintOf(&sEdit);
    case 1:  return CardFeetOf(&sEdit);
    case 2:  return (u32)sGridOpen;
    default: return (u32)sCell;
    }
}

/* --- typed names (the web page) ------------------------------------------------- */

static int sCursorRow;      /* the screen's row, as the last key left it */

CARD_EXPORT int PortCardWantsText(void)
{
    return sSub == PORT_CARD_SUB_CARD && sCursorRow == R_NAME;
}

/* The typing state is on (A on the NAME row): the web page's keyboard types
 * into the name; the game's keys are not sent letters then. */
CARD_EXPORT int PortCardTyping(void)
{
    return sSub == PORT_CARD_SUB_CARD && sCursorRow == R_NAME && sTyping;
}

/* Enter or Escape on a keyboard: the typing state ends, the name kept. */
CARD_EXPORT void PortCardTypingEnd(void)
{
    if (!sTyping)
        return;
    sTyping = 0;
    Changed();
}

/* The screen tells us its row (menu_poc.c keeps the cursor). */
void PortCardNoteRow(int row)
{
    sCursorRow = row;
}

CARD_EXPORT void PortCardTypeChar(int c)
{
    int len;

    if (sSub != PORT_CARD_SUB_CARD || !sTyping)
        return;
    len = (int)strlen(sEdit.name);
    if (c == 8) {
        if (len > 0)
            sEdit.name[len - 1] = '\0';
        len = (int)strlen(sEdit.name);
    } else {
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (!NameCharOk(c) || len >= PORT_CARD_NAME_MAX || (c == ' ' && len == 0))
            return;
        sEdit.name[len++] = (char)c;
        sEdit.name[len] = '\0';
    }
    sNamePos = len < PORT_CARD_NAME_MAX ? len : PORT_CARD_NAME_MAX - 1;
    Store();
}

CARD_EXPORT void PortCardSetNameJs(const char *name)
{
    if (sSub == PORT_CARD_SUB_NONE || name == NULL)
        return;
    snprintf(sEdit.name, sizeof(sEdit.name), "%s", name);
    CleanName(sEdit.name);
    sNamePos = (int)strlen(sEdit.name);
    if (sNamePos >= PORT_CARD_NAME_MAX)
        sNamePos = PORT_CARD_NAME_MAX - 1;
    Store();
}

CARD_EXPORT const char *PortCardNameJs(void)
{
    return sSub != PORT_CARD_SUB_NONE ? sEdit.name : sCards[0].name;
}

/* --- the debug sheet ------------------------------------------------------------- */

#if !defined(__EMSCRIPTEN__)
int PortNativeWritePng(const char *path, const u32 *rgba, int w, int h);

void PortCardDebugSheet(void)
{
    static int done;
    /* the sprays, then a sweep of custom colours */
    enum { CELL = 40, COLS = 14, ROWS_ = 11 };
    static u32 img[CELL * COLS * CELL * ROWS_];
    u8 px[64 * 64];
    int fw, fh = 0, r, c;
    const char *path = getenv("KATAM_PAINT_SHEET");

    if (done || gSpriteTables == NULL || path == NULL)
        return;
    done = 1;
    fw = PortPocDecodeFrame(0, 0, px, 64, 64, &fh);
    memset(img, 0x40, sizeof img);
    for (r = 0; r < ROWS_; r++)
        for (c = 0; c < COLS; c++) {
            u16 pal[16];
            u32 v, feet = 0;
            int x, y, i;

            if (r == 0)
                v = PortPaintSpray(c);
            else if (r < 4)       /* hues at three brightnesses */
                v = PortPaintCustom(PortCardHsvRgb(c * 360 / COLS, r == 1 ? 45 : 85, r == 3 ? 60 : 100));
            else if (r == 4)      /* greys and darks */
                v = PortPaintCustom(PortCardHsvRgb(c * 360 / COLS, c % 2 ? 10 : 0, 20 + c * 6));
            else if (r == 5)      /* each spray's own fill, as a custom colour: must equal row 0 */
                v = PortPaintCustom((u32)PortPaintSprayRgb(c));
            else if (r == 6)      /* very dark */
                v = PortPaintCustom(PortCardHsvRgb(c * 360 / COLS, 90, 30));
            else if (r == 7)      /* pastels */
                v = PortPaintCustom(PortCardHsvRgb(c * 360 / COLS, 20, 100));
            else {                /* feet of their own: the presets on a pink spray, a */
                v = r == 8 ? PortPaintSpray(0)      /* custom blue, a preset grey   */
                  : r == 9 ? PortPaintCustom(0x3060E0) : PortPaintPresetBody(29);
                feet = PortPaintFeetOfRgb((u32)PortPaintPresetRgb((c + (r - 8) * 9) % PORT_PAINT_PRESETS));
            }
            if (!PortPaintBodyPalette(v, feet, 0, pal))
                continue;
            /* the palette as a strip on top, then the Kirby */
            for (i = 0; i < 16; i++)
                for (y = 0; y < 4; y++)
                    for (x = 0; x < 2; x++) {
                        u16 e = pal[i];
                        u32 rr = (u32)((e & 31) * 255 / 31), gg = (u32)(((e >> 5) & 31) * 255 / 31),
                            bb = (u32)(((e >> 10) & 31) * 255 / 31);

                        img[(r * CELL + y) * CELL * COLS + c * CELL + 4 + i * 2 + x] =
                            0xFF000000u | (bb << 16) | (gg << 8) | rr;
                    }
            for (y = 0; y < fh && y < CELL - 6; y++)
                for (x = 0; x < fw && x < CELL; x++) {
                    u8 k = px[y * 64 + x];
                    u16 e;
                    u32 rr, gg, bb;

                    if (!k)
                        continue;
                    e = pal[k];
                    rr = (u32)((e & 31) * 255 / 31);
                    gg = (u32)(((e >> 5) & 31) * 255 / 31);
                    bb = (u32)(((e >> 10) & 31) * 255 / 31);
                    img[(r * CELL + 6 + y) * CELL * COLS + c * CELL + x + (CELL - fw) / 2] =
                        0xFF000000u | (bb << 16) | (gg << 8) | rr;
                }
        }
    PortNativeWritePng(path, img, CELL * COLS, CELL * ROWS_);
    PortLog("[katam-port] paint sheet: %s (standing frame %dx%d)", path, fw, fh);
}
#else
void PortCardDebugSheet(void) {}
#endif

#ifdef __cplusplus
}
#endif
