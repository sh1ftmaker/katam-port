/* Putting Kirby in an arbitrary room.
 *
 * There are 286 rooms in the game and a scripted run reaches a handful.  That
 * is the binding constraint on almost every question anyone wants to ask about
 * this game at runtime -- what an object does, which globals move when it does
 * it, whether a randomized boss fight actually works -- because all of those
 * need the game to *be somewhere* first, and getting somewhere means playing
 * there.  18 rooms have no incoming door edge at all, so for those, playing
 * there is not even possible.  (33 rooms have no *outgoing* door edge; the two
 * counts are different measurements and an earlier version of this comment
 * conflated them.)
 *
 * --- why this is not three field writes -----------------------------------
 *
 * Taking a door writes roomId, x and y on the Kirby (src/kirby.c:12877-12880),
 * and writing those three fields from outside does nothing.  The write sticks
 * -- read gKirbys[0].roomId back and it holds the new room -- and the level
 * never changes, because the loader is not driven by that field.
 *
 * A room change is a Kirby *state*.  sub_08055920 installs sub_08055D9C as the
 * state function (kirby->base.base.unk78), and sub_08055D9C calls sub_080562D0
 * on the next frame.  That function assigns roomId from three different
 * branches, and which one runs decides whether this works at all.
 *
 * This takes the RESPAWN branch: it reads the destination from
 * kirby->roomId and kirby->spawnLocation, the same two fields a warp star
 * writes (src/warp_star.c:1801) and the same ones the game reads when Kirby
 * respawns after dying.  The port fills them in and calls the game's own entry
 * point; the fade, the object teardown, FillLevelInfo and the respawn are all
 * the game's code doing what it always does, so the state it lands in is the
 * state a player would have arrived in.
 *
 * The same shape as platform/rando.c: the port supplies data, the game's own
 * code does the work.  Nothing here reimplements a transition.
 *
 * --- the door route, tried first and abandoned ----------------------------
 *
 * The obvious route is the DOOR branch: synthesise a door descriptor, point
 * kirby->unk10C at it, set unk58 & 0x4000 and unk62 & 0x10, and let the game
 * take the door.  That is what this file did first, and it hangs.
 *
 * The door branch also marks the door used on the pause map, via
 * sub_08002C18, which walks the *source* room's progress list looking for the
 * destination with `for (i = 0;; i++)` and no terminator test
 * (src/code_080023A4.c:1646).  The terminator's stride byte is zero, so a
 * destination the source room has no door to does not miss -- the pointer
 * stops advancing and the game spins forever inside one frame.
 *
 * A synthesised door is never in that table.  So the door route can only
 * deliver Kirby to destinations the current room already has a real door to,
 * which is exactly the limitation a warp exists to remove.  It looked like it
 * worked because the room a scripted run ends up in has precisely one door --
 * to room 804, cell (2,5) -- and that was the only warp anyone had tried.
 * docs/naming/warp-hang.md is the full account.
 */

#include "global.h"
#include "data.h"
#include "kirby.h"
#include "port/port.h"

/* Not in any header the port copies; code_080332BC.c externs it the same way,
 * which is what proves the symbol is external and links. */
extern void sub_08055920(struct Kirby *);

/* Set when a warp has been asked for but the game has not reached a state
 * where it can be delivered.  See PortWarpPoll. */
static u16 sPendingRoom;
static u8 sPendingX, sPendingY, sPendingPlayer;
static bool8 sPending;

u32 gPortWarpCount;

static bool8 CanWarpNow(struct Kirby *kirby)
{
    /* A warp is only meaningful once the player is in a level: before that
     * there is no room to leave, gCurLevelInfo holds 0xFFFF, and installing a
     * transition state would run it against a level that does not exist. */
    if (kirby->base.base.base.roomId == 0xFFFF)
        return FALSE;
    if (gCurLevelInfo[kirby->base.base.base.unk56].currentRoom == 0xFFFF)
        return FALSE;
    /* Already mid-transition -- flags bit 8 is the door/warp state, and
     * starting a second one on top of it is how you get two room loads racing
     * for one slot. */
    if (kirby->base.base.base.flags & 0x100)
        return FALSE;
    return TRUE;
}

