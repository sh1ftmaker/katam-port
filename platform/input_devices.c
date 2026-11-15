/* Input devices -> players: drop-in / drop-out local multiplayer, the host's
 * half.  Header: platform/port/input_devices.h.  The JavaScript twin for the
 * web page is web/input_devices.js; the two keep the same rules.
 *
 * --- what a device is -------------------------------------------------------
 *
 * Anything that produces a GBA key mask once a frame: keyboard set A (WASD,
 * J/K, Enter...), keyboard set B (arrows + numpad and . , ; '), the touch
 * buttons, each gamepad.  The host says when one appears (PortDevAdd, with a
 * key that recognises the same pad coming back) and goes (PortDevRemove), and
 * gives its mask every frame (PortDevSetKeys).
 *
 * --- who drives whom ---------------------------------------------------------
 *
 * A device either belongs to a player (0..3) or is FOLLOW: nobody claimed it,
 * so it drives player 1 -- the default, and today's behaviour for a single
 * player with a keyboard and a pad.  PARKED drives nobody but can join (a
 * device whose player dropped out, so a pad put down does not move player 1);
 * OFF is ignored altogether.
 *
 * Drop in: Start pressed on a FOLLOW gamepad that is not the device player 1
 * is using (the last FOLLOW device to press anything), or keyboard set B's
 * Start, or Start on a PARKED device, makes that device a new player on the
 * next free slot -- the slot it last had if free, else the lowest.  The press
 * is swallowed (it neither pauses player 1's game nor the joiner's) until the
 * button is let go.  The first device anybody touches is player 1's, so the
 * pad that starts the game from the title never "joins".
 *
 * Drop out: PortDevDropOut (the menu), or a device unplugged: when a player's
 * last device has been gone PORT_DEV_GRACE_FRAMES, the CPU takes the Kirby
 * and the device is remembered as PARKED; plugged back in, Start rejoins (the
 * same slot if it is still free).  Back within the grace period, it simply
 * carries on.
 *
 * --- what crosses into the game ----------------------------------------------
 *
 * Only two things, both recorded: the per-player masks (player 1's through
 * the console's own keys, 2-4 through PortLmpSetHostKeys and the latch's
 * `kp` lines) and join/leave requests (PortLmpRequestHuman -> `human` lines).
 * The assignment itself is the host's: a replay or another device never needs
 * it.  The core asks the simulation (PortLmpSlotIsHuman) which slots are
 * people and keeps it so: a player with a device whose slot is not a person
 * (a join pressed on the title screen, a new game after one) is asked for
 * again; a slot that is a person without ever having had a device here
 * (MULTIPLAYER > LOCAL from the game's own menu) is given the first spare
 * device -- a gamepad player 1 is not using, else keyboard set B -- which is
 * what the old "pad 2 / the arrows are player 2" rule did.
 */

#include <stdio.h>
#include <string.h>

#include "port/port.h"
#include "port/input_devices.h"
#include "port/localmp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define START 0x0008

struct Dev {
    int  used, id, kind, player, present;
    int  lastSlot;          /* the player slot it last drove, -1 none          */
    int  swallow;           /* Start is held from a join press: not passed on  */
    u16  keys, prev;
    char key[64];
    char name[PORT_DEV_NAME_MAX];
};

static struct Dev sDev[PORT_DEV_MAX];
static int sNextId = 1;
static int sP1Dev;                       /* id, 0 = none yet                     */
static int sGrace[4];                    /* frames left, 0 = none                */
static int sPend[4], sPendOn[4];         /* a request in flight                  */
static int sBound[4];                    /* has had a device since it became human */
static u16 sOut[4];
static int sVerbose = 1;

static u32 GameRequest(int slot, int on) { return PortLmpRequestHuman(slot, on); }
static const struct PortDevSink kGame = {
    PortLmpSlotIsHuman, GameRequest, PortLmpCanRequest, PortLmpSetHostKeys, PortLmpSlotJoinable
};
static const struct PortDevSink *sSink = &kGame;

void PortDevSetSink(const struct PortDevSink *sink) { sSink = sink ? sink : &kGame; }
void PortDevSetVerbose(int on) { sVerbose = on; }

#define LOG(...) do { if (sVerbose) PortLog(__VA_ARGS__); } while (0)

static struct Dev *Find(int id)
{
    int i;

