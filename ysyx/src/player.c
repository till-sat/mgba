/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include <amdev.h>
#include <klib-macros.h>

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/version.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/crc32.h>

struct ysyx_player {
	struct mCore* core;
	mColor* video;
	uint32_t* rgb;
	unsigned stride;
	unsigned rows;
	uint32_t buttons;
	bool stopped;
	bool crashed;
};

static void crashed(void* context) {
	struct ysyx_player* player = context;
	player->crashed = true;
	player->stopped = true;
}

static void stopped(void* context) {
	((struct ysyx_player*) context)->stopped = true;
}

static uint32_t button_for_key(int keycode) {
	switch (keycode) {
	case AM_KEY_UP: return 1u << GBA_KEY_UP;
	case AM_KEY_DOWN: return 1u << GBA_KEY_DOWN;
	case AM_KEY_LEFT: return 1u << GBA_KEY_LEFT;
	case AM_KEY_RIGHT: return 1u << GBA_KEY_RIGHT;
	case AM_KEY_Z: return 1u << GBA_KEY_A;
	case AM_KEY_X: return 1u << GBA_KEY_B;
	case AM_KEY_A: return 1u << GBA_KEY_L;
	case AM_KEY_S: return 1u << GBA_KEY_R;
	case AM_KEY_RETURN: return 1u << GBA_KEY_START;
	case AM_KEY_BACKSPACE: return 1u << GBA_KEY_SELECT;
	default: return 0;
	}
}

static bool poll_input(struct ysyx_player* player) {
	AM_INPUT_KEYBRD_T event;
	do {
		event = io_read(AM_INPUT_KEYBRD);
		if (event.keycode == AM_KEY_NONE) break;
		if (event.keydown && (event.keycode == AM_KEY_ESCAPE || event.keycode == AM_KEY_Q)) {
			player->stopped = true;
			break;
		}
		uint32_t button = button_for_key(event.keycode);
		if (event.keydown) player->buttons |= button;
		else player->buttons &= ~button;
	} while (!player->stopped);
	player->core->setKeys(player->core, player->buttons);
	return !player->stopped;
}

static bool copy_video(struct ysyx_player* player, unsigned* width, unsigned* height) {
	player->core->currentVideoSize(player->core, width, height);
	if (!*width || !*height || *width > player->stride || *height > player->rows) return false;
	for (unsigned y = 0; y < *height; ++y) {
		mColor* source = player->video + y * player->stride;
		uint32_t* destination = player->rgb + y * *width;
		for (unsigned x = 0; x < *width; ++x) {
#ifdef COLOR_16_BIT
			destination[x] = mColorConvert(source[x], mCOLOR_NATIVE, mCOLOR_RGB8);
#else
			uint32_t color = source[x];
			destination[x] = ((color & 0x000000FFu) << 16) |
			                 (color & 0x0000FF00u) | ((color >> 16) & 0x000000FFu);
#endif
		}
	}
	return true;
}

static void present(const uint32_t* pixels, unsigned width, unsigned height) {
	io_write(AM_GPU_FBDRAW, 0, 0, (void*) pixels, (int) width, (int) height, true);
}

int ysyx_run(struct mCore* core, unsigned frame_limit, bool headless) {
	struct ysyx_player player = { .core = core };
	struct mStandardLogger logger;
	mStandardLoggerInit(&logger);
	mStandardLoggerConfig(&logger, &core->config);
	mLogSetDefaultLogger(&logger.d);
	core->baseVideoSize(core, &player.stride, &player.rows);
	player.video = calloc((size_t) player.stride * player.rows, sizeof(*player.video));
	player.rgb = calloc((size_t) player.stride * player.rows, sizeof(*player.rgb));
	if (!player.video || !player.rgb) {
		printf("mGBA: video allocation failed\n");
		goto fail;
	}
	core->setVideoBuffer(core, player.video, player.stride);
	core->setSync(core, NULL);
	if (!ioe_init()) {
		printf("mGBA: ioe_init failed\n");
		goto fail;
	}
	AM_GPU_CONFIG_T gpu = io_read(AM_GPU_CONFIG);
	if (!headless && (!gpu.present || player.stride > (unsigned) gpu.width || player.rows > (unsigned) gpu.height)) {
		printf("mGBA: GPU is too small for %ux%u\n", player.stride, player.rows);
		goto fail;
	}
	struct mCoreCallbacks callbacks = { .context = &player, .coreCrashed = crashed, .shutdown = stopped };
	core->addCoreCallbacks(core, &callbacks);
	if (frame_limit) {
		core->rtc.override = RTC_FIXED;
		core->rtc.value = 946684800;
	}
	core->reset(core);
	unsigned frames = 0, width = 0, height = 0;
	while (!player.stopped && frames < frame_limit) {
		if (!poll_input(&player)) break;
		core->runFrame(core);
		if (player.stopped || !copy_video(&player, &width, &height)) break;
		if (!headless) present(player.rgb, width, height);
		/* The standard ysyxSoC AM platform has no audio device yet. */
		mAudioBufferClear(core->getAudioBuffer(core));
		++frames;
	}
	if (player.crashed || frames != frame_limit) goto fail_callbacks;
	uint32_t checksum = 0;
	for (size_t i = 0; i < (size_t) width * height; ++i) {
		uint8_t rgb[] = { player.rgb[i] >> 16, player.rgb[i] >> 8, player.rgb[i] };
		checksum = crc32(checksum, rgb, sizeof(rgb));
	}
	printf("Frames: %u; video: %ux%u; CRC32: %08x\n", frames, width, height, checksum);
	core->clearCoreCallbacks(core);
	mStandardLoggerDeinit(&logger);
	return 0;

fail_callbacks:
	core->clearCoreCallbacks(core);
fail:
	mStandardLoggerDeinit(&logger);
	return 1;
}

void ysyx_configure(struct mCore* core) {
	struct mCoreOptions opts = {
		.useBios = true,
		.audioBuffers = 1024,
		.volume = 0x100,
		.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL,
	};
	mCoreInitConfig(core, "ysyx-am");
	mCoreConfigSetDefaultIntValue(&core->config, "logToStdout", true);
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadForeignConfig(core, &core->config);
}
