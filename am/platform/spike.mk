# SPDX-License-Identifier: MPL-2.0
PLATFORM_MAKEFILE := am/platform/spike.mk
CROSS ?= riscv64-unknown-elf-
CC := $(CROSS)gcc
AR := $(CROSS)ar
HOST_CXX ?= c++
HOST_CC ?= cc
HOST_SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
HOST_SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
BUILD_DIR := build/spike
TARGET := $(BUILD_DIR)/mgba.elf
CORE_LIBRARY := $(BUILD_DIR)/libmgba.a
AM_LIBRARY := $(BUILD_DIR)/libam.a
NEWLIB_ROOT ?= $(abspath build/newlib)
NEWLIB_PREFIX := $(NEWLIB_ROOT)/install/riscv64-unknown-elf
RUNTIME_READY := $(NEWLIB_ROOT)/.ready
SPIKE_PREFIX ?= /home/tillsat/tools/spike-20feb9c2
SPIKE := $(SPIKE_PREFIX)/bin/spike
SPIKE_PLUGIN := $(BUILD_DIR)/protosoc.so
RISCV_ISA := rv32im_zicsr_zifencei_zicbom
HEADLESS ?= 0
AUDIO ?= 1
FRAMES ?= $(if $(filter 1,$(HEADLESS)),120,0)
GPIO ?= 0

PLATFORM_CFLAGS := -march=$(RISCV_ISA) -mabi=ilp32 -mstrict-align -mcmodel=medany \
                   -msmall-data-limit=0 -ffreestanding -ffunction-sections -fdata-sections
PROJECT_CFLAGS += $(PLATFORM_CFLAGS)
# The RV32 Newlib headers use long-based fixed-width integer typedefs; the
# upstream core has established int format strings for these values.
PROJECT_CFLAGS += -Wno-format -Wno-sign-compare
PROJECT_CPPFLAGS += -DAM_BAREMETAL -isystem $(NEWLIB_PREFIX)/include
AM_CPPFLAGS := -DAM_BAREMETAL -DAM_SPIKE -isystem $(NEWLIB_PREFIX)/include
LINK_SCRIPT := am/src/riscv/link.ld
PLATFORM_LDFLAGS = -nostdlib -nostartfiles -static -Wl,--gc-sections,-T,$(LINK_SCRIPT),-Map,$@.map
PLATFORM_LIBS := -L$(NEWLIB_PREFIX)/lib -lc -lgcc
CORE_SOURCES := $(filter-out src/core/directories.c src/util/vfs/vfs-dirent.c src/util/vfs/vfs-fd.c src/util/audio-resampler.c src/util/interpolator.c,$(CORE_SOURCES))
FRONTEND_SOURCES := src/platform/am/embedded.c src/platform/am/player.c
AM_SOURCES := am/src/protosoc/io.c am/src/riscv/runtime.c
BOOT_OBJECT := $(BUILD_DIR)/am/src/riscv/start.o
ROM_OBJECT := $(BUILD_DIR)/rom.o

.PHONY: runtime spike-deps check-rv32 test-runtime test-media
runtime: $(RUNTIME_READY)
$(RUNTIME_READY): am/tools/build_newlib.py FORCE
	$(PYTHON) am/tools/build_newlib.py --root "$(NEWLIB_ROOT)" --cross "$(CROSS)"

