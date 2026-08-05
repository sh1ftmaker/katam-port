/* gba_layout_check.c -- compile the layout assertions, and nothing else.
 *
 * port/gba_layout.h is 2000-odd _Static_asserts over the size and every member
 * offset of every structure the decompilation defines, generated from the
 * port's ILP32 build by tools/gen_gba_layout.py.  This file exists to put them
 * in front of a compiler.
 *
 * It is a translation unit of its own rather than an include from somewhere
 * useful for two reasons.  The assertions need every game header in scope at
 * once, which no real source file wants; and every build the port has --
 * wasm32, i686, armhf, mingw32 -- picks up platform/*.c automatically, so all
 * four check the same table without anything being wired up per platform.  A
 * host whose ABI moves a member fails here, by name, instead of booting and
 * being quietly wrong.
 *
 * There is no code.  The empty-translation-unit warning is the only thing to
 * suppress, and the port compiles with -w anyway.
 */

#include "port/gba_layout.h"

/* C linkage for the 64-bit builds.
 *
 * Those compile the game as C++ and tools/cxxify.py gives it C linkage, so
 * everything on the seam has to agree.  It is applied to platform/*.c as a
 * class rather than to the files that happened to break: the failure is an
 * undefined reference to a mangled name, or -- for a `const`, which is
 * internal-linkage in C++ and external in C -- to a symbol that is plainly
 * defined a few lines away.  Neither says which file to fix, and the set of
 * files that need it changes whenever a declaration moves.  A no-op in C.
 *
 * The block opens below the includes so that SDL and the system headers stay
 * outside it. */
#ifdef __cplusplus
extern "C" {
#endif

/* ISO C forbids an empty translation unit; this is the conventional answer. */
typedef int katam_layout_check_translation_unit_is_not_empty;

#ifdef __cplusplus
}
#endif
