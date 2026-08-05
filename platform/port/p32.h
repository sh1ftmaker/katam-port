#ifndef GUARD_PORT_P32_H
#define GUARD_PORT_P32_H

/* A pointer that is four bytes wide on a 64-bit host.
 *
 * The GBA gave every pointer member of every structure four bytes, and the
 * port reads those structures at the addresses the console put them --
 * `gTasks` at 0x030019F0, the sound bank out of the ROM, the Kirby array at
 * 0x02020EE0.  A host pointer of eight bytes moves every member after it and
 * grows fixed-extent arrays past their neighbours, which is the whole of what
 * docs/SIXTYFOUR.md measured: 148 structures change shape and 1110 member
 * offsets move.
 *
 * So the storage has to stay four bytes while the *uses* keep working.  C has
 * no way to say that -- which is why SIXTYFOUR.md concluded a 64-bit build
 * needed a type-aware rewriter over ~40,000 member accesses, and it was right
 * about C.  In C++ a class with operator-> is exactly that thing, so what
 * changes is 285 member declarations and no use sites at all.
 *
 *     struct Chest {
 *         PTR32(struct Task) task;      // 4 bytes in both builds
 *         u16 x;                        // at offset 4 in both builds
 *     };
 *
 * `chest->task->state`, `chest->task = t`, `if (chest->task)`, `(u32)chest->task`
 * and passing it to something expecting `struct Task *` all still compile and
 * still mean what they meant.
 *
 * Under ILP32 -- wasm32, i686, armhf, mingw32 -- PTR32 expands to a plain
 * pointer and none of this exists.  Those builds stay C, stay byte-identical,
 * and remain the reference the 64-bit build is checked against.
 *
 * WHAT THIS DOES NOT DO
 *
 * It does not make an address fit in 32 bits; it only stops the *storage* from
 * being wider.  Everything the game can hold a pointer to must live below
 * 4 GiB -- the GBA map does by construction, but the port's own code and any
 * host storage the game observes do not, and that is the link-line and
 * allocator work in docs/SIXTYFOUR.md, not this file.  Storing a high address
 * through a P32 truncates it, so PORT_CHECK_POINTERS makes that an abort
 * rather than a corrupted pointer.
 */

#include "gba/types.h"

#ifdef __cplusplus

#include <stdint.h>

#ifdef PORT_CHECK_POINTERS
void PortP32Truncated(const void *p);
#endif

template <class T>
struct P32 {
    /* The only member, and it must stay the only member: sizeof(P32) == 4 and
     * standard layout are what let a structure containing one be memcpy'd,
     * DMA'd and read straight out of the ROM the way the game does. */
    u32 v;

    /* Defaulted rather than written out.  A user-provided default constructor
     * would make P32 non-trivial, and then `struct Chest c;` -- a plain
     * uninitialised local, which the decompilation has thousands of -- would
     * stop being trivially constructible along with every structure holding
     * one.  Defaulted keeps it trivial and keeps those structures aggregates. */
    P32() = default;

    P32(T *p)
        : v(static_cast<u32>(reinterpret_cast<uintptr_t>(p)))
    {
#ifdef PORT_CHECK_POINTERS
        if (reinterpret_cast<uintptr_t>(p) >> 32)
            PortP32Truncated(p);
#endif
    }

    /* There is deliberately no constructor from nullptr_t.  `p = NULL` and
     * `p = 0` are what the decompilation writes, and both are null pointer
     * constants that reach the T* constructor above on their own.  Adding an
     * overload for nullptr_t makes g++'s NULL -- which is `__null`, of type
     * long -- ambiguous between the two, so the overload breaks exactly the
     * spelling it was meant to support. */

    /* The conversion carries almost everything on its own -- `*p`, `p[i]`,
     * `p + 1`, `p == q`, `if (p)`, and passing it to anything that wants a
     * T*.  operator-> is the one the language will not synthesise. */
    operator T *() const { return reinterpret_cast<T *>(static_cast<uintptr_t>(v)); }
    T *operator->() const { return reinterpret_cast<T *>(static_cast<uintptr_t>(v)); }

    /* `(u32)obj->ptr` is everywhere in this codebase, and it does not work
     * through the conversion above: a cast is allowed one user-defined
     * conversion, and pointer-to-integer is not a standard conversion that can
     * follow it.  So the integer conversion has to be its own.
     *
     * explicit, which is the whole trick.  A C-style cast and a static_cast
     * both consider explicit conversion operators, so `(u32)p` and
     * `(uintptr_t)p` compile; implicit contexts do not, so `if (p)`,
     * `p == q` and `p == NULL` still resolve through the pointer conversion
     * alone instead of becoming ambiguous. */
    explicit operator u32() const { return v; }

    /* Deliberately absent: operator bool.  With operator T* already present it
     * would make `if (p)` ambiguous rather than convenient, and the pointer
     * conversion gives the same answer contextually. */
};

/* void needs its own: `void &` is not a type, so operator-> and the
 * dereference the primary template promises cannot exist.  This is the case
 * that matters most in practice -- struct Object2::unk78 is a void* that the
 * game stores *functions* in, which is the shape docs/SIXTYFOUR.md flagged as
 * invisible to any type analysis. */
template <>
struct P32<void> {
    u32 v;

    P32() = default;

    P32(void *p)
        : v(static_cast<u32>(reinterpret_cast<uintptr_t>(p)))
    {
#ifdef PORT_CHECK_POINTERS
        if (reinterpret_cast<uintptr_t>(p) >> 32)
            PortP32Truncated(p);
#endif
    }

    operator void *() const { return reinterpret_cast<void *>(static_cast<uintptr_t>(v)); }
    explicit operator u32() const { return v; }
};

static_assert(sizeof(P32<int>) == 4, "P32 must be four bytes -- it is a GBA pointer member");
static_assert(alignof(P32<int>) == 4, "P32 must align to four -- it is a GBA pointer member");
static_assert(sizeof(P32<void>) == 4, "P32<void> must be four bytes");

#define PTR32(...) P32<__VA_ARGS__>

#else /* !__cplusplus */

/* The ILP32 builds.  A pointer is already four bytes, so PTR32 is a spelling
 * and nothing more, and those builds keep compiling as C. */
#define PTR32(...) __VA_ARGS__ *

#endif /* __cplusplus */

#endif /* GUARD_PORT_P32_H */
