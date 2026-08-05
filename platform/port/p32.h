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
#include <type_traits>

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

    /* From a void*, because that is how the game hands out every object it
     * allocates:
     *
     *     void *ptr = TaskGetStructPtr(task);
     *     chest2 = ptr;
     *
     * C converts void* to T* implicitly.  Without this the conversion has
     * nowhere to go, and it is not a rare shape -- it is the shape of every
     * object constructor in the game.
     *
     * A template, and that is not decoration.  Written as a plain
     * `P32(void *)` it sits alongside `P32(T *)` and makes `p = NULL`
     * ambiguous all over again -- g++'s NULL is `__null`, of type long, and
     * converts equally well to either.  Deduction throws the overload out for
     * anything that is not literally a void*, so NULL reaches the T*
     * constructor and a real void* reaches this one. */
    template <class V, class = typename std::enable_if<
                           std::is_same<V, void *>::value
                        || std::is_same<V, const void *>::value>::type>
    P32(V p)
        : v(static_cast<u32>(reinterpret_cast<uintptr_t>(p)))
    {
#ifdef PORT_CHECK_POINTERS
        if (reinterpret_cast<uintptr_t>(p) >> 32)
            PortP32Truncated(const_cast<void *>(static_cast<const void *>(p)));
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

    /* volatile-qualified, for members declared `const u8 *volatile srcp` --
     * the qualifier is on the pointer, so the member itself is a volatile P32
     * and a plain const member function cannot be called on it.  The multiboot
     * transfer state is the only place this occurs, and there the volatile is
     * the point: the SIO interrupt writes those fields. */
    operator T *() const volatile { return reinterpret_cast<T *>(static_cast<uintptr_t>(v)); }
    T *operator->() const volatile { return reinterpret_cast<T *>(static_cast<uintptr_t>(v)); }

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

    /* And to uintptr_t, which on LP64 is not u32.  A cast gets one
     * user-defined conversion, so it cannot reach uintptr_t by going through
     * the u32 one above and widening. */
    explicit operator uintptr_t() const { return static_cast<uintptr_t>(v); }

    /* And to any other integer the decompilation casts to -- s32, intptr_t.
     * Same reasoning as the two above and the same explicit, so none of them
     * can be picked implicitly; where the target is exactly u32 or uintptr_t
     * the non-template conversions win on overload resolution. */
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    explicit operator I() const { return static_cast<I>(v); }

    /* `(void *)obj->pal` and `(struct Kirby *)obj->unk78` -- a cast to some
     * *other* pointer type.  C does this freely and the decompilation does it
     * constantly; C++ will not chain the conversion above into a second
     * pointer conversion, because only one user-defined conversion is allowed
     * and pointer-to-pointer is not a standard conversion that can follow it.
     *
     * explicit again, and for the same reason: a cast considers it, an
     * implicit context does not.  Where U happens to be T, overload resolution
     * prefers the non-template conversion above, so this adds no ambiguity. */
    template <class U>
    explicit operator U *() const { return reinterpret_cast<U *>(static_cast<uintptr_t>(v)); }


    /* From another PTR32 of a different pointee.  The decompilation assigns
     * across pointer types freely -- most often through a void* member -- and
     * in C that is either legal (void*) or a warning the port's -w silences.
     * Matching that here is the point: the 64-bit build should accept what the
     * ILP32 builds accept, no more and no less. */
    template <class U>
    P32(const P32<U> &o) : v(o.v) {}

    /* `obj->unk78 == SomeFunction` -- the same GNU-ism as the constructor
     * above, in a comparison rather than an assignment.  C compares the
     * addresses; so does this. */
    template <class R, class... A>
    bool operator==(R (*f)(A...)) const
    { return v == static_cast<u32>(reinterpret_cast<uintptr_t>(f)); }

    template <class R, class... A>
    bool operator!=(R (*f)(A...)) const
    { return v != static_cast<u32>(reinterpret_cast<uintptr_t>(f)); }


    /* Pointer arithmetic and stepping.  `p++` over a table and `p + i` are
     * ordinary C on a pointer member; the conversion above cannot carry them
     * because the result has to be a P32 again for `p++` to mean anything. */
    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    T *operator+(I n) const
    { return reinterpret_cast<T *>(static_cast<uintptr_t>(v)) + n; }

    P32 &operator++()
    { v += static_cast<u32>(sizeof(T)); return *this; }

    P32 operator++(int)
    { P32 old = *this; v += static_cast<u32>(sizeof(T)); return old; }

    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    P32 &operator+=(I n)
    { v += static_cast<u32>(n * static_cast<I>(sizeof(T))); return *this; }

    template <class I, class = typename std::enable_if<std::is_integral<I>::value>::type>
    P32 &operator-=(I n)
    { v -= static_cast<u32>(n * static_cast<I>(sizeof(T))); return *this; }

    P32 &operator--()
    { v -= static_cast<u32>(sizeof(T)); return *this; }

    /* When T is a *function* type, the member is a function pointer, and the
     * decompilation's typedef for the sound driver's jump table is
     * `void (*MPlayFunc)()` -- an unprototyped pointer, which in C accepts and
     * yields a function of any signature.  Eleven sites in m4a.c rely on that:
     * they store ply_memacc, SampleFreqSet, TrackStop and the rest into one
     * table and read them back as `void (*)(void *)`.
     *
     * C++ has no unprototyped function pointer, so both directions have to be
     * said explicitly.  Both are constrained to function types, so nothing
     * here loosens an ordinary data pointer.
     */
    template <class R, class... A, class TT = T,
              typename std::enable_if<std::is_function<TT>::value, int>::type = 0>
    P32(R (*f)(A...))
        : v(static_cast<u32>(reinterpret_cast<uintptr_t>(f)))
    {
#ifdef PORT_CHECK_POINTERS
        if (reinterpret_cast<uintptr_t>(f) >> 32)
            PortP32Truncated(reinterpret_cast<const void *>(f));
#endif
    }

    /* Deduced from the target type, which is what makes
     * `void (*func)(void *) = table[35];` work. */
    template <class F, class TT = T,
              typename std::enable_if<std::is_function<TT>::value
                                   && std::is_pointer<F>::value, int>::type = 0>
    operator F() const { return reinterpret_cast<F>(static_cast<uintptr_t>(v)); }

    /* A void pointee converts to any pointer type implicitly, because that is
     * what void* does in C.  P32<void> has a specialisation of its own below;
     * this covers P32<const void>, which goes through the primary template and
     * would otherwise only convert to `const void *` -- leaving the ordinary C
     * assignment `const u8 *p = obj->someVoidMember;` with nowhere to go.
     *
     * U cannot be deduced in a boolean context, so `if (p)` and `p == NULL`
     * still resolve through the plain pointer conversion. */
    template <class U, class TT = T,
              typename std::enable_if<std::is_void<TT>::value, int>::type = 0>
    operator U *() const { return reinterpret_cast<U *>(static_cast<uintptr_t>(v)); }

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

    /* A *function* stored in a void* member.
     *
     * `ws->unk0.obj2.unk78 = sub_0800C124;` is the assignment
     * docs/SIXTYFOUR.md singles out as the one no type analysis can find: the
     * member is a void*, the value is a function, and in C that is a GNU
     * extension nobody warns about.  C++ has no implicit conversion from a
     * function pointer to void* at all, so without this the assignment does
     * not compile -- which is the first time any build has had an opinion
     * about it.
     *
     * Variadic over the signature so it covers every function the game
     * installs rather than the handful that appear today. */
    template <class R, class... A>
    P32(R (*f)(A...))
        : v(static_cast<u32>(reinterpret_cast<uintptr_t>(f)))
    {
#ifdef PORT_CHECK_POINTERS
        if (reinterpret_cast<uintptr_t>(f) >> 32)
            PortP32Truncated(reinterpret_cast<const void *>(f));
#endif
    }

    operator void *() const { return reinterpret_cast<void *>(static_cast<uintptr_t>(v)); }
    explicit operator u32() const { return v; }
    explicit operator uintptr_t() const { return static_cast<uintptr_t>(v); }

    /* `p + n` on a void*.  Byte arithmetic, which is what GNU C does with it
     * and what the decompilation is relying on; standard C++ has no opinion
     * because it has no such operator. */
    void *operator+(u32 n) const
    { return reinterpret_cast<u8 *>(static_cast<uintptr_t>(v)) + n; }

    /* The cast back out, to whatever the caller knows it really is.  Same
     * reasoning as the primary template's. */
    template <class U>
    explicit operator U *() const { return reinterpret_cast<U *>(static_cast<uintptr_t>(v)); }

    /* From another PTR32 of a different pointee.  The decompilation assigns
     * across pointer types freely -- most often through a void* member -- and
     * in C that is either legal (void*) or a warning the port's -w silences.
     * Matching that here is the point: the 64-bit build should accept what the
     * ILP32 builds accept, no more and no less. */
    template <class U>
    P32(const P32<U> &o) : v(o.v) {}

    /* `obj->unk78 == SomeFunction` -- the same GNU-ism as the constructor
     * above, in a comparison rather than an assignment.  C compares the
     * addresses; so does this. */
    template <class R, class... A>
    bool operator==(R (*f)(A...)) const
    { return v == static_cast<u32>(reinterpret_cast<uintptr_t>(f)); }

    template <class R, class... A>
    bool operator!=(R (*f)(A...)) const
    { return v != static_cast<u32>(reinterpret_cast<uintptr_t>(f)); }
};

