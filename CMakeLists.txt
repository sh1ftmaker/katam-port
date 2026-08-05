# katam-port -- the native desktop build.
#
#   make sync            (Makefile; copies and codemods the decompilation)
#   make stubs           (Makefile; works out what is still ARM-only)
#   cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/native -j
#   ./build/native/katam ~/roms/your-copy.gba
#
# `make native` runs the two cmake lines for you.
#
# Why CMake rather than another Makefile target
# ---------------------------------------------
# The web build is one compiler with one set of flags, and a Makefile is the
# right shape for that.  The native build is not: Windows, macOS and arm64
# Linux are coming, and they differ in the things a Makefile is worst at --
# finding SDL (pkg-config here, a framework on macOS, vcpkg or a bundled SDK on
# Windows), what "PIE" is called, and which compiler is even in use.  CMake
# already knows all of that, and it keeps the per-platform knowledge in one
# `if(APPLE)` block instead of spread through a variable soup.
#
# It also means the three platform agents that follow can add a toolchain file
# and a source file, and nothing else.
#
# Nothing here touches the web build.  Different generated sources, different
# object directory, different compiler; the only thing shared is the source
# list and the flags, which is exactly what should be shared.

cmake_minimum_required(VERSION 3.16)
project(katam-port C)

set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# The build has to be ILP32.  This is not a preference and it is not
# negotiable; it is the second decision the port rests on, after the memory
# map, and it is the thing that decides which platforms are reachable at all.
#
# The game is a 32-bit ARM program and the decompilation is its source.  111 of
# its structures have a pointer member, and those structures are not private
# to the C code:
#
#   struct ToneData is read out of the ROM.  On the GBA it is 12 bytes with
#   `wav` at offset 4.  Built for x86-64 it is 24 bytes with `wav` at offset 8,
#   so every instrument in the sound bank is read from the wrong place.
#
#   struct Kirby lives at 0x00020EE0 because linker.ld put it there, and the
#   array extent comes with it.  A wider struct means the array runs past its
#   neighbour, and the game's own allocator and the port disagree about where
#   objects are -- which is exactly the failure docs/ARCHITECTURE.md says the
#   whole address-map decision exists to avoid.
#
# The web build gets this free: wasm32 is ILP32.  A native build has to ask for
# it, and a native build that does not ask is not subtly slower or slightly
# wrong, it is a different program.
#
# There is no run-time check that could catch this, so it is a build-time
# error, and the fix is a toolchain file -- see cmake/toolchain-linux-i686.cmake
# and docs/NATIVE.md.
# ---------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This toolchain has ${CMAKE_SIZEOF_VOID_P}-byte pointers; the port needs "
    "4.\n"
    "The decompilation's structures have to keep their GBA layout -- see the "
    "comment above this message in CMakeLists.txt.\n\n"
    "On x86-64 Linux:\n"
    "    sudo apt install gcc-multilib libsdl2-dev:i386   # or your distro's\n"
    "    cmake -S . -B build/native \\\n"
    "          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-i686.cmake\n\n"
    "`make native` does this for you.  docs/NATIVE.md has the rest, including "
    "what it means for platforms with no 32-bit userland.")
endif()

