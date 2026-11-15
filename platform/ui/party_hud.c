/* The party HUD: everyone's health and state, like a party menu, without
 * getting in the way.
 *
 * --- what it shows -------------------------------------------------------
 *
 * The game always simulates four Kirbys, but its HUD (game/src/hud.c) only
 * ever shows one: gLocalPlayerId's ability, lives and health along the
 * bottom.  That bar stays exactly as it is and is player 1's card.  Every
 * other Kirby that matters gets a small card of its own, stacked in the top
 * right corner of the picture:
 *
 *   - each human player (local multiplayer, online), always;
 *   - a CPU helper while it is in the group's room (the game's own icons in
 *     the top left already say where the absent ones are).
 *
 * A card is 16 HUD pixels tall: the game's own lives icon (the Kirby from
 * the bottom bar) in that Kirby's colours, a tag (2P..4P, or CPU), the
 * player's name from the player cards when they have one (PortPlayerName:
 * locally the card's, online the name the host relays), a health
 * bar with one pip per hit point in the bar's own colours, the ability's
 * name and the lives left.  States: KO (hp gone: grey icon, "KO", or "OUT"
 * with no lives left), AWAY (a human in another room: the word), CPU.  When a
 * card is up, a "1P" tag goes on the game's bar too, so it reads as player
 * 1's card.  Alone -- one human and nobody with him -- nothing is drawn and
 * the screen is the game's.
 *
 * "You": the tags of the players this device controls are lit (white
 * lettering on the Kirby's colour), everyone else's are dark.  On one screen
 * that is every human; online it is this device's own players
 * (PortLnSelfSlots).  Per-device presentation, read
 * from host state, never written anywhere the simulation can see.
 *
 * --- where it draws from -------------------------------------------------
 *
 * Presentation only, both renderers.  At the blit of each simulated frame
 * (PortPartyHudSample) it reads the Kirbys (gKirbys, gNumHumanPlayers,
 * gLocalPlayerId) and the registers the frame was drawn with; present.c then
 * draws the cards with GlrDrawOverlay over the presented picture -- the GPU
 * renderer's world (with or without the HUD layer) or the Classic renderer's
 * 240x160 picture -- after the game's HUD and before the menu.  Nothing is
 * written to game memory, recorded or hashed, so rewinds, replays and the
 * whole-image checks of online play never see it, and every device may draw
 * its own.
 *
 *   - Shown exactly when the game's HUD is: BG1 on and on screen block 28
 *     (the test rl_build.c uses for the HUD regions), not in forced blank,
 *     not the attract demo.  Pause, menus, cutscenes and the title have no
 *     HUD and so no cards.
 *   - Fades with it: the HUD's palette line (BG palette 15, which the game's
 *     fades darken and brighten) and BLDY when BG1 is a brightness target
 *     give an affine map per channel that every card pixel goes through.
 *   - Its size is the HUD's: PortLayoutHudScale with the GPU renderer, the
 *     picture's own scale with the Classic one.  Zooming out (local
 *     multiplayer) shrinks the world, never the cards.
 *   - Where: in the bar beside the picture (or above it) when the window
 *     has one wide enough and nothing of the page's is there -- whole
 *     cards, at the HUD's size or a step or two smaller (never under half).
 *     Otherwise pinned to the HUD's top right corner (or the original
 *     strip's), pushed down below anything registered as a keep-out
 *     (PortPartyHudKeepOut: the web page's touch pads and menu button, the
 *     native menu button).
 *   - Legible on anything (2026-11-15, after "the host cannot see the party
 *     HUD" on a large desktop window): every card sits on an opaque plate
 *     of near black with a light edge (the Kirby's light shade, or grey),
 *     so either the plate or the edge stands out against any background,
 *     and the lettering is white or a light shade on the plate.  States are
 *     words, never fading: AWAY (a person in another room) in light cyan,
 *     KO / OUT in grey, on the compact card too.  The old design (alpha 200
 *     over the play field, AWAY at 150) all but vanished over busy or
 *     matching scenery.  tools/hud_sweep_test.mjs measures it.
 *   - Size: the HUD's scale made a whole number (the 4x6 lettering is only
 *     even at whole multiples), never under 1.5 page pixels per HUD pixel
 *     (CardScale: an old HUD-size setting or a squeezed picture must not
 *     shrink them past reading), never over a third of the picture's width.
 *   - Compact over the play field: a card is its tag, health bar and state
 *     word, one row of 8 on a 10-row plate.  It opens to the whole card for
 *     three seconds when it first appears and whenever that Kirby's health,
 *     ability, lives or state change (WatchCard), then closes again.
 *   - The 2D canvas (no WebGL2, host_web.c): the same cards, hint and tags,
 *     blended at scale 1 into a copy of the 240x160 picture
 *     (PortPartyHudDrawSoft).
 *
 * --- name tags -----------------------------------------------------------------
 *
 * SETTINGS > NAME TAGS (gPortSettings.nameTags, off by default): a small
 * tag over each person's Kirby in the screen's room while two or more
 * people play -- the player card's name, or "2P" -- outlined in the Kirby's
 * light shade, on a plate of its colour for this device's own players.
 * Anchored by PortPlayerTagAnchor (world pixels), projected with the view
 * present.c reports (PortPartyHudSetView), stacked when Kirbys stand side
 * by side, dropped when off the picture.  Drawn under the cards.
 *
 * --- colours ---------------------------------------------------------------------
 *
 * A player card's custom colour (kirby_paint.c) shades the little Kirby,
 * its tag and its bar the way the simulation shades the bottom bar
 * (PortPaintHudPaletteOf), so they match the Kirby rather than the nearest
 * spray.
 *
 * --- the pictures -----------------------------------------------------------
 *
 * Built at run time from the player's ROM, never stored: the Kirby is the
 * lives icon from the HUD's tile sheet (gUnk_082EC7A0, the tiles hud.c puts
 * at 0x18F/0x190/0x19F), coloured with the HUD's palette for that Kirby's
 * colour (sHudPalettes[color], the same table the bottom bar uses), and the
 * bar is drawn in that palette's bar colours.  The lettering is a 4x6 font
 * of our own with the HUD's black outline; ability names are English.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "task.h"
#include "constants/kirby.h"
#include "port/port.h"
#include "port/backend.h"
#include "port/localmp.h"
#include "port/lmpnet.h"
#include "port/player_card.h"
#include "port/kirby_paint.h"
#include "render/gl_renderer.h"
#include "render/render_list.h"
#include "ui/party_hud.h"
#include "ui/settings.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const u16 sHudPalettes[18][16];       /* game/src/hud.c */

#define IO16(off) (*(vu16 *)(GBA_IO_BASE + (off)))
#define PORT_HUD_W 240
#define PORT_HUD_H 160

/* --- the party, as last sampled ------------------------------------------- */

enum { ST_OK = 0, ST_KO, ST_OUT, ST_AWAY };

typedef struct {
    u8 slot, color, hp, maxHp, lives, ability, state, human, local;
    u8 full;                /* the whole card (else tag + health only)      */
    char name[PORT_CARD_NAME_MAX + 1];  /* the player card's name, "" none */
    u16 pal[16];            /* the HUD palette for its colour (a card's own) */
} Member;

/* A name tag over a person's Kirby: world pixels of the top of its head. */
typedef struct {
    u8 on, local;
    s32 x, y;
    char text[PORT_CARD_NAME_MAX + 1];
    u16 pal[16];
} NameTag;

