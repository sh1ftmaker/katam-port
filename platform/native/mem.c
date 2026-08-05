/* Reserving the GBA memory map in a native process.
 *
 * This is the one thing the port cannot do without.  The game is written
 * against addresses, not an API -- `*(vu16 *)0x0600E002 = 0xF1B0;` is real
 * code in the HUD, and TaskGetStructPtr hands out `EWRAM_START + (off << 2)`
 * from the game's own allocator.  So EWRAM has to be at 0x02000000 and VRAM at
 * 0x06000000, in this process, or nothing works.  See docs/ARCHITECTURE.md.
 *
 * In wasm that costs one linker flag: -sGLOBAL_BASE=0x0A000000 puts every byte
 * the compiler owns above the map, and the low addresses are simply free.
 * Natively there is no equivalent flag, so the addresses are taken at run time
 * instead -- and the interesting question is whether anything else in the
 * process wants them.
 *
 * Nothing does, and it is not luck:
 *
 *   The executable.  Built PIE (the default on every modern toolchain), so the
 *   loader places it at a randomised high address -- 0x55.. on x86-64 Linux,
 *   0x1.. on macOS.  Its .data and .bss go with it.  A non-PIE binary would
 *   load at 0x400000, which is below EWRAM and therefore harmless in itself,
 *   but its brk heap would then grow upward from there straight into
 *   0x02000000.  PortNativeReserveMap checks for that and says so.
 *
 *   The heap.  glibc's brk arena starts at the end of the executable's .bss,
 *   so it inherits the high load address; large allocations and every other
 *   allocator go through mmap, which the kernel hands out from mmap_base near
 *   the stack and grows *downward*.  Neither can reach the map from above
 *   without exhausting 128 TiB of address space first.
 *
 *   The stack.  Near the top of the address space, growing down.
 *
 *   Shared libraries.  Same mmap region as the heap.
 *
 * That is the argument.  The reason this file does not stop there is that an
 * argument is not evidence: the reservation is made with MAP_FIXED_NOREPLACE
 * (never bare MAP_FIXED, which would silently unmap a collision rather than
 * report it), each region is then written to and read back, and --verbose
 * prints what the kernel actually shows.  See docs/NATIVE.md.
 *
 * Page size is asked for rather than assumed.  Apple silicon and a good many
 * arm64 Linux kernels use 16 KiB pages, some ppc64 and arm64 configurations
 * 64 KiB, and Windows reserves at a 64 KiB granularity.  Every base address in
 * the map is 64 KiB aligned already, so the only thing that changes is how far
 * each size is rounded up -- and since the regions are 16 MiB apart, rounding
 * cannot make two of them collide.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native.h"

/* ---------------------------------------------------------------------------
 * What to reserve.
 *
 * The ROM region is the whole 32 MiB the cartridge bus decodes (GBA_ROM_MAX),
 * not the 16 MiB image, because the game reads past the end of its own data in
 * a few places and the hardware returns open bus.  In wasm those reads land in
 * untouched linear memory and come back zero; here they need real pages behind
 * them or they would fault.
 *
 * That 32 MiB swallows save memory, which tools/portify.py relocated from the
 * hardware's 0x0E000000 down to 0x09000000 to save the wasm build 80 MiB of
 * reservation.  So SRAM sits *inside* the ROM's address window.  The two do
 * not fight -- the image is 16 MiB and stops at 0x09000000 -- but they are one
 * mapping, not two, and asking for them separately is how you get an
 * EEXIST from the second request.  Coalesce() below is what makes that a
 * non-event rather than a startup failure, and it is why the table may list
 * overlapping regions at all.
 * ------------------------------------------------------------------------ */

struct Region {
    uintptr_t base;
    size_t    size;
    const char *name;
};

