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
RV32_RUNNER ?= 0
RV32_STATS ?= 0
RV32_TRACE_POLL ?= 1
AM_SCANLINE_RGB ?= 1
GBN_DISPLAY ?= 0
GBA_IDLE_SKIP ?= 0
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

ifeq ($(filter $(PLATFORM),native spike spike_zve32x verilator fpga ysyxsoc),)
$(error Unsupported PLATFORM '$(PLATFORM)'; choose native, spike, spike_zve32x, verilator, fpga or ysyxsoc)
endif

BUILD_DIR := build
TARGET := $(BUILD_DIR)/mgba
CORE_LIBRARY := $(BUILD_DIR)/libmgba.a
AM_LIBRARY := $(BUILD_DIR)/libam.a
GBN_LIBRARY = $(BUILD_DIR)/libgba-next.a
ifeq ($(PLATFORM),native)
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
endif
PROJECT_CPPFLAGS := -D_GNU_SOURCE -Iinclude -Isrc -Iam/include -include mgba/flags.h
PROJECT_CFLAGS := -std=c11 -fwrapv -Wall -Wextra \
	-Wno-missing-field-initializers -Werror=implicit-function-declaration \
	-Werror=implicit-int -Werror=incompatible-pointer-types

ifeq ($(AM_SCANLINE_RGB),1)
PROJECT_CPPFLAGS += -DMGBA_AM_SCANLINE_RGB
endif

ifeq ($(RUNNER_THREADED),1)
PROJECT_CPPFLAGS += -DMGBA_RUNNER_THREADED
endif
ifeq ($(RV32_RUNNER),1)
PROJECT_CPPFLAGS += -DMGBA_RV32
ifeq ($(RV32_TRACE_POLL),1)
PROJECT_CPPFLAGS += -DMGBA_RV32_TRACE_POLL
endif
ifeq ($(RV32_STATS),1)
PROJECT_CPPFLAGS += -DMGBA_RV32_STATS
endif
endif
ifeq ($(GBA_IDLE_SKIP),1)
PROJECT_CPPFLAGS += -DMGBA_GBA_IDLE_SKIP
endif

# Keep this list explicit: only the standalone player's core is built.
CORE_SOURCES := \
	src/arm/arm.c \
	src/arm/decoder-arm.c \
	src/arm/decoder-thumb.c \
	src/arm/isa-arm.c \
	src/arm/isa-thumb.c \
	src/arm/rv32.c \
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
	src/gba/idle.c \
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

# The replacement core is built independently of mGBA's headers and flags.
# Its differential test alone links the old core as an oracle.
GBN_RV32 ?= 0
GBN_RV32_STATS ?= 0
# Translation descriptor capacity and associativity, independent of code bytes.
GBN_RV32_SLOTS ?= 16384
GBN_RV32_WAYS ?= 8
# Executable arena capacity; reduce for targets with less memory.
GBN_RV32_CODE_MIB ?= 16
# Optional vector PPU kernels: bit 0 = 4bpp rows, bit 1 = RGB composition,
# bit 2 = indexed spans. RISCV_C_ISA may keep ordinary C/LTO scalar.
GBN_PPU_RVV ?= 0
ifeq ($(GBN_RV32):$(PLATFORM),1:native)
$(error GBN_RV32=1 requires a bare-metal RV32 platform)
endif
GBN_OBJECTS := $(addprefix $(BUILD_DIR)/src/gba-next/,core.o devices.o dma.o bios.o timer.o audio.o sio.o save.o ppu.o ppu-rvv.o rv32.o)
GBN_CPPFLAGS := -Iinclude -DGBN_RV32=$(GBN_RV32) -DGBN_RV32_STATS=$(GBN_RV32_STATS) -DGBN_RV32_SLOTS=$(GBN_RV32_SLOTS) -DGBN_RV32_WAYS=$(GBN_RV32_WAYS) -DGBN_RV32_CODE_MIB=$(GBN_RV32_CODE_MIB) -DGBN_PPU_RVV=$(GBN_PPU_RVV)
ifeq ($(PROFILE),1)
GBN_CPPFLAGS += -DGBN_PROFILE
endif
ifneq ($(PLATFORM),native)
GBN_CPPFLAGS += -isystem $(NEWLIB_PREFIX)/include
endif