typedef struct {
    u8 show;                /* the game's HUD is up                          */
    u8 n;                   /* cards                                         */
    u8 tag;                 /* the "1P" tag on the game's bar                */
    u8 p1slot, p1color, p1local;
    u8 lo[3], hi[3];        /* fade: c -> lo + c * (hi - lo) / 31            */
    u16 p1pal[16];
    Member m[3];
} Party;

static Party sNow;
static NameTag sTags[4];    /* sampled with the party; independent of it   */
static u8 sTagsShow;
static Party sTagFade;      /* the HUD's fade, for the tags (lo/hi only)   */

/* Where the picture's world pixels land: view pixel = world - cam - view
 * origin; the picture is viewW x viewH of those (present.c, from the GPU
 * renderer's frame, or the game camera for the Classic one). */
static struct { int ok; s32 camX, camY, viewX, viewY, viewW, viewH; } sView;

void PortPartyHudSetView(int ok, int camX, int camY, int viewX, int viewY, int viewW, int viewH)
{
    sView.ok = ok && viewW > 0 && viewH > 0;
    sView.camX = camX; sView.camY = camY;
    sView.viewX = viewX; sView.viewY = viewY;
    sView.viewW = viewW; sView.viewH = viewH;
}

/* --- compact cards ------------------------------------------------------------
 * A card is its tag and health bar; for EXPAND_FRAMES after its health,
 * ability, lives or state change (or when it first appears) it opens to the
 * whole card -- icon, name, ability, lives -- then closes again.  Per slot,
 * per device, presentation only. */
#define EXPAND_FRAMES 180
static struct { u8 seen, hp, ability, lives, state, human; int left; } sWatch[4];

static int WatchCard(const Member *m)
{
    int s = m->slot & 3;

    if (!sWatch[s].seen || sWatch[s].hp != m->hp || sWatch[s].ability != m->ability
        || sWatch[s].lives != m->lives || sWatch[s].state != m->state || sWatch[s].human != m->human)
        sWatch[s].left = EXPAND_FRAMES;
    sWatch[s].seen = 2;         /* 2: on a card this sample */
    sWatch[s].hp = m->hp;
    sWatch[s].ability = m->ability;
    sWatch[s].lives = m->lives;
    sWatch[s].state = m->state;
    sWatch[s].human = m->human;
    if (sWatch[s].left > 0)
        sWatch[s].left--;
    return sWatch[s].left > 0;
}

/* The HUD palette slot s's Kirby wears: the game's row for its colour, or a
 * player card's custom colour shaded the way the simulation shades it
 * (kirby_paint.c), so the little Kirby matches the big one. */
static void PaletteOf(int s, int color, u16 out[16])
{
    if (!PortPaintHudPaletteOf(PortPaintOfSlot(s), color, out))
        memcpy(out, sHudPalettes[color < 18 ? color : 0], 16 * sizeof(u16));
}
static int sKeep[PORT_PARTY_KEEPOUTS][4];
static int Lum5Of(u16 c)
{
    return ((c & 31) * 5 + ((c >> 5) & 31) * 9 + ((c >> 10) & 31) * 2) / 16;
}

/* The hint line, at the top of the picture (PortPartyHudDraw): in rooms
 * mode, players apart, while this screen's player stands at a big door or
 * on the goal star that waits for someone elsewhere (localmp.c
 * PortLmpWaitingFor), "THE DOOR WAITS FOR 2P - R CALLS", up to 4 s per wait,
 * then quiet until the player has left it for a second.  (Phone calls get
 * no line of ours: the game's own HUD shows the caller's icon, and a called
 * person comes only by pressing R -- tools/rooms/phone_optin_test.mjs.)
 *
 * Shown whenever the game's HUD is up, party cards on or off; it never stops
 * anything.  sHintText is what is up, "" = nothing; sHintEdge its plate's
 * edge. */
#define HINT_FRAMES 240
static char sHintText[40], sHintDrawn[40];
static int sHintOn, sHintOff;
static u16 sHintEdge = 0x5AD6;
static u8 sHintKind;            /* 0 none, 1 door, 2 star                  */

static void SampleHint(void)
{
    int who = -1, kind = PortLmpWaitingFor(&who);

    if (!kind) {
        if (++sHintOff > 60)
            sHintOn = 0;
        sHintText[0] = '\0';
        sHintKind = 0;
        return;
    }
    sHintOff = 0;
    if (sHintOn++ >= HINT_FRAMES) {
        sHintText[0] = '\0';
        sHintKind = 0;
        return;
    }
    snprintf(sHintText, sizeof sHintText, "%s WAITS FOR %dP - R CALLS",
             kind == 2 ? "THE STAR" : "THE DOOR", who + 1);
    sHintEdge = 0x5AD6;
    sHintKind = (u8)kind;
}

void PortPartyHudKeepOut(int slot, int x, int y, int w, int h)
{
    if (slot < 0 || slot >= PORT_PARTY_KEEPOUTS)
        return;
    if (w <= 0 || h <= 0)
        x = y = w = h = 0;
    sKeep[slot][0] = x;
    sKeep[slot][1] = y;
    sKeep[slot][2] = w;
    sKeep[slot][3] = h;
}

int PortPartyHudCards(void)
{
    return sNow.show ? sNow.n : 0;
}

/* Where the cards were last drawn: x y w h (framebuffer), 1 if in a bar
 * beside or above the picture, and how many were drawn whole. */
static int sCardsAt[6];

int PortPartyHudFullCards(void)
{
    int i, n = 0;

    for (i = 0; sNow.show && i < sNow.n; i++)
        n += sNow.m[i].full;
    return n;
}

