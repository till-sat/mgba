/* SPDX-License-Identifier: MPL-2.0 */
/* Exercise the public AM contract without linking any emulator code. */
#include <am.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #condition, am_error()); \
		exit(1); \
	} \
} while (0)

static void key(SDL_Keycode code, bool down, bool repeat) {
	SDL_Event event = { .type = down ? SDL_KEYDOWN : SDL_KEYUP };
	event.key.keysym.sym = code;
	event.key.repeat = repeat;
	CHECK(SDL_PushEvent(&event) == 1);
}

int main(void) {
	struct am_config config = { .title = "AM contract test", .width = 32, .height = 24, .audio_rate = 44100 };
	CHECK(am_init(&config));
	CHECK(!am_capabilities()->video && !am_capabilities()->audio && !am_capabilities()->buttons);
	CHECK(am_audio_write(NULL, 10) == 0 && am_audio_queued() == 0);
	CHECK(am_input_read().buttons == 0 && !am_input_read().quit);
	uint64_t before = am_uptime_us();
	am_sleep_us(3000);
	CHECK(am_uptime_us() >= before + 3000);
	am_shutdown();
	am_shutdown();

	config.video = true;
	config.width = 0;
	CHECK(!am_init(&config));
	CHECK(am_error()[0] && !am_capabilities()->video);
	config.width = 32;
	config.audio = true;
	CHECK(am_init(&config));
	CHECK(am_capabilities()->video && am_capabilities()->audio && am_capabilities()->buttons);
	CHECK(am_capabilities()->audio_rate > 0 && am_capabilities()->audio_capacity > 0);
	SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

	static const struct { SDL_Keycode key; uint32_t button; } controls[] = {
		{ SDLK_z, AM_BUTTON_A }, { SDLK_x, AM_BUTTON_B },
		{ SDLK_a, AM_BUTTON_L }, { SDLK_s, AM_BUTTON_R },
		{ SDLK_RETURN, AM_BUTTON_START }, { SDLK_BACKSPACE, AM_BUTTON_SELECT },
		{ SDLK_UP, AM_BUTTON_UP }, { SDLK_DOWN, AM_BUTTON_DOWN },
		{ SDLK_LEFT, AM_BUTTON_LEFT }, { SDLK_RIGHT, AM_BUTTON_RIGHT },
	};
	for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
		key(controls[i].key, true, false);
		CHECK(am_input_read().buttons == controls[i].button);
		key(controls[i].key, false, false);
		CHECK(am_input_read().buttons == 0);
	}
	key(SDLK_z, true, false);
	key(SDLK_RIGHT, true, false);
	key(SDLK_x, true, false);
	key(SDLK_z, false, false);
	CHECK(am_input_read().buttons == (AM_BUTTON_B | AM_BUTTON_RIGHT));
	SDL_Event focus = { .type = SDL_WINDOWEVENT };
	focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
	CHECK(SDL_PushEvent(&focus) == 1);
	CHECK(am_input_read().buttons == 0);
	key(SDLK_z, true, true);
	key(SDLK_SPACE, true, false);
	CHECK(am_input_read().buttons == 0);
	key(SDLK_ESCAPE, true, false);
	CHECK(am_input_read().quit);

	uint32_t pixels[] = { 0x00ff0000, 0x0000ff00, 0, 0x000000ff, 0x00ffffff, 0 };
	CHECK(!am_video_present(pixels, 2, 2, 1));
	CHECK(am_video_present(pixels, 2, 2, 3));
	int16_t samples[16384 * 2] = { 1, -1 };
	size_t written = am_audio_write(samples, 16384);
	CHECK(written > 0 && written <= am_capabilities()->audio_capacity);
	size_t queued = am_audio_queued();
	CHECK(queued <= am_capabilities()->audio_capacity);
	am_sleep_us(250000);
	CHECK(am_audio_queued() < queued);
	am_shutdown();
	CHECK(!am_capabilities()->audio && am_audio_queued() == 0);
	CHECK(am_init(&config));
	CHECK(!am_input_read().quit);
	am_shutdown();
	puts("PASS: AM lifecycle, capabilities, timing, buttons, video and bounded audio");
	return 0;
}
