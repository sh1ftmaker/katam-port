/* Local multiplayer: two to four people on one screen, in one running game.
 *
 * Off until someone is a second player: MULTIPLAYER > LOCAL, KATAM_LOCAL_MP=2
 * natively, ?localmp=2 on the web, `option localmp N` in a recording -- or a
 * join at any frame of play (drop in: `human FRAME SLOT ON`, below).  Off,
 * nothing here runs: every game-side hook tests gPortSim->lmp.humans, which
 * stays zero, and the 16 bytes of the sim block it lives in were the spare
 * zero bytes the hash always covered.
 *
 * --- what the game already has -------------------------------------------
 *
 * The game always simulates four Kirbys in one world.  sub_0803F324 decides
 * where each one's buttons come from: slot 0 reads the pad, slots
 * 1..gNumHumanPlayers-1 read the link-cable ring (multi-cart play), the rest
 * read the CPU's output (gUnk_02038590[i].unk9E).  gNumHumanPlayers is what
 * the rest of the game asks when it needs "is this Kirby a person": boss
 * health scaling, shards, chests, the borrow-a-life rule when a player runs
 * out, the pause map, who may enter a big door, and so on.  And
 * sub_080332BC, the start of play, gives CPU brains (sub_0800ECAC) only to
 * slots >= its human count.
 *
 * So another local human is: make that slot a person in the game's eyes,
 * and give it a pad instead of the CPU's output (PortLmpKeys, latched with
 * the console's own keys and recorded with them).  Nothing about the Kirby
 * is faked; it is the same Kirby a link partner would have been, simulated
 * by the same code.
 *
 * --- human slots: drop in, drop out -----------------------------------------
 *
 * Any of slots 1..3 can become a person, or go back to the CPU, at any frame
 * of play: PortLmpSetSlotHuman, a frame-stamped input (`human FRAME SLOT ON`
 * in a recording, PORT_LN_EV_HUMAN online).  The scheme, and why:
 *
 *   gPortSim->lmp.slots   WHO is a person: a bit per slot, bit 0 always.
 *   gNumHumanPlayers      HOW MANY: the bits set, so 1..4, never 0.
 *
 * The game reads gNumHumanPlayers in 200 places (docs/MULTIPLAYER.md §13
 * lists them).  About 110 use it as a number -- boss HP and phase thresholds
 * (table[count - 1]), the boss HP bar, the mirror-phase tally, `count > 1`
 * for the borrow-a-life rule -- and those keep reading it unchanged: the
 * number of people playing is what they mean.  About 75 use it as a set of
 * slots -- `slot < count` for "is this Kirby a person" (doors by Up, chests,
 * shards, items, switches, cannons, the phone, deaths and lives, room
 * arrivals that spawn objects, the CPU brain's "is a person in my room")
 * and `for (i = 0; i < count; i++)` over the people (the big doors that wait
 * for everyone, the goal star, the enemies' target pick, the spawn windows).
 * Those go through PortIsHuman / PortHumanNext / PortCpuNext / PortSlotOrder
 * (port/localmp.h), which are the original expression whenever nobody has
 * joined.  The rest are link-cable, demo and subgame code, which never sees
 * the mask (it is only engaged in ordinary story play).
 *
 * Humans need not be a prefix of the slots.  The other schemes, measured
 * against the readers and rejected:
 *   - count = highest human slot + 1, a CPU in the "gap" below it: the CPU
 *     brain assumes slots below the count are people (code_0800ECAC_2.c:
 *     809, 2334-2410: it finds itself as "a person in my room" and never
 *     follows anyone), the gap CPU's brain is not reset on room changes
 *     (kirby.c:12976), it loses lives, borrows them and counts for game
 *     over, and the big doors and the goal star wait for it (shard.c:122,
 *     special_doors.c:218, warp_star.c:2003);
 *   - renumbering (moving the last human down into the vacated slot):
 *     changes which Kirby a person is holding, mid-game.
 * So when player 2 leaves while player 3 stays, slot 1 is simply a CPU
 * again, slot 2 a person, and gNumHumanPlayers 2.
 *
 * The CPU brains.  Every play start now gives every slot 1..3 a brain
 * (sub_080332BC is told one human, PortLmpStartHumans) and the humans join
 * right after (PortLmpStarted): MULTIPLAYER > LOCAL is "start, then a join".
 * A person's brain is frozen (sub_0800EF60 returns early for a human slot)
 * and reset (sub_0800EE04) when the CPU takes the Kirby back, so it starts
 * from where the Kirby is, not from where it was when the person took over.
 * Should a brain ever be missing it is created then.  Other brains that were
 * following a slot that stopped being a person follow player 1 instead.
 *
 * What a join or a leave keeps: the Kirby -- position, ability, health,
 * lives -- is not touched.  A joiner in another room, dying, mid-door, in a
 * cannon or a goal sequence is brought to the group by the one-room rule
 * below as soon as they can be moved.
 *
 * What it changes mid-fight: a boss's HP was set at spawn for the old count
 * (object.c:2081) while its phase thresholds and its HP bar read the live
 * one, so a count change rescales the HP of every live boss in proportion
 * (RescaleBosses) -- a boss at half health stays at half health.
 *
 * A join outside play (the title, the menus) is held in the mask and
 * applied when the next game starts; the title screen ends everything
 * (PortLmpEnd), and the host's device core asks again for anyone who is
 * still holding a pad (platform/input_devices.c).
 *
 * What link play does NOT give is one screen.  On a cable each console draws
 * the room its own player is in, with a camera on its own player.  Here
 * there is one console, so:
 *
 * --- the shared camera ----------------------------------------------------
 *
 * The local screen's camera follows a point, not a Kirby: the game's two
 * follow modes (sub_08001C40, sub_08002D60) ask PortLmpCamTarget for it.  It
 * is the centre of the humans in the room, pulled back toward player 1 if
 * the group is wider than the widest view the session may show (the sim
 * view, a recorded input), so player 1 is never lost.  The game's own clamp
 * to the camera rectangle (room bounds, scroll locks) runs after, unchanged.
 * The GPU renderer then zooms out to fit the group (platform/ui/present.c,
 * presentation only), down to PortLmpMinZoom.
 *
 * --- the straggler rule ---------------------------------------------------
 *
 * A human other than player 1 who stays outside the widest frame for a
 * second is carried to player 1 (the respawn route, the same one
 * platform/warp.c uses).  Nobody is ever held back: the leader is never
 * walled in by the camera.  Player 1 is the anchor, as in the Kirby games
 * that have drop-in co-op.  The frame is placed the way the renderer places
 * it, so someone left outside a scroll lock is carried in too.
 *
 * --- one room for everyone -----------------------------------------------
 *
 * Every human's arrival in a room (sub_080562D0, which every door, mirror,
 * warp, warp star and respawn ends in) makes that room the group's room
 * (PortLmpArrived).  Once a frame, any other human who is not in it and can
 * be moved is taken there, to the cell the first human in the room stands
 * on, by the respawn route.  A human who cannot be moved yet (mid-door,
 * dying, in a cannon or a goal sequence) is taken as soon as they can be.
 * The room's loading and fade play once, for whoever arrived first; a
 * follower who is not the screen's player arrives behind a hidden fade.
 */