/* `n + p` as well as `p + n`.  A member operator only covers the order with
 * the class on the left, and the decompilation writes both. */
inline void *operator+(u32 n, const P32<void> &p)
{
    return reinterpret_cast<u8 *>(static_cast<uintptr_t>(p.v)) + n;
}


/* The reversed operand orders.  A member operator only covers the case with
 * the class on the left; the decompilation writes `SomeFunc != obj->unk78` as
 * well as the other way round. */
template <class T, class R, class... A>
inline bool operator==(R (*f)(A...), const P32<T> &p)
{ return p == f; }

template <class T, class R, class... A>
inline bool operator!=(R (*f)(A...), const P32<T> &p)
{ return p != f; }

static_assert(sizeof(P32<int>) == 4, "P32 must be four bytes -- it is a GBA pointer member");
static_assert(alignof(P32<int>) == 4, "P32 must align to four -- it is a GBA pointer member");
static_assert(sizeof(P32<void>) == 4, "P32<void> must be four bytes");

#define PTR32(...) P32<__VA_ARGS__>

/* A function-pointer member needs its own spelling, because C's declarator
 * syntax puts the name in the middle -- `void (*cb)(struct Task *)` -- and
 * there is no way to write that as `SOMETHING(void (struct Task *)) cb`.  So
 * the macro takes the three pieces and each language assembles them its own
 * way.  P32 handles the type without any special case: T is a function type,
 * T* is a function pointer, and the conversion operator makes `obj->cb(x)`
 * work unchanged.
 *
 *     PTR32_FN(void, cb, (struct Task *));
 */
