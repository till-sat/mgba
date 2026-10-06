/* SPDX-License-Identifier: MPL-2.0 */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba-util/vfs.h>

static struct GBA initial, expected;
static struct ARMCore initialCPU, expectedCPU;
static unsigned checks, accelerated, events;

static void quiet(struct mLogger* logger, int category, enum mLogLevel level,
                  const char* format, va_list args) {
	(void) logger; (void) category; (void) level; (void) format; (void) args;
}

static void event(struct ARMCore* cpu) { (void) cpu; ++events; }

static void rejectUnchanged(struct GBA* gba, const char* label) {
	initial = *gba;
	initialCPU = *gba->cpu;
	if (GBASkipIdleLoop(gba) || memcmp(gba, &initial, sizeof(*gba)) ||
	    memcmp(gba->cpu, &initialCPU, sizeof(initialCPU))) {
		fprintf(stderr, "%s was not rejected before execution\n", label);
		exit(1);
	}
	++checks;
}

static void check(struct GBA* gba, const char* label, unsigned trial, bool reject) {
	struct ARMCore* cpu = gba->cpu;
	initial = *gba;
	initialCPU = *cpu;
	events = 0;
	ARMRunLoop(cpu);
	expected = *gba;
	expectedCPU = *cpu;
	unsigned expectedEvents = events;
	*gba = initial;
	*cpu = initialCPU;
	events = 0;
	uint32_t skipped = GBASkipIdleLoop(gba);
	if (skipped) ++accelerated;
	ARMRunLoop(cpu);
	if ((reject && skipped) || events != expectedEvents ||
	    memcmp(gba, &expected, sizeof(*gba)) || memcmp(cpu, &expectedCPU, sizeof(*cpu))) {
		fprintf(stderr, "%s trial %u differs, skipped=%u, pc=%08X/%08X cycles=%d/%d\n",
		        label, trial, skipped, cpu->gprs[ARM_PC], expectedCPU.gprs[ARM_PC],
		        cpu->cycles, expectedCPU.cycles);
		exit(1);
	}
	*gba = initial;
	*cpu = initialCPU;
	++checks;
}