static const struct Region kMap[] = {
    { GBA_EWRAM_BASE, GBA_EWRAM_SIZE, "EWRAM"   },
    { GBA_IWRAM_BASE, GBA_IWRAM_SIZE, "IWRAM"   },
    { GBA_IO_BASE,    GBA_IO_SIZE,    "I/O"     },
    { GBA_PLTT_BASE,  GBA_PLTT_SIZE,  "palette" },
    { GBA_VRAM_BASE,  GBA_VRAM_SIZE,  "VRAM"    },
    { GBA_OAM_BASE,   GBA_OAM_SIZE,   "OAM"     },
    { GBA_ROM_BASE,   GBA_ROM_MAX,    "ROM"     },
    { GBA_SRAM_BASE,  GBA_SRAM_SIZE,  "save"    },
};
#define NUM_REGIONS ((int)(sizeof(kMap) / sizeof(kMap[0])))

/* The whole window, used to answer "is this address ours?" in one compare
 * pair.  Everything from EWRAM's base to the end of the highest region. */
static uintptr_t sWindowLo = (uintptr_t)-1;
static uintptr_t sWindowHi;

static size_t RoundUp(size_t n, size_t to)
{
    return (n + to - 1) / to * to;
}

static uintptr_t RoundDown(uintptr_t n, size_t to)
{
    return n - (n % to);
}

/* Merge the table into the fewest non-overlapping spans, page-aligned.
 * Regions are already in ascending order, but sorting here would be four lines
 * and the table is eight entries; keeping it ordered by hand is one less thing
 * that can be wrong when someone adds a region. */
static int Coalesce(struct Region *out, size_t page)
{
    int i, n = 0;

    for (i = 0; i < NUM_REGIONS; i++) {
        uintptr_t lo = RoundDown(kMap[i].base, page);
        uintptr_t hi = kMap[i].base + RoundUp(kMap[i].size, page);

        if (n > 0 && lo <= out[n - 1].base + out[n - 1].size) {
            /* Overlaps or abuts the previous span: extend it. */
            uintptr_t prevHi = out[n - 1].base + out[n - 1].size;
            if (hi > prevHi)
                out[n - 1].size = hi - out[n - 1].base;
            continue;
        }
        out[n].base = lo;
        out[n].size = hi - lo;
        out[n].name = kMap[i].name;
        n++;
    }
    return n;
}

static void Fatal(const char *fmt, ...)
{
    va_list ap;
    char buf[512];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    PortError("[katam-port] %s", buf);
    fflush(stdout);
    fflush(stderr);
    exit(2);
}

void PortNativeReserveMap(void)
{
    struct Region spans[NUM_REGIONS];
    size_t page = PortHostPageSize();
    int n, i;

    if (page == 0)
        page = 4096;

    n = Coalesce(spans, page);

    for (i = 0; i < n; i++) {
        const char *why = "the address range was already in use";

        if (PortHostReserve(spans[i].base, spans[i].size, &why) != 0)
            Fatal("could not reserve 0x%08lX..0x%08lX for the GBA memory map "
                  "(%s).\n"
                  "  The port needs the console's real addresses -- see "
                  "docs/NATIVE.md.\n"
                  "  On Linux this usually means the binary was linked "
                  "non-PIE, or something\n"
                  "  in this process (a preloaded library, a sanitizer) got "
                  "there first.",
                  (unsigned long)spans[i].base,
                  (unsigned long)(spans[i].base + spans[i].size), why);

        if (spans[i].base < sWindowLo)
            sWindowLo = spans[i].base;
        if (spans[i].base + spans[i].size > sWindowHi)
            sWindowHi = spans[i].base + spans[i].size;
    }

    /* Prove it.  A reservation that reported success and landed somewhere else
     * would be undetectable from the game's side until pointers started
     * disagreeing about where objects live, which is a week of debugging; one
     * store and one load per region costs nothing and rules it out here.
     *
     * Both ends of each region, because a size that was rounded the wrong way
     * fails only at the far end. */
    for (i = 0; i < NUM_REGIONS; i++) {
        volatile u32 *lo = (volatile u32 *)kMap[i].base;
        volatile u32 *hi = (volatile u32 *)(kMap[i].base + kMap[i].size - 4);

        *lo = 0xC0DEF00Du;
        *hi = 0x0BADCAFEu;
        if (*lo != 0xC0DEF00Du || *hi != 0x0BADCAFEu)
            Fatal("%s did not read back what was written at 0x%08lX -- the "
                  "reservation is not what it claims to be",
                  kMap[i].name, (unsigned long)kMap[i].base);
        *lo = 0;
        *hi = 0;
    }

    /* The other half of the question: can anything grow *into* the map?  A
     * non-PIE executable loads at 0x400000 and its brk heap grows up from
     * there, so the first big allocation would walk into EWRAM.  The address
     * of a function in this binary answers it. */
    if ((uintptr_t)&PortNativeReserveMap < sWindowHi)
        PortError("[katam-port] this binary is loaded at 0x%lX, below the top "
                  "of the GBA map (0x%lX) -- it was not linked PIE, and the "
                  "heap can grow into VRAM.  Rebuild with -fPIE -pie.",
                  (unsigned long)&PortNativeReserveMap,
                  (unsigned long)sWindowHi);
}

