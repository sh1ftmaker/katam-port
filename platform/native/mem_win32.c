/* The Win32 half of the memory seam.
 *
 * Four functions.  Everything about *what* to reserve and *why it is safe* is
 * in platform/native/mem.c; this file only knows how to ask the kernel.
 *
 * How Windows differs from the POSIX side
 * ---------------------------------------
 * VirtualAlloc with an explicit base never relocates.  That is the one thing
 * mem_posix.c has to work for -- MAP_FIXED_NOREPLACE, and a check afterwards
 * because an old kernel ignores it -- and here it is simply the documented
 * behaviour: the call either gives you the address you asked for or returns
 * NULL.  So there is no MAP_FIXED trap to avoid.
 *
 * What Windows adds instead is a reason to look before you leap.  On Linux the
 * argument that nothing else wants 0x02000000..0x0A000000 is structural: a PIE
 * executable is loaded high, brk follows it, and mmap grows down from the top.
 * On Windows none of that reasoning transfers.  A 32-bit process has a 2 GiB
 * user address space, the image lands wherever its base says (or wherever ASLR
 * puts it, if ASLR is left on), and every DLL the loader pulled in before main
 * ran is already somewhere.  SDL2.dll is in the import table, so it is mapped
 * before this file gets a chance to speak.
 *
 * The answer is not an argument, it is a walk.  RangeIsFree() asks VirtualQuery
 * about every region the reservation covers and requires all of them to be
 * MEM_FREE before VirtualAlloc is called at all.  VirtualAlloc failing would
 * also have caught a collision, but it would have said "failed" and nothing
 * else; the walk can say *what* is in the way and, if it belongs to a module,
 * which one.  On the one failure this port cannot recover from, that is the
 * difference between a bug report and a shrug.
 *
 * PortHostReportAddressSpace() is the other half of the same idea: --verbose
 * prints the kernel's own view of the low address space, which is what
 * /proc/<pid>/maps gives you on Linux and what Windows has no file for.
 *
 * Two smaller differences, both real:
 *
 *   Granularity.  VirtualAlloc rounds a base down to dwAllocationGranularity
 *   (64 KiB) and a size up to dwPageSize (4 KiB).  PortHostPageSize returns the
 *   coarser of the two so that mem.c's spans and the kernel's regions have the
 *   same edges; every base in the GBA map is 64 KiB aligned already.
 *
 *   Commit.  mem_posix.c passes MAP_NORESERVE, because most of the 32 MiB ROM
 *   window is address space the game never touches.  Windows has no equivalent
 *   that also lets the pages be written on demand -- MEM_RESERVE alone means a
 *   read faults rather than materialising a zero page -- so this commits the
 *   whole map, about 34 MiB.  That is charge against the commit limit, not
 *   resident memory: the pages stay zero-shared until written.
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601     /* GetModuleHandleExA */
#endif

#include <stdio.h>
#include <string.h>

#include <windows.h>

#include "native.h"

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static const char *LastErrorText(void)
{
    static char buf[256];
    DWORD err = GetLastError();
    DWORD n;

    n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, err, 0, buf, (DWORD)sizeof(buf), NULL);
    if (n == 0) {
        snprintf(buf, sizeof(buf), "Windows error %lu", (unsigned long)err);
        return buf;
    }
    /* FormatMessage ends its strings with CRLF and a full stop. */
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'
                     || buf[n - 1] == '.' || buf[n - 1] == ' '))
        buf[--n] = '\0';
    return buf;
}

/* Which module, if any, owns this address.  A collision inside the GBA window
 * is almost certainly a DLL that the loader based there, and naming it is the
 * whole diagnostic. */
static const char *ModuleAt(uintptr_t addr)
{
    static char path[MAX_PATH];
    HMODULE mod = NULL;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod))
        return NULL;
    if (GetModuleFileNameA(mod, path, (DWORD)sizeof(path)) == 0)
        return NULL;
    {
        const char *slash = strrchr(path, '\\');

        return slash != NULL ? slash + 1 : path;
    }
}

