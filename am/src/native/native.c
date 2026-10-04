/* Copyright (c) 2013-2026 Jeffrey Pfau
 * SPDX-License-Identifier: MPL-2.0 */
#include <am.h>

#include <SDL.h>
#include <limits.h>
#include <string.h>

#define WINDOW_SCALE 3
#define AUDIO_CAPACITY 8192
#define AUDIO_FRAME_BYTES (2 * sizeof(int16_t))

static struct {
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	SDL_AudioDeviceID audio;
	struct am_caps caps;
	struct am_input input;
	unsigned width;
	unsigned height;
	uint64_t started;
	uint64_t frequency;
	bool initialized;
	char error[256];
} native;

static bool remember_error(void) {
	SDL_strlcpy(native.error, SDL_GetError(), sizeof(native.error));
	return false;
}

static bool video_size(unsigned width, unsigned height) {
	if (!width || !height || width > INT_MAX / WINDOW_SCALE || height > INT_MAX / WINDOW_SCALE) {
		SDL_SetError("Invalid video dimensions");
		return remember_error();
	}
	SDL_Texture* texture = SDL_CreateTexture(native.renderer, SDL_PIXELFORMAT_ARGB8888,
	                                        SDL_TEXTUREACCESS_STREAMING, width, height);
	if (!texture) {
		return remember_error();
	}
	if (SDL_RenderSetLogicalSize(native.renderer, width, height) < 0) {
		SDL_DestroyTexture(texture);
		return remember_error();
	}
	SDL_SetWindowMinimumSize(native.window, width, height);
	SDL_SetWindowSize(native.window, width * WINDOW_SCALE, height * WINDOW_SCALE);
	SDL_DestroyTexture(native.texture);
	native.texture = texture;
	native.width = width;
	native.height = height;
	return true;
}

bool am_init(const struct am_config* config) {
	if (native.initialized) {
		SDL_SetError("AM is already initialized");
		return remember_error();
	}
	memset(&native, 0, sizeof(native));
	if (!config || (config->video && (!config->width || !config->height ||
	    config->width > INT_MAX / WINDOW_SCALE || config->height > INT_MAX / WINDOW_SCALE)) ||
	    (config->audio && (!config->audio_rate || config->audio_rate > INT_MAX))) {
		SDL_SetError("Invalid AM configuration");
		return remember_error();
	}
	Uint32 flags = SDL_INIT_TIMER;
	if (config->video) flags |= SDL_INIT_VIDEO;
	if (config->audio) flags |= SDL_INIT_AUDIO;
	if (SDL_Init(flags) < 0) {
		remember_error();
		am_shutdown();
		return false;
	}
	native.initialized = true;
	native.started = SDL_GetPerformanceCounter();
	native.frequency = SDL_GetPerformanceFrequency();
	if (config->video) {
		native.window = SDL_CreateWindow(config->title ? config->title : "AM native",
		                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		                                 config->width * WINDOW_SCALE, config->height * WINDOW_SCALE,
		                                 SDL_WINDOW_RESIZABLE);
		if (!native.window) goto fail;
		native.renderer = SDL_CreateRenderer(native.window, -1, SDL_RENDERER_ACCELERATED);
		if (!native.renderer) {
			native.renderer = SDL_CreateRenderer(native.window, -1, SDL_RENDERER_SOFTWARE);
		}
		if (!native.renderer) goto fail;
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
		if (SDL_SetRenderDrawColor(native.renderer, 0, 0, 0, 255) < 0 ||
		    !video_size(config->width, config->height)) goto fail;
		native.caps.video = true;
		native.caps.buttons = true;
	}
	if (config->audio) {
		SDL_AudioSpec wanted = {
			.freq = config->audio_rate,
			.format = AUDIO_S16SYS,
			.channels = 2,
			.samples = 1024,
		};
		SDL_AudioSpec obtained;
		native.audio = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
		if (!native.audio) goto fail;
		if (obtained.format != AUDIO_S16SYS || obtained.channels != 2 || obtained.freq <= 0) {
			SDL_SetError("Unsupported audio format");
			goto fail;
		}
		native.caps.audio = true;
		native.caps.audio_rate = obtained.freq;
		native.caps.audio_capacity = AUDIO_CAPACITY;
		SDL_PauseAudioDevice(native.audio, 0);
	}
	return true;

fail:
	remember_error();
	am_shutdown();
	return false;
}

