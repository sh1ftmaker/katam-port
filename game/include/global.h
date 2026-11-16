#ifndef GUARD_GLOBAL_H
#define GUARD_GLOBAL_H

#ifdef __cplusplus
extern "C" {
#endif


#include "gba/gba.h"
#include <string.h>
#include <stdint.h>

/* PORT-FORK: a read the console makes past the end of EWRAM.  The hardware
 * mirrors EWRAM every 256 KiB, so such a read lands back inside it; natively
 * the address is unmapped and faults, and in wasm it reads whatever the port
 * keeps there.  This reads what the console reads, on every host. */
#if KATAM_WIDE
/* The wide build (port/wide.h): the tables these reads index have moved, so
 * the console's address is computed in the console's own layout. */
#define EWRAM_MIRROR_U8(addr) (PortWideMirrorU8((uintptr_t)(addr)))
#else
#define EWRAM_MIRROR_U8(addr) \
    (*(const u8 *)(uintptr_t)(0x02000000u + (((uintptr_t)(addr) - 0x02000000u) & 0x3FFFFu)))
#endif

/* PORT: the row of a per-human-count table (boss HP, the HP bar's scale,
 * a boss's half-HP thresholds: four rows, for 1..4 people), indexed by the
 * game as table[gNumHumanPlayers - 1].  More than four people (the wide
 * build) use the four-player row (owner's decision; ONE-WORLD-16.md §5).
 * The default build has at most four, and this is exactly that expression. */
#define KATAM_HUMANS_ROW (KATAM_MIN4(gNumHumanPlayers) - 1)
/* The same count where the game scales a number by it (dark_mind.c). */
#define KATAM_HUMANS_SCALE KATAM_MIN4(gNumHumanPlayers)
/* PORT: a loop the console writes `i < 4` over every Kirby, where Kirbys
 * past the fourth must take part too (they would be left behind in a room
 * nobody can leave, docs/design/ONE-WORLD-16.md §5): the live Kirbys in a
 * build with more than four, the literal four otherwise (the default build,
 * and the wide build at four, proof W0). */
#if KATAM_KIRBYS > 4
#define KATAM_LIVE_KIRBYS gNumKirbys
#else
#define KATAM_LIVE_KIRBYS 4
#endif

// Prevent cross-jump optimization.
#define BLOCK_CROSS_JUMP /* PORT: asm barrier removed */

// to help in decompiling
#define asm_comment(x) asm volatile("@ -- " x " -- ")
#define asm_unified(x) asm(".syntax unified\n" x "\n.syntax divided")

#if defined (__APPLE__) || defined (__CYGWIN__)
// Get the IDE to stfu

// We define it this way to fool preproc.
#define INCBIN(...) {0}
#define INCBIN_U8  INCBIN
#define INCBIN_U16 INCBIN
#define INCBIN_U32 INCBIN
#define INCBIN_S8  INCBIN
#define INCBIN_S16 INCBIN
#define INCBIN_S32 INCBIN
#define _(x) (x)
#define __(x) (x)
#endif // __APPLE__

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define SWAP(a, b, temp)    \
{                           \
    temp = a;               \
    a = b;                  \
    b = temp;               \
}

// useful math macros

// Converts a number to Q8.8 fixed-point format
#define Q_8_8(n) ((s16)((n) * 256))

// Converts a number to Q4.12 fixed-point format
#define Q_4_12(n)  ((s16)((n) * 4096))

// Converts a number to Q2.14 fixed-point format
#define Q_2_14(n) ((s16)((n) * 0x4000))

#define abs(n) (((n) >= 0) ? (n) : -(n))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) >= (b) ? (a) : (b))

#define XorSwap(a, b) \
({ \
    (a) ^= (b); \
    (b) ^= (a); \
    (a) ^= (b); \
})

// TODO: move to proper location
typedef void (*HBlankFunc)(u8);
typedef void (*FuncType_030068C0)(void);
typedef u32 (*FuncType_08D5FDD4)(void);
typedef void (*IntrFunc)(void);

/* PORT: replaced by port/ram_symbols.h -- extern void *iwram_end; */

#ifdef __cplusplus
}
#endif

#endif // GUARD_GLOBAL_H