int PortPartyHudNameTags(void)
{
    int s, n = 0;

    for (s = 0; sTagsShow && s < 4; s++)
        n += sTags[s].on;
    return n;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* Tests: the setting (PS_PARTY_*) and the cards up now. */
EMSCRIPTEN_KEEPALIVE int PortPartyHudSettingJs(void) { return gPortSettings.partyHud; }
EMSCRIPTEN_KEEPALIVE int PortPartyHudCardsJs(void) { return PortPartyHudCards(); }
EMSCRIPTEN_KEEPALIVE int PortPartyHudFullJs(void) { return PortPartyHudFullCards(); }
EMSCRIPTEN_KEEPALIVE int PortPartyHudTagsJs(void) { return PortPartyHudNameTags(); }
EMSCRIPTEN_KEEPALIVE int PortPartyHudAtJs(int i) { return i >= 0 && i < 6 ? sCardsAt[i] : 0; }
EMSCRIPTEN_KEEPALIVE int PortPartyHudKeepJs(int s, int k) { return s >= 0 && s < PORT_PARTY_KEEPOUTS && k >= 0 && k < 4 ? sKeep[s][k] : 0; }
/* The game's HUD is up: play, not the pause screens, menus or cutscenes
 * (web/input_devices.js: the join hint shows only then). */
static int HudShowing(void);
EMSCRIPTEN_KEEPALIVE int PortGameHudUpJs(void) { return HudShowing(); }
#endif

/* Is Kirby slot s a person?  The game's own answer is s < gNumHumanPlayers;
 * with drop-in/drop-out (localmp.h, PortIsHuman) the humans are any set. */
static int IsHuman(int s)
{
#ifdef PortIsHuman
    return PortIsHuman(s);
#else
    return s < gNumHumanPlayers;
#endif
}

/* Does this device control slot s?  Per device: never simulation state. */
static int IsLocal(int s)
{
    /* online: the session says which Kirbys this device plays (one bit
     * per slot; several when people share this device) */
    if (PortLnActive())
        return (PortLnSelfSlots() >> s) & 1;
    if (gPortSim->lmp.humans)
        return IsHuman(s);
    return s == gLocalPlayerId;
}

static int HudShowing(void)
{
    u16 dispcnt = IO16(REG_OFFSET_DISPCNT);

    if (dispcnt & DISPCNT_FORCED_BLANK)
        return 0;
    if (!(dispcnt & DISPCNT_BG1_ON) || ((IO16(REG_OFFSET_BG1CNT) >> 8) & 0x1F) != 28)
        return 0;
    if (gUnk_0203AD10 & 0x10)           /* the attract demo's HUD */
        return 0;
    if (gGameplayHudTask == NULL)
        return 0;
    return 1;
}

/* The fade the HUD is under, as lo/hi per 5-bit channel. */
static void SampleFade(Party *p)
{
    const u16 *pal = (const u16 *)(uintptr_t)(GBA_PLTT_BASE + 0xF0 * 2);
    u16 black = pal[7], white = pal[8], bldcnt = IO16(REG_OFFSET_BLDCNT);
    int c;

    for (c = 0; c < 3; c++) {
        int lo = (black >> (5 * c)) & 31, hi = (white >> (5 * c)) & 31;

        if (hi < lo) {                  /* not the HUD's palette: no fade */
            lo = 0;
            hi = 31;
        }
        if ((bldcnt & BLDCNT_TGT1_BG1) && ((bldcnt >> 6) & 3) >= 2) {
            int y = IO16(REG_OFFSET_BLDY) & 31;

            if (y > 16) y = 16;
            if (((bldcnt >> 6) & 3) == 2) {
                lo += (31 - lo) * y / 16;
                hi += (31 - hi) * y / 16;
            } else {
                lo -= lo * y / 16;
                hi -= hi * y / 16;
            }
        }
        p->lo[c] = (u8)lo;
        p->hi[c] = (u8)hi;
    }
}

/* The name tags: each person's Kirby in the screen's room, while there are
 * two or more people (alone, a tag over yourself says nothing). */
static void SampleTags(void)
{
    int s, people = 0;

    memset(sTags, 0, sizeof(sTags));
    sTagsShow = 0;
    if (!gPortSettings.nameTags || !HudShowing() || gNumKirbys < 2 || gNumKirbys > 4 || !sView.ok)
        return;
    for (s = 0; s < gNumKirbys; s++)
        people += IsHuman(s) != 0;
    if (people < 2)
        return;
    for (s = 0; s < gNumKirbys; s++) {
        NameTag *t = &sTags[s];
        const char *name;
        s32 x, y;
        u16 room;

        if (!IsHuman(s) || !PortPlayerTagAnchor(s, &x, &y, &room))
            continue;
        name = PortPlayerName(s);
        if (name[0])
            snprintf(t->text, sizeof(t->text), "%s", name);
        else
            snprintf(t->text, sizeof(t->text), "%dP", s + 1);
        t->on = 1;
        t->local = (u8)IsLocal(s);
        t->x = x;
        t->y = y;
        PaletteOf(s, gKirbys[s].color < 18 ? gKirbys[s].color : 0, t->pal);
        sTagsShow = 1;
    }
    if (sTagsShow)
        SampleFade(&sTagFade);
}

void PortPartyHudSample(void)
{
    Party p;
    int s, n = 0, me, pass;
    u16 room;

    memset(&p, 0, sizeof(p));
    SampleTags();
    if (HudShowing() && gNumKirbys >= 2 && gNumKirbys <= 4)
        SampleHint();
    else {
        sHintText[0] = '\0';
        sHintKind = 0;
    }
    if (!gPortSettings.partyHud || !HudShowing() || gNumKirbys < 2 || gNumKirbys > 4) {
        SampleFade(&p);         /* the hint's fade */
        sNow = p;
        for (s = 0; s < 4; s++)
            sWatch[s].seen = 0;
        return;
    }
    p.show = 1;
    me = gLocalPlayerId & 3;
    room = gKirbys[me].base.roomId;
    /* Humans first, then the CPUs, each by slot. */
    for (pass = 0; pass < 2; pass++) {
        for (s = 0; s < gNumKirbys && n < 3; s++) {
            const struct Kirby *k = &gKirbys[s];
            Member *m;
            int human = IsHuman(s) != 0;

            if (s == me || human != (pass == 0))
                continue;
            if (!human && (gPortSettings.partyHud < PS_PARTY_HELPERS || k->base.roomId != room))
                continue;               /* a helper not with the group */
            m = &p.m[n++];
            m->slot = (u8)s;
            m->color = k->color < 18 ? k->color : 0;
            m->hp = k->hp > 0 ? (u8)k->hp : 0;
            m->maxHp = k->maxHp > 0 ? (u8)(k->maxHp > 12 ? 12 : k->maxHp) : 0;
            m->lives = k->lives > 99 ? 99 : k->lives;
            m->ability = k->ability < KIRBY_ABILITY_COUNT ? k->ability : 0;
            m->human = (u8)human;
            m->local = (u8)(human && IsLocal(s));
            if (human)          /* a person's name from the player cards */
                snprintf(m->name, sizeof(m->name), "%s", PortPlayerName(s));
            if (k->hp <= 0)
                m->state = k->lives == 0 ? ST_OUT : ST_KO;
            else if (k->base.roomId != room)
                m->state = ST_AWAY;
            else
                m->state = ST_OK;
            PaletteOf(s, m->color, m->pal);
            m->full = (u8)WatchCard(m);
        }
    }
    for (s = 0; s < 4; s++) {   /* a card gone: it opens again when it is back */
        if (sWatch[s].seen == 2)
            sWatch[s].seen = 1;
        else
            sWatch[s].seen = 0;
    }
    p.n = (u8)n;
    p.tag = n > 0;
    p.p1slot = (u8)me;
    p.p1color = gKirbys[me].color < 18 ? gKirbys[me].color : 0;
    PaletteOf(me, p.p1color, p.p1pal);
    p.p1local = (u8)IsLocal(me);
    SampleFade(&p);
    sNow = p;

    {
        /* KATAM_PARTY_LOG=FILE: one line per sampled frame with cards. */
        static FILE *f;
        static int init;

        if (!init) {
            const char *e = getenv("KATAM_PARTY_LOG");
            init = 1;
            f = e ? fopen(e, "w") : NULL;
        }
        if (f) {
            int i;

            fprintf(f, "%u room=%u hud=%08X n=%d", (unsigned)PortFrameNumber(), room,
                    (unsigned)gUnk_0203AD10, n);
            for (i = 0; i < 4 && i < gNumKirbys; i++)
                fprintf(f, " [%d h%d/%d l%d a%d c%d r%u%s]", i, gKirbys[i].hp, gKirbys[i].maxHp,
                        gKirbys[i].lives, gKirbys[i].ability, gKirbys[i].color,
                        gKirbys[i].base.roomId, IsHuman(i) ? " H" : "");
            fputc('\n', f);
            fflush(f);
        }
    }
}

/* --- pictures ------------------------------------------------------------- */

/* Wide enough for a card with an eight-letter name and a twelve-pip bar. */
#define CANVAS_W 120

/* Pixels are composed as GBA colours with an alpha and only turned into
 * RGBA (faded, premultiplied) at the end. */
typedef struct {
    int w, h;
    u16 c[CANVAS_W * 64];
    u8 a[CANVAS_W * 64];
} Canvas;

static void Put(Canvas *cv, int x, int y, u16 c, u8 a)
{
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
        return;
    cv->c[y * cv->w + x] = c;
    cv->a[y * cv->w + x] = a;
}

/* The 4x6 font.  '#' ink; glyphs 3 or 4 wide. */
static const struct { char ch; const char *rows[6]; } kFont[] = {
    { 'A', { ".##.", "#..#", "#..#", "####", "#..#", "#..#" } },
    { 'B', { "###.", "#..#", "###.", "#..#", "#..#", "###." } },
    { 'C', { ".###", "#...", "#...", "#...", "#...", ".###" } },
    { 'D', { "###.", "#..#", "#..#", "#..#", "#..#", "###." } },
    { 'E', { "####", "#...", "###.", "#...", "#...", "####" } },
    { 'F', { "####", "#...", "###.", "#...", "#...", "#..." } },
    { 'G', { ".###", "#...", "#.##", "#..#", "#..#", ".###" } },
    { 'H', { "#..#", "#..#", "####", "#..#", "#..#", "#..#" } },
    { 'I', { "###", ".#.", ".#.", ".#.", ".#.", "###" } },
    { 'J', { "..##", "...#", "...#", "...#", "#..#", ".##." } },
    { 'K', { "#..#", "#.#.", "##..", "#.#.", "#..#", "#..#" } },
    { 'L', { "#...", "#...", "#...", "#...", "#...", "####" } },
    { 'M', { "#..#", "####", "####", "#..#", "#..#", "#..#" } },
    { 'N', { "#..#", "##.#", "#.##", "#..#", "#..#", "#..#" } },
    { 'O', { ".##.", "#..#", "#..#", "#..#", "#..#", ".##." } },
    { 'P', { "###.", "#..#", "#..#", "###.", "#...", "#..." } },
    { 'Q', { ".##.", "#..#", "#..#", "#..#", "#.#.", ".#.#" } },
    { 'R', { "###.", "#..#", "#..#", "###.", "#.#.", "#..#" } },
    { 'S', { ".###", "#...", ".##.", "...#", "...#", "###." } },
    { 'T', { "###", ".#.", ".#.", ".#.", ".#.", ".#." } },
    { 'U', { "#..#", "#..#", "#..#", "#..#", "#..#", ".##." } },
    { 'V', { "#..#", "#..#", "#..#", "#..#", ".##.", ".##." } },
    { 'W', { "#..#", "#..#", "#..#", "####", "####", "#..#" } },
    { 'X', { "#..#", "#..#", ".##.", ".##.", "#..#", "#..#" } },
    { 'Y', { "#.#", "#.#", "#.#", ".#.", ".#.", ".#." } },
    { 'Z', { "####", "...#", "..#.", ".#..", "#...", "####" } },
    { '0', { ".##.", "#..#", "#.##", "##.#", "#..#", ".##." } },
    { '1', { ".#.", "##.", ".#.", ".#.", ".#.", "###" } },
    { '2', { ".##.", "#..#", "..#.", ".#..", "#...", "####" } },
    { '3', { "###.", "...#", ".##.", "...#", "...#", "###." } },
    { '4', { "#..#", "#..#", "####", "...#", "...#", "...#" } },
    { '5', { "####", "#...", "###.", "...#", "...#", "###." } },
    { '6', { ".##.", "#...", "###.", "#..#", "#..#", ".##." } },
    { '7', { "####", "...#", "..#.", ".#..", ".#..", ".#.." } },
    { '8', { ".##.", "#..#", ".##.", "#..#", "#..#", ".##." } },
    { '9', { ".##.", "#..#", "#..#", ".###", "...#", ".##." } },
    { 'x', { "...", "...", "#.#", ".#.", "#.#", "..." } },
    { '-', { "...", "...", "###", "...", "...", "..." } },
    { '.', { ".", ".", ".", ".", ".", "#" } },
    { '!', { "#", "#", "#", "#", ".", "#" } },
};

static int Glyph(char ch)
{
    int i;

    for (i = 0; i < (int)(sizeof(kFont) / sizeof(kFont[0])); i++)
        if (kFont[i].ch == ch)
            return i;
    return -1;
}

static int TextW(const char *s)
{
    int w = 0;

    for (; *s; s++) {
        int g = Glyph(*s);
        w += g < 0 ? 3 : (int)strlen(kFont[g].rows[0]) + 1;
    }
    return w > 0 ? w - 1 : 0;
}

/* Lettering at (x, y) (top-left of the ink), with a black outline round it
 * when `outline` (the HUD's style); returns the width. */
static int Text(Canvas *cv, int x, int y, const char *s, u16 ink, u16 edge, int outline, u8 a)
{
    int pass, w = TextW(s);

    for (pass = outline ? 0 : 1; pass < 2; pass++) {
        int cx = x;
        const char *p;

        for (p = s; *p; p++) {
            int g = Glyph(*p), gw, r, c;

            if (g < 0) {
                cx += 3;
                continue;
            }
            gw = (int)strlen(kFont[g].rows[0]);
            for (r = 0; r < 6; r++)
                for (c = 0; c < gw; c++) {
                    int dx, dy;

                    if (kFont[g].rows[r][c] != '#')
                        continue;
                    if (pass == 1) {
                        Put(cv, cx + c, y + r, ink, a);
                        continue;
                    }
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++)
                            Put(cv, cx + c + dx, y + r + dy, edge, a);
                }
            cx += gw + 1;
        }
    }
    return w;
}

