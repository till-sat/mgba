/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "sdl-events.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba/internal/gba/input.h>

static int _key(SDL_Keycode keycode) {
	switch (keycode) {
	case SDLK_z: return GBA_KEY_A;
	case SDLK_x: return GBA_KEY_B;
	case SDLK_a: return GBA_KEY_L;
	case SDLK_s: return GBA_KEY_R;
	case SDLK_RETURN: return GBA_KEY_START;
	case SDLK_BACKSPACE: return GBA_KEY_SELECT;
	case SDLK_UP: return GBA_KEY_UP;
	case SDLK_DOWN: return GBA_KEY_DOWN;
	case SDLK_LEFT: return GBA_KEY_LEFT;
	case SDLK_RIGHT: return GBA_KEY_RIGHT;
	default: return -1;
	}
}

void mSDLHandleEvent(struct mCoreThread* context, const SDL_Event* event) {
	switch (event->type) {
	case SDL_QUIT:
		mCoreThreadEnd(context);
		break;
	case SDL_WINDOWEVENT:
		if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
			/* A released key may belong to another window after focus changes. */
			mCoreThreadInterrupt(context);
			context->core->setKeys(context->core, 0);
			mCoreThreadContinue(context);
		}
		break;
	case SDL_KEYDOWN:
	case SDL_KEYUP: {
		if (event->key.repeat) {
			break;
		}
		SDL_Keycode keycode = event->key.keysym.sym;
		if (event->type == SDL_KEYDOWN && (keycode == SDLK_ESCAPE || keycode == SDLK_q)) {
			mCoreThreadEnd(context);
			break;
		}
		int key = _key(keycode);
		if (key >= 0) {
			mCoreThreadInterrupt(context);
			if (event->type == SDL_KEYDOWN) {
				context->core->addKeys(context->core, 1 << key);
			} else {
				context->core->clearKeys(context->core, 1 << key);
			}
			mCoreThreadContinue(context);
		}
		break;
	}
	default:
		break;
	}
}
