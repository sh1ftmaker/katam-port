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
 *   online:  the session layer carries profiles (PortCardProfileString on
 *            the guest, PortCardHostApplyProfile on the host) and names the
 *            slots (PortCardSetSlotName); this file only keeps the host's
 *            own slot 0 in step.
 *
 * While a card is open for editing nothing is scheduled: the picker's every
 * step would otherwise be an event; the result goes in when the card closes.
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
 * behind COLOR's A, PICK (HUE, VIVID, BRIGHT), with a live preview of
 * the Kirby and colour bars drawn beside them.  This file is their model:
 * text, keys, the edited card.  Host memory, front end only. */

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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define CARD_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CARD_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

static struct PortCard sCards[PORT_CARD_PLAYERS];
static char sSlotName[4][PORT_CARD_NAME_MAX + 1];
static int sSlotNamed[4];
static int sTagsOn = 1;

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
}

const struct PortCard *PortCardGet(int p)
{
    return &sCards[p >= 0 && p < PORT_CARD_PLAYERS ? p : 0];
}

static void Save(void)
{
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

/* 0xRRGGBB -> h 0..359, s and v 0..100 (for opening the picker on a spray). */
static void RgbHsv(u32 rgb, u16 *h, u8 *s, u8 *v)
{
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn, hh = 0;

    if (d) {
        if (mx == r)      hh = (g - b) * 60 / d;
        else if (mx == g) hh = 120 + (b - r) * 60 / d;
        else              hh = 240 + (r - g) * 60 / d;
    }
    *h = (u16)((hh % 360 + 360) % 360);
    *s = (u8)(mx ? d * 100 / mx : 0);
    *v = (u8)(mx * 100 / 255);
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
    case PORT_CARD_CUSTOM: return PortPaintCustom(PortCardHsvRgb(c->hue, c->sat, c->val));
    default:               return PORT_PAINT_NONE;
    }
}

u32 PortCardPaint(int p)
{
    return CardPaintOf(PortCardGet(p));
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

    if (strncmp(key, "card", 4) != 0 || key[4] < '0' || key[4] > '3' || key[5] != '\0')
        return 0;
    p = key[4] - '0';
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

        if (c->mode == PORT_CARD_GAME && !c->name[0] && !c->hue && !c->sat && !c->val)
            continue;   /* untouched: nothing to keep */
        w = snprintf(buf + n, (size_t)(size - n), "card%d=%u,%u,%u,%u,%u,%s\n", p,
                     (unsigned)c->mode, (unsigned)c->spray, (unsigned)c->hue,
                     (unsigned)c->sat, (unsigned)c->val, c->name);
        if (w < 0 || w >= size - n)
            break;
        n += w;
    }
    return n;
}

/* --- names ----------------------------------------------------------------------- */

CARD_EXPORT const char *PortPlayerName(int slot)
{
    if (slot < 0 || slot > 3)
        return "";
    if (sSlotNamed[slot])
        return sSlotName[slot];
    if (gPortLnOn)
        return "";          /* online: the session names its players */
    if (!PortLmpSlotIsHuman(slot))
        return "";
    return PortCardShownName(slot);
}

CARD_EXPORT u32 PortPlayerColor(int slot)
{
    u32 v;
    s32 rgb;

    if (slot < 0 || slot > 3)
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
    if (slot < 0 || slot > 3)
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

    if (!sTagsOn || slot < 0 || slot > 3 || !PortPlayerName(slot)[0])
        return 0;
    k = &gKirbys[slot];
    if (k->hp <= 0 || k->base.roomId == 0xFFFF || (k->base.flags & 0x100)
        || k->base.roomId != gKirbys[gLocalPlayerId].base.roomId)
        return 0;
    if (x) *x = k->base.x >> 8;
    if (y) *y = (k->base.y >> 8) - 20;     /* about a Kirby's height up */
    if (room) *room = k->base.roomId;
    return 1;
}

CARD_EXPORT int PortCardTagsOn(void) { return sTagsOn; }
CARD_EXPORT void PortCardSetTagsOn(int on) { sTagsOn = on != 0; }

/* --- online hand-off ------------------------------------------------------------ */

CARD_EXPORT const char *PortCardProfileString(void)
{
    static char buf[PORT_CARD_NAME_MAX + 16];

    snprintf(buf, sizeof(buf), "%s\t%08X", sCards[0].name, (unsigned)PortCardPaint(0));
    return buf;
}

CARD_EXPORT int PortCardHostApplyPaint(int slot, u32 paint)
{
    if (!gPortLnOn || PortLnRole() != PORT_LN_HOST || slot < 0 || slot > 3 || !PortPaintValid(paint))
        return -1;
    return PortLnEvent(PORT_LN_EV_PAINT, slot, (int)paint, 0, 0);
}