/* One 8x8 tile of the HUD sheet, 4bpp, flipped as asked. */
static void Tile(Canvas *cv, int x, int y, int tile, int hflip, const u16 *pal, int grey, u8 a)
{
    const u8 *src = (const u8 *)&gUnk_082EC7A0[0][0] + tile * 32;
    int r, c;

    for (r = 0; r < 8; r++)
        for (c = 0; c < 8; c++) {
            int sc = hflip ? 7 - c : c;
            int v = (src[r * 4 + sc / 2] >> ((sc & 1) * 4)) & 15;
            u16 col;

            if (v == 0)
                continue;
            col = pal[v];
            if (grey) {
                int l = ((col & 31) * 5 + ((col >> 5) & 31) * 9 + ((col >> 10) & 31) * 2) / 16;

                l = l * 3 / 4 + 4;
                col = (u16)(l | (l << 5) | (l << 10));
            }
            Put(cv, x + c, y + r, col, a);
        }
}

/* The bottom bar's lives icon, hud.c's tiles 0x18F 0x190 / 0x19F 0x19F(h). */
static void KirbyIcon(Canvas *cv, int x, int y, const u16 *pal, int grey, u8 a)
{
    Tile(cv, x, y, 0x18F - 0x185, 0, pal, grey, a);
    Tile(cv, x + 8, y, 0x190 - 0x185, 0, pal, grey, a);
    Tile(cv, x, y + 8, 0x19F - 0x185, 0, pal, grey, a);
    Tile(cv, x + 8, y + 8, 0x19F - 0x185, 1, pal, grey, a);
}