$(GBN_OBJECTS): Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
$(BUILD_DIR)/src/gba-next/%.o: src/gba-next/%.c
	@mkdir -p "$(@D)"
	$(CC) $(GBN_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

# GCC 14 cannot deserialize these RVV intrinsics in LTO ("target specific
# builtin not available"). Keep vector kernels in an ordinary object; the
# scalar core/renderer retain LTO and call this unit only on vector targets.
$(BUILD_DIR)/src/gba-next/ppu-rvv.o: src/gba-next/ppu-rvv.c
	@mkdir -p "$(@D)"
	$(CC) $(GBN_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(if $(RISCV_ISA),-march=$(RISCV_ISA)) -fno-lto -fno-strict-aliasing -MMD -MP -c "$<" -o "$@"

$(GBN_LIBRARY): $(GBN_OBJECTS)
	@rm -f "$@"
	$(AR) rcs "$@" $^

.PHONY: gba-next
gba-next: $(GBN_LIBRARY)

.PHONY: regen-gba-next-bios
regen-gba-next-bios:
	$(PYTHON) am/tools/gba_next_bios.py

-include $(GBN_OBJECTS:.o=.d)

# Fixed-work runner for the replacement core. Use the same input and frame
# window on native/Spike/FPGA; physical output is absent but PPU/APU stay on.
GBN_FRAMES ?= 120
GBN_WARMUP ?= 30
GBN_INPUT ?=
# Set to 0 for timing without validation CRCs or per-sample PCM inspection.
# PPU/APU simulation stays enabled in both modes.
GBN_VALIDATE ?= 1
ifeq ($(filter $(GBN_VALIDATE),0 1),)
$(error GBN_VALIDATE must be 0 or 1)
endif
GBN_BENCH_TARGET = $(BUILD_DIR)/gba-next-bench$(if $(filter native,$(PLATFORM)),,.elf)
GBN_BENCH_OBJECT = $(BUILD_DIR)/src/platform/gba-next/benchmark.o
GBN_ROM_OBJECT = $(BUILD_DIR)/gba-next-rom.o
GBN_BENCH_FLAGS = -DGBN_BENCH_FRAMES=$(GBN_FRAMES) -DGBN_BENCH_WARMUP=$(GBN_WARMUP) -DGBN_BENCH_VALIDATE=$(GBN_VALIDATE)
ifneq ($(filter $(PLATFORM),spike spike_zve32x fpga verilator),)
GBN_BENCH_FLAGS += -DGBN_COUNTERS
endif

$(BUILD_DIR)/.gba-next-bench-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(GBN_FRAMES)' '$(GBN_WARMUP)' '$(abspath $(GBN_INPUT))' '$(GBN_VALIDATE)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(BUILD_DIR)/gba-next-input.inc: $(GBN_INPUT) am/tools/gba_next_input.py $(BUILD_DIR)/.gba-next-bench-config
	$(PYTHON) am/tools/gba_next_input.py $(if $(GBN_INPUT),--input "$(GBN_INPUT)") --output "$@"

ifeq ($(PLATFORM),native)
$(BUILD_DIR)/.rom-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(abspath $(ROM_PATH))' '$(ROM_EMBED_FLAGS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"
endif

$(GBN_ROM_OBJECT): $(ROM_PATH) am/tools/embed_rom.py $(BUILD_DIR)/.rom-config $(BUILD_DIR)/.build-config
	$(PYTHON) am/tools/embed_rom.py "$(ROM_PATH)" "$(BUILD_DIR)/gba-next-rom.S" $(ROM_EMBED_FLAGS)
	$(CC) $(PLATFORM_CFLAGS) -c "$(BUILD_DIR)/gba-next-rom.S" -o "$@"

$(GBN_BENCH_OBJECT): src/platform/gba-next/benchmark.c $(BUILD_DIR)/gba-next-input.inc $(BUILD_DIR)/.gba-next-bench-config Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
	@mkdir -p "$(@D)"
	$(CC) $(GBN_CPPFLAGS) -Iam/include -I$(BUILD_DIR) $(AM_RUNNER_FLAGS) $(ROM_EMBED_CPPFLAGS) $(GBN_BENCH_FLAGS) $(CPPFLAGS) \
		$(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(GBN_BENCH_TARGET): $(GBN_BENCH_OBJECT) $(GBN_ROM_OBJECT) $(GBN_LIBRARY) $(AM_LIBRARY) $(BOOT_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PLATFORM_CFLAGS) $(CFLAGS) $(LTO) $(LDFLAGS) $(PLATFORM_LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(GBN_ROM_OBJECT) $(GBN_BENCH_OBJECT) \
		-Wl,--start-group $(GBN_LIBRARY) $(AM_LIBRARY) $(PLATFORM_LIBS) $(LDLIBS) -Wl,--end-group

.PHONY: gba-next-bench
gba-next-bench: $(GBN_BENCH_TARGET)

ifeq ($(PLATFORM),native)
.PHONY: run-gba-next-bench
run-gba-next-bench: $(GBN_BENCH_TARGET)
	"$(abspath $<)"
else ifneq ($(filter $(PLATFORM),spike spike_zve32x),)
.PHONY: run-gba-next-bench
run-gba-next-bench: $(GBN_BENCH_TARGET) $(SPIKE_PLUGIN)
	$(SPIKE_COMMAND) --device=am_protosoc,0 "$<"
else ifeq ($(PLATFORM),fpga)
$(BUILD_DIR)/gba-next-bench.bin: $(GBN_BENCH_TARGET)
	$(OBJCOPY) -O binary "$<" "$@"
.PHONY: run-gba-next-bench
run-gba-next-bench: $(BUILD_DIR)/gba-next-bench.bin $(LOADER).bin
	$(PYTHON) -u am/tools/run_fpga.py --protosoc "$(PROTOSOC)" --port "$(PORT)" \
		--loader "$(LOADER).bin" --app "$<" --baud $(BAUD) \
		--watch $(WATCH) --log "$(BUILD_DIR)/gba-next-fpga.log"
endif

-include $(GBN_BENCH_OBJECT:.o=.d)

# Interactive independent-core player; no benchmark CRC/pacing conflation.
GBN_PLAYER_FRAMES ?= 0
GBN_PLAYER_AUDIO ?= 1
GBN_PLAYER_TARGET = $(BUILD_DIR)/gba-next-player$(if $(filter native,$(PLATFORM)),,.elf)
GBN_PLAYER_OBJECT = $(BUILD_DIR)/src/platform/gba-next/player.o
$(BUILD_DIR)/.gba-next-player-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(GBN_PLAYER_FRAMES)' '$(GBN_PLAYER_AUDIO)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(GBN_PLAYER_OBJECT): src/platform/gba-next/player.c $(BUILD_DIR)/.gba-next-player-config Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
	@mkdir -p "$(@D)"
	$(CC) $(GBN_CPPFLAGS) -Iam/include $(AM_RUNNER_FLAGS) $(ROM_EMBED_CPPFLAGS) $(CPPFLAGS) \
		-DGBN_PLAYER_FRAMES=$(GBN_PLAYER_FRAMES) -DGBN_PLAYER_AUDIO=$(GBN_PLAYER_AUDIO) \
		$(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(GBN_PLAYER_TARGET): $(GBN_PLAYER_OBJECT) $(GBN_ROM_OBJECT) $(GBN_LIBRARY) $(AM_LIBRARY) $(BOOT_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PLATFORM_CFLAGS) $(CFLAGS) $(LTO) $(LDFLAGS) $(PLATFORM_LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(GBN_ROM_OBJECT) $(GBN_PLAYER_OBJECT) \
		-Wl,--start-group $(GBN_LIBRARY) $(AM_LIBRARY) $(PLATFORM_LIBS) $(LDLIBS) -Wl,--end-group

.PHONY: gba-next-player run-gba-next-player
gba-next-player: $(GBN_PLAYER_TARGET)
ifeq ($(PLATFORM),native)
run-gba-next-player: $(GBN_PLAYER_TARGET)
	"$(abspath $<)"
else ifneq ($(filter $(PLATFORM),spike spike_zve32x),)
.PHONY: run-gba-next
run-gba-next run-gba-next-player: $(GBN_PLAYER_TARGET) $(SPIKE_PLUGIN)
	$(SPIKE_COMMAND) --device=am_protosoc,0 --device="am_media,GBA next (Spike)" "$<"
endif
-include $(GBN_PLAYER_OBJECT:.o=.d)

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
		'$(LDFLAGS) $(LDLIBS) $(PLATFORM_LIBS)' '$(AM_CPPFLAGS)' 'GBN_DISPLAY=$(GBN_DISPLAY)' '$(GBN_CPPFLAGS)' 'RISCV_ISA=$(RISCV_ISA)' > "$@.tmp"
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

ifeq ($(PLATFORM),native)
$(BUILD_DIR)/gba-memory-test: am/test/gba-memory.c src/gba/memory.c $(CORE_LIBRARY) Makefile $(BUILD_DIR)/.build-config
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) \
		-MMD -MP -MF "$@.d" -MT "$@" $< $(CORE_LIBRARY) $(LDFLAGS) $(LDLIBS) -lm -o "$@"

-include $(BUILD_DIR)/gba-memory-test.d

.PHONY: test-gba-memory
test-gba-memory: $(BUILD_DIR)/gba-memory-test
	"$(abspath $<)"

$(BUILD_DIR)/gba-idle-test: am/test/gba-idle.c $(CORE_LIBRARY) Makefile $(BUILD_DIR)/.build-config
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) \
		-MMD -MP -MF "$@.d" -MT "$@" $< $(CORE_LIBRARY) $(LDFLAGS) $(LDLIBS) -lm -o "$@"

-include $(BUILD_DIR)/gba-idle-test.d

.PHONY: test-gba-idle
test-gba-idle: $(BUILD_DIR)/gba-idle-test
	"$(abspath $<)"

$(BUILD_DIR)/gba-rgb-test: am/test/gba-rgb.c $(CORE_LIBRARY) Makefile $(BUILD_DIR)/.build-config
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) \
		-MMD -MP -MF "$@.d" -MT "$@" $< $(CORE_LIBRARY) $(LDFLAGS) $(LDLIBS) -lm -o "$@"

-include $(BUILD_DIR)/gba-rgb-test.d

.PHONY: test-gba-rgb
test-gba-rgb: $(BUILD_DIR)/gba-rgb-test
	"$(abspath $<)" $(RGB_TEST_ROMS)

$(BUILD_DIR)/gba-next-test: am/test/gba-next.c $(GBN_LIBRARY) $(CORE_LIBRARY) Makefile $(BUILD_DIR)/.build-config
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) \
		-MMD -MP -MF "$@.d" -MT "$@" $< $(GBN_LIBRARY) $(CORE_LIBRARY) $(LDFLAGS) $(LDLIBS) -lm -o "$@"

-include $(BUILD_DIR)/gba-next-test.d

.PHONY: test-gba-next
test-gba-next: $(BUILD_DIR)/gba-next-test
	"$(abspath $<)"

$(BUILD_DIR)/gba-next-ppu-test: am/test/gba-next-ppu.c $(GBN_LIBRARY) $(CORE_LIBRARY) Makefile $(BUILD_DIR)/.build-config
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) \
		-MMD -MP -MF "$@.d" -MT "$@" $< $(GBN_LIBRARY) $(CORE_LIBRARY) $(LDFLAGS) $(LDLIBS) -lm -o "$@"

-include $(BUILD_DIR)/gba-next-ppu-test.d

.PHONY: test-gba-next-ppu
test-gba-next-ppu: $(BUILD_DIR)/gba-next-ppu-test
	"$(abspath $<)"

$(BUILD_DIR)/gba-next-run: src/platform/gba-next/main.c $(GBN_LIBRARY) Makefile $(BUILD_DIR)/.build-config
	$(CC) $(GBN_CPPFLAGS) $(CPPFLAGS) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) $(if $(filter 1,$(GBN_DISPLAY)),-DGBN_SDL $(SDL_CFLAGS)) \
		-MMD -MP -MF "$@.d" -MT "$@" $< $(GBN_LIBRARY) $(LDFLAGS) $(LDLIBS) $(if $(filter 1,$(GBN_DISPLAY)),$(SDL_LIBS)) -o "$@"

-include $(BUILD_DIR)/gba-next-run.d

.PHONY: run-gba-next
run-gba-next: $(BUILD_DIR)/gba-next-run
	"$(abspath $<)" $(ARGS) "$(ROM_PATH)"
endif

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
