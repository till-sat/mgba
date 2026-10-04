/* SPDX-License-Identifier: MPL-2.0 */
/* Inject SDL input and inspect host output across the guest MMIO boundary. */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned frames, audio_frames;
static int failed;

void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause) {
	void (*real)(SDL_AudioDeviceID, int) = dlsym(RTLD_NEXT, "SDL_PauseAudioDevice");
	(void) pause;
	real(device, 1);
}

static void key(SDL_Keycode code, int pressed) {
	SDL_Event event = {0};
	event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.keysym.sym = code;
	SDL_PushEvent(&event);
}

int SDL_QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 len) {
	int (*real)(SDL_AudioDeviceID, const void*, Uint32) = dlsym(RTLD_NEXT, "SDL_QueueAudio");
	const int16_t* samples = data;
	for (unsigned i = 0; i < len / 4; ++i) {
		int16_t left = (int) ((audio_frames + i) % 2048) * 31 - 32000;
		if (samples[i * 2] != left || samples[i * 2 + 1] != -left) {
			if (!failed) fprintf(stderr, "MEDIA_AUDIO_MISMATCH frame=%u expected=%d/%d actual=%d/%d\n",
			                    audio_frames + i, left, -left, samples[i * 2], samples[i * 2 + 1]);
			failed = 1;
		}
	}
	audio_frames += len / 4;
	return real(device, data, len);
}

void SDL_RenderPresent(SDL_Renderer* renderer) {
	void (*real)(SDL_Renderer*) = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	++frames;
	if (frames == 1 || frames == 24) {
		int w, h;
		SDL_GetRendererOutputSize(renderer, &w, &h);
		SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!surface || SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
		                                  surface->pixels, surface->pitch)) failed = 1;
		else {
			unsigned width = frames == 1 ? 240 : 256, height = frames == 1 ? 160 : 224;
			if (w != (int) width * 3 || h != (int) height * 3) failed = 1;
			for (int y = 0; y < h; ++y) {
				uint32_t* row = (uint32_t*) ((uint8_t*) surface->pixels + y * surface->pitch);
				for (int x = 0; x < w; ++x) {
					unsigned sx = x / 3, sy = y / 3;
					uint32_t expected = (sx << 16) | (sy << 8) | (sx ^ sy);
					if ((row[x] & 0xffffff) != expected) {
						if (!failed) fprintf(stderr, "MEDIA_PIXEL_MISMATCH frame=%u x=%d y=%d expected=%06x actual=%06x\n",
						                    frames, x, y, expected, row[x] & 0xffffff);
						failed = 1;
					}
				}
			}
		}
		SDL_FreeSurface(surface);
	}
	real(renderer);
	if (frames <= 20) {
		static const SDL_Keycode keys[] = { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
		                                  SDLK_z, SDLK_x, SDLK_a, SDLK_s, SDLK_RETURN, SDLK_BACKSPACE };
		key(keys[(frames - 1) / 2], frames & 1);
	} else if (frames == 21) {
		key(SDLK_z, 1);
		key(SDLK_x, 1);
	} else if (frames == 22 || frames == 23) {
		SDL_Event event = {0};
		event.type = frames == 22 ? SDL_WINDOWEVENT : SDL_QUIT;
		event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
		SDL_PushEvent(&event);
	}
}

__attribute__((destructor)) static void report(void) {
	printf("MEDIA_PROBE %s frames=%u audio_frames=%u\n",
	       !failed && frames == 24 && audio_frames >= 2048 ? "PASS" : "FAIL", frames, audio_frames);
}
