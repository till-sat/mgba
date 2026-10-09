# SPDX-License-Identifier: MPL-2.0
PLATFORM_MAKEFILE += am/platform/rv32.mk
CROSS ?= riscv64-unknown-elf-
CC := $(CROSS)gcc
AR := $(CROSS)ar
OBJCOPY := $(CROSS)objcopy
HOST_CXX ?= c++
HOST_CC ?= cc
HOST_SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
HOST_SDL_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
BUILD_DIR := build/$(PLATFORM)
TARGET := $(BUILD_DIR)/mgba.elf
CORE_LIBRARY := $(BUILD_DIR)/libmgba.a
AM_LIBRARY := $(BUILD_DIR)/libam.a
NEWLIB_ROOT ?= $(abspath build/newlib)
NEWLIB_PREFIX := $(NEWLIB_ROOT)/install/riscv64-unknown-elf
RUNTIME_READY := $(NEWLIB_ROOT)/.ready
RISCV_ISA ?= rv32im_zicsr_zifencei_zicbom
# Ordinary C/LTO can stay scalar while explicit kernels use the runtime ISA.
RISCV_C_ISA ?= $(RISCV_ISA)
RISCV_ABI ?= ilp32
# A scalar libc may be shared by scalar/vector application comparisons.
NEWLIB_ISA ?= $(RISCV_ISA)
HEADLESS ?= 0
AUDIO ?= 1
FRAMES ?= $(if $(filter 1,$(HEADLESS)),120,0)
GPIO ?= 0
BENCHMARK ?= 0
WARMUP ?= 30
ROM_EMBED_FLAGS ?=
ROM_EMBED_CPPFLAGS ?=

PLATFORM_CFLAGS := -march=$(RISCV_C_ISA) -mabi=$(RISCV_ABI) -mstrict-align -mcmodel=medany \
                   -msmall-data-limit=0 -ffreestanding -ffunction-sections -fdata-sections
ifneq ($(findstring zve,$(RISCV_ISA)),)
PLATFORM_CFLAGS += -DAM_RVV
endif
ifneq ($(findstring rv32gcv,$(RISCV_ISA)),)
PLATFORM_CFLAGS += -DAM_RVV
endif
PROJECT_CFLAGS += $(PLATFORM_CFLAGS)
# The RV32 Newlib headers use long-based fixed-width integer typedefs; the
# upstream core has established int format strings for these values.
PROJECT_CFLAGS += -Wno-format -Wno-sign-compare
PROJECT_CPPFLAGS += -DAM_BAREMETAL $(AM_RUNNER_FLAGS) -isystem $(NEWLIB_PREFIX)/include
PROJECT_CPPFLAGS += $(ROM_EMBED_CPPFLAGS)
AM_CPPFLAGS := -DAM_BAREMETAL $(AM_RUNNER_FLAGS) -isystem $(NEWLIB_PREFIX)/include
LINK_SCRIPT ?= am/src/riscv/link.ld
PLATFORM_LDFLAGS = -nostdlib -nostartfiles -static -Wl,--gc-sections,-T,$(LINK_SCRIPT),-Map,$@.map
PLATFORM_LIBS := -L$(NEWLIB_PREFIX)/lib -lc -lgcc
CORE_SOURCES := $(filter-out src/core/directories.c src/util/vfs/vfs-dirent.c src/util/vfs/vfs-fd.c src/util/audio-resampler.c src/util/interpolator.c,$(CORE_SOURCES))
FRONTEND_SOURCES := src/platform/am/embedded.c src/platform/am/player.c
AM_SOURCES ?= am/src/protosoc/io.c am/src/riscv/runtime.c
PROFILE ?= 0
PERF_COUNTERS ?= 0
ifeq ($(PERF_COUNTERS),1)
ifneq ($(PLATFORM):$(BENCHMARK):$(PROFILE),fpga:1:0)
$(error PERF_COUNTERS=1 requires PLATFORM=fpga BENCHMARK=1 PROFILE=0 and the diagnostic PMU bitstream)
endif
PROJECT_CPPFLAGS += -DAM_PERF_COUNTERS
endif
ifneq ($(filter $(PLATFORM),fpga spike spike_zve32x verilator),)
PROJECT_CPPFLAGS += -DAM_BENCH_COUNTERS
ifeq ($(PROFILE),1)
PROJECT_CPPFLAGS += -DAM_PROFILE_SAMPLING
AM_SOURCES += am/src/protosoc/profile.c
endif
else ifeq ($(PROFILE),1)
$(error PROFILE=1 requires a proto-soc platform: fpga, spike, spike_zve32x or verilator)
endif
ifneq ($(findstring -DAM_SIM_MEDIA,$(AM_RUNNER_FLAGS)),)
AM_SOURCES += am/src/riscv/sim-media.c
endif
BOOT_SOURCE ?= am/src/riscv/start.S
BOOT_OBJECT := $(BUILD_DIR)/am/src/riscv/start.o
ROM_OBJECT := $(BUILD_DIR)/rom.o

