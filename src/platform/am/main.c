/* Copyright (c) 2013-2026 Jeffrey Pfau
 * SPDX-License-Identifier: MPL-2.0 */
#include "player.h"
#include <errno.h>

static void usage(const char* name) {
	printf("Usage: %s [--frames N] [--headless] [--benchmark N [--warmup N]] ROM\n", name);
	printf("Run one Game Boy, Game Boy Color, or Game Boy Advance ROM through AM.\n");
	printf("Native buttons: arrows + Z/X (A/B), A/S (L/R), Enter (Start), Backspace (Select).\n");
	printf("Q or Escape quits. In-game saves use ROM-name.sav beside the ROM.\n");
	printf("--frames N runs without pacing, fixes RTC time, and prints a final frame CRC32.\n");
	printf("--headless disables display and audio; requires --frames N.\n");
	printf("--benchmark N measures N headless frames after --warmup N (default 30); input is fixed to zero.\n");
}

int main(int argc, char** argv) {
	const char* path = NULL;
	bool headless = false;
	bool benchmark = false, frame_option = false, warmup_option = false;
	unsigned frames = 0;
	unsigned warmup = 30;
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage(argv[0]);
		return 0;
	}
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--headless")) headless = true;
		else if ((!strcmp(argv[i], "--frames") || !strcmp(argv[i], "--benchmark") ||
		          !strcmp(argv[i], "--warmup")) && i + 1 < argc) {
			bool is_warmup = !strcmp(argv[i], "--warmup");
			bool is_benchmark = !strcmp(argv[i], "--benchmark");
			char* end;
			const char* number = argv[++i];
			errno = 0;
			unsigned long value = strtoul(number, &end, 10);
			if (errno || number[0] < '0' || number[0] > '9' || *end || (!value && !is_warmup) || value > UINT_MAX) goto invalid;
			if (is_warmup) { warmup = value; warmup_option = true; }
			else {
				if (frame_option) goto invalid;
				frame_option = true;
				benchmark = is_benchmark;
				frames = value;
			}
		} else if (argv[i][0] == '-' || path) goto invalid;
		else path = argv[i];
	}
	if (!path || (headless && !frames) || (warmup_option && !benchmark)) goto invalid;
	struct mCore* core = mCoreFind(path);
	if (!core) {
		fprintf(stderr, "Unsupported or unreadable ROM: %s\n", path);
		return 1;
	}
	if (!core->init(core)) {
		fprintf(stderr, "Could not initialize the emulator core.\n");
		free(core);
		return 1;
	}
	mAMConfigure(core);
	int result = 1;
	if (!mCoreLoadFile(core, path)) {
		fprintf(stderr, "Could not load ROM: %s\n", path);
	} else {
		if (!mCoreAutoloadSave(core)) fprintf(stderr, "Warning: could not open the ROM save file.\n");
		result = benchmark ? mAMBenchmark(core, warmup, frames) : mAMRun(core, headless, true, frames);
		core->unloadROM(core);
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return result;

invalid:
	usage(argv[0]);
	return 1;
}
