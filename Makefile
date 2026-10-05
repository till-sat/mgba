# SPDX-License-Identifier: MPL-2.0
# AM player. Build: make -j4; run: make run
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
PREFIX ?= /usr/local
DESTDIR ?=
CFLAGS ?= -O3 -DNDEBUG
LTO ?= -flto=auto
RUNNER_THREADED ?= 0
ARGS ?=
PLATFORM ?= native
ifeq ($(PLATFORM),ysyxsoc)
ROM ?= cinema/gba/obj/2d-wrap/test.gba
else
ROM ?= dragonball
endif

# User-facing ROM values are game names. Resolve a name to the fixed ROM
# directory while retaining path values for temporary fixtures and platform
# tests. An extension may be supplied for GB/GBC ROMs; GBA names default to
# .gba.
ROM_DIR ?= roms
ROM_INPUT := $(ROM)
ifneq ($(findstring /,$(ROM_INPUT)),)
ROM_PATH := $(ROM_INPUT)
else ifneq ($(suffix $(ROM_INPUT)),)
ROM_PATH := $(ROM_DIR)/$(ROM_INPUT)
else
ROM_PATH := $(ROM_DIR)/$(ROM_INPUT).gba
endif

ifeq ($(filter $(PLATFORM),native spike verilator fpga ysyxsoc),)
$(error Unsupported PLATFORM '$(PLATFORM)'; choose native, spike, verilator, fpga or ysyxsoc)
endif

BUILD_DIR := build
TARGET := $(BUILD_DIR)/mgba
CORE_LIBRARY := $(BUILD_DIR)/libmgba.a
AM_LIBRARY := $(BUILD_DIR)/libam.a
ifeq ($(PLATFORM),native)
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
endif
PROJECT_CPPFLAGS := -D_GNU_SOURCE -Iinclude -Isrc -Iam/include -include mgba/flags.h
PROJECT_CFLAGS := -std=c11 -fwrapv -Wall -Wextra \
	-Wno-missing-field-initializers -Werror=implicit-function-declaration \
	-Werror=implicit-int -Werror=incompatible-pointer-types

ifeq ($(RUNNER_THREADED),1)
PROJECT_CPPFLAGS += -DMGBA_RUNNER_THREADED
endif

