/* KATAM_ROM_CHECK: a debug build's check on the game's reads of the ROM's
 * animation table through a sprite's (animId, variant).
 *
 * The ROM keeps, per animation, an array of variant pointers (one script per
 * variant) and does not say how long it is.  A variant past the end reads the
 * next array's pointers, or the first words of a script, as a script: garbage
 * commands, then a wild read (the warp-star ride of Kirby 4.. did exactly
 * that: `variant + unk56` over a 4-variant animation, d1d531e).  AddressSanitizer
 * cannot see it -- the whole ROM is one mapped buffer -- so this build counts
 * each animation's variants once, from the ROM as loaded, and says so (with a
 * stack, under the sanitizers) the first time each bad (animId, variant) pair
 * is interpreted.
 *
 * The count is an upper bound by layout: an array ends where the next
 * animation's array, or any animation's first script, begins.  The default
 * build's recordings never trip it (that is how the bound was checked), so a
 * report is an index the console's own game never produced.
 *
 * Off unless the build defines KATAM_ROM_CHECK (cmake -DKATAM_ROM_CHECK=ON):
 * the file is empty otherwise, and port/wide.h's PORT_ROM_CHECK_ANIM is a
 * no-op, so no other build changes. */
#ifdef KATAM_ROM_CHECK

#include <stdlib.h>
#include <string.h>

#include "port/port.h"
#include "port/backend.h"
#include "global.h"
#include "sprite.h"
#include "kirby.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#include <sanitizer/common_interface_defs.h>
#define ROMCHK_STACK() __sanitizer_print_stack_trace()
#endif
#endif
#ifndef ROMCHK_STACK
#define ROMCHK_STACK() ((void)0)
#endif

enum { ROMCHK_MAX_ANIMS = 4096 };

static u16 sAnims;                       /* animations in the table          */
static u8 sCount[ROMCHK_MAX_ANIMS];     /* variants per animation (bound)   */
static u32 sSeen[ROMCHK_MAX_ANIMS][8];  /* reported (animId, variant) pairs */
static u32 sTable;                       /* the table they were counted from */
static u32 sReports;

static int InRom(u32 a)
{
    return a >= GBA_ROM_BASE && a - GBA_ROM_BASE + 4u <= gPortRomSize;
}

static u32 Rd(u32 a)
{
    u32 v;

    memcpy(&v, (const void *)(uintptr_t)a, 4);
    return v;
}

static int CmpU32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;

    return x < y ? -1 : x > y;
}

static void Count(u32 table)
{
    static u32 bounds[2 * ROMCHK_MAX_ANIMS + 1];
    u32 n = 0, nb = 0, i;

    sTable = table;
    while (n < ROMCHK_MAX_ANIMS && InRom(table + 4u * n) && InRom(Rd(table + 4u * n)))
        n++;
    sAnims = (u16)n;
    for (i = 0; i < n; i++) {
        u32 arr = Rd(table + 4u * i);

        bounds[nb++] = arr;
        if (InRom(arr) && InRom(Rd(arr)))
            bounds[nb++] = Rd(arr);
    }
    bounds[nb++] = table;
    qsort(bounds, nb, sizeof bounds[0], CmpU32);
    for (i = 0; i < n; i++) {
        u32 arr = Rd(table + 4u * i), end = arr + 4u * 255u, lo = 0, hi = nb, c = 0;

        while (lo < hi) {                       /* first bound > arr */
            u32 mid = (lo + hi) / 2;

            if (bounds[mid] <= arr)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < nb && bounds[lo] < end)
            end = bounds[lo];
        while (arr + 4u * c < end && c < 255u && InRom(arr + 4u * c) && InRom(Rd(arr + 4u * c)))
            c++;
        sCount[i] = (u8)c;
    }
    PortLog("[katam-port] rom-check: %u animations counted from %08X", (unsigned)n, (unsigned)table);
}

static const char *Owner(const struct Sprite *sprite, int *k)
{
    uintptr_t p = (uintptr_t)sprite, b = (uintptr_t)&gKirbys[0];

    if (p >= b && p < b + sizeof gKirbys) {
        *k = (int)((p - b) / sizeof gKirbys[0]);
        return "Kirby";
    }
    *k = -1;
    return "not a Kirby's";
}

void PortRomCheckAnim(const struct Sprite *sprite, int site)
{
    u32 table = (u32)(uintptr_t)(const void *)gSpriteTables->anims;
    u32 id = sprite->animId, v = sprite->variant;
    int k;
    const char *who;

    if (!InRom(table))
        return;                                 /* retargeted: not checked */
    if (table != sTable)
        Count(table);
    if (id < sAnims && v < sCount[id])
        return;
    if (id < sAnims && (sSeen[id][v >> 5] >> (v & 31) & 1))
        return;
    if (id < sAnims)
        sSeen[id][v >> 5] |= 1u << (v & 31);
    else if (sReports > 64)
        return;
    sReports++;
    who = Owner(sprite, &k);
    PortLog("[katam-port] rom-check: animation %u variant %u past its %u variants "
            "(site %d, %s sprite %d, frame %u)",
            (unsigned)id, (unsigned)v, id < sAnims ? (unsigned)sCount[id] : 0u, site, who, k,
            (unsigned)PortFrameNumber());
    ROMCHK_STACK();
}

#endif /* KATAM_ROM_CHECK */