/* A tag: lettering on a rounded plate, 8 tall. */
static int Tag(Canvas *cv, int x, int y, const char *s, u16 plate, u16 ink, u8 a)
{
    int w = TextW(s) + 4, r, c;

    for (r = 0; r < 8; r++)
        for (c = 0; c < w; c++) {
            int edge = r == 0 || r == 7 || c == 0 || c == w - 1;
            int corner = (r == 0 || r == 7) && (c == 0 || c == w - 1);

            if (corner)
                continue;
            Put(cv, x + c, y + r, edge ? 0 : plate, a);
        }
    Text(cv, x + 2, y + 1, s, ink, 0, 0, a);
    return w;
}

/* A lit tag (a player this device controls): white on the Kirby's colour --
 * its darkest body shade, which white reads on for every colour. */
static int LitTag(Canvas *cv, int x, int y, const char *s, const u16 *pal, u8 a)
{
    return Tag(cv, x, y, s, pal[3], 0x7FFF, a);
}

/* A dark one: everyone else's. */
static int DarkTag(Canvas *cv, int x, int y, const char *s, const u16 *pal, u8 a)
{
    return Tag(cv, x, y, s, 0x0842, pal[1], a);
}

/* The health bar, one pip per hit point, in the bar's colours (hud.c's bar
 * tiles: 1 2 the fill, 8 its highlight, 5 6 an empty cell, 7 the edge). */
static int Bar(Canvas *cv, int x, int y, int hp, int maxHp, const u16 *pal, int grey, u8 a)
{
    int w = maxHp * 3 + 1, i, r;

    for (r = 0; r < 8; r++)
        for (i = 0; i < w; i++) {
            int pip = i / 3, col = i % 3;
            u16 c;

            if (r == 0 || r == 7) {
                if (i == 0 || i == w - 1)
                    continue;
                c = pal[7];
            } else if (col == 0) {
                c = pal[7];
            } else if (pip < hp && !grey) {
                c = col == 1 ? (r == 1 ? pal[8] : pal[1]) : (r == 6 ? pal[3] : pal[2]);
            } else {
                c = col == 1 ? pal[6] : pal[5];
            }
            Put(cv, x + i, y + r, c, a);
        }
    return w;
}

static const char *const kAbility[KIRBY_ABILITY_COUNT] = {
    "NORMAL", "FIRE", "ICE", "BURNING", "WHEEL", "PARASOL", "CUTTER", "BEAM",
    "STONE", "BOMB", "THROW", "SLEEP", "COOK", "LASER", "UFO", "SPARK",
    "TORNADO", "HAMMER", "SWORD", "CUPID", "FIGHTER", "MAGIC", "SMASH", "MINI",
    "CRASH", "MISSILE", "MASTER",
};

#define WHITE 0x7FFF
#define GREY  0x4A52            /* RGB(18, 18, 18): the HUD's grey (index 15) */
/* The plate every card sits on: near black with a blue cast, opaque, and a
 * light edge round it.  Whatever is behind, one of the two stands out: the
 * plate against a light background, the edge against a dark one -- and the
 * lettering is always white or a light shade on the plate (the sweep test,
 * tools/hud_sweep_test.mjs, measures both from screenshots). */
#define PLATE 0x1862            /* RGB(2, 3, 6)                                  */
#define EDGE_CPU 0x5AD6         /* RGB(22, 22, 22)                               */
#define AWAY_INK 0x7F94         /* RGB(20, 28, 31): "AWAY", a light cyan         */
#define KO_INK 0x5AD6
#define CARD_H 16
#define CARD_GAP 2
#define PLATE_H 18              /* a whole card on its plate                     */
#define COMPACT_PLATE_H 10      /* a compact one: the 8-row tag and bar, plus 2  */

static void TagFor(const Member *m, char *buf)
{
    if (m->human)
        snprintf(buf, 4, "%dP", m->slot + 1);
    else
        snprintf(buf, 4, "CPU");
}

static const char *StateLabel(const Member *m)
{
    return m->state == ST_OUT ? "OUT" : m->state == ST_KO ? "KO" : m->state == ST_AWAY ? "AWAY" : NULL;
}

/* Luminance of a GBA colour, 0..31 (rough, for picking an edge). */
#define Lum5 Lum5Of

/* The plate's edge: the Kirby's light shade for a person (so the card says
 * whose it is at a glance), the HUD's light grey for a CPU, or when that
 * shade is too dark to stand out against a dark background. */
static u16 EdgeOf(const Member *m)
{
    if (!m->human || Lum5(m->pal[1]) < 18)
        return EDGE_CPU;
    return m->pal[1];
}

static void Plate(Canvas *cv, int x, int y, int w, int h, u16 edge)
{
    int r, c;

    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            int top = r == 0 || r == h - 1, side = c == 0 || c == w - 1;

            if (top && side)
                continue;               /* rounded corners */
            Put(cv, x + c, y + r, top || side ? edge : PLATE, 255);
        }
}

/* A whole card's content (icon, tag, name, bar / state or ability, lives). */
static int FullWidth(const Member *m)
{
    char tag[4], lives[4];
    int top, bottom;
    const char *label = StateLabel(m);

    TagFor(m, tag);
    snprintf(lives, sizeof(lives), "x%d", m->lives);
    top = TextW(tag) + 4 + 1 + (m->maxHp * 3 + 1);
    if (m->name[0])
        top += TextW(m->name) + 3;
    bottom = TextW(label ? label : kAbility[m->ability]) + 2 + 3 + TextW(lives);
    return 17 + (top > bottom ? top : bottom) + 1;
}

/* Compact: the tag, the bar and the state when it is not OK, one row of 8. */
static int CompactWidth(const Member *m)
{
    char tag[4];
    const char *label = StateLabel(m);

    TagFor(m, tag);
    return TextW(tag) + 4 + 1 + (m->maxHp * 3 + 1) + (label ? 3 + TextW(label) : 0);
}

/* On their plates: 2 columns of edge and space each side, 1 row each end. */
static int PlateW(const Member *m) { return (m->full ? FullWidth(m) : CompactWidth(m)) + 4; }
static int PlateH(const Member *m) { return m->full ? PLATE_H : COMPACT_PLATE_H; }

static int CardTag(Canvas *cv, int x, int y, const Member *m, u8 a)
{
    char tag[4];

    TagFor(m, tag);
    if (!m->human)
        return Tag(cv, x, y, tag, GREY, WHITE, a);
    if (m->local)
        return LitTag(cv, x, y, tag, m->pal, a);
    return DarkTag(cv, x, y, tag, m->pal, a);
}

/* A whole card, its left edge at x0, its right edge (the lives) at x1. */
static void Card(Canvas *cv, int x0, int x1, int y, const Member *m)
{
    const u16 *pal = m->pal;
    int ko = m->state == ST_KO || m->state == ST_OUT;
    const char *label = StateLabel(m);
    char lives[4];
    int x;

    KirbyIcon(cv, x0, y, pal, ko, 255);
    x = x0 + 17 + CardTag(cv, x0 + 17, y, m, 255);
    /* the name, after the tag, in the Kirby's light shade (outlined) */
    if (m->name[0])
        x += 2 + Text(cv, x + 2, y + 1, m->name, ko ? KO_INK : pal[1], 0, 1, 255) + 1;
    Bar(cv, x + 1, y, m->hp, m->maxHp, pal, ko, 255);

    Text(cv, x0 + 18, y + 9, label ? label : kAbility[m->ability],
         m->state == ST_AWAY ? AWAY_INK : ko ? KO_INK : WHITE, 0, 1, 255);
    snprintf(lives, sizeof(lives), "x%d", m->lives);
    Text(cv, x1 - 1 - TextW(lives), y + 9, lives, pal[9], 0, 1, 255);
}