static void synthetic(struct mCore* core) {
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	gba->idleOptimization = IDLE_LOOP_REMOVE;
	/* Each loop ends in Bcc back to the first instruction. The last three
	 * cases must not skip: an evolving counter, another MMIO read, changed
	 * address register. Fill the surrounding code with B . for taken exits. */
	static const struct {
		const char* name;
		uint16_t code[8];
		unsigned count;
		bool reject;
	} cases[] = {
		{ "vcount compare", {0x8808, 0x289F, 0xD9FC}, 3, false },
		{ "dispstat mask", {0x8808, 0x4010, 0x2800, 0xD0FB}, 4, false },
		{ "register offset", {0x5A88, 0x2800, 0xD1FC}, 3, false },
		{ "shift and compare", {0x8808, 0x07C0, 0x2800, 0xD0FB}, 4, false },
		{ "counter", {0x8808, 0x3301, 0x2800, 0xD1FB}, 4, true },
		{ "timer", {0x8818, 0x2800, 0xD1FC}, 3, true },
		{ "address write", {0x8808, 0x3100, 0x2800, 0xD1FB}, 4, true },
	};
	static const uint32_t bases[] = {0x08000100, 0x0A000100, 0x0C000100, 0x03000100, 0x02000100};
	static const uint16_t waits[] = {0, 0x4000, 0x4317, 0x47FF};
	cpu->irqh.processEvents = event;
	for (unsigned c = 0; c < sizeof(cases) / sizeof(*cases); ++c) {
		for (unsigned b = 0; b < sizeof(bases) / sizeof(*bases); ++b) {
			uint32_t start = bases[b];
			uint32_t* code = b < 3 ? gba->memory.rom : b == 3 ? gba->memory.iwram : gba->memory.wram;
			for (unsigned i = 0; i < 128; ++i) code[i] = 0xE7FEE7FE;
			for (unsigned i = 0; i < cases[c].count; ++i) STORE_16(cases[c].code[i], 0x100 + 2 * i, code);
			for (unsigned w = 0; w < sizeof(waits) / sizeof(*waits); ++w) {
				GBAAdjustWaitstates(gba, waits[w]);
				for (unsigned trial = 0; trial < 512; ++trial) {
					for (unsigned r = 0; r < 16; ++r) cpu->gprs[r] = trial * 0x31415927U ^ r;
					cpu->cpsr.packed = ((trial & 15) << 28) | 0x3F;
					cpu->executionMode = MODE_THUMB;
					cpu->gprs[1] = GBA_BASE_IO + ((trial & 32) ? GBA_REG_DISPSTAT : GBA_REG_VCOUNT);
					cpu->gprs[2] = c == 2 ? 0 : 1;
					cpu->gprs[3] = GBA_BASE_IO + GBA_REG_TM0CNT_HI;
					gba->memory.io[GBA_REG(VCOUNT)] = trial % 228;
					gba->memory.io[GBA_REG(DISPSTAT)] = trial & 7;
					gba->haltPending = trial & 16;
					cpu->gprs[ARM_PC] = start + (trial % cases[c].count) * 2;
					ThumbWritePC(cpu);
					gba->lastJump = start;
					gba->idleLoop = start;
					gba->memory.lastPrefetchedPc = trial & 64 ? cpu->gprs[ARM_PC] + 8 : 0;
					cpu->cycles = 0;
					cpu->nextEvent = trial < 128 ? trial : 256 + trial;
					check(gba, cases[c].name, trial, cases[c].reject);
					if (trial < 128) {
						/* Stale pipeline after a code write. */
						cpu->prefetch[0] = 0x46C0;
						check(gba, "stale prefetch", trial, true);
					}
				}
			}
		}
	}
	uint32_t start = GBA_BASE_IWRAM + 0x100;
	for (unsigned i = 0; i < cases[0].count; ++i) STORE_16(cases[0].code[i], 0x100 + i * 2, gba->memory.iwram);
	cpu->executionMode = MODE_THUMB;
	cpu->cpsr.packed = 0x3F;
	cpu->gprs[ARM_PC] = start;
	cpu->gprs[1] = GBA_BASE_IO + GBA_REG_VCOUNT;
	ThumbWritePC(cpu);
	gba->lastJump = start;
	gba->idleLoop = start;
	cpu->cycles = 0;
	cpu->nextEvent = 512;
	struct ARMCore readyCPU = *cpu;
	struct GBA ready = *gba;
	for (unsigned trial = 0; trial < 10; ++trial) {
		*cpu = readyCPU;
		*gba = ready;
		switch (trial) {
		case 0: cpu->halted = 1; break;
		case 1: gba->cpuBlocked = true; break;
		case 2: gba->earlyExit = true; break;
		case 3: gba->idleOptimization = IDLE_LOOP_IGNORE; break;
		case 4: gba->idleOptimization = IDLE_LOOP_DETECT; break;
		case 5: cpu->executionMode = MODE_ARM; break;
		case 6: cpu->nextEvent = 0; break;
		case 7: cpu->gprs[1] |= 1; break;
		case 8: cpu->prefetch[1] = 0x3001; break;
		case 9: gba->lastJump |= 1; break;
		}
		rejectUnchanged(gba, "guard");
	}
	*cpu = readyCPU;
	*gba = ready;
	/* Patch code between invocations, as a DMA transfer or CPU write could.
	 * There is no cached proof that may survive this change. */
	GBASkipIdleLoop(gba);
	STORE_16(0x8008, 0x100, gba->memory.iwram); /* STRH */
	rejectUnchanged(gba, "patched store");
	*cpu = readyCPU;
	*gba = ready;
	uint32_t random = 0x12345678;
	for (unsigned trial = 0; trial < 4096; ++trial) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		*cpu = readyCPU;
		*gba = ready;
		uint16_t code[] = {
			0x8808,
			0x4000 | ((trial & 15) << 6) | (2 << 3),
			((trial % 3) << 11) | (((trial >> 4) & 31) << 6),
			0x2800 | (random & 255),
			0xD0FA | ((trial % 14) << 8),
		};
		for (unsigned i = 0; i < 5; ++i) STORE_16(code[i], 0x100 + i * 2, gba->memory.iwram);
		cpu->gprs[0] = random;
		cpu->gprs[2] = random ^ (random >> 16);
		cpu->cpsr.packed = (random & 0xF0000000) | 0x3F;
		cpu->gprs[ARM_PC] = start + (trial % 5) * 2;
		ThumbWritePC(cpu);
	gba->lastJump = start;
	gba->idleLoop = start;
		gba->memory.io[GBA_REG(VCOUNT)] = random % 228;
		cpu->nextEvent = 1 + (random & 1023);
		check(gba, "random ALU/flags/conditions", trial, (trial & 15) == 13);
	}
	/* Synthetic blocks still exercise rejection and state equivalence. Real
	 * ROM coverage below is the source of accepted idle-loop candidates. */
	printf("PASS: %u full CPU/GBA state comparisons at event boundaries; %u accelerated\n", checks, accelerated);
}

