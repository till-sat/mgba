/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
	if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

int main(void) {
	struct am_config config = {0};
	CHECK(am_init(&config));
	CHECK(!am_capabilities()->video && !am_capabilities()->audio && am_capabilities()->buttons);
	CHECK(am_input_read().buttons == (AM_BUTTON_A | AM_BUTTON_START | AM_BUTTON_SELECT));
	CHECK(!am_init(&config) && am_capabilities()->buttons);
	uint64_t before = am_uptime_us();
	am_sleep_us(100);
	CHECK(am_uptime_us() >= before + 100);
	unsigned char* data = calloc(511, 1);
	CHECK(data && ((uintptr_t) data & 7) == 0);
	for (unsigned i = 0; i < 511; ++i) { CHECK(data[i] == 0); data[i] = i; }
	data = realloc(data, 8193);
	CHECK(data);
	for (unsigned i = 0; i < 511; ++i) CHECK(data[i] == (unsigned char) i);
	free(data);
	char output[64];
	volatile uint64_t wide = 0x123456789abcdef0ull;
	snprintf(output, sizeof(output), "%llu:%08x", (unsigned long long) (wide / 1234567), 0x89abcdefu);
	CHECK(!strcmp(output, "1062533234294:89abcdef"));
	am_shutdown();
	CHECK(!am_capabilities()->buttons && !am_input_read().buttons && !am_uptime_us());
	config.video = true;
	CHECK(!am_init(&config) && !am_capabilities()->video);
	CHECK(strlen(am_error()) > 0);
	am_shutdown();
	CHECK(strlen(am_error()) > 0);
	config.video = false;
	config.audio = true;
	CHECK(!am_init(&config) && !am_capabilities()->audio);
	CHECK(am_audio_write(NULL, 0) == 0 && am_audio_queued() == 0);
	config.audio = false;
	CHECK(am_init(&config));
	am_shutdown();
	puts("PASS: RV32 startup, heap, libc, AM lifecycle, UART, timer and GPIO");
	return 0;
}
