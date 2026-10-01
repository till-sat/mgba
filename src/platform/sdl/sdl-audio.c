/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "sdl-audio.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>

mLOG_DEFINE_CATEGORY(SDL_AUDIO, "SDL Audio", "platform.sdl.audio");

static void _mSDLAudioCallback(void* context, Uint8* data, int len);

bool mSDLInitAudio(struct mSDLAudio* context, struct mCoreThread* threadContext) {
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		return false;
	}
	context->desiredSpec = (SDL_AudioSpec) {
		.freq = context->sampleRate,
		.channels = 2,
		.samples = context->samples,
		.format = AUDIO_S16SYS,
		.callback = _mSDLAudioCallback,
		.userdata = context,
	};
	context->deviceId = SDL_OpenAudioDevice(NULL, 0, &context->desiredSpec, &context->obtainedSpec,
	                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!context->deviceId) {
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return false;
	}
	if (context->obtainedSpec.channels != 2 || context->obtainedSpec.format != AUDIO_S16SYS) {
		SDL_CloseAudioDevice(context->deviceId);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		SDL_SetError("Unsupported audio format");
		return false;
	}

	mAudioBufferInit(&context->buffer, context->samples, context->obtainedSpec.channels);
	mAudioResamplerInit(&context->resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetDestination(&context->resampler, &context->buffer, context->obtainedSpec.freq);
	context->core = threadContext->core;
	context->sync = &threadContext->impl->sync;
	SDL_PauseAudioDevice(context->deviceId, 0);
	return true;
}

void mSDLDeinitAudio(struct mSDLAudio* context) {
	SDL_PauseAudioDevice(context->deviceId, 1);
	SDL_CloseAudioDevice(context->deviceId);
	mAudioBufferDeinit(&context->buffer);
	mAudioResamplerDeinit(&context->resampler);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void _mSDLAudioCallback(void* context, Uint8* data, int len) {
	struct mSDLAudio* audioContext = context;
	struct mAudioBuffer* buffer = audioContext->core->getAudioBuffer(audioContext->core);
	unsigned sampleRate = audioContext->core->audioSampleRate(audioContext->core);
	double fauxClock = 1;
	if (audioContext->sync) {
		if (audioContext->sync->fpsTarget > 0 && audioContext->core) {
			fauxClock = mCoreCalculateFramerateRatio(audioContext->core, audioContext->sync->fpsTarget);
		}
		mCoreSyncLockAudio(audioContext->sync);
		audioContext->sync->audioHighWater = audioContext->samples + audioContext->resampler.highWaterMark + audioContext->resampler.lowWaterMark + (audioContext->samples >> 6);
		audioContext->sync->audioHighWater *= sampleRate / (fauxClock * audioContext->obtainedSpec.freq);

		if (audioContext->resampler.source) {
			size_t destSize = mAudioBufferCapacity(audioContext->resampler.source);
			if (audioContext->sync->audioHighWater >= destSize) {
				audioContext->sync->audioHighWater = destSize;
			}
		}
	}
	mAudioResamplerSetSource(&audioContext->resampler, buffer, sampleRate / fauxClock, true);
	mAudioResamplerProcess(&audioContext->resampler);
	if (audioContext->sync) {
		mCoreSyncConsumeAudio(audioContext->sync);
	}
	len /= 2 * audioContext->obtainedSpec.channels;
	int available = mAudioBufferRead(&audioContext->buffer, (int16_t*) data, len);

	if (available < len) {
		memset(((short*) data) + audioContext->obtainedSpec.channels * available, 0, (len - available) * audioContext->obtainedSpec.channels * sizeof(short));
	}
}