# Keep this list explicit: only the standalone player's core is built.
CORE_SOURCES := \
	src/arm/arm.c \
	src/arm/decoder-arm.c \
	src/arm/decoder-thumb.c \
	src/arm/isa-arm.c \
	src/arm/isa-thumb.c \
	src/core/bitmap-cache.c \
	src/core/cache-set.c \
	src/core/cheats.c \
	src/core/config.c \
	src/core/core.c \
	src/core/directories.c \
	src/core/interface.c \
	src/core/log.c \
	src/core/map-cache.c \
	src/core/rewind.c \
	src/core/serialize.c \
	src/core/sync.c \
	src/core/tile-cache.c \
	src/core/timing.c \
	src/core/version.c \
	src/gb/audio.c \
	src/gb/cheats.c \
	src/gb/core.c \
	src/gb/gb.c \
	src/gb/io.c \
	src/gb/mbc.c \
	src/gb/mbc/huc-3.c \
	src/gb/mbc/licensed.c \
	src/gb/mbc/mbc.c \
	src/gb/mbc/pocket-cam.c \
	src/gb/mbc/tama5.c \
	src/gb/mbc/unlicensed.c \
	src/gb/memory.c \
	src/gb/overrides.c \
	src/gb/renderers/cache-set.c \
	src/gb/renderers/software.c \
	src/gb/serialize.c \
	src/gb/sio.c \
	src/gb/timer.c \
	src/gb/video.c \
	src/gba/audio.c \
	src/gba/bios.c \
	src/gba/cart/ereader.c \
	src/gba/cart/gpio.c \
	src/gba/cart/matrix.c \
	src/gba/cart/unlicensed.c \
	src/gba/cart/vfame.c \
	src/gba/cheats.c \
	src/gba/cheats/codebreaker.c \
	src/gba/cheats/gameshark.c \
	src/gba/cheats/parv3.c \
	src/gba/core.c \
	src/gba/dma.c \
	src/gba/gba.c \
	src/gba/hle-bios.c \
	src/gba/io.c \
	src/gba/memory.c \
	src/gba/overrides.c \
	src/gba/renderers/cache-set.c \
	src/gba/renderers/common.c \
	src/gba/renderers/software-bg.c \
	src/gba/renderers/software-mode0.c \
	src/gba/renderers/software-obj.c \
	src/gba/renderers/video-software.c \
	src/gba/savedata.c \
	src/gba/serialize.c \
	src/gba/sio.c \
	src/gba/sio/gbp.c \
	src/gba/timer.c \
	src/gba/video.c \
	src/platform/posix/memory.c \
	src/sm83/isa-sm83.c \
	src/sm83/sm83.c \
	src/third-party/inih/ini.c \
	src/util/audio-buffer.c \
	src/util/audio-resampler.c \
	src/util/circle-buffer.c \
	src/util/configuration.c \
	src/util/crc32.c \
	src/util/formatting.c \
	src/util/gbk-table.c \
	src/util/geometry.c \
	src/util/hash.c \
	src/util/image.c \
	src/util/interpolator.c \
	src/util/md5.c \
	src/util/patch-fast.c \
	src/util/patch-ips.c \
	src/util/patch-ups.c \
	src/util/patch.c \
	src/util/sha1.c \
	src/util/string.c \
	src/util/table.c \
	src/util/vector.c \
	src/util/vfs.c \
	src/util/vfs/vfs-dirent.c \
	src/util/vfs/vfs-fd.c \
	src/util/vfs/vfs-mem.c

FRONTEND_SOURCES := src/platform/am/main.c src/platform/am/player.c
ifeq ($(PLATFORM),native)
AM_SOURCES := am/src/native/native.c
endif
AM_CPPFLAGS := $(SDL_CFLAGS)
PLATFORM_LIBS := $(SDL_LIBS) -lm
ifneq ($(PLATFORM),native)
include am/platform/$(PLATFORM).mk
endif
CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD_DIR)/%.o)
FRONTEND_OBJECTS := $(FRONTEND_SOURCES:%.c=$(BUILD_DIR)/%.o)
AM_OBJECTS := $(AM_SOURCES:%.c=$(BUILD_DIR)/%.o)
OBJECTS := $(CORE_OBJECTS) $(FRONTEND_OBJECTS) $(AM_OBJECTS)

.PHONY: all check-deps run test test-am test-spike test-verilator test-ysyxsoc clean install FORCE
all: $(TARGET)
ifneq ($(PLATFORM),native)
all: check-rv32
endif

# Checks run only for builds; cleaning does not require installed dependencies.
check-deps:
ifeq ($(PLATFORM),native)
	@test "$$(uname -s)" = Linux || { echo "This Makefile targets Linux." >&2; exit 1; }
	@$(PKG_CONFIG) --exists sdl2 || { echo "SDL2 development files and pkg-config are required." >&2; exit 1; }
else
	@command -v $(CC) >/dev/null || { echo "The RV32 bare-metal toolchain is required." >&2; exit 1; }
endif

$(BUILD_DIR):
	mkdir -p "$@"

