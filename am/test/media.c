/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
	if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

static uint32_t pixels[256][259];
static int16_t samples[2048 * 2];

int main(void) {
	struct am_config config = { .width = 240, .height = 160, .video = true,
	                            .audio = true, .audio_rate = 44100 };
	CHECK(am_init(&config));
	CHECK(am_capabilities()->video && am_capabilities()->audio && am_capabilities()->buttons);
	CHECK(am_capabilities()->audio_rate > 0 && am_capabilities()->audio_capacity > 0);
	CHECK(!am_init(&config));
	for (unsigned y = 0; y < 256; ++y) {
		for (unsigned x = 0; x < 256; ++x) pixels[y][x] = (x << 16) | (y << 8) | (x ^ y);
	}
	/* The host test injects key events after each presentation. */
	for (unsigned key = 0; key < 10; ++key) {
		CHECK(am_video_present(&pixels[0][0], 240, 160, 259));
		CHECK(am_input_read().buttons == (1u << key));
		CHECK(am_video_present(&pixels[0][0], 240, 160, 259));
		CHECK(am_input_read().buttons == 0);
	}
	CHECK(am_video_present(&pixels[0][0], 240, 160, 259));
	CHECK(am_input_read().buttons == (AM_BUTTON_A | AM_BUTTON_B));
	CHECK(am_video_present(&pixels[0][0], 240, 160, 259));
	CHECK(am_input_read().buttons == 0);
	CHECK(am_video_present(&pixels[0][0], 240, 160, 259));
	CHECK(am_input_read().quit);
	CHECK(am_video_present(&pixels[0][0], 256, 224, 259));
	CHECK(!am_video_present(&pixels[0][0], 257, 224, 259));
	CHECK(!am_video_present(&pixels[0][0], 240, 160, 239));
	for (unsigned i = 0; i < 2048; ++i) {
		samples[i * 2] = (int) i * 31 - 32000;
		samples[i * 2 + 1] = -samples[i * 2];
	}
	/* The host probe pauses playback so a full queue is deterministic. */
	size_t capacity = am_capabilities()->audio_capacity;
	for (size_t written = 0; written < capacity;) {
		size_t count = capacity - written;
		if (count > 2048) count = 2048;
		CHECK(am_audio_write(samples, count) == count);
		written += count;
	}
	CHECK(am_audio_write(samples, 2048) == 0);
	CHECK(am_audio_queued() == capacity);
	uint64_t before = am_uptime_us();
	am_sleep_us(1000);
	CHECK(am_uptime_us() >= before + 1000);
	am_shutdown();
	CHECK(!am_capabilities()->video && !am_input_read().quit);
	config.video = false;
	config.width = config.height = 0;
	CHECK(am_init(&config));
	CHECK(!am_capabilities()->video && am_capabilities()->audio);
	am_shutdown();
	puts("PASS: MMIO video, stride, dimensions, audio, all GPIO buttons, focus and quit");
	return 0;
}