static const char *StateName(DWORD state)
{
    switch (state) {
    case MEM_FREE:    return "free";
    case MEM_RESERVE: return "reserved";
    case MEM_COMMIT:  return "committed";
    default:          return "?";
    }
}

static const char *TypeName(DWORD type)
{
    switch (type) {
    case MEM_IMAGE:   return "image";
    case MEM_MAPPED:  return "mapped";
    case MEM_PRIVATE: return "private";
    default:          return "";
    }
}

/* PAGE_GUARD, PAGE_NOCACHE and PAGE_WRITECOMBINE are modifier bits on top of a
 * base protection; mask them off before comparing. */
#define PROT_BASE(p) ((p) & 0xFFu)

static int ProtectIsReadable(DWORD protect)
{
    if (protect & PAGE_GUARD)
        return 0;               /* touching it raises STATUS_GUARD_PAGE first */
    switch (PROT_BASE(protect)) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return 1;
    default:
        return 0;               /* PAGE_NOACCESS, PAGE_EXECUTE */
    }
}

/* --------------------------------------------------------------------------
 * The seam
 * ------------------------------------------------------------------------ */

size_t PortHostPageSize(void)
{
    SYSTEM_INFO si;

    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);
    return si.dwAllocationGranularity != 0 ? (size_t)si.dwAllocationGranularity
                                           : (size_t)65536;
}

/* Is every page of [addr, addr+size) unclaimed?
 *
 * VirtualAlloc would refuse a collision anyway.  This runs first so that the
 * refusal can be explained: mem.c turns a failed reservation into a fatal
 * error, and "0x06000000 is committed image memory belonging to SDL2.dll" is
 * an answer, where "could not reserve" is a question. */
static int RangeIsFree(uintptr_t addr, size_t size, const char **why)
{
    static char msg[512];
    uintptr_t at = addr;
    uintptr_t end = addr + size;

    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;

        memset(&mbi, 0, sizeof(mbi));
        if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            snprintf(msg, sizeof(msg),
                     "VirtualQuery(0x%08lX) failed: %s",
                     (unsigned long)at, LastErrorText());
            *why = msg;
            return 0;
        }
        if (mbi.State != MEM_FREE) {
            const char *mod = ModuleAt((uintptr_t)mbi.BaseAddress);

            snprintf(msg, sizeof(msg),
                     "0x%08lX..0x%08lX is already %s %s memory%s%s",
                     (unsigned long)(uintptr_t)mbi.BaseAddress,
                     (unsigned long)((uintptr_t)mbi.BaseAddress + mbi.RegionSize),
                     StateName(mbi.State), TypeName(mbi.Type),
                     mod != NULL ? ", belonging to " : "",
                     mod != NULL ? mod : "");
            *why = msg;
            return 0;
        }

        next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= at) {       /* cannot happen; do not spin if it does */
            *why = "VirtualQuery returned an empty region";
            return 0;
        }
        at = next;
    }
    return 1;
}

