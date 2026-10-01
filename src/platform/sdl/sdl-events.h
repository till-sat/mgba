/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SDL_EVENTS_H
#define SDL_EVENTS_H

#include "sdl-common.h"

CXX_GUARD_START

struct mCoreThread;
void mSDLHandleEvent(struct mCoreThread* context, const SDL_Event* event);

CXX_GUARD_END

#endif
