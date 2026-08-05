# katam-port -- WebAssembly port of the Kirby & The Amazing Mirror decompilation
#
#   make            build web/katam.html + katam.js + katam.wasm
#   make sync       re-copy and re-codemod the decomp sources (do this after
#                   pulling new decompilation work)
#   make layout     re-measure the committed GBA struct layout table, after the
#                   decompilation has changed a structure on purpose
#   make serve      build and serve on http://localhost:8000
#   make clean
#
# Point KATAM_DECOMP at your checkout of the decompilation if it is not the
# default.  Nothing from the ROM is read at build time -- the player supplies
# their own ROM to the page at run time.

KATAM_DECOMP ?= $(HOME)/Desktop/katam
ROM          ?= $(KATAM_DECOMP)/baserom.gba
EMSDK        ?= $(HOME)/emsdk

BUILD     := build
PORT_SRC  := $(BUILD)/port-src
GENERATED := $(BUILD)/generated
OUT       := web

CC := $(EMSDK)/upstream/emscripten/emcc

# ---------------------------------------------------------------------------
# Memory layout.  This is the decision the whole port rests on: the real GBA
# memory map is reserved inside the wasm linear memory, so EWRAM is genuinely
# at 0x02000000, VRAM at 0x06000000 and the ROM at 0x08000000.  Placing the
# compiler's own static data, stack and heap above 0x0A000000 keeps them out of
# it.  See docs/ARCHITECTURE.md.
# ---------------------------------------------------------------------------
# The reserved GBA map runs from EWRAM at 0x02000000 to the end of save memory,
# which portify.py relocates to sit just above the 16 MiB ROM (0x09010000)
# rather than at the hardware's 0x0E000000.  The compiler's own data starts
# above that.  This is one contiguous wasm allocation made at page load, so the
# 80 MiB hole the hardware layout would leave is 80 MiB a phone has to find.
# emcc parses these as byte sizes and rejects 0x-prefixed hex, so decimal:
#   167772160 = 0x0A000000,  201326592 = 0x0C000000  (192 MiB total)
GLOBAL_BASE    := 167772160
INITIAL_MEMORY := 201326592

# ---------------------------------------------------------------------------
# PORTABLE     -- the decomp's own guard (include/portable.h) that compiles the
#                 register pins and codegen barriers away.  Added upstream.
# NONMATCHING  -- selects the readable C branch wherever a function has one,
#                 instead of the byte-matching branch.
# ---------------------------------------------------------------------------
DEFINES := -DPORTABLE -DNONMATCHING -DMODERN=1
# Where the compiler's own data starts, so the DMA range check can tell the
# port's C variables from a stale GBA pointer.  DmaFill sources its value from
# a local, which lives up here rather than anywhere on the GBA map.
DEFINES += -DPORT_GLOBAL_BASE=$(GLOBAL_BASE)u

# CHECK_POINTERS=1 range-checks every TaskGetStructPtr result (see
# platform/checks.c).  Object files do not record the flag, so switch with a
# clean build: rm -rf build/obj.
CHECK_POINTERS ?= 0
ifeq ($(CHECK_POINTERS),1)
DEFINES += -DPORT_CHECK_POINTERS
endif

# DEBUG_INFO=1 compiles the game itself with DWARF, so a trap's code offset
# resolves to file:line:column instead of a bare function name.  This has to be
# set at *compile* time: linking -g over -g0 objects produces a module with
# debug info for emscripten's own libraries and none for the game, which looked
# like a working debug build and was not.  The objects do not record the flag,
# so they get their own directory rather than silently mixing.
DEBUG_INFO ?= 0
ifeq ($(DEBUG_INFO),1)
OBJDIR   := $(BUILD)/obj-g
DBG_CFLAG := -g
else
OBJDIR   := $(BUILD)/obj
DBG_CFLAG := -g0
endif

INCLUDES := -I$(PORT_SRC)/include -I$(GENERATED) -Iplatform

# The generated headers replace declarations that used to live in the game's
# own headers, so they have to be in scope before anything else is parsed.
FORCE_INCLUDE := -include port/prelude.h -include port/ram_symbols.h -include port/rom_data.h

# -Wno-everything is right for the decompilation itself: it is machine-shaped C
# that warns about everything and means none of it.  The one warning that has
# to survive is an implicit declaration.  On ARM a call to an undeclared
# function is a call; in WebAssembly the implicit `int f()` gives it a
# different *type* from the definition, wasm-ld resolves the disagreement by
# pointing the call at a stub whose whole body is `unreachable`, and the
# program traps the first time it runs that line.  The port shipped exactly
# that once (a PortTrace call with no prototype in warp_star.c, which killed
# the game the moment a warp star ticked), so it is an error here, not a
# warning.  It has to come after -Wno-everything to survive it.
WARN := -Wno-everything -Werror=implicit-function-declaration

