# 32-bit ARM (armhf, arm-linux-gnueabihf), cross-compiled from anywhere.
#
#   cmake -S . -B build/native-armhf \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-armhf.cmake
#
# You do not need this file to build *on* a 32-bit ARM machine.  armhf is
# already ILP32, so a Raspberry Pi running a 32-bit OS builds with plain
# `make native` and no toolchain file at all -- which is what the Makefile does
# when `uname -m` is armv7l or armv6l.  This file is for cross-compiling from a
# desktop, because compiling four thousand files of decompilation on a Pi takes
# the better part of an hour.
#
# armhf is the ARM target, not aarch64, and that is not a preference.  The port
# has to be ILP32 -- CMakeLists.txt explains why at length and refuses to
# configure otherwise -- and aarch64 has no ILP32 userland anybody ships.  A
# 64-bit ARM machine runs this build through its kernel's 32-bit support and
# the armhf multiarch libraries; docs/NATIVE.md says exactly what that needs.
#
#   Debian/Ubuntu   sudo apt install gcc-arm-linux-gnueabihf
#                   sudo dpkg --add-architecture armhf
#                   sudo apt install libsdl2-dev:armhf
#   Fedora          sudo dnf install gcc-arm-linux-gnu   (and an armhf sysroot)
#
# On Debian and Ubuntu, `libsdl2-dev:armhf` is only installable on an armhf or
# arm64 machine; on x86-64 the multiarch dependency solver will not do it, and
# the sysroot below is the way through.  See "Building without root" in
# docs/NATIVE.md, which is also how this file was tested.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# The triple prefix.  Override with -DKATAM_ARM_PREFIX= if your toolchain calls
# itself something else (armv7l-linux-gnueabihf-, arm-none-linux-gnueabihf-).
if(NOT DEFINED KATAM_ARM_PREFIX AND DEFINED ENV{KATAM_ARM_PREFIX})
  set(KATAM_ARM_PREFIX "$ENV{KATAM_ARM_PREFIX}")
endif()
if(NOT KATAM_ARM_PREFIX)
  set(KATAM_ARM_PREFIX "arm-linux-gnueabihf-")
endif()

if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER "${KATAM_ARM_PREFIX}gcc")
endif()
set(CMAKE_AR      "${KATAM_ARM_PREFIX}ar"      CACHE FILEPATH "")
set(CMAKE_RANLIB  "${KATAM_ARM_PREFIX}ranlib"  CACHE FILEPATH "")
set(CMAKE_STRIP   "${KATAM_ARM_PREFIX}strip"   CACHE FILEPATH "")

# No -m flag here, unlike the i686 file: an arm-linux-gnueabihf compiler is
# ILP32 and hard-float by construction, and there is no other ABI it can emit.
# Whatever -march the toolchain defaults to is left alone as well -- Debian and
# Ubuntu armhf are built for ARMv7-A with VFPv3-D16, Raspberry Pi OS 32-bit for
# ARMv6 with VFP2, and picking one here would silently exclude the other.
# KATAM_ARM_ABI_FLAGS is how to say which you meant:
#
#   -DKATAM_ARM_ABI_FLAGS="-march=armv6+fp -mfpu=vfp -marm"   Pi 1, Pi Zero
#
# Nothing in the port needs NEON and none of it is vectorised by hand; the
# software mixer in platform/m4a_mixer.c is scalar float and the PPU is integer.
if(NOT DEFINED KATAM_ARM_ABI_FLAGS AND DEFINED ENV{KATAM_ARM_ABI_FLAGS})
  set(KATAM_ARM_ABI_FLAGS "$ENV{KATAM_ARM_ABI_FLAGS}")
endif()

# A sysroot assembled by hand, for a machine where you cannot install packages
# -- or, on x86-64, where apt will not install libsdl2-dev:armhf however much
# root you have.  Point KATAM_SYSROOT_ARM at a directory holding the unpacked
# armhf libc, headers and SDL2 packages; docs/NATIVE.md has the commands.
if(NOT DEFINED KATAM_SYSROOT_ARM AND DEFINED ENV{KATAM_SYSROOT_ARM})
  set(KATAM_SYSROOT_ARM "$ENV{KATAM_SYSROOT_ARM}")
endif()

if(KATAM_SYSROOT_ARM)
  # CMAKE_SYSROOT, unlike the i686 file, which deliberately does not use it.
  # There the 32-bit sysroot holds only bits/ and gnu/stubs-32.h and the rest of
  # the headers have to come from the host, because glibc's x86 headers are one
  # set chosen between by __x86_64__.  An armhf sysroot is a different
  # architecture's whole userland -- libc6-dev, linux-libc-dev and SDL's headers
  # are all in it -- so there is nothing to borrow from the host and everything
  # to get wrong by borrowing.
  set(CMAKE_SYSROOT "${KATAM_SYSROOT_ARM}")

  # Debian's cross packages install the target libc somewhere a sysroot does
  # not: /usr/arm-linux-gnueabihf/lib, outside CMAKE_SYSROOT and searched
  # first.  Its libc.so is a linker script naming that absolute path, so the
  # link fails with "cannot find /usr/arm-linux-gnueabihf/lib/libc.so.6" while
  # a perfectly good libc.so.6 sits in the sysroot.  An explicit -L and -B put
  # the sysroot's multiarch directory ahead of it, for libraries and for the
  # crt objects respectively.
  string(APPEND KATAM_ARM_ABI_FLAGS
      " -B${KATAM_SYSROOT_ARM}/usr/lib/arm-linux-gnueabihf"
      " -L${KATAM_SYSROOT_ARM}/usr/lib/arm-linux-gnueabihf")

  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${KATAM_SYSROOT_ARM}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "${KATAM_SYSROOT_ARM}/usr/lib/arm-linux-gnueabihf/pkgconfig:${KATAM_SYSROOT_ARM}/usr/share/pkgconfig")
else()
  set(ENV{PKG_CONFIG_LIBDIR}
      "/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig")
endif()

set(CMAKE_C_FLAGS_INIT          "${KATAM_ARM_ABI_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${KATAM_ARM_ABI_FLAGS}")

# find_package and find_library must not wander into the host's own libraries;
# headers and libraries come from the sysroot, programs from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run the built binary through qemu where one is available, so that CMake's own
# try_run checks work and so `make native-test ARM=1` can drive the real thing
# on a desktop.  binfmt_misc does the same job transparently when it is
# registered; this covers the machines where it is not.  A binary run this way
# is genuine ARM code, but the address space it sees is qemu's rather than the
# kernel's -- docs/NATIVE.md says what that does and does not prove.
if(NOT DEFINED KATAM_ARM_QEMU AND DEFINED ENV{KATAM_ARM_QEMU})
  set(KATAM_ARM_QEMU "$ENV{KATAM_ARM_QEMU}")
endif()
if(KATAM_ARM_QEMU)
  if(KATAM_SYSROOT_ARM)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${KATAM_ARM_QEMU}" "-L" "${KATAM_SYSROOT_ARM}")
  else()
    set(CMAKE_CROSSCOMPILING_EMULATOR "${KATAM_ARM_QEMU}")
  endif()
endif()