void am_shutdown(void) {
	if (native.audio) SDL_CloseAudioDevice(native.audio);
	SDL_DestroyTexture(native.texture);
	SDL_DestroyRenderer(native.renderer);
	SDL_DestroyWindow(native.window);
	SDL_Quit();
	native.audio = 0;
	native.texture = NULL;
	native.renderer = NULL;
	native.window = NULL;
	native.initialized = false;
	memset(&native.caps, 0, sizeof(native.caps));
	memset(&native.input, 0, sizeof(native.input));
}

const struct am_caps* am_capabilities(void) {
	return &native.caps;
}

const char* am_error(void) {
	return native.error;
}

static uint32_t button_for_key(SDL_Keycode key) {
	switch (key) {
	case SDLK_UP: return AM_BUTTON_UP;
	case SDLK_DOWN: return AM_BUTTON_DOWN;
	case SDLK_LEFT: return AM_BUTTON_LEFT;
	case SDLK_RIGHT: return AM_BUTTON_RIGHT;
	case SDLK_z: return AM_BUTTON_A;
	case SDLK_x: return AM_BUTTON_B;
	case SDLK_a: return AM_BUTTON_L;
	case SDLK_s: return AM_BUTTON_R;
	case SDLK_RETURN: return AM_BUTTON_START;
	case SDLK_BACKSPACE: return AM_BUTTON_SELECT;
	default: return 0;
	}
}

struct am_input am_input_read(void) {
	if (!native.caps.buttons) return native.input;
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			native.input.quit = true;
			break;
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) native.input.buttons = 0;
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (event.key.repeat) break;
			if (event.type == SDL_KEYDOWN) {
				if (event.key.keysym.sym == SDLK_q || event.key.keysym.sym == SDLK_ESCAPE) {
					native.input.quit = true;
				}
				native.input.buttons |= button_for_key(event.key.keysym.sym);
			} else {
				native.input.buttons &= ~button_for_key(event.key.keysym.sym);
			}
			break;
		default:
			break;
		}
	}
	return native.input;
}

uint64_t am_uptime_us(void) {
	if (!native.initialized) return 0;
	uint64_t ticks = SDL_GetPerformanceCounter() - native.started;
	return ticks / native.frequency * 1000000 + ticks % native.frequency * 1000000 / native.frequency;
}

void am_sleep_us(uint64_t duration) {
	while (duration >= 1000000) {
		SDL_Delay(1000);
		duration -= 1000000;
	}
	SDL_Delay((Uint32) ((duration + 999) / 1000));
}

bool am_video_present(const uint32_t* pixels, unsigned width, unsigned height, size_t stride) {
	if (!native.caps.video || !pixels || stride < width || stride > INT_MAX / sizeof(*pixels)) {
		SDL_SetError("Invalid video frame or unavailable display");
		return remember_error();
	}
	if ((width != native.width || height != native.height) && !video_size(width, height)) return false;
	if (SDL_UpdateTexture(native.texture, NULL, pixels, stride * sizeof(*pixels)) < 0 ||
	    SDL_RenderClear(native.renderer) < 0 ||
	    SDL_RenderCopy(native.renderer, native.texture, NULL, NULL) < 0) return remember_error();
	SDL_RenderPresent(native.renderer);
	return true;
}

size_t am_audio_queued(void) {
	return native.audio ? SDL_GetQueuedAudioSize(native.audio) / AUDIO_FRAME_BYTES : 0;
}

size_t am_audio_write(const int16_t* samples, size_t frames) {
	if (!native.audio || !samples || !frames) return 0;
	size_t queued = am_audio_queued();
	size_t space = queued < AUDIO_CAPACITY ? AUDIO_CAPACITY - queued : 0;
	if (frames > space) frames = space;
	if (frames && SDL_QueueAudio(native.audio, samples, frames * AUDIO_FRAME_BYTES) < 0) {
		remember_error();
		return 0;
	}
	return frames;
}