# A changed command line invalidates objects; unchanged settings keep timestamps.
$(BUILD_DIR)/.build-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(CC)' '$(AR)' '$(PROJECT_CPPFLAGS) $(CPPFLAGS)' \
		'$(PROJECT_CFLAGS) $(CFLAGS) $(LTO)' '$(SDL_CFLAGS)' \
		'$(LDFLAGS) $(LDLIBS) $(PLATFORM_LIBS)' '$(AM_CPPFLAGS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(OBJECTS): Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | check-deps $(RUNTIME_READY)

# The AM library has no mGBA headers, build flags, or core dependency.
$(BUILD_DIR)/am/%.o: am/%.c
	@mkdir -p "$(@D)"
	$(CC) -Iam/include $(AM_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) \
		-MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p "$(@D)"
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) \
		$(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(CORE_LIBRARY): $(CORE_OBJECTS)
	@rm -f "$@"
	$(AR) rcs "$@" $(CORE_OBJECTS)

$(AM_LIBRARY): $(AM_OBJECTS)
	@rm -f "$@"
	$(AR) rcs "$@" $(AM_OBJECTS)

$(TARGET): $(FRONTEND_OBJECTS) $(CORE_LIBRARY) $(AM_LIBRARY) $(BOOT_OBJECT) $(ROM_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PLATFORM_CFLAGS) $(CFLAGS) $(LTO) $(LDFLAGS) $(PLATFORM_LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(ROM_OBJECT) $(FRONTEND_OBJECTS) \
		-Wl,--start-group $(CORE_LIBRARY) $(AM_LIBRARY) $(PLATFORM_LIBS) $(LDLIBS) -Wl,--end-group

ifeq ($(PLATFORM),native)
run: $(TARGET)
	"$(abspath $(TARGET))" $(ARGS) "$(ROM_PATH)"

$(BUILD_DIR)/am/test/native.o: Makefile $(BUILD_DIR)/.build-config | check-deps

$(BUILD_DIR)/am-native-test: $(BUILD_DIR)/am/test/native.o $(AM_LIBRARY)
	$(CC) $(CFLAGS) $(LTO) $(LDFLAGS) -o "$@" $^ $(SDL_LIBS) $(LDLIBS)

test-am: $(BUILD_DIR)/am-native-test
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$(abspath $(BUILD_DIR)/am-native-test)"

test: $(TARGET) test-am
	$(PYTHON) -u src/platform/am/test/smoke.py $(TARGET)

install: $(TARGET)
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/share/man/man6" \
		"$(DESTDIR)$(PREFIX)/share/doc/mgba"
	install -m 755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/mgba"
	install -m 644 doc/mgba.6 "$(DESTDIR)$(PREFIX)/share/man/man6/mgba.6"
	install -m 644 LICENSE README.md "$(DESTDIR)$(PREFIX)/share/doc/mgba/"
	install -m 644 src/third-party/inih/LICENSE.txt "$(DESTDIR)$(PREFIX)/share/doc/mgba/inih-license.txt"
else
install:
	@echo "Install is supported only for PLATFORM=native." >&2; exit 1
endif

test-spike:
	+$(MAKE) PLATFORM=spike test

.PHONY: test-threaded
test-threaded:
	$(PYTHON) -u am/test/thumb-threaded.py

test-verilator:
	+$(MAKE) PLATFORM=verilator test

ifneq ($(PLATFORM),ysyxsoc)
test-ysyxsoc:
	+$(MAKE) PLATFORM=ysyxsoc test
endif

ifeq ($(PLATFORM),native)
.PHONY: test-interactive test-media-bus
test-interactive: test test-media-bus
	+$(MAKE) PLATFORM=spike test-interactive
	+$(MAKE) PLATFORM=verilator test-interactive
	+$(MAKE) PLATFORM=ysyxsoc test-interactive

test-media-bus:
	verilator --cc --exe --build -j 4 --top-module axi_media_bridge -Wno-fatal \
		--Mdir "$(abspath $(BUILD_DIR)/media-bus)" am/sim/axi_media_bridge.sv \
		"$(abspath am/test/rtl-media-bus.cpp)" -o bus-test
	"$(abspath $(BUILD_DIR)/media-bus/bus-test)"
endif

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d) $(BUILD_DIR)/am/test/native.d $(BUILD_DIR)/am/test/riscv.d $(BUILD_DIR)/am/test/media.d