set(PORT_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/build/port-src")
set(GENERATED  "${CMAKE_CURRENT_SOURCE_DIR}/build/generated")

if(NOT EXISTS "${PORT_SRC}/src")
  message(FATAL_ERROR
    "build/port-src is not there.  The decompilation is copied and codemodded "
    "into it at build time and is not committed:\n"
    "    make sync KATAM_DECOMP=/path/to/your/katam\n"
    "    make stubs KATAM_DECOMP=/path/to/your/katam")
endif()
if(NOT EXISTS "${GENERATED}/port/ram_symbols.h")
  message(FATAL_ERROR "build/generated is incomplete -- run `make sync` first.")
endif()

# ---------------------------------------------------------------------------
# SDL2
#
# 2.0.18 is the floor: SDL_RenderSetIntegerScale, SDL_GetPrefPath and the
# game-controller API are all older than that, but the audio-format defines
# settled around then.
# ---------------------------------------------------------------------------
add_library(katam_sdl2 INTERFACE)

# pkg-config first, because it is the only one of the two that honours
# PKG_CONFIG_LIBDIR -- and on an x86-64 machine building i686 that is the
# difference between finding the right SDL and finding the 64-bit one and
# failing at the link with a wall of "incompatible" messages.  The toolchain
# file sets it.  find_package is the fallback for macOS and Windows, where
# pkg-config is often not installed.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2PC QUIET IMPORTED_TARGET sdl2)
endif()

if(TARGET PkgConfig::SDL2PC)
  target_link_libraries(katam_sdl2 INTERFACE PkgConfig::SDL2PC)
  message(STATUS "SDL2 ${SDL2PC_VERSION} via pkg-config")
elseif(CMAKE_CROSSCOMPILING)
  # Do not fall through to find_package here.  It searches the *host's* CMake
  # package registry, so on an x86-64 machine building i686 it cheerfully finds
  # the 64-bit SDL and hands it to the linker, which then produces four hundred
  # lines about incompatible files.  A missing .pc for this architecture means
  # the 32-bit SDL is not installed, and that is the thing to say.
  message(FATAL_ERROR
    "No sdl2.pc for ${CMAKE_SYSTEM_PROCESSOR} on PKG_CONFIG_LIBDIR "
    "($ENV{PKG_CONFIG_LIBDIR}).\n"
    "Debian/Ubuntu: sudo apt install libsdl2-dev:i386\n"
    "Fedora:        sudo dnf install SDL2-devel.i686\n"
    "Arch:          enable [multilib], then lib32-sdl2\n"
    "Without root, see \"Building without root\" in docs/NATIVE.md.")
else()
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    message(FATAL_ERROR
      "SDL2 not found.  Debian/Ubuntu: libsdl2-dev (and libsdl2-dev:i386 for "
      "the 32-bit build).  Fedora: SDL2-devel.i686.  macOS: brew install sdl2. "
      "Windows: vcpkg install sdl2:x86-windows, or point -DSDL2_DIR at an SDL2 "
      "development package.")
  endif()
  # SDL2::SDL2 exists in SDL's own config package and in vcpkg's; the older
  # FindSDL2 modules only set the variables.
  if(TARGET SDL2::SDL2)
    target_link_libraries(katam_sdl2 INTERFACE SDL2::SDL2)
    if(TARGET SDL2::SDL2main)
      target_link_libraries(katam_sdl2 INTERFACE SDL2::SDL2main)
    endif()
  else()
    target_include_directories(katam_sdl2 INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(katam_sdl2 INTERFACE ${SDL2_LIBRARIES})
  endif()
endif()

# ---------------------------------------------------------------------------
# Sources
#
# The same three lists the Makefile builds, which is the point: there is one
# port, not a web one and a native one.  CONFIGURE_DEPENDS because `make sync`
# adds and removes game sources whenever the decompilation moves, and a build
# that quietly links yesterday's file list is the failure the Makefile's
# `prune` target exists to prevent.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE GAME_SRCS     CONFIGURE_DEPENDS "${PORT_SRC}/src/*.c")
file(GLOB         GEN_SRCS      CONFIGURE_DEPENDS "${GENERATED}/*.c")
file(GLOB         PLATFORM_SRCS CONFIGURE_DEPENDS
                                "${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c")

# The host layer.  platform/web/*.c is emscripten's and is not built here.
set(NATIVE_SRCS
  platform/native/host_sdl.c
  platform/native/audio_sdl.c
  platform/native/save_file.c
  platform/native/mem.c
  platform/native/mp_native.c
  platform/native/png.c)

if(WIN32)
  list(APPEND NATIVE_SRCS platform/native/mem_win32.c
                          platform/native/dialog_win32.c)
else()
  list(APPEND NATIVE_SRCS platform/native/mem_posix.c
                          platform/native/dialog_posix.c)
endif()

add_executable(katam ${GAME_SRCS} ${GEN_SRCS} ${PLATFORM_SRCS} ${NATIVE_SRCS})

target_include_directories(katam PRIVATE
  "${PORT_SRC}/include" "${GENERATED}" "${CMAKE_CURRENT_SOURCE_DIR}/platform")

# ---------------------------------------------------------------------------
# The game's compile flags.  These are load-bearing; see the Makefile, which
# has the same set and the same reasons.
#
#   PORTABLE      the decomp's own guard for the register pins and codegen
#                 barriers that only exist to make agbcc emit exact ARM.
#   NONMATCHING   take the readable C branch wherever a function has one.
#   -fgnu89-inline  the decomp is agbcc (gcc 2.95), where a plain `inline`
#                 definition also emits an external one.  C99 inverted that,
#                 and without this flag every `inline void Foo(...)` in the
#                 game becomes an undefined symbol at link time.
#   -fwrapv, -fno-strict-aliasing
#                 the game overflows signed arithmetic and reads structures
#                 through mismatched pointers, both routinely, and both were
#                 fine on the compiler it was written for.
# ---------------------------------------------------------------------------
target_compile_definitions(katam PRIVATE PORTABLE NONMATCHING MODERN=1)

set(FORCE_INCLUDES port/prelude.h port/ram_symbols.h port/rom_data.h)

include(CheckCCompilerFlag)

if(MSVC)
  message(FATAL_ERROR
    "MSVC cannot build this: the decompilation is GNU C -- statement "
    "expressions, __attribute__, gnu89 inline semantics.  Use clang-cl "
    "(-T ClangCL) or a MinGW toolchain.")
endif()

# The generated headers replace declarations that used to live in the game's
# own headers, so they have to be in scope before anything else is parsed.
#
# SHELL: is not decoration.  target_compile_options de-duplicates its list, and
# three `-include` flags reduce to one followed by three filenames -- which the
# compiler reads as "compile these four files into one object" and refuses.
foreach(h ${FORCE_INCLUDES})
  target_compile_options(katam PRIVATE "SHELL:-include ${h}")
endforeach()

target_compile_options(katam PRIVATE
  -std=gnu99 -fgnu89-inline -fno-strict-aliasing -fwrapv)

# The decompilation is not warning-clean and is not meant to be: it is the
# output of matching a 2004 ARM binary.  The Makefile uses -Wno-everything,
# which is clang's; -w is the one both compilers understand, and gcc accepts
# -Wno-everything silently while doing nothing with it, so pass both.
target_compile_options(katam PRIVATE -w)
check_c_compiler_flag("-Wno-everything" HAVE_WNO_EVERYTHING)
if(HAVE_WNO_EVERYTHING)
  target_compile_options(katam PRIVATE -Wno-everything)
endif()

# Some of those are errors rather than warnings on gcc 14 and clang 16, where
# -w does not save you: C99 removed implicit declarations and the compilers
# finally acted on it.  The decomp has plenty.  Demote them where the compiler
# understands how.
foreach(diag implicit-function-declaration implicit-int int-conversion
             incompatible-pointer-types return-mismatch)
  string(MAKE_C_IDENTIFIER "HAVE_NOERR_${diag}" have_var)
  check_c_compiler_flag("-Wno-error=${diag}" ${have_var})
  if(${have_var})
    target_compile_options(katam PRIVATE -Wno-error=${diag})
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Where the executable lands in the address space.
#
# The whole port depends on the GBA's regions being available at their true
# addresses -- 0x02000000 through 0x0A000000.  A position-independent
# executable is loaded high and takes its .data, .bss and brk heap with it; a
# non-PIE one loads at 0x400000 and grows its heap straight into EWRAM.  So PIE
# is not a hardening preference here, it is the reason the reservation works.
# platform/native/mem.c checks at run time and says so if this went wrong.
# ---------------------------------------------------------------------------
include(CheckPIESupported OPTIONAL RESULT_VARIABLE HAVE_CHECK_PIE)
if(HAVE_CHECK_PIE)
  check_pie_supported()
endif()
set_property(TARGET katam PROPERTY POSITION_INDEPENDENT_CODE TRUE)

if(APPLE)
  # Every 64-bit Mach-O gets a 4 GiB __PAGEZERO, which covers the entire GBA
  # map and cannot be mapped into.  Shrinking it to one page is the whole of
  # the macOS memory story.  Untested here -- see docs/NATIVE.md.
  target_link_options(katam PRIVATE "-Wl,-pagezero_size,0x1000")
endif()

target_link_libraries(katam PRIVATE katam_sdl2)
if(NOT WIN32)
  target_link_libraries(katam PRIVATE m)
endif()

# Optimise the game even in a Debug build unless someone asks otherwise: -O0
# turns the software PPU into a slideshow, and a frame that takes 200 ms
# changes what the game does.
target_compile_options(katam PRIVATE $<$<CONFIG:Debug>:-O1>)