    for (i = 0; i < PORT_DEV_MAX; i++)
        if (sDev[i].used && sDev[i].id == id)
            return &sDev[i];
    return NULL;
}

void PortDevReset(void)
{
    memset(sDev, 0, sizeof sDev);
    memset(sGrace, 0, sizeof sGrace);
    memset(sPend, 0, sizeof sPend);
    memset(sBound, 0, sizeof sBound);
    memset(sOut, 0, sizeof sOut);
    sP1Dev = 0;
    sNextId = 1;
}

static void Copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

int PortDevAdd(int kind, const char *key, const char *name)
{
    int i, freeSlot = -1, oldest = -1;
    struct Dev *d;

    /* The same physical device coming back. */
    if (key && key[0])
        for (i = 0; i < PORT_DEV_MAX; i++) {
            d = &sDev[i];
            if (d->used && !d->present && d->kind == kind && strcmp(d->key, key) == 0) {
                d->present = 1;
                d->keys = d->prev = 0;
                Copy(d->name, sizeof d->name, name);
                if (d->player >= 1)
                    sGrace[d->player] = 0;
                LOG("[katam-port] devices: %s is back (%s)", d->name,
                    d->player >= 0 ? "still playing" : d->player == PORT_DEV_PARKED
                    ? "parked: Start rejoins" : "player 1");
                return d->id;
            }
        }
    for (i = 0; i < PORT_DEV_MAX; i++) {
        if (!sDev[i].used) { freeSlot = i; break; }
        if (!sDev[i].present && sDev[i].player < 1 && oldest < 0)
            oldest = i;
    }
    if (freeSlot < 0)
        freeSlot = oldest;
    if (freeSlot < 0)
        return 0;
    d = &sDev[freeSlot];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->id = sNextId++;
    d->kind = kind;
    d->player = PORT_DEV_FOLLOW;
    d->present = 1;
    d->lastSlot = -1;
    Copy(d->key, sizeof d->key, key);
    Copy(d->name, sizeof d->name, name);
    LOG("[katam-port] devices: %s connected (device %d)", d->name, d->id);
    return d->id;
}

/* Does player p have a device, present (or, with `absentToo`, unplugged)? */
static int HasDevice(int p, int absentToo)
{
    int i;

    for (i = 0; i < PORT_DEV_MAX; i++)
        if (sDev[i].used && sDev[i].player == p && (sDev[i].present || absentToo))
            return 1;
    return 0;
}

void PortDevRemove(int id)
{
    struct Dev *d = Find(id);

    if (d == NULL || !d->present)
        return;
    d->present = 0;
    d->keys = d->prev = 0;
    d->swallow = 0;
    if (sP1Dev == id)
        sP1Dev = 0;
    if (d->player >= 1 && !HasDevice(d->player, 0)) {
        sGrace[d->player] = PORT_DEV_GRACE_FRAMES;
        LOG("[katam-port] devices: %s unplugged -- player %d's Kirby goes to the CPU in %d frames",
            d->name, d->player + 1, PORT_DEV_GRACE_FRAMES);
    } else {
        LOG("[katam-port] devices: %s unplugged", d->name);
    }
}

void PortDevSetKeys(int id, u16 mask)
{
    struct Dev *d = Find(id);

    if (d != NULL && d->present)
        d->keys = (u16)(mask & 0x03FF);
}

/* Ask the simulation; remember it so the reconciliation waits for it. */
static int Ask(int slot, int on)
{
    if (!sSink->canRequest())
        return 0;
    if (sSink->request(slot, on) == 0)
        return 0;
    sPend[slot] = 60;
    sPendOn[slot] = on;
    return 1;
}

/* A slot nobody's device is on: the one this device last had, else the lowest
 * person-without-a-device (MULTIPLAYER > LOCAL), else the lowest CPU. */
static int FreeSlot(const struct Dev *d)
{
    int s;

    if (d->lastSlot >= 1 && !HasDevice(d->lastSlot, 1) && !(sPend[d->lastSlot] && !sPendOn[d->lastSlot]))
        return d->lastSlot;
    for (s = 1; s < 4; s++)
        if (!HasDevice(s, 1) && sSink->slotIsHuman(s) && !(sPend[s] && !sPendOn[s]))
            return s;
    /* A CPU Kirby the story is not holding first (the intro walks some
     * into the hub; a join there waits until it lets go). */
    for (s = 1; s < 4; s++)
        if (!HasDevice(s, 1) && !sSink->slotIsHuman(s)
            && (sSink->slotJoinable == NULL || sSink->slotJoinable(s)))
            return s;
    for (s = 1; s < 4; s++)
        if (!HasDevice(s, 1) && !sSink->slotIsHuman(s))
            return s;
    return -1;
}

