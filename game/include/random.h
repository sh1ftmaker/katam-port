#ifndef GUARD_RANDOM_H
#define GUARD_RANDOM_H

#ifdef __cplusplus
extern "C" {
#endif


#include "global.h"

/* PORT: replaced by port/ram_symbols.h -- extern u32 gRngVal; */

#define Rand32() (gRngVal = gRngVal * 1663525 + 1013904223)
#define Rand16() (Rand32(), gRngVal >> 16)

/* PORT-FORK: two RNG calls in one full expression are evaluated in an order C
 * leaves to the compiler, and gcc, g++ and clang each pick their own, so every
 * such site is written out with sequenced temporaries.  The order is the one
 * agbcc compiled -- the decompilation matches the ROM, so the ROM's code is
 * the original behaviour -- and tools/lint_determinism.py (make
 * lint-determinism) keeps new unsequenced uses out.  What agbcc did:
 *
 *   - Rand16() as operands of an arithmetic operator, `(Rand16() & 7) +
 *     (Rand16() & 0xF)`: gcc folds the comma expressions out of the operator,
 *     so the RNG advances twice and *one* read of gRngVal feeds both operands.
 *     That is Rand16Twice().
 *   - One RNG call per function argument, `f(Rand16() & 7, Rand16() & 7)`:
 *     left to right, each argument its own value.
 */
#define Rand16Twice() (Rand32(), Rand32(), gRngVal >> 16)

#define RandLessThan3() ({ \
    u16 _variable; \
    u16 _rand = Rand16(); \
 \
    if (_rand < 0x5555) \
        _variable = 0; \
    else if (_rand < 0xAAAA) \
        _variable = 1; \
    else \
        _variable = 2; \
    _variable; \
})

#define RandLessThan(x) ({ \
    u16 _r = Rand16(), _i; \
 \
    for (_i = 0; _i <= (x)-2; ++_i) \
        if (_r < (_i+1) * (0x10000/(x))) \
            break; \
    _i; \
})

#ifdef __cplusplus
}
#endif

#endif