static void Deliver(u8 playerId, u16 room, u8 cellX, u8 cellY)
{
    struct Kirby *kirby = &gKirbys[playerId];

    sub_08055920(kirby);

    /* Take the RESPAWN branch of sub_080562D0 (src/kirby.c:12913), not the
     * door branch (:12867).  The door branch calls sub_08002C18 to light the
     * door up on the pause map, and that function walks the source room's
     * progress list with `for (i = 0;; i++)` and no terminator test
     * (src/code_080023A4.c:1646).  The terminator's stride byte is 0, so a
     * destination the room has no door to is not a miss -- the pointer stops
     * advancing and the game spins forever inside one frame, writing nothing.
     *
     * A synthesised door is by definition not in that table, so the door
     * branch can only ever deliver Kirby to the current room's real door
     * destinations, which is the one thing PortWarp exists to avoid.  That is
     * why the first version of this file appeared to work: the room a scripted
     * run is standing in has exactly one door, to room 804 cell (2,5), and
     * that was the only warp that had ever been tried.
     * See docs/naming/warp-hang.md.
     *
     * Both bits must be actively CLEARED, not merely left unset: Kirby can be
     * standing on a real door tile when the warp is asked for, and then the
     * collision sweep has already set unk58 & 0x4000 for him.  sub_08055920
     * sets flags bit 8, which makes the sweep skip this object, so what is
     * written here survives to the frame sub_080562D0 runs on. */
    kirby->base.base.base.unk58 &= ~0x4000;
    kirby->base.base.base.unk62 &= ~0x10;

    /* The destination, in the fields that branch reads.  Note the field-name
     * collision: base.base.base.roomId at +0x60 is the live object's room and
     * writing it from outside does nothing, while kirby->roomId at +0x106 is
     * the respawn record, which is an input the transition state reads.  This
     * writes the second.  A warp star writes the same two fields
     * (src/warp_star.c:1801). */
    kirby->roomId = room;
    kirby->spawnLocation.x = cellX;
    kirby->spawnLocation.y = cellY;

    /* sub_08055920 sets flyTimer to 5 for two animation states (:12593), and a
     * non-zero flyTimer makes the respawn branch overwrite the respawn record
     * from unkF6/unkF8/unkFA (:12921).  This warp still lands correctly -- that
     * write happens after the position is taken -- but the *next* one would
     * inherit a respawn point the port never chose. */
    kirby->flyTimer = 0;

    gPortWarpCount++;
    PortLog("[katam-port] warp: player %u -> room %u at cell (%u,%u)",
            (unsigned)playerId, (unsigned)room,
            (unsigned)cellX, (unsigned)cellY);
}

void PortWarp(u8 playerId, u16 room, u8 cellX, u8 cellY)
{
    if (playerId >= 4) {
        PortLog("[katam-port] warp: player %u does not exist", (unsigned)playerId);
        return;
    }

    if (CanWarpNow(&gKirbys[playerId])) {
        Deliver(playerId, room, cellX, cellY);
        return;
    }

    /* Asking before the game is ready is the normal case for a scripted run --
     * the caller counts frames and cannot know which frame the level appears
     * on.  Rather than fail, hold the request and let PortWarpPoll deliver it
     * on the first frame it can.  A silently dropped warp would look exactly
     * like a warp that worked and did nothing. */
    sPending = TRUE;
    sPendingPlayer = playerId;
    sPendingRoom = room;
    sPendingX = cellX;
    sPendingY = cellY;
    PortLog("[katam-port] warp: room %u queued, waiting for a level",
            (unsigned)room);
}

/* Called once a frame from the port's frame loop. */
void PortWarpPoll(void)
{
    if (!sPending)
        return;
    if (!CanWarpNow(&gKirbys[sPendingPlayer]))
        return;
    sPending = FALSE;
    Deliver(sPendingPlayer, sPendingRoom, sPendingX, sPendingY);
}

void PortWarpReport(void)
{
    if (sPending)
        PortLog("[katam-port] warp: room %u never delivered -- the game did "
                "not reach a level", (unsigned)sPendingRoom);
    else if (gPortWarpCount)
        PortLog("[katam-port] warp: %u delivered", (unsigned)gPortWarpCount);
}
