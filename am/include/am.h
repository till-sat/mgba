/* SPDX-License-Identifier: MPL-2.0 */
#ifndef MINI_AM_H
#define MINI_AM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical buttons, independent of keyboard codes, GPIO pins, and emulator keys. */
enum am_button {
	AM_BUTTON_UP     = 1u << 0,
	AM_BUTTON_DOWN   = 1u << 1,
	AM_BUTTON_LEFT   = 1u << 2,
	AM_BUTTON_RIGHT  = 1u << 3,
	AM_BUTTON_A      = 1u << 4,
	AM_BUTTON_B      = 1u << 5,
	AM_BUTTON_L      = 1u << 6,
	AM_BUTTON_R      = 1u << 7,
	AM_BUTTON_START  = 1u << 8,
	AM_BUTTON_SELECT = 1u << 9,
};

struct am_config {
	const char* title;
	unsigned width;
	unsigned height;
	unsigned audio_rate;
	bool video;
	bool audio;
};

struct am_caps {
	bool video;
	bool buttons;
	bool audio;
	unsigned audio_rate;
	size_t audio_capacity; /* Stereo frames, not bytes or individual samples. */
};

struct am_input {
	uint32_t buttons; /* A set bit means held down. */
	bool quit;        /* Host exit request; separate from game buttons. */
};

/* One active instance. Failed initialization cleans up acquired resources.
 * Config selects requested devices; consult capabilities before using them.
 * Shutdown is also valid after failed initialization. Calls belong to the
 * application thread. Backends must never call into the emulator. */
bool am_init(const struct am_config* config);
void am_shutdown(void);
const struct am_caps* am_capabilities(void);
const char* am_error(void);

/* Poll host events and return the complete held-button state. */
struct am_input am_input_read(void);

/* Monotonic microseconds since initialization. Sleep may overshoot. */
uint64_t am_uptime_us(void);
void am_sleep_us(uint64_t duration);

/* Pixels are numeric 0x00RRGGBB values; stride is measured in pixels.
 * The caller retains ownership and may reuse the buffer after return. */
bool am_video_present(const uint32_t* pixels, unsigned width, unsigned height, size_t stride);

/* Interleaved, signed 16-bit stereo at caps.audio_rate. Nonblocking write:
 * copies up to the available capacity and returns accepted stereo frames.
 * Absent audio accepts zero frames. Queued excludes the device's own buffer. */
size_t am_audio_write(const int16_t* samples, size_t frames);
size_t am_audio_queued(void);

#ifdef __cplusplus
}
#endif

#endif
