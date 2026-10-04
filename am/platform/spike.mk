# SPDX-License-Identifier: MPL-2.0
PLATFORM_MAKEFILE := am/platform/spike.mk
AM_RUNNER_FLAGS := -DAM_SPIKE -DAM_SIM_MEDIA
include am/platform/rv32.mk
SPIKE_PREFIX ?= /home/tillsat/tools/spike-20feb9c2
SPIKE := $(SPIKE_PREFIX)/bin/spike
SPIKE_PLUGIN := $(BUILD_DIR)/protosoc.so

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

$(SPIKE_PLUGIN): am/sim/protosoc.cc am/sim/media.cc am/sim/media.h am/sim/media-device.h am/src/protosoc/platform.h am/include/am.h $(BUILD_DIR)/host/native.o $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.spike-config | spike-deps
	$(HOST_CXX) -std=c++20 -O2 -Wall -Wextra -fPIC -shared -I"$(SPIKE_PREFIX)/include" -Iam/include $(HOST_SDL_CFLAGS) \
		am/sim/protosoc.cc am/sim/media.cc $(BUILD_DIR)/host/native.o -o "$@" \
		-L"$(SPIKE_PREFIX)/lib" -Wl,-rpath,"$(SPIKE_PREFIX)/lib" -lriscv $(HOST_SDL_LIBS)

SPIKE_COMMAND = "$(SPIKE)" --isa=$(RISCV_ISA) --priv=m \
                -m0xa0000000:0x04000000 --extlib="$(abspath $(SPIKE_PLUGIN))"
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

.PHONY: test-interactive
test-interactive: test
	+$(PYTHON) -u src/platform/am/test/rtl-interactive.py --platform spike \
		--build-dir "$(abspath $(BUILD_DIR))" --spike-prefix "$(SPIKE_PREFIX)"