#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "object.h"
#include "functions.h"
#include "task.h"
#include "constants/object_types.h"
#include "port/port.h"
#include "port/simblock.h"
#include "port/localmp.h"
#include "port/rollback.h"
#include "port/lmpnet.h"
#include "port/netsim.h"
#include "port/seats.h"
#include "port/inputlog.h"

/* C linkage for the 64-bit builds -- see platform/warp.c. */
#ifdef __cplusplus
extern "C" {
#endif

extern void sub_08055920(struct Kirby *);
extern void sub_0800ECAC(u8, u16, u16);
extern void sub_0800EE04(u8, u32);
extern void sub_0800EF60(void);

#define L (&gPortSim->lmp)

/* The host's setting and pads.  Not simulation state: the setting reaches
 * the game through PortLmpStartHumans and is recorded as `option localmp`,
 * and the pads through the latch, recorded as `kp` lines. */
static int sSetting, sFromHost;
static u16 sHostKeys[4];
/* Can player 2 press anything on this device?  The web page clears it on a
 * touch-only device until a key or a gamepad shows up. */
static int sP2Input = 1;

/* Margins, in pixels, kept between a framed player and the picture's edge. */
#define MARGIN_X 40
#define MARGIN_Y 32
/* Frames a straggler may stay outside the widest frame before being carried. */
#define STRAY_FRAMES 60

void PortLmpSetHumans(int n)
{
    if (n < 2)
        n = 0;
    if (n > 4)
        n = 4;
    sSetting = n;
    sFromHost = n;
}

void PortLmpChoose(int n)
{
    if (n < 2)
        n = 0;
    if (n > 4)
        n = 4;
    sSetting = n;
}

int PortLmpSettingFromHost(void)
{
    return sFromHost;
}

void PortLmpSetP2Input(int available)
{
    sP2Input = available != 0;
}

/* Read inside the simulation (the MULTIPLAYER screen dims LOCAL on a
 * touch-only device).  In an online session (platform/lmpnet.c) player 2 is
 * on the far side of the network with a pad of their own, and what this
 * device has must not decide anything the other device simulates too
 * (tools/netaudit: p2-input). */
int PortLmpP2InputAvailable(void)
{
    if (PortLnActive())
        return 1;
    return sP2Input;
}

/* A session that runs the game for several machines (rollback, netsim
 * world, seats, the link cable) owns the Kirby slots: no local players. */
static int SessionActive(void)
{
    return PortRbActive() || PortNetActive() || PortSeatActive() || PortSimViewLocked();
}

int PortLmpSetting(void)
{
    return sSetting;
}

void PortLmpSetHostKeys(int player, u16 mask)
{
    if (player >= 1 && player <= 3)
        sHostKeys[player] = mask & 0x03FF;
}

u16 PortLmpHostKeys(int player)
{
    return (player >= 1 && player <= 3) ? sHostKeys[player] : 0;
}

static float sMinZoom = -1.0f;

float PortLmpMinZoom(void)
{
    if (sMinZoom < 0.0f) {
        const char *e = getenv("KATAM_LOCAL_MP_MINZOOM");

        sMinZoom = e ? (float)atof(e) : 0.5f;
        if (sMinZoom < 0.25f) sMinZoom = 0.25f;
        if (sMinZoom > 1.0f) sMinZoom = 1.0f;
    }
    return sMinZoom;
}

/* The latch: this frame's pads for the other humans, into the sim block. */
void PortLmpLatch(u16 keys[4])
{
    int p;

    if (!L->humans)
        return;
    for (p = 1; p < 4; p++) {
        u16 k = PortLmpHumanSlot(p) ? (u16)(keys[p] & 0x03FF) : 0;

        gPortSim->frame.lmpPressed[p - 1] = k & ~L->keys[p - 1];
        L->keys[p - 1] = k;
    }
}

/* --- game hooks ------------------------------------------------------------ */

static int Popcount(u8 m)
{
    int n = 0;

    for (; m; m &= (u8)(m - 1))
        n++;
    return n;
}

/* The start of play (sub_080332BC's human count).  Everyone it is told is a
 * person; it is always told one, so that slots 1..3 all get a CPU brain, and
 * the other humans -- the MULTIPLAYER > LOCAL setting, and anyone who joined
 * outside play -- join once the brains exist (PortLmpStartedSlow). */
u8 PortLmpStartHumans(u8 n)
{
    u8 want = L->slots;
    int s;

    L->humans = 0;
    for (s = 1; s < sSetting && s < 4; s++)
        want |= (u8)(1u << s);
    if (want & 0x0E) {
        if (SessionActive()) {
            PortLog("[katam-port] localmp: refused -- a netplay/rollback/link session is active");
            L->slots = 0;
            return n;
        }
        L->slots = (u8)(want | 1u);
    } else {
        L->slots = 0;
    }
    return n;
}

static void Engage(void)
{
    L->groupRoom = 0xFFFF;
    L->stray[0] = L->stray[1] = L->stray[2] = L->stray[3] = 0;
}

void PortLmpStartedSlow(void)
{
    if (!(L->slots & 0x0E) || (gUnk_0203AD10 & 0x12))
        return;
    Engage();
    L->humans = (u8)Popcount(L->slots);
    gNumHumanPlayers = L->humans;
    PortLog("[katam-port] localmp: game starts with %d human players (slots %X)",
            (int)L->humans, (unsigned)L->slots);
}

/* Ordinary play with local humans: not the attract demo (0x10), not link
 * play (2), and the game agrees there is more than one human. */
static int Active(void)
{
    return L->humans >= 2 && gNumHumanPlayers >= 2 && !(gUnk_0203AD10 & 0x12);
}

int PortLmpInPlay(void)
{
    return Active();
}

/* --- human slots ----------------------------------------------------------- */

static int InRoom(const struct Kirby *k);

/* Slot s's CPU brain task (sub_0800ECAC: priority 0x2100 | slot). */
static struct Task *Brain(u8 s)
{
    int i;

    for (i = 0; i < MAX_TASK_NUM; i++) {
        struct Task *t = &gTasks[i];

        if (t->main == FN(sub_0800EF60) && t->priority == (u16)(0x2100 | s) && t->structOffset)
            return t;
    }
    return NULL;
}

/* Ordinary story play: Kirbys with brains, not link play or the demo. */
static int InStoryPlay(void)
{
    int s;

    if (gUnk_0203AD10 & 0x12)
        return 0;
    for (s = 1; s < 4; s++)
        if (Brain((u8)s))
            return 1;
    return 0;
}

/* A boss's HP was set at spawn from gUnk_08351530[type][count - 1]
 * (object.c:2081); its phase thresholds and its HP bar read the live count.
 * Keep the fraction it has left.  Dark Mind's first form (its own table,
 * indexed by a spawn parameter the object does not keep) is left alone. */
static void RescaleBosses(int from, int to)
{
    int i;

    if (from < 1 || from > 4 || to < 1 || to > 4 || from == to)
        return;
    for (i = 0; i < MAX_TASK_NUM; i++) {
        const struct Task *t = &gTasks[i];
        struct Object *o;
        s32 a, b, hp;

        if (!t->main || !t->structOffset)
            continue;
        o = (struct Object *)((t->flags & TASK_USE_EWRAM)
                ? (uintptr_t)EWRAM_START + ((uintptr_t)t->structOffset << 2)
                : (uintptr_t)IWRAM_START + t->structOffset);
        if (o->base.header.kind != 1 || !(o->base.flags & 0x4000000) || !ObjType38To52(o)
            || o->type == OBJ_DARK_MIND_FORM_1 || o->unk80 <= 0)
            continue;
        a = gUnk_08351530[ObjTypeAltIdx(o)][from - 1];
        b = gUnk_08351530[ObjTypeAltIdx(o)][to - 1];
        if (a <= 0 || b <= 0)
            continue;
        hp = (o->unk80 * b + a / 2) / a;
        if (hp < 1)
            hp = 1;
        PortLog("[katam-port] localmp: f%u boss type %u hp %d -> %d (%d -> %d humans)",
                (unsigned)gPortSim->frame.frameCount, (unsigned)o->type, (int)o->unk80,
                (int)hp, from, to);
        o->unk80 = (s16)hp;
    }
}

static void ToCpu(u8 s)
{
    struct Unk_02038590 *b = &gUnk_02038590[s];

    if (Brain(s) == NULL) {
        u16 room = gCurLevelInfo[s].currentRoom != 0xFFFF ? gCurLevelInfo[s].currentRoom
                                                          : gKirbys[s].base.roomId;

        PortLog("[katam-port] localmp: slot %u had no CPU brain; making one", (unsigned)s);
        sub_0800ECAC(s, room, gAIKirbyState > 199 ? 0x321 : 0x323);
    } else if (gCurLevelInfo[s].currentRoom != 0xFFFF) {
        sub_0800EE04(s, 0);
    }
    b->unk9C = 0;
    b->unk9E = 0;
    b->unkA8 = &gKirbys[0];
    L->keys[s - 1] = 0;
    L->joining &= (u16)~(1u << s);
    gPortSim->frame.lmpPressed[s - 1] = 0;
}

/* During the tutorial and the intro (gAIKirbyState below NORMAL) the other
 * Kirbys are the story's: they mirror player 1, and a script walks them
 * through a door into the hub (801) for the opening cutscene.  A person
 * taking one over on that walk would arrive in the hub as a person and start
 * the cutscene early (measured: tests/recordings/players_rooms.txt); so a
 * join waits while that Kirby is between rooms or in the hub.  Anywhere else
 * it joins at once and the one-room rule brings it to the group. */
static int Scripted(u8 s)
{
    const struct Kirby *k = &gKirbys[s];

    return gAIKirbyState < AI_KIRBY_STATE_NORMAL
        && (!InRoom(k) || k->base.roomId == 0x321);
}

static void Apply(int slot, int on, u8 old, u8 m)
{
    int s, from, to;

    if (!L->humans)
        Engage();
    from = gNumHumanPlayers;
    to = Popcount(m);
    L->slots = m;
    L->humans = (u8)to;
    for (s = 1; s < 4; s++) {
        u8 bit = (u8)(1u << s);

        if ((old & bit) && !(m & bit))
            ToCpu((u8)s);
        else if (!(old & bit) && (m & bit)) {
            const struct Kirby *k = &gKirbys[s];
            u16 g = L->groupRoom != 0xFFFF ? L->groupRoom : gKirbys[0].base.roomId;

            gUnk_02038590[s].unk9E = 0;
            L->stray[s] = 0;
            /* A joiner away from the group -- another room, mid-door, dying
             * -- is brought to the group; their own arrival somewhere (the
             * door they were already in) must not move everyone there. */
            if (k->base.roomId != g || !InRoom(k))
                L->joining |= bit;
        }
    }
    /* Brains that were following someone who is no longer a person. */
    for (s = 1; s < 4; s++) {
        struct Unk_02038590 *b = &gUnk_02038590[s];
        const struct Kirby *lead = b->unkA8;

        if (!(m & (1u << s)) && lead != NULL && (old & (1u << (lead->base.unk56 & 3)))
            && !(m & (1u << (lead->base.unk56 & 3))))
            b->unkA8 = &gKirbys[0];
    }
    RescaleBosses(from, to);
    gNumHumanPlayers = (u8)to;
    PortLog("[katam-port] localmp: f%u player %d %s; %d human players (slots %X)",
            (unsigned)gPortSim->frame.frameCount, slot + 1, on ? "joins" : "leaves",
            to, (unsigned)m);
}

void PortLmpSetSlotHuman(int slot, int on)
{
    u8 old = L->slots ? L->slots : 1, m;

    if (slot < 1 || slot > 3)
        return;
    if (SessionActive()) {
        PortLog("[katam-port] localmp: slot change refused -- a rollback/netsim/seat session owns the slots");
        return;
    }
    L->joining &= (u16)~(0x100u << slot);     /* a waiting join is superseded */
    m = on ? (u8)(old | (1u << slot)) : (u8)(old & ~(1u << slot));
    if (m == old && L->slots)
        return;
    if (!InStoryPlay()) {
        /* Held for the next game's start (PortLmpStartHumans). */
        L->slots = (m & 0x0E) ? m : 0;
        PortLog("[katam-port] localmp: f%u player %d %s (not in play: from the next game)",
                (unsigned)gPortSim->frame.frameCount, slot + 1, on ? "joins" : "leaves");
        return;
    }
    if (on && Scripted((u8)slot)) {
        L->joining |= (u16)(0x100u << slot);
        PortLog("[katam-port] localmp: f%u player %d joins once that Kirby is with player 1 (the story has it)",
                (unsigned)gPortSim->frame.frameCount, slot + 1);
        return;
    }
    Apply(slot, on, old, m);
}

/* Joins waiting for the story to let go of the Kirby (Scripted). */
static void Waiting(void)
{
    int s;

    for (s = 1; s < 4; s++)
        if ((L->joining & (0x100u << s)) && !Scripted((u8)s)) {
            u8 old = L->slots ? L->slots : 1;

            L->joining &= (u16)~(0x100u << s);
            if (!(old & (1u << s)))
                Apply(s, 1, old, (u8)(old | (1u << s)));
        }
}

int PortLmpSlotJoinable(int slot)
{
    if (slot < 1 || slot > 3)
        return 0;
    return !InStoryPlay() || !Scripted((u8)slot);
}

int PortLmpCanRequest(void)
{
    /* Online (platform/lmpnet.c) the slots are host = 1, guest = 2 for now:
     * more people on either device is the session's to seal and carry. */
    return !SessionActive() && !PortLnActive() && !PortInputLogReplaying();
}

u32 PortLmpRequestHuman(int slot, int on)
{
    if (slot < 1 || slot > 3 || !PortLmpCanRequest())
        return 0;
    return PortInputLogScheduleHuman((u8)slot, (u8)(on != 0));
}

int PortLmpProbe(int slot, const char *cond)
{
    const struct Kirby *k;
    int i;

    if (slot < 0 || slot > 3 || cond == NULL)
        return 0;
    k = &gKirbys[slot];
    if (strcmp(cond, "dead") == 0)
        return k->hp <= 0;
    if (strcmp(cond, "alive") == 0)
        return k->hp > 0 && k->base.roomId != 0xFFFF;
    if (strcmp(cond, "away") == 0)
        return k->base.roomId != gKirbys[0].base.roomId;
    if (strcmp(cond, "door") == 0)
        return (k->base.flags & 0x100) != 0;
    if (strcmp(cond, "human") == 0)
        return PortLmpSlotIsHuman(slot);
    if (strcmp(cond, "cpu") == 0)
        return !PortLmpSlotIsHuman(slot);
    if (strncmp(cond, "anim:", 5) == 0)
        return k->animationIndex == (u16)atoi(cond + 5);
    if (strncmp(cond, "flag:", 5) == 0)
        return (k->base.flags & (u32)strtoul(cond + 5, NULL, 16)) != 0;
    if (strcmp(cond, "boss") == 0) {
        for (i = 0; i < MAX_TASK_NUM; i++) {
            const struct Task *t = &gTasks[i];
            const struct Object *o;

            if (!t->main || !t->structOffset)
                continue;
            o = (const struct Object *)((t->flags & TASK_USE_EWRAM)
                    ? (uintptr_t)EWRAM_START + ((uintptr_t)t->structOffset << 2)
                    : (uintptr_t)IWRAM_START + t->structOffset);
            if (o->base.header.kind == 1 && (o->base.flags & 0x4000000) && ObjType38To52(o)
                && o->unk80 > 0 && o->base.roomId == k->base.roomId)
                return 1;
        }
        return 0;
    }
    return 0;
}

int PortLmpSlotIsHuman(int slot)
{
    if (slot == 0)
        return 1;
    if (slot < 0 || slot > 3)
        return 0;
    return ((L->slots >> slot) & 1) || ((L->joining >> (8 + slot)) & 1);
}

void PortLmpEndSlow(void)
{
    if (L->humans)
        PortLog("[katam-port] localmp: back to the title; local players off");
    L->humans = 0;
    L->slots = 0;
    L->groupRoom = 0;
    L->keys[0] = L->keys[1] = L->keys[2] = 0;
    L->joining = 0;
    L->stray[0] = L->stray[1] = L->stray[2] = L->stray[3] = 0;
    gPortSim->frame.lmpPressed[0] = gPortSim->frame.lmpPressed[1] = gPortSim->frame.lmpPressed[2] = 0;
}

/* Is slot k one of the humans on this screen?  (Slot 0 always.) */
static int Human(int k)
{
    return k == 0 || PortLmpHumanSlot(k);
}

/* In a room and not between rooms: what the camera frames. */
static int InRoom(const struct Kirby *k)
{
    return k->base.roomId != 0xFFFF
        && gCurLevelInfo[k->base.unk56].currentRoom != 0xFFFF
        && !(k->base.flags & 0x100)
        && k->hp > 0;
}

/* Can be taken to another room now: the conditions the CPU's own room move
 * (sub_080551FC) checks, less its "standing where a CPU would" bit. */
static int Movable(const struct Kirby *k)
{
    return InRoom(k)
        && !Macro_0810B1F4(&k->base)
        && k->animationIndex != 39
        && k->animationIndex <= 122
        && !k->unk110
        && !(k->base.flags & 0x3800B00);
}

/* Could Kirby stand in cell (x, y) of player p's room: the cell open (not
 * solid, mini-only, a hazard or a slope), the one below something to stand
 * on (solid, a platform or a slope) and not a hazard. */
static int Standable(u8 p, int x, int y)
{
    const struct LevelInfo *li = &gCurLevelInfo[p];
    int w = li->roomWidth / 16, h = li->roomHeight / 16;
    u32 a, b;

    if (x < 1 || y < 1 || x >= w - 1 || y >= h - 1)
        return 0;
    a = gCollisionAttributes[GetCollisionTile(p, (u16)x, (u16)y)];
    b = gCollisionAttributes[GetCollisionTile(p, (u16)x, (u16)(y + 1))];
    if (a & (0x200 | 0x400 | 0x2000 | 0xF0000000))
        return 0;
    if (b & 0x2000)
        return 0;
    return (b & (0x200 | 0x4)) || (b & 0xF0000000);
}

/* Where to put someone taken to `to`: a cell they can stand in near it --
 * beside it first when `beside` (so the two do not overlap and push each
 * other off a door), else on it -- looking down up to ten cells below each
 * column for ground, so nobody lands in mid-air or over a pit.  Failing
 * that, `to`'s own respawn point when it is in this room (a door's arrival
 * cell), else `to`'s cell.  Results are respawn-record cells: the one below
 * Kirby's feet, x + 1 (sub_080562D0: x = cell * 16 - 8, y = cell * 16 - 7). */
static void LandingCell(const struct Kirby *to, u16 room, int beside, u8 *cx, u8 *cy)
{
    static const s8 kBeside[5] = { -1, 1, 0, -2, 2 };
    static const s8 kOn[5] = { 0, -1, 1, -2, 2 };
    const s8 *order = beside ? kBeside : kOn;
    u8 p = to->base.unk56;
    int kx = to->base.x >> 12, ky = to->base.y >> 12, i, dy;

    if (gCurLevelInfo[p].currentRoom == room)
        for (i = 0; i < 5; i++)
            for (dy = 0; dy <= 10; dy++)
                if (Standable(p, kx + order[i], ky + dy)) {
                    *cx = (u8)(kx + order[i] + 1);
                    *cy = (u8)(ky + dy + 1);
                    return;
                }
    if (to->roomId == room) {
        *cx = (u8)to->spawnLocation.x;
        *cy = (u8)to->spawnLocation.y;
        return;
    }
    *cx = (u8)((to->base.x + 0x800) >> 12);
    *cy = (u8)((to->base.y + 0x701) >> 12);
}

/* The respawn route, as platform/warp.c Deliver takes it: sub_080562D0 reads
 * the destination from the respawn record next frame. */
static void Carry(struct Kirby *k, u16 room, const struct Kirby *to, int beside)
{
    u8 cx, cy;

    LandingCell(to, room, beside, &cx, &cy);

    sub_08055920(k);
    k->base.unk58 &= ~0x4000;
    k->base.unk62 &= ~0x10;
    k->roomId = room;
    k->spawnLocation.x = cx;
    k->spawnLocation.y = cy;
    k->flyTimer = 0;
    PortLog("[katam-port] localmp: f%u player %u -> room %u cell (%u,%u)",
            (unsigned)gPortSim->frame.frameCount, (unsigned)k->base.unk56 + 1,
            (unsigned)room, (unsigned)cx, (unsigned)cy);
}

/* The widest frame the screen may show this frame, in 1/256 px, placed the
 * way the renderer places it: centred on the game's camera, then kept inside
 * the camera rectangle widened by the 8 px the game insets it. */
static void Frame(u8 p, s32 *x0, s32 *y0, s32 *x1, s32 *y1)
{
    const struct LevelInfo *li = &gCurLevelInfo[p];
    s32 w = gPortSim->view.viewW << 8, h = gPortSim->view.viewH << 8;
    s32 cx = li->viewportPosition.x + 0x7800, cy = li->viewportPosition.y + 0x5000;
    s32 rx0 = li->unk_S32Vec2_6C.x - 0x800, rx1 = li->unk_S32Vec2_74.x + 0x800;
    s32 ry0 = li->unk_S32Vec2_6C.y - 0x800, ry1 = li->unk_S32Vec2_74.y;

    *x0 = cx - w / 2; *x1 = *x0 + w;
    *y0 = cy - h / 2; *y1 = *y0 + h;
    if (rx1 - rx0 >= w) {
        if (*x0 < rx0) { *x1 += rx0 - *x0; *x0 = rx0; }
        if (*x1 > rx1) { *x0 -= *x1 - rx1; *x1 = rx1; }
    }
    if (ry1 - ry0 >= h) {
        if (*y0 < ry0) { *y1 += ry0 - *y0; *y0 = ry0; }
        if (*y1 > ry1) { *y0 -= *y1 - ry1; *y1 = ry1; }
    }
}

/* The off-screen arrows (hud.c DrawHudOtherKirbys) ask "is this Kirby off
 * the 240x160 screen"; zoomed out, someone can be in the picture and still
 * get an arrow pointing at them.  In the widest frame = not off-screen. */
int PortLmpInFrameSlow(const struct ObjectBase *o)
{
    s32 x0, y0, x1, y1;

    if (!Active() || o->roomId != gCurLevelInfo[gLocalPlayerId].currentRoom)
        return 0;
    Frame(gLocalPlayerId, &x0, &y0, &x1, &y1);
    return o->x >= x0 + 0x800 && o->x <= x1 - 0x800 && o->y >= y0 + 0x800 && o->y <= y1 - 0x800;
}

void PortLmpCamTargetSlow(u8 player, s32 *x, s32 *y)
{
    s32 minX, maxX, minY, maxY, cx, cy, hw, hh;
    u16 room;
    int k, n;

    if (!Active() || player != gLocalPlayerId)
        return;
    n = 4;
    room = gKirbys[player].base.roomId;
    minX = maxX = *x;
    minY = maxY = *y;
    for (k = 0; k < n; k++) {
        const struct Kirby *o = &gKirbys[k];

        if (k == player || !Human(k) || o->base.roomId != room || !InRoom(o))
            continue;
        if (o->base.x < minX) minX = o->base.x;
        if (o->base.x > maxX) maxX = o->base.x;
        if (o->base.y < minY) minY = o->base.y;
        if (o->base.y > maxY) maxY = o->base.y;
    }
    cx = minX + (maxX - minX) / 2;
    cy = minY + (maxY - minY) / 2;
    /* Never so far from this screen's own player that they leave the
     * widest frame. */
    hw = ((gPortSim->view.viewW / 2) - MARGIN_X) << 8;
    hh = ((gPortSim->view.viewH / 2) - MARGIN_Y) << 8;
    if (hw < 0) hw = 0;
    if (hh < 0) hh = 0;
    if (cx < *x - hw) cx = *x - hw;
    if (cx > *x + hw) cx = *x + hw;
    if (cy < *y - hh) cy = *y - hh;
    if (cy > *y + hh) cy = *y + hh;
    *x = cx;
    *y = cy;
}

/* KATAM_LOCAL_MP_DOORS=1: on each arrival of the screen's player, list the
 * room's door cells (collision attribute 0x4000).  For writing test routes. */
static void ListDoors(u8 p)
{
    const char *e = getenv("KATAM_LOCAL_MP_DOORS");
    const struct LevelInfo *li = &gCurLevelInfo[p];
    u16 x, y, w = li->roomWidth / 16, h = li->roomHeight / 16;

    if (e == NULL || atoi(e) == 0)
        return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (gCollisionAttributes[GetCollisionTile(p, x, y)] & 0x4000)
                PortLog("[katam-port] localmp-door: room %u cell (%u,%u)",
                        (unsigned)li->currentRoom, (unsigned)x, (unsigned)y);
}

/* sub_080562D0's last word: a human has arrived in a room. */
void PortLmpArrivedSlow(struct Kirby *k)
{
    if (k->base.unk56 == gLocalPlayerId)
        ListDoors(gLocalPlayerId);
    if (!Active() || !Human(k->base.unk56))
        return;
    if (L->joining & (1u << (k->base.unk56 & 3))) {
        if (L->groupRoom == 0xFFFF || L->groupRoom == k->base.roomId)
            L->joining &= (u16)~(1u << (k->base.unk56 & 3));
        else {
            PortLog("[katam-port] localmp: f%u player %u (just joined) arrives in room %u; brought to the group",
                    (unsigned)gPortSim->frame.frameCount, (unsigned)k->base.unk56 + 1,
                    (unsigned)k->base.roomId);
            return;
        }
    }
    if (L->groupRoom != k->base.roomId)
        PortLog("[katam-port] localmp: f%u player %u arrives in room %u; the group follows",
                (unsigned)gPortSim->frame.frameCount, (unsigned)k->base.unk56 + 1,
                (unsigned)k->base.roomId);
    L->groupRoom = k->base.roomId;
}

/* KATAM_LOCAL_MP_TRACE=N: every N frames, where each human is (testing). */
static int sTrace = -1;

static void Trace(void)
{
    u32 f = gPortSim->frame.frameCount;
    int k;

    if (sTrace < 0) {
        const char *e = getenv("KATAM_LOCAL_MP_TRACE");
        sTrace = e ? atoi(e) : 0;
    }
    if (sTrace <= 0 || f % (u32)sTrace)
        return;
    PortLog("[katam-port] localmp-trace: f%u pauser %u freeze %X keys %03X %03X %03X",
            (unsigned)f, (unsigned)gUnk_0203AD50, (unsigned)gUnk_03000510.unk4, (unsigned)L->keys[0],
            (unsigned)L->keys[1], (unsigned)L->keys[2]);
    for (k = 0; k < 4; k++) {
        const struct Kirby *o = &gKirbys[k];

        if (!Human(k))
            continue;

        PortLog("[katam-port] localmp-trace: f%u p%d room %u x %d y %d hp %d lives %d flags %08X anim %u state %08X cam %d,%d",
                (unsigned)f, k + 1, (unsigned)o->base.roomId, (int)(o->base.x >> 8),
                (int)(o->base.y >> 8), (int)o->hp, (int)o->lives, (unsigned)o->base.flags,
                (unsigned)o->animationIndex, (unsigned)(uintptr_t)o->stateFn,
                (int)(gCurLevelInfo[gLocalPlayerId].viewportPosition.x >> 8),
                (int)(gCurLevelInfo[gLocalPlayerId].viewportPosition.y >> 8));
    }
}

/* Once a frame, at the latch (platform/main.c, after PortWarpPoll). */
void PortLmpFrame(void)
{
    const struct Kirby *anchor = NULL;
    u16 g;
    int k, n;

    if (L->joining & 0x0E00)
        Waiting();
    if (!Active())
        return;
    Trace();
    n = 4;
    g = L->groupRoom;
    if (g == 0xFFFF) {
        /* First frame in a level: the group is wherever player 1 is. */
        if (gKirbys[0].base.roomId == 0xFFFF || !InRoom(&gKirbys[0]))
            return;
        g = L->groupRoom = gKirbys[0].base.roomId;
    }
    /* The first human in the group's room, preferring one who is alive. */
    for (k = 0; k < n; k++)
        if (Human(k) && gKirbys[k].base.roomId == g
            && (anchor == NULL || (anchor->hp <= 0 && gKirbys[k].hp > 0)))
            anchor = &gKirbys[k];
    if (anchor == NULL)
        return;

    /* A human who is dying respawns with the group, not back where they last
     * came through a door: their respawn record (what the death sequence's
     * sub_08055920 reads) becomes the group's -- the anchor's own record when
     * it is in this room, a safe floor spot by construction, else the
     * anchor's cell.  Otherwise their respawn would be an arrival elsewhere
     * and drag everyone back with them. */
    for (k = 0; k < n; k++) {
        struct Kirby *o = &gKirbys[k];

        if (!Human(k) || o == anchor || o->hp > 0 || o->base.roomId == 0xFFFF)
            continue;
        if (anchor->roomId == g) {
            o->roomId = g;
            o->spawnLocation = anchor->spawnLocation;
        } else {
            o->roomId = g;
            o->spawnLocation.x = (s16)((anchor->base.x + 0x800) >> 12);
            o->spawnLocation.y = (s16)((anchor->base.y + 0x701) >> 12);
        }
    }

    /* Everyone into the group's room. */
    for (k = 0; k < n; k++) {
        struct Kirby *o = &gKirbys[k];

        if ((L->joining & (1u << k)) && o->base.roomId == g && InRoom(o))
            L->joining &= (u16)~(1u << k);

        if (Human(k) && o->base.roomId != g && Movable(o)) {
            L->stray[k] = 0;
            Carry(o, g, anchor, 1);
        } else if ((L->joining & (1u << k)) && o->base.roomId != g && o->base.roomId != 0xFFFF
                   && o->hp > 0 && !(o->base.flags & 0x3800000)) {
            /* A joiner that a CPU-only state holds (a CPU parked in the hub,
             * waiting for a call: flags 0xB00, which only its brain -- now
             * frozen -- would end): after two seconds, carried anyway. */
            if (L->stray[k] < 255)
                L->stray[k]++;
            if (L->stray[k] >= 120) {
                L->stray[k] = 0;
                o->base.flags &= ~0xB00u;
                Carry(o, g, anchor, 1);
            }
        }
    }

    /* Stragglers: outside the widest frame of player 1's screen. */
    if (gKirbys[0].base.roomId == g && InRoom(&gKirbys[0])) {
        s32 x0, y0, x1, y1;

        Frame(gLocalPlayerId, &x0, &y0, &x1, &y1);
        for (k = 1; k < n; k++) {
            struct Kirby *o = &gKirbys[k];

            if (!Human(k) || o->base.roomId != g || !InRoom(o)) {
                if (!(L->joining & (1u << k)))      /* its counter, above */
                    L->stray[k] = 0;
                continue;
            }
            if (o->base.x >= x0 + 0x800 && o->base.x <= x1 - 0x800
                && o->base.y >= y0 + 0x800 && o->base.y <= y1 - 0x800) {
                L->stray[k] = 0;
                continue;
            }
            if (L->stray[k] < 255)
                L->stray[k]++;
            if (L->stray[k] >= STRAY_FRAMES && Movable(o)) {
                L->stray[k] = 0;
                Carry(o, g, &gKirbys[0], 0);
            }
        }
    }
}

/* --- presentation ------------------------------------------------------------ */

int PortLmpViewNeed(s32 *w, s32 *h)
{
    u8 p = gLocalPlayerId;
    const struct LevelInfo *li;
    s32 minX, maxX, minY, maxY, rw, rh;
    u16 room;
    int k, n;

    if (!Active() || p >= 4)
        return 0;
    li = &gCurLevelInfo[p];
    room = gKirbys[p].base.roomId;
    if (room == 0xFFFF || li->currentRoom == 0xFFFF)
        return 0;
    n = 4;
    minX = maxX = gKirbys[p].base.x;
    minY = maxY = gKirbys[p].base.y;
    for (k = 0; k < n; k++) {
        const struct Kirby *o = &gKirbys[k];

        if (k == p || !Human(k) || o->base.roomId != room || !InRoom(o))
            continue;
        if (o->base.x < minX) minX = o->base.x;
        if (o->base.x > maxX) maxX = o->base.x;
        if (o->base.y < minY) minY = o->base.y;
        if (o->base.y > maxY) maxY = o->base.y;
    }
    *w = ((maxX - minX) >> 8) + 2 * MARGIN_X;
    *h = ((maxY - minY) >> 8) + 2 * MARGIN_Y;
    /* No wider than the room the camera may show: past that the extra
     * picture would only be letterbox. */
    rw = ((li->unk_S32Vec2_74.x - li->unk_S32Vec2_6C.x) >> 8) + 16;
    rh = ((li->unk_S32Vec2_74.y - li->unk_S32Vec2_6C.y) >> 8) + 8;
    if (rw > 0 && *w > rw) *w = rw;
    if (rh > 0 && *h > rh) *h = rh;
    return 1;
}

#ifdef __cplusplus
}
#endif
