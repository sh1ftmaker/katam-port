#ifndef GUARD_PORT_SEATS_H
#define GUARD_PORT_SEATS_H
/* C linkage for the 64-bit builds -- see platform/port/backend.h. */
#ifdef __cplusplus
extern "C" {
#endif

#include "gba/types.h"

/* ---------------------------------------------------------------------------
 * Seats: several game states through one instance.
 *
 * A server that runs the world for everyone (docs/SERVER-RENDER.md) needs one
 * picture per player, and a picture is not a function of the shared
 * simulation alone: the game streams the *local* Kirby's room into VRAM
 * incrementally, so each player's VRAM, OAM and scroll registers are the
 * product of their own history.  Four players are four game states.
 *
 * Four instances would be four copies of a 16 MiB ROM and four sets of V8
 * code inside a 128 MiB Durable Object.  So instead: one instance, and the
 * rollback engine's snapshot -- 387 KiB, six memcpys, valid at the frame
 * boundary inside PortPresentFrame -- used as a context switch.  Every frame,
 * every seat in turn: restore its state, pin the camera to its Kirby, feed it
 * the round's inputs, run the frame, present its picture, save its state.
 * The seats share the ROM, the code and the input timeline; nothing else.
 *
 * A "round" is one game frame for every seat.  PortPresentFrame is entered
 * once per seat per round; the host sees one PortBlitFramebuffer per seat,
 * tagged by PortSeatCurrent(), and one vsync wait per round, after the last
 * seat's blit.
 *
 * Where the switch happens, and why it is exact: a seat's state is saved at
 * the end of PortPresentFrame, after its VBlank handler and before the host's
 * key latch, and the next seat's state is loaded at the same point.  The
 * restored seat therefore resumes at precisely the instruction it was saved
 * at, with the same stack, and runs its GameLoop iteration and the next
 * PortPresentFrame up to that point again.  Its inputs and camera are applied
 * at the top of PortPresentFrame -- the same point the rollback engine applies
 * a timeline frame -- so a seat's frame is bit-for-bit what a separate
 * instance would have computed (tools/seat_test.mjs is the proof: four seats
 * against four real instances, every framebuffer, every frame).
 *
 * Inputs: PortSeatSetInput(slot, keys) sets a slot's buttons for the *next*
 * round; they are latched when seat 0 begins it.  A slot nobody is driving
 * takes the game's own AI, exactly as an unseated Kirby does under rollback.
 * The rollback engine must be shut down before PortSeatInit -- the seats
 * take over its two jobs (inputs and focus) and its ring would otherwise
 * snapshot whichever seat happened to be loaded.
 * ------------------------------------------------------------------------- */

#include "port/simblock.h"
#define PORT_SEAT_MAX PORT_MAX_SEATS

/* Clone the current game state into `seats` seats and start multiplexing
 * from the next frame.  Call from inside the host's present callback (the
 * state is at the frame boundary there) with no rollback session active,
 * after PortSeatSetInput has been given the inputs the *current* frame was
 * simulated with -- they are what each seat's console latches first.
 * Returns non-zero on success. */
int  PortSeatInit(int seats);
void PortSeatShutdown(void);
int  PortSeatActive(void);
int  PortSeatCount(void);

/* The seat whose frame is running -- and, inside PortBlitFramebuffer, whose
 * picture that is. */
int  PortSeatCurrent(void);
int  PortSeatIsLast(void);          /* the round ends with this seat        */

/* Buttons for a slot in the next round, and whether anyone is driving it. */
void PortSeatSetInput(int slot, u16 keys);
void PortSeatSetDriven(int slot, int driven);
void PortSeatSetPlayerCount(int count);   /* Kirbys in play, 1..4 (default 4) */

/* --- the frame hooks, called from PortPresentFrame only ----------------- */
void PortSeatBeginFrame(void);      /* at the top: camera and inputs        */
void PortSeatEndFrame(void);        /* at the end: save, switch, load       */
int  PortSeatKeyOverride(u16 *keys);

#ifdef __cplusplus
}
#endif
#endif /* GUARD_PORT_SEATS_H */
