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

# C and CXX.  The port is a C program on every ILP32 target and that is what it
# stays; CXX is here for the 64-bit builds, which compile the same sources
# through a C++ front end so that a structure's pointer members can keep the
# four bytes the console gave them.  platform/port/p32.h is the whole of the
# reason and docs/SIXTYFOUR.md is the measurement behind it.  Enabling the
# language costs an ILP32 build nothing -- no source changes language unless
# CMAKE_SIZEOF_VOID_P says it has to.
project(katam-port C CXX)

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
#
# KATAM_ALLOW_LP64 turns the error into a warning, for measuring the 64-bit
# conversion rather than for building anything.  It is not a step towards a
# 64-bit build and it does not produce a working game: it produces a program
# whose structures are the wrong shape, which is the thing the paragraph above
# says has no symptom.  docs/SIXTYFOUR.md is what it is for.
# ---------------------------------------------------------------------------
option(KATAM_ALLOW_LP64
       "EXPERIMENT ONLY: configure with 8-byte pointers.  The game will be \
wrong.  See docs/SIXTYFOUR.md." OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND KATAM_ALLOW_LP64)
  message(WARNING
    "================================================================\n"
    "KATAM_ALLOW_LP64 is on and this toolchain has "
    "${CMAKE_SIZEOF_VOID_P}-byte pointers.\n"
    "\n"
    "This is not a 64-bit build of the port.  148 of the game's 246\n"
    "structures change size or move a member under LP64, including the\n"
    "sound bank read out of the ROM and the Kirby array linker.ld placed\n"
    "at a fixed address.  A binary produced this way boots, renders, and\n"
    "is permanently and silently wrong.\n"
    "\n"
    "platform/gba_layout_check.c will refuse to compile and will name\n"
    "every structure that moved.  That is the intended output.  To count\n"
    "the *other* breakage instead, add -DKATAM_SKIP_LAYOUT_CHECK=ON.\n"
    "\n"
    "docs/SIXTYFOUR.md has the measurement this switch exists for.\n"
    "================================================================")
elseif(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This toolchain has ${CMAKE_SIZEOF_VOID_P}-byte pointers; the port needs "
    "4.\n"
    "The decompilation's structures have to keep their GBA layout -- see the "
    "comment above this message in CMakeLists.txt.\n\n"
    "On x86-64 Linux:\n"
    "    sudo apt install gcc-multilib libsdl2-dev:i386   # or your distro's\n"
    "    cmake -S . -B build/native \\\n"
    "          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-i686.cmake\n\n"
    "On aarch64 -- a 64-bit Raspberry Pi OS, say -- there is no ILP32 userland "
    "to switch to.  Build armhf instead and run it under the kernel's 32-bit "
    "support:\n"
    "    cmake -S . -B build/native \\\n"
    "          -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-armhf.cmake\n\n"
    "`make native` does the x86 case for you.  docs/NATIVE.md has the rest, "
    "including which arm64 kernels can run a 32-bit binary at all and what a "
    "platform with no 32-bit userland would need.\n\n"
    "If you are measuring what a 64-bit port would cost rather than trying to "
    "play the game, -DKATAM_ALLOW_LP64=ON turns this into a warning.  Read "
    "docs/SIXTYFOUR.md first; the result is not a working build and is not "
    "meant to be.")
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
if(PkgConfig_FOUND AND NOT WIN32)
  pkg_check_modules(SDL2PC QUIET IMPORTED_TARGET sdl2)
endif()

# Windows takes the other road, for two reasons.  The sdl2.pc inside SDL's
# MinGW development tarball has the build machine's `prefix=` baked into it and
# points at a directory that does not exist on yours; sdl2-config.cmake in the
# same tarball works out its prefix from its own location and is correct
# wherever the tarball was unpacked.  And SDL_NO_MWINDOWS below has no
# pkg-config equivalent at all.
if(WIN32)
  # -mwindows would make this a GUI-subsystem program, and a GUI-subsystem
  # program on Windows has no stdout.  Everything this port has to say -- the
  # memory map, a refused DMA, the message explaining why the reservation
  # failed and the game is not going to start -- goes to stdout, and
  # tools/native_smoke.sh reads it.  A console window next to the game window
  # is the price; a silent failure is not worth paying instead.  SDL's CMake
  # package removes the flag when this is set, and only then.
  set(SDL2_NO_MWINDOWS TRUE)
