/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"
#include "sdl-events.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>

#define DEFAULT_WINDOW_SCALE 3

bool mSDLSWInit(struct mSDLRenderer* renderer) {
	renderer->core->baseVideoSize(renderer->core, &renderer->width, &renderer->height);
	renderer->window = SDL_CreateWindow(projectName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                                   renderer->width * DEFAULT_WINDOW_SCALE,
	                                   renderer->height * DEFAULT_WINDOW_SCALE, SDL_WINDOW_RESIZABLE);
	if (!renderer->window) {
		return false;
	}
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer->sdlRenderer) {
		renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_SOFTWARE);
	}
	if (!renderer->sdlRenderer) {
		return false;
	}
	SDL_SetWindowMinimumSize(renderer->window, renderer->width, renderer->height);
	if (SDL_RenderSetLogicalSize(renderer->sdlRenderer, renderer->width, renderer->height) < 0 ||
	    SDL_SetRenderDrawColor(renderer->sdlRenderer, 0, 0, 0, 255) < 0) {
		return false;
	}
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
	Uint32 format = SDL_PIXELFORMAT_RGB565;
#else
	Uint32 format = SDL_PIXELFORMAT_ABGR1555;
#endif
#else
	Uint32 format = SDL_PIXELFORMAT_ABGR8888;
#endif
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, format, SDL_TEXTUREACCESS_STREAMING,
	                                   renderer->width, renderer->height);
	if (!renderer->sdlTex) {
		return false;
	}
	renderer->outputBuffer = calloc(renderer->width * renderer->height, sizeof(*renderer->outputBuffer));
	if (!renderer->outputBuffer) {
		SDL_SetError("Could not allocate the video buffer");
		return false;
	}
	/* Own a stable buffer; SDL texture locks need not return the same memory. */
	renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, renderer->width);
	return true;
}

bool mSDLSWRunloop(struct mSDLRenderer* renderer, struct mCoreThread* context) {
	SDL_Event event;
	SDL_Rect source = { 0, 0, renderer->width, renderer->height };
	while (mCoreThreadIsActive(context)) {
		while (SDL_PollEvent(&event)) {
			mSDLHandleEvent(context, &event);
			if (!mCoreThreadIsActive(context)) {
				break;
			}
		}
		if (!mCoreThreadIsActive(context)) {
			break;
		}

		bool ready = mCoreSyncWaitFrameStart(&context->impl->sync);
		bool success = true;
		if (ready) {
			unsigned width, height;
			renderer->core->currentVideoSize(renderer->core, &width, &height);
			if (width != (unsigned) source.w || height != (unsigned) source.h) {
				source.w = width;
				source.h = height;
				SDL_SetWindowMinimumSize(renderer->window, width, height);
				SDL_SetWindowSize(renderer->window, width * DEFAULT_WINDOW_SCALE, height * DEFAULT_WINDOW_SCALE);
				success = SDL_RenderSetLogicalSize(renderer->sdlRenderer, width, height) == 0;
			}
			success = success && SDL_UpdateTexture(renderer->sdlTex, &source, renderer->outputBuffer,
			                            renderer->width * BYTES_PER_PIXEL) == 0;
		}
		mCoreSyncWaitFrameEnd(&context->impl->sync);
		if (!success) {
			return false;
		}
		if (ready) {
			if (SDL_RenderClear(renderer->sdlRenderer) < 0 ||
			    SDL_RenderCopy(renderer->sdlRenderer, renderer->sdlTex, &source, NULL) < 0) {
				return false;
			}
			SDL_RenderPresent(renderer->sdlRenderer);
		}
	}
	return true;
}

void mSDLSWDeinit(struct mSDLRenderer* renderer) {
	SDL_DestroyTexture(renderer->sdlTex);
	SDL_DestroyRenderer(renderer->sdlRenderer);
	SDL_DestroyWindow(renderer->window);
	free(renderer->outputBuffer);
}
