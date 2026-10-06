/* SPDX-License-Identifier: MPL-2.0 */
/* Compile the real memory implementation here to compare its fast paths with
 * the retained general paths, including private mapping and prefetch state. */
#include "gba/memory.c"
#include <mgba/core/core.h>
#include <mgba/gba/core.h>

static struct GBA before, expected;
static struct ARMCore cpuBefore, cpuExpected;
static unsigned checks, callbackCount;

static void quiet(struct mLogger* logger, int category, enum mLogLevel level,
                  const char* format, va_list args) {
	(void) logger; (void) category; (void) level; (void) format; (void) args;
}

static void callback(void* context) {
	(void) context;
	++callbackCount;
}

static void fail(const char* kind, uint32_t address, unsigned trial) {
	fprintf(stderr, "%s differs: address=%08X trial=%u\n", kind, address, trial);
	exit(1);
}

static void checkLoad(struct ARMCore* cpu, uint32_t address, unsigned trial) {
	struct GBA* gba = (struct GBA*) cpu->master;
	before = *gba;
	cpuBefore = *cpu;
	int cycles = 17, referenceCycles = 17;
	callbackCount = 0;
	uint32_t reference = _load16Slow(cpu, address, trial & 1 ? &referenceCycles : NULL);
	unsigned referenceCallbacks = callbackCount;
	expected = *gba;
	cpuExpected = *cpu;
	*gba = before;
	*cpu = cpuBefore;
	callbackCount = 0;
	uint32_t actual = GBALoad16(cpu, address, trial & 1 ? &cycles : NULL);
	if (actual != reference || cycles != referenceCycles || callbackCount != referenceCallbacks ||
	    memcmp(gba, &expected, sizeof(*gba)) || memcmp(cpu, &cpuExpected, sizeof(*cpu))) {
		fail("Load16 value/cycles/side effects", address, trial);
	}
	*gba = before;
	*cpu = cpuBefore;
	++checks;
}

static void checkLoad8(struct ARMCore* cpu, uint32_t address, unsigned trial) {
	struct GBA* gba = (struct GBA*) cpu->master;
	before = *gba;
	cpuBefore = *cpu;
	int cycles = 17, referenceCycles = 17;
	uint32_t reference = _load8Slow(cpu, address, trial & 1 ? &referenceCycles : NULL);
	expected = *gba;
	cpuExpected = *cpu;
	*gba = before;
	*cpu = cpuBefore;
	uint32_t actual = GBALoad8(cpu, address, trial & 1 ? &cycles : NULL);
	if (actual != reference || cycles != referenceCycles ||
	    memcmp(gba, &expected, sizeof(*gba)) || memcmp(cpu, &cpuExpected, sizeof(*cpu))) {
		fail("Load8 value/cycles/side effects", address, trial);
	}
	*gba = before;
	*cpu = cpuBefore;
	++checks;
}

static void checkStore16(struct ARMCore* cpu, uint32_t address, unsigned trial) {
	struct GBA* gba = (struct GBA*) cpu->master;
	int16_t value = (int16_t) (0x1357U * trial + address);
	before = *gba;
	cpuBefore = *cpu;
	int cycles = 17, referenceCycles = 17;
	_store16Slow(cpu, address, value, trial & 1 ? &referenceCycles : NULL);
	expected = *gba;
	cpuExpected = *cpu;
	*gba = before;
	*cpu = cpuBefore;
	GBAStore16(cpu, address, value, trial & 1 ? &cycles : NULL);
	if (cycles != referenceCycles || memcmp(gba, &expected, sizeof(*gba)) ||
	    memcmp(cpu, &cpuExpected, sizeof(*cpu))) {
		fail("Store16 value/cycles/side effects", address, trial);
	}
	*gba = before;
	*cpu = cpuBefore;
	++checks;
}

static void checkPrefetch(struct ARMCore* cpu) {
	struct GBA* gba = (struct GBA*) cpu->master;
	static const uint32_t pcs[] = { 0, 0x08000102, 0xFFFFFFFE };
	for (unsigned p = 0; p < sizeof(pcs) / sizeof(*pcs); ++p) {
		cpu->gprs[ARM_PC] = pcs[p];
		for (int region = -1; region <= GBA_REGION_ROM2_EX; ++region) {
			gba->memory.activeRegion = region;
			for (unsigned prefetch = 0; prefetch < 2; ++prefetch) {
				gba->memory.prefetch = prefetch;
				for (unsigned s = 0; s <= 8; ++s) {
					cpu->memory.activeSeqCycles16 = s;
					for (unsigned n = 0; n <= 8; ++n) {
						cpu->memory.activeNonseqCycles16 = n;
						for (int dist = -4; dist <= 20; ++dist) {
							for (unsigned offset = 0; offset < 4; ++offset) {
								uint32_t address = GBA_BASE_IWRAM + 4 + offset;
								uint32_t last = pcs[p] + dist;
								gba->memory.lastPrefetchedPc = last;
								int referenceCycles = 17, actualCycles = 17;
								uint32_t reference = _load16Slow(cpu, address, &referenceCycles);
								uint32_t referenceLast = gba->memory.lastPrefetchedPc;
								gba->memory.lastPrefetchedPc = last;
								uint32_t actual = GBALoad16(cpu, address, &actualCycles);
								if (reference != actual || referenceCycles != actualCycles ||
								    referenceLast != gba->memory.lastPrefetchedPc) {
									fail("IWRAM prefetch cycles/state", address, checks);
								}
								++checks;
							}
						}
					}
				}
			}
		}
	}
}

