/* Optional run-time checks, built with CHECK_POINTERS=1.
 *
 * The port's failure mode is a trap at an address nothing on the JS side can
 * explain: WebAssembly reports an out-of-bounds access with no faulting
 * address, and on Safari the wasm frames are swallowed by the Asyncify
 * wrapper, so the crash report names `original(...args)` and nothing else.
 *
 * Rather than improve the autopsy, this catches the pointer where it is born.
 * Every object in the game reaches its own state through one macro:
 *
 *     #define TaskGetStructPtr(taskp)                             \
 *         ((taskp)->flags & TASK_USE_EWRAM                        \
 *         ? (void *)EWRAM_START + ((taskp)->structOffset << 2)    \
 *         : (void *)IWRAM_START + (taskp)->structOffset)
 *
 * 1086 call sites, one definition.  Under CHECK_POINTERS the macro is
 * redirected here, where the result is range-checked before anyone writes
 * through it.  A bad pointer is reported with the whole task that produced it
 * and then replaced with scratch memory, so the run continues and one session
 * shows every offender instead of only the first.
 */

#include <stdio.h>
#include <string.h>

#include "port/port.h"
#include "task.h"

/* C linkage for the 64-bit builds.
 *
 * This file sits on the seam in both directions: it calls the game's functions
 * and the game calls its adapters back (tools/portify.py rewrites the call
 * sites).  Those builds compile the game as C++ with C linkage -- see
 * tools/cxxify.py -- so this file has to agree, or the calls mangle one way
 * and the definitions the other.  A no-op in C. */
#ifdef __cplusplus
extern "C" {
#endif

/* Somewhere harmless to send writes that would otherwise leave the map.
 * Sized for the largest task struct the game allocates. */
static u8 sScratch[0x800];

static u32 sReported;
static u32 sBadCalls;

void PortReportPointerChecks(void)
{
    if (sBadCalls != 0)
        PortLog("[katam-port] %u task-pointer failures (%u distinct reported)",
                sBadCalls, sReported);
}

void *PortTaskStruct(struct Task *task)
{
    uintptr_t addr;
    int inEwram, inIwram;

    if (task == NULL) {
        sBadCalls++;
        if (sReported < 20) {
            sReported++;
            PortError("[katam-port] TaskGetStructPtr(NULL)");
        }
        return sScratch;
    }

    if (task->flags & TASK_USE_EWRAM)
        addr = GBA_EWRAM_BASE + ((uintptr_t)task->structOffset << 2);
    else
        addr = GBA_IWRAM_BASE + task->structOffset;

    inEwram = addr >= GBA_EWRAM_BASE && addr < GBA_EWRAM_BASE + GBA_EWRAM_SIZE;
    inIwram = addr >= GBA_IWRAM_BASE && addr < GBA_IWRAM_BASE + GBA_IWRAM_SIZE;

    if (inEwram || inIwram)
        return (void *)addr;

    sBadCalls++;
    if (sReported < 20) {
        sReported++;
        /* The whole task, because which field is wrong is the question:
         * a bogus structOffset means the allocator, a bogus flags means the
         * task itself has been overwritten. */
        PortError("[katam-port] task pointer leaves the map: 0x%08X\n"
                "    task at %p: parent=%u prev=%u next=%u structOffset=0x%04X\n"
                "    main=%p dtor=%p priority=%u flags=0x%04X",
                (unsigned)addr, (void *)task,
                task->parent, task->prev, task->next, task->structOffset,
                (void *)task->main, (void *)task->dtor,
                task->priority, task->flags);
    }
    return sScratch;
}

/* --- task destructors ----------------------------------------------------
 *
 * TaskDestroy ends with `if (task->dtor != NULL) task->dtor(task);`, and that
 * is what trapped when Kirby went through a door.  Wasm reports "null function
 * or function signature mismatch" without saying which pointer or which task,
 * and the two causes need opposite fixes -- a mistyped function is a
 * declaration bug, a nonsense pointer means the task was overwritten.
 *
 * Comparing against the destructors the game installs is not possible from
 * here: nearly all of them are `static`, so their addresses cannot be taken
 * from another translation unit.  Instead the last call is recorded at a fixed
 * address inside the reserved map -- unused space above save memory and below
 * everything the compiler owns -- where the harness can read it back after the
 * trap.  It costs four stores per destructor call and needs no linkage at all.
 */

#define PORT_TRACE ((volatile u32 *)0x09800000u)

void PortCallDtor(struct Task *task)
{
    if (task->dtor == NULL)
        return;

    PORT_TRACE[0] = 0x44544F52u;            /* 'DTOR', so the harness can tell
                                             * a real record from stale bytes */
    PORT_TRACE[1] = (u32)task;
    PORT_TRACE[2] = (u32)task->dtor;
    PORT_TRACE[3] = (u32)task->main;
    PORT_TRACE[4] = task->flags | ((u32)task->priority << 16);
    PORT_TRACE[5] = task->structOffset | ((u32)task->parent << 16);
    PORT_TRACE[6]++;                        /* how many got this far */

    task->dtor(task);
}

#ifdef __cplusplus
}
#endif