# -fgnu89-inline matters more than it looks.  The decomp is compiled by agbcc
# (gcc 2.95), where a plain `inline` definition also emits an external one.
# C99 inverted that rule, so without this flag every `inline void Foo(...)` in
# the game becomes an undefined symbol at link time.
# -funsigned-char says out loud what the GBA's compiler did silently: plain
# `char` is unsigned there.  Measured, not assumed -- agbcc compiles
# `char c = -1; return c < 0;` to `mov r0, #0`.  clang's wasm32 and gcc's x86
# default to signed and gcc's ARM to unsigned, so a build that does not say
# which it wants is a different program on each host, with nothing to report it.
# Both smoke tests are byte-identical with and without the flag; it is here so
# that stays true on hosts nobody has built on yet.
CFLAGS := -O2 $(DBG_CFLAG) -std=gnu99 -fgnu89-inline -fno-strict-aliasing -fwrapv \
          -funsigned-char \
          $(WARN) $(DEFINES) $(INCLUDES) $(FORCE_INCLUDE) -MMD -MP

# --profiling-funcs keeps the wasm name section, so a trap on someone's phone
# reports `sub_0805405C` instead of `wasm-function[731]`.  It costs a little
# size and nothing in speed, which is a good trade while the port still
# crashes.
#
# --fatal-warnings is the second half of -Werror=implicit-function-declaration
# above.  wasm-ld has exactly one warning that matters and it is not survivable:
# a call whose type disagrees with the definition cannot be emitted, so it emits
# a stub whose body is `unreachable` and links anyway.  That shipped once.  The
# compile flag catches the implicit-declaration route in; this catches a wrong
# prototype written out in full, which the compiler cannot see across files.
LDLINT := -Wl,--fatal-warnings
LDFLAGS := -O2 --profiling-funcs $(LDLINT) \
    -sASYNCIFY \
    -sASYNCIFY_STACK_SIZE=32768 \
    -sGLOBAL_BASE=$(GLOBAL_BASE) \
    -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
    -sALLOW_MEMORY_GROWTH=0 \
    -sSTACK_SIZE=1048576 \
    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask,_PortSetWatch,_PortAudioTestTone,_PortMpUseLoopback,_PortMpUseJs,_PortMpDetach,_PortMpLoopbackSelfId,_PortMpSelfTest,_PortMpReport,_PortSetStateTrace,_PortSetStateDetailFrame,_PortSetStateDump,_PortSetDmaTrace,_PortSetDmaStack,_PortSetStateWindow,_PortSetRenderEnabled,_PortRbSelfTest,_PortRbInit,_PortRbShutdown,_PortRbReport \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,ccall,cwrap \
    -sENVIRONMENT=web \
    --shell-file web/shell.html