static void CompactCard(Canvas *cv, int x0, int y, const Member *m)
{
    int ko = m->state == ST_KO || m->state == ST_OUT;
    const char *label = StateLabel(m);
    int x = x0 + CardTag(cv, x0, y, m, 255);

    x += 1 + Bar(cv, x + 1, y, m->hp, m->maxHp, m->pal, ko, 255);
    if (label)
        Text(cv, x + 2, y + 1, label, m->state == ST_AWAY ? AWAY_INK : KO_INK, 0, 1, 255);
}

/* Composed canvases and what they were composed from. */
#define HINT_W 192
static Canvas sCards, sTag, sHint, sNameTag[4];
static u32 sHintPx[HINT_W * COMPACT_PLATE_H];
static Party sComposed;
static int sHaveComposed;
static u32 sCardsPx[CANVAS_W * 64], sTagPx[16 * 8];
static u32 sNameTagPx[4][CANVAS_W * 10];
static NameTag sNameTagDrawn[4];
static u8 sNameTagFade[4][6];
static u8 sNameTagUpload[4];         /* composed, not yet sent to the GPU */
/* Where each card's plate is in the composed canvas: x y w h. */
static int sCardBox[3][4];

/* Into premultiplied RGBA, through the HUD's fade; `alpha` (0..255) scales
 * the whole canvas (the name tags are a little see-through). */
static void Finish(const Canvas *cv, u32 *out, const Party *p, int alpha)
{
    int i;

    for (i = 0; i < cv->w * cv->h; i++) {
        u32 a = cv->a[i] * (u32)alpha / 255, rgb[3];
        int c;

        if (a == 0) {
            out[i] = 0;
            continue;
        }
        for (c = 0; c < 3; c++) {
            u32 v = (cv->c[i] >> (5 * c)) & 31;

            v = p->lo[c] + v * (u32)(p->hi[c] - p->lo[c]) / 31;
            rgb[c] = (v * 255 / 31) * a / 255;
        }
        out[i] = rgb[0] | (rgb[1] << 8) | (rgb[2] << 16) | (a << 24);
    }
}

/* The cards' size in HUD pixels as they would be composed. */
static void CardsSize(const Party *p, int *w, int *h)
{
    int i;

    *w = *h = 0;
    for (i = 0; i < p->n; i++) {
        const Member *m = &p->m[i];
        int mw = PlateW(m);

        if (mw > *w) *w = mw;
        *h += PlateH(m) + (i ? CARD_GAP : 0);
    }
    if (*w > CANVAS_W) *w = CANVAS_W;
}

static void Compose(const Party *p)
{
    int i, w, h, wFull = 0, y;

    for (i = 0; i < p->n; i++)
        if (p->m[i].full && PlateW(&p->m[i]) > wFull)
            wFull = PlateW(&p->m[i]);
    CardsSize(p, &w, &h);
    if (wFull > w) wFull = w;
    memset(&sCards, 0, sizeof(sCards));
    sCards.w = w;
    sCards.h = h;
    /* right-aligned: the cards hug the corner, the whole ones share an edge */
    for (y = 0, i = 0; i < p->n; i++) {
        const Member *m = &p->m[i];
        int pw = m->full ? wFull : PlateW(m), px = w - pw;

        if (pw > w) pw = w;
        Plate(&sCards, px, y, pw, PlateH(m), EdgeOf(m));
        if (m->full)
            Card(&sCards, px + 2, px + pw - 2, y + 1, m);
        else
            CompactCard(&sCards, px + 2, y + 1, m);
        sCardBox[i][0] = px; sCardBox[i][1] = y; sCardBox[i][2] = pw; sCardBox[i][3] = PlateH(m);
        y += PlateH(m) + CARD_GAP;
    }
    Finish(&sCards, sCardsPx, p, 255);

    memset(&sTag, 0, sizeof(sTag));
    {
        char t[4];

        snprintf(t, sizeof(t), "%dP", p->p1slot + 1);
        sTag.w = TextW(t) + 4;
        sTag.h = 8;
        if (p->p1local)
            LitTag(&sTag, 0, 0, t, p->p1pal, 255);
        else
            DarkTag(&sTag, 0, 0, t, p->p1pal, 255);
    }
    Finish(&sTag, sTagPx, p, 255);
}

/* --- drawing -------------------------------------------------------------- */

static int Overlaps(double x, double y, double w, double h, const int *k)
{
    return k[2] > 0 && x < k[0] + k[2] && k[0] < x + w && y < k[1] + k[3] && k[1] < y + h;
}

static int HitsKeepOut(double x, double y, double w, double h)
{
    int k;

    for (k = 0; k < PORT_PARTY_KEEPOUTS; k++)
        if (Overlaps(x, y, w, h, sKeep[k]))
            return 1;
    return 0;
}

/* Where a drawing goes: the GPU's overlays, or a blend into a 240x160 copy
 * of the picture (the 2D canvas, PortPartyHudDrawSoft). */
typedef struct {
    int fbW, fbH, smooth;
    u32 *soft;                      /* non-NULL: blend here (fbW x fbH)   */
} Target;

static void Emit(const Target *t, int slot, const u32 *px, int w, int h, int dirty,
                 double x, double y, double scale)
{
    int r, c, x0, y0;

    if (!t->soft) {
        GlrDrawOverlay(slot, px, w, h, dirty, t->fbW, t->fbH, x, y, scale, t->smooth);
        return;
    }
    /* software: scale 1, premultiplied over */
    x0 = (int)(x + 0.5);
    y0 = (int)(y + 0.5);
    for (r = 0; r < h; r++) {
        int ty = y0 + r;

        if (ty < 0 || ty >= t->fbH)
            continue;
        for (c = 0; c < w; c++) {
            int tx = x0 + c;
            u32 s = px[r * w + c], d, a, o = 0;
            int ch;

            if (tx < 0 || tx >= t->fbW || !(s >> 24))
                continue;
            a = s >> 24;
            d = t->soft[ty * t->fbW + tx];
            for (ch = 0; ch < 3; ch++) {
                u32 v = ((s >> (8 * ch)) & 255) + ((d >> (8 * ch)) & 255) * (255 - a) / 255;

                o |= (v > 255 ? 255 : v) << (8 * ch);
            }
            t->soft[ty * t->fbW + tx] = o | 0xFF000000u;
        }
    }
}

/* Name tags over the people's Kirbys: the name (or "2P") in the Kirby's light
 * shade with the HUD's black outline, centred over its head; this device's
 * own players' on a plate of their colour.  Under the cards. */
