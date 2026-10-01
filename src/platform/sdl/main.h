/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SDL_MAIN_H
#define SDL_MAIN_H

#include <mgba-util/common.h>
#include <mgba-util/image.h>
#include "sdl-common.h"

CXX_GUARD_START

struct mCore;
struct mCoreThread;
struct mSDLRenderer {
	struct mCore* core;
	mColor* outputBuffer;
	SDL_Window* window;
	SDL_Texture* sdlTex;
	SDL_Renderer* sdlRenderer;
	unsigned width;
	unsigned height;
};

bool mSDLSWInit(struct mSDLRenderer* renderer);
bool mSDLSWRunloop(struct mSDLRenderer* renderer, struct mCoreThread* context);
void mSDLSWDeinit(struct mSDLRenderer* renderer);

CXX_GUARD_END

#endif
