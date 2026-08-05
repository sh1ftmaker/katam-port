# 64-bit ARM (aarch64, arm64), cross-compiled from anywhere.
#
#   cmake -S . -B build/native-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-arm64.cmake
#
# aarch64 is LP64: pointers are eight bytes.  The port's structures have to
# keep the four-byte pointer members the console gave them, which is what
# platform/port/p32.h exists for, and the conversion of the decompilation's
# pointer member declarations to PTR32 is done -- 324 members, plus the types
# defined inside .c files rather than headers, which took a second pass and two
# bugs to find.  Configure with -DKATAM_ALLOW_LP64=ON; CMakeLists.txt explains
# why that is still opt-in.
#
# Verified on 2026-08-05: wasm32, i686, x86-64 and this target produce
# byte-identical DMA transfer streams over 63236 transfers and 1401 frames of
# scripted input, and pixel-identical frames.  `make layout-check` asserts the
# 246 types with known console offsets and tools/abi_size_diff.py covers the
# rest by comparing DWARF sizes against an ILP32 build.
#
# armhf -- cmake/toolchain-linux-armhf.cmake -- is still the simpler ARM build
# and needs no C++ toolchain and no generated tree; it is ILP32 and runs on any
# arm64 kernel with 4 KiB pages.  docs/NATIVE.md has the detail on which
# kernels those are.
#
# The cross packages ship aarch64-linux-gnu-g++-13 with no unsuffixed alias, so
# in practice this wants both compilers named:
#
#   cmake -S . -B build/native-arm64 -DKATAM_ALLOW_LP64=ON \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-arm64.cmake \
#         -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-13
#
# Name the C compiler only if the unsuffixed aarch64-linux-gnu-gcc is missing:
# on this machine the versioned gcc-13 and the unsuffixed wrapper resolve their
# libc differently, and only the wrapper links against a hand-built sysroot.
#
#   Debian/Ubuntu   sudo apt install g++-aarch64-linux-gnu
#                   sudo dpkg --add-architecture arm64
#                   sudo apt install libsdl2-dev:arm64
#
# On x86-64 the multiarch solver will not install libsdl2-dev:arm64, the same
# way it will not install the armhf one; unpacking a sysroot by hand is the way
# through, and "Building without root" in docs/NATIVE.md has the commands.
# Note that arm64 packages come from ports.ubuntu.com rather than the main
# archive -- only the *cross* packages (gcc, libc6-dev-arm64-cross) are in it.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# The triple prefix.  Debian ships the compiler as aarch64-linux-gnu-gcc-13
# with no unsuffixed g++ in the cross package, so KATAM_ARM64_PREFIX may need
# to carry the version: -DKATAM_ARM64_PREFIX=aarch64-linux-gnu- with
# -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++-13, or set both explicitly.
if(NOT DEFINED KATAM_ARM64_PREFIX AND DEFINED ENV{KATAM_ARM64_PREFIX})
  set(KATAM_ARM64_PREFIX "$ENV{KATAM_ARM64_PREFIX}")
endif()
if(NOT KATAM_ARM64_PREFIX)
  set(KATAM_ARM64_PREFIX "aarch64-linux-gnu-")
endif()

