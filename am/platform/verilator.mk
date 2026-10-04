# SPDX-License-Identifier: MPL-2.0
# Boot mGBA through the unmodified proto-soc Boot ROM and NOR stage2.
PLATFORM_MAKEFILE := am/platform/verilator.mk
AM_RUNNER_FLAGS := -DAM_SIM_MEDIA
HEADLESS ?= 0
AUDIO ?= 1
FRAMES ?= $(if $(filter 1,$(HEADLESS)),2,0)
include am/platform/rv32.mk
include am/platform/rtl-media.mk
ifneq ($(GPIO),0)
$(error PLATFORM=verilator uses SDL keyboard input; GPIO must be 0)
endif

PROTOSOC ?= /home/tillsat/proto-core/soc
BRANCH ?= quad-issue-rvv
ICACHE ?= 16
DCACHE ?= 16
JOBS ?= 4
MAX_CYCLES ?= $(if $(filter 0,$(FRAMES)),0,500000000)
PROGRESS ?= 10000000
SIMBUILD ?= $(abspath $(BUILD_DIR)/soc-$(BRANCH))
SOC_RUNNER := $(SIMBUILD)/Vsoc
STAGE2 := $(PROTOSOC)/sw/boot/build/stage2.bin
BOOT_PACKER := $(PROTOSOC)/sw/tools/boot_image.py
FLASH_IMAGE := $(BUILD_DIR)/mgba-flash.bin

.PHONY: sim protosoc-deps
all: $(FLASH_IMAGE)

protosoc-deps:
	@test -f "$(PROTOSOC)/sim/Makefile" -a -f "$(BOOT_PACKER)" || \
		{ echo "Set PROTOSOC to the proto-core soc directory." >&2; exit 1; }

# Let the SoC's own Makefiles track its firmware, RTL, and cache configuration.
$(STAGE2): FORCE | protosoc-deps
	+$(MAKE) -C "$(PROTOSOC)/sw/boot"

$(BUILD_DIR)/mgba.bin: $(TARGET)
	$(CROSS)objcopy -O binary "$<" "$@"

$(FLASH_IMAGE): $(BUILD_DIR)/mgba.bin $(STAGE2) $(BOOT_PACKER)
	$(PYTHON) "$(BOOT_PACKER)" pack --stage2 "$(STAGE2)" --app "$<" \
		--app-load 0xa0000000 --output "$@"

$(BUILD_DIR)/media-test-flash.bin: $(BUILD_DIR)/media-test.bin $(STAGE2) $(BOOT_PACKER)
	$(PYTHON) "$(BOOT_PACKER)" pack --stage2 "$(STAGE2)" --app "$<" \
		--app-load 0xa0000000 --output "$@"

sim: $(RTL_NATIVE) | protosoc-deps
	+$(MAKE) -C "$(PROTOSOC)/sim" BRANCH=$(BRANCH) SIMBUILD="$(SIMBUILD)" \
		L1=cache ICACHE=$(ICACHE) DCACHE=$(DCACHE) JOBS=$(JOBS) \
		AM_MEDIA_ROOT="$(RTL_MEDIA_ROOT)" AM_MEDIA_NATIVE="$(RTL_NATIVE)" \
		FLASH_AW=24 SDRAM_AW=26 build

run: $(FLASH_IMAGE) sim check-rv32
	"$(SOC_RUNNER)" "$(abspath $(FLASH_IMAGE))" -m $(MAX_CYCLES) -p $(PROGRESS)

test: test-interactive
	+$(PYTHON) -u src/platform/am/test/verilator.py --make "$(MAKE)" \
		--build-dir "$(abspath $(BUILD_DIR))" --runner "$(SOC_RUNNER)" \
		--protosoc "$(PROTOSOC)" --cross "$(CROSS)" --newlib-root "$(NEWLIB_ROOT)" \
		--max-cycles $(if $(filter 0,$(MAX_CYCLES)),500000000,$(MAX_CYCLES))