static struct mCore* openCore(const void* rom, size_t size) {
	struct mCore* core = GBACoreCreate();
	if (!core || !core->init(core)) exit(1);
	mCoreInitConfig(core, NULL);
	struct VFile* vf = VFileFromConstMemory(rom, size);
	if (!vf || !core->loadROM(core, vf)) exit(1);
	core->reset(core);
	return core;
}

static void closeCore(struct mCore* core) {
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
}

static void romCheck(const char* path, unsigned frames) {
	FILE* f = fopen(path, "rb");
	if (!f) exit(1);
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	rewind(f);
	void* rom = malloc(size);
	if (!rom || fread(rom, 1, size, f) != size) exit(1);
	fclose(f);
	struct mCore* cores[2] = { openCore(rom, size), openCore(rom, size) };
	struct GBASerializedState* states[2] = {calloc(1, sizeof(*states[0])), calloc(1, sizeof(*states[1]))};
	mColor* videos[2] = {calloc(240 * 160, sizeof(mColor)), calloc(240 * 160, sizeof(mColor))};
	for (unsigned i = 0; i < 2; ++i) cores[i]->setVideoBuffer(cores[i], videos[i], 240);
	uint64_t skipped = 0;
	unsigned hits = 0;
	for (unsigned frame = 0; frame < frames; ++frame) {
		for (unsigned i = 0; i < 2; ++i) {
			struct GBA* gba = cores[i]->board;
			unsigned counter = gba->video.frameCounter;
			uint32_t start = mTimingCurrentTime(&gba->timing);
			while (gba->video.frameCounter == counter && (uint32_t) mTimingCurrentTime(&gba->timing) - start < VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH) {
				if (i) {
					uint32_t amount = GBASkipIdleLoop(gba);
					skipped += amount;
					hits += amount != 0;
				}
				ARMRunLoop(gba->cpu);
			}
			cores[i]->saveState(cores[i], states[i]);
		}
		if (memcmp(states[0], states[1], sizeof(*states[0])) || memcmp(videos[0], videos[1], 240 * 160 * sizeof(mColor))) {
			fprintf(stderr, "%s frame %u state/video differs\n", path, frame);
			for (unsigned j = 0, shown = 0; j < sizeof(*states[0]) && shown < 20; ++j) {
				if (((uint8_t*)states[0])[j] != ((uint8_t*)states[1])[j]) {
					fprintf(stderr, "state[%X] %02X/%02X\n", j, ((uint8_t*)states[0])[j], ((uint8_t*)states[1])[j]);
					++shown;
				}
			}
			exit(1);
		}
	}
	printf("PASS: %s: %u frame-by-frame serialized states and images; %u skips, %llu guest cycles skipped\n", path, frames, hits, (unsigned long long) skipped);
	for (unsigned i = 0; i < 2; ++i) { free(states[i]); free(videos[i]); closeCore(cores[i]); }
	free(rom);
}

int main(int argc, char** argv) {
	struct mLogger logger = { .log = quiet };
	mLogSetDefaultLogger(&logger);
	if (argc > 1) {
		for (int i = 1; i < argc; ++i) romCheck(argv[i], 150);
		return 0;
	}
	uint32_t rom[256];
	for (unsigned i = 0; i < 256; ++i) rom[i] = 0xE7FEE7FE;
	struct mCore* core = openCore(rom, sizeof(rom));
	synthetic(core);
	closeCore(core);
	return 0;
}
