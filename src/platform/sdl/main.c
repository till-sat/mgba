/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"
#include "sdl-audio.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/thread.h>

#define PORT "sdl"

static int mSDLRun(struct mSDLRenderer* renderer, const char* romPath);
static struct mStandardLogger _logger;

static void usage(const char* name) {
	printf("Usage: %s ROM\n", name);
	printf("Run one Game Boy, Game Boy Color, or Game Boy Advance ROM.\n");
	printf("Keyboard: arrows + Z/X (A/B), A/S (L/R), Enter (Start), Backspace (Select).\n");
	printf("Q or Escape quits. In-game saves use ROM-name.sav beside the ROM.\n");
}

int main(int argc, char** argv) {
#ifdef _WIN32
	AttachConsole(ATTACH_PARENT_PROCESS);
	freopen("CONOUT$", "w", stdout);
#endif
	if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return 0;
	}
	if (argc != 2) {
		usage(argv[0]);
		return 1;
	}

	struct mSDLRenderer renderer = {0};
	renderer.core = mCoreFind(argv[1]);
	if (!renderer.core) {
		fprintf(stderr, "Unsupported or unreadable ROM: %s\n", argv[1]);
		return 1;
	}
	if (!renderer.core->init(renderer.core)) {
		fprintf(stderr, "Could not initialize the emulator core.\n");
		free(renderer.core);
		return 1;
	}

	struct mCoreOptions opts = {
		.useBios = true,
		.rewindEnable = false,
		.audioBuffers = 1024,
		.videoSync = true,
		.audioSync = true,
		.volume = 0x100,
		.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL,
	};
	mCoreInitConfig(renderer.core, PORT);
	mCoreConfigSetDefaultIntValue(&renderer.core->config, "logToStdout", true);
	mCoreConfigLoadDefaults(&renderer.core->config, &opts);
	/* Fixed controls and saves beside the ROM; do not read global settings. */
	mCoreLoadForeignConfig(renderer.core, &renderer.core->config);
	mStandardLoggerInit(&_logger);
	mStandardLoggerConfig(&_logger, &renderer.core->config);
	mLogSetDefaultLogger(&_logger.d);

	int ret = 1;
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "Could not initialize SDL video: %s\n", SDL_GetError());
	} else {
		ret = mSDLRun(&renderer, argv[1]);
	}
	mSDLSWDeinit(&renderer);
	SDL_Quit();
	mCoreConfigDeinit(&renderer.core->config);
	renderer.core->deinit(renderer.core);
	mLogSetDefaultLogger(NULL);
	mStandardLoggerDeinit(&_logger);
	return ret;
}

#if defined(_WIN32) && !defined(_UNICODE)
#include <mgba-util/string.h>

int wmain(int argc, wchar_t** argv) {
	char** argv8 = malloc(sizeof(char*) * argc);
	int i;
	for (i = 0; i < argc; ++i) {
		argv8[i] = utf16to8((uint16_t*) argv[i], wcslen(argv[i]) * 2);
	}
	__argv = argv8;
	int ret = main(argc, argv8);
	for (i = 0; i < argc; ++i) {
		free(argv8[i]);
	}
	free(argv8);
	return ret;
}
#endif

static int mSDLRun(struct mSDLRenderer* renderer, const char* romPath) {
	if (!mCoreLoadFile(renderer->core, romPath)) {
		fprintf(stderr, "Could not load ROM: %s\n", romPath);
		return 1;
	}

	/* Battery saves are flushed by the core when the ROM is unloaded. */
	if (!mCoreAutoloadSave(renderer->core)) {
		fprintf(stderr, "Warning: could not open the ROM save file.\n");
	}
	if (!mSDLSWInit(renderer)) {
		fprintf(stderr, "Could not initialize SDL renderer: %s\n", SDL_GetError());
		renderer->core->unloadROM(renderer->core);
		return 1;
	}

	struct mCoreThread thread = { .core = renderer->core };
	thread.logger.logger = &_logger.d;
	struct mSDLAudio audio = {
		.samples = renderer->core->opts.audioBuffers,
		.sampleRate = 44100,
	};
	bool didFail = !mCoreThreadStart(&thread);
	if (!didFail) {
		if (mSDLInitAudio(&audio, &thread)) {
			if (!mSDLSWRunloop(renderer, &thread)) {
				didFail = true;
				fprintf(stderr, "Could not render video: %s\n", SDL_GetError());
			}
			if (mCoreThreadHasCrashed(&thread)) {
				didFail = true;
				fprintf(stderr, "The game crashed.\n");
			}
			/* Stop callbacks before destroying the thread's synchronization state. */
			mSDLDeinitAudio(&audio);
		} else {
			didFail = true;
			fprintf(stderr, "Could not initialize SDL audio: %s\n", SDL_GetError());
		}
		mCoreThreadEnd(&thread);
		mCoreThreadJoin(&thread);
	} else {
		fprintf(stderr, "Could not start the emulator thread.\n");
	}
	renderer->core->unloadROM(renderer->core);
	return didFail;
}
