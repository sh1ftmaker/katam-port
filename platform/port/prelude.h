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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* The Windows member of that list, and it is not obvious: MinGW's <intrin.h>
 * redeclares abs, memcpy and friends as compiler intrinsics, and SDL_cpuinfo.h
 * includes it -- so <SDL.h> in platform/native/*.c is parsed with global.h's
 * `abs` macro already live and every declaration on that line becomes a syntax
 * error.  Same failure as <stdlib.h>, same fix: set the guard first. */
#include <intrin.h>
#endif

/* The 64-bit builds compile the game as C++, so that a GBA structure's pointer
 * members can stay four bytes wide -- see docs/SIXTYFOUR.md.  This is a no-op
 * for the C builds and must stay one. */
#include "port/cxx_compat.h"

/* PTR32, the four-byte pointer member itself.  It has to be visible before any
 * of the game's headers are parsed, because that is where it is used, and it
 * has to be outside the extern "C" block below, because a template cannot have
 * C language linkage. */
#include "port/p32.h"

/* The port's own hooks, declared here so that every translation unit sees a
 * prototype.  portify.py rewrites `asm("swi 3")` into a PortHalt() call inside
 * the game's own main.c, and the asm-wrapper stubs it generates call
 * PortMissingFunction -- neither file includes the port headers, and an
 * implicit declaration would give those calls the wrong wasm signature.
 *
 * "Wrong signature" is not a warning here, it is a trap.  An implicit
 * declaration is `int f()`, wasm-ld sees a call typed (...)->i32 against a
 * definition typed (...)->void, and rather than fail it emits a stub whose
 * entire body is `unreachable` and points the call at that.  The build
 * succeeds with one warning; the program dies the first time the call runs.
 * That is what happened to PortTrace below -- see the note in
 * tools/portify.py:trace_star_states.  Anything portify.py injects into the
 * game's own sources belongs in this list.
 *
 * uint32_t rather than u32: this header is parsed before gba/types.h, and it
 * is the same type (types.h does `typedef uint32_t u32`), so the declaration
 * here and the one in port/port.h agree.
 *
 * extern "C" for the 64-bit builds.  Those compile the game as C++ and
 * tools/cxxify.py gives every game header and source C linkage, so that the
 * C++ build links by the same rules the C builds do -- see cxxify.py's
 * "linkage" section.  AgbMain is defined over there and called from here, so
 * this declaration has to agree with it; the Port* hooks are the other
 * direction, defined in platform/ and called from the game, and they have to
 * agree for the same reason. */
#ifdef __cplusplus
extern "C" {
#endif

void PortMissingFunction(const char *name);
void PortUnimplemented(const char *what);
void PortHalt(void);
void PortTrace(const char *tag, uint32_t a, uint32_t b, uint32_t c);

/* Defined by the game in src/main.c; the port calls it to start. */
void AgbMain(void);

#ifdef __cplusplus
}
#endif

/* --- the sound engine's own globals, at their hardware addresses -----------
 *
 * These are the same problem tools/gen_ram_symbols.py solves for the game's
 * 189 linker-script globals, but they come from a section placement
 * (`. = 0x00000560; src/m4a.o(common_data);` in linker.ld) rather than a named
 * symbol, so the generator does not see them.
 *
 * They cannot be ordinary C globals here, and the reason is specific: the ROM
 * holds gMPlayTable, four {info, tracks, ...} records whose pointers are the
 * GBA addresses below.  m4aSoundInit walks that table and calls
 * MPlayOpen(gMPlayTable[i].info, ...), while the game's own code writes
 * m4aMPlayFadeOut(&gMPlayInfo_1, ...).  If gMPlayInfo_1 is a compiler-placed
 * global those two are different objects and the fade silently applies to
 * nothing.  Putting them where the ROM already thinks they are is the same
 * decision the whole port rests on -- see docs/ARCHITECTURE.md.
 *
 * tools/portify.py deletes the definitions in src/m4a.c and comments the
 * matching externs out of gba/m4a.h, exactly as gen_ram_symbols.py does.
 * Addresses are from katam.map (src/m4a.o(common_data) and
 * data/sound_data.o(ewram_data)).
 *
 * gNumMusicPlayers and gMaxLines are not storage at all: linker.ld sets them
 * to the literal values 4 and 0 and m4a.h casts the *address* to an integer.
 * gMaxLines being zero is load-bearing -- it is what keeps SoundMain's
 * scanline-budget branch dormant, so the mixer never reads REG_VCOUNT and
 * never has to be resumable half way through a frame.
 */
#define gSoundInfo         (*(struct SoundInfo *)0x03000560)
/* PTR32_TD, because this one is an *array* at a fixed address with a fixed
 * extent.  linker.ld gives gMPlayJumpTable 0x90 bytes -- 36 entries of four --
 * and MPlayFunc is a function pointer, so on a 64-bit host the entries become
 * eight bytes and index 35 lands at 0x03001628, past the end of the table and
 * inside gCgbChans, which nothing has written.  Clear64byte then calls zero.
 *
 * That is the failure docs/SIXTYFOUR.md predicted from the address map before
 * any of this was built, and it is the first thing the narrowing had to fix
 * that is not a structure member. */
#define gMPlayJumpTable    ((PTR32_TD(MPlayFunc) *)0x03001510)
#define gCgbChans          ((struct CgbChannel *)0x030015A0)
#define gMPlayInfo_0       (*(struct MusicPlayerInfo *)0x030016A0)
#define gMPlayInfo_1       (*(struct MusicPlayerInfo *)0x030016E0)
#define gMPlayInfo_2       (*(struct MusicPlayerInfo *)0x03001720)
#define gMPlayMemAccArea   ((u8 *)0x03001760)
#define gMPlayInfo_3       (*(struct MusicPlayerInfo *)0x03001770)
#define gNumMusicPlayers   ((char *)4)
#define gMaxLines          ((char *)0)

#endif
