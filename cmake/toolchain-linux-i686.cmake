# 32-bit x86 on a 64-bit Linux host.
#
#   cmake -S . -B build/native -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-i686.cmake
#
# The port has to be ILP32 -- CMakeLists.txt explains why at length, and
# refuses to build otherwise.  On a 64-bit x86 machine that means -m32 and a
# 32-bit SDL:
#
#   Debian/Ubuntu   sudo apt install gcc-multilib libsdl2-dev:i386
#   Fedora          sudo dnf install glibc-devel.i686 libgcc.i686 SDL2-devel.i686
#   Arch            enable [multilib], then lib32-sdl2
#
# This file is also the template for the other three platforms.  A toolchain
# file is the whole of what a new target needs from the build system: set the
# compiler and its ABI flags, tell pkg-config where that architecture's .pc
# files are, and stop.  Nothing in CMakeLists.txt is per-platform except one
# `if(APPLE)` and one `if(WIN32)`.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(KATAM_ABI_FLAGS "-m32")

# A sysroot assembled by hand, for a machine where you cannot install packages.
# Point KATAM_SYSROOT32 at a directory holding the unpacked 32-bit libc, libgcc
# and SDL2 development packages and everything below follows from it.  Leave it
# unset on a normal machine.
if(NOT DEFINED KATAM_SYSROOT32 AND DEFINED ENV{KATAM_SYSROOT32})
  set(KATAM_SYSROOT32 "$ENV{KATAM_SYSROOT32}")
endif()

if(KATAM_SYSROOT32)
  # Not CMAKE_SYSROOT: that would take the *headers* from there too, and a
  # partial sysroot has only the architecture-specific ones.  The 32-bit and
  # 64-bit glibc headers on a multiarch system are the same files, chosen
  # between by __x86_64__, so the host's are correct and only bits/ and
  # gnu/stubs-32.h have to come first.
  string(APPEND KATAM_ABI_FLAGS
      " -isystem ${KATAM_SYSROOT32}/usr/include"
      " -isystem /usr/include/x86_64-linux-gnu"
      " -B${KATAM_SYSROOT32}/usr/lib32"
      " -B${KATAM_SYSROOT32}/usr/lib/gcc/x86_64-linux-gnu/13/32"
      " -L${KATAM_SYSROOT32}/usr/lib32"
      " -Wl,--sysroot=${KATAM_SYSROOT32}"
      # A hand-assembled sysroot has SDL and libc but not the twenty libraries
      # SDL is linked against, so the linker cannot resolve *their* symbols and
      # refuses.  The runtime loader can, from the host's own i386 libraries.
      # This is the one concession the escape hatch needs; a machine with
      # libsdl2-dev:i386 properly installed does not use it.
      " -Wl,--allow-shlib-undefined")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${KATAM_SYSROOT32}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "${KATAM_SYSROOT32}/usr/lib/i386-linux-gnu/pkgconfig")
else()
  set(ENV{PKG_CONFIG_LIBDIR}
      "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/share/pkgconfig")
endif()

set(CMAKE_C_FLAGS_INIT        "${KATAM_ABI_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${KATAM_ABI_FLAGS}")

# find_package must not wander into the 64-bit CMake config packages.
set(CMAKE_FIND_LIBRARY_CUSTOM_LIB_SUFFIX "32")