if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${KATAM_ARM64_PREFIX}gcc")
endif()
# The C++ compiler is not optional here, unlike every other target: a 64-bit
# build compiles the game through a C++ front end so that PTR32 can be a
# 4-byte class.  See platform/port/p32.h and tools/cxxify.py.
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER "${KATAM_ARM64_PREFIX}g++")
endif()
set(CMAKE_AR      "${KATAM_ARM64_PREFIX}ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB  "${KATAM_ARM64_PREFIX}ranlib"  CACHE FILEPATH "")
set(CMAKE_STRIP   "${KATAM_ARM64_PREFIX}strip"   CACHE FILEPATH "")

# No -m flag and no -march.  An aarch64-linux-gnu compiler emits LP64 AArch64
# and there is no other ABI it can produce; the ILP32 variant (-mabi=ilp32,
# which would have made all of this unnecessary) has no libc anybody ships and
# no upstream kernel support, and clang's aarch64_32 target is Apple's watchOS
# ABI.  That was checked rather than assumed -- docs/SIXTYFOUR.md records it.
if(NOT DEFINED KATAM_ARM64_ABI_FLAGS AND DEFINED ENV{KATAM_ARM64_ABI_FLAGS})
  set(KATAM_ARM64_ABI_FLAGS "$ENV{KATAM_ARM64_ABI_FLAGS}")
endif()

# A sysroot assembled by hand, holding the unpacked arm64 libc, headers and
# SDL2.  docs/NATIVE.md has the commands.
# Set this in the *environment*, not with -D.  A toolchain file is evaluated
# before the cache is populated, so a -DKATAM_SYSROOT_ARM64=... is not visible
# here on the first configure: the else branch below runs, PKG_CONFIG_LIBDIR is
# left pointing at the host's own directories, and the build fails claiming
# there is no SDL for this architecture when there is one in the sysroot.
#
#   KATAM_SYSROOT_ARM64=$PWD/sysroot64 cmake -S . -B build/native-arm64 ...
if(NOT DEFINED KATAM_SYSROOT_ARM64 AND DEFINED ENV{KATAM_SYSROOT_ARM64})
  set(KATAM_SYSROOT_ARM64 "$ENV{KATAM_SYSROOT_ARM64}")
endif()

if(KATAM_SYSROOT_ARM64)
  set(CMAKE_SYSROOT "${KATAM_SYSROOT_ARM64}")

  # Two library directories, because the packages come from two places and
  # disagree about where a target library lives.  Debian's *cross* packages put
  # the libc in /usr/aarch64-linux-gnu/lib, which is not a multiarch path and
  # is not inside any sysroot; the arm64 packages from ports.ubuntu.com put
  # everything else in the multiarch /usr/lib/aarch64-linux-gnu.  A sysroot
  # needs both on the search path, and -B as well so the crt objects are found.
  string(APPEND KATAM_ARM64_ABI_FLAGS
      " -B${KATAM_SYSROOT_ARM64}/usr/aarch64-linux-gnu/lib"
      " -L${KATAM_SYSROOT_ARM64}/usr/aarch64-linux-gnu/lib"
      " -L${KATAM_SYSROOT_ARM64}/usr/lib/aarch64-linux-gnu")

  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${KATAM_SYSROOT_ARM64}")
  # /usr/lib/pkgconfig is on the list as well as the multiarch directory.  A
  # distribution SDL2 lands in the multiarch one; an SDL2 built from source and
  # installed with prefix=/usr lands here, and building one is the practical
  # answer on this target -- the packaged libSDL2.so pulls in twenty shared
  # libraries (libdrm, gbm, wayland, X11, pulse ...) and a sysroot has to hold
  # every one of them to link at all.  A static SDL2 configured for dummy video
  # and dummy audio needs none of them, which is all a headless run wants.
  # docs/NATIVE.md has the configure line.
  set(ENV{PKG_CONFIG_LIBDIR}
      "${KATAM_SYSROOT_ARM64}/usr/lib/aarch64-linux-gnu/pkgconfig:${KATAM_SYSROOT_ARM64}/usr/lib/pkgconfig:${KATAM_SYSROOT_ARM64}/usr/share/pkgconfig")
else()
  set(ENV{PKG_CONFIG_LIBDIR}
      "/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig")
endif()

set(CMAKE_C_FLAGS_INIT            "${KATAM_ARM64_ABI_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT          "${KATAM_ARM64_ABI_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT   "${KATAM_ARM64_ABI_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run the result through qemu where one is available, so CMake's own try_run
# checks work and the binary can be exercised on a desktop.  What a qemu run
# does and does not prove is the same caveat the armhf build carries, and
# docs/NATIVE.md states it: the instructions are real, the address space is
# qemu's.  For this target that caveat matters more than usual, because the
# whole question is where things land in memory.
if(NOT DEFINED KATAM_ARM64_QEMU AND DEFINED ENV{KATAM_ARM64_QEMU})
  set(KATAM_ARM64_QEMU "$ENV{KATAM_ARM64_QEMU}")
endif()
if(KATAM_ARM64_QEMU)
  if(KATAM_SYSROOT_ARM64)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${KATAM_ARM64_QEMU}" "-L" "${KATAM_SYSROOT_ARM64}")
  else()
    set(CMAKE_CROSSCOMPILING_EMULATOR "${KATAM_ARM64_QEMU}")
  endif()
endif()
