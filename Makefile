# katam-port -- WebAssembly port of the Kirby & The Amazing Mirror decompilation
#
#   make            build web/katam.html + katam.js + katam.wasm
#   make sync       re-copy and re-codemod the decomp sources (do this after
#                   pulling new decompilation work)
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

WARN := -Wno-everything

# -fgnu89-inline matters more than it looks.  The decomp is compiled by agbcc
# (gcc 2.95), where a plain `inline` definition also emits an external one.
# C99 inverted that rule, so without this flag every `inline void Foo(...)` in
# the game becomes an undefined symbol at link time.
CFLAGS := -O2 $(DBG_CFLAG) -std=gnu99 -fgnu89-inline -fno-strict-aliasing -fwrapv \
          $(WARN) $(DEFINES) $(INCLUDES) $(FORCE_INCLUDE) -MMD -MP

# --profiling-funcs keeps the wasm name section, so a trap on someone's phone
# reports `sub_0805405C` instead of `wasm-function[731]`.  It costs a little
# size and nothing in speed, which is a good trade while the port still
# crashes.
LDFLAGS := -O2 --profiling-funcs \
    -sASYNCIFY \
    -sASYNCIFY_STACK_SIZE=32768 \
    -sGLOBAL_BASE=$(GLOBAL_BASE) \
    -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
    -sALLOW_MEMORY_GROWTH=0 \
    -sSTACK_SIZE=1048576 \
    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,ccall,cwrap \
    -sENVIRONMENT=web \
    --shell-file web/shell.html

GAME_SRCS     := $(shell find $(PORT_SRC)/src -name '*.c' 2>/dev/null)
PLATFORM_SRCS := $(wildcard platform/*.c)
GEN_SRCS      := $(wildcard $(GENERATED)/*.c)
SRCS          := $(GAME_SRCS) $(PLATFORM_SRCS) $(GEN_SRCS)
OBJS          := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

TARGET := $(OUT)/katam.html

.PHONY: all sync clean serve compile stubs test debug prune dist deploy check-dist release pages

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
	    --tree $(PORT_SRC) --out $(GENERATED)/port/ram_symbols.h
	python3 tools/gen_rom_data.py \
	    --data-dir $(KATAM_DECOMP)/data \
	    --tree $(PORT_SRC) --out $(GENERATED)/port/rom_data.h \
	    --out-copies $(GENERATED)/rom_copies.c \
	    --out-tables $(GENERATED)/rom_fn_tables.c \
	    --map $(KATAM_DECOMP)/katam.map --rom $(ROM)

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
	$(CC) -O2 --profiling-funcs -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask \
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
	$(CC) -O1 -g --profiling-funcs \
	    -gseparate-dwarf=$(BUILD)/katam-dbg.debug.wasm \
	    -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask \
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
	$(CC) -O1 --profiling-funcs -sSAFE_HEAP=1 -sASSERTIONS=1 \
	    -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded,_PortSetLayerMask \
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

# The whole chain: sync, rebuild, publish, and verify the live page.
# scripts/release.sh also fixes up PATH, since make's shell cannot find the
# nvm-installed node that wrangler needs.
release:
	@bash scripts/release.sh

# Publish to GitHub Pages instead of (or as well as) Cloudflare.
pages:
	@bash scripts/publish-pages.sh

deploy: dist check-dist
	CLOUDFLARE_ACCOUNT_ID=$(CF_ACCOUNT_ID) npx --yes wrangler@latest pages deploy $(DIST) \
	    --project-name=$(PAGES_PROJECT) --branch=main --commit-dirty=true

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
