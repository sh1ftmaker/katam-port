/* Adapters for function pointers the game deliberately mis-casts.
 *
 * The decompilation contains casts like
 *
 *     TaskCreate(sub_08001FF8, 0, 1, 0, (TaskDestructor)sub_08002E3C);
 *     gCurTask->main = (TaskMain)sub_0802D528;
 *
 * and they are faithful: on ARM a call simply loads r0-r3 and branches, so
 * handing a no-argument function to something that calls it with one argument
 * costs nothing, and the argument sits unread in r0.
 *
 * WebAssembly checks the callee's type against the call site's and traps if
 * they differ -- `call_indirect to a signature that does not match`, with no
 * indication of which pointer.  This is what stopped the port when Kirby went
 * through a door: TaskDestroy called a destructor that takes no arguments.
 *
 * Each adapter here has the signature the call site expects and forwards to
 * the real function, which is what the hardware was doing implicitly.
 */

#include "port/port.h"
#include "task.h"
#include "data.h"      /* struct Unk_08D6CD0C, for the room-table read below */

/* --- destructors ---------------------------------------------------------
 * Unambiguous: the target takes no arguments, so the pointer ARM left in r0
 * was never read.  Dropping it is exactly equivalent. */

void sub_08002E3C(void);

void PortDtor_sub_08002E3C(struct Task *task)
{
    (void)task;
    sub_08002E3C();
}

/* --- task mains ----------------------------------------------------------
 * These take the task's own struct.  TasksExec calls `gCurTask->main()` with
 * no argument, so on ARM they read whatever r0 happened to hold -- and the
 * only value that makes them work is the current task's struct pointer, which
 * is what every other main in the same files fetches for itself with
 * TaskGetStructPtr(gCurTask).
 *
 * INFERENCE, not proof: the reference assembly for TasksExec would settle
 * whether r0 genuinely holds that at the call.  Every other reading makes
 * these functions dereference garbage on hardware too, so this is the reading
 * that makes the original work.  If an object driven by one of these
 * misbehaves, start here.
 */

struct Unk_0802B4A8;
void sub_0802D528(struct Unk_0802B4A8 *);
void sub_0802D53C(struct Unk_0802B4A8 *);
void sub_0802D550(struct Unk_0802B4A8 *);

void PortMain_sub_0802D528(void) { sub_0802D528(TaskGetStructPtr(gCurTask)); }
void PortMain_sub_0802D53C(void) { sub_0802D53C(TaskGetStructPtr(gCurTask)); }
void PortMain_sub_0802D550(void) { sub_0802D550(TaskGetStructPtr(gCurTask)); }

/* --- reads through a pointer that is not valid yet ------------------------
 *
 * sub_080338B4 builds the HUD before the room is known.  CreateKirby leaves
 * ObjectBase.roomId at 0xFFFF -- sub_0803EA90 sets it deliberately -- and
 * nothing fills it in until sub_08055920, which sub_080332BC calls *after*
 * sub_080338B4 has returned.  So sub_08034FA8 runs this:
 *
 *     gUnk_08D6CD0C[gKirbys[gUnk_0203AD3C].base.base.base.roomId]->unk46
 *
 * with roomId == 0xFFFF.  The index is still inside the ROM (0x08DACD08) and
 * yields 0x100F0D0F, which is not an address the GBA decodes: the console
 * returns open bus and carries on.  Nothing is lost, because sub_080338B4
 * zeroes the very bytes this writes on the next line:
 *
 *     sub_08034FA8(NULL);
 *     CPU_FILL(0, (void *)0x060077A0, 0x100, 16);
 *
 * WebAssembly has no open bus.  0x100F0D55 is past the end of linear memory,
 * so the load traps and takes the game down on Start Game.
 *
 * Checking the pointer against the ROM that was actually loaded restores the
 * hardware's behaviour: a garbage pointer reads as nothing rather than
 * killing the run.  Zero is as good as any other value here, since the copy
 * it feeds is overwritten before anything can display it.
 */

u16 PortRoomTilesetIndex(u16 roomId)
{
    const struct Unk_08D6CD0C *room = gUnk_08D6CD0C[roomId];
    uintptr_t addr = (uintptr_t)room;

    if (addr < GBA_ROM_BASE || addr + sizeof(*room) > GBA_ROM_BASE + gPortRomSize)
        return 0;
    return room->unk46;
}
