/* SPDX-License-Identifier: MPL-2.0 */
#include "player.h"
#include <mgba-util/vfs.h>

extern const unsigned char _rom_start[], _rom_end[];
_Static_assert(AM_FRAME_LIMIT >= 0, "AM_FRAME_LIMIT must be nonnegative");
_Static_assert(!AM_HEADLESS || AM_FRAME_LIMIT > 0, "Headless runs require a positive frame limit");

int main(void) {
	size_t rom_size = (uintptr_t) _rom_end - (uintptr_t) _rom_start;
	struct VFile* rom = VFileFromConstMemory(_rom_start, rom_size);
	struct mCore* core = rom ? mCoreFindVF(rom) : NULL;
	if (!core || !core->init(core)) {
		if (rom) rom->close(rom);
		free(core);
		fprintf(stderr, "Could not initialize embedded ROM.\n");
		return 1;
	}
	mAMConfigure(core);
	int result = 1;
	if (core->loadROM(core, rom)) {
		/* An expandable RAM VFile preserves save semantics within this run. */
		struct VFile* save = VFileMemChunk(NULL, 0);
		if (!save || !core->loadSave(core, save)) {
			if (save) save->close(save);
			fprintf(stderr, "Could not allocate RAM save storage.\n");
		} else {
			printf("AM Spike/proto-soc: RV32IM, ROM %lu bytes\n", (unsigned long) rom_size);
			result = mAMRun(core, AM_HEADLESS, AM_AUDIO, AM_FRAME_LIMIT);
		}
		core->unloadROM(core);
	} else {
		rom->close(rom);
		fprintf(stderr, "Could not load embedded ROM.\n");
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return result;
}
