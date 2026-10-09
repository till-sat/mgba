# SPDX-License-Identifier: MPL-2.0
# Invoke from the repository root: make -f am/bench/peanut/Makefile PLATFORM=fpga
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
PLATFORM ?= native
BUILD_DIR ?= build/rv32/gb-comparison/peanut-$(PLATFORM)
ROM ?= cinema/gb/acid/dmg-acid2/test.gb
FRAMES ?= 120
WARMUP ?= 30
INPUT ?= 0
CROSS ?= riscv64-unknown-elf-
NEWLIB := $(abspath build/newlib/install/riscv64-unknown-elf)
CPPFLAGS += -Iam/bench/peanut/vendor -Iam/include -DBENCH_FRAMES=$(FRAMES) -DBENCH_WARMUP=$(WARMUP) -DBENCH_INPUT=$(INPUT)
CFLAGS := -std=c11 -O3 -g -DNDEBUG -fwrapv -Wall -Wextra -Wno-unused-parameter
SOURCES := am/bench/peanut/main.c
ifeq ($(PLATFORM),native)
CC := cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
LDFLAGS += -Wl,-z,noexecstack
else ifneq ($(filter $(PLATFORM),fpga spike),)
CC := $(CROSS)gcc
CPPFLAGS += -DAM_BAREMETAL $(if $(filter fpga,$(PLATFORM)),-DAM_FPGA,-DAM_SPIKE) -isystem $(NEWLIB)/include
CFLAGS += -march=rv32im_zicsr_zifencei_zicbom -mabi=ilp32 -mstrict-align -mcmodel=medany -msmall-data-limit=0 -ffreestanding -ffunction-sections -fdata-sections
SOURCES += am/src/protosoc/io.c am/src/riscv/runtime.c am/src/riscv/start.S
ifeq ($(MEMORY_PROBE),1)
CPPFLAGS += -DBENCH_MEMORY_PROBE
SOURCES += am/bench/peanut/memory-probe.c
endif
LDFLAGS += -nostdlib -nostartfiles -static -Wl,--gc-sections,-T,am/src/riscv/link.ld,-Map,$(BUILD_DIR)/peanut.map
LDLIBS += -L$(NEWLIB)/lib -lc -lgcc
else
$(error PLATFORM must be native, fpga or spike)
endif

.PHONY: all FORCE
all: $(BUILD_DIR)/peanut.elf $(if $(filter-out native,$(PLATFORM)),$(BUILD_DIR)/peanut.bin)
$(BUILD_DIR):
	mkdir -p "$@"
$(BUILD_DIR)/config: FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(CC)' '$(CPPFLAGS)' '$(CFLAGS)' '$(LDFLAGS)' '$(LDLIBS)' '$(abspath $(ROM))' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm $@.tmp
$(BUILD_DIR)/rom.S: $(ROM) $(BUILD_DIR)/config am/tools/embed_rom.py
	python3 am/tools/embed_rom.py "$(ROM)" "$@"
$(BUILD_DIR)/peanut.elf: $(SOURCES) $(BUILD_DIR)/rom.S $(BUILD_DIR)/config am/bench/peanut/vendor/peanut_gb.h am/bench/peanut/vendor/hedley.h am/src/riscv/link.ld
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(BUILD_DIR)/rom.S $(LDFLAGS) -o "$@" $(LDLIBS)
$(BUILD_DIR)/peanut.bin: $(BUILD_DIR)/peanut.elf
	$(CROSS)objcopy -O binary "$<" "$@"