CARD_EXPORT int PortCardHostApplyProfile(int slot, const char *profile)
{
    char name[PORT_CARD_NAME_MAX + 1];
    const char *tab;
    unsigned paint = 0;
    size_t n;

    if (profile == NULL || (tab = strchr(profile, '\t')) == NULL)
        return -1;
    n = (size_t)(tab - profile);
    if (n > PORT_CARD_NAME_MAX)
        n = PORT_CARD_NAME_MAX;
    memcpy(name, profile, n);
    name[n] = '\0';
    if (sscanf(tab + 1, "%x", &paint) != 1)
        return -1;
    PortCardSetSlotName(slot, name);
    return PortCardHostApplyPaint(slot, paint);
}

/* --- the host's frame hook ----------------------------------------------------- */

static int sEditing;
/* What was asked of the simulation and when, so a paint waiting for its
 * latch is not asked for again every frame. */
static u32 sAsked[4], sAskedAt[4];
static int sAskedValid[4];

static void Ask(int s, u32 want)
{
    u32 f;

    if (gPortLnOn) {
        int r = PortLnEvent(PORT_LN_EV_PAINT, s, (int)want, 0, 0);

        if (r < 0)
            return;
        f = (u32)r;
    } else {
        f = PortInputLogSchedulePaint((u8)s, want);
        if (f == 0 && PortFrameNumber() > 1)
            return;
    }
    sAsked[s] = want;
    sAskedAt[s] = f;
    sAskedValid[s] = 1;
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
    if (sEditing || PortInputLogReplaying() || PortRbActive() || PortNetActive() || PortSeatActive())
        return;
    if (gPortLnOn) {
        /* online: the host keeps its own slot in step; the guests' slots
         * are the session layer's (PortCardHostApplyProfile) */
        if (PortLnRole() != PORT_LN_HOST || PortLnCatchingUp())
            return;
        n = 1;
    } else {
        n = 4;
    }
    for (s = 0; s < n; s++) {
        u32 want = (s == 0 || PortLmpSlotIsHuman(s)) ? PortCardPaint(s) : PORT_PAINT_NONE;

        if (PortPaintOfSlot(s) == want) {
            sAskedValid[s] = 0;
            continue;
        }
        if (sAskedValid[s] && sAsked[s] == want && PortFrameNumber() <= sAskedAt[s] + 2)
            continue;       /* on its way */
        Ask(s, want);
    }
}

/* --- the screens' model ------------------------------------------------------- */

enum { R_PLAYER, R_NAME, R_COLOR };
enum { R_HUE, R_SAT, R_VAL };

static int sSub, sPlayer, sFixed, sNamePos;
static struct PortCard sEdit;       /* the card being edited                */
static u32 sGen = 1;

#define HUE_STEP 6
#define SV_STEP  5

static void Changed(void)
{
    sGen++;
}