static void Claim(struct Dev *d, int s)
{
    d->player = s;
    if (s >= 1) {
        d->lastSlot = s;
        sBound[s] = 1;
        sGrace[s] = 0;
    }
    if (sP1Dev == d->id)
        sP1Dev = 0;
}

int PortDevJoin(int id)
{
    struct Dev *d = Find(id);
    int s;

    if (d == NULL || !d->present || d->player == PORT_DEV_OFF || d->player >= 1)
        return 0;
    if (!sSink->canRequest()) {
        LOG("[katam-port] devices: %s cannot join now (replay or online session)", d->name);
        return 0;
    }
    s = FreeSlot(d);
    if (s < 0) {
        LOG("[katam-port] devices: %s cannot join -- four players already", d->name);
        return 0;
    }
    if (!sSink->slotIsHuman(s) && !Ask(s, 1))
        return 0;
    Claim(d, s);
    LOG("[katam-port] devices: %s joins as player %d", d->name, s + 1);
    return 1;
}

int PortDevDropOut(int player)
{
    int i;

    if (player < 1 || player > 3)
        return 0;
    for (i = 0; i < PORT_DEV_MAX; i++)
        if (sDev[i].used && sDev[i].player == player)
            sDev[i].player = PORT_DEV_PARKED;
    sGrace[player] = 0;
    sBound[player] = 0;
    if (sSink->slotIsHuman(player))
        Ask(player, 0);
    LOG("[katam-port] devices: player %d drops out; the CPU takes the Kirby", player + 1);
    return 1;
}

int PortDevAssign(int id, int player)
{
    struct Dev *d = Find(id);

    if (d == NULL || player < PORT_DEV_OFF || player > 3)
        return 0;
    if (player >= 1 && !sSink->slotIsHuman(player) && !Ask(player, 1))
        return 0;
    if (player >= 0)
        Claim(d, player);
    else {
        d->player = player;
        if (sP1Dev == id && player != PORT_DEV_FOLLOW)
            sP1Dev = 0;
    }
    LOG("[katam-port] devices: %s -> %s", d->name,
        player >= 0 ? (player == 0 ? "player 1" : player == 1 ? "player 2"
                       : player == 2 ? "player 3" : "player 4")
        : player == PORT_DEV_FOLLOW ? "player 1 (unclaimed)"
        : player == PORT_DEV_PARKED ? "parked" : "off");
    return 1;
}

int PortDevPlayer1Device(void)
{
    return sP1Dev;
}

static int JoinPress(const struct Dev *d, u16 pressed)
{
    if (!(pressed & START))
        return 0;
    if (d->player == PORT_DEV_PARKED)
        return d->kind == PORT_DEV_PAD || d->kind == PORT_DEV_KEYS_B || d->kind == PORT_DEV_KEYS_A;
    if (d->player != PORT_DEV_FOLLOW)
        return 0;
    if (d->kind == PORT_DEV_KEYS_B)
        return 1;
    return d->kind == PORT_DEV_PAD && sP1Dev != 0 && sP1Dev != d->id;
}