static void DrawNameTags(const Target *tg, const int world[4], double scale)
{
    double kx, ky, rx[4], ry[4], rw[4], rh[4];
    int s, on[4], dirty[4], moved, pass;

    if (!sTagsShow || !sView.ok || scale <= 0.0)
        return;
    kx = (double)world[2] / sView.viewW;
    ky = (double)world[3] / sView.viewH;
    for (s = 0; s < 4; s++) {
        NameTag *t = &sTags[s];
        Canvas *cv = &sNameTag[s];

        on[s] = 0;
        dirty[s] = 0;
        if (!t->on)
            continue;
        if (memcmp(t->text, sNameTagDrawn[s].text, sizeof(t->text)) != 0 || t->local != sNameTagDrawn[s].local
            || memcmp(t->pal, sNameTagDrawn[s].pal, sizeof(t->pal)) != 0 || cv->w == 0
            || memcmp(sNameTagFade[s], sTagFade.lo, 3) != 0 || memcmp(sNameTagFade[s] + 3, sTagFade.hi, 3) != 0) {
            memset(cv, 0, sizeof(*cv));
            cv->w = TextW(t->text) + 4;
            if (cv->w > CANVAS_W) cv->w = CANVAS_W;
            cv->h = 8;
            if (t->local)
                LitTag(cv, 0, 0, t->text, t->pal, 255);
            else
                Text(cv, 2, 1, t->text, t->pal[1], 0, 1, 255);
            Finish(cv, sNameTagPx[s], &sTagFade, 230);
            sNameTagDrawn[s] = *t;
            memcpy(sNameTagFade[s], sTagFade.lo, 3);
            memcpy(sNameTagFade[s] + 3, sTagFade.hi, 3);
            sNameTagUpload[s] = 1;
        }
        dirty[s] = sNameTagUpload[s];
        /* world pixel -> view pixel -> framebuffer: centred over the head */
        rw[s] = cv->w * scale;
        rh[s] = cv->h * scale;
        rx[s] = world[0] + (t->x - sView.camX - sView.viewX) * kx - rw[s] / 2;
        ry[s] = world[1] + (t->y - sView.camY - sView.viewY) * ky - rh[s];
        if (rx[s] < world[0]) rx[s] = world[0];
        if (rx[s] + rw[s] > world[0] + world[2]) rx[s] = world[0] + world[2] - rw[s];
        on[s] = 1;
    }
    /* Kirbys side by side: a tag that would cover another goes up a row. */
    for (pass = 0, moved = 1; moved && pass < 4; pass++) {
        int a, b;

        moved = 0;
        for (a = 0; a < 4; a++)
            for (b = 0; b < a; b++)
                if (on[a] && on[b] && rx[a] < rx[b] + rw[b] && rx[b] < rx[a] + rw[a]
                    && ry[a] < ry[b] + rh[b] && ry[b] < ry[a] + rh[a]) {
                    ry[a] = ry[b] - rh[a] - scale;
                    moved = 1;
                }
    }
    for (s = 0; s < 4; s++) {
        if (!on[s] || ry[s] < world[1] || ry[s] + rh[s] > world[1] + world[3])
            continue;               /* off the top or bottom: no tag */
        Emit(tg, 3 + s, sNameTagPx[s], sNameTag[s].w, sNameTag[s].h, dirty[s],
             (double)(int)(rx[s] + 0.5), (double)(int)(ry[s] + 0.5), scale);
        sNameTagUpload[s] = 0;
    }
}

/* Where things were last drawn, for the tests (framebuffer pixels):
 * the cards' stack x y w h, 1 if in a bar beside or above the picture, how
 * many were drawn whole; each card's plate; the hint's box. */
/* (sCardsAt is declared with the getters above) */
static int sCardAt[3][4];
static int sHintAt[4];
static double sCardScaleNow;

/* The scale the cards are drawn at: the HUD's, made a whole number (the 4x6
 * lettering is only even at whole multiples) and never so small that a HUD
 * pixel is under 1.5 page pixels -- an old HUD size setting, or a picture
 * squeezed small, must not shrink them past reading. */
static double CardScale(double scale, double dpr, const int world[4], int w, int h)
{
    double s = scale >= 2.0 ? (double)(int)scale : scale, lo = dpr * 1.5;

    if (s < lo)
        s = (double)(int)(lo + 0.999);
    /* ... but they must fit the picture: at most a third of its width and
     * half its height */
    while (s > 1.0 && (w * s > world[2] / 3.0 || h * s > world[3] / 2.0))
        s -= 1.0;
    return s < 1.0 ? (scale < 1.0 ? scale : 1.0) : s;
}

