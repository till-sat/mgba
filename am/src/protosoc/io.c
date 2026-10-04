/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include "platform.h"
#include "../riscv/sim-platform.h"

static uint64_t started;
static unsigned frequency;
static uint32_t read32(uintptr_t address) { return *(volatile uint32_t*) address; }

unsigned am_platform_hz(void) {
	return read32(AM_SOC_SYSCTRL + 0x38);
}

uint64_t am_platform_ticks(void) {
	uint32_t hi, lo, again;
	do {
		hi = read32(AM_SOC_CLINT + 0xbffc);
		lo = read32(AM_SOC_CLINT + 0xbff8);
		again = read32(AM_SOC_CLINT + 0xbffc);
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
}

void am_platform_putch(char ch) {
	volatile uint8_t* uart = (volatile uint8_t*) AM_SOC_UART;
	while (!(uart[5] & 0x20)) {}
	uart[0] = (uint8_t) ch;
}

void am_platform_boot(void) {
	volatile uint8_t* uart = (volatile uint8_t*) AM_SOC_UART;
	frequency = am_platform_hz();
	while (!(uart[5] & 0x40)) {}
	unsigned divisor = (frequency + 8 * 115200) / (16 * 115200);
	if (!divisor) divisor = 1;
	uart[3] = 0x80;
	uart[0] = divisor;
	uart[1] = divisor >> 8;
	uart[3] = 0x03;
	uart[1] = 0;
	uart[2] = 0x07;
	started = am_platform_ticks();
}

bool am_platform_valid(void) {
	return read32(AM_SOC_SYSCTRL + 0x14) == AM_SOC_MAP_VERSION;
}
uint32_t am_platform_buttons(void) { return read32(AM_SOC_GPIO + 4) & 0x3ff; }

#ifndef AM_SIM_MEDIA
static struct am_caps caps;
static const char* error = "";
static bool initialized;

bool am_init(const struct am_config* config) {
	if (initialized) {
		error = "AM is already initialized";
		return false;
	}
	error = "";
	caps = (struct am_caps) {0};
	if (!config || read32(AM_SOC_SYSCTRL + 0x14) != AM_SOC_MAP_VERSION || !frequency) {
		error = "Unsupported proto-soc map or timer frequency";
		return false;
	}
	if (config->video || config->audio) {
		error = "This proto-soc configuration has no display or audio device";
		return false;
	}
	caps.buttons = true;
	started = am_platform_ticks();
	initialized = true;
	return true;
}

void am_shutdown(void) {
	caps = (struct am_caps) {0};
	initialized = false;
}
const struct am_caps* am_capabilities(void) { return &caps; }
const char* am_error(void) { return error; }

struct am_input am_input_read(void) {
	/* Prototype wiring: GPIO[9:0] matches am_button, active high.
	 * Physical board wiring and debounce remain board-specific work. */
	struct am_input input = { .buttons = caps.buttons ? read32(AM_SOC_GPIO + 4) & 0x3ff : 0 };
	return input;
}

uint64_t am_uptime_us(void) {
	if (!initialized) return 0;
	uint64_t ticks = am_platform_ticks() - started;
	if (!frequency) return 0;
	return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

void am_sleep_us(uint64_t duration) {
	if (!initialized) return;
	uint64_t start = am_uptime_us();
	while (am_uptime_us() - start < duration) {}
}

bool am_video_present(const uint32_t* pixels, unsigned width, unsigned height, size_t stride) {
	(void) pixels; (void) width; (void) height; (void) stride;
	error = "Display is unavailable";
	return false;
}

size_t am_audio_write(const int16_t* samples, size_t frames) {
	(void) samples; (void) frames;
	return 0;
}
size_t am_audio_queued(void) {
	return 0;
}

#endif
