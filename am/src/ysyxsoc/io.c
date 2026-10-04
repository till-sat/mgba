/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include "platform.h"
#include "../riscv/sim-platform.h"

static uint64_t started;
#ifndef AM_SIM_MEDIA
static struct am_caps caps;
static const char* error = "";
static bool initialized;
#endif

static uint32_t read32(uintptr_t address) {
	return *(volatile uint32_t*) address;
}

#ifndef AM_SIM_MEDIA
static void write32(uintptr_t address, uint32_t value) {
	*(volatile uint32_t*) address = value;
}

#endif

unsigned am_platform_hz(void) {
#ifdef AM_YSYXSOC_PROTO
	/* proto-core's local CLINT counts CPU clocks. */
	return AM_YSYXSOC_HZ;
#else
	return 1000000u;
#endif
}

uint64_t am_platform_ticks(void) {
	uint32_t hi, lo, again;
	do {
		hi = read32(AM_YSYXSOC_CLINT + 4);
		lo = read32(AM_YSYXSOC_CLINT);
		again = read32(AM_YSYXSOC_CLINT + 4);
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
}

void am_platform_putch(char ch) {
	volatile uint8_t* uart = (volatile uint8_t*) AM_YSYXSOC_UART;
	while (!(uart[5] & 0x20)) {}
	uart[0] = (uint8_t) ch;
}

void am_platform_boot(void) {
	volatile uint8_t* uart = (volatile uint8_t*) AM_YSYXSOC_UART;
	/* 16550 setup used by the ysyxSoC AM runtime (divisor = 1). */
	uart[3] = 0x80;
	uart[0] = 1;
	uart[1] = 0;
	uart[3] = 0x03;
	uart[1] = 0;
	uart[2] = 0x07;
	started = am_platform_ticks();
}

bool am_platform_valid(void) { return true; }
uint32_t am_platform_buttons(void) { return read32(AM_YSYXSOC_GPIO + 4) & 0x3ff; }

#ifndef AM_SIM_MEDIA
bool am_init(const struct am_config* config) {
	if (initialized) {
		error = "AM is already initialized";
		return false;
	}
	if (!config || (config->video && (!config->width || !config->height ||
		config->width > AM_YSYXSOC_FB_WIDTH || config->height > AM_YSYXSOC_FB_HEIGHT))) {
		error = "Invalid ysyxSoC display configuration";
		return false;
	}
	if (config->audio) {
		error = "The ysyxSoC platform has no audio device";
		return false;
	}
	error = "";
	caps = (struct am_caps) {
		.video = config->video,
		.buttons = true,
		.audio = false,
		.audio_rate = 0,
		.audio_capacity = 0,
	};
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
	/* The SoC GPIO block exposes the same ten active-high button bits as proto-soc. */
	struct am_input input = {0};
	if (caps.buttons) input.buttons = read32(AM_YSYXSOC_GPIO + 4) & 0x3ffu;
	return input;
}

uint64_t am_uptime_us(void) {
	if (!initialized) return 0;
	return am_platform_ticks() - started;
}

void am_sleep_us(uint64_t duration) {
	if (!initialized) return;
	uint64_t start = am_uptime_us();
	while (am_uptime_us() - start < duration) {}
}

bool am_video_present(const uint32_t* pixels, unsigned width, unsigned height, size_t stride) {
	if (!caps.video || !pixels || !width || !height || stride < width ||
		width > AM_YSYXSOC_FB_WIDTH || height > AM_YSYXSOC_FB_HEIGHT) {
		error = "Invalid ysyxSoC frame or unavailable display";
		return false;
	}
	volatile uint32_t* framebuffer = (volatile uint32_t*) AM_YSYXSOC_FB;
	for (unsigned y = 0; y < height; ++y) {
		for (unsigned x = 0; x < width; ++x) {
			framebuffer[y * AM_YSYXSOC_FB_WIDTH + x] = pixels[y * stride + x];
		}
	}
	__asm__ volatile("fence iorw, iorw" ::: "memory");
	write32(AM_YSYXSOC_VGACTL + 4, 1);
	return true;
}

size_t am_audio_write(const int16_t* samples, size_t frames) {
	(void) samples;
	(void) frames;
	return 0;
}

size_t am_audio_queued(void) { return 0; }

#endif
