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
# The reserved GBA map runs from EWRAM at 0x02000000 up to the end of save
# memory at 0x0E010000, so the compiler's own data starts above that.
# emcc parses these as byte sizes and rejects 0x-prefixed hex, so they are in
# decimal:  251658240 = 0x0F000000,  285212672 = 0x11000000.
GLOBAL_BASE    := 251658240
INITIAL_MEMORY := 285212672

# ---------------------------------------------------------------------------
# PORTABLE     -- the decomp's own guard (include/portable.h) that compiles the
#                 register pins and codegen barriers away.  Added upstream.
# NONMATCHING  -- selects the readable C branch wherever a function has one,
#                 instead of the byte-matching branch.
# ---------------------------------------------------------------------------
DEFINES := -DPORTABLE -DNONMATCHING -DMODERN=1

INCLUDES := -I$(PORT_SRC)/include -I$(GENERATED) -Iplatform

# The generated headers replace declarations that used to live in the game's
# own headers, so they have to be in scope before anything else is parsed.
FORCE_INCLUDE := -include port/prelude.h -include port/ram_symbols.h -include port/rom_data.h

WARN := -Wno-everything

# -fgnu89-inline matters more than it looks.  The decomp is compiled by agbcc
# (gcc 2.95), where a plain `inline` definition also emits an external one.
# C99 inverted that rule, so without this flag every `inline void Foo(...)` in
# the game becomes an undefined symbol at link time.
CFLAGS := -O2 -g0 -std=gnu99 -fgnu89-inline -fno-strict-aliasing -fwrapv \
          $(WARN) $(DEFINES) $(INCLUDES) $(FORCE_INCLUDE) -MMD -MP

LDFLAGS := -O2 \
    -sASYNCIFY \
    -sASYNCIFY_STACK_SIZE=32768 \
    -sGLOBAL_BASE=$(GLOBAL_BASE) \
    -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
    -sALLOW_MEMORY_GROWTH=0 \
    -sSTACK_SIZE=1048576 \
    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,ccall,cwrap \
    -sENVIRONMENT=web \
    --shell-file web/shell.html

GAME_SRCS     := $(shell find $(PORT_SRC)/src -name '*.c' 2>/dev/null)
PLATFORM_SRCS := $(wildcard platform/*.c)
GEN_SRCS      := $(wildcard $(GENERATED)/*.c)
SRCS          := $(GAME_SRCS) $(PLATFORM_SRCS) $(GEN_SRCS)
OBJS          := $(patsubst %.c,$(BUILD)/obj/%.o,$(SRCS))

TARGET := $(OUT)/katam.html

.PHONY: all sync clean serve compile stubs test prune

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
	@find $(BUILD)/obj -name '*.o' 2>/dev/null | while read o; do \
	    src=$${o#$(BUILD)/obj/}; src=$${src%.o}.c; \
	    [ -f "$$src" ] || { echo "  PRUNE   $$o"; rm -f "$$o" "$${o%.o}.d"; }; \
	done

$(PORT_SRC)/src: ; @$(MAKE) sync

# --- compile --------------------------------------------------------------
$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

compile: $(OBJS)

# Rebuild when a header changes -- the force-included port headers are
# regenerated on every sync, and a stale object silently reverts a fix.
-include $(OBJS:.o=.d)

$(TARGET): $(OBJS) web/shell.html
	@mkdir -p $(OUT)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# --- headless smoke test --------------------------------------------------
# Boots the port under node with a real ROM and runs frames, so "does it get
# past GameInit" can be answered without a browser.  Needs a ROM you own; the
# path is yours to supply and nothing here is committed.
ROM ?= $(KATAM_DECOMP)/baserom.gba

$(BUILD)/katam-node.js: $(OBJS)
	$(CC) -O2 -sASYNCIFY -sASYNCIFY_STACK_SIZE=32768 \
	    -sGLOBAL_BASE=$(GLOBAL_BASE) -sINITIAL_MEMORY=$(INITIAL_MEMORY) \
	    -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=1048576 \
	    -sEXPORTED_FUNCTIONS=_main,_PortSetKeys,_PortRomLoaded \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
	    -sENVIRONMENT=node -sMODULARIZE=1 -sEXPORT_NAME=createKatam -sINVOKE_RUN=1 \
	    $(OBJS) -o $@

test: $(BUILD)/katam-node.js
	@test -f $(ROM) || { echo "no ROM at $(ROM) -- set ROM=/path/to/your.gba"; exit 1; }
	node tools/headless_test.js $(BUILD)/katam-node.js $(ROM) $(FRAMES)

FRAMES ?= 180

# --- convenience ----------------------------------------------------------
serve: all
	@echo "http://localhost:8000/katam.html"
	@cd $(OUT) && python3 -m http.server 8000

clean:
	rm -rf $(BUILD)/obj $(OUT)/katam.html $(OUT)/katam.js $(OUT)/katam.wasm

distclean: clean
	rm -rf $(BUILD)
