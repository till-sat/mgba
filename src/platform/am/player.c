/* Copyright (c) 2013-2026 Jeffrey Pfau
 * SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#ifdef AM_PROFILE_SAMPLING
#include <am-profile.h>
#endif

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/audio-buffer.h>
#ifndef AM_BAREMETAL
#include <mgba-util/audio-resampler.h>
#endif
#include <mgba-util/crc32.h>

#include "player.h"

struct player {
	struct mCore* core;
	mColor* video;
	uint32_t* rgb;
	unsigned stride;
	unsigned rows;
	bool stopped;
	bool crashed;
	struct mAudioBuffer audio;
#ifdef AM_BAREMETAL
	struct {
		uint32_t phase;
		uint32_t step;
		unsigned source_rate;
		unsigned destination_rate;
	} resampler;
#else
	struct mAudioResampler resampler;
#endif
	bool audio_ready;
};

#ifdef AM_BAREMETAL
/* Q16.16 linear resampler. It avoids libgcc floating-point helpers on RV32. */
static void fixed_resampler_set_rates(struct player* player, unsigned source, unsigned destination) {
	player->resampler.source_rate = source;
	player->resampler.destination_rate = destination;
	player->resampler.phase = 0;
	player->resampler.step = destination ? ((uint64_t) source << 16) / destination : 0;
}

static size_t fixed_resampler_process(struct player* player, struct mAudioBuffer* source) {
	struct mAudioBuffer* destination = &player->audio;
	size_t available = mAudioBufferAvailable(source);
	size_t written = 0;
	while (!mAudioBufferFull(destination)) {
		size_t index = player->resampler.phase >> 16;
		if (index + 1 >= available) break;
		unsigned fraction = player->resampler.phase & 0xffff;
		int16_t samples[2];
		for (unsigned channel = 0; channel < 2; ++channel) {
			int32_t first = mAudioBufferPeek(source, channel, index);
			int32_t second = mAudioBufferPeek(source, channel, index + 1);
			samples[channel] = (int16_t) ((first * (int32_t) (65536 - fraction) +
			                                second * (int32_t) fraction) >> 16);
		}
		if (!mAudioBufferWrite(destination, samples, 1)) break;
		player->resampler.phase += player->resampler.step;
		++written;
	}
	size_t consumed = player->resampler.phase >> 16;
	if (consumed) {
		mAudioBufferRead(source, NULL, consumed);
		player->resampler.phase -= consumed << 16;
	}
	return written;
}
#endif

static void crashed(void* context) {
	struct player* player = context;
	player->crashed = true;
	player->stopped = true;
}

static void stopped(void* context) {
	((struct player*) context)->stopped = true;
}

static uint32_t core_buttons(uint32_t buttons) {
	static const struct { uint32_t button; unsigned key; } map[] = {
		{ AM_BUTTON_UP, GBA_KEY_UP }, { AM_BUTTON_DOWN, GBA_KEY_DOWN },
		{ AM_BUTTON_LEFT, GBA_KEY_LEFT }, { AM_BUTTON_RIGHT, GBA_KEY_RIGHT },
		{ AM_BUTTON_A, GBA_KEY_A }, { AM_BUTTON_B, GBA_KEY_B },
		{ AM_BUTTON_L, GBA_KEY_L }, { AM_BUTTON_R, GBA_KEY_R },
		{ AM_BUTTON_START, GBA_KEY_START }, { AM_BUTTON_SELECT, GBA_KEY_SELECT },
	};
	uint32_t keys = 0;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
		if (buttons & map[i].button) keys |= 1u << map[i].key;
	}
	return keys;
}

static bool poll_input(struct player* player) {
	struct am_input input = am_input_read();
	player->core->setKeys(player->core, core_buttons(input.buttons));
	player->stopped |= input.quit;
	return !player->stopped;
}