#define PTR32_FN(ret, name, args) P32<ret args> name

/* A member whose pointer-ness is hidden inside a typedef:
 *
 *     typedef void (*SubGameMenuFunc)(struct SubGameMenu *);
 *     SubGameMenuFunc unk154;      ->   PTR32_TD(SubGameMenuFunc) unk154;
 *
 * The declarator has no star in it, so neither PTR32 nor PTR32_FN can be
 * spelled at the site without knowing what the typedef expands to.  Asking the
 * type system instead works for both shapes at once -- a function-pointer
 * typedef and a data-pointer typedef -- and needs nothing but the name. */
#define PTR32_TD(td) P32<std::remove_pointer_t<td> >

/* A pointer to an array: `const u16 (*unk48)[2]`.  Same problem as PTR32_FN --
 * C wraps the name in the declarator, so there is no type to hand to PTR32 --
 * and the same shape of answer. */
#define PTR32_ARR(type, name, dims) P32<type dims> name

#else /* !__cplusplus */

/* The ILP32 builds.  A pointer is already four bytes, so these are spellings
 * and nothing more, and those builds keep compiling as C. */
#define PTR32(...) __VA_ARGS__ *
#define PTR32_FN(ret, name, args) ret (*name) args
#define PTR32_TD(td) td
#define PTR32_ARR(type, name, dims) type (*name) dims

#endif /* __cplusplus */

#endif /* GUARD_PORT_P32_H */