static void Load(int p)
{
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

CARD_EXPORT void PortPlayerCardClose(void)
{
    if (sSub != PORT_CARD_SUB_NONE)
        Store();
    sSub = PORT_CARD_SUB_NONE;
    sEditing = 0;
    Changed();
}

CARD_EXPORT int PortCardSub(void) { return sSub; }
int PortCardPlayer(void) { return sPlayer; }
u32 PortCardGen(void) { return sGen; }

const char *PortCardHeader(int sub)
{
    return sub == PORT_CARD_SUB_PICK ? "COLOR" : "PLAYER CARD";
}

const char *PortCardRowName(int sub, int row)
{
    if (sub == PORT_CARD_SUB_PICK)
        return row == R_HUE ? "HUE" : row == R_SAT ? "VIVID" : "BRIGHT";
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
        /* the bars say it (platform/menu_poc.c draws them in the value's place) */
        (void)selected;
        return;
    }
    switch (row) {
    case R_PLAYER:
        snprintf(buf, size, selected && !sFixed ? "<P%d>" : "P%d", sPlayer + 1);
        break;
    case R_NAME: {
        /* the letters so far, the one being picked between arrows */
        int i, o = 0, len = (int)strlen(sEdit.name);

        for (i = 0; i < PORT_CARD_NAME_MAX && o < size - 4; i++) {
            char c = i < len ? sEdit.name[i] : ' ';

            if (selected && i == sNamePos) {
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
        switch (row) {
        case R_HUE: return "LEFT RIGHT CHANGE THE COLOR";
        case R_SAT: return "FROM GRAY TO VIVID";
        default:    return "FROM DARK TO BRIGHT";
        }
    }
    switch (row) {
    case R_PLAYER: return sFixed ? "WHOSE CARD THIS IS" : "LEFT RIGHT PICK A PLAYER";
    case R_NAME:   return "LEFT RIGHT LETTER - A NEXT";
    default:
        return sEdit.mode == PORT_CARD_GAME ? "THE GAMES OWN SPRAY PAINT"
             : "A PICKS ANY COLOR";
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

/* The picker opens on the colour shown now. */
static void PickFromCurrent(void)
{
    s32 rgb = -1;

    if (sEdit.mode == PORT_CARD_SPRAY)
        rgb = PortPaintSprayRgb(sEdit.spray);
    else if (sEdit.mode == PORT_CARD_GAME)
        rgb = PortPaintSprayRgb(sPlayer);
    if (rgb >= 0)
        RgbHsv((u32)rgb, &sEdit.hue, &sEdit.sat, &sEdit.val);
    sEdit.mode = PORT_CARD_CUSTOM;
}

static int Clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

CARD_EXPORT int PortCardKey(int *row, unsigned key)
{
    switch (sSub) {
    case PORT_CARD_SUB_CARD:
        if (key == 3 || key == 4) {
            *row = (*row + (key == 4 ? 1 : PORT_CARD_ROWS - 1)) % PORT_CARD_ROWS;
            Changed();
            return PORT_CARD_K_CURSOR;
        }
        if (key == 2) {
            if (*row == R_NAME && sNamePos > 0) {
                sNamePos--;
                Changed();
                return PORT_CARD_K_CURSOR;
            }
            PortPlayerCardClose();
            return PORT_CARD_K_CLOSE;
        }
        switch (*row) {
        case R_PLAYER:
            if (sFixed || (key != 5 && key != 6 && key != 1))
                return key == 1 ? PORT_CARD_K_BACK : PORT_CARD_K_QUIET;
            Store();
            Load((sPlayer + (key == 5 ? PORT_CARD_PLAYERS - 1 : 1)) % PORT_CARD_PLAYERS);
            Changed();
            return PORT_CARD_K_CURSOR;
        case R_NAME:
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
                *row = R_HUE;
                Changed();
                return PORT_CARD_K_SELECT;
            }
            return PORT_CARD_K_NONE;
        }
        return PORT_CARD_K_NONE;

    case PORT_CARD_SUB_PICK:
        if (key == 3 || key == 4) {
            *row = (*row + (key == 4 ? 1 : PORT_CARD_ROWS - 1)) % PORT_CARD_ROWS;
            Changed();
            return PORT_CARD_K_CURSOR;
        }
        if (key == 2 || key == 1) {
            Store();
            sSub = PORT_CARD_SUB_CARD;
            *row = R_COLOR;
            Changed();
            return key == 1 ? PORT_CARD_K_SELECT : PORT_CARD_K_BACK;
        }
        if (key == 5 || key == 6) {
            int d = key == 6 ? 1 : -1;

            if (*row == R_HUE)
                sEdit.hue = (u16)((sEdit.hue + d * HUE_STEP + 360) % 360);
            else if (*row == R_SAT)
                sEdit.sat = (u8)Clamp100(sEdit.sat + d * SV_STEP);
            else
                sEdit.val = (u8)Clamp100(sEdit.val + d * SV_STEP);
            sEdit.mode = PORT_CARD_CUSTOM;
            Store();
            return PORT_CARD_K_CURSOR;
        }
        return PORT_CARD_K_NONE;
    }
    return PORT_CARD_K_NONE;
}

int PortCardPreviewPalette(u16 out[16])
{
    return PortPaintBodyPalette(CardPaintOf(&sEdit), sPlayer, out);
}

int PortCardBar(int row, u16 out[13])
{
    int i, cur;

    for (i = 0; i < 13; i++) {
        int h = sEdit.hue, s = sEdit.sat, v = sEdit.val;

        if (row == R_HUE)      { h = i * 360 / 13; s = s < 40 ? 60 : s; v = v < 50 ? 90 : v; }
        else if (row == R_SAT) s = i * 100 / 12;
        else                   v = i * 100 / 12;
        out[i] = Bgr555(PortCardHsvRgb(h, s, v));
    }
    cur = row == R_HUE ? (sEdit.hue * 13 / 360) : (row == R_SAT ? sEdit.sat : sEdit.val) * 12 / 100;
    return cur < 0 ? 0 : cur > 12 ? 12 : cur;
}

/* --- typed names (the web page) ------------------------------------------------- */

static int sCursorRow;      /* the screen's row, as the last key left it */

CARD_EXPORT int PortCardWantsText(void)
{
    return sSub == PORT_CARD_SUB_CARD && sCursorRow == R_NAME;
}

/* The screen tells us its row (menu_poc.c keeps the cursor). */
void PortCardNoteRow(int row)
{
    sCursorRow = row;
}

CARD_EXPORT void PortCardTypeChar(int c)
{
    int len;

    if (sSub != PORT_CARD_SUB_CARD)
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
    enum { CELL = 40, COLS = 14, ROWS_ = 8 };
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
            u32 v;
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
            else                  /* pastels */
                v = PortPaintCustom(PortCardHsvRgb(c * 360 / COLS, 20, 100));
            if (!PortPaintBodyPalette(v, 0, pal))
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
