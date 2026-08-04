#ifndef GUARD_PORT_PRELUDE_H
#define GUARD_PORT_PRELUDE_H

/* Force-included ahead of every translation unit, before the game's own
 * headers.
 *
 * Two reasons it has to come first:
 *
 *  - The game uses INT_MAX / SHRT_MAX / USHRT_MAX without including
 *    <limits.h>, because agbcc's headers supplied them.
 *
 *  - global.h defines `abs` as a macro.  Any system header parsed after that
 *    point sees `int abs (int);` turn into nonsense, which takes out
 *    <stdlib.h> and everything that includes it -- <emscripten.h> among them.
 *    Pulling the system headers in here means their include guards are already
 *    set by the time the macro exists.
 */

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The port's own hooks, declared here so that every translation unit sees a
 * prototype.  portify.py rewrites `asm("swi 3")` into a PortHalt() call inside
 * the game's own main.c, and the asm-wrapper stubs it generates call
 * PortMissingFunction -- neither file includes the port headers, and an
 * implicit declaration would give those calls the wrong wasm signature. */
void PortMissingFunction(const char *name);
void PortUnimplemented(const char *what);
void PortHalt(void);

/* Defined by the game in src/main.c; the port calls it to start. */
void AgbMain(void);

#endif