u16 PortDevFrame(void)
{
    int i, s;
    u16 out[4] = { 0, 0, 0, 0 };

    /* Joins first, against the device player 1 was using until now. */
    for (i = 0; i < PORT_DEV_MAX; i++) {
        struct Dev *d = &sDev[i];
        u16 pressed;

        if (!d->used || !d->present)
            continue;
        pressed = (u16)(d->keys & ~d->prev);
        if (JoinPress(d, pressed) && PortDevJoin(d->id))
            d->swallow = 1;
    }
    /* Player 1's device: the last unclaimed one to press anything. */
    for (i = 0; i < PORT_DEV_MAX; i++) {
        struct Dev *d = &sDev[i];

        if (d->used && d->present && d->player == PORT_DEV_FOLLOW && (d->keys & ~d->prev))
            sP1Dev = d->id;
    }

    /* Players whose devices are gone. */
    for (s = 1; s < 4; s++) {
        if (!sGrace[s])
            continue;
        if (HasDevice(s, 0)) {
            sGrace[s] = 0;
            continue;
        }
        if (--sGrace[s] == 0) {
            for (i = 0; i < PORT_DEV_MAX; i++)
                if (sDev[i].used && sDev[i].player == s)
                    sDev[i].player = PORT_DEV_PARKED;
            sBound[s] = 0;
            LOG("[katam-port] devices: player %d's controller did not come back; the CPU takes the Kirby",
                s + 1);
            if (sSink->slotIsHuman(s))
                Ask(s, 0);
        }
    }

    /* Keep the simulation's slots and the devices in step. */
    for (s = 1; s < 4; s++) {
        int human = sSink->slotIsHuman(s);

        if (!human)
            sBound[s] = HasDevice(s, 1);
        if (sPend[s]) {
            if (!human == !sPendOn[s] || --sPend[s] == 0)
                sPend[s] = 0;
            continue;
        }
        if (HasDevice(s, 1) && !human) {
            if (sSink->canRequest())
                Ask(s, 1);
        } else if (human && !HasDevice(s, 1) && !sBound[s]) {
            /* A person with no device here: the game's own LOCAL start. */
            struct Dev *spare = NULL;

            for (i = 0; i < PORT_DEV_MAX && spare == NULL; i++)
                if (sDev[i].used && sDev[i].present && sDev[i].player == PORT_DEV_FOLLOW
                    && sDev[i].kind == PORT_DEV_PAD && sDev[i].id != sP1Dev)
                    spare = &sDev[i];
            for (i = 0; i < PORT_DEV_MAX && spare == NULL; i++)
                if (sDev[i].used && sDev[i].present && sDev[i].player == PORT_DEV_FOLLOW
                    && sDev[i].kind == PORT_DEV_KEYS_B)
                    spare = &sDev[i];
            if (spare != NULL) {
                Claim(spare, s);
                LOG("[katam-port] devices: player %d is a person with no controller; %s is theirs",
                    s + 1, spare->name);
            }
        }
    }

    /* The masks. */
    for (i = 0; i < PORT_DEV_MAX; i++) {
        struct Dev *d = &sDev[i];
        u16 k;

        if (!d->used || !d->present)
            continue;
        k = d->keys;
        if (d->swallow) {
            if (!(k & START))
                d->swallow = 0;
            k &= (u16)~START;
        }
        d->prev = d->keys;
        if (d->player == PORT_DEV_FOLLOW)
            out[0] |= k;
        else if (d->player >= 0 && d->player <= 3)
            out[d->player] |= k;
    }
    for (s = 0; s < 4; s++)
        sOut[s] = out[s];
    for (s = 1; s < 4; s++)
        sSink->hostKeys(s, out[s]);
    return out[0];
}

u16 PortDevPlayerKeys(int player)
{
    return player >= 0 && player < 4 ? sOut[player] : 0;
}

int PortDevList(struct PortDevInfo *out, int max)
{
    int i, n = 0;

    for (i = 0; i < PORT_DEV_MAX && n < max; i++) {
        const struct Dev *d = &sDev[i];

        if (!d->used)
            continue;
        out[n].id = d->id;
        out[n].kind = d->kind;
        out[n].player = d->player;
        out[n].present = d->present;
        out[n].keys = d->keys;
        Copy(out[n].name, sizeof out[n].name, d->name);
        n++;
    }
    return n;
}

int PortDevListPlayers(struct PortDevPlayerInfo out[4])
{
    int p, i;

    for (p = 0; p < 4; p++) {
        struct PortDevPlayerInfo *o = &out[p];
        size_t len = 0;

        memset(o, 0, sizeof *o);
        o->player = p;
        o->human = sSink->slotIsHuman(p);
        o->pending = p >= 1 && sPend[p] != 0;
        o->graceLeft = p >= 1 ? sGrace[p] : 0;
        for (i = 0; i < PORT_DEV_MAX; i++) {
            const struct Dev *d = &sDev[i];

            if (!d->used || !(d->player == p || (p == 0 && d->player == PORT_DEV_FOLLOW)))
                continue;
            if (!d->present && d->player != p)
                continue;
            o->devices[o->numDevices++] = d->id;
            if (len < sizeof o->names - 1)
                len += (size_t)snprintf(o->names + len, sizeof o->names - len, "%s%s%s",
                                        len ? ", " : "", d->name, d->present ? "" : " (unplugged)");
        }
    }
    return 4;
}

#ifdef __cplusplus
}
#endif
