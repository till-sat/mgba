/* SPDX-License-Identifier: MPL-2.0 */
/* Drive input by presented frames, independent of simulator wall-clock speed. */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned frames, audio_bytes, audio_nonzero;

int SDL_QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 len) {
	int (*real)(SDL_AudioDeviceID, const void*, Uint32) = dlsym(RTLD_NEXT, "SDL_QueueAudio");
	int result = real(device, data, len);
	if (!result) {
		audio_bytes += len;
		for (unsigned i = 0; i < len; ++i) audio_nonzero += ((const uint8_t*) data)[i] != 0;
	}
	return result;
}

void SDL_RenderPresent(SDL_Renderer* renderer) {
	void (*real)(SDL_Renderer*) = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	++frames;
	int w, h;
	SDL_GetRendererOutputSize(renderer, &w, &h);
	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	const char* directory = getenv("AM_TEST_CAPTURE");
	if (!surface || SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
	                                    surface->pixels, surface->pitch)) {
		printf("PLAYER_PROBE_ERROR %s\n", SDL_GetError());
	} else if (directory) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/frame-%02u.bmp", directory, frames);
		if (SDL_SaveBMP(surface, path)) printf("PLAYER_PROBE_ERROR %s\n", SDL_GetError());
	}
	SDL_FreeSurface(surface);
	real(renderer);
	if (getenv("AM_TEST_INPUT")) {
		SDL_Event event = {0};
		if (frames == 2 || frames == 4) {
			event.type = frames == 2 ? SDL_KEYDOWN : SDL_KEYUP;
			event.key.keysym.sym = SDLK_z;
			SDL_PushEvent(&event);
		} else if (frames == 6) {
			event.type = SDL_QUIT;
			SDL_PushEvent(&event);
		}
	}
}

__attribute__((destructor)) static void report(void) {
	printf("PLAYER_PROBE frames=%u audio_bytes=%u audio_nonzero=%u\n", frames, audio_bytes, audio_nonzero);
}
