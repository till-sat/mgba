# SPDX-License-Identifier: MPL-2.0
PLATFORM_MAKEFILE := am/platform/ysyxsoc.mk
AM_RUNNER_FLAGS := -DAM_YSYXSOC
YSYXSOC_CORE ?= proto
BRANCH ?= quad-issue-rvv
CPU_MHZ ?= 200
DEV_MHZ ?= 100
ICACHE ?= 16
DCACHE ?= 16
JOBS ?= 4

ifeq ($(strip $(YSYX_NPC)),)
$(error PLATFORM=ysyxsoc requires YSYX_NPC=/path/to/your-core/soc/ysyxsoc)
endif
ifeq ($(strip $(YSYX_SOC)),)
$(error PLATFORM=ysyxsoc requires YSYX_SOC=/path/to/ysyx/ysyxSoC)
endif
YSYX_ROOT := $(abspath $(YSYX_SOC)/..)

ifeq ($(YSYXSOC_CORE),proto)
AM_RUNNER_FLAGS += -DAM_YSYXSOC_PROTO -DAM_SIM_MEDIA -DAM_YSYXSOC_HZ=$(CPU_MHZ)000000
HEADLESS ?= 0
AUDIO ?= 1
AM_SOURCES := am/src/ysyxsoc/io.c am/src/ysyxsoc/runtime.c
else ifeq ($(YSYXSOC_CORE),rv32e)

# The reference ysyxSoC/NPC core is RV32E.  Its standard AM platform uses
# the same ISA and ABI for applications and for the Flash bootloader.
RISCV_ISA := rv32em_zicsr_zifencei
RISCV_ABI := ilp32e
NEWLIB_ROOT ?= $(abspath build/newlib-rv32e)
HEADLESS ?= 1
AUDIO ?= 0
FRAMES ?= $(if $(filter 1,$(HEADLESS)),120,0)

AM_SOURCES := am/src/ysyxsoc/io.c am/src/ysyxsoc/runtime.c am/src/ysyxsoc/heap.c
else
$(error YSYXSOC_CORE must be proto or rv32e)
endif
YSYXSOC_XIP ?= 0
FRAMES ?= $(if $(filter 1,$(HEADLESS)),2,0)
ifeq ($(YSYXSOC_XIP),1)
LINK_SCRIPT := am/src/ysyxsoc/xip.ld
BOOT_SOURCE := am/src/ysyxsoc/xip-start.S
else
LINK_SCRIPT := am/src/ysyxsoc/link.ld
ROM_EMBED_FLAGS := --omit-ff-tail
ROM_EMBED_CPPFLAGS := -DAM_ROM_OMIT_FF_TAIL
endif

include am/platform/rv32.mk
ifeq ($(YSYXSOC_CORE),proto)
include am/platform/rtl-media.mk
SIMBUILD ?= $(abspath $(BUILD_DIR)/soc-$(BRANCH))
SOC_RUNNER := $(SIMBUILD)/Vsoc
MAX_CYCLES ?= $(if $(filter 0,$(FRAMES)),0,500000000)
PROGRESS ?= 10000000
endif

# Keep the shared object cache honest when switching between the standard
# FSBL/SSBL image and the direct-Flash XIP bring-up mode.
PROJECT_CFLAGS += -DAM_YSYXSOC_XIP=$(YSYXSOC_XIP)

YSYXSOC_NPC_BIN ?= $(YSYX_ROOT)/npc/multi/build/test
YSYXSOC_BOOTLOADER ?= $(YSYX_ROOT)/am-kernels/tests/bootloader
YSYXSOC_BOOTLOADER_BIN := $(abspath $(BUILD_DIR)/bootloader-$(YSYXSOC_CORE)/bootloader.bin)
YSYXSOC_APP_BIN := $(BUILD_DIR)/mgba.bin
YSYXSOC_FLASH_IMAGE := $(BUILD_DIR)/mgba-flash.bin
YSYXSOC_FLASH_APP_OFF ?= 16384
YSYXSOC_FLASH_SIZE ?= 0x1000000

all: $(YSYXSOC_FLASH_IMAGE)
.PHONY: ysyxsoc-image sim
ysyxsoc-image: $(YSYXSOC_FLASH_IMAGE)

