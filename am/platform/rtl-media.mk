# SPDX-License-Identifier: MPL-2.0
# Host SDL object shared by the two Verilator integrations.
PLATFORM_MAKEFILE += am/platform/rtl-media.mk
RTL_NATIVE := $(abspath $(BUILD_DIR)/host/native.o)
RTL_PROBE := $(abspath $(BUILD_DIR)/host/media-probe.so)
RTL_MEDIA_ROOT := $(abspath .)

$(RTL_NATIVE): am/src/native/native.c am/include/am.h am/platform/rtl-media.mk
	@mkdir -p "$(@D)"
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -fPIC -Iam/include $(HOST_SDL_CFLAGS) -c "$<" -o "$@"

$(RTL_PROBE): am/test/media-probe.c am/platform/rtl-media.mk
	@mkdir -p "$(@D)"
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -fPIC -shared $(HOST_SDL_CFLAGS) "$<" -o "$@" $(HOST_SDL_LIBS) -ldl

$(BUILD_DIR)/am/test/media.o: Makefile $(PLATFORM_MAKEFILE) $(BUILD_DIR)/.build-config | $(RUNTIME_READY)
$(BUILD_DIR)/media-test.elf: $(BUILD_DIR)/am/test/media.o $(AM_LIBRARY) $(BOOT_OBJECT) $(LINK_SCRIPT) $(RUNTIME_READY)
	$(CC) $(PLATFORM_CFLAGS) $(CFLAGS) $(LTO) $(LDFLAGS) $(PLATFORM_LDFLAGS) -o "$@" \
		$(BOOT_OBJECT) $(BUILD_DIR)/am/test/media.o \
		-Wl,--start-group $(AM_LIBRARY) $(PLATFORM_LIBS) -Wl,--end-group

$(BUILD_DIR)/media-test.bin: $(BUILD_DIR)/media-test.elf
	$(OBJCOPY) -O binary "$<" "$@"

test-media: $(BUILD_DIR)/media-test-flash.bin $(RTL_PROBE) sim
	timeout 900 env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy LD_PRELOAD="$(RTL_PROBE)" \
		"$(SOC_RUNNER)" "$(abspath $(BUILD_DIR)/media-test-flash.bin)" -m 500000000 -p 10000000 > "$(BUILD_DIR)/media-test.log" 2>&1
	@tail -8 "$(BUILD_DIR)/media-test.log"
	@$(PYTHON) -c 'from pathlib import Path; s=Path("$(BUILD_DIR)/media-test.log").read_text(); assert "MEDIA_PROBE PASS" in s and "MEDIA_PROBE FAIL" not in s and "PASS: MMIO" in s and "HIT GOOD TRAP" in s, s[-3000:]'

.PHONY: test-interactive
test-interactive: test-media
	+$(PYTHON) -u src/platform/am/test/rtl-interactive.py --platform $(PLATFORM) \
		--build-dir "$(abspath $(BUILD_DIR))" --runner "$(SOC_RUNNER)"