endif()

if(TARGET PkgConfig::SDL2PC)
  target_link_libraries(katam_sdl2 INTERFACE PkgConfig::SDL2PC)
  message(STATUS "SDL2 ${SDL2PC_VERSION} via pkg-config")
elseif(CMAKE_CROSSCOMPILING AND NOT WIN32)
  # Do not fall through to find_package here.  It searches the *host's* CMake
  # package registry, so on an x86-64 machine building i686 it cheerfully finds
  # the 64-bit SDL and hands it to the linker, which then produces four hundred
  # lines about incompatible files.  A missing .pc for this architecture means
  # the 32-bit SDL is not installed, and that is the thing to say.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    # "aarch64" does not contain "arm", so this has to be tested first and by
    # name; without it an arm64 build is told to install SDL2-devel.i686.
    set(SDL_HINT
      "Debian/Ubuntu: sudo apt install libsdl2-dev:arm64   (needs "
      "dpkg --add-architecture arm64; the arm64 packages come from "
      "ports.ubuntu.com, not the main archive)\n")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(SDL_HINT
      "Debian/Ubuntu: sudo apt install libsdl2-dev:armhf   (needs "
      "dpkg --add-architecture armhf, and apt will only do it on an armhf or "
      "arm64 machine)\n")
  else()
    set(SDL_HINT
      "Debian/Ubuntu: sudo apt install libsdl2-dev:i386\n"
      "Fedora:        sudo dnf install SDL2-devel.i686\n"
      "Arch:          enable [multilib], then lib32-sdl2\n")
  endif()
  message(FATAL_ERROR
    "No sdl2.pc for ${CMAKE_SYSTEM_PROCESSOR} on PKG_CONFIG_LIBDIR "
    "($ENV{PKG_CONFIG_LIBDIR}).\n"
    ${SDL_HINT}
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
    # SDL2main is deliberately not linked on Windows.  It supplies a WinMain
    # that calls SDL_main, which needs platform/main.c's main() to have been
    # renamed by SDL_main.h -- and main.c does not include SDL at all, by
    # design, because it is shared with the emscripten build.  The Windows
    # build defines SDL_MAIN_HANDLED and calls SDL_SetMainReady() instead; see
    # platform/native/host_sdl.c.
    if(TARGET SDL2::SDL2main AND NOT WIN32)
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

# platform/gba_layout_check.c is 2000-odd _Static_asserts over the size and
# every member offset of every structure the decompilation defines, generated
# from the ILP32 build by tools/gen_gba_layout.py.  It is picked up by the glob
# above and by the Makefile's, so all four builds check the same table without
# anything being wired up per platform.
#
# The only reason to drop it is the LP64 measurement: with the assertions in,
# the build stops at the first structure that moved, and the point of that
# experiment is to count everything *else* that breaks.  This is not a way to
# make a 64-bit build work.
option(KATAM_SKIP_LAYOUT_CHECK
       "EXPERIMENT ONLY: do not compile the GBA layout assertions." OFF)
if(KATAM_SKIP_LAYOUT_CHECK)
  list(REMOVE_ITEM PLATFORM_SRCS
       "${CMAKE_CURRENT_SOURCE_DIR}/platform/gba_layout_check.c")
  message(WARNING
    "KATAM_SKIP_LAYOUT_CHECK is on: the GBA struct layout is not being "
    "checked.  Nothing else in this build will notice a structure that "
    "changed shape.")
endif()

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

# ---------------------------------------------------------------------------
# The 64-bit builds compile as C++
#
# Not because anybody wanted C++, and not for one file: a GBA structure's
# pointer members have to stay four bytes wide on a host whose pointers are
# eight, and PTR32 in platform/port/p32.h is the only way to say that.  The
# headers those structures live in are included by nearly every translation
# unit, so the choice is all of them or none of them.
#
# All of them, and that is deliberate rather than lazy.  Compiling half the
# tree as C++ would mangle half the names and leave the other half expecting C
# linkage, so every call across the seam would fail to link -- or, worse,
# resolve to something else.  One language throughout means one mangling
# throughout and no seam to get wrong.
#
# tools/cxxify.py is what makes the decompilation's C acceptable to a C++ front
# end.  Every transform it applies is valid C as well, so the ILP32 builds
# compile the identical tree and stay the reference this build is checked
# against.
# ---------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_source_files_properties(
    ${GAME_SRCS} ${GEN_SRCS} ${PLATFORM_SRCS} ${NATIVE_SRCS}
    PROPERTIES LANGUAGE CXX)
  set_target_properties(katam PROPERTIES LINKER_LANGUAGE CXX)
  message(STATUS
    "64-bit target: compiling as C++ so pointer members can stay 4 bytes "
    "(platform/port/p32.h)")