$(BOOT_OBJECT): am/src/riscv/start.S $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
	@mkdir -p "$(@D)"
	$(CC) $(PLATFORM_CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/.rom-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(abspath $(ROM))' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(BUILD_DIR)/.frame-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(FRAMES)' '$(HEADLESS)' '$(AUDIO)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(BUILD_DIR)/src/platform/am/embedded.o: src/platform/am/embedded.c $(BUILD_DIR)/.frame-config
	@mkdir -p "$(@D)"
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) -DAM_FRAME_LIMIT=$(FRAMES) -DAM_HEADLESS=$(HEADLESS) -DAM_AUDIO=$(AUDIO) \
		$(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(ROM_OBJECT): $(ROM) am/tools/embed_rom.py $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config $(BUILD_DIR)/.rom-config
	@mkdir -p "$(@D)"
	$(PYTHON) am/tools/embed_rom.py "$(ROM)" "$(BUILD_DIR)/rom.S"
	$(CC) $(PLATFORM_CFLAGS) -c "$(BUILD_DIR)/rom.S" -o "$@"

spike-deps:
	@test -x "$(SPIKE)" -a -f "$(SPIKE_PREFIX)/include/riscv/abstract_device.h" || \
		{ echo "Set SPIKE_PREFIX to the Spike install used by proto-core." >&2; exit 1; }
	@$(PKG_CONFIG) --exists sdl2 || { echo "SDL2 development files are required for simulated media." >&2; exit 1; }

$(BUILD_DIR)/.spike-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(SPIKE_PREFIX)' '$(HOST_CXX)' '$(HOST_CC)' '$(HOST_SDL_CFLAGS)' '$(HOST_SDL_LIBS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(BUILD_DIR)/host/native.o: am/src/native/native.c am/include/am.h $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.spike-config | spike-deps
	@mkdir -p "$(@D)"
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -fPIC -Iam/include $(HOST_SDL_CFLAGS) -c "$<" -o "$@"

$(SPIKE_PLUGIN): am/sim/protosoc.cc am/sim/media.cc am/sim/media.h am/src/protosoc/platform.h am/include/am.h $(BUILD_DIR)/host/native.o $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.spike-config | spike-deps
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -fPIC -shared -I"$(SPIKE_PREFIX)/include" -Iam/include $(HOST_SDL_CFLAGS) \
		am/sim/protosoc.cc am/sim/media.cc $(BUILD_DIR)/host/native.o -o "$@" \
		-L"$(SPIKE_PREFIX)/lib" -Wl,-rpath,"$(SPIKE_PREFIX)/lib" -lriscv $(HOST_SDL_LIBS)

SPIKE_COMMAND = "$(SPIKE)" --isa=$(RISCV_ISA) --priv=m \
                -m0xa0000000:0x04000000 --extlib="$(abspath $(SPIKE_PLUGIN))"
RV32_FLOAT_SYMBOLS = ' (__[a-z0-9_]*(df|sf)[0-9]+|__float[a-z0-9_]*|__fix[a-z0-9_]*f[a-z0-9_]*|__extend[a-z0-9_]*|__trunc[a-z0-9_]*|(sin|cos|exp|log|floor|sqrt|hypot)f?)$$'
check-rv32: $(TARGET)
	@if $(CROSS)nm "$<" | grep -Eq $(RV32_FLOAT_SYMBOLS); then \
		echo "RV32 image contains floating-point helpers." >&2; \
		$(CROSS)nm "$<" | grep -E $(RV32_FLOAT_SYMBOLS) >&2; \
		exit 1; \
	fi
	@echo "PASS: RV32 image has no floating-point helpers"

run: $(TARGET) $(SPIKE_PLUGIN) check-rv32
	$(SPIKE_COMMAND) --device=am_protosoc,$(GPIO) --device=am_media "$(TARGET)"

$(BUILD_DIR)/am/test/riscv.o: Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
$(BUILD_DIR)/runtime-test.elf: $(BUILD_DIR)/am/test/riscv.o $(AM_LIBRARY) $(BOOT_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PLATFORM_CFLAGS) $(CFLAGS) $(LTO) $(LDFLAGS) $(PLATFORM_LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(BUILD_DIR)/am/test/riscv.o \
		-Wl,--start-group $(AM_LIBRARY) $(PLATFORM_LIBS) -Wl,--end-group

test-runtime: $(BUILD_DIR)/runtime-test.elf $(SPIKE_PLUGIN)
	timeout 30 $(SPIKE_COMMAND) --device=am_protosoc,0x310 "$(BUILD_DIR)/runtime-test.elf"

test-am: test-runtime

$(BUILD_DIR)/am/test/media.o: Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
$(BUILD_DIR)/media-test.elf: $(BUILD_DIR)/am/test/media.o $(AM_LIBRARY) $(BOOT_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PLATFORM_CFLAGS) $(CFLAGS) $(LTO) $(LDFLAGS) $(PLATFORM_LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(BUILD_DIR)/am/test/media.o \
		-Wl,--start-group $(AM_LIBRARY) $(PLATFORM_LIBS) -Wl,--end-group

$(BUILD_DIR)/host/media-probe.so: am/test/media-probe.c $(BUILD_DIR)/.spike-config | spike-deps
	@mkdir -p "$(@D)"
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -fPIC -shared $(HOST_SDL_CFLAGS) "$<" -o "$@" $(HOST_SDL_LIBS) -ldl

test-media: $(BUILD_DIR)/media-test.elf $(SPIKE_PLUGIN) $(BUILD_DIR)/host/media-probe.so
	timeout 30 env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy LD_PRELOAD="$(abspath $(BUILD_DIR)/host/media-probe.so)" \
		$(SPIKE_COMMAND) --device=am_protosoc,0 --device=am_media "$(BUILD_DIR)/media-test.elf" > "$(BUILD_DIR)/media-test.log" 2>&1
	@cat "$(BUILD_DIR)/media-test.log"
	@$(PYTHON) -c 'from pathlib import Path; log = Path("$(BUILD_DIR)/media-test.log").read_text(); assert "MEDIA_PROBE PASS" in log and "MEDIA_PROBE FAIL" not in log, log'

test: check-rv32 test-runtime test-media
	+$(PYTHON) -u src/platform/am/test/spike.py --make "$(MAKE)" \
		--build-dir "$(BUILD_DIR)" --spike-prefix "$(SPIKE_PREFIX)" \
		--cross "$(CROSS)" --newlib-root "$(NEWLIB_ROOT)" --isa "$(RISCV_ISA)"
