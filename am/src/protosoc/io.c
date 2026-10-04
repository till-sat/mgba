/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include "platform.h"
#ifdef AM_SPIKE
#include "../../sim/media.h"
#endif

static uint64_t started;
static unsigned frequency;
static struct am_caps caps;
static const char* error = "";
static bool initialized;
#ifdef AM_SPIKE
static bool media;
static void write32(uintptr_t address, uint32_t value) {
	*(volatile uint32_t*) address = value;
}
static void publish(void) { __asm__ volatile("fence iorw, iorw" ::: "memory"); }
#endif

static uint32_t read32(uintptr_t address) {
	return *(volatile uint32_t*) address;
}

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
	unsigned divisor = frequency / (16 * 115200);
	if (!divisor) divisor = 1;
	uart[3] = 0x80;
	uart[0] = divisor;
	uart[1] = divisor >> 8;
	uart[3] = 0x03;
	uart[1] = 0;
	uart[2] = 0x07;
	started = am_platform_ticks();
}

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
#ifdef AM_SPIKE
		if ((config->video && (!config->width || !config->height ||
		     config->width > AM_SIM_MAX_WIDTH || config->height > AM_SIM_MAX_HEIGHT)) ||
		    (config->audio && !config->audio_rate)) {
			error = "Invalid simulation media configuration";
			return false;
		}
		if (read32(AM_SIM_BASE + AM_SIM_ID) != AM_SIM_MAGIC) {
			error = "Unsupported simulation media device";
			return false;
		}
		write32(AM_SIM_BASE + AM_SIM_WIDTH, config->width);
		write32(AM_SIM_BASE + AM_SIM_HEIGHT, config->height);
		write32(AM_SIM_BASE + AM_SIM_AUDIO_RATE, config->audio_rate);
		publish();
		write32(AM_SIM_BASE + AM_SIM_CONTROL,
		        (config->video ? AM_SIM_VIDEO : 0) | (config->audio ? AM_SIM_AUDIO : 0));
		if (read32(AM_SIM_BASE + AM_SIM_STATUS) & AM_SIM_ERROR) {
			write32(AM_SIM_BASE + AM_SIM_CONTROL, 0);
			error = "Could not open simulation media; check the host diagnostic";
			return false;
		}
		media = true;
		caps.video = config->video;
		caps.audio = config->audio;
		caps.audio_rate = read32(AM_SIM_BASE + AM_SIM_AUDIO_RATE);
		caps.audio_capacity = read32(AM_SIM_BASE + AM_SIM_AUDIO_CAP);
#else
		error = "This proto-soc configuration has no display or audio device";
		return false;
#endif
	}
	caps.buttons = true;
	started = am_platform_ticks();
	initialized = true;
	return true;
}

void am_shutdown(void) {
#ifdef AM_SPIKE
	if (media) write32(AM_SIM_BASE + AM_SIM_CONTROL, 0);
	media = false;
#endif
	caps = (struct am_caps) {0};
	initialized = false;
}
const struct am_caps* am_capabilities(void) { return &caps; }
const char* am_error(void) { return error; }

struct am_input am_input_read(void) {
	/* Prototype wiring: GPIO[9:0] matches am_button, active high.
	 * Physical board wiring and debounce remain board-specific work. */
	struct am_input input = { .buttons = caps.buttons ? read32(AM_SOC_GPIO + 4) & 0x3ff : 0 };
#ifdef AM_SPIKE
	input.quit = media && read32(AM_SIM_BASE + AM_SIM_QUIT);
#endif
	return input;
}

uint64_t am_uptime_us(void) {
	if (!initialized) return 0;
#ifdef AM_SPIKE
	if (media) {
		uint32_t lo = read32(AM_SIM_BASE + AM_SIM_TIME_LO);
		uint32_t hi = read32(AM_SIM_BASE + AM_SIM_TIME_HI);
		return ((uint64_t) hi << 32) | lo;
	}
#endif
	uint64_t ticks = am_platform_ticks() - started;
	if (!frequency) return 0;
	return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

void am_sleep_us(uint64_t duration) {
	if (!initialized) return;
#ifdef AM_SPIKE
	if (media) {
		while (duration) {
			unsigned step = duration > 10000 ? 10000 : duration;
			write32(AM_SIM_BASE + AM_SIM_SLEEP_US, step);
			duration -= step;
			if (am_input_read().quit) break;
		}
		return;
	}
#endif
	uint64_t start = am_uptime_us();
	while (am_uptime_us() - start < duration) {}
}

bool am_video_present(const uint32_t* pixels, unsigned width, unsigned height, size_t stride) {
#ifdef AM_SPIKE
	if (!caps.video || !pixels || !width || !height || stride < width ||
	    width > AM_SIM_MAX_WIDTH || height > AM_SIM_MAX_HEIGHT) {
		error = "Invalid frame or unavailable simulation display";
		return false;
	}
	volatile uint32_t* framebuffer = (volatile uint32_t*) (AM_SIM_BASE + AM_SIM_FB);
	for (unsigned y = 0; y < height; ++y) {
		for (unsigned x = 0; x < width; ++x) framebuffer[y * width + x] = pixels[y * stride + x];
	}
	write32(AM_SIM_BASE + AM_SIM_WIDTH, width);
	write32(AM_SIM_BASE + AM_SIM_HEIGHT, height);
	publish();
	write32(AM_SIM_BASE + AM_SIM_PRESENT, 1);
	if (read32(AM_SIM_BASE + AM_SIM_STATUS) & AM_SIM_ERROR) {
		error = "Simulation display failed; check the host diagnostic";
		return false;
	}
	return true;
#else
	(void) pixels; (void) width; (void) height; (void) stride;
	error = "Display is unavailable";
	return false;
#endif
}

size_t am_audio_write(const int16_t* samples, size_t frames) {
#ifdef AM_SPIKE
	if (!caps.audio || !samples || !frames) return 0;
	if (frames > AM_SIM_PCM_FRAMES) frames = AM_SIM_PCM_FRAMES;
	volatile uint32_t* pcm = (volatile uint32_t*) (AM_SIM_BASE + AM_SIM_PCM);
	for (size_t i = 0; i < frames; ++i) {
		pcm[i] = (uint16_t) samples[i * 2] | ((uint32_t) (uint16_t) samples[i * 2 + 1] << 16);
	}
	publish();
	write32(AM_SIM_BASE + AM_SIM_AUDIO_SUBMIT, frames);
	if (read32(AM_SIM_BASE + AM_SIM_STATUS) & AM_SIM_ERROR) {
		error = "Simulation audio failed; check the host diagnostic";
		return 0;
	}
	return read32(AM_SIM_BASE + AM_SIM_AUDIO_WRITTEN);
#else
	(void) samples; (void) frames;
	return 0;
#endif
}
size_t am_audio_queued(void) {
#ifdef AM_SPIKE
	if (caps.audio) return read32(AM_SIM_BASE + AM_SIM_AUDIO_QUEUED);
#endif
	return 0;
}