static void Layout(const Target *tg, const int world[4], double scale, int strip, double dpr)
{
    const Party *p = &sNow;
    Party whole;
    int dirty = 0, inBand = 0, w, h, i, moved;
    double left, right, top, bottom, cw, ch, x = 0, y = 0, gap, sc, cardScale;

    sCardsAt[0] = sCardsAt[1] = sCardsAt[2] = sCardsAt[3] = sCardsAt[4] = sCardsAt[5] = 0;
    memset(sCardAt, 0, sizeof(sCardAt));
    memset(sHintAt, 0, sizeof(sHintAt));
    if (scale <= 0.0)
        return;
    DrawNameTags(tg, world, scale);
    /* The HUD's own frame: the picture's corners, or the centred strip. */
    left = world[0];
    right = world[0] + world[2];
    if (strip) {
        double hw = PORT_HUD_W * scale;

        left = world[0] + (world[2] - hw) / 2;
        right = left + hw;
    }
    top = world[1];
    bottom = world[1] + world[3];

    if (p->show && p->n > 0) {
        /* Where: in a bar beside or above the picture when there is room
         * there and nothing of the page's is in the way -- whole cards, a
         * size or two smaller than the HUD's if that is what fits (never
         * under half); they cost the play field nothing there.  Else the
         * picture's top right corner, below any keep-out, compact unless
         * something just changed. */
        whole = *p;
        for (i = 0; i < whole.n; i++)
            whole.m[i].full = 1;
        CardsSize(&whole, &w, &h);
        /* from the HUD's own scale (a whole number, and no smaller than
         * reading needs, CardScale) down to half of it */
        sc = scale >= 2.0 ? (double)(int)scale : scale;
        if (sc < dpr * 1.5)
            sc = (double)(int)(dpr * 1.5 + 0.999);
        for (; !inBand && (sc >= scale / 2 || sc >= dpr * 1.5 - 0.001) && sc >= 1.0 && sc >= dpr * 1.5 - 0.001; sc -= 1.0) {
            double bw = w * sc, bh = h * sc, g = 2 * sc;

            if (tg->fbW - (world[0] + world[2]) >= bw + 2 * g
                && !HitsKeepOut(world[0] + world[2] + g, top + g, bw, bh)) {
                x = (double)(int)(world[0] + world[2] + g + 0.5);     /* the right bar */
                y = (double)(int)(top + g + 0.5);
                inBand = 1;
            } else if (world[1] >= bh + 2 * g
                       && !HitsKeepOut(right - g - bw, world[1] - g - bh, bw, bh)) {
                x = (double)(int)(right - g - bw + 0.5);                /* the top bar */
                y = (double)(int)(world[1] - g - bh + 0.5);
                inBand = 1;
            }
            if (inBand)
                cardScale = sc;
        }
        if (inBand)
            p = &whole;
        else {
            CardsSize(p, &w, &h);
            cardScale = CardScale(scale, dpr, world, w, h);
        }
        cw = w * cardScale;
        ch = h * cardScale;
        gap = 2 * cardScale;
        if (!sHaveComposed || memcmp(&sComposed, p, sizeof(*p)) != 0) {
            Compose(p);
            sComposed = *p;
            sHaveComposed = 1;
            dirty = 1;
        }
        if (!inBand) {
            /* Cards: top right, 2 HUD pixels in, below any keep-out in the way. */
            x = (double)(int)(right - gap - cw + 0.5);
            if (x < world[0]) x = world[0];
            y = (double)(int)(top + gap + 0.5);
            for (moved = 1, i = 0; moved && i < 8; i++) {
                int k;

                moved = 0;
                for (k = 0; k < PORT_PARTY_KEEPOUTS; k++)
                    if (Overlaps(x, y, cw, ch, sKeep[k]) && sKeep[k][1] + sKeep[k][3] + cardScale + ch <= bottom) {
                        y = (double)(int)(sKeep[k][1] + sKeep[k][3] + cardScale + 0.5);
                        moved = 1;
                    }
            }
        }
        sCardsAt[0] = (int)x; sCardsAt[1] = (int)y; sCardsAt[2] = (int)cw; sCardsAt[3] = (int)ch;
        sCardsAt[4] = inBand;
        for (i = 0; i < p->n; i++) {
            sCardsAt[5] += p->m[i].full;
            sCardAt[i][0] = (int)(x + sCardBox[i][0] * cardScale);
            sCardAt[i][1] = (int)(y + sCardBox[i][1] * cardScale);
            sCardAt[i][2] = (int)(sCardBox[i][2] * cardScale);
            sCardAt[i][3] = (int)(sCardBox[i][3] * cardScale);
        }
        sCardScaleNow = cardScale;
        Emit(tg, 0, sCardsPx, sCards.w, sCards.h, dirty, x, y, cardScale);

        /* "1P" on the game's bar, in the gap between the ability name (tiles
         * 0-4) and the lives icon (tile 7): x 42, rows 148-155. */
        if (p->tag)
            Emit(tg, 1, sTagPx, sTag.w, sTag.h, dirty,
                 (double)(int)(left + 42 * scale + 0.5),
                 (double)(int)(bottom - (PORT_HUD_H - 148) * scale + 0.5), scale);
    }

    if (sHintText[0]) {
        /* The hint (a door or the star that waits): on a plate, centred
         * at the top -- moved left of the cards when they are in the way, or
         * under them when there is no room. */
        int hd = strcmp(sHintText, sHintDrawn) != 0 || sHint.w == 0;
        double hs = CardScale(scale, dpr, world, HINT_W / 2, COMPACT_PLATE_H), hw, hh, hx, hy;

        if (hd) {
            memset(&sHint, 0, sizeof(sHint));
            sHint.w = TextW(sHintText) + 6;
            if (sHint.w > HINT_W) sHint.w = HINT_W;
            sHint.h = COMPACT_PLATE_H;
            Plate(&sHint, 0, 0, sHint.w, sHint.h, sHintEdge);
            Text(&sHint, 3, 2, sHintText, WHITE, 0, 1, 255);
            Finish(&sHint, sHintPx, &sNow, 255);
            snprintf(sHintDrawn, sizeof sHintDrawn, "%s", sHintText);
        }
        while (hs > 1.0 && sHint.w * hs > world[2] - 4 * hs)
            hs -= 1.0;
        hw = sHint.w * hs;
        hh = sHint.h * hs;
        hx = (double)(int)(world[0] + (world[2] - hw) / 2 + 0.5);
        hy = (double)(int)(world[1] + 2 * hs + 0.5);
        if (sCardsAt[2] > 0 && !sCardsAt[4]
            && Overlaps(hx, hy, hw, hh, sCardsAt)) {
            double lx = sCardsAt[0] - 2 * hs - hw;

            if (lx >= world[0] + 2 * hs)
                hx = (double)(int)(lx + 0.5);
            else
                hy = (double)(int)(sCardsAt[1] + sCardsAt[3] + 2 * hs + 0.5);
        }
        for (moved = 1, i = 0; moved && i < 8; i++) {
            int k;

            moved = 0;
            for (k = 0; k < PORT_PARTY_KEEPOUTS; k++)
                if (Overlaps(hx, hy, hw, hh, sKeep[k]) && sKeep[k][1] + sKeep[k][3] + hs + hh <= bottom) {
                    hy = (double)(int)(sKeep[k][1] + sKeep[k][3] + hs + 0.5);
                    moved = 1;
                }
        }
        sHintAt[0] = (int)hx; sHintAt[1] = (int)hy; sHintAt[2] = (int)hw; sHintAt[3] = (int)hh;
        Emit(tg, 2, sHintPx, sHint.w, sHint.h, hd, hx, hy, hs);
    }
}

void PortPartyHudDraw(int fbW, int fbH, const int world[4], double scale, int strip, int smooth, double dpr)
{
    Target t;

    t.fbW = fbW;
    t.fbH = fbH;
    t.smooth = smooth;
    t.soft = NULL;
    Layout(&t, world, scale, strip, dpr > 0.0 ? dpr : 1.0);
}

/* The 2D canvas (host_web.c): sample as present.c would for the Classic
 * renderer, then blend into the 240x160 copy at scale 1. */
int PortPartyHudSoftSample(void)
{
    s32 kx, ky, cx, cy;

    PortRlFocus(&kx, &ky, &cx, &cy);
    PortPartyHudSetView(1, cx >> 8, cy >> 8, 0, 0, 240, 160);
    PortPartyHudSample();
    return (sNow.show && sNow.n > 0) || sHintText[0] || sTagsShow;
}

void PortPartyHudDrawSoft(u32 *pixels, int w, int h)
{
    static const int world[4] = { 0, 0, 240, 160 };
    Target t;
    int k, keep[PORT_PARTY_KEEPOUTS][4];

    if (w != 240 || h != 160)
        return;
    t.fbW = w;
    t.fbH = h;
    t.smooth = 0;
    t.soft = pixels;
    /* the page's controls are not over this canvas's pixels */
    memcpy(keep, sKeep, sizeof(keep));
    memset(sKeep, 0, sizeof(sKeep));
    sHaveComposed = 0;          /* the overlays' uploads are not ours */
    for (k = 0; k < 4; k++)
        sNameTagUpload[k] = 1;
    Layout(&t, world, 1.0, 0, 0.0);
    memcpy(sKeep, keep, sizeof(keep));
}

#ifdef __EMSCRIPTEN__
/* Tests: card i's plate (k 0..3: x y w h, framebuffer pixels), its slot and
 * state (k 4: slot, 5: ST_*, 6: whole), the hint's box (i 3, k 0..3), the
 * scale the cards were drawn at x 100 (i 3, k 4). */
EMSCRIPTEN_KEEPALIVE int PortPartyHudCardJs(int i, int k)
{
    if (i == 3)
        return k >= 0 && k < 4 ? sHintAt[k] : k == 4 ? (int)(sCardScaleNow * 100 + 0.5) : 0;
    if (i < 0 || i > 2 || i >= sNow.n || !sNow.show)
        return k == 4 || k == 5 ? -1 : 0;
    if (k >= 0 && k < 4)
        return sCardAt[i][k];
    if (k == 4) return sNow.m[i].slot;
    if (k == 5) return sNow.m[i].state;
    if (k == 6) return sCardsAt[4] ? 1 : sNow.m[i].full;
    return 0;
}
/* The hint line now ("" none). */
EMSCRIPTEN_KEEPALIVE const char *PortPartyHudHintJs(void) { return sHintText; }
EMSCRIPTEN_KEEPALIVE int PortPartyHudHintKindJs(void) { return sHintKind; }
/* Rigs without a picture (tools/rooms/phone_optin_test.mjs): sample now. */
EMSCRIPTEN_KEEPALIVE int PortPartyHudTestSampleJs(void) { return PortPartyHudSoftSample(); }
#endif

#ifdef __cplusplus
}
#endif
