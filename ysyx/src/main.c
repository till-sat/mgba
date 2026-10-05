/* SPDX-License-Identifier: MPL-2.0 */
#include <am.h>
#include <klib.h>

#include <mgba/core/core.h>
#include <mgba-util/vfs.h>

extern const unsigned char _rom_start[], _rom_end[];
void ysyx_configure(struct mCore* core);
int ysyx_run(struct mCore* core, unsigned frame_limit, bool headless);

#ifndef AM_FRAME_LIMIT
#define AM_FRAME_LIMIT 2
#endif

int main(const char* args) {
	(void) args;
	struct VFile* rom = VFileFromConstMemory(_rom_start, (size_t) (_rom_end - _rom_start));
	struct mCore* core = rom ? mCoreFindVF(rom) : NULL;
	if (!core || !core->init(core)) {
		if (rom) rom->close(rom);
		printf("mGBA: could not initialize embedded ROM\n");
		return 1;
	}
	ysyx_configure(core);
	int result = 1;
	if (core->loadROM(core, rom)) {
		struct VFile* save = VFileMemChunk(NULL, 0);
		if (save && core->loadSave(core, save)) {
			result = ysyx_run(core, AM_FRAME_LIMIT, true);
		} else {
			if (save) save->close(save);
			printf("mGBA: could not allocate save storage\n");
		}
		core->unloadROM(core);
	} else {
		rom->close(rom);
		printf("mGBA: could not load embedded ROM\n");
	}
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	return result;
}
