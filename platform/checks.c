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
            PortLog("[katam-port] TaskGetStructPtr(NULL)");
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
        PortLog("[katam-port] task pointer leaves the map: 0x%08X\n"
                "    task at %p: parent=%u prev=%u next=%u structOffset=0x%04X\n"
                "    main=%p dtor=%p priority=%u flags=0x%04X",
                (unsigned)addr, (void *)task,
                task->parent, task->prev, task->next, task->structOffset,
                (void *)task->main, (void *)task->dtor,
                task->priority, task->flags);
    }
    return sScratch;
}