endif()

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
#   -funsigned-char
#                 plain `char` is unsigned on the GBA and the flag is what says
#                 so out loud.  Measured, not assumed -- agbcc compiles
#                 `char c = -1; return c < 0;` to `mov r0, #0` and warns that
#                 the comparison is always false.  x86 and wasm both default to
#                 signed, ARM and PowerPC to unsigned, so without this flag the
#                 armhf build would quietly be a different program from the
#                 i686 one and nothing would report it.  Pinning it to what the
#                 console did is the only choice that makes every build the same
#                 program *and* the right one.
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

# -fno-strict-aliasing, -fwrapv and -funsigned-char are the semantics of the
# console and apply whichever front end is parsing the file.  The standard
# level cannot: -std=gnu99 and -fgnu89-inline are C spellings, and a 64-bit
# build is compiling these same files as C++.
#
# -fpermissive on the C++ side is doing one specific job, not papering over the
# port.  C++ forbids the implicit void*-to-T* conversion that the decompilation
# performs constantly -- `void *p = TaskGetStructPtr(t); chest = p;` is the
# shape of every object constructor in the game -- and -fpermissive demotes
# exactly that to a warning.  Everything else C++ objects to was fixed at the
# source by tools/cxxify.py, and there are no remaining diagnostics.
target_compile_options(katam PRIVATE
  -fno-strict-aliasing -fwrapv -funsigned-char
  $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
  $<$<COMPILE_LANGUAGE:C>:-fgnu89-inline>
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
  $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)

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
    # C only: every one of these names a C diagnostic, and C++ either has no
    # such warning or makes the construct an error that no -Wno-error can
    # demote.  Passing them to g++ is at best noise and at worst a build that
    # fails on the flag rather than on the code.
    target_compile_options(katam PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-error=${diag}>)
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
# Windows is the exception and is handled below: PE has no PIE, the answer
# there is a fixed image base above the map, and asking CMake for -pie on
# MinGW would fight it.
if(NOT WIN32)
  include(CheckPIESupported OPTIONAL RESULT_VARIABLE HAVE_CHECK_PIE)
  if(HAVE_CHECK_PIE)
    check_pie_supported()
  endif()
  set_property(TARGET katam PROPERTY POSITION_INDEPENDENT_CODE TRUE)
endif()

if(APPLE)
  # Every 64-bit Mach-O gets a 4 GiB __PAGEZERO, which covers the entire GBA
  # map and cannot be mapped into.  Shrinking it to one page is the whole of
  # the macOS memory story.  Untested here -- see docs/NATIVE.md.
  target_link_options(katam PRIVATE "-Wl,-pagezero_size,0x1000")
endif()