$(BUILD_DIR)/am/bench/gba-next-ppu.o: am/bench/gba-next-ppu.c Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
	@mkdir -p "$(@D)"
	$(CC) $(GBN_CPPFLAGS) -Isrc -Iam/include $(PROJECT_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR)/gba-next-ppu-bench.elf: $(BUILD_DIR)/am/bench/gba-next-ppu.o $(GBN_LIBRARY) $(AM_LIBRARY) $(BOOT_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PROJECT_CFLAGS) $(CFLAGS) $(LTO) $(PLATFORM_LDFLAGS) $(LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(BUILD_DIR)/am/bench/gba-next-ppu.o \
		-Wl,--start-group $(GBN_LIBRARY) $(AM_LIBRARY) $(PLATFORM_LIBS) -Wl,--end-group

.PHONY: gba-next-ppu-bench
gba-next-ppu-bench: $(BUILD_DIR)/gba-next-ppu-bench.elf
-include $(BUILD_DIR)/am/bench/gba-next-ppu.d

.PHONY: runtime check-rv32 test-runtime test-media
runtime: $(RUNTIME_READY)
$(RUNTIME_READY): am/tools/build_newlib.py FORCE
	$(PYTHON) am/tools/build_newlib.py --root "$(NEWLIB_ROOT)" --cross "$(CROSS)" \
		--march "$(NEWLIB_ISA)" --mabi "$(RISCV_ABI)"

$(BOOT_OBJECT): $(BOOT_SOURCE) $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
	@mkdir -p "$(@D)"
	$(CC) $(PLATFORM_CFLAGS) -c "$<" -o "$@"

$(BUILD_DIR)/.rom-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(abspath $(ROM_PATH))' '$(ROM_EMBED_FLAGS)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(BUILD_DIR)/.frame-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(FRAMES)' '$(HEADLESS)' '$(AUDIO)' '$(BENCHMARK)' '$(WARMUP)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(BUILD_DIR)/src/platform/am/embedded.o: src/platform/am/embedded.c $(BUILD_DIR)/.frame-config
	@mkdir -p "$(@D)"
	$(CC) $(PROJECT_CPPFLAGS) $(CPPFLAGS) -DAM_FRAME_LIMIT=$(FRAMES) -DAM_HEADLESS=$(HEADLESS) -DAM_AUDIO=$(AUDIO) \
		-DAM_BENCHMARK=$(BENCHMARK) -DAM_WARMUP=$(WARMUP) \
		$(PROJECT_CFLAGS) $(CFLAGS) $(LTO) -MMD -MP -c "$<" -o "$@"

$(ROM_OBJECT): $(ROM_PATH) am/tools/embed_rom.py $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config $(BUILD_DIR)/.rom-config
	@mkdir -p "$(@D)"
	$(PYTHON) am/tools/embed_rom.py "$(ROM_PATH)" "$(BUILD_DIR)/rom.S" $(ROM_EMBED_FLAGS)
	$(CC) $(PLATFORM_CFLAGS) -c "$(BUILD_DIR)/rom.S" -o "$@"

RV32_FLOAT_SYMBOLS = ' (__[a-z0-9_]*(df|sf)[0-9]+|__float[a-z0-9_]*|__fix[a-z0-9_]*f[a-z0-9_]*|__extend[a-z0-9_]*|__trunc[a-z0-9_]*|(sin|cos|exp|log|floor|sqrt|hypot)f?)$$'
check-rv32: $(TARGET)
	@if $(CROSS)nm "$<" | grep -Eq $(RV32_FLOAT_SYMBOLS); then \
		echo "RV32 image contains floating-point helpers." >&2; \
		$(CROSS)nm "$<" | grep -E $(RV32_FLOAT_SYMBOLS) >&2; \
		exit 1; \
	fi
	@echo "PASS: RV32 image has no floating-point helpers"