int main(void) {
	struct mLogger logger = { .log = quiet };
	mLogSetDefaultLogger(&logger);
	struct mCore* core = GBACoreCreate();
	if (!core || !core->init(core)) return 1;
	mCoreInitConfig(core, NULL);
	/* A synthetic ROM keeps these comparisons independent of any game. */
	uint32_t rom[256];
	for (unsigned i = 0; i < sizeof(rom) / sizeof(*rom); ++i) rom[i] = 0xE7FEE7FE;
	struct VFile* vf = VFileFromConstMemory(rom, sizeof(rom));
	if (!vf || !core->loadROM(core, vf)) return 1;
	core->reset(core);
	struct ARMCore* cpu = core->cpu;
	struct GBA* gba = core->board;
	struct mCoreCallbacks callbacks = { .keysRead = callback, .coreCrashed = callback };
	core->addCoreCallbacks(core, &callbacks);
	for (unsigned i = 0; i < GBA_SIZE_IWRAM / 4; ++i) gba->memory.iwram[i] = i * 0x9E3779B9U;
	for (unsigned i = 0; i < GBA_SIZE_EWRAM / 4; ++i) gba->memory.wram[i] = i * 0x31415927U;
	struct GBA initial = *gba;
	struct ARMCore initialCPU = *cpu;
	checkPrefetch(cpu);
	*gba = initial;
	*cpu = initialCPU;
	static const uint32_t addresses[] = {
		0, 1, 0x3FFE, 0x4000, 0x01000000, 0x02000000, 0x0203FFFF,
		0x02FFFFFF, 0x03000000, 0x03000001, 0x03007FFE, 0x03007FFF,
		0x03008000, 0x03FFFFFE, 0x03FFFFFF, 0x040003FE, 0x04000400,
		0x04010000, 0x04FFFFFE, 0x05000000, 0x05FFFFFF, 0x06000000,
		0x06017FFF, 0x06018000, 0x0601FFFF, 0x07000000, 0x07FFFFFF,
		0x08000000, 0x080003FF, 0x08000400, 0x09000000, 0x0A000003,
		0x0C000002, 0x0D000002, 0x10000000, 0xFFFFFFFF
	};
	static const uint32_t ramAddresses[] = {
		0x02000000, 0x02000001, 0x0203FFFE, 0x0203FFFF, 0x02FFFFFF,
		0x03000000, 0x03000001, 0x03007FFE, 0x03007FFF, 0x03FFFFFF,
	};
	for (unsigned trial = 0; trial < 128; ++trial) {
		gba->memory.activeRegion = trial & 8 ? GBA_REGION_ROM0 : GBA_REGION_IWRAM;
		gba->memory.prefetch = trial & 2;
		cpu->gprs[ARM_PC] = 0x08000102;
		gba->memory.lastPrefetchedPc = cpu->gprs[ARM_PC] + (trial % 24) - 4;
		cpu->memory.activeSeqCycles16 = (trial >> 2) % 9;
		cpu->memory.activeNonseqCycles16 = (trial >> 3) % 9;
		gba->haltPending = true;
		gba->memory.io[GBA_REG(JOYSTAT)] = 0xFFFF;
		for (unsigned i = 0; i < GBA_REG_BG0CNT / 2; ++i) {
			gba->memory.io[i] = (trial * 0x9E37U) ^ (i * 0x4321U);
		}
		for (unsigned i = 0; i < sizeof(addresses) / sizeof(*addresses); ++i) {
			checkLoad(cpu, addresses[i], trial);
		}
		for (unsigned i = 0; i < sizeof(ramAddresses) / sizeof(*ramAddresses); ++i) {
			checkLoad8(cpu, ramAddresses[i], trial);
			checkStore16(cpu, ramAddresses[i], trial);
		}
		/* Includes timers, keys, write-only registers, wave RAM and JOY receive. */
		for (uint32_t offset = 0; offset < GBA_SIZE_IO; ++offset) {
			checkLoad(cpu, GBA_BASE_IO + offset, trial);
		}
	}
	*gba = initial;
	*cpu = initialCPU;
	core->clearCoreCallbacks(core);
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	mLogSetDefaultLogger(NULL);
	printf("PASS: %u GBA memory comparisons (values, cycles, IO effects, prefetch and address mirrors)\n", checks);
	return 0;
}
