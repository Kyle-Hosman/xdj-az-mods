# SPDX-License-Identifier: GPL-3.0-only
# Builds azmod.so and azstem.so for the XDJ-AZ (aarch64, glibc). Run it inside the
# azmod-build:18.04 container (see ../BUILDING.md); the official releases are built
# with exactly these commands, so the same source gives the same bytes.
#
#   make CC=gcc azmod azstem           -> out/azmod.so, out/azstem.so
#   make CC=gcc azhandoff              -> out/az-handoff-guard (rides the stick in MODS/deck/lib)
#
# The compile lines below are kept identical to the ones the official release is
# built with. The export tool checks that (make -n) every time it runs.

CC ?= gcc

# `-mno-outline-atomics` is a GCC 10+ flag. Older GCCs (e.g. Ubuntu 18.04's
# 7.5) error out on it. Probe and add it only if the compiler accepts it.
NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' \
    | $(CC) -x c -mno-outline-atomics -c -o /dev/null - 2>/dev/null \
    && echo -mno-outline-atomics)

OUT := out
AZMOD := $(OUT)/azmod.so
AZSTEM := $(OUT)/azstem.so
AZHANDOFF := $(OUT)/az-handoff-guard

.PHONY: all azmod azstem azhandoff clean
all: azmod azstem azhandoff
azmod: $(OUT) $(AZMOD)
azstem: $(OUT) $(AZSTEM)
azhandoff: $(OUT) $(AZHANDOFF)

$(OUT):
	mkdir -p $(OUT)

# --- azmod.so (the Mod Menu and every on-screen mod; LD_PRELOAD into EP147) ---
# azmod/text.c renders overlay labels with FreeType (already on the deck).
AZMOD_FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null || echo -I/usr/include/freetype2)
# Implicit function declarations must be errors: an LD_PRELOAD object links fine against a
# function that does not exist and only fails when the call runs, inside EP147.
# AZ_UNWIND: EP147 throws C++ exceptions through our hooks on normal error paths;
# every C frame needs unwind info or a handled error becomes std::terminate.
AZ_UNWIND := -fexceptions -fasynchronous-unwind-tables
# Warnings are errors: the source builds with none under -Wall -Wextra (implicit function
# declarations included, the reason given above).
AZ_WARN := -Wall -Wextra -Werror
AZMOD_CFLAGS := $(AZ_UNWIND) -O2 $(AZ_WARN) -fPIC -shared -static-libgcc -Wl,--no-undefined $(NO_OUTLINE_ATOMICS) -Iazmod $(AZMOD_FT_CFLAGS)
# Release mode (the default) hides rows that are still in testing.
AZMOD_RELEASE ?= 1
ifeq ($(AZMOD_RELEASE),1)
AZMOD_CFLAGS += -DAZMOD_RELEASE
endif

# Release builds ship without the full symbol table: each target links to <target>.full, the
# shipped file is that link with --strip-unneeded (the dynamic exports stay). The .full file is
# the same build with its symbols, for the unit tests' binary lints (procmem-lint.sh scans it and
# proves the shipped file is exactly it, stripped) and for reading a crash address. Development
# builds (AZMOD_RELEASE=0) ship unstripped.
STRIP ?= strip
OBJCOPY ?= objcopy
# azmod.so keeps exactly one group of symbols: the arrays of the asset headers below (art made
# from the deck's firmware, and the library key), which the public source tree does not ship and
# its tools/extract-assets.py copies out of an official azmod.so by name. The list is those
# headers' identifiers that are data symbols of this build, so it follows the headers.
AZMOD_ASSET_HDRS := $(addprefix azmod/,caption_strips.h fxart.h fxhdr.h fxbars.h holdtile.h sfxtile.h menulabels.h collabels.h stems4art.h srcref.h wavref.h onelibkey.h onelibkey_sm.h)
ifeq ($(AZMOD_RELEASE),1)
az_strip = $(STRIP) --strip-unneeded -o $(1) $(1).full
az_strip_keep_assets = nm --defined-only $(1).full | awk '$$2 ~ /^[rRdD]$$/ {print $$3}' | sort -u > $(1).objs && \
	grep -ohE '[A-Za-z_][A-Za-z0-9_]*' $(AZMOD_ASSET_HDRS) | sort -u | comm -12 - $(1).objs > $(1).keep && \
	$(OBJCOPY) --strip-unneeded --keep-symbols=$(1).keep $(1).full $(1) && rm -f $(1).objs
else
az_strip_keep_assets = cp $(1).full $(1)
az_strip = cp $(1).full $(1)
endif
# Test triggers and diagnostic probes: emulator test builds only, never released.
ifeq ($(AZMOD_PROBES),1)
AZMOD_CFLAGS += -DAZMOD_PROBES
endif
# menu.c #includes the Mod Menu's parts (ui_draw.c ... menu_composite.c, listed there): they are
# not translation units of their own, and the azmod/*.c wildcard below makes them prerequisites.
AZMOD_SRCS := azmod/azmod.c azmod/menu.c azmod/text.c azmod/azconf.c
AZMOD_HDRS := $(wildcard azmod/*.h azmod/*.c azmod/*.def) common/ep147build.h
$(AZMOD): $(AZMOD_SRCS) $(AZMOD_HDRS) | $(OUT)
	$(CC) $(AZMOD_CFLAGS) -o $@.full $(AZMOD_SRCS) -ldl -lpthread -lX11 -lXext -lfreetype -lm
	$(call az_strip_keep_assets,$@)
	@echo "Built: $@"

# --- azstem.so (stems audio, drum rolls; LD_PRELOAD into EP147) ---
# stb_vorbis.c (public domain / MIT, vendored) is its own translation unit.
AZSTEM_CFLAGS := $(AZ_UNWIND) -O2 $(AZ_WARN) -fPIC -shared -static-libgcc $(NO_OUTLINE_ATOMICS) -Iazstem -DAZMOD_RELEASE
AZSTEM_STB_CFLAGS := $(AZ_UNWIND) -O2 -w -fPIC $(NO_OUTLINE_ATOMICS) -Iazstem -fvisibility=hidden
$(AZSTEM): $(wildcard azstem/*.c azstem/*.h) common/ep147build.h | $(OUT)
	$(CC) $(AZSTEM_STB_CFLAGS) -c -o $(OUT)/stb_vorbis.o azstem/stb_vorbis.c
	$(CC) $(AZSTEM_CFLAGS) -o $@.full azstem/azstem.c $(OUT)/stb_vorbis.o -lm -ldl -lpthread
	$(call az_strip,$@)
	@echo "Built: $@"

# --- az-handoff-guard (the way back to the deck during a mod's handoff; runs while EP147 is stopped) ---
# static: it must not depend on anything else on the deck
$(AZHANDOFF): azhandoff/az-handoff-guard.c azhandoff/guard_logic.h | $(OUT)
	$(CC) -O2 -Wall -Wextra -static -Werror $(NO_OUTLINE_ATOMICS) -o $@.full azhandoff/az-handoff-guard.c
	$(call az_strip,$@)

clean:
	rm -rf $(OUT)