if(WIN32)
  # Windows wants the opposite of what Linux wants, for the same reason.
  #
  # On Linux the executable must be PIE, because a non-PIE one loads at
  # 0x400000 and its brk heap grows upward from there straight into EWRAM.  A
  # Windows process has no brk: every heap, stack and mapping comes from
  # VirtualAlloc, which hands out whatever is free -- and after
  # PortNativeReserveMap has run, the GBA window is not free.  So the only
  # thing that can collide with the map is something already placed before
  # main(): the image itself, and the DLLs in the import table.
  #
  # ASLR is what makes that a lottery.  With --dynamicbase the loader may put
  # this image anywhere, including inside 0x02000000..0x0A000000, and the
  # program would then fail to start on some boots and not others.  A fixed
  # base above the whole map removes the image from the question entirely, and
  # leaves platform/native/mem.c's "is this binary below the top of the map"
  # check meaning exactly what it means on Linux.  0x10000000 is 256 MiB, well
  # clear of the map's top at 0x0A000000 and well below where the loader bases
  # system DLLs.
  #
  # This is a real loss of hardening and it is the same trade the port makes on
  # every platform: the addresses are the program.  DLLs keep their own ASLR --
  # only this image is pinned -- and if one of them does land in the window,
  # platform/native/mem_win32.c walks the address space with VirtualQuery and
  # names it rather than failing mysteriously.
  target_link_options(katam PRIVATE
    "-Wl,--image-base,0x10000000"
    "-Wl,--disable-dynamicbase"
    "-Wl,--nxcompat"
    # Console subsystem, said out loud rather than left to the default,
    # because -mwindows arriving from anywhere would silently take stdout
    # away.  See the SDL2_NO_MWINDOWS comment above.
    "-Wl,--subsystem,console")

  # Keep platform/main.c's main() as main(): SDL_main.h renames it otherwise,
  # and it is shared with the emscripten build.  host_sdl.c calls
  # SDL_SetMainReady() to make up for the SDL2main that is not linked.
  target_compile_definitions(katam PRIVATE SDL_MAIN_HANDLED)

  # GetOpenFileNameW.
  target_link_libraries(katam PRIVATE comdlg32)
endif()

target_link_libraries(katam PRIVATE katam_sdl2)
if(NOT WIN32)
  target_link_libraries(katam PRIVATE m)
endif()

# Optimise the game even in a Debug build unless someone asks otherwise: -O0
# turns the software PPU into a slideshow, and a frame that takes 200 ms
# changes what the game does.
target_compile_options(katam PRIVATE $<$<CONFIG:Debug>:-O1>)

# ---------------------------------------------------------------------------
# Packaging, Windows only
#
# A Windows player does not have a package manager to fetch SDL2 from, so the
# .exe alone is not a thing anyone can run: it exits before main with "SDL2.dll
# was not found".  `cmake --build . --target package-windows` assembles the
# folder that is: the executable, the DLL SDL's own development package ships
# next to it, and a readme saying the one thing that matters, which is that
# there is no game in here and the player supplies their own ROM.
#
# The DLL is copied from SDL2::SDL2's IMPORTED_LOCATION, so it is whichever
# SDL the build actually linked against and cannot drift from it.  Nothing is
# vendored into the repository: SDL is linked, and its licence and readme
# travel with the folder.
# ---------------------------------------------------------------------------
if(WIN32)
  set(PKG_DIR "${CMAKE_BINARY_DIR}/katam-port-windows-i686")

  # The repository keeps the readme with Unix line endings like everything
  # else; the copy that ships gets CRLF, because it is going to be opened in
  # Notepad by someone who has never heard of this program.
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/platform/native/README-windows.txt"
                 "${CMAKE_BINARY_DIR}/README.txt"
                 @ONLY NEWLINE_STYLE CRLF)

  if(TARGET SDL2::SDL2)
    set(PKG_SDL_COPY COMMAND ${CMAKE_COMMAND} -E copy_if_different
                             "$<TARGET_FILE:SDL2::SDL2>" "${PKG_DIR}/")
  else()
    # A pkg-config or FindSDL2 path that never produced an imported target has
    # no file to point at.  Say so at build time rather than shipping a folder
    # that is missing the one thing it exists to carry.
    set(PKG_SDL_COPY COMMAND ${CMAKE_COMMAND} -E echo
        "package-windows: no SDL2::SDL2 target -- copy SDL2.dll in by hand")
  endif()

  add_custom_target(package-windows
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PKG_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:katam>" "${PKG_DIR}/"
    ${PKG_SDL_COPY}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/README.txt" "${PKG_DIR}/README.txt"
    DEPENDS katam
    COMMENT "Assembling ${PKG_DIR}")
endif()