static bool submit_audio(struct player* player, bool paced) {
	struct mAudioBuffer* source = player->core->getAudioBuffer(player->core);
	if (!am_capabilities()->audio) {
		mAudioBufferClear(source);
		return true;
	}
#ifdef AM_BAREMETAL
	if (!player->audio_ready) return true;
#else
	mAudioResamplerSetSource(&player->resampler, source, player->core->audioSampleRate(player->core), true);
#endif
	int16_t samples[2048 * 2];
#ifdef AM_BAREMETAL
	while (fixed_resampler_process(player, source)) {
#else
	while (mAudioResamplerProcess(&player->resampler)) {
#endif
		size_t count = mAudioBufferRead(&player->audio, samples, 2048);
		size_t offset = 0;
		while (offset < count) {
			size_t space = am_capabilities()->audio_capacity - am_audio_queued();
			if (space) {
				size_t written = am_audio_write(samples + offset * 2, count - offset);
				if (!written) return false;
				offset += written;
			} else if (paced) {
				if (!poll_input(player)) return true;
				am_sleep_us(1000);
			} else {
				/* Bounded runs never wait for host playback. */
				break;
			}
		}
	}
	return true;
}

static bool copy_video(struct player* player, unsigned* width, unsigned* height) {
	player->core->currentVideoSize(player->core, width, height);
	if (!*width || !*height || *width > player->stride || *height > player->rows) {
		fprintf(stderr, "Invalid core video dimensions.\n");
		return false;
	}
	for (unsigned y = 0; y < *height; ++y) {
		for (unsigned x = 0; x < *width; ++x) {
			mColor color = player->video[y * player->stride + x];
			player->rgb[y * *width + x] = mColorConvert(color, mCOLOR_NATIVE, mCOLOR_RGB8);
		}
	}
	return true;
}

static int run(struct mCore* core, bool headless, bool audio, unsigned frame_limit,
               bool benchmark, unsigned warmup) {
	struct player state = { .core = core };
	struct player* player = &state;
	struct mStandardLogger logger;
	mStandardLoggerInit(&logger);
	mStandardLoggerConfig(&logger, &core->config);
	mLogSetDefaultLogger(&logger.d);
	core->baseVideoSize(core, &player->stride, &player->rows);
	player->video = calloc(player->stride * player->rows, sizeof(*player->video));
	player->rgb = calloc(player->stride * player->rows, sizeof(*player->rgb));
	int result = 1;
	if (!player->video || !player->rgb) {
		fprintf(stderr, "Could not allocate video buffers.\n");
		goto done;
	}
	core->setVideoBuffer(core, player->video, player->stride);
	core->setSync(core, NULL);
	struct am_config config = {
		.title = projectName,
		.width = player->stride,
		.height = player->rows,
		.audio_rate = 44100,
		.video = !headless,
		.audio = !headless && audio,
	};
	if (!am_init(&config)) {
		fprintf(stderr, "Could not initialize AM: %s\n", am_error());
		goto done;
	}
	mAudioBufferInit(&player->audio, 2048, 2);
#ifdef AM_BAREMETAL
	if (am_capabilities()->audio) {
		fixed_resampler_set_rates(player, core->audioSampleRate(core), am_capabilities()->audio_rate);
		player->audio_ready = true;
	}
#else
	mAudioResamplerInit(&player->resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetDestination(&player->resampler, &player->audio, am_capabilities()->audio_rate);
	player->audio_ready = true;
#endif
	struct mCoreCallbacks callbacks = { .context = player, .coreCrashed = crashed, .shutdown = stopped };
	core->addCoreCallbacks(core, &callbacks);
	if (frame_limit) {
		core->rtc.override = RTC_FIXED;
		core->rtc.value = 946684800; /* 2000-01-01 UTC. */
	}
	core->reset(core);
	unsigned frames = 0, warmed = 0, width = 0, height = 0;
	uint64_t deadline = am_uptime_us(), remainder = 0;
	uint64_t measured_start = 0, elapsed = 0;
	if (benchmark) {
		core->setKeys(core, 0);
		printf("Benchmark start: warmup=%u; frames=%u; software rendering; no output; keys=0\n", warmup, frame_limit);
		fflush(NULL);

#ifdef AM_PROFILE_SAMPLING
		am_profile_prepare();
#endif
	}
	result = 0;
	while ((benchmark || poll_input(player)) && (warmed < warmup || !frame_limit || frames < frame_limit)) {
		if (benchmark && warmed == warmup && !frames) {
			measured_start = am_uptime_us();
#ifdef AM_PROFILE_SAMPLING
			am_profile_start();
#endif
		}
		core->runFrame(core);
		if (player->stopped) break;
		if (!copy_video(player, &width, &height)) {
			result = 1;
			break;
		}
		if (am_capabilities()->video && !am_video_present(player->rgb, width, height, width)) {
			fprintf(stderr, "Could not render video: %s\n", am_error());
			result = 1;
			break;
		}
		if (!submit_audio(player, !frame_limit)) {
			fprintf(stderr, "Could not queue audio: %s\n", am_error());
			result = 1;
			break;
		}
		if (warmed < warmup) ++warmed;
		else ++frames;
		if (!frame_limit) {
			uint64_t period = (uint64_t) core->frameCycles(core) * 1000000 + remainder;
			deadline += period / core->frequency(core);
			remainder = period % core->frequency(core);
			uint64_t now = am_uptime_us();
			if (deadline > now) am_sleep_us(deadline - now);
			else if (now - deadline > 100000) deadline = now;
		}
	}
	if (benchmark) {
#ifdef AM_PROFILE_SAMPLING
		am_profile_stop();
#endif
		elapsed = am_uptime_us() - measured_start;
	}
	if (player->crashed) {
		fprintf(stderr, "The game crashed.\n");
		result = 1;
	}
	if (frame_limit && !result) {
		/* Hash RGB bytes explicitly, independent of host integer byte order. */
		uint32_t checksum = 0;
		for (size_t i = 0; i < (size_t) width * height; ++i) {
			uint8_t rgb[] = { player->rgb[i] >> 16, player->rgb[i] >> 8, player->rgb[i] };
			checksum = crc32(checksum, rgb, sizeof(rgb));
		}
		printf("Frames: %u; video: %ux%u; CRC32: %08" PRIX32 "\n", frames, width, height, checksum);
	}
	if (benchmark && !result) {
		if (frames != frame_limit || !elapsed) {
			fprintf(stderr, "Incomplete benchmark or unavailable timer.\n");
			result = 1;
		} else {
			uint64_t fps_milli = (uint64_t) frames * 1000000000 / elapsed;
			printf("Benchmark: warmup=%u; frames=%u; elapsed_us=%" PRIu64 "; FPS=%" PRIu64 ".%03" PRIu64 "\n",
			       warmup, frames, elapsed, fps_milli / 1000, fps_milli % 1000);
		}
	}
#ifdef AM_PROFILE_SAMPLING
	if (benchmark) am_profile_report();
#endif
	core->clearCoreCallbacks(core);
#ifndef AM_BAREMETAL
	mAudioResamplerDeinit(&player->resampler);
#endif
	mAudioBufferDeinit(&player->audio);
done:
	am_shutdown();
	free(player->rgb);
	free(player->video);
	mLogSetDefaultLogger(NULL);
	mStandardLoggerDeinit(&logger);
	return result;
}

int mAMRun(struct mCore* core, bool headless, bool audio, unsigned frame_limit) {
	return run(core, headless, audio, frame_limit, false, 0);
}

int mAMBenchmark(struct mCore* core, unsigned warmup, unsigned frames) {
	if (!frames) return 1;
	return run(core, true, false, frames, true, warmup);
}

void mAMConfigure(struct mCore* core) {
	struct mCoreOptions opts = {
		.useBios = true,
		.audioBuffers = 1024,
		.volume = 0x100,
		.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL,
	};
	mCoreInitConfig(core, "am");
	mCoreConfigSetDefaultIntValue(&core->config, "logToStdout", true);
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadForeignConfig(core, &core->config);
}
