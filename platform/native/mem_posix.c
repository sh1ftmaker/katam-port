/* The POSIX half of the memory seam: Linux, macOS, the BSDs.
 *
 * Three functions.  Everything about *what* to reserve and *why it is safe* is
 * in platform/native/mem.c; this file only knows how to ask the kernel.
 *
 * macOS
 * -----
 * mmap here behaves the same, but the addresses are not available by default:
 * the linker gives every 64-bit Mach-O a __PAGEZERO segment of 4 GiB, which
 * covers the entire GBA map, and nothing can be mapped inside it.  The fix is
 * a link flag, not a code change:
 *
 *     -Wl,-pagezero_size,0x1000
 *
 * MAP_FIXED_NOREPLACE does not exist on macOS.  The nearest equivalent is
 * VM_FLAGS_FIXED without VM_FLAGS_OVERWRITE via mach_vm_map, or -- simpler and
 * good enough -- probe the range with mincore() first and only then map it
 * with MAP_FIXED.  That has a race in a threaded program; this one reserves
 * before SDL starts any thread.  See docs/NATIVE.md.
 */

#define _GNU_SOURCE 1

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "native.h"

#ifndef MAP_FIXED_NOREPLACE
/* Linux 4.17+.  Older headers, and every other POSIX, do not define it; the
 * flag then falls back to a mincore() probe plus MAP_FIXED below. */
#define MAP_FIXED_NOREPLACE 0
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

size_t PortHostPageSize(void)
{
    long n = sysconf(_SC_PAGESIZE);

    return n > 0 ? (size_t)n : 4096;
}

/* Is any page of [addr, addr+size) already mapped?
 *
 * Only used where MAP_FIXED_NOREPLACE is unavailable.  mincore() answers
 * "which of these pages are resident", but the part that matters is its error:
 * ENOMEM means the range contains something unmapped, which is exactly the
 * "nobody is here" this needs. */
static int AnythingMapped(uintptr_t addr, size_t size)
{
    size_t page = PortHostPageSize();
    size_t i;

    for (i = 0; i < size; i += page) {
        unsigned char v;

        errno = 0;
        if (mincore((void *)(addr + i), page, &v) == 0)
            return 1;               /* mapped */
        if (errno != ENOMEM)
            return 1;               /* cannot tell -- assume occupied */
    }
    return 0;
}

int PortHostReserve(uintptr_t addr, size_t size, const char **why)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    void *got;

    /* MAP_NORESERVE because this is 32 MiB of *address space*, most of which
     * is never touched: the ROM image is 16 MiB of a 32 MiB window and the
     * upper half exists only so that the game's reads past the end of its own
     * data land on a page instead of a fault.  Committing it would show up as
     * 32 MiB of dirty memory on a machine with no swap. */

    if (MAP_FIXED_NOREPLACE != 0) {
        flags |= MAP_FIXED_NOREPLACE;
    } else {
        if (AnythingMapped(addr, size)) {
            *why = "something is already mapped there";
            return -1;
        }
        flags |= MAP_FIXED;
    }

    got = mmap((void *)addr, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (got == MAP_FAILED) {
        *why = strerror(errno);
        return -1;
    }
    /* MAP_FIXED_NOREPLACE on a kernel older than 4.17 is ignored rather than
     * rejected, and the mapping silently goes somewhere else.  That is the one
     * outcome this whole file exists to prevent, so check. */
    if ((uintptr_t)got != addr) {
        munmap(got, size);
        *why = "the kernel ignored the fixed address and relocated the mapping "
               "(kernel older than 4.17?)";
        return -1;
    }
    return 0;
}

/* Does every page of [addr, addr+len) exist?
 *
 * mincore() answers "which of these pages are resident", and the part that
 * matters is its error: ENOMEM means the range contains something unmapped.
 * That is the same ENOMEM msync(MS_ASYNC) gives, and msync was here first,
 * because it needs no output buffer.
 *
 * msync turned out to be the wrong one to trust, and the difference is not
 * academic.  Under qemu-user -- which is how the armhf build is tested on a
 * desktop, and how anyone runs a foreign-architecture binary -- the emulator
 * reserves the whole guest address space up front, so from the host kernel's
 * side an unmapped guest page is still part of a PROT_NONE mapping.  msync has
 * nothing to flush and returns success for every address in the four gigabytes:
 *
 *     0x3fcbc034   msync=ok   mincore=ENOMEM   /proc/self/maps: absent   read: SIGSEGV
 *
 * That address is not hypothetical; it is a source pointer the game's own DMA
 * presents during level load, which every other host correctly refuses.  Taking
 * msync's word for it turned a reported-and-skipped transfer into a segfault.
 * On a real kernel -- checked here on x86-64 and i686 -- both calls agree, so
 * mincore is not a workaround, it is the probe that answers the question that
 * was being asked.  msync stays as the fallback for a platform without
 * mincore().
 *
 * The vector is a fixed block and the range is walked in chunks, so a long
 * transfer costs a few more syscalls rather than an allocation in the middle of
 * a DMA.  mem.c memoises the answer; in a normal frame this runs a handful of
 * times. */
int PortHostAddrValid(uintptr_t addr, size_t len)
{
    unsigned char vec[512];
    size_t page = PortHostPageSize();
    uintptr_t lo = addr - (addr % page);
    uintptr_t hi = addr + len;
    uintptr_t at;

    hi = hi + page - 1;
    hi -= hi % page;
    if (hi <= lo)
        return 0;                   /* the rounding wrapped: not an address */

    for (at = lo; at < hi; ) {
        size_t want = (size_t)(hi - at);
        size_t chunk = sizeof(vec) * page;

        if (want > chunk)
            want = chunk;

        errno = 0;
        if (mincore((void *)at, want, vec) != 0) {
            if (errno == ENOMEM)
                return 0;           /* something in there is not mapped */
            /* mincore is unavailable or refused for a reason of its own.
             * Fall back rather than guess. */
            errno = 0;
            return msync((void *)lo, (size_t)(hi - lo), MS_ASYNC) == 0;
        }
        at += want;
    }
    return 1;
}

/* The kernel's own view, for --verbose.
 *
 * /proc/<pid>/maps is the thing docs/NATIVE.md quotes when it says the
 * reservation landed where it was asked for, and quoting it from inside the
 * process means the evidence is in the log next to the claim rather than in
 * somebody's terminal afterwards.  Everything below 0x20000000, which is the
 * map and its immediate neighbourhood, then a count for the rest.
 *
 * macOS and the BSDs have no such file; there is a mach_vm_region walk that
 * would do the same job, and until somebody needs it this says so rather than
 * printing nothing and letting it look like an empty address space. */
void PortHostReportAddressSpace(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int above = 0;

    if (f == NULL) {
        PortLog("[katam-port] no /proc/self/maps on this system -- the "
                "kernel's own view of the address space is not available");
        return;
    }

    PortLog("[katam-port] the kernel's own view (/proc/self/maps):");
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long lo = strtoul(line, NULL, 16);

        if (lo >= 0x20000000UL) {
            above++;
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        PortLog("[katam-port]   %s", line);
    }
    fclose(f);
    PortLog("[katam-port]   ... and %d mapping%s above 0x20000000",
            above, above == 1 ? "" : "s");
}
