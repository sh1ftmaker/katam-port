# 32-bit Windows, cross-compiled with MinGW-w64 from a Linux host (or built on
# Windows itself under MSYS2's mingw32 shell, where the compiler is already on
# PATH and CMAKE_C_COMPILER below finds it).
#
#   cmake -S . -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-i686.cmake
#   cmake --build build/win32 -j
#   cmake --build build/win32 --target package-windows
#
# x86, not x64.  CMakeLists.txt explains at length why the port has to be
# ILP32 and refuses to configure otherwise; the short version is that 111 of
# the decompilation's structures contain a pointer and are read out of the ROM
# at addresses linker.ld chose.  There is no 64-bit Windows build to be had
# here for the same reason there is no macOS build.
#
# MSVC cannot compile the decompilation at all -- it is GNU C, with statement
# expressions, __attribute__ and gnu89 inline semantics -- so this is MinGW.
# clang-cl with --target=i686-pc-windows-gnu would also work and nobody has
# tried it.
#
# SDL2
# ----
# Windows has no pkg-config and no system SDL.  Point KATAM_SDL2_MINGW at the
# unpacked SDL2-devel-<version>-mingw tarball from libsdl.org -- specifically
# at its i686-w64-mingw32 subdirectory, which holds include/, lib/ and the
# bin/SDL2.dll that `package-windows` ships:
#
#   curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-mingw.tar.gz
#   tar xf SDL2-devel-2.30.11-mingw.tar.gz
#   cmake ... -DKATAM_SDL2_MINGW=$PWD/SDL2-2.30.11/i686-w64-mingw32
#
# Its sdl2-config.cmake works out the prefix from its own location, so the
# tarball can live anywhere.  Its sdl2.pc cannot -- the `prefix=` in there is a
# path on the machine that built the release -- which is why CMakeLists.txt
# does not consult pkg-config on Windows.
#
# There is no vendored copy of SDL in this repository and there should not be
# one: it is linked, and the DLL that ships is the one from libsdl.org.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(KATAM_MINGW_PREFIX i686-w64-mingw32 CACHE STRING
    "MinGW-w64 target triple (i686-w64-mingw32)")

# Debian and Ubuntu install the driver under three names -- plain, -posix and
# -win32 -- and which of them exists depends on which alternative was
# selected.  Any of them builds this; the threading model only matters for
# libstdc++, and there is no C++ here.
find_program(CMAKE_C_COMPILER
             NAMES ${KATAM_MINGW_PREFIX}-gcc
                   ${KATAM_MINGW_PREFIX}-gcc-posix
                   ${KATAM_MINGW_PREFIX}-gcc-win32
             DOC "MinGW-w64 C compiler for ${KATAM_MINGW_PREFIX}")
if(NOT CMAKE_C_COMPILER)
  message(FATAL_ERROR
    "No ${KATAM_MINGW_PREFIX}-gcc on PATH.\n"
    "Debian/Ubuntu: sudo apt install gcc-mingw-w64-i686\n"
    "Fedora:        sudo dnf install mingw32-gcc\n"
    "Arch:          pacman -S mingw-w64-gcc\n"
    "Without root, see \"Building without root\" in docs/NATIVE.md -- the\n"
    "packages unpack into a scratch directory and the compiler relocates.")
endif()

find_program(CMAKE_RC_COMPILER NAMES ${KATAM_MINGW_PREFIX}-windres)

# Cross-compiling: look for headers and libraries in the target's world, and
# for programs in the host's.  Without this find_package(SDL2) will happily
# find the *host's* SDL and hand a Linux .so to a PE linker.
if(NOT DEFINED KATAM_SDL2_MINGW AND DEFINED ENV{KATAM_SDL2_MINGW})
  set(KATAM_SDL2_MINGW "$ENV{KATAM_SDL2_MINGW}")
endif()

if(KATAM_SDL2_MINGW)
  list(APPEND CMAKE_FIND_ROOT_PATH "${KATAM_SDL2_MINGW}")
  # find_package(SDL2) looks here for sdl2-config.cmake.
  set(SDL2_DIR "${KATAM_SDL2_MINGW}/lib/cmake/SDL2" CACHE PATH
      "SDL2's CMake package inside the MinGW development tarball")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must not be consulted at all.  With PKG_CONFIG_LIBDIR unset it
# reads the host's /usr/lib/pkgconfig and reports the host's 64-bit SDL as if
# it were the target's, and the first sign of trouble is several hundred lines
# from the linker.  CMakeLists.txt already skips pkg-config on Windows; this
# is the belt to that pair of braces.
set(ENV{PKG_CONFIG_LIBDIR} "")
set(ENV{PKG_CONFIG_PATH} "")

# -static-libgcc so the folder that ships is katam.exe and SDL2.dll and
# nothing else.  libgcc_s_dw2-1.dll is the only other runtime dependency a C
# program picks up from MinGW, it exists only to unwind exceptions there are
# none of here, and explaining a missing DLL to a player is a support cost for
# no benefit.  msvcrt.dll, which the rest of the C library comes from, is part
# of Windows.
set(CMAKE_C_FLAGS_INIT          "")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc")
