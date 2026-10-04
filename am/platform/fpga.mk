# SPDX-License-Identifier: MPL-2.0
# Load a headless benchmark into volatile SDRAM through Boot ROM recovery.
PLATFORM_MAKEFILE := am/platform/fpga.mk
AM_RUNNER_FLAGS := -DAM_FPGA
HEADLESS ?= 1
AUDIO ?= 0
BENCHMARK ?= 1
FRAMES ?= 120
WARMUP ?= 30
include am/platform/rv32.mk

PROFILE ?= 0
ifeq ($(PROFILE),1)
PROJECT_CPPFLAGS += -DAM_PROFILE_SAMPLING
AM_SOURCES += am/src/protosoc/profile.c
endif

ifneq ($(HEADLESS):$(AUDIO):$(BENCHMARK),1:0:1)
$(error PLATFORM=fpga requires HEADLESS=1 AUDIO=0 BENCHMARK=1)
endif

PROTOSOC ?= /home/tillsat/proto-core/soc
PORT ?= /dev/ttyUSB0
BAUD ?= 500000
WATCH ?= 1800
LOADER := $(BUILD_DIR)/ram-loader
BOOT_SOURCES := $(PROTOSOC)/sw/boot/start.S $(PROTOSOC)/sw/boot/stage2.ld \
                $(PROTOSOC)/sw/boot/boot.h $(PROTOSOC)/sw/include/soc_map.h

all: $(BUILD_DIR)/mgba.bin $(LOADER).bin

$(BUILD_DIR)/mgba.bin: $(TARGET)
	$(CROSS)objcopy -O binary "$<" "$@"

$(LOADER).elf: am/src/protosoc/ram-loader.c $(BOOT_SOURCES) $(PLATFORM_MAKEFILE) | $(BUILD_DIR)
	$(CC) $(PLATFORM_CFLAGS) -Os -Wall -Wextra -Werror -nostdlib -nostartfiles \
		-I$(PROTOSOC)/sw/boot -I$(PROTOSOC)/sw/include \
		-Wl,--gc-sections,-T,$(PROTOSOC)/sw/boot/stage2.ld \
		$(PROTOSOC)/sw/boot/start.S $< -o "$@" -lgcc

$(LOADER).bin: $(LOADER).elf
	$(CROSS)objcopy -O binary "$<" "$@"

run: all
	$(PYTHON) -u am/tools/run_fpga.py --protosoc "$(PROTOSOC)" --port "$(PORT)" \
		--loader "$(LOADER).bin" --app "$(BUILD_DIR)/mgba.bin" --baud $(BAUD) \
		--watch $(WATCH) --log "$(BUILD_DIR)/fpga.log"

.PHONY: test
test:
	$(PYTHON) am/test/uart-runner.py --protosoc "$(PROTOSOC)"