GAME_SRCS     := $(shell find $(PORT_SRC)/src -name '*.c' 2>/dev/null)
# platform/*.c is the console -- PPU, DMA, BIOS, the mixer -- and knows nothing
# about the host.  platform/web/*.c is the browser half of port/backend.h; the
# native build swaps in platform/native/*.c instead.  See docs/NATIVE.md.
PLATFORM_SRCS := $(wildcard platform/*.c) $(wildcard platform/web/*.c)
GEN_SRCS      := $(wildcard $(GENERATED)/*.c)
SRCS          := $(GAME_SRCS) $(PLATFORM_SRCS) $(GEN_SRCS)
OBJS          := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

TARGET := $(OUT)/katam.html

.PHONY: all sync clean serve compile stubs test debug prune dist deploy check-dist release pages abi-size-check ptr-array-check size-check relayout relayout-check shell-check shell-tap-test \
        native native-run native-test native-clean \
        arm64 arm64-clean \
        windows windows-package windows-clean \
        layout layout-check

all: $(TARGET)

# --- source sync ----------------------------------------------------------
# portify.py copies the decomp and rewrites what will not compile off ARM.
# The two generators then turn the linker-script RAM symbols and the ROM data
# labels into address macros, editing the copied headers as they go.
sync:
	@echo "  SYNC    $(KATAM_DECOMP)"
	python3 tools/portify.py --decomp $(KATAM_DECOMP) --out $(PORT_SRC)
	python3 tools/gen_ram_symbols.py \
	    --linker-script $(KATAM_DECOMP)/linker.ld \
	    --tree $(PORT_SRC) --out $(GENERATED)/port/ram_symbols.h \
	    --map $(KATAM_DECOMP)/katam.map
	python3 tools/gen_rom_data.py \
	    --data-dir $(KATAM_DECOMP)/data \
	    --tree $(PORT_SRC) --out $(GENERATED)/port/rom_data.h \
	    --out-copies $(GENERATED)/rom_copies.c \
	    --out-tables $(GENERATED)/rom_fn_tables.c \
	    --map $(KATAM_DECOMP)/katam.map --elf $(KATAM_DECOMP)/katam.elf \
	    --rom $(ROM)

# --- the GBA struct layout table ------------------------------------------
# platform/port/gba_layout.h is committed: the size and every member offset of
# every structure the decompilation defines, measured from the ILP32 build and
# turned into _Static_asserts that every build compiles
# (platform/gba_layout_check.c).  A host whose ABI moves a member fails to
# compile and says which one, instead of booting and being quietly wrong.
#
# `make layout` re-measures and rewrites the table.  Run it when the
# decompilation changes a structure on purpose, read the diff, and commit it --
# the numbers are the layout the ROM and linker.ld are written against, so a
# change here is a change to the memory the game reads.
#
# It has to be measured from a 32-bit compiler; there is no useful default on
# an x86-64 machine, so LAYOUT_CC/LAYOUT_ABI are what to override.  The 32-bit
# libc headers come from KATAM_SYSROOT32 if the machine has no gcc-multilib
# (see "Building without root" in docs/NATIVE.md).
LAYOUT_CC  ?= $(CC32)
CC32       ?= gcc
LAYOUT_ABI ?= -m32
LAYOUT_HDR := platform/port/gba_layout.h
ifneq ($(KATAM_SYSROOT32),)
LAYOUT_SYSROOT := -isystem $(KATAM_SYSROOT32)/usr/include \
                  -isystem /usr/include/x86_64-linux-gnu
endif
LAYOUT_CFLAGS := $(LAYOUT_ABI) -std=gnu99 -fgnu89-inline -fno-strict-aliasing \
                 -fwrapv -funsigned-char -w $(DEFINES) $(INCLUDES) \
                 $(FORCE_INCLUDE) $(LAYOUT_SYSROOT)

layout:
	@python3 tools/gen_gba_layout.py --tree $(PORT_SRC) --out $(LAYOUT_HDR) \
	    --cc $(LAYOUT_CC) --cflags "$(LAYOUT_CFLAGS)"

# Fails if the committed table no longer describes the tree.  The build already
# fails on a *changed* number, because the assertions are compiled; this catches
# the other direction -- a structure that stopped being declared anywhere and
# quietly dropped out of the table, taking its assertions with it.
layout-check:
	@python3 tools/gen_gba_layout.py --tree $(PORT_SRC) --out $(LAYOUT_HDR) \
	    --cc $(LAYOUT_CC) --cflags "$(LAYOUT_CFLAGS)" --check

# --- the two things layout-check cannot see --------------------------------
#
# layout-check asserts the 246 types whose console offsets are known.  Two
# kinds of ABI breakage are invisible to it, and each cost a day to find the
# first time:
#
#   abi-size-check   a type the decompilation never places at a fixed address
#                    is not in the table, so nothing notices it changing size
#                    under LP64 -- and every sizeof() the game hands to
#                    TaskCreate then differs between the two builds.  Needs an
#                    ILP32 and an LP64 build tree, both with -g.
#
#   ptr-array-check  a file-scope array of pointers is four-byte-strided on the
#                    console and eight-byte-strided under LP64, which is
#                    harmless until something reads it through another type.
#                    One place in the tree does.
#
# Both are cheap and neither needs a ROM.  docs/SIXTYFOUR.md has the two bugs.
ABI32_DIR ?= $(BUILD)/native/CMakeFiles/katam.dir
ABI64_DIR ?= $(BUILD)/lp64/CMakeFiles/katam.dir

abi-size-check:
	@python3 tools/abi_size_diff.py $(ABI32_DIR) $(ABI64_DIR)

ptr-array-check:
	@python3 tools/check_ptr_arrays.py $(PORT_SRC)

# --- the third thing layout-check cannot see -------------------------------
#
# layout-check asserts that the tree has not *drifted*.  It is generated from
# the tree, so a structure that was never right in the first place gets its
# wrong size asserted and defended.  That is what happened to the warp star:
# struct Unk_08353510 is documented 0xC on its closing brace and compiled to
# 10, because the GBA's compiler rounds a structure size up to a multiple of 4
# and clang does not, and the layout table asserted the 10.
#
# size-check is the other direction -- the tree against what the decompilation
# says the console does.  Needs no ROM and no 32-bit toolchain.
size-check:
	@python3 tools/check_doc_sizes.py --tree $(PORT_SRC)

# Repairs the *values* in the committed layout table without needing the 32-bit
# hosted toolchain gen_gba_layout.py requires (gcc-multilib).  It cannot add or
# remove a type -- only gen_gba_layout can -- so `make layout` is the right tool
# wherever it runs; this is the fallback for a machine that cannot install
# multilib, and it measures with emcc instead.  The two were checked against
# each other once multilib was available here: gcc's i386 and clang's wasm32
# agreed on all 2144 offsets, including the six structure sizes this found.
relayout:
	@python3 tools/refresh_layout_asserts.py

relayout-check:
	@python3 tools/refresh_layout_asserts.py --check

# --- the page itself -------------------------------------------------------
# web/shell.html is hand-edited and has no build step of its own, so nothing
# else would notice a syntax error in it, and the parts that only misbehave on
# a phone are exactly the parts nobody exercises on a desktop.  Needs no ROM,
# no browser and no wasm -- see the header of tools/shell_test.js.
shell-check:
	@node tools/shell_test.js web/shell.html

# The same page, in a real browser, tapped with a real finger.  shell-check
# reasons about the source; this one answers the only question that settles a
# touch bug -- does a tap on this control produce anything.  It builds the
# page, serves it, and drives headless Chrome over the DevTools protocol.
# Chrome is not Safari, but the rule at the bottom of every one of these bugs
# (preventDefault on touchstart suppresses the synthesised click) is specified
# behaviour that Chrome implements, so the class reproduces.  Needs no ROM.
shell-tap-test: $(TARGET)
	@node tools/shell_tap_test.js

# --- the 4-byte pointer member ---------------------------------------------
# platform/port/p32.h is what lets a 64-bit build keep the GBA's structure
# layout, so it is checked on both sides of the thing it spans: as C++ on this
# host, where a pointer is 8 bytes and P32 has real work to do, and as C
# through the wasm compiler, where PTR32 is a plain pointer and the point is
# that the spelling changed nothing.  See docs/SIXTYFOUR.md.
#
# P32_CXX overrides the C++ compiler, which is how the aarch64 toolchain runs
# the same test on real 64-bit ARM -- see docs/NATIVE.md.
P32_CXX ?= g++
P32_TEST_BIN := $(BUILD)/p32_test

p32-test:
	@mkdir -p $(BUILD)
	@$(P32_CXX) -x c++ -std=gnu++17 -w $(INCLUDES) tools/p32_test.c -o $(P32_TEST_BIN)
	@$(P32_TEST_BIN)
	@$(CC) -x c -std=gnu99 -w -fsyntax-only $(INCLUDES) tools/p32_test.c \
	  && echo "p32_test (C, ILP32): layout assertions hold"

# The functions that are still ARM-only turn up as undefined symbols at link
# time.  The list is re-derived from a real link rather than maintained by
# hand, because the decompilation keeps shrinking it.
stubs:
	@KATAM_DECOMP=$(KATAM_DECOMP) EMSDK=$(EMSDK) BUILD=$(BUILD) \
	 PORT_SRC=$(PORT_SRC) GENERATED=$(GENERATED) bash tools/refresh_stubs.sh

# A source that disappears (replaced by the platform layer, or renamed
# upstream) leaves its object behind, and a stale object is still a definition.
prune:
	@find $(OBJDIR) -name '*.o' 2>/dev/null | while read o; do \
	    src=$${o#$(OBJDIR)/}; src=$${src%.o}.c; \
	    [ -f "$$src" ] || { echo "  PRUNE   $$o"; rm -f "$$o" "$${o%.o}.d"; }; \
	done

$(PORT_SRC)/src: ; @$(MAKE) sync

# --- compile --------------------------------------------------------------
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

compile: $(OBJS)

# Rebuild when a header changes -- the force-included port headers are
# regenerated on every sync, and a stale object silently reverts a fix.
-include $(OBJS:.o=.d)

$(TARGET): $(OBJS) web/shell.html
	@mkdir -p $(OUT)
	@echo "  LINK    $@"
	@$(CC) $(LDFLAGS) $(OBJS) -o $@

# --- headless smoke test --------------------------------------------------
# Boots the port under node with a real ROM and runs frames, so "does it get
# past GameInit" can be answered without a browser.  Needs a ROM you own; the
# path is yours to supply and nothing here is committed.
ROM ?= $(KATAM_DECOMP)/baserom.gba

# --profiling-funcs keeps the name section, which costs a little size and
# nothing in speed.  Without it a trap's stack is `wasm-function[1911]` and
# resolve_fnptr.py has nothing to resolve it to -- the harness's whole reason
# for existing is to name the thing that broke.
$(BUILD)/katam-node.js: $(OBJS)
	$(CC) -O2 --profiling-funcs $(LDLINT) -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask,_PortSetWatch,_PortAudioTestTone,_PortMpUseLoopback,_PortMpUseJs,_PortMpDetach,_PortMpLoopbackSelfId,_PortMpSelfTest,_PortMpReport,_PortSetStateTrace,_PortSetStateDetailFrame,_PortSetStateDump,_PortSetDmaTrace,_PortSetDmaStack,_PortSetStateWindow,_PortSetRenderEnabled,_PortRbSelfTest,_PortRbInit,_PortRbShutdown,_PortRbReport \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
	    -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createKatam -sINVOKE_RUN=1 \
	    $(OBJS) -o $@

# Same as the node build but with full DWARF, for turning a trap into a source
# line.  Only meaningful with DEBUG_INFO=1 -- use `make debug`, which sets it;
# building this rule against -g0 objects yields debug info for emscripten's
# libraries and none for the game.
#
# -gseparate-dwarf keeps the debug section out of the .wasm and in a side file,
# which llvm-symbolizer reads directly.  -O1 rather than -O0: -O0 is slow
# enough to change what the game does, and line info survives -O1 well.
$(BUILD)/katam-dbg.js: $(OBJS)
	$(CC) -O1 -g --profiling-funcs $(LDLINT) \
	    -gseparate-dwarf=$(BUILD)/katam-dbg.debug.wasm \
	    -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask,_PortSetWatch,_PortAudioTestTone,_PortMpUseLoopback,_PortMpUseJs,_PortMpDetach,_PortMpLoopbackSelfId,_PortMpSelfTest,_PortMpReport,_PortSetStateTrace,_PortSetStateDetailFrame,_PortSetStateDump,_PortSetDmaTrace,_PortSetDmaStack,_PortSetStateWindow,_PortSetRenderEnabled,_PortRbSelfTest,_PortRbInit,_PortRbShutdown,_PortRbReport \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
	    -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createKatam \
	    -sINVOKE_RUN=1 $(OBJS) -o $@
# Deliberately no -sASSERTIONS here.  Its handleException runs the stack-cookie
# check first, aborts inside it, and replaces the real trap and its wasm stack
# with "the application has corrupted its heap memory area" -- which names
# nothing.  The bare trap carries the frames the DWARF is for.

# Every load and store bounds-checked.  Slow, but it is the only build that
# says *which address* was touched -- plain wasm reports "out of bounds" with
# no address at all, and -O0 -sASSERTIONS only reports that something near zero
# was clobbered.  Keep the names, or the report has no function to blame.
$(BUILD)/katam-safe.js: $(OBJS)
	$(CC) -O1 --profiling-funcs $(LDLINT) -sSAFE_HEAP=1 -sASSERTIONS=1 \
	    -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask,_PortSetWatch,_PortAudioTestTone,_PortMpUseLoopback,_PortMpUseJs,_PortMpDetach,_PortMpLoopbackSelfId,_PortMpSelfTest,_PortMpReport,_PortSetStateTrace,_PortSetStateDetailFrame,_PortSetStateDump,_PortSetDmaTrace,_PortSetDmaStack,_PortSetStateWindow,_PortSetRenderEnabled,_PortRbSelfTest,_PortRbInit,_PortRbShutdown,_PortRbReport \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
	    -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createKatam \
	    -sINVOKE_RUN=1 $(OBJS) -o $@

safe: $(BUILD)/katam-safe.js
	@test -f $(ROM) || { echo "no ROM at $(ROM) -- set ROM=/path/to/your.gba"; exit 1; }
	node tools/headless_test.js $(BUILD)/katam-safe.js $(ROM) $(FRAMES)

# Recurses with DEBUG_INFO=1 so the game is *compiled* with DWARF, into its own
# object directory.  Asking for `make debug` and getting a module built from
# release objects is the failure this exists to prevent.
debug:
	@$(MAKE) --no-print-directory DEBUG_INFO=1 $(BUILD)/katam-dbg.js
	@test -f $(ROM) || { echo "no ROM at $(ROM) -- set ROM=/path/to/your.gba"; exit 1; }
	node tools/headless_test.js $(BUILD)/katam-dbg.js $(ROM) $(FRAMES)

test: $(BUILD)/katam-node.js
	@test -f $(ROM) || { echo "no ROM at $(ROM) -- set ROM=/path/to/your.gba"; exit 1; }
	node tools/headless_test.js $(BUILD)/katam-node.js $(ROM) $(FRAMES)

FRAMES ?= 180

# --- native desktop build --------------------------------------------------
# CMakeLists.txt is the real build; these are the two lines you would type.
# It shares the source list and the compile flags with the web build and
# nothing else -- in particular its objects live in build/native, well away
# from build/obj, because mixing objects across toolchains is exactly the
# failure DEBUG_INFO above warns about.  See docs/NATIVE.md.
#
# The build is 32-bit.  That is not a size choice: the decompilation's
# structures have to keep their GBA layout, and 111 of them contain a pointer.
# CMakeLists.txt refuses to build any other way and says why.  A genuinely
# 32-bit host -- i686, armv6l, armv7l -- needs no toolchain file at all, which
# is why a Raspberry Pi running a 32-bit OS just builds.  A 64-bit host needs
# one, and which one depends on whether its 32-bit mode is a compiler flag
# (x86-64: -m32) or a different architecture (aarch64: armhf, run under the
# kernel's 32-bit support).
#
# To cross-compile armhf from a desktop, which is much faster than compiling on
# the Pi:
#
#   make native NATIVE_TOOLCHAIN=cmake/toolchain-linux-armhf.cmake \
#               NATIVE_DIR=build/native-armhf
NATIVE_DIR ?= $(BUILD)/native
NATIVE_BIN := $(NATIVE_DIR)/katam
NATIVE_ARCH := $(shell uname -m)
ifeq ($(NATIVE_ARCH),x86_64)
NATIVE_TOOLCHAIN ?= cmake/toolchain-linux-i686.cmake
endif
ifeq ($(NATIVE_ARCH),aarch64)
NATIVE_TOOLCHAIN ?= cmake/toolchain-linux-armhf.cmake
endif
ifneq ($(NATIVE_TOOLCHAIN),)
NATIVE_CMAKE_ARGS := -DCMAKE_TOOLCHAIN_FILE=$(NATIVE_TOOLCHAIN)
endif

# The hint on failure is not decoration.  Without the 32-bit toolchain, cmake
# fails at its own "can the compiler compile a trivial program" check, and the
# message it prints is about a scratch directory rather than about the one
# package that is missing.
native:
	@cmake -S . -B $(NATIVE_DIR) -DCMAKE_BUILD_TYPE=Release $(NATIVE_CMAKE_ARGS) >/dev/null \
	  || { echo; \
	       echo "The native build is 32-bit -- see docs/NATIVE.md for why it has to be."; \
	       echo "On Debian/Ubuntu that needs:"; \
	       if [ "$(NATIVE_ARCH)" = aarch64 ]; then \
	         echo "    sudo apt install gcc-arm-linux-gnueabihf"; \
	         echo "    sudo dpkg --add-architecture armhf && sudo apt update"; \
	         echo "    sudo apt install libsdl2-dev:armhf"; \
	         echo "and a kernel with 4 KiB pages -- a Raspberry Pi 5 boots a 16 KiB-page"; \
	         echo "kernel by default and cannot run a 32-bit binary at all until you put"; \
	         echo "kernel=kernel8.img in config.txt.  docs/NATIVE.md has the detail."; \
	       else \
	         echo "    sudo apt install gcc-multilib libsdl2-dev:i386"; \
	         echo "Fedora: glibc-devel.i686 libgcc.i686 SDL2-devel.i686"; \
	       fi; \
	       echo "Without root, see \"Building without root\" in docs/NATIVE.md."; \
	       exit 1; }
	@cmake --build $(NATIVE_DIR) -j $(shell nproc 2>/dev/null || echo 4)
	@echo "  NATIVE  $(NATIVE_BIN)"

native-run: native
	@test -f $(ROM) || { echo "no ROM at $(ROM) -- set ROM=/path/to/your.gba"; exit 1; }
	@$(NATIVE_BIN) $(ROM)

# The native equivalent of `make test`: boot with a real ROM, run frames with
# no window and no audio device, and write the last one out.  Proves the binary
# reaches gameplay without anybody watching it.
native-test: native
	@test -f $(ROM) || { echo "no ROM at $(ROM) -- set ROM=/path/to/your.gba"; exit 1; }
	@bash tools/native_smoke.sh $(NATIVE_BIN) $(ROM)

native-clean:
	rm -rf $(NATIVE_DIR)

# --- arm64 (aarch64) -- the 64-bit target, IN PROGRESS ----------------------
# This does not play a level yet.  It compiles, links, boots, reserves the GBA map
# at its true addresses, loads the ROM, starts the sound engine and reaches the
# game's task scheduler, where it crashes.  The finished, playable ARM build is
# armhf, which is ILP32 and runs on any arm64 kernel with 4 KiB pages:
#
#   make native NATIVE_TOOLCHAIN=cmake/toolchain-linux-armhf.cmake \
#               NATIVE_DIR=build/native-armhf
#
# -DKATAM_ALLOW_LP64=ON is what gets past CMakeLists.txt's pointer-size guard.
# It is not a fix and the configure says so at length every time; the guard is
# there because an LP64 build of the port has no run-time symptom that would
# point back at the ABI.  docs/SIXTYFOUR.md is the whole of this project and
# says how far it has got.
#
# KATAM_SYSROOT_ARM64, KATAM_ARM64_PREFIX and KATAM_ARM64_QEMU are read from the
# *environment* by the toolchain file, not from -D: a toolchain file runs before
# the cache exists, so a -D is invisible to it on the first configure and the
# build fails claiming there is no arm64 SDL while one sits in the sysroot.
#
#   KATAM_SYSROOT_ARM64=$PWD/root make arm64
#
# "Building without root" in docs/NATIVE.md assembles that sysroot from scratch.
ARM64_DIR ?= $(BUILD)/native-arm64
ARM64_BIN := $(ARM64_DIR)/katam

arm64:
	@echo "  arm64 is the LP64 build: the game compiled as C++ so that PTR32 keeps"
	@echo "  every structure at its GBA layout.  Verified transfer-for-transfer and"
	@echo "  pixel-for-pixel against wasm32, i686 and x86-64 -- see docs/SIXTYFOUR.md."
	@echo "  armhf needs none of that machinery and is the simpler ARM build."
	@cmake -S . -B $(ARM64_DIR) -DCMAKE_BUILD_TYPE=Release \
	       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux-arm64.cmake \
	       -DKATAM_ALLOW_LP64=ON >/dev/null \
	  || { echo; \
	       echo "The arm64 build needs an aarch64 cross toolchain and an arm64 SDL2."; \
	       echo "On Debian/Ubuntu, on an arm64 machine:"; \
	       echo "    sudo apt install g++-aarch64-linux-gnu libsdl2-dev"; \
	       echo "From an x86-64 desktop there is no apt route at all: the arm64"; \
	       echo "packages live on ports.ubuntu.com, not in the archive, and only the"; \
	       echo "cross compilers are in it.  Unpack a sysroot by hand and name it in"; \
	       echo "the ENVIRONMENT -- a -D is not visible to a toolchain file:"; \
	       echo "    KATAM_SYSROOT_ARM64=\$$PWD/root make arm64"; \
	       echo "See \"Building without root\" in docs/NATIVE.md for the whole recipe."; \
	       exit 1; }
	@cmake --build $(ARM64_DIR) -j $(shell nproc 2>/dev/null || echo 4)
	@echo "  ARM64   $(ARM64_BIN)"

arm64-clean:
	rm -rf $(ARM64_DIR)

# --- Windows ---------------------------------------------------------------
# The same sources through cmake/toolchain-windows-i686.cmake, cross-compiled
# with MinGW-w64 or built in MSYS2's mingw32 shell.  x86, not x64, for the same
# reason the Linux build is -m32.
#
# KATAM_SDL2_MINGW points at the i686-w64-mingw32 subdirectory of SDL's own
# MinGW development tarball from libsdl.org.  There is no system SDL on Windows
# to find and no pkg-config to find it with, so this is not optional and there
# is no sensible default to guess.  See docs/NATIVE.md.
#
#   make windows KATAM_SDL2_MINGW=~/SDL2-2.30.11/i686-w64-mingw32
#   make windows-package KATAM_SDL2_MINGW=...
WIN_DIR ?= $(BUILD)/win32
WIN_BIN := $(WIN_DIR)/katam.exe
WIN_PKG := $(WIN_DIR)/katam-port-windows-i686
KATAM_SDL2_MINGW ?=

windows:
	@test -n "$(KATAM_SDL2_MINGW)" || { \
	    echo "Set KATAM_SDL2_MINGW to the i686-w64-mingw32 directory of SDL's"; \
	    echo "MinGW development tarball:"; \
	    echo "    curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-devel-2.30.11-mingw.tar.gz"; \
	    echo "    tar xf SDL2-devel-2.30.11-mingw.tar.gz"; \
	    echo "    make windows KATAM_SDL2_MINGW=\$$PWD/SDL2-2.30.11/i686-w64-mingw32"; \
	    exit 1; }
	@cmake -S . -B $(WIN_DIR) -DCMAKE_BUILD_TYPE=Release \
	       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-i686.cmake \
	       -DKATAM_SDL2_MINGW=$(KATAM_SDL2_MINGW) >/dev/null \
	  || { echo; \
	       echo "The Windows build is 32-bit MinGW -- see docs/NATIVE.md."; \
	       echo "Debian/Ubuntu: sudo apt install gcc-mingw-w64-i686"; \
	       echo "Without root, see \"Building without root\" in docs/NATIVE.md."; \
	       exit 1; }
	@cmake --build $(WIN_DIR) -j $(shell nproc 2>/dev/null || echo 4)
	@echo "  WINDOWS $(WIN_BIN)"

# The folder a player can actually run: the .exe, the SDL2.dll it linked
# against, and a readme saying to bring their own ROM.  The find is the same
# guard check-dist puts on the web publish directory, for the same reason: what
# leaves this tree must carry no game data.
windows-package: windows
	@cmake --build $(WIN_DIR) --target package-windows >/dev/null
	@bad=$$(find $(WIN_PKG) -type f \( -iname '*.gba' -o -iname '*.gb' \
	         -o -iname '*.gbc' -o -iname '*.bin' -o -iname '*.sav' \
	         -o -size +8M \)); \
	if [ -n "$$bad" ]; then \
	    echo "REFUSING TO PACKAGE -- $(WIN_PKG) contains game data:"; \
	    echo "$$bad"; \
	    exit 1; \
	fi
	@echo "  PACKAGE $(WIN_PKG)"

windows-clean:
	rm -rf $(WIN_DIR)

# --- publishing -----------------------------------------------------------
# Assembles the three build outputs into a directory that can be served as-is.
# What ships is the port: the page, the loader and the wasm.  No ROM, and no
# way for the site to supply one -- the player brings their own, from a local
# file or a URL they choose.
DIST           := $(BUILD)/dist
PAGES_PROJECT  ?= katam-port
# Set CF_ACCOUNT_ID in your environment; wrangler needs it when the token
# can see more than one account.
CF_ACCOUNT_ID  ?=

dist: all
	@rm -rf $(DIST) && mkdir -p $(DIST)
	@cp $(OUT)/katam.html $(DIST)/index.html
	@cp $(OUT)/katam.js $(OUT)/katam.wasm $(DIST)/
	@python3 tools/stamp_build.py --dir $(DIST)
	@# The URLs carry a build id, so the payloads can be cached hard.  The page
	@# itself must not be: it is what points at the current build.
	@printf '/*.wasm\n  Cache-Control: public, max-age=31536000, immutable\n' > $(DIST)/_headers
	@printf '/*.js\n  Cache-Control: public, max-age=31536000, immutable\n' >> $(DIST)/_headers
	@printf '/\n  Cache-Control: no-cache, must-revalidate\n' >> $(DIST)/_headers
	@printf '/index.html\n  Cache-Control: no-cache, must-revalidate\n' >> $(DIST)/_headers
	@printf 'User-agent: *\nDisallow: /\n' > $(DIST)/robots.txt
	@echo "  DIST    $(DIST) ($$(du -sh $(DIST) | cut -f1))"

# The last thing between a ROM and the internet.  dist rebuilds the directory
# from three named files so this should never fire -- which is the point: it
# guards the publish itself, not the assembly, and it runs on whatever is
# actually about to be uploaded.
check-dist:
	@bad=$$(find $(DIST) -type f \( -iname '*.gba' -o -iname '*.gb' -o -iname '*.gbc' \
	         -o -iname '*.bin' -o \( -size +8M ! -name 'katam.wasm' \) \)); \
	if [ -n "$$bad" ]; then \
	    echo "REFUSING TO PUBLISH -- $(DIST) contains game data:"; \
	    echo "$$bad"; \
	    exit 1; \
	fi; \
	echo "  CHECK   $(DIST) carries no game data"

# --- publishing ------------------------------------------------------------
#
# GitHub Pages is the site.  https://sh1ftmaker.github.io/katam-port/, served
# from the gh-pages branch by scripts/publish-pages.sh.
#
# The Cloudflare Pages project (katam-port.pages.dev) was taken down on
# 2026-08-05 at the owner's request and must not be recreated.  Both routes to
# it refuse below rather than being deleted, because a `deploy` target that has
# quietly vanished invites someone to write it again from memory, and running
# `wrangler pages deploy` against a project name that no longer exists does not
# fail -- it *creates* the project and puts the site back up.
CF_TAKEN_DOWN = \
	echo "katam-port.pages.dev was taken down on purpose and must not come back."; \
	echo "wrangler would recreate the project rather than fail.  Use: make pages"; \
	exit 1

# Publish to GitHub Pages.  This is the one that publishes.
pages:
	@bash scripts/publish-pages.sh

# The whole chain: sync, rebuild, publish, and verify the live page.
# scripts/release.sh deploys to Cloudflare, so it is off until it does not.
release:
	@$(CF_TAKEN_DOWN)

deploy:
	@$(CF_TAKEN_DOWN)

# --- convenience ----------------------------------------------------------
# Serving a ROM next to the page makes ?rom= same-origin, which is the one way
# the URL path works without the remote host opting in via CORS.  It stays
# local: *.gba is gitignored, and `dist` copies three named files by name, so
# nothing here can reach a deployment.
serve: all
	@if [ -f "$(ROM)" ]; then \
	    cp "$(ROM)" $(OUT)/rom.gba; \
	    echo "  serving your ROM locally at /rom.gba (not published)"; \
	    echo "  http://localhost:8000/katam.html?rom=/rom.gba"; \
	else \
	    echo "  http://localhost:8000/katam.html"; \
	    echo "  (set ROM=/path/to/your.gba to have it served alongside)"; \
	fi
	@cd $(OUT) && python3 -m http.server 8000

clean:
	rm -rf $(BUILD)/obj $(OUT)/katam.html $(OUT)/katam.js $(OUT)/katam.wasm

distclean: clean
	rm -rf $(BUILD)