$(BUILD_DIR)/.ysyxsoc-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(YSYX_SOC)' '$(YSYXSOC_NPC_BIN)' '$(YSYXSOC_BOOTLOADER)' '$(YSYXSOC_FLASH_APP_OFF)' '$(YSYXSOC_XIP)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(YSYXSOC_APP_BIN): $(TARGET) $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.ysyxsoc-config
	$(OBJCOPY) -S -O binary "$<" "$@"

# Keep this bootloader's ABI and build directory isolated from ysyx's RV32E work.
$(YSYXSOC_BOOTLOADER_BIN): $(wildcard $(YSYXSOC_BOOTLOADER)/*.[cS] $(YSYXSOC_BOOTLOADER)/*.ld) $(YSYXSOC_BOOTLOADER)/Makefile
	$(MAKE) -C "$(YSYXSOC_BOOTLOADER)" APP_BASE=0xa0000000 \
		BUILD="$(abspath $(BUILD_DIR)/bootloader-$(YSYXSOC_CORE))" CROSS="$(CROSS)" \
		ARCH_FLAGS="-march=$(RISCV_ISA) -mabi=$(RISCV_ABI)"

ifeq ($(YSYXSOC_XIP),1)
$(YSYXSOC_FLASH_IMAGE): $(YSYXSOC_APP_BIN) am/tools/ysyxsoc_image.py
	$(PYTHON) am/tools/ysyxsoc_image.py --app "$<" --out "$@" --xip --limit $(YSYXSOC_FLASH_SIZE)
else
$(YSYXSOC_FLASH_IMAGE): $(YSYXSOC_APP_BIN) $(YSYXSOC_BOOTLOADER_BIN) am/tools/ysyxsoc_image.py
	$(PYTHON) am/tools/ysyxsoc_image.py --app "$<" --boot "$(YSYXSOC_BOOTLOADER_BIN)" \
		--offset $(YSYXSOC_FLASH_APP_OFF) --limit $(YSYXSOC_FLASH_SIZE) --out "$@"
endif

ifeq ($(YSYXSOC_CORE),proto)
$(BUILD_DIR)/media-test-flash.bin: $(BUILD_DIR)/media-test.bin $(YSYXSOC_BOOTLOADER_BIN) am/tools/ysyxsoc_image.py
	$(PYTHON) am/tools/ysyxsoc_image.py --app "$<" --boot "$(YSYXSOC_BOOTLOADER_BIN)" \
		--offset $(YSYXSOC_FLASH_APP_OFF) --limit $(YSYXSOC_FLASH_SIZE) --out "$@" $(if $(filter 1,$(YSYXSOC_XIP)),--xip,)

sim: $(RTL_NATIVE)
	+$(MAKE) -C "$(YSYX_NPC)" BUILD="$(SIMBUILD)" YSYXSOC="$(YSYX_SOC)" \
		ICACHE=$(ICACHE) DCACHE=$(DCACHE) CPU_MHZ=$(CPU_MHZ) DEV_MHZ=$(DEV_MHZ) JOBS=$(JOBS) \
		AM_MEDIA_ROOT="$(RTL_MEDIA_ROOT)" AM_MEDIA_NATIVE="$(RTL_NATIVE)" build

run: $(YSYXSOC_FLASH_IMAGE) check-rv32 sim
	"$(SOC_RUNNER)" "$(abspath $(YSYXSOC_FLASH_IMAGE))" -m $(MAX_CYCLES) -p $(PROGRESS)

test-ysyxsoc: test

test: test-interactive
else
.PHONY: ysyxsoc-deps ysyxsoc-image test-ysyxsoc
ysyxsoc-deps:
	@test -x "$(YSYXSOC_NPC_BIN)" || { echo "Set YSYXSOC_NPC_BIN to the built ysyxSoC NPC simulator." >&2; exit 1; }

ysyxsoc-image: $(YSYXSOC_FLASH_IMAGE)

run: $(YSYXSOC_FLASH_IMAGE) check-rv32 ysyxsoc-deps
	$(YSYXSOC_NPC_BIN) -b "$(YSYXSOC_FLASH_IMAGE)"

test-ysyxsoc: HEADLESS=1
test-ysyxsoc: AUDIO=0
test-ysyxsoc: FRAMES=1
test-ysyxsoc: $(YSYXSOC_FLASH_IMAGE) check-rv32 ysyxsoc-deps
	timeout 120 "$(YSYXSOC_NPC_BIN)" -b "$(YSYXSOC_FLASH_IMAGE)"

endif
