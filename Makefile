# SPDX-License-Identifier: MPL-2.0
# Standalone Linux player. Build: make -j4; run: make run
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
PREFIX ?= /usr/local
DESTDIR ?=
CFLAGS ?= -O3 -DNDEBUG
LTO ?= -flto
ROM ?= roms/dragonball.gba

BUILD_DIR := build
TARGET := $(BUILD_DIR)/mgba
CORE_LIBRARY := $(BUILD_DIR)/libmgba.a
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
PROJECT_CPPFLAGS := -D_GNU_SOURCE -Iinclude -Isrc -include mgba/flags.h
PROJECT_CFLAGS := -std=c11 -fwrapv -pthread -Wall -Wextra \
	-Wno-missing-field-initializers -Werror=implicit-function-declaration \
	-Werror=implicit-int -Werror=incompatible-pointer-types

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
	src/core/thread.c \
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

SDL_SOURCES := \
	src/platform/sdl/main.c \
	src/platform/sdl/sdl-audio.c \
	src/platform/sdl/sdl-events.c \
	src/platform/sdl/sw-sdl2.c
CORE_OBJECTS := $(CORE_SOURCES:%.c=$(BUILD_DIR)/%.o)
SDL_OBJECTS := $(SDL_SOURCES:%.c=$(BUILD_DIR)/%.o)
OBJECTS := $(CORE_OBJECTS) $(SDL_OBJECTS)

.PHONY: all check-deps run test clean install FORCE
all: $(TARGET)

# Checks run only for builds; cleaning does not require installed dependencies.
check-deps:
	@test "$$(uname -s)" = Linux || { echo "This Makefile targets Linux." >&2; exit 1; }
	@$(PKG_CONFIG) --exists sdl2 || { echo "SDL2 development files and pkg-config are required." >&2; exit 1; }

$(BUILD_DIR):
	mkdir -p "$@"

# A changed command line invalidates objects; unchanged settings keep timestamps.
$(BUILD_DIR)/.build-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(CC)' '$(AR)' '$(PROJECT_CPPFLAGS) $(CPPFLAGS)' \
		'$(PROJECT_CFLAGS) $(CFLAGS) $(LTO)' '$(SDL_CFLAGS)' \
		'$(LDFLAGS) $(LDLIBS) $(SDL_LIBS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(OBJECTS): Makefile $(BUILD_DIR)/.build-config | check-deps
$(SDL_OBJECTS): private FRONTEND_CPPFLAGS := -DBUILD_SDL $(SDL_CFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p "$(@D)"
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(FRONTEND_CPPFLAGS) \
		$(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(CORE_LIBRARY): $(CORE_OBJECTS)
	@rm -f "$@"
	$(AR) rcs "$@" $(CORE_OBJECTS)

$(TARGET): $(SDL_OBJECTS) $(CORE_LIBRARY)
	$(CC) $(CFLAGS) $(LTO) $(LDFLAGS) -o "$@" $(SDL_OBJECTS) \
		$(CORE_LIBRARY) $(SDL_LIBS) -pthread -lm $(LDLIBS)

run: $(TARGET)
	"./$(TARGET)" "$(ROM)"

test: $(TARGET)
	$(PYTHON) -u src/platform/sdl/test/smoke.py $(TARGET)

install: $(TARGET)
	install -d "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/share/man/man6" \
		"$(DESTDIR)$(PREFIX)/share/doc/mgba"
	install -m 755 $(TARGET) "$(DESTDIR)$(PREFIX)/bin/mgba"
	install -m 644 doc/mgba.6 "$(DESTDIR)$(PREFIX)/share/man/man6/mgba.6"
	install -m 644 LICENSE README.md "$(DESTDIR)$(PREFIX)/share/doc/mgba/"
	install -m 644 src/third-party/inih/LICENSE.txt "$(DESTDIR)$(PREFIX)/share/doc/mgba/inih-license.txt"

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
