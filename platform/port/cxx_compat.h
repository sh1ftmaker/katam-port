#ifndef GUARD_PORT_CXX_COMPAT_H
#define GUARD_PORT_CXX_COMPAT_H

/* The parts of "the decompilation's C, seen by a C++ compiler" that belong in
 * a header rather than in a codemod.
 *
 * Only the 64-bit builds compile the game as C++, and only because a 4-byte
 * pointer member cannot be spelled in C -- see docs/SIXTYFOUR.md and
 * platform/port/p32.h.  tools/cxxify.py does the source-level half of the job;
 * what is here is the half where a definition serves better than a rewrite,
 * because it covers constructs the decompilation has not written yet as well
 * as the ones it has.
 *
 * Nothing in this file is included by the C builds, and nothing in it may
 * change layout or code generation -- it exists to make C++ accept what C
 * already accepted, never to alter what either one means.
 */

#ifdef __cplusplus

#include <type_traits>

/* ------------------------------------------------------------------------ *
 * Incrementing an enumeration
 *
 * `for (i = FIRST; i < N; i++)` over an enum is ordinary C and has no built-in
 * meaning in C++.  Three sites do it today (two in pause_world_map.c, one in
 * save.c).  A codemod could rewrite those three, but it would have to know
 * each variable's type to write the cast, and it would miss the fourth site
 * the moment the decompilation adds one.  Defining the operator costs nothing
 * and cannot miss.
 *
 * Constrained to enums so it can never be selected for anything else, and
 * written to match C's semantics exactly: promote, add one, convert back.
 * ------------------------------------------------------------------------ */

template <class E>
inline typename std::enable_if<std::is_enum<E>::value, E &>::type
operator++(E &e)
{
    e = static_cast<E>(static_cast<int>(e) + 1);
    return e;
}

template <class E>
inline typename std::enable_if<std::is_enum<E>::value, E>::type
operator++(E &e, int)
{
    E old = e;
    e = static_cast<E>(static_cast<int>(e) + 1);
    return old;
}

template <class E>
inline typename std::enable_if<std::is_enum<E>::value, E &>::type
operator--(E &e)
{
    e = static_cast<E>(static_cast<int>(e) - 1);
    return e;
}

/* ------------------------------------------------------------------------ *
 * Passing a member to a transparent union
 *
 * GCC's __attribute__((transparent_union)) is a C-only feature: a function
 * taking such a union may be called with any member's type directly.  g++
 * parses the attribute, warns that it is ignoring it, and then rejects every
 * such call.  The decompilation has five transparent unions -- LevelInfo_1E0,
 * AnimCmd, Unk_08930E00, Unk_03002E60 and one anonymous -- and two call sites
 * that rely on the C behaviour.
 *
 * The member has to be named rather than left to fall out of brace order.  C
 * picks the member whose type matches the argument; plain `AnimCmd{cmd}`
 * initialises the *first* member instead, which for AnimCmd is
 * `const struct AnimCmd_GetTiles *` while the argument is `const s32 *`.
 * Naming it makes the two languages pick the same member by construction, and
 * a wrong name is a compile error rather than a reinterpreted pointer.
 *
 * In C it expands to the argument untouched, so the C builds are bit-for-bit
 * unaffected and the codemod that inserts it stays unconditional.
 * ------------------------------------------------------------------------ */

#define PORT_TRANSPARENT(uniontype, member, value) (uniontype{. member = (value)})

#else /* !__cplusplus */

#define PORT_TRANSPARENT(uniontype, member, value) (value)

#endif /* __cplusplus */

#endif /* GUARD_PORT_CXX_COMPAT_H */
