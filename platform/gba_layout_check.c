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

/* ISO C forbids an empty translation unit; this is the conventional answer. */
typedef int katam_layout_check_translation_unit_is_not_empty;