int PortHostReserve(uintptr_t addr, size_t size, const char **why)
{
    static char msg[256];
    void *got;

    if (!RangeIsFree(addr, size, why))
        return -1;

    /* MEM_RESERVE|MEM_COMMIT in one call, with an explicit base.  Windows does
     * not have mmap's "here is somewhere else instead" failure mode: an
     * explicit lpAddress that is not available is NULL, never a different
     * address.  The check below is therefore belt and braces, and it is here
     * for the same reason it is in mem_posix.c -- a reservation that silently
     * relocated would be undetectable from the game's side. */
    got = VirtualAlloc((LPVOID)addr, size, MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    if (got == NULL) {
        snprintf(msg, sizeof(msg), "VirtualAlloc: %s", LastErrorText());
        *why = msg;
        return -1;
    }
    if ((uintptr_t)got != addr) {
        VirtualFree(got, 0, MEM_RELEASE);
        *why = "VirtualAlloc returned a different address than the one asked "
               "for";
        return -1;
    }
    return 0;
}

/* Is [addr, addr+len) memory this process can actually touch?
 *
 * The POSIX side answers this with msync(), which succeeds for anything that
 * is mapped.  VirtualQuery is finer-grained, and the extra detail matters:
 *
 *   MEM_FREE is the msync ENOMEM case -- nothing there, reject.
 *   MEM_RESERVE has no equivalent on Linux.  Address space has been claimed
 *     but no pages stand behind it; a read faults exactly as if it were free.
 *     Windows heaps and thread stacks both keep large reserved-but-uncommitted
 *     tails, so this is not a corner case, and accepting it would hand dma.c a
 *     range that segfaults on touch.
 *   PAGE_NOACCESS and PAGE_GUARD are committed and still fault.  The guard page
 *     under every thread stack is the common one.
 *
 * A range can span several regions, so this walks rather than asking once: a
 * transfer that starts in the last page of the heap and runs off the end has a
 * perfectly valid first region.
 *
 * Readability is the test, not writability, which matches what msync() does on
 * the POSIX side.  A DMA *into* read-only host memory would still fault, but
 * dma.c never has a host destination that is not one of the port's own
 * writable arrays, and diverging from the POSIX answer here would mean the two
 * platforms accept different transfers -- which is a worse bug than the one it
 * would prevent. */
int PortHostAddrValid(uintptr_t addr, size_t len)
{
    uintptr_t at = addr;
    uintptr_t end;

    if (len == 0)
        return 0;
    end = addr + len;
    if (end < addr)
        return 0;

    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;

        memset(&mbi, 0, sizeof(mbi));
        if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) != sizeof(mbi))
            return 0;           /* above the user address space, or gone */
        if (mbi.State != MEM_COMMIT)
            return 0;
        if (!ProtectIsReadable(mbi.Protect))
            return 0;

        next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= at)
            return 0;
        at = next;
    }
    return 1;
}

/* --------------------------------------------------------------------------
 * The kernel's own view, for --verbose
 *
 * mem.c can only report what the port believes about itself.  This is the
 * other question -- what does the operating system say is mapped -- and on
 * Windows there is no /proc/<pid>/maps to read, so the process has to ask from
 * the inside.
 * ------------------------------------------------------------------------ */

#define REPORT_CEILING 0x20000000u      /* the same cut-off docs/NATIVE.md uses
                                           for the Linux /proc/<pid>/maps
                                           excerpt: everything near the map,
                                           and a count for the rest */

void PortHostReportAddressSpace(void)
{
    SYSTEM_INFO si;
    uintptr_t at = 0;
    uintptr_t limit;
    uintptr_t lowestAbove = 0;
    int above = 0;

    memset(&si, 0, sizeof(si));
    GetSystemInfo(&si);
    limit = (uintptr_t)si.lpMaximumApplicationAddress;

    PortLog("[katam-port] the kernel's own view (VirtualQuery), user address "
            "space 0x%08lX..0x%08lX:",
            (unsigned long)(uintptr_t)si.lpMinimumApplicationAddress,
            (unsigned long)limit);

    while (at < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t next;

        memset(&mbi, 0, sizeof(mbi));
        if (VirtualQuery((LPCVOID)at, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= at)
            break;

        if (mbi.State != MEM_FREE) {
            if (at < REPORT_CEILING) {
                const char *mod = ModuleAt((uintptr_t)mbi.BaseAddress);

                PortLog("[katam-port]   0x%08lX +0x%08lX  %s %s%s%s",
                        (unsigned long)(uintptr_t)mbi.BaseAddress,
                        (unsigned long)mbi.RegionSize,
                        StateName(mbi.State), TypeName(mbi.Type),
                        mod != NULL ? "  " : "", mod != NULL ? mod : "");
            } else {
                if (above == 0)
                    lowestAbove = (uintptr_t)mbi.BaseAddress;
                above++;
            }
        }
        at = next;
    }

    if (above == 0)
        PortLog("[katam-port]   ... and nothing at all above 0x%08lX",
                (unsigned long)REPORT_CEILING);
    else
        PortLog("[katam-port]   ... and %d region%s above 0x%08lX, the lowest "
                "at 0x%08lX", above, above == 1 ? "" : "s",
                (unsigned long)REPORT_CEILING, (unsigned long)lowestAbove);
}
