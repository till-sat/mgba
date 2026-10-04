/* SPDX-License-Identifier: MPL-2.0 */
/* Interpose native device calls to test the AM player without display or speakers. */
#define _GNU_SOURCE
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Uint32 started;
static int stage;
static int captured;
static unsigned long audioBytes;
static unsigned long nonzero;
static SDL_Window* window;

static int fail(const char* name) {
	const char* value = getenv("MGBA_TEST_FAIL");
	return value && !strcmp(value, name);
}

int SDL_QueueAudio(SDL_AudioDeviceID device, const void* data, Uint32 len) {
	int (*real)(SDL_AudioDeviceID, const void*, Uint32) = dlsym(RTLD_NEXT, "SDL_QueueAudio");
	int result = real(device, data, len);
	if (!result) {
		audioBytes += len;
		for (Uint32 i = 0; i < len; ++i) nonzero += ((const Uint8*) data)[i] != 0;
	}
	return result;
}

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
	SDL_Window* (*real)(const char*, int, int, int, int, Uint32) = dlsym(RTLD_NEXT, "SDL_CreateWindow");
	if (fail("window")) {
		SDL_SetError("test window failure");
		return NULL;
	}
	window = real(title, x, y, w, h, flags);
	printf("PROBE_RESIZABLE %d\n", !!(flags & SDL_WINDOW_RESIZABLE));
	return window;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h) {
	SDL_Texture* (*real)(SDL_Renderer*, Uint32, int, int, int) = dlsym(RTLD_NEXT, "SDL_CreateTexture");
	if (fail("texture")) {
		SDL_SetError("test texture failure");
		return NULL;
	}
	return real(renderer, format, access, w, h);
}

int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch) {
	int (*real)(SDL_Texture*, const SDL_Rect*, const void*, int) = dlsym(RTLD_NEXT, "SDL_UpdateTexture");
	if (fail("update")) {
		return SDL_SetError("test update failure");
	}
	return real(texture, rect, pixels, pitch);
}

int SDL_PollEvent(SDL_Event* event) {
	int (*real)(SDL_Event*) = dlsym(RTLD_NEXT, "SDL_PollEvent");
	if (!started) {
		/* Slow instruction simulators may spend seconds initializing the core. */
		if (getenv("MGBA_TEST_AFTER_FRAME")) return real(event);
		started = SDL_GetTicks();
	}
	Uint32 elapsed = SDL_GetTicks() - started;
	const unsigned times[] = { 400, 700, 1000, 1300, 1800 };
	if (stage < 5 && elapsed >= times[stage]) {
		if (stage == 2 && getenv("MGBA_TEST_RESIZE")) {
			int width, height;
			if (sscanf(getenv("MGBA_TEST_RESIZE"), "%dx%d", &width, &height) == 2) {
				SDL_SetWindowSize(window, width, height);
			}
		}
		memset(event, 0, sizeof(*event));
		if (stage == 3) {
			event->type = SDL_WINDOWEVENT;
			event->window.event = SDL_WINDOWEVENT_FOCUS_LOST;
		} else {
			event->type = stage == 1 ? SDL_KEYUP : SDL_KEYDOWN;
			event->key.keysym.sym = stage < 2 ? SDLK_z : stage == 2 ? SDLK_RIGHT : SDLK_q;
		}
		if ((stage == 2 || stage == 3) && getenv("MGBA_TEST_KEYUP_ONLY")) {
			event->type = SDL_USEREVENT;
		}
		if (stage == 4 && getenv("MGBA_TEST_ESCAPE")) {
			event->key.keysym.sym = SDLK_ESCAPE;
		}
		if (stage == 4 && getenv("MGBA_TEST_CLOSE")) {
			event->type = SDL_QUIT;
		}
		++stage;
		return 1;
	}
	return real(event);
}

void SDL_RenderPresent(SDL_Renderer* renderer) {
	void (*real)(SDL_Renderer*) = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	if (!started && getenv("MGBA_TEST_AFTER_FRAME")) started = SDL_GetTicks();
	if (!captured && started && SDL_GetTicks() - started > 1450) {
		int w, h;
		SDL_GetRendererOutputSize(renderer, &w, &h);
		/* Read the entire backbuffer, including bars outside the logical viewport. */
		int logicalWidth, logicalHeight;
		SDL_RenderGetLogicalSize(renderer, &logicalWidth, &logicalHeight);
		SDL_RenderSetLogicalSize(renderer, 0, 0);
		SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
		if (surface && SDL_RenderReadPixels(renderer, NULL, surface->format->format, surface->pixels, surface->pitch) == 0) {
			printf("PROBE_FRAME %dx%d\n", w, h);
			const char* out = getenv("MGBA_TEST_SCREEN");
			if (out && SDL_SaveBMP(surface, out) < 0) {
				printf("PROBE_ERROR %s\n", SDL_GetError());
			}
		} else {
			printf("PROBE_ERROR %s\n", SDL_GetError());
		}
		SDL_FreeSurface(surface);
		SDL_RenderSetLogicalSize(renderer, logicalWidth, logicalHeight);
		captured = 1;
	}
	real(renderer);
}

__attribute__((destructor)) static void report(void) {
	printf("PROBE_AUDIO bytes=%lu nonzero=%lu\n", audioBytes, nonzero);
}