/* --- the DMA range check -------------------------------------------------
 *
 * platform/dma.c has already tried the GBA map and failed, so what arrives
 * here is either one of the port's own C pointers or a stale console address.
 * Two conditions, in the cheap order:
 *
 *   1. it must not be inside the reserved window, and
 *   2. it must be memory this process can really touch.
 *
 * (1) alone would accept 0x12345678 and hand it to memcpy.  (2) alone would
 * accept an address inside the map that dma.c has already judged undecoded --
 * 0x04000400, say, which is reserved but is not a register.
 *
 * The memo is worth having because the common case is a single repeated
 * address: DmaFill's fill value lives on the stack, at the same page, tens of
 * times a frame. */
int PortHostRangeOk(uintptr_t addr, u32 len)
{
    static uintptr_t sMemoPage;
    static int sMemoOk;
    uintptr_t page;

    if (len == 0)
        return 0;
    if (addr + len < addr)
        return 0;

    /* Any overlap with the window at all, not just containment: a run that
     * starts below EWRAM and ends inside it is not host data. */
    if (addr < sWindowHi && addr + len > sWindowLo)
        return 0;

    page = addr & ~(uintptr_t)0xFFFu;
    if (page == sMemoPage && ((addr + len - 1) & ~(uintptr_t)0xFFFu) == page)
        return sMemoOk;

    sMemoOk = PortHostAddrValid(addr, len);
    sMemoPage = page;
    return sMemoOk;
}

/* --- diagnostics --------------------------------------------------------- */

void PortNativeReportMap(void)
{
    size_t page = PortHostPageSize();
    struct Region spans[NUM_REGIONS];
    int n = Coalesce(spans, page);
    int i;

    PortLog("[katam-port] page size %lu, GBA window 0x%08lX..0x%08lX (%lu MiB "
            "of address space in %d mapping%s)",
            (unsigned long)page, (unsigned long)sWindowLo,
            (unsigned long)sWindowHi,
            (unsigned long)((sWindowHi - sWindowLo) >> 20), n,
            n == 1 ? "" : "s");
    for (i = 0; i < n; i++)
        PortLog("[katam-port]   0x%08lX +0x%08lX  %s",
                (unsigned long)spans[i].base, (unsigned long)spans[i].size,
                spans[i].name);
    {
        void *heap = malloc(64);

        PortLog("[katam-port] this binary at %p, a stack address at %p, a heap "
                "address at %p -- all outside the window",
                (void *)&PortNativeReserveMap, (void *)&n, heap);
        free(heap);
    }

    /* Everything above is the port's account of itself, which is the thing in
     * question: a reservation that reported success and landed elsewhere would
     * print exactly the same lines.  This is the other witness. */
    PortHostReportAddressSpace();
}
