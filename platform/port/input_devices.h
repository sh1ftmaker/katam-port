#ifndef GUARD_PORT_INPUT_DEVICES_H
#define GUARD_PORT_INPUT_DEVICES_H
/* Input devices -> players: the host-independent core of drop-in / drop-out
 * local multiplayer.  platform/input_devices.c has the design; the web page
 * has the same core in JavaScript (web/input_devices.js).
 *
 * Not simulation state.  The host feeds every device's GBA key mask once a
 * frame; the core says which player each device drives, turns that into the
 * per-player masks the latch takes (player 1's keys, PortLmpSetHostKeys for
 * players 2-4) and asks the simulation for joins and leaves
 * (PortLmpRequestHuman).  Only those requests and the masks reach the game,
 * both recorded. */

#include "gba/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_DEV_MAX      16
#define PORT_DEV_NAME_MAX 48

/* kinds */
#define PORT_DEV_KEYS_A   1   /* keyboard set A: WASD/arrows, J K, Enter ... */
#define PORT_DEV_KEYS_B   2   /* keyboard set B: arrows + numpad, . , ; ...  */
#define PORT_DEV_TOUCH    3   /* on-screen buttons                            */
#define PORT_DEV_PAD      4   /* a gamepad (XInput and friends, via SDL / the Gamepad API) */

/* What a device drives (PortDevAssign, struct PortDevInfo.player). */
#define PORT_DEV_FOLLOW  (-1) /* nobody claimed it: it drives player 1        */
#define PORT_DEV_PARKED  (-2) /* drives nobody; its Start joins (after a drop out) */
#define PORT_DEV_OFF     (-3) /* ignored entirely until reassigned            */
/* 0..3: claimed by player 1..4 */

/* Frames a player's last device may be gone before the CPU takes the Kirby. */
#define PORT_DEV_GRACE_FRAMES 180

/* --- the host's side ------------------------------------------------------ */
/* A device appeared.  `key` identifies the physical device across unplugging
 * (an SDL joystick GUID + name, the Gamepad API id); devices with the same key
 * are taken as the same one coming back.  Returns the device id (> 0), or 0
 * when the table is full. */
int  PortDevAdd(int kind, const char *key, const char *name);
/* It went away (unplugged).  Its player keeps the Kirby for the grace period;
 * the same key coming back within it takes the player back. */
void PortDevRemove(int id);
/* This frame's buttons of device `id` (GBA mask). */
void PortDevSetKeys(int id, u16 mask);
/* Once a frame, after every PortDevSetKeys: work out joins, leaves and the
 * per-player masks, and hand the masks to the simulation's latch
 * (PortLmpSetHostKeys for players 2-4).  Returns player 1's mask, which the
 * host latches as the console's own keys. */
u16  PortDevFrame(void);
u16  PortDevPlayerKeys(int player);     /* 0..3, as PortDevFrame left them */
/* Forget every device (tests). */
void PortDevReset(void);

/* --- the menu's side (the future PLAYERS page) ----------------------------- */
struct PortDevInfo {
    int  id;
    int  kind;
    int  player;            /* 0..3, or PORT_DEV_FOLLOW / PARKED / OFF        */
    int  present;           /* 0 while unplugged (in the grace period)        */
    u16  keys;              /* this frame's buttons                           */
    char name[PORT_DEV_NAME_MAX];
};
struct PortDevPlayerInfo {
    int  player;            /* 0..3                                           */
    int  human;             /* the simulation says this slot is a person      */
    int  pending;           /* a join or leave was asked for, not applied yet */
    int  graceLeft;         /* frames until the CPU takes it (devices gone)   */
    int  numDevices;
    int  devices[PORT_DEV_MAX];   /* ids                                      */
    char names[96];         /* "Xbox Controller, Keyboard (arrows)"           */
};
int  PortDevList(struct PortDevInfo *out, int max);            /* -> count */
int  PortDevListPlayers(struct PortDevPlayerInfo out[4]);       /* -> 4 */
/* Move a device to player 0..3, or to FOLLOW / PARKED / OFF.  Moving one to a
 * player who is not in the game yet asks for the join; taking the last
 * device off a player does NOT make them leave (use PortDevDropOut).  Returns
 * 0 if the device does not exist or the join cannot be asked for. */
int  PortDevAssign(int id, int player);
/* Player 2..4 (1..3) leaves: their devices are parked (Start rejoins) and the
 * CPU takes the Kirby.  Player 1 cannot leave.  Returns 0 if refused. */
int  PortDevDropOut(int player);
/* The join press: Start on a gamepad that is not the one player 1 is using,
 * or keyboard set B's Start.  Exposed for a menu "join" button too. */
int  PortDevJoin(int id);
/* The device player 1 drives with right now (the last unclaimed one used). */
int  PortDevPlayer1Device(void);

/* --- the simulation's side, replaceable for tests -------------------------- */
struct PortDevSink {
    int (*slotIsHuman)(int slot);           /* PortLmpSlotIsHuman             */
    u32 (*request)(int slot, int on);       /* PortLmpRequestHuman            */
    int (*canRequest)(void);                /* PortLmpCanRequest              */
    void (*hostKeys)(int player, u16 mask); /* PortLmpSetHostKeys             */
    int (*slotJoinable)(int slot);          /* PortLmpSlotJoinable            */
};
void PortDevSetSink(const struct PortDevSink *sink);   /* NULL = the game's */
/* Log join/leave/plug decisions (PortLog).  On by default. */
void PortDevSetVerbose(int on);

#ifdef __cplusplus
}
#endif
#endif /* GUARD_PORT_INPUT_DEVICES_H */
