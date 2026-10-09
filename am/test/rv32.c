/* SPDX-License-Identifier: MPL-2.0 */
/* Execute emitted RV32 instructions on Spike and compare with the original
 * ARM/Thumb handlers. Including the emitter keeps its private test API private. */
#include "../../src/arm/rv32.c"
#include <mgba/core/log.h>
#include <stdio.h>
#include <stdlib.h>

#if !RV32_NATIVE
#error This test must execute on RV32, not the host fallback
#endif

static void quietLog(struct mLogger* logger, int category, enum mLogLevel level,
                     const char* format, va_list args) {
	UNUSED(logger); UNUSED(category); UNUSED(level); UNUSED(format); UNUSED(args);
}

static bool sharedDataTest;
static void selectDataHelpers(struct RV32Block* block) {
	static struct RV32Context* context;
	if (!sharedDataTest) return;
	if (!context) {
		context = calloc(1, sizeof(*context));
		if (!context || !_emitDataHelpers(context)) { puts("FAIL shared data setup"); exit(1); }
		context->dataReady = true;
	}
	block->context = context;
	block->sharedData = true;
}

static uint32_t seed = 0x59312D87;
static uint32_t randomWord(void) {
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static void checkNative(const struct RV32Block* block, unsigned start, unsigned trial) {
	static const uint32_t edge[] = {0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000,
		0x80000001, 31, 32, 33, 255, 256, 0xFFFFFF00, 0x55555555, 0xAAAAAAAA, 0xFFFF, 0x10000};
	struct ARMCore actual = {0};
	for (unsigned r = 0; r < 16; ++r) {
		actual.gprs[r] = trial < 16 ? edge[(trial + r) & 15] : randomWord();
	}
	actual.gprs[ARM_PC] = 0x08000102;
	actual.cpsr.packed = ((trial & 15) << 28) | 0x0123453F;
	actual.spsr.packed = randomWord();
	actual.executionMode = MODE_THUMB;
	actual.memory.activeSeqCycles16 = 2;
	actual.cycles = 7;
	actual.nextEvent = actual.cycles + block->spans[start].cycles;
	actual.prefetch[0] = block->opcodes[start];
	actual.prefetch[1] = block->opcodes[start + 1];
	struct ARMCore expected = actual;
	for (unsigned i = start; i < start + block->spans[start].length; ++i) {
		expected.prefetch[0] = block->opcodes[i + 1];
		expected.prefetch[1] = block->opcodes[i + 2];
		expected.gprs[ARM_PC] += 2;
		_thumbTable[block->opcodes[i] >> 6](&expected, block->opcodes[i]);
	}
	bool executed = block->resident ? ((bool (*)(struct ARMCore*)) (uintptr_t) block->nativeCode)(&actual) :
		_executeNativeBlock(&actual, block, start);
	if (!executed || memcmp(&actual, &expected, sizeof(actual))) {
		printf("FAIL opcode=%04x length=%u trial=%u cpsr=%08lx/%08lx cycles=%ld/%ld\n",
		       block->opcodes[start], block->spans[start].length, trial,
		       (unsigned long) actual.cpsr.packed, (unsigned long) expected.cpsr.packed,
		       (long) actual.cycles, (long) expected.cycles);
		for (unsigned r = 0; r < 16; ++r) {
			if (actual.gprs[r] != expected.gprs[r]) printf("r%u=%08lx/%08lx\n", r,
				(unsigned long) actual.gprs[r], (unsigned long) expected.gprs[r]);
		}
		exit(1);
	}
	if (block->resident) return;
	/* Refuse a native block if an event falls within it; state must be untouched. */
	actual.nextEvent = actual.cycles + block->spans[start].cycles - 1;
	expected = actual;
	if (_executeNativeBlock(&actual, block, start) || memcmp(&actual, &expected, sizeof(actual))) {
		puts("FAIL event guard changed state");
		exit(1);
	}
}

static void events(struct ARMCore* cpu) { cpu->nextEvent = INT32_MAX; ++cpu->halted; }
static void region(struct ARMCore* cpu, uint32_t address) { UNUSED(cpu); UNUSED(address); }
static uint32_t read16(struct ARMCore* cpu, uint32_t address, int* cycles) {
	UNUSED(cpu);
	*cycles += 2;
	return address & 0xFFFF;
}
static uint32_t read32(struct ARMCore* cpu, uint32_t address, int* cycles) {
	UNUSED(cpu);
	*cycles += 3;
	return address ^ 0x89ABCDEF;
}
static uint32_t read8(struct ARMCore* cpu, uint32_t address, int* cycles) {
	UNUSED(cpu);
	*cycles += 1;
	return address & 0xFF;
}

static void checkLoads(void) {
	static struct RV32Block block;
	struct ARMCore configuration = {0};
	configuration.memory.activeSeqCycles16 = 2;
	unsigned encodings = 0, comparisons = 0;
	for (unsigned opcode = 0; opcode <= 0xFFFF; ++opcode) {
		memset(&block, 0, sizeof(block));
		block.start = 0x08000100;
		block.length = 1;
		block.opcodes[0] = opcode;
		block.opcodes[1] = 0x1234;
		block.opcodes[2] = 0x5678;
		if (!_emitMemoryLoad(&block, 0, &configuration)) continue;
		if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL load emission"); exit(1); }
		++encodings;
		for (unsigned trial = 0; trial < 16; ++trial) {
			struct ARMCore actual = {0};
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.gprs[ARM_PC] = block.start + 2;
			actual.prefetch[0] = opcode;
			actual.prefetch[1] = block.opcodes[1];
			actual.cpsr.packed = randomWord();
			actual.cycles = trial;
			actual.nextEvent = trial + 1;
			actual.memory.activeSeqCycles16 = trial & 3;
			actual.memory.activeNonseqCycles16 = trial & 7;
			actual.memory.load8 = read8;
			actual.memory.load16 = read16;
			actual.memory.load32 = read32;
			struct ARMCore expected = actual;
			expected.prefetch[0] = block.opcodes[1];
			expected.prefetch[1] = block.opcodes[2];
			expected.gprs[ARM_PC] += 2;
			_thumbTable[opcode >> 6](&expected, opcode);
			bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
			if (!complete || memcmp(&actual, &expected, sizeof(actual))) {
				printf("FAIL load opcode=%04x trial=%u cycles=%ld/%ld\n", opcode, trial,
				       (long) actual.cycles, (long) expected.cycles);
				exit(1);
			}
			++comparisons;
		}
	}
	printf("PASS: %u load encodings, %u native/interpreter state comparisons\n", encodings, comparisons);
}

/* Real GBA callbacks are the oracle for RAM mirrors, unaligned rotations,
 * sign extension, waitstates and ROM prefetch state. Compile with the normal
 * callbacks, then also replace them to exercise generated runtime guards. */
static void checkRAMLoads(void) {
	static const uint16_t opcodes[] = {0x5888, 0x5A88, 0x5C88, 0x5688, 0x5E88,
		0x6808, 0x8808, 0x7808, 0x9800, 0x5889, 0x588A};
	static const uint32_t addresses[] = {0x02000000, 0x02000001, 0x0203FFFE, 0x0203FFFF,
		0x02FFFFFE, 0x02FFFFFF, 0x03000000, 0x03000001, 0x03007FFE, 0x03007FFF,
		0x03FFFFFE, 0x03FFFFFF, 0x04000006, 0x08000010};
	static const int distances[] = {-4, -1, 0, 1, 2, 14, 15, 16, 32};
	static struct GBA gba, before, expectedGBA;
	static uint32_t wram[GBA_SIZE_EWRAM / 4], iwram[GBA_SIZE_IWRAM / 4], rom[256];
	static struct RV32Block block;
	for (unsigned i = 0; i < GBA_SIZE_EWRAM / 4; ++i) wram[i] = randomWord();
	for (unsigned i = 0; i < GBA_SIZE_IWRAM / 4; ++i) iwram[i] = randomWord();
	for (unsigned i = 0; i < 256; ++i) rom[i] = randomWord();
	struct ARMCore configuration = {0};
	configuration.memory.load8 = GBALoad8;
	configuration.memory.load16 = GBALoad16;
	configuration.memory.load32 = GBALoad32;
	unsigned checks = 0;
	for (unsigned op = 0; op < sizeof(opcodes) / sizeof(*opcodes); ++op) {
		memset(&block, 0, sizeof(block));
		selectDataHelpers(&block);
		block.start = 0x08000100;
		block.length = 1;
		block.opcodes[0] = opcodes[op];
		block.opcodes[1] = 0x2707;
		block.opcodes[2] = 0xE7FE;
		if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL RAM emission"); exit(1); }
		for (unsigned trial = 0; trial < 128; ++trial) {
			for (unsigned a = 0; a < sizeof(addresses) / sizeof(*addresses); ++a) {
				memset(&gba, 0, sizeof(gba));
				gba.memory.wram = wram;
				gba.memory.iwram = iwram;
				gba.memory.rom = rom;
				gba.memory.romSize = sizeof(rom);
				gba.haltPending = true;
				gba.memory.prefetch = trial >> 6;
				gba.memory.activeRegion = trial % 3 ? GBA_REGION_ROM0 : GBA_REGION_EWRAM;
				gba.memory.waitstatesNonseq16[GBA_REGION_EWRAM] = (trial * 3) & 15;
				gba.memory.waitstatesNonseq32[GBA_REGION_EWRAM] = (trial * 5) & 31;
				gba.memory.lastPrefetchedPc = block.start + 4 + distances[(trial + a) % 9];
				struct ARMCore actual = configuration;
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.master = &gba.d;
				actual.gprs[ARM_PC] = block.start + 2;
				actual.gprs[1] = addresses[a];
				actual.gprs[2] = 0;
				actual.gprs[ARM_SP] = addresses[a];
				actual.prefetch[0] = block.opcodes[0];
				actual.prefetch[1] = block.opcodes[1];
				actual.cpsr.packed = randomWord();
				actual.cycles = (int) trial - 13;
				actual.nextEvent = actual.cycles + 1;
				actual.memory.activeSeqCycles16 = trial & 7;
				actual.memory.activeNonseqCycles16 = (trial >> 3) & 7;
				if (trial % 17 == 0) {
					actual.memory.load8 = read8;
					actual.memory.load16 = read16;
					actual.memory.load32 = read32;
				}
				before = gba;
				struct ARMCore expected = actual;
				expected.prefetch[0] = block.opcodes[1];
				expected.prefetch[1] = block.opcodes[2];
				expected.gprs[ARM_PC] += 2;
				_thumbTable[block.opcodes[0] >> 6](&expected, block.opcodes[0]);
				expectedGBA = gba;
				gba = before;
				bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
				if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL RAM opcode=%04x address=%08lx trial=%u cycles=%ld/%ld prefetch=%08lx/%08lx\n",
					       block.opcodes[0], (unsigned long) addresses[a], trial, (long) actual.cycles,
					       (long) expected.cycles, (unsigned long) gba.memory.lastPrefetchedPc,
					       (unsigned long) expectedGBA.memory.lastPrefetchedPc);
					exit(1);
				}
				++checks;
			}
		}
	}
	printf("PASS: %u real RAM/IO/ROM load states, mirrors, alignment, waitstates, prefetch and callback replacement\n", checks);
}

static uint16_t ioTestKeys(struct mKeyCallback* callback) {
	UNUSED(callback);
	return 0x00F3;
}

/* Scan every byte of the IO window using every load width/signedness, then
 * exercise the distinct byte/halfword/word aliases outside that window.
 * The original Thumb handler + GBAIORead are the oracle, including timers,
 * input callbacks, wave RAM, audio-enable gating and write-only/open bus. */
static void checkIOLoads(void) {
	static const uint16_t opcodes[] = {0x5888, 0x5A88, 0x5C88, 0x5688, 0x5E88, 0x5889, 0x5A8A};
	static const uint32_t aliases[] = {0x04000400, 0x04000401, 0x04000800, 0x04000803,
		0x04010000, 0x04010001, 0x04010004, 0x04010007, 0x04010200, 0x04010203,
		0x04FF0006, 0x04FF0130, 0x04FFFFFE, 0x04FFFFFF, 0x05000000};
	static struct GBA gba, before, expectedGBA;
	static struct RV32Block block;
	static struct mKeyCallback keys = {.readKeys = ioTestKeys};
	static const int distances[] = {-4, -1, 0, 1, 2, 14, 15, 16, 32};
	struct ARMCore configuration = {0};
	configuration.memory.load8 = GBALoad8;
	configuration.memory.load16 = GBALoad16;
	configuration.memory.load32 = GBALoad32;
	unsigned checks = 0;
	for (unsigned op = 0; op < sizeof(opcodes) / sizeof(*opcodes); ++op) {
		memset(&block, 0, sizeof(block));
		selectDataHelpers(&block);
		block.start = 0x08000100;
		block.length = 1;
		block.opcodes[0] = opcodes[op];
		block.opcodes[1] = 0x2707;
		block.opcodes[2] = 0xE7FE;
		if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL IO emission"); exit(1); }
		for (unsigned trial = 0; trial < 8; ++trial) {
			for (unsigned a = 0; a < GBA_SIZE_IO + sizeof(aliases) / sizeof(*aliases); ++a) {
				uint32_t address = a < GBA_SIZE_IO ? GBA_BASE_IO + a : aliases[a - GBA_SIZE_IO];
				memset(&gba, 0, sizeof(gba));
				for (unsigned i = 0; i < GBA_SIZE_IO / 2; ++i) gba.memory.io[i] = (i * 3137) ^ (trial * 259);
				gba.haltPending = true;
				gba.memory.prefetch = trial & 1;
				gba.memory.activeRegion = trial % 3 ? GBA_REGION_ROM0 : GBA_REGION_IWRAM;
				gba.memory.lastPrefetchedPc = block.start + 4 + distances[(trial + a) % 9];
				gba.memory.io[GBA_REG(SOUNDCNT_X)] = trial & 2 ? 0x80 : 0;
				gba.memory.io[GBA_REG(JOYSTAT)] = 0xFFFF;
				gba.keyCallback = &keys;
				gba.allowOpposingDirections = trial & 4;
				gba.sio.siocnt = 0xDEAD;
				gba.sio.rcnt = 0xFACE;
				gba.audio.psg.timing = &gba.timing;
				gba.audio.psg.timingFactor = 4;
				gba.audio.psg.style = GB_AUDIO_GBA;
				gba.audio.psg.enable = trial & 2;
				gba.audio.psg.playingCh3 = trial & 2;
				gba.audio.psg.ch3.rate = 2047;
				gba.audio.psg.ch3.volume = 1;
				gba.audio.enable = trial & 2;
				gba.audio.psg.ch3.bank = trial & 1;
				for (unsigned i = 0; i < 8; ++i) gba.audio.psg.ch3.wavedata32[i] = 0x89ABCDEF ^ (i * 0x1010101);
				for (unsigned i = 0; i < 4; ++i) {
					gba.timers[i].flags = trial & 2 ? GBATimerFlagsFillEnable(0) : 0;
					gba.timers[i].reload = 0x1000 + i * 64;
					gba.timers[i].lastEvent = 1;
				}
				struct ARMCore actual = configuration;
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.master = &gba.d;
				gba.cpu = &actual;
				actual.gprs[ARM_PC] = block.start + 2;
				actual.gprs[1] = address;
				actual.gprs[2] = 0;
				actual.prefetch[0] = block.opcodes[0];
				actual.prefetch[1] = block.opcodes[1];
				actual.executionMode = MODE_THUMB;
				actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
				actual.cycles = trial + 13;
				actual.nextEvent = actual.cycles + 1;
				actual.memory.activeSeqCycles16 = trial & 3;
				actual.memory.activeNonseqCycles16 = (trial * 3) & 7;
				if (trial == 7) {
					actual.memory.load8 = read8;
					actual.memory.load16 = read16;
					actual.memory.load32 = read32;
				}
				mTimingInit(&gba.timing, &actual.cycles, &actual.nextEvent);
				before = gba;
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				gba.timing.relativeCycles = &expected.cycles;
				gba.timing.nextEvent = &expected.nextEvent;
				expected.prefetch[0] = block.opcodes[1];
				expected.prefetch[1] = block.opcodes[2];
				expected.gprs[ARM_PC] += 2;
				_thumbTable[block.opcodes[0] >> 6](&expected, block.opcodes[0]);
				expectedGBA = gba;
				expectedGBA.cpu = &actual;
				expectedGBA.timing.relativeCycles = &actual.cycles;
				expectedGBA.timing.nextEvent = &actual.nextEvent;
				gba = before;
				bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
				if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL IO opcode=%04x address=%08lx trial=%u cycles=%ld/%ld halt=%u/%u\n", block.opcodes[0],
					       (unsigned long) address, trial, (long) actual.cycles, (long) expected.cycles,
					       gba.haltPending, expectedGBA.haltPending);
					exit(1);
				}
				++checks;
			}
		}
	}
	printf("PASS: %u IO load states, full window, mixed-width aliases, signed/unaligned loads, idle state and special callbacks\n", checks);
}

static void changedStore(struct ARMCore* cpu, uint32_t address, int32_t value, int* cycles) {
	cpu->gprs[3] ^= address ^ value;
	cpu->cpsr.packed ^= 0x90000000;
	*cycles += 3;
}
static void changedStore16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycles) {
	changedStore(cpu, address, value, cycles);
}
static void changedStore8(struct ARMCore* cpu, uint32_t address, int8_t value, int* cycles) {
	changedStore(cpu, address, value, cycles);
}

static void checkRAMStores(void) {
	static const uint16_t opcodes[] = {0x5088, 0x5288, 0x5488, 0x6008, 0x8008, 0x7008,
		0x9000, 0x5089, 0x528A};
	static const uint32_t addresses[] = {0x02000000, 0x02000001, 0x0203FFFE, 0x0203FFFF,
		0x02FFFFFE, 0x02FFFFFF, 0x03000000, 0x03000001, 0x03007FFE, 0x03007FFF,
		0x03FFFFFE, 0x03FFFFFF, 0x04000204, 0x04000205, 0x10000000};
	static const int distances[] = {-4, -1, 0, 1, 2, 14, 15, 16, 32};
	static struct GBA gba, before, expectedGBA;
	static uint32_t wram[2][GBA_SIZE_EWRAM / 4], iwram[2][GBA_SIZE_IWRAM / 4], rom[256];
	static struct RV32Block block;
	for (unsigned i = 0; i < GBA_SIZE_EWRAM / 4; ++i) wram[0][i] = wram[1][i] = randomWord();
	for (unsigned i = 0; i < GBA_SIZE_IWRAM / 4; ++i) iwram[0][i] = iwram[1][i] = randomWord();
	struct ARMCore configuration = {0};
	configuration.memory.store8 = GBAStore8;
	configuration.memory.store16 = GBAStore16;
	configuration.memory.store32 = GBAStore32;
	unsigned checks = 0;
	for (unsigned op = 0; op < sizeof(opcodes) / sizeof(*opcodes); ++op) {
		memset(&block, 0, sizeof(block));
		selectDataHelpers(&block);
		block.start = 0x08000100;
		block.length = 1;
		block.opcodes[0] = opcodes[op];
		block.opcodes[1] = 0x2707;
		block.opcodes[2] = 0xE7FE;
		if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL RAM store emission"); exit(1); }
		for (unsigned trial = 0; trial < 128; ++trial) {
			for (unsigned a = 0; a < sizeof(addresses) / sizeof(*addresses); ++a) {
				memset(&gba, 0, sizeof(gba));
				gba.memory.wram = wram[0];
				gba.memory.iwram = iwram[0];
				gba.memory.rom = rom;
				gba.memory.prefetch = trial >> 6;
				gba.memory.activeRegion = trial % 3 ? GBA_REGION_ROM0 : GBA_REGION_EWRAM;
				gba.memory.waitstatesNonseq16[GBA_REGION_EWRAM] = (trial * 3) & 15;
				gba.memory.waitstatesNonseq32[GBA_REGION_EWRAM] = (trial * 5) & 31;
				gba.memory.lastPrefetchedPc = block.start + 4 + distances[(trial + a) % 9];
				struct ARMCore actual = configuration;
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.master = &gba.d;
				gba.cpu = &actual;
				actual.gprs[ARM_PC] = block.start + 2;
				actual.gprs[1] = addresses[a];
				actual.gprs[2] = 0;
				actual.gprs[ARM_SP] = addresses[a];
				actual.prefetch[0] = block.opcodes[0];
				actual.prefetch[1] = block.opcodes[1];
				actual.cpsr.packed = randomWord();
				actual.cycles = (int) trial - 13;
				actual.nextEvent = actual.cycles + 1;
				actual.memory.activeSeqCycles16 = trial & 7;
				actual.memory.activeNonseqCycles16 = (trial >> 3) & 7;
				if (trial % 17 == 0) {
					actual.memory.store8 = changedStore8;
					actual.memory.store16 = changedStore16;
					actual.memory.store32 = changedStore;
				}
				uint32_t* word = addresses[a] >> BASE_OFFSET == GBA_REGION_EWRAM ?
					&wram[0][(addresses[a] & (GBA_SIZE_EWRAM - 4)) / 4] :
					addresses[a] >> BASE_OFFSET == GBA_REGION_IWRAM ?
					&iwram[0][(addresses[a] & (GBA_SIZE_IWRAM - 4)) / 4] : NULL;
				uint32_t oldWord = word ? *word : 0;
				before = gba;
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				expected.prefetch[0] = block.opcodes[1];
				expected.prefetch[1] = block.opcodes[2];
				expected.gprs[ARM_PC] += 2;
				_thumbTable[block.opcodes[0] >> 6](&expected, block.opcodes[0]);
				uint32_t expectedWord = word ? *word : 0;
				if (word) *word = oldWord;
				expectedGBA = gba;
				expectedGBA.cpu = &actual;
				gba = before;
				bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
				if (!complete || (word && *word != expectedWord) || memcmp(&actual, &expected, sizeof(actual)) ||
				    memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL RAM store opcode=%04x address=%08lx trial=%u cycles=%ld/%ld\n",
					       block.opcodes[0], (unsigned long) addresses[a], trial, (long) actual.cycles, (long) expected.cycles);
					exit(1);
				}
				if (word) *word = oldWord;
				++checks;
			}
		}
	}
	if (memcmp(wram[0], wram[1], sizeof(wram[0])) || memcmp(iwram[0], iwram[1], sizeof(iwram[0]))) {
		puts("FAIL RAM store modified memory outside its destination"); exit(1);
	}
	printf("PASS: %u RAM store states, aliases, alignment, mirroring, prefetch, WAITCNT and callback replacement\n", checks);
}

static void checkLiteralLoads(void) {
	static uint32_t rom[2][256];
	static struct GBA gba, before, expectedGBA;
	static struct RV32Block block;
	for (unsigned r = 0; r < 2; ++r)
		for (unsigned i = 0; i < 256; ++i) rom[r][i] = randomWord();
	struct ARMCore configuration = {0};
	configuration.memory.load32 = GBALoad32;
	unsigned checks = 0;
	for (unsigned opcode = 0x4800; opcode < 0x5000; ++opcode) {
		for (unsigned trial = 0; trial < 16; ++trial) {
			memset(&block, 0, sizeof(block));
			block.start = trial == 15 ? 0x07000100 : 0x08000100 + (trial % 6) * 0x01000000;
			block.length = 1;
			block.opcodes[0] = opcode;
			block.opcodes[1] = 0x2707;
			block.opcodes[2] = 0xE7FE;
			if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL literal emission"); exit(1); }
			memset(&gba, 0, sizeof(gba));
			gba.memory.rom = rom[trial & 1];
			gba.memory.romSize = trial % 4 ? sizeof(rom[0]) : 2;
			gba.memory.prefetch = trial & 2;
			gba.memory.activeRegion = block.start >> BASE_OFFSET;
			gba.memory.lastPrefetchedPc = randomWord();
			for (unsigned r = 8; r <= 13; ++r) gba.memory.waitstatesNonseq32[r] = (trial + r) & 15;
			struct ARMCore actual = configuration;
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.master = &gba.d;
			actual.gprs[ARM_PC] = block.start + 2;
			actual.prefetch[0] = opcode;
			actual.prefetch[1] = block.opcodes[1];
			actual.cpsr.packed = randomWord();
			actual.cycles = (int) trial - 13;
			actual.nextEvent = actual.cycles + 1;
			actual.memory.activeSeqCycles16 = trial & 3;
			actual.memory.activeNonseqCycles16 = trial & 7;
			if (trial == 14) actual.memory.load32 = read32;
			before = gba;
			struct ARMCore expected = actual;
			expected.prefetch[0] = block.opcodes[1];
			expected.prefetch[1] = block.opcodes[2];
			expected.gprs[ARM_PC] += 2;
			_thumbTable[opcode >> 6](&expected, opcode);
			expectedGBA = gba;
			gba = before;
			bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
			if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
				printf("FAIL literal opcode=%04x trial=%u cycles=%ld/%ld\n", opcode, trial,
				       (long) actual.cycles, (long) expected.cycles);
				exit(1);
			}
			++checks;
		}
	}
	printf("PASS: %u literal load states, all offsets/destinations, ROM mirrors, replacement, bounds and callbacks\n", checks);
}

static void branchRegion(struct ARMCore* cpu, uint32_t address) {
	cpu->shifterCarryOut = address ^ (uint32_t) cpu->gprs[ARM_PC] ^ cpu->prefetch[0] ^
		cpu->prefetch[1] ^ cpu->cpsr.packed ^ (uint32_t) cpu->cycles;
	cpu->memory.activeSeqCycles16 ^= 2;
	cpu->memory.activeNonseqCycles16 ^= 3;
}

static void checkBranches(void) {
	static struct RV32Block block;
	static uint32_t code[256];
	for (unsigned i = 0; i < 256; ++i) code[i] = randomWord();
	struct ARMCore configuration = {0};
	configuration.memory.activeSeqCycles16 = 2;
	unsigned encodings = 0, comparisons = 0;
	for (unsigned opcode = 0; opcode <= 0xFFFF; ++opcode) {
		memset(&block, 0, sizeof(block));
		block.start = 0x08000100;
		block.length = 1;
		block.opcodes[0] = opcode;
		block.opcodes[1] = 0x1234;
		block.opcodes[2] = 0x5678;
		if (!_emitBranch(&block, 0)) continue;
		if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL branch emission"); exit(1); }
		++encodings;
		for (unsigned flags = 0; flags < 16; ++flags) {
			struct ARMCore actual = {0};
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.gprs[ARM_PC] = block.start + 2;
			actual.prefetch[0] = opcode;
			actual.prefetch[1] = block.opcodes[1];
			actual.cpsr.packed = (flags << 28) | 0x1234;
			actual.cycles = flags;
			actual.nextEvent = flags + 1;
			actual.memory.activeRegion = code;
			actual.memory.activeMask = sizeof(code) - 2;
			actual.memory.activeSeqCycles16 = flags & 3;
			actual.memory.activeNonseqCycles16 = flags & 7;
			actual.memory.setActiveRegion = branchRegion;
			struct ARMCore expected = actual;
			expected.prefetch[0] = block.opcodes[1];
			expected.prefetch[1] = block.opcodes[2];
			expected.gprs[ARM_PC] += 2;
			_thumbTable[opcode >> 6](&expected, opcode);
			bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
			if (!complete || memcmp(&actual, &expected, sizeof(actual))) {
				printf("FAIL branch opcode=%04x flags=%u pc=%08lx/%08lx cycles=%ld/%ld\n", opcode, flags,
				       (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
				       (long) actual.cycles, (long) expected.cycles);
				exit(1);
			}
			++comparisons;
		}
	}
	printf("PASS: %u branch encodings, %u native/interpreter state comparisons\n", encodings, comparisons);
}

static void checkStandardBranches(void) {
	static uint32_t code[256];
	static struct GBA gba, before, expectedGBA;
	static struct RV32Block block;
	for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
	struct ARMCore configuration = {0};
	unsigned checks = 0;
	for (unsigned condition = 0; condition < 15; ++condition) {
		for (unsigned trial = 0; trial < 32; ++trial) {
			memset(&block, 0, sizeof(block));
			block.start = 0x08000100 + (trial % 3) * 0x02000000;
			block.length = 1;
			block.opcodes[0] = condition == 14 ? 0xE03E : 0xD03E | (condition << 8);
			block.opcodes[1] = 0x2707;
			block.opcodes[2] = 0xE7FE;
			uint32_t target = block.start + 0x80;
			if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL standard branch emission"); exit(1); }
			for (unsigned variant = 0; variant < 9; ++variant) {
				memset(&gba, 0, sizeof(gba));
				gba.memory.rom = code;
				gba.memory.romSize = sizeof(code);
				gba.memory.romMask = sizeof(code) - 1;
				gba.memory.activeRegion = block.start >> BASE_OFFSET;
				gba.memory.lastPrefetchedPc = block.start + trial;
				gba.idleOptimization = IDLE_LOOP_IGNORE;
				gba.idleLoop = target + 2;
				gba.lastJump = block.start;
				for (unsigned r = 8; r <= 13; ++r) {
					gba.memory.waitstatesSeq16[r] = 1 + (trial & 3);
					gba.memory.waitstatesNonseq16[r] = 2 + (trial & 7);
					gba.memory.waitstatesSeq32[r] = 2 + (trial & 3);
					gba.memory.waitstatesNonseq32[r] = 4 + (trial & 7);
				}
				struct ARMCore actual = {0};
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.master = &gba.d;
				gba.cpu = &actual;
				actual.gprs[ARM_PC] = block.start + 2;
				actual.prefetch[0] = block.opcodes[0];
				actual.prefetch[1] = block.opcodes[1];
				actual.cpsr.packed = ((trial & 15) << 28) | (trial & 16 ? 0x1F : 0x3F);
				actual.executionMode = MODE_THUMB;
				actual.cycles = (int) trial - 13;
				actual.nextEvent = actual.cycles + 1;
				actual.memory.activeRegion = code;
				actual.memory.activeMask = sizeof(code) - 2;
				actual.memory.activeSeqCycles16 = trial & 3;
				actual.memory.activeNonseqCycles16 = trial & 7;
				actual.memory.setActiveRegion = GBASetActiveRegion;
				switch (variant) {
				case 1: gba.idleOptimization = IDLE_LOOP_REMOVE; break;
				case 2: case 3:
					gba.idleOptimization = IDLE_LOOP_DETECT;
					gba.lastJump = target;
					gba.idleDetectionStep = variant - 2;
					memcpy(gba.cachedRegisters, actual.gprs, sizeof(gba.cachedRegisters));
					gba.cachedRegisters[ARM_PC] = target;
					break;
				case 4: case 5:
					gba.idleOptimization = IDLE_LOOP_REMOVE;
					gba.idleLoop = target;
					gba.haltPending = variant == 5;
					break;
				case 6: gba.memory.activeRegion ^= 2; break;
				case 7: gba.memory.activeRegion = GBA_REGION_BIOS; break;
				case 8: gba.memory.romSize = 2; break;
				}
				before = gba;
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				expected.prefetch[0] = block.opcodes[1];
				expected.prefetch[1] = block.opcodes[2];
				expected.gprs[ARM_PC] += 2;
				_thumbTable[block.opcodes[0] >> 6](&expected, block.opcodes[0]);
				expectedGBA = gba;
				expectedGBA.cpu = &actual;
				gba = before;
				bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
				if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL standard branch condition=%u trial=%u variant=%u cycles=%ld/%ld\n",
					       condition, trial, variant, (long) actual.cycles, (long) expected.cycles);
					exit(1);
				}
				++checks;
			}
		}
	}
	printf("PASS: %u standard branch states, idle probing/halt, BIOS/ROM mappings and bounds\n", checks);
}

static bool patchOnStore;
static void write16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycles) {
	UNUSED(address);
	UNUSED(value);
	*cycles += 2;
	if (patchOnStore) {
		struct GBA* gba = (struct GBA*) cpu->master;
		STORE_16(0x2609, 0x106, gba->memory.rom);
		gba->isPristine = false;
	} else {
		/* Model a WAITCNT change inside a slice. */
		cpu->memory.activeSeqCycles16 ^= 2;
	}
}

static struct RV32Block* primeBlock(struct RV32Context* context, struct ARMCore* cpu, uint32_t start) {
	struct RV32Block* block = _findBlock(context, cpu, start);
	return block ? block : _findBlock(context, cpu, start);
}

/* Test the generated path even for fixtures that only execute one slice. */
static void primeCPU(struct ARMCore* cpu) {
	struct RV32Context* context = _contextFor(cpu, (struct GBA*) cpu->master);
	if (!context) { puts("FAIL test cache allocation"); exit(1); }
	primeBlock(context, cpu, cpu->gprs[ARM_PC] - WORD_SIZE_THUMB);
}

static void checkRunner(void) {
	static const uint16_t programs[][12] = {
		{0x2001, 0x2102, 0x1840, 0x3001, 0x2804, 0x4249, 0x4008, 0xE7FE},
		{0x2001, 0x2102, 0x8810, 0x3001, 0x0049, 0x2801, 0xD1F8, 0xE7FE},
		{0x2001, 0x8011, 0x2202, 0x3301, 0x0049, 0x2801, 0xD1F8, 0xE7FE},
		{0xB07F, 0xAF7F, 0xB0FF, 0xE7FE}, /* Large SP offsets. */
		{0x2800, 0xD000, 0x2001, 0x2102, 0xE7FE},
		{0x2001, 0x4770}, /* BX changes the instruction set. */
	};
	for (unsigned c = 0; c < sizeof(programs) / sizeof(*programs); ++c) {
		for (unsigned trial = 0; trial < 128; ++trial) {
			uint32_t actualCode[128], expectedCode[128];
			for (unsigned i = 0; i < 128; ++i) actualCode[i] = 0xE7FEE7FE;
			memcpy((uint8_t*) actualCode + 0x100, programs[c], sizeof(programs[c]));
			memcpy(expectedCode, actualCode, sizeof(actualCode));
			struct ARMCore actual = {0}, expected;
			static struct GBA actualGBA, expectedGBA;
			memset(&actualGBA, 0, sizeof(actualGBA));
			actualGBA.memory.rom = actualCode;
			actualGBA.memory.romSize = sizeof(actualCode);
			actualGBA.isPristine = true;
			expectedGBA = actualGBA;
			expectedGBA.memory.rom = expectedCode;
			uint32_t start = 0x08000100 + 0x02000000 * (trial % 3);
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.gprs[ARM_PC] = start + 2;
			actual.gprs[ARM_LR] = (start + 0x80) | (trial & 1);
			actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
			actual.executionMode = MODE_THUMB;
			actual.nextEvent = trial;
			actual.memory.activeRegion = actualCode;
			actual.memory.activeMask = 510;
			actual.memory.activeSeqCycles16 = trial & 3;
			actual.memory.activeNonseqCycles16 = 4;
			actual.memory.setActiveRegion = region;
			actual.memory.load16 = read16;
			actual.memory.store16 = write16;
			actual.irqh.processEvents = events;
			actual.prefetch[0] = programs[c][0];
			actual.prefetch[1] = programs[c][1];
			if (trial & 16) actual.prefetch[(trial >> 5) & 1] = 0x2707;
			actual.master = &actualGBA.d;
			expected = actual;
			expected.master = &expectedGBA.d;
			expected.memory.activeRegion = expectedCode;
			patchOnStore = trial & 64;
			RV32Invalidate(&actual);
			primeCPU(&actual);
			RV32RunLoop(&actual);
			ARMRunLoopLegacy(&expected);
			actual.master = expected.master;
			actual.memory.activeRegion = expected.memory.activeRegion;
			if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(actualCode, expectedCode, sizeof(actualCode))) {
				printf("FAIL runner case=%u trial=%u pc=%08lx/%08lx cycles=%ld/%ld\n", c, trial,
				       (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
				       (long) actual.cycles, (long) expected.cycles);
				exit(1);
			}
		}
	}
	puts("PASS: runner spans, event boundaries, ROM mirrors, stale pipeline, ROM writes, WAITCNT and BX");
}

static uint32_t visibleRead(struct ARMCore* cpu, uint32_t address, int* cycles) {
	/* A helper may inspect architectural state (for IO, tracing or debugging).
	 * Make its result and a side effect depend on all deferred state. */
	uint32_t value = address ^ cpu->cpsr.packed ^ cpu->prefetch[0] ^
		(cpu->prefetch[1] << 16) ^ (uint32_t) cpu->cycles ^ (uint32_t) *cycles;
	for (unsigned r = 0; r < 16; ++r) value = (value << 1) ^ (uint32_t) cpu->gprs[r];
	cpu->shifterOperand = value;
	*cycles += 1 + (value & 3);
	return value;
}
static uint32_t mutatingRead(struct ARMCore* cpu, uint32_t address, int* cycles) {
	uint32_t value = visibleRead(cpu, address, cycles);
	for (unsigned r = 0; r < ARM_PC; ++r) cpu->gprs[r] ^= value + r;
	cpu->cpsr.packed ^= 0xF0000000;
	cpu->cycles += 2;
	return value;
}
static uint32_t visibleRead8(struct ARMCore* cpu, uint32_t address, int* cycles) {
	return visibleRead(cpu, address, cycles) & 0xFF;
}
static uint32_t visibleRead16(struct ARMCore* cpu, uint32_t address, int* cycles) {
	return visibleRead(cpu, address, cycles) & 0xFFFF;
}

static void checkLongBlocks(void) {
	static const uint16_t operations[] = {0x2001, 0x2102, 0x1840, 0x3001, 0x2804,
		0x4249, 0x4008, 0x8810, 0x7810, 0x6810, 0x0049, 0x4810, 0x9F7F};
	static struct GBA gba;
	uint32_t code[256];
	for (unsigned trial = 0; trial < 256; ++trial) {
		for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
		for (unsigned i = 0; i < 64; ++i) {
			uint16_t opcode = operations[randomWord() % (sizeof(operations) / sizeof(*operations))];
			STORE_16(opcode, 0x100 + i * 2, code);
		}
		struct ARMCore actual = {0};
		memset(&gba, 0, sizeof(gba));
		gba.isPristine = true;
		gba.memory.rom = code;
		gba.memory.romSize = sizeof(code);
		actual.master = &gba.d;
		for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
		actual.gprs[ARM_PC] = 0x08000102;
		actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
		actual.executionMode = MODE_THUMB;
		actual.cycles = trial & 32 ? INT32_MIN : 0;
		actual.nextEvent = actual.cycles + trial * 2;
		actual.memory.activeRegion = code;
		actual.memory.activeMask = sizeof(code) - 2;
		actual.memory.activeSeqCycles16 = trial & 3;
		actual.memory.activeNonseqCycles16 = 4;
		actual.memory.setActiveRegion = region;
		actual.memory.load8 = visibleRead8;
		actual.memory.load16 = visibleRead16;
		actual.memory.load32 = trial & 1 ? mutatingRead : visibleRead;
		actual.irqh.processEvents = events;
		LOAD_16(actual.prefetch[0], 0x100, code);
		LOAD_16(actual.prefetch[1], 0x102, code);
		struct ARMCore expected = actual;
		RV32Invalidate(&actual);
		primeCPU(&actual);
		RV32RunLoop(&actual);
		ARMRunLoopLegacy(&expected);
		if (memcmp(&actual, &expected, sizeof(actual))) {
			printf("FAIL long block trial=%u pc=%08lx/%08lx cycles=%ld/%ld\n", trial,
			       (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
			       (long) actual.cycles, (long) expected.cycles);
			exit(1);
		}
	}
	puts("PASS: 256 mixed long blocks, helper-visible state, code-buffer boundaries and negative cycle deadlines");
}

static void noEvents(struct ARMCore* cpu) { UNUSED(cpu); }
static unsigned continuationVariant;
static void continuationStoreEffect(struct ARMCore* cpu, int32_t value) {
	struct GBA* gba = (struct GBA*) cpu->master;
	uint32_t observed = value ^ cpu->cycles ^ cpu->cpsr.packed ^ cpu->prefetch[0] ^ cpu->prefetch[1];
	for (unsigned r = 0; r < 16; ++r) observed ^= (uint32_t) cpu->gprs[r] << (r & 7);
	cpu->gprs[7] = observed;
	cpu->cpsr.packed ^= 0x90000000;
	cpu->memory.activeSeqCycles16 ^= 2;
	if (continuationVariant >= 4) {
		STORE_16(0x2509, 0x10A, gba->memory.rom);
		gba->isPristine = false;
	}
	if (continuationVariant == 5) RV32Invalidate(cpu);
}
static void continuationStore32(struct ARMCore* cpu, uint32_t address, int32_t value, int* cycles) {
	GBAStore32(cpu, address, value, cycles);
	continuationStoreEffect(cpu, value);
}
static void continuationStore16(struct ARMCore* cpu, uint32_t address, int16_t value, int* cycles) {
	GBAStore16(cpu, address, value, cycles);
	continuationStoreEffect(cpu, value);
}
static void continuationStore8(struct ARMCore* cpu, uint32_t address, int8_t value, int* cycles) {
	GBAStore8(cpu, address, value, cycles);
	continuationStoreEffect(cpu, value);
}

static void checkStoreContinuation(void) {
	static const uint16_t operations[][2] = {
		{0x6008, 0x680C}, {0x8008, 0x880C}, {0x7008, 0x780C},
		{0x5088, 0x588C}, {0x5288, 0x5A8C}, {0x5488, 0x5C8C},
		{0x9000, 0x9C00}, {0x5089, 0x588C}, {0x528A, 0x5A8C}
	};
	static struct GBA gba, before, expectedGBA;
	static uint32_t wram[GBA_SIZE_EWRAM / 4], iwram[GBA_SIZE_IWRAM / 4];
	static uint32_t code[256], savedCode[256], expectedCode[256];
	unsigned checks = 0;
	struct ARMCore actual;
	for (unsigned op = 0; op < sizeof(operations) / sizeof(*operations); ++op) {
		for (continuationVariant = 0; continuationVariant < 6; ++continuationVariant) {
			for (unsigned trial = 0; trial < 64; ++trial) {
				for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
				const uint16_t program[] = {operations[op][0], 0x3601, operations[op][1],
					0x3001, operations[op][0], 0x3501, 0xE7F8};
				memcpy((uint8_t*) code + 0x100, program, sizeof(program));
				memset(&gba, 0, sizeof(gba)); memset(&actual, 0, sizeof(actual));
				uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
				uint32_t address = trial & 1 ? 0x02000000 : 0x03000000;
				if (continuationVariant == 1) address |= 0xFFFFFC | (trial & 3);
				if (continuationVariant == 2) address = 0x04000204;
				uint32_t* word = address >> BASE_OFFSET == GBA_REGION_EWRAM ?
					&wram[(address & (GBA_SIZE_EWRAM - 4)) / 4] :
					address >> BASE_OFFSET == GBA_REGION_IWRAM ? &iwram[(address & (GBA_SIZE_IWRAM - 4)) / 4] : NULL;
				if (word) *word = randomWord();
				actual.master = &gba.d; gba.cpu = &actual;
				gba.memory.wram = wram; gba.memory.iwram = iwram; gba.memory.rom = code;
				gba.memory.romSize = sizeof(code); gba.memory.romMask = sizeof(code) - 1;
				gba.isPristine = !(trial & 8); gba.idleOptimization = IDLE_LOOP_IGNORE;
				gba.memory.prefetch = trial & 2; gba.memory.lastPrefetchedPc = start + (trial & 31);
				gba.memory.activeRegion = start >> BASE_OFFSET;
				gba.memory.waitstatesNonseq16[GBA_REGION_EWRAM] = trial & 7;
				gba.memory.waitstatesNonseq32[GBA_REGION_EWRAM] = trial & 15;
				actual.executionMode = MODE_THUMB; actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.gprs[1] = actual.gprs[ARM_SP] = address; actual.gprs[2] = 0;
				actual.gprs[ARM_PC] = start + 2;
				actual.memory.activeRegion = code; actual.memory.activeMask = sizeof(code) - 2;
				actual.memory.activeSeqCycles16 = trial & 3; actual.memory.activeNonseqCycles16 = trial & 7;
				actual.memory.load8 = GBALoad8; actual.memory.load16 = GBALoad16; actual.memory.load32 = GBALoad32;
				actual.memory.store8 = GBAStore8; actual.memory.store16 = GBAStore16; actual.memory.store32 = GBAStore32;
				actual.memory.setActiveRegion = GBASetActiveRegion; actual.irqh.processEvents = noEvents;
				actual.prefetch[0] = program[0]; actual.prefetch[1] = program[1];
				actual.cycles = trial & 16 ? INT32_MIN : -13; actual.nextEvent = actual.cycles + trial * 2;
				RV32Invalidate(&actual);
				struct RV32Context* context = _contextFor(&actual, &gba);
				struct RV32Block* block = primeBlock(context, &actual, start);
				if (!block || !block->executable || block->length < 6 || !context->dataReady) {
					puts("FAIL store continuation did not form a complete native memory block"); exit(1);
				}
				if (continuationVariant >= 3) {
					actual.memory.store8 = continuationStore8;
					actual.memory.store16 = continuationStore16;
					actual.memory.store32 = continuationStore32;
				}
				for (unsigned slice = 0; slice < 2; ++slice) {
					if (slice) actual.nextEvent = actual.cycles + 127;
					before = gba; memcpy(savedCode, code, sizeof(code));
					uint32_t oldWord = word ? *word : 0;
					struct ARMCore expected = actual; gba.cpu = &expected;
					ARMRunLoopLegacy(&expected);
					expectedGBA = gba; expectedGBA.cpu = &actual;
					uint32_t expectedWord = word ? *word : 0;
					memcpy(expectedCode, code, sizeof(code));
					gba = before; memcpy(code, savedCode, sizeof(code)); if (word) *word = oldWord;
					ARMRunLoop(&actual);
					if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba)) ||
					    memcmp(code, expectedCode, sizeof(code)) || (word && *word != expectedWord)) {
						printf("FAIL store continuation op=%04x variant=%u trial=%u slice=%u pc=%08lx/%08lx cycles=%ld/%ld\n",
						       operations[op][0], continuationVariant, trial, slice, (unsigned long) actual.gprs[ARM_PC],
						       (unsigned long) expected.gprs[ARM_PC], (long) actual.cycles, (long) expected.cycles); exit(1);
					}
					++checks;
				}
			}
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u native store/load continuation slices, aliases, event boundaries, WAITCNT, callbacks, code patches and explicit invalidation\n", checks);
}

static void referenceStep(struct ARMCore* cpu) {
	uint16_t opcode = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	LOAD_16(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
	_thumbTable[opcode >> 6](cpu, opcode);
}

static uint32_t instructionsRetired(void);

static void checkChaining(void) {
	static struct GBA gba;
	uint32_t code[256];
	for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
	STORE_16(0x3001, 0x100, code); /* ADD r0,1; B block B */
	STORE_16(0xE00D, 0x102, code);
	STORE_16(0x3101, 0x120, code); /* ADD r1,1; B block A */
	STORE_16(0xE7ED, 0x122, code);
	memset(&gba, 0, sizeof(gba));
	gba.memory.rom = code;
	gba.memory.romSize = sizeof(code);
	gba.isPristine = true;
	struct ARMCore actual = {0};
	for (unsigned trial = 0; trial < 256; ++trial) {
		memset(&actual, 0, sizeof(actual));
		uint32_t start = 0x08000100 + 0x02000000 * (trial % 3);
		uint16_t backedge = trial & 128 ? 0xE7FD : 0xE7ED; /* B B / B A. */
		STORE_16(backedge, 0x122, code);
		actual.master = &gba.d;
		actual.executionMode = MODE_THUMB;
		actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
		actual.gprs[ARM_PC] = start + 2;
		actual.memory.activeRegion = code;
		actual.memory.activeMask = sizeof(code) - 2;
		actual.memory.activeSeqCycles16 = trial & 3;
		actual.memory.activeNonseqCycles16 = 4;
		/* Standard same-ROM branches keep dirty registers native across
		 * the A/B link; overridden callbacks exercise full synchronization. */
		actual.memory.setActiveRegion = trial & 1 ? region : GBASetActiveRegion;
		gba.cpu = &actual;
		gba.memory.activeRegion = start >> BASE_OFFSET;
		gba.memory.romMask = sizeof(code) - 1;
		gba.idleOptimization = IDLE_LOOP_IGNORE;
		actual.irqh.processEvents = noEvents;
		actual.cycles = trial & 32 ? INT32_MIN : 0;
		actual.nextEvent = actual.cycles + trial;
		actual.prefetch[0] = 0x3001;
		actual.prefetch[1] = 0xE00D;
		struct ARMCore initial = actual;
		RV32Invalidate(&actual);
		struct RV32Context* context = _contextFor(&actual, &gba);
		struct RV32Block* first = primeBlock(context, &actual, start);
		actual.gprs[ARM_PC] = start + 0x22;
		actual.prefetch[0] = 0x3101;
		actual.prefetch[1] = backedge;
		struct RV32Block* second = primeBlock(context, &actual, start + 0x20);
		if (!first || !second || !first->executable || !second->executable || !context->dispatchReady || !context->branchReady) {
			puts("FAIL chain compilation"); exit(1);
		}
		actual = initial;
		struct ARMCore expected = actual;
		ARMRunLoopLegacy(&expected);
		/* On the ordinary ROM path, entering the general dispatcher must
		 * fail this test. A direct edge must use the target's lazy entry. */
		uint32_t savedDispatch[RV32_DISPATCH_MAX_WORDS];
		memcpy(savedDispatch, context->dispatchCode, sizeof(savedDispatch));
		if (!(trial & 1)) {
			struct RV32Block stop = {0};
			stop.resident = true;
			if (!_emitI(&stop, 0, RV_X0, 0, RV_A0) || !_emitReturn(&stop)) exit(1);
			memcpy(context->dispatchCode, stop.nativeCode, stop.nativeWords * sizeof(uint32_t));
			__asm__ volatile("fence.i" ::: "memory");
		}
		/* One host call must traverse the cached A/B loop to the deadline. */
		bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) first->nativeCode)(&actual);
		memcpy(context->dispatchCode, savedDispatch, sizeof(savedDispatch));
		__asm__ volatile("fence.i" ::: "memory");
		if (!complete || actual.cycles < actual.nextEvent || memcmp(&actual, &expected, sizeof(actual))) {
			printf("FAIL chained execution trial=%u pc=%08lx/%08lx cycles=%ld/%ld\n", trial,
			       (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
			       (long) actual.cycles, (long) expected.cycles);
			exit(1);
		}
		for (unsigned guard = 0; guard < 8; ++guard) {
			struct RV32Block saved = *second;
			switch (guard) {
			case 0: second->valid = false; break;
			case 1: second->executable = false; break;
			case 2: second->start ^= 2 | (1u << (1 + __builtin_ctz(RV32_BLOCK_CACHE_SIZE))); break; /* Same slot. */
			case 3: second->region = code + 1; break;
			case 4: second->mask ^= 2; break;
			case 5: second->seqCycles ^= 2; break;
			case 6: second->opcodes[0] ^= 1; break;
			case 7: second->opcodes[1] ^= 1; break;
			}
			actual = initial;
			actual.nextEvent = actual.cycles + 1000;
			expected = actual;
			referenceStep(&expected);
			referenceStep(&expected);
			context->codeEpoch = 0; /* Model the host boundary that changed the cache. */
			complete = ((bool (*)(struct ARMCore*)) (uintptr_t) first->nativeCode)(&actual);
			*second = saved;
			__asm__ volatile("fence.i" ::: "memory");
			if (!complete || memcmp(&actual, &expected, sizeof(actual))) {
				printf("FAIL chain entry guard=%u trial=%u\n", guard, trial);
				exit(1);
			}
		}
	}
	puts("PASS: 256 chained A/B and self loops, 2048 invalidation/mapping/pipeline guards and event exits");
	RV32Invalidate(&actual);
}

/* Exercise each direct-edge guard in isolation. Rejected edges must return
 * without changing CPU/GBA state; accepted ones refill only at the target's
 * event exit, even when the caller's PC/prefetch are still the source state. */
static void checkDirectEdges(bool arm) {
	static uint32_t code[GBA_SIZE_ROM0 / 4];
	static struct GBA gba, initialGBA, expectedGBA;
	static struct RV32Block wrapper;
	struct ARMCore actual = {0};
	/* Populate both 16 MiB halves so every ROM region has a real backing
	 * address. The different first opcodes detect a lost address bit 24. */
	for (unsigned bank = 0; bank < 2; ++bank) {
		uint32_t offset = bank * 0x01000000;
		for (unsigned i = 0; i < 256; ++i) code[offset / 4 + i] = arm ? 0xEAFFFFFE : 0xE7FEE7FE;
		if (arm) {
			STORE_32(0xEA000006, offset + 0x100, code);
			STORE_32(bank ? 0xE2822001 : 0xE2811001, offset + 0x120, code);
		} else {
			STORE_16(0xE00E, offset + 0x100, code);
			STORE_16(bank ? 0x3201 : 0x3101, offset + 0x120, code);
		}
	}
	unsigned checks = 0;
	for (unsigned trial = 0; trial < 64; ++trial) {
		memset(&gba, 0, sizeof(gba));
		memset(&actual, 0, sizeof(actual));
		uint32_t start = 0x08000100 + (trial % 6) * 0x01000000;
		uint32_t target = start + 0x20;
		gba.cpu = &actual;
		gba.isPristine = true;
		gba.memory.rom = code;
		gba.memory.romSize = sizeof(code);
		gba.memory.romMask = sizeof(code) - 1;
		gba.memory.activeRegion = start >> BASE_OFFSET;
		gba.memory.lastPrefetchedPc = start + trial;
		gba.idleOptimization = trial & 1 ? IDLE_LOOP_REMOVE : IDLE_LOOP_IGNORE;
		gba.idleLoop = target + 2;
		for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
		actual.master = &gba.d;
		actual.executionMode = arm ? MODE_ARM : MODE_THUMB;
		actual.cpsr.packed = ((trial & 15) << 28) | (arm ? 0x1F : 0x3F);
		actual.cycles = (int) trial - 13;
		actual.nextEvent = actual.cycles + 1;
		actual.memory.activeRegion = code;
		actual.memory.activeMask = sizeof(code) - (arm ? 4 : 2);
		actual.memory.activeSeqCycles16 = actual.memory.activeSeqCycles32 = trial & 3;
		actual.memory.activeNonseqCycles16 = actual.memory.activeNonseqCycles32 = trial & 7;
		actual.memory.setActiveRegion = GBASetActiveRegion;
		actual.gprs[ARM_PC] = target + (arm ? 4 : 2);
		if (arm) { LOAD_32(actual.prefetch[0], target & actual.memory.activeMask, code); }
		else { LOAD_16(actual.prefetch[0], target & actual.memory.activeMask, code); }
		actual.prefetch[1] = arm ? 0xEAFFFFFE : 0xE7FE;
		RV32Invalidate(&actual);
		struct RV32Context* context = _contextFor(&actual, &gba);
		struct RV32Block* dest = primeBlock(context, &actual, target);
		if (!dest || !dest->executable || !(arm ? context->armBranchReady : context->branchReady)) { puts("FAIL direct-edge setup"); exit(1); }
		memset(&wrapper, 0, sizeof(wrapper));
		wrapper.arm = arm;
		wrapper.resident = true;
		wrapper.start = start;
		wrapper.seqCycles = actual.memory.activeSeqCycles16;
		wrapper.branchDispatch = arm ? context->armBranchCode : context->branchCode;
		wrapper.cache = context->blocks;
		if (!_emitEntry(&wrapper) || !_emitFastBranchCall(&wrapper, target) ||
		    !_emitI(&wrapper, 0, RV_X0, 0, RV_A0) || !_emitReturn(&wrapper)) exit(1);
		__asm__ volatile("fence.i" ::: "memory");
		actual.gprs[ARM_PC] = start + (arm ? 8 : 4);
		actual.prefetch[0] = 0xE7FE;
		actual.prefetch[1] = 0xE7FE;
		struct ARMCore initial = actual;
		initialGBA = gba;
		struct ARMCore expected = actual;
		gba.cpu = &expected;
		if (arm) _armTable[0xA00](&expected, 0xEA000006);
		else _thumbTable[0xE00E >> 6](&expected, 0xE00E);
		expectedGBA = gba;
		expectedGBA.cpu = &actual;
		gba = initialGBA;
		uint32_t guardedInstructions = instructionsRetired();
		bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) wrapper.nativeCode)(&actual);
		guardedInstructions = instructionsRetired() - guardedInstructions;
		if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
			printf("FAIL direct-edge commit trial=%u\n", trial); exit(1);
		}
		++checks;
		/* Reusing the proven edge must preserve the full branch state and
		 * actually execute fewer RV32 instructions than the guarded entry. */
		actual = initial;
		gba = initialGBA;
		uint32_t provedInstructions = instructionsRetired();
		complete = ((bool (*)(struct ARMCore*)) (uintptr_t) wrapper.nativeCode)(&actual);
		provedInstructions = instructionsRetired() - provedInstructions;
		if (!complete || provedInstructions >= guardedInstructions ||
		    memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
			printf("FAIL proven edge arm=%u trial=%u instructions=%lu/%lu\n", arm, trial,
			       (unsigned long) provedInstructions, (unsigned long) guardedInstructions); exit(1);
		}
		++checks;
		for (unsigned guard = 0; guard < 19; ++guard) {
			actual = initial;
			gba = initialGBA;
			struct RV32Block saved = *dest;
			switch (guard) {
			case 0: actual.memory.setActiveRegion = region; break;
			case 1: actual.executionMode = arm ? MODE_THUMB : MODE_ARM; break;
			case 2: actual.cpsr.packed ^= 32; break;
			case 3: gba.isPristine = false; break;
			case 4: gba.memory.activeRegion ^= 2; break;
			case 5: gba.memory.romSize = target & (GBA_SIZE_ROM0 - 1); break;
			case 6: gba.idleOptimization = IDLE_LOOP_DETECT; break;
			case 7: case 8:
				gba.idleOptimization = IDLE_LOOP_REMOVE;
				gba.idleLoop = target;
				gba.haltPending = guard == 8;
				break;
			case 9: dest->valid = false; break;
			case 10: dest->executable = false; break;
			case 11: dest->start ^= 2 | (1u << (1 + __builtin_ctz(RV32_BLOCK_CACHE_SIZE))); break;
			case 12: dest->region = code + 1; break;
			case 13: dest->mask ^= 4; break;
			case 14: dest->seqCycles ^= 2; break;
			case 15:
				if (arm) actual.memory.activeSeqCycles32 ^= 2; else actual.memory.activeSeqCycles16 ^= 2;
				dest->seqCycles ^= 2; break;
			case 16: if (arm) dest->armOpcodes[0] ^= 1; else dest->opcodes[0] ^= 1; break;
			case 17: if (arm) dest->armOpcodes[1] ^= 1; else dest->opcodes[1] ^= 1; break;
			case 18: dest->arm = !dest->arm; break;
			}
			expected = actual;
			expectedGBA = gba;
			context->codeEpoch = 0; /* Every mutated guard must be rechecked at a boundary. */
			complete = ((bool (*)(struct ARMCore*)) (uintptr_t) wrapper.nativeCode)(&actual);
			*dest = saved;
			__asm__ volatile("fence.i" ::: "memory");
			if (complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
				printf("FAIL direct-edge guard=%u trial=%u\n", guard, trial); exit(1);
			}
			++checks;
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %s %u direct-edge commit/reject states, deferred target pipeline and all mutable guards\n", arm ? "ARM" : "Thumb", checks);
}

static uint32_t changeBranchState(struct ARMCore* cpu, uint32_t address, int* cycles) {
	struct GBA* gba = (struct GBA*) cpu->master;
	if (address & 1) {
		cpu->memory.setActiveRegion = branchRegion;
	} else {
		gba->idleOptimization = IDLE_LOOP_DETECT;
		gba->lastJump = cpu->gprs[ARM_PC] - 4;
		gba->idleDetectionStep = 0;
	}
	*cycles += 2;
	return 0xFACE;
}

/* A -> B uses the direct native edge. B must still observe a changed
 * mapping callback, newly enabled idle detection, or its own idle target. */
static void checkChainedSideEffects(void) {
	static struct GBA gba, before, expectedGBA;
	uint32_t code[256];
	struct ARMCore actual = {0};
	for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
	STORE_16(0x3001, 0x100, code);
	STORE_16(0xE00D, 0x102, code);
	unsigned checks = 0;
	for (unsigned trial = 0; trial < 64; ++trial) {
		for (unsigned variant = 0; variant < 3; ++variant) {
			memset(&gba, 0, sizeof(gba));
			memset(&actual, 0, sizeof(actual));
			uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
			uint16_t secondOpcode = variant == 2 ? 0x3101 : 0x881A; /* ADD r1,1 / LDRH r2,[r3]. */
			uint16_t branchOpcode = variant == 2 ? 0xE7ED : 0xE7FD; /* B A / B B. */
			STORE_16(secondOpcode, 0x120, code);
			STORE_16(branchOpcode, 0x122, code);
			gba.cpu = &actual;
			gba.isPristine = true;
			gba.memory.rom = code;
			gba.memory.romSize = sizeof(code);
			gba.memory.romMask = sizeof(code) - 1;
			gba.memory.activeRegion = start >> BASE_OFFSET;
			gba.idleOptimization = variant == 2 ? IDLE_LOOP_REMOVE : IDLE_LOOP_IGNORE;
			gba.idleLoop = start;
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.master = &gba.d;
			actual.executionMode = MODE_THUMB;
			actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
			actual.gprs[3] = variant;
			actual.gprs[ARM_PC] = start + 2;
			actual.cycles = (int) trial - 13;
			actual.nextEvent = actual.cycles + (variant == 2 ? 18 : 22);
			actual.prefetch[0] = 0x3001;
			actual.prefetch[1] = 0xE00D;
			actual.memory.activeRegion = code;
			actual.memory.activeMask = sizeof(code) - 2;
			actual.memory.activeSeqCycles16 = 2;
			actual.memory.activeNonseqCycles16 = 4;
			actual.memory.setActiveRegion = GBASetActiveRegion;
			actual.memory.load16 = changeBranchState;
			actual.irqh.processEvents = noEvents;
			struct ARMCore initial = actual;
			RV32Invalidate(&actual);
			struct RV32Context* context = _contextFor(&actual, &gba);
			struct RV32Block* first = primeBlock(context, &actual, start);
			actual.gprs[ARM_PC] = start + 0x22;
			actual.prefetch[0] = secondOpcode;
			actual.prefetch[1] = branchOpcode;
			struct RV32Block* second = primeBlock(context, &actual, start + 0x20);
			/* A callback now ends its native segment. Cache the following
			 * branch as well so this still exercises a complete native chain. */
			actual.gprs[ARM_PC] = start + 0x24;
			actual.prefetch[0] = branchOpcode;
			actual.prefetch[1] = 0xE7FE;
			primeBlock(context, &actual, start + 0x22);
			if (!first || !second || !first->executable || !second->executable || !context->branchReady) {
				puts("FAIL branch revalidation setup"); exit(1);
			}
			actual = initial;
			before = gba;
			struct ARMCore expected = actual;
			gba.cpu = &expected;
			for (unsigned i = 0; i < 4; ++i) referenceStep(&expected);
			expectedGBA = gba;
			expectedGBA.cpu = &actual;
			gba = before;
			bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) first->nativeCode)(&actual);
			if (!complete || actual.cycles < actual.nextEvent || memcmp(&actual, &expected, sizeof(actual)) ||
			    memcmp(&gba, &expectedGBA, sizeof(gba))) {
				printf("FAIL branch revalidation trial=%u variant=%u\n", trial, variant); exit(1);
			}
			++checks;
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u branch revalidation states after callbacks and per-target idle checks\n", checks);
}

static uint32_t pollingRead(struct ARMCore* cpu, uint32_t address, int* cycles) {
	UNUSED(address);
	++((struct GBA*) cpu->master)->idleDetectionFailures;
	*cycles += 2;
	return 0;
}

static int32_t pollingStall(struct ARMCore* cpu, int32_t cycles) {
	++((struct GBA*) cpu->master)->idleDetectionFailures;
	return cycles;
}

static void pollingEvent(struct ARMCore* cpu) {
	struct GBA* gba = (struct GBA*) cpu->master;
	++gba->idleDetectionFailures;
	gba->memory.io[GBA_REG(VCOUNT)] = gba->idleDetectionFailures & 1;
	cpu->nextEvent = cpu->cycles + 71;
}

#ifdef MGBA_RV32_TRACE_POLL
static unsigned traceCallbacks;
static uint32_t traceRead16(struct ARMCore* cpu, uint32_t address, int* cycles) {
	++traceCallbacks;
	return GBALoad16(cpu, address, cycles);
}

/* Several disjoint ROM blocks form one loop. Idempotent RAM writes qualify;
 * visible memory counters, high-register counters and callbacks must execute
 * every iteration. Compare full CPU/GBA state, touched RAM and callback count. */
static void checkTracePolling(void) {
	static uint32_t code[256], iwram[GBA_SIZE_IWRAM / 4], wram[GBA_SIZE_EWRAM / 4];
	static struct GBA gba, before, expectedGBA;
	struct ARMCore actual;
	unsigned checks = 0, accelerated = 0;
	for (unsigned variant = 0; variant < 6; ++variant) {
		for (unsigned width = 1; width <= 4; width *= 2) {
			for (unsigned region = 0; region < 2; ++region) {
				for (unsigned trial = 0; trial < 96; ++trial) {
					uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
					for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
					const uint16_t a[] = {0x8808, width == 1 ? 0x7010 : width == 2 ? 0x8010 : 0x6010,
						0x2800, 0xD01B, 0x2401, 0xE019};
					memcpy((uint8_t*) code + 0x100, a, sizeof(a));
					const uint16_t c[] = {width == 1 ? 0x7813 : width == 2 ? 0x8813 : 0x6813, 0x2B00, 0xE01C};
					memcpy((uint8_t*) code + 0x140, c, sizeof(c));
					if (variant == 1) STORE_16(0x3401, 0x108, code); /* Visible low-register counter. */
					if (variant == 2) {
						const uint16_t count[] = {0x882B, 0x3301, 0x802B, 0x2300, 0xE01A};
						memcpy((uint8_t*) code + 0x140, count, sizeof(count));
					}
					STORE_16(variant == 3 ? 0x44B8 : 0x2307, 0x180, code); /* ADD r8,r7 / MOV r3,7. */
					STORE_16(0xE7BD, 0x182, code);
					memset(&actual, 0, sizeof(actual)); memset(&gba, 0, sizeof(gba));
					gba.cpu = &actual; actual.master = &gba.d;
					gba.isPristine = !(trial & 8); gba.idleOptimization = IDLE_LOOP_IGNORE;
					gba.memory.rom = code; gba.memory.romSize = sizeof(code); gba.memory.romMask = sizeof(code) - 1;
					gba.memory.iwram = iwram; gba.memory.wram = wram;
					gba.memory.activeRegion = start >> BASE_OFFSET;
					gba.memory.prefetch = trial & 1;
					gba.memory.lastPrefetchedPc = start + (trial & 15);
					gba.memory.waitstatesNonseq16[GBA_REGION_EWRAM] = 2;
					gba.memory.waitstatesNonseq32[GBA_REGION_EWRAM] = 5;
					gba.memory.io[GBA_REG(VCOUNT)] = 1 + trial % 227;
					gba.haltPending = trial & 2;
					actual.executionMode = MODE_THUMB; actual.privilegeMode = MODE_SYSTEM;
					actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
					for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
					actual.gprs[1] = GBA_BASE_IO + GBA_REG_VCOUNT;
					uint32_t base = region ? GBA_BASE_EWRAM : GBA_BASE_IWRAM;
					actual.gprs[2] = base + (trial & 3) + (region ? GBA_SIZE_EWRAM : GBA_SIZE_IWRAM);
					actual.gprs[5] = base + 16; actual.gprs[7] = 1;
					actual.memory.activeRegion = code; actual.memory.activeMask = sizeof(code) - 2;
					actual.memory.activeSeqCycles16 = trial % 3;
					actual.memory.activeNonseqCycles16 = trial % 5;
					actual.memory.load8 = GBALoad8; actual.memory.load16 = variant == 4 ? traceRead16 : GBALoad16;
					actual.memory.load32 = GBALoad32; actual.memory.store8 = GBAStore8;
					actual.memory.store16 = GBAStore16; actual.memory.store32 = GBAStore32;
					actual.memory.setActiveRegion = GBASetActiveRegion; actual.irqh.processEvents = noEvents;
					actual.cycles = variant == 5 ? -100 : 0;
					actual.nextEvent = actual.cycles + (trial < 64 ? trial * 3 + 1 : 10000 + trial);
					uint32_t* ram = region ? wram : iwram;
					ram[0] = randomWord(); ram[4] = trial;
					RV32Invalidate(&actual);
					struct RV32Context* context = _contextFor(&actual, &gba);
					unsigned offsets[] = {0, 8, 0x40, 0x80};
					for (unsigned i = 0; i < 4; ++i) {
						uint32_t pc = start + offsets[i];
						actual.gprs[ARM_PC] = pc + 2;
						LOAD_16(actual.prefetch[0], pc & actual.memory.activeMask, code);
						LOAD_16(actual.prefetch[1], (pc + 2) & actual.memory.activeMask, code);
						struct RV32Block* block = primeBlock(context, &actual, pc);
						if (!block || !block->executable || block->pollLoop) { puts("FAIL trace block setup"); exit(1); }
					}
					actual.gprs[ARM_PC] = start + 2; actual.prefetch[0] = a[0]; actual.prefetch[1] = a[1];
					for (unsigned slice = 0; slice < 2; ++slice) {
						if (slice) {
							/* An event changes the polled value, deadline and RAM. */
							actual.nextEvent = actual.cycles + 127;
							gba.memory.io[GBA_REG(VCOUNT)] ^= 0xA5;
							ram[0] ^= 0x13579BDF;
						}
						before = gba;
						uint32_t old0 = ram[0], old4 = ram[4];
						struct ARMCore expected = actual; gba.cpu = &expected;
						traceCallbacks = 0;
						uint32_t retired = instructionsRetired();
						ARMRunLoopLegacy(&expected);
						uint32_t referenceInstructions = instructionsRetired() - retired;
						unsigned expectedCallbacks = traceCallbacks;
						uint32_t expected0 = ram[0], expected4 = ram[4];
						expectedGBA = gba; expectedGBA.cpu = &actual;
						gba = before; ram[0] = old0; ram[4] = old4; traceCallbacks = 0;
						retired = instructionsRetired();
						ARMRunLoop(&actual);
						uint32_t nativeInstructions = instructionsRetired() - retired;
						if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba)) ||
						    ram[0] != expected0 || ram[4] != expected4 || traceCallbacks != expectedCallbacks) {
							printf("FAIL trace variant=%u width=%u region=%u trial=%u slice=%u pc=%08lx/%08lx cycles=%ld/%ld callbacks=%u/%u ram=%08lx/%08lx,%08lx/%08lx\n",
							       variant, width, region, trial, slice, (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
							       (long) actual.cycles, (long) expected.cycles, traceCallbacks, expectedCallbacks,
							       (unsigned long) ram[0], (unsigned long) expected0, (unsigned long) ram[4], (unsigned long) expected4);
							for (unsigned r = 0; r < 16; ++r) if (actual.gprs[r] != expected.gprs[r])
								printf("r%u=%08lx/%08lx\n", r, (unsigned long) actual.gprs[r], (unsigned long) expected.gprs[r]);
							exit(1);
						}
						if (!variant && trial >= 64 && !slice) {
							if (nativeInstructions >= referenceInstructions / 8) {
								printf("FAIL trace did not accelerate width=%u trial=%u native=%lu reference=%lu\n", width, trial,
								       (unsigned long) nativeInstructions, (unsigned long) referenceInstructions); exit(1);
							}
							++accelerated;
						}
						++checks;
					}
				}
			}
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u cross-block fixed-point slices, %u accelerated; idempotent RAM writes, counters, callbacks and event mutations\n", checks, accelerated);
}
#endif

static uint32_t instructionsRetired(void) {
	uint32_t count;
	__asm__ volatile("csrr %0, minstret" : "=r"(count) :: "memory");
	return count;
}

/* Compare complete event slices, including deadlines at every position in
 * the loop. Fixed read values do not make callbacks pure: the callback case
 * below returns zero but increments GBA state on every read. */
static void checkPollingLoops(void) {
	static const struct {
		uint16_t code[6];
		bool candidate;
	} programs[] = {
		{{0x7808, 0x2800, 0xD0FC, 0xE7FE}, true}, /* LDRB; CMP; BEQ */
		{{0x8808, 0x2800, 0xD0FC, 0xE7FE}, true}, /* LDRH */
		{{0x6808, 0x4010, 0x2800, 0xD0FB, 0xE7FE}, true}, /* LDR; AND; CMP; BEQ */
		{{0x5688, 0x2800, 0xD0FC, 0xE7FE}, true}, /* LDRSB */
		{{0x5E88, 0x2800, 0xD0FC, 0xE7FE}, true}, /* LDRSH */
		{{0x4803, 0x2800, 0xD0FC, 0xE7FE}, true}, /* ROM literal */
		{{0x2000, 0x2800, 0xD0FC, 0xE7FE}, true}, /* Constant arithmetic */
		{{0xE7FE}, true}, /* Pure branch */
		{{0x3001, 0x2800, 0xD1FC, 0xE7FE}, false}, /* Counter */
		{{0x8808, 0x3102, 0x2800, 0xD0FB, 0xE7FE}, false}, /* Moving pointer */
		{{0x8808, 0x8008, 0x2800, 0xD0FB, 0xE7FE}, false}, /* Store */
		{{0x4340, 0x2800, 0xD0FC, 0xE7FE}, false}, /* Unsupported MUL */
	};
	static uint32_t code[256], wram[GBA_SIZE_EWRAM / 4], iwram[GBA_SIZE_IWRAM / 4];
	static struct GBA gba, before, expectedGBA, nextBefore;
	static const uint32_t addresses[] = {0x04000006, 0x04010006, 0x04000007, 0x03000000, 0x02000000, 0x04000100};
	static const int distances[] = {-4, 0, 1, 14, 16, 32};
	struct ARMCore actual = {0};
	unsigned checks = 0, accelerated = 0, nativeProbes = 0;
	uint32_t longestNative = 0, shortestReference = UINT32_MAX;
	for (unsigned p = 0; p < sizeof(programs) / sizeof(*programs); ++p) {
		for (unsigned trial = 0; trial < 96; ++trial) {
			for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
			memcpy((uint8_t*) code + 0x100, programs[p].code, sizeof(programs[p].code));
			STORE_32(0, 0x110, code);
			memset(&gba, 0, sizeof(gba));
			memset(&actual, 0, sizeof(actual));
			uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
			gba.cpu = &actual;
			gba.isPristine = true;
			gba.memory.rom = code;
			gba.memory.romSize = sizeof(code);
			gba.memory.romMask = sizeof(code) - 1;
			gba.memory.wram = wram;
			gba.memory.iwram = iwram;
			gba.memory.activeRegion = start >> BASE_OFFSET;
			gba.memory.prefetch = trial & 1;
			gba.memory.waitstatesNonseq16[GBA_REGION_EWRAM] = 2 + (trial & 3);
			gba.memory.waitstatesNonseq32[GBA_REGION_EWRAM] = 5 + (trial & 3);
			gba.memory.lastPrefetchedPc = start + distances[trial % 6];
			gba.idleOptimization = IDLE_LOOP_IGNORE;
			gba.haltPending = true;
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.master = &gba.d;
			actual.executionMode = MODE_THUMB;
			actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
			actual.gprs[0] = 0;
			actual.gprs[1] = addresses[(trial / 16) % 6];
			actual.gprs[2] = 0;
			actual.gprs[ARM_PC] = start + 2;
			actual.prefetch[0] = programs[p].code[0];
			actual.prefetch[1] = p == 7 ? 0xE7FE : programs[p].code[1];
			if (p == 7) STORE_16(0xE7FE, 0x102, code);
			actual.memory.activeRegion = code;
			actual.memory.activeMask = sizeof(code) - 2;
			actual.memory.activeSeqCycles16 = trial % 4;
			actual.memory.activeNonseqCycles16 = (trial / 4) % 4;
			actual.memory.setActiveRegion = GBASetActiveRegion;
			actual.memory.load8 = GBALoad8;
			actual.memory.load16 = GBALoad16;
			actual.memory.load32 = GBALoad32;
			actual.memory.store16 = GBAStore16;
			actual.memory.stall = pollingStall;
			actual.cycles = trial & 8 ? INT32_MIN : -13;
			actual.nextEvent = actual.cycles + (trial & 31);
			actual.irqh.processEvents = noEvents;
			mTimingInit(&gba.timing, &actual.cycles, &actual.nextEvent);
			RV32Invalidate(&actual);
			struct RV32Context* context = _contextFor(&actual, &gba);
			struct RV32Block* block = primeBlock(context, &actual, start);
			if (!block || !block->executable || block->pollLoop != programs[p].candidate) {
				printf("FAIL polling classification program=%u trial=%u\n", p, trial); exit(1);
			}
			for (unsigned variant = 0; variant < 5; ++variant) {
				if (variant == 4 && (!programs[p].candidate || trial % 17)) continue;
				/* Previous variants may replace a slot after a waitstate
				 * callback. Prime the intended configuration for this run. */
				RV32Invalidate(&actual);
				context = _contextFor(&actual, &gba);
				block = primeBlock(context, &actual, start);
				if (!block || !block->executable) { puts("FAIL polling cache setup"); exit(1); }
				struct ARMCore initial = actual;
				before = gba;
				if (variant == 1) {
					actual.nextEvent = actual.cycles + 200;
					if (p <= 1) actual.irqh.processEvents = pollingEvent;
				}
				if (variant == 2) {
					actual.nextEvent = actual.cycles + 150;
					actual.memory.load8 = pollingRead;
					actual.memory.load16 = pollingRead;
					actual.memory.load32 = pollingRead;
				}
				if (variant == 3) actual.memory.setActiveRegion = branchRegion;
				if (variant == 4) {
					actual.cycles = 0;
					actual.nextEvent = 100000;
					actual.gprs[1] = 0x04000006;
				}
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				gba.timing.relativeCycles = &expected.cycles;
				gba.timing.nextEvent = &expected.nextEvent;
				uint32_t measured = instructionsRetired();
				ARMRunLoopLegacy(&expected);
				uint32_t referenceInstructions = instructionsRetired() - measured;
				expectedGBA = gba;
				expectedGBA.cpu = &actual;
				expectedGBA.timing.relativeCycles = &actual.cycles;
				expectedGBA.timing.nextEvent = &actual.nextEvent;
				gba = before;
				measured = instructionsRetired();
				if (variant == 4) {
					/* These fixtures have positive waits. Coalescing must
					 * happen inside native code, before entering its C wrapper. */
					for (unsigned probe = 0; probe < 2; ++probe) {
						block->loopPure = true;
						if (!((bool (*)(struct ARMCore*)) (uintptr_t) block->nativeCode)(&actual) ||
						    !block->loopPure) break;
					}
					if (actual.cycles < 99900) { puts("FAIL Thumb native probe did not coalesce"); exit(1); }
					++nativeProbes;
				}
				RV32RunLoop(&actual);
				uint32_t nativeInstructions = instructionsRetired() - measured;
				if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL polling program=%u trial=%u variant=%u pc=%08lx/%08lx cycles=%ld/%ld\n",
					       p, trial, variant, (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
					       (long) actual.cycles, (long) expected.cycles); exit(1);
				}
				if (variant == 4 && programs[p].candidate) {
					if (nativeInstructions >= referenceInstructions / 8) {
						printf("FAIL polling did not coalesce program=%u trial=%u instructions=%lu/%lu\n", p, trial,
						       (unsigned long) nativeInstructions, (unsigned long) referenceInstructions); exit(1);
					}
					if (nativeInstructions > longestNative) longestNative = nativeInstructions;
					if (referenceInstructions < shortestReference) shortestReference = referenceInstructions;
					++accelerated;
				}
				if (variant == 1 && p <= 1) {
					/* The event changed VCOUNT. The next slice must reload
					 * it, observe the changed condition and leave the loop. */
					nextBefore = gba;
					expected = actual;
					gba.cpu = &expected;
					gba.timing.relativeCycles = &expected.cycles;
					gba.timing.nextEvent = &expected.nextEvent;
					ARMRunLoopLegacy(&expected);
					expectedGBA = gba;
					expectedGBA.cpu = &actual;
					expectedGBA.timing.relativeCycles = &actual.cycles;
					expectedGBA.timing.nextEvent = &actual.nextEvent;
					gba = nextBefore;
					RV32RunLoop(&actual);
					if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
						printf("FAIL polling event change program=%u trial=%u\n", p, trial); exit(1);
					}
					++checks;
				}
				actual = initial;
				gba = before;
				++checks;
			}
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u polling event states; %u long loops coalesced (native <= %lu instructions, interpreter >= %lu)\n",
	       checks, accelerated, (unsigned long) longestNative, (unsigned long) shortestReference);
	printf("PASS: %u Thumb loops coalesced entirely inside native probe entries\n", nativeProbes);
}

static void checkContexts(void) {
	static struct GBA gbas[RV32_MAX_CONTEXTS + 1];
	struct ARMCore cpus[RV32_MAX_CONTEXTS + 1] = {0};
	uint32_t code[RV32_MAX_CONTEXTS + 2][256];
	for (unsigned c = 0; c < RV32_MAX_CONTEXTS + 2; ++c) {
		for (unsigned i = 0; i < 256; ++i) code[c][i] = 0xE7FEE7FE;
		STORE_16(0x3001 + c, 0x100, code[c]);
		STORE_16(0xE00D, 0x102, code[c]);
		STORE_16(0x3101 + c, 0x120, code[c]);
		STORE_16(0xE7ED, 0x122, code[c]);
	}
	for (unsigned c = 0; c < RV32_MAX_CONTEXTS + 1; ++c) {
		struct ARMCore* cpu = &cpus[c];
		memset(&gbas[c], 0, sizeof(gbas[c]));
		gbas[c].isPristine = true;
		gbas[c].memory.rom = code[c];
		gbas[c].memory.romSize = sizeof(code[c]);
		cpu->master = &gbas[c].d;
		cpu->executionMode = MODE_THUMB;
		cpu->cpsr.packed = 0x3F;
		cpu->gprs[ARM_PC] = 0x08000102;
		cpu->prefetch[0] = 0x3001 + c;
		cpu->prefetch[1] = 0xE00D;
		cpu->memory.activeRegion = code[c];
		cpu->memory.activeMask = sizeof(code[c]) - 2;
		cpu->memory.activeSeqCycles16 = c & 3;
		cpu->memory.activeNonseqCycles16 = 4;
		cpu->memory.setActiveRegion = region;
		cpu->irqh.processEvents = noEvents;
		RV32Invalidate(cpu);
	}
	for (unsigned round = 0; round < 32; ++round) {
		for (unsigned c = 0; c < RV32_MAX_CONTEXTS + 1; ++c) {
			struct ARMCore* cpu = &cpus[c];
			if (round == 8 && c == 0) {
				/* Keep already-prefetched instructions when replacing the ROM. */
				gbas[c].memory.rom = code[RV32_MAX_CONTEXTS + 1];
				cpu->memory.activeRegion = gbas[c].memory.rom;
			}
			if (round == 16) RV32Invalidate(cpu);
			cpu->nextEvent = cpu->cycles + 97 + round;
			struct ARMCore expected = *cpu;
			RV32RunLoop(cpu);
			ARMRunLoopLegacy(&expected);
			if (memcmp(cpu, &expected, sizeof(*cpu))) {
				printf("FAIL context eviction round=%u cpu=%u\n", round, c);
				exit(1);
			}
		}
	}
	for (unsigned c = 0; c < RV32_MAX_CONTEXTS + 1; ++c) RV32Invalidate(&cpus[c]);
	puts("PASS: context reuse/eviction, ROM replacement and explicit invalidation");
}

static void checkProfileLocations(void) {
	struct ARMCore cpu = {0}, other = {0};
	static struct GBA gba;
	RV32Invalidate(&cpu);
	struct RV32Context* context = _contextFor(&cpu, &gba);
	struct RV32Block* block = &context->blocks[0];
	struct RV32CodeLocation before, after;
#define PROFILE_CHECK(expr) do { if (!(expr)) { printf("FAIL profile lookup line %d\n", __LINE__); exit(1); } } while (0)
	/* A native address is not a durable identity: preserve the resolved key
	 * before a cache collision, code replacement, or CPU-context reuse. */
	block->valid = true;
	block->arm = false;
	block->nativeWords = 4;
	block->start = 0x08000100;
	uintptr_t pc = (uintptr_t) &block->nativeCode[3];
	PROFILE_CHECK(RV32ResolveCode(&cpu, pc, &before));
	PROFILE_CHECK(before.kind == RV32_CODE_THUMB && before.address == 0x08000100 && before.offset == 12);
	PROFILE_CHECK(!RV32ResolveCode(&other, pc, &after));
	PROFILE_CHECK(!RV32ResolveCode(NULL, pc, &after));
	PROFILE_CHECK(!RV32ResolveCode(&cpu, pc + 1, &after));
	PROFILE_CHECK(!RV32ResolveCode(&cpu, pc + 4, &after));
	PROFILE_CHECK(!RV32ResolveCode(&cpu, (uintptr_t) block, &after));
	block->arm = true;
	block->start = 0x08004000;
	PROFILE_CHECK(RV32ResolveCode(&cpu, pc, &after));
	PROFILE_CHECK(after.kind == RV32_CODE_ARM && after.address == 0x08004000 && before.address == 0x08000100);
	block->valid = false;
	PROFILE_CHECK(!RV32ResolveCode(&cpu, pc, &after));
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) context->dispatchCode, &after) && after.kind == RV32_CODE_THUMB_DISPATCH);
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) context->armDispatchCode, &after) && after.kind == RV32_CODE_ARM_DISPATCH);
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) context->branchCode, &after) && after.kind == RV32_CODE_THUMB_BRANCH);
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) context->armBranchCode, &after) && after.kind == RV32_CODE_ARM_BRANCH);
#ifdef MGBA_RV32_TRACE_POLL
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) &context->traceCode[3], &after) &&
	              after.kind == RV32_CODE_THUMB_TRACE && after.offset == 12);
#endif
	if (!context->dataReady) { PROFILE_CHECK(_emitDataHelpers(context)); context->dataReady = true; }
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) &context->dataCode[7][5], &before));
	PROFILE_CHECK(RV32ResolveCode(&cpu, (uintptr_t) &context->dataCodeTail[7][5], &after));
	PROFILE_CHECK(before.kind == RV32_CODE_DATA && before.address == 7 && before.offset == 20);
	PROFILE_CHECK(before.kind == after.kind && before.address == after.address && before.offset == after.offset);
	RV32Invalidate(&cpu);
	PROFILE_CHECK(!RV32ResolveCode(&cpu, (uintptr_t) context->dispatchCode, &after));
	_contextFor(&other, &gba);
	PROFILE_CHECK(!RV32ResolveCode(&cpu, pc, &after));
	RV32Invalidate(&other);
#undef PROFILE_CHECK
	puts("PASS: sampled code attribution survives block/context reuse, distinguishes ARM/Thumb/helpers and rejects stale PCs");
}

static void armReferenceStep(struct ARMCore* cpu, uint32_t opcode, uint32_t next, uint32_t following) {
	cpu->prefetch[0] = next;
	cpu->prefetch[1] = following;
	cpu->gprs[ARM_PC] += 4;
	if ((opcode >> 28) != 15 && ARMTestCondition(cpu, opcode >> 28))
		_armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 15)](cpu, opcode);
	else cpu->cycles += 1 + cpu->memory.activeSeqCycles32;
}

static void armCompare(const struct ARMCore* actual, const struct ARMCore* expected, uint32_t opcode, unsigned trial) {
	if (!memcmp(actual, expected, sizeof(*actual))) return;
	printf("FAIL ARM opcode=%08lx trial=%u cycles=%ld/%ld flags=%08lx/%08lx shifter=%08lx,%08lx/%08lx,%08lx\n",
	       (unsigned long) opcode, trial, (long) actual->cycles, (long) expected->cycles,
	       (unsigned long) actual->cpsr.packed, (unsigned long) expected->cpsr.packed,
	       (unsigned long) actual->shifterOperand, (unsigned long) actual->shifterCarryOut,
	       (unsigned long) expected->shifterOperand, (unsigned long) expected->shifterCarryOut);
	for (unsigned r = 0; r < 16; ++r) if (actual->gprs[r] != expected->gprs[r])
		printf("r%u=%08lx/%08lx\n", r, (unsigned long) actual->gprs[r], (unsigned long) expected->gprs[r]);
	exit(1);
}

static void checkARMArithmetic(void) {
	static struct RV32Block block;
	static const uint32_t values[] = {0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000,
		0x80000001, 31, 32, 33, 255, 256, 0xFFFFFF00, 0x55555555, 0xAAAAAAAA, 0xFFFF, 0x10000};
	unsigned checks = 0, encodings = 0;
	struct ARMCore configuration = {0};
	configuration.memory.activeSeqCycles32 = 2;
	for (unsigned operation = 0; operation < 16; ++operation) {
		if (operation >= 5 && operation <= 7) continue;
		for (unsigned form = 0; form < 144; ++form) {
			for (unsigned variant = 0; variant < 8; ++variant) {
				if (operation >= 8 && operation <= 11 && !(variant & 1)) continue;
				unsigned rd = (form + variant) % 15, rn = (rd + 3) % 16, rm = (rd + 7) % 16;
				if (variant & 2) rn = rd;
				if (variant & 4) rm = rd;
				uint32_t operand = form < 128 ? (form << 5) | rm :
					0x02000000 | ((form - 128) << 8) | ((form * 41 + variant * 23) & 255);
				uint32_t opcode = ((form + variant) % 16 << 28) | (operation << 21) |
					((variant & 1) << 20) | (rn << 16) | (rd << 12) | operand;
				memset(&block, 0, sizeof(block));
				block.arm = true;
				block.start = 0x08000100;
				block.length = 1;
				block.armOpcodes[0] = opcode;
				block.armOpcodes[1] = 0xEAFFFFFE;
				block.armOpcodes[2] = 0xE1A00000;
				if (!_isARMALU(opcode) || !_emitARMBlock(&block, &configuration)) { puts("FAIL ARM arithmetic emission"); exit(1); }
				++encodings;
				for (unsigned trial = 0; trial < 16; ++trial) {
					struct ARMCore actual = {0};
					for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = values[(trial + r) & 15] ^ (trial & 1 ? randomWord() : 0);
					actual.gprs[ARM_PC] = block.start + 4;
					actual.cpsr.packed = (trial << 28) | 0x0123451F;
					actual.spsr.packed = randomWord();
					actual.shifterOperand = randomWord();
					actual.shifterCarryOut = randomWord();
					actual.cycles = trial & 1 ? INT32_MIN : -7;
					actual.nextEvent = actual.cycles + 1;
					actual.memory.activeSeqCycles32 = trial & 3;
					actual.prefetch[0] = opcode;
					actual.prefetch[1] = block.armOpcodes[1];
					struct ARMCore expected = actual;
					armReferenceStep(&expected, opcode, block.armOpcodes[1], block.armOpcodes[2]);
					if (!((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual)) { puts("FAIL ARM execution"); exit(1); }
					armCompare(&actual, &expected, opcode, trial);
					++checks;
				}
			}
		}
	}
	printf("PASS: %u ARM ALU encodings, %u complete states, all conditions/NZCV, immediate shifts/rotates, PC operands and aliases\n", encodings, checks);
}

static uint32_t armVisibleRead(struct ARMCore* cpu, uint32_t address, int* cycles) {
	return visibleRead(cpu, address, cycles) ^ cpu->privilegeMode;
}
static uint32_t armVisibleRead8(struct ARMCore* cpu, uint32_t address, int* cycles) {
	return armVisibleRead(cpu, address, cycles) & 255;
}

static void checkARMLoads(void) {
	static const uint32_t opcodes[] = {0xE5910000, 0xE5D10000, 0xE5110001, 0xE5510001,
		0xE5911000, 0xE59D0000, 0xE59F000C, 0x05910000, 0x15910000, 0xF5910000,
		0xE4B10000, 0xE4F10000, 0xE4310003, 0xE4710003, /* LDRT/LDRBT +/- */
		0xE4B11003, 0xE4BD0004, 0xE49D0004, 0xE53D0004, /* aliases and SP */
		0xE5B10003, 0xE5310003, 0xE4910003, 0xE4110003, /* pre/post writeback */
		0x14B10000, 0xF4B10000, 0xE49F0000, 0xE4BF0004,
		0xE59F0000, 0xE59F0001, 0xE59F0002, 0xE59F0003,
		0xE51F0001, 0xE51F0002, 0xE51F0003, 0xE59F0FFF, 0xE51F0FFF};
	static const uint32_t addresses[] = {0x02000000, 0x02000001, 0x0203FFFE, 0x0203FFFF,
		0x02FFFFFE, 0x02FFFFFF, 0x03000000, 0x03000001, 0x03007FFE, 0x03007FFF,
		0x03FFFFFE, 0x03FFFFFF, 0x04000006, 0x08000010};
	static const int distances[] = {-4, -1, 0, 1, 2, 14, 15, 16, 32};
	static struct GBA gba, before, expectedGBA;
	static uint32_t wram[GBA_SIZE_EWRAM / 4], iwram[GBA_SIZE_IWRAM / 4], rom[256];
	static struct RV32Block block;
	for (unsigned i = 0; i < GBA_SIZE_EWRAM / 4; ++i) wram[i] = randomWord();
	for (unsigned i = 0; i < GBA_SIZE_IWRAM / 4; ++i) iwram[i] = randomWord();
	for (unsigned i = 0; i < 256; ++i) rom[i] = randomWord();
	struct ARMCore configuration = {0};
	configuration.memory.load8 = GBALoad8;
	configuration.memory.load16 = GBALoad16;
	configuration.memory.load32 = GBALoad32;
	configuration.memory.setActiveRegion = region;
	unsigned checks = 0;
	for (unsigned op = 0; op < sizeof(opcodes) / sizeof(*opcodes); ++op) {
		memset(&block, 0, sizeof(block));
		selectDataHelpers(&block);
		block.arm = true;
		block.start = 0x08000100;
		block.length = 1;
		block.armOpcodes[0] = opcodes[op];
		block.armOpcodes[1] = 0xE2877007;
		block.armOpcodes[2] = 0xEAFFFFFE;
		if (!_emitARMBlock(&block, &configuration)) { puts("FAIL RAM emission"); exit(1); }
		for (unsigned trial = 0; trial < 128; ++trial) {
			for (unsigned a = 0; a < sizeof(addresses) / sizeof(*addresses); ++a) {
				memset(&gba, 0, sizeof(gba));
				gba.memory.wram = wram;
				gba.memory.iwram = iwram;
				gba.memory.rom = rom;
				gba.memory.romSize = sizeof(rom);
				gba.haltPending = true;
				gba.memory.prefetch = trial >> 6;
				gba.memory.activeRegion = trial % 3 ? GBA_REGION_ROM0 : GBA_REGION_EWRAM;
				gba.memory.waitstatesNonseq16[GBA_REGION_EWRAM] = (trial * 3) & 15;
				gba.memory.waitstatesNonseq32[GBA_REGION_EWRAM] = (trial * 5) & 31;
				gba.memory.lastPrefetchedPc = block.start + 4 + distances[(trial + a) % 9];
				struct ARMCore actual = configuration;
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.master = &gba.d;
				gba.cpu = &actual;
				actual.memory.activeRegion = rom;
				actual.memory.activeMask = sizeof(rom) - 4;
				actual.gprs[ARM_PC] = block.start + 4;
				actual.gprs[1] = addresses[a];
				actual.gprs[2] = 0;
				actual.gprs[ARM_SP] = addresses[a];
				actual.prefetch[0] = block.armOpcodes[0];
				actual.prefetch[1] = block.armOpcodes[1];
				static const enum PrivilegeMode modes[] = {MODE_USER, MODE_SYSTEM, MODE_IRQ, MODE_SUPERVISOR, MODE_FIQ, MODE_UNDEFINED, MODE_ABORT, MODE_SYSTEM};
				actual.privilegeMode = modes[(trial >> 1) & 7];
				actual.cpsr.packed = ((trial & 15) << 28) | actual.privilegeMode;
				for (unsigned bank = 0; bank < 6; ++bank) {
					for (unsigned reg = 0; reg < 7; ++reg) actual.bankedRegisters[bank][reg] = randomWord();
					actual.bankedSPSRs[bank] = randomWord();
				}
				actual.cycles = (int) trial - 13;
				actual.nextEvent = actual.cycles + 1;
				actual.memory.activeSeqCycles16 = trial & 7;
				actual.memory.activeNonseqCycles16 = (trial >> 3) & 7;
				actual.memory.activeSeqCycles32 = (trial * 3) & 15;
				actual.memory.activeNonseqCycles32 = (trial * 5) & 31;
				if (trial % 17 == 0) {
					actual.memory.load8 = armVisibleRead8;
					actual.memory.load16 = read16;
					actual.memory.load32 = armVisibleRead;
				}
				before = gba;
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				armReferenceStep(&expected, block.armOpcodes[0], block.armOpcodes[1], block.armOpcodes[2]);
				expectedGBA = gba;
				expectedGBA.cpu = &actual;
				gba = before;
				struct ARMCore initial = actual;
				bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
				if (!complete && sharedDataTest) {
					if (memcmp(&actual, &initial, sizeof(actual)) || memcmp(&gba, &before, sizeof(gba))) {
						puts("FAIL shared ARM rejection changed state"); exit(1);
					}
					armReferenceStep(&actual, block.armOpcodes[0], block.armOpcodes[1], block.armOpcodes[2]);
					complete = true;
				}
				if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL ARM load opcode=%08lx address=%08lx trial=%u cycles=%ld/%ld prefetch=%08lx/%08lx\n",
					       (unsigned long) block.armOpcodes[0], (unsigned long) addresses[a], trial, (long) actual.cycles,
					       (long) expected.cycles, (unsigned long) gba.memory.lastPrefetchedPc,
					       (unsigned long) expectedGBA.memory.lastPrefetchedPc);
					exit(1);
				}
				++checks;
			}
		}
	}
	printf("PASS: %u ARM RAM/IO/ROM load states, mirrors, alignment, waitstates, prefetch and callback replacement\n", checks);
}

static void checkARMBranches(void) {
	static uint32_t rom[1024];
	static struct GBA gba, before, expectedGBA;
	static struct RV32Block block;
	unsigned checks = 0;
	for (unsigned i = 0; i < 1024; ++i) rom[i] = 0xE2800001 + i;
	for (unsigned condition = 0; condition < 16; ++condition) {
		for (unsigned trial = 0; trial < 128; ++trial) {
			struct ARMCore actual = {0};
			memset(&gba, 0, sizeof(gba));
			memset(&block, 0, sizeof(block));
			block.arm = true;
			block.start = 0x08000200 + (trial % 6) * 0x01000000;
			block.length = 1;
			int offset = trial & 1 ? -4 : 12;
			uint32_t opcode = (condition << 28) | 0x0A000000 | ((trial & 2) << 23) | ((uint32_t) offset & 0xFFFFFF);
			block.armOpcodes[0] = opcode;
			block.armOpcodes[1] = 0xE2811001;
			block.armOpcodes[2] = 0xE2822001;
			gba.cpu = &actual;
			gba.isPristine = true;
			gba.memory.rom = rom;
			gba.memory.romSize = GBA_SIZE_ROM0;
			gba.memory.romMask = sizeof(rom) - 1;
			gba.memory.activeRegion = block.start >> BASE_OFFSET;
			gba.memory.lastPrefetchedPc = block.start + 6;
			gba.idleOptimization = IDLE_LOOP_IGNORE;
			actual.master = &gba.d;
			actual.memory.activeRegion = rom;
			actual.memory.activeMask = sizeof(rom) - 4;
			actual.memory.activeSeqCycles32 = trial & 3;
			actual.memory.activeNonseqCycles32 = trial & 7;
			actual.memory.setActiveRegion = trial & 16 ? region : GBASetActiveRegion;
			actual.gprs[ARM_PC] = block.start + 4;
			actual.gprs[ARM_LR] = randomWord();
			actual.cpsr.packed = ((trial & 15) << 28) | 0x1F;
			actual.cycles = (int) trial - 13;
			actual.nextEvent = actual.cycles + 1;
			actual.prefetch[0] = opcode;
			actual.prefetch[1] = block.armOpcodes[1];
			if (!_emitARMBlock(&block, &actual)) { puts("FAIL ARM branch emission"); exit(1); }
			before = gba;
			struct ARMCore expected = actual;
			armReferenceStep(&expected, opcode, block.armOpcodes[1], block.armOpcodes[2]);
			expectedGBA = gba;
			gba = before;
			((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
			armCompare(&actual, &expected, opcode, trial);
			if (memcmp(&gba, &expectedGBA, sizeof(gba))) { puts("FAIL ARM branch GBA state"); exit(1); }
			++checks;
		}
	}
	printf("PASS: %u ARM B/BL states, conditions, links, six ROM regions, callbacks and pipeline refill\n", checks);
}

static uint32_t armChangingRead(struct ARMCore* cpu, uint32_t address, int* cycles) {
	uint32_t value = visibleRead(cpu, address, cycles);
	cpu->gprs[7] ^= 0x13579BDF;
	cpu->cpsr.packed ^= 0x30000000;
	cpu->memory.activeSeqCycles32 ^= 3;
	cpu->cycles += 2;
	return value;
}

static void armChangingStore(struct ARMCore* cpu, uint32_t address, int32_t value, int* cycles) {
	struct GBA* gba = (struct GBA*) cpu->master;
	++gba->idleDetectionFailures;
	cpu->memory.activeSeqCycles32 = value & 3;
	*cycles += 3;
	if (address & 1) {
		gba->memory.rom[0x100 / 4] = 0xE2800002;
		gba->isPristine = false;
	}
}

static void checkARMRunner(void) {
	static uint32_t code[256], savedCode[256], expectedCode[256];
	static struct GBA gba, before, expectedGBA;
	struct ARMCore actual = {0};
	unsigned checks = 0;
	for (unsigned program = 0; program < 7; ++program) {
		for (unsigned trial = 0; trial < 128; ++trial) {
			for (unsigned i = 0; i < 256; ++i) code[i] = 0xEAFFFFFE;
			for (unsigned i = 0; i < 32; ++i) code[0x100 / 4 + i] =
				0xE2900001 | ((i & 7) << 16) | ((i & 7) << 12);
			code[0x180 / 4] = 0xEAFFFFDE; /* Back to the long block. */
			if (program == 1) {
				code[0x104 / 4] = 0xEA00001D; /* A -> B. */
				code[0x180 / 4] = 0xE2811001;
				code[0x184 / 4] = 0xEAFFFFDD; /* B -> A. */
			} else if (program == 2) {
				code[0x100 / 4] = 0xE1B00211; /* MOVS r0,r1,LSL r2. */
				code[0x104 / 4] = 0xE0B03004; /* ADCS r3,r0,r4. */
				code[0x108 / 4] = 0xE0100190; /* MULS r0,r0,r1. */
				code[0x10C / 4] = 0xEAFFFFFB;
			} else if (program == 3) {
				code[0x100 / 4] = 0xE5910000;
				code[0x104 / 4] = 0xE2877001;
				code[0x108 / 4] = 0xEAFFFFFC;
			} else if (program == 4) {
				code[0x100 / 4] = 0xE12FFF13; /* BX r3. */
				code[0x200 / 4] = 0x47203001; /* Thumb ADD r0,1; BX r4. */
				code[0x204 / 4] = 0xE7FEE7FE;
			} else if (program >= 5) {
				code[0x100 / 4] = 0xE5810000;
				code[0x104 / 4] = 0xE2800001;
				code[0x108 / 4] = 0xEAFFFFFC;
			}
			memset(&gba, 0, sizeof(gba));
			memset(&actual, 0, sizeof(actual));
			uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
			gba.isPristine = true;
			gba.cpu = &actual;
			gba.memory.rom = code;
			gba.memory.romSize = sizeof(code);
			gba.memory.romMask = sizeof(code) - 1;
			gba.memory.activeRegion = start >> BASE_OFFSET;
			gba.idleOptimization = IDLE_LOOP_IGNORE;
			actual.master = &gba.d;
			actual.executionMode = MODE_ARM;
			actual.cpsr.packed = ((trial & 15) << 28) | 0x1F;
			for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
			actual.gprs[1] = program == 6 ? 1 : 0;
			actual.gprs[2] = trial * 3;
			actual.gprs[3] = (start + 0x100) | 1;
			actual.gprs[4] = start;
			actual.gprs[ARM_PC] = start + 4;
			actual.memory.activeRegion = code;
			actual.memory.activeMask = sizeof(code) - 4;
			actual.memory.activeSeqCycles32 = trial & 3;
			actual.memory.activeNonseqCycles32 = trial & 7;
			actual.memory.activeSeqCycles16 = 2;
			actual.memory.activeNonseqCycles16 = 4;
			actual.memory.setActiveRegion = trial & 16 ? region : GBASetActiveRegion;
			actual.memory.load32 = GBALoad32;
			actual.memory.store32 = armChangingStore;
			actual.memory.stall = pollingStall;
			actual.irqh.processEvents = noEvents;
			actual.prefetch[0] = code[0x100 / 4];
			actual.prefetch[1] = code[0x104 / 4];
			actual.cycles = trial & 32 ? INT32_MIN : -13;
			actual.nextEvent = actual.cycles + trial * 3;
			RV32Invalidate(&actual);
			struct RV32Context* context = _contextFor(&actual, &gba);
			struct RV32Block* first = primeBlock(context, &actual, start);
			if (!first || !first->arm || !first->executable || !context->armDispatchReady || !context->armBranchReady) {
				puts("FAIL ARM runner setup"); exit(1);
			}
			/* Pre-cache the second ARM block and a Thumb target. */
			struct ARMCore initial = actual;
			actual.gprs[ARM_PC] = start + 0x84;
			actual.prefetch[0] = code[0x180 / 4]; actual.prefetch[1] = code[0x184 / 4];
			primeBlock(context, &actual, start + 0x80);
			actual.executionMode = MODE_THUMB;
			actual.cpsr.t = 1;
			actual.memory.activeMask |= 2;
			actual.gprs[ARM_PC] = start + 0x102;
			actual.prefetch[0] = code[0x200 / 4] & 0xFFFF; actual.prefetch[1] = code[0x200 / 4] >> 16;
			primeBlock(context, &actual, start + 0x100);
			actual = initial;
			if (program == 3) actual.memory.load32 = armChangingRead;
			/* Distinct mapping, stale pipeline and cache mode must reject the
			 * previously compiled entry, even at the same numerical address. */
			if (program == 0 && (trial & 8)) actual.prefetch[1] = 0xE2833007;
			for (unsigned slice = 0; slice < (program == 4 ? 4u : 1u); ++slice) {
				if (slice) actual.nextEvent = actual.cycles + 137;
				before = gba;
				memcpy(savedCode, code, sizeof(code));
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				ARMRunLoopLegacy(&expected);
				expectedGBA = gba;
				expectedGBA.cpu = &actual;
				memcpy(expectedCode, code, sizeof(code));
				gba = before;
				memcpy(code, savedCode, sizeof(code));
				/* Exercise the public integration entry, not only the private JIT. */
				ARMRunLoop(&actual);
				armCompare(&actual, &expected, savedCode[0x100 / 4], trial);
				if (memcmp(&gba, &expectedGBA, sizeof(gba)) || memcmp(code, expectedCode, sizeof(code))) {
					printf("FAIL ARM runner GBA program=%u trial=%u slice=%u\n", program, trial, slice); exit(1);
				}
				++checks;
			}
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u ARM event slices, long blocks, native links, changing callbacks, ROM writes, stale pipeline and ARM/Thumb transitions\n", checks);
}

static void checkARMPollingLoops(void) {
	static const uint32_t programs[][5] = {
		{0xEAFFFFFE},
		{0xE5D10000, 0xE3500000, 0x0AFFFFFC, 0xEAFFFFFE},
		{0xE5910000, 0xE3500000, 0x0AFFFFFC, 0xEAFFFFFE},
		{0xE3A00000, 0xE3500000, 0x0AFFFFFC, 0xEAFFFFFE},
		{0xE2800001, 0xE3500000, 0x1AFFFFFC, 0xEAFFFFFE},
		{0xE4B10000, 0xE3500000, 0x0AFFFFFC, 0xEAFFFFFE},
		{0xE4B10001, 0xE3500000, 0x0AFFFFFC, 0xEAFFFFFE},
		{0xE4F10000, 0xE3500000, 0x0AFFFFFC, 0xEAFFFFFE},
	};
	static uint32_t code[256];
	static struct GBA gba, before, expectedGBA;
	struct ARMCore actual = {0};
	unsigned checks = 0, accelerated = 0, nativeProbes = 0;
	for (unsigned p = 0; p < sizeof(programs) / sizeof(*programs); ++p) {
		for (unsigned trial = 0; trial < 96; ++trial) {
			for (unsigned variant = 0; variant < 4; ++variant) {
				for (unsigned i = 0; i < 256; ++i) code[i] = 0xEAFFFFFE;
				memcpy(code + 0x100 / 4, programs[p], sizeof(programs[p]));
				if (!p) code[0x104 / 4] = 0xEAFFFFFE;
				memset(&gba, 0, sizeof(gba)); memset(&actual, 0, sizeof(actual));
				uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
				gba.cpu = &actual; gba.isPristine = !(trial & 32);
				gba.memory.rom = code; gba.memory.romSize = sizeof(code); gba.memory.romMask = sizeof(code) - 1;
				gba.memory.activeRegion = start >> BASE_OFFSET;
				gba.memory.prefetch = trial & 1;
				gba.memory.lastPrefetchedPc = start + (trial & 31);
				gba.idleOptimization = IDLE_LOOP_IGNORE;
				gba.haltPending = true;
				actual.master = &gba.d;
				actual.privilegeMode = trial & 1 ? MODE_USER : MODE_SYSTEM;
				actual.cpsr.packed = ((trial & 15) << 28) | 0x1F;
				actual.gprs[1] = trial & 2 ? 0x04000007 : 0x04000006;
				actual.gprs[ARM_PC] = start + 4;
				actual.memory.activeRegion = code; actual.memory.activeMask = sizeof(code) - 4;
				actual.memory.activeSeqCycles16 = trial & 3;
				actual.memory.activeNonseqCycles16 = (trial >> 2) & 3;
				actual.memory.activeSeqCycles32 = 2 * actual.memory.activeSeqCycles16 + 1;
				actual.memory.activeNonseqCycles32 = actual.memory.activeNonseqCycles16 + actual.memory.activeSeqCycles16 + 1;
				actual.memory.setActiveRegion = GBASetActiveRegion;
				actual.memory.load32 = GBALoad32; actual.memory.load8 = GBALoad8;
				actual.irqh.processEvents = pollingEvent;
				actual.cycles = trial & 8 ? INT32_MIN : -13;
				actual.nextEvent = actual.cycles + (variant == 0 ? trial & 31 : 200);
				actual.prefetch[0] = code[0x100 / 4]; actual.prefetch[1] = code[0x104 / 4];
				RV32Invalidate(&actual);
				struct RV32Context* context = _contextFor(&actual, &gba);
				struct RV32Block* block = primeBlock(context, &actual, start);
				if (!block || !block->executable || block->pollLoop != (p != 4 && p != 6)) { printf("FAIL ARM poll setup p=%u trial=%u variant=%u len=%u words=%u poll=%u\n", p, trial, variant, block ? block->length : 0, block ? block->nativeWords : 0, block ? block->pollLoop : 0); exit(1); }
				if (variant == 2) { actual.memory.load8 = pollingRead; actual.memory.load32 = pollingRead; }
				bool recover = variant == 2 && p == 5 && trial == 0;
				if (recover) actual.irqh.processEvents = noEvents;
				bool measure = variant == 3 && p != 4 && p != 6 && trial % 17 == 0;
				if (measure) { actual.cycles = 0; actual.nextEvent = 100000; }
				for (unsigned slice = 0; slice < (recover ? 36u : 2u); ++slice) {
					if (recover && slice) {
						actual.memory.load32 = GBALoad32;
						actual.nextEvent = actual.cycles + (slice == 35 ? 100000 : 200);
					}
					before = gba;
					struct ARMCore expected = actual;
					gba.cpu = &expected;
					uint32_t referenceInstructions = instructionsRetired();
					ARMRunLoopLegacy(&expected);
					referenceInstructions = instructionsRetired() - referenceInstructions;
					expectedGBA = gba; expectedGBA.cpu = &actual;
					gba = before;
					uint32_t nativeInstructions = instructionsRetired();
					if (measure && !slice) {
						for (unsigned probe = 0; probe < 3; ++probe) {
							block->loopPure = true;
							if (!((bool (*)(struct ARMCore*)) (uintptr_t) block->nativeCode)(&actual) ||
							    !block->loopPure) break;
						}
						if (actual.cycles < 99900) { puts("FAIL ARM native probe did not coalesce"); exit(1); }
						++nativeProbes;
					}
					ARMRunLoop(&actual);
					nativeInstructions = instructionsRetired() - nativeInstructions;
					armCompare(&actual, &expected, programs[p][0], trial);
					if (memcmp(&gba, &expectedGBA, sizeof(gba))) { puts("FAIL ARM polling GBA state"); exit(1); }
					if ((measure && !slice) || (recover && slice == 35)) {
						if (nativeInstructions >= referenceInstructions / 8) { printf("FAIL ARM loop did not coalesce program=%u trial=%u native=%lu reference=%lu\n", p, trial, (unsigned long) nativeInstructions, (unsigned long) referenceInstructions); exit(1); }
						++accelerated;
					}
					++checks;
				}
			}
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u ARM polling/event states; %u long loops coalesced, counter and side-effect callbacks preserved\n", checks, accelerated);
	printf("PASS: %u ARM loops coalesced entirely inside native probe entries\n", nativeProbes);
}

/* Mutations deliberately bypass emulator write hooks: debugger patches and
 * cartridge callbacks must be safe even when they modify the same buffer. */
static bool invalidateOnRead;
static unsigned codePatchOffset;
static uint32_t codePatchValue;
static void patchCode(struct ARMCore* cpu) {
	struct GBA* gba = (struct GBA*) cpu->master;
	if (cpu->executionMode == MODE_ARM) { STORE_32(codePatchValue, codePatchOffset, gba->memory.rom); }
	else { STORE_16(codePatchValue, codePatchOffset, gba->memory.rom); }
	gba->isPristine = false;
}
static uint32_t patchingRead(struct ARMCore* cpu, uint32_t address, int* cycles) {
	patchCode(cpu);
	if (invalidateOnRead) RV32Invalidate(cpu);
	*cycles += 3;
	return address ^ cpu->prefetch[0] ^ cpu->prefetch[1];
}

static void checkWritableCode(void) {
	static uint32_t code[256], savedCode[256], expectedCode[256], iwram[GBA_SIZE_IWRAM / 4];
	static struct GBA gba, before, expectedGBA;
	struct ARMCore actual = {0};
	unsigned checks = 0, nativeInvalidations = 0, nativeWraps = 0;
	for (unsigned arm = 0; arm < 2; ++arm) {
		unsigned width = arm ? 4 : 2;
		for (unsigned variant = 0; variant < 12; ++variant) {
			for (unsigned trial = 0; trial < 48; ++trial) {
				for (unsigned i = 0; i < 256; ++i) code[i] = arm ? 0xEAFFFFFE : 0xE7FEE7FE;
				const uint32_t armA[] = {0xE2800001, 0xE2811002, 0xE2822003, 0xEA00001B};
				const uint16_t thumbA[] = {0x3001, 0x3102, 0x3203, 0xE03B};
				const uint32_t armB[] = {0xE5930000, 0xE2822001, 0xE2844002, 0xE2855003, 0xEAFFFFDA};
				const uint16_t thumbB[] = {0x6818, 0x3201, 0x3402, 0x3503, 0xE7BA};
				memcpy((uint8_t*) code + 0x100, arm ? (const void*) armA : (const void*) thumbA, arm ? sizeof(armA) : sizeof(thumbA));
				memcpy((uint8_t*) code + 0x180, arm ? (const void*) armB : (const void*) thumbB, arm ? sizeof(armB) : sizeof(thumbB));
				memset(&gba, 0, sizeof(gba)); memset(&actual, 0, sizeof(actual));
				uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
				gba.cpu = &actual; gba.isPristine = variant == 6;
				gba.memory.rom = code; gba.memory.romSize = sizeof(code); gba.memory.romMask = sizeof(code) - 1;
				gba.memory.iwram = iwram; gba.memory.activeRegion = start >> BASE_OFFSET;
				gba.idleOptimization = IDLE_LOOP_IGNORE;
				actual.master = &gba.d; actual.executionMode = arm ? MODE_ARM : MODE_THUMB;
				actual.cpsr.packed = ((trial & 15) << 28) | (arm ? 0x1F : 0x3F);
				actual.privilegeMode = MODE_SYSTEM;
				for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
				actual.gprs[3] = GBA_BASE_IWRAM;
				actual.gprs[ARM_PC] = start + width;
				actual.memory.activeRegion = code; actual.memory.activeMask = sizeof(code) - width;
				actual.memory.activeSeqCycles16 = trial & 3; actual.memory.activeNonseqCycles16 = trial & 7;
				actual.memory.activeSeqCycles32 = 2 * (trial & 3) + 1;
				actual.memory.activeNonseqCycles32 = (trial & 7) + (trial & 3) + 1;
				actual.memory.setActiveRegion = GBASetActiveRegion;
				actual.memory.load32 = GBALoad32;
				actual.irqh.processEvents = variant == 5 ? patchCode : noEvents;
				actual.cycles = trial & 16 ? INT32_MIN : -13;
				actual.nextEvent = actual.cycles + 9 + trial * 3;
				actual.prefetch[0] = arm ? armA[0] : thumbA[0]; actual.prefetch[1] = arm ? armA[1] : thumbA[1];
				RV32Invalidate(&actual);
				struct RV32Context* context = _contextFor(&actual, &gba);
				struct ARMCore initial = actual;
				/* Prime all continuations to exercise native links after loads
				 * and after an event splits the middle of a previous block. */
				for (unsigned target = 0; target < 2; ++target) {
					for (unsigned i = 0; i < (target ? 5u : 4u); ++i) {
						uint32_t address = start + target * 0x80 + i * width;
						actual.gprs[ARM_PC] = address + width;
						if (arm) { LOAD_32(actual.prefetch[0], address & actual.memory.activeMask, code); LOAD_32(actual.prefetch[1], (address + width) & actual.memory.activeMask, code); }
						else { LOAD_16(actual.prefetch[0], address & actual.memory.activeMask, code); LOAD_16(actual.prefetch[1], (address + width) & actual.memory.activeMask, code); }
						struct RV32Block* block = primeBlock(context, &actual, address);
						if (!block || !block->executable || block->writable == gba.isPristine) { puts("FAIL writable setup"); exit(1); }
					}
				}
				actual = initial;
				invalidateOnRead = variant == 11;
				if ((variant >= 3 && variant <= 6) || variant >= 10) actual.memory.load32 = patchingRead;
				codePatchOffset = variant < 2 || variant == 7 ? 0x100 + (variant == 1 ? trial % 2 : 2) * width :
					0x180 + (variant == 4 ? 1 + trial % 2 : 3) * width;
				codePatchValue = arm ? 0xE3A04000 | (trial + 1) : 0x2400 | (trial + 1);
				if (variant < 3 || (variant >= 7 && variant < 10)) patchCode(&actual);
				if (variant == 8) actual.prefetch[trial % 2] = arm ? 0xE3A05077 : 0x2577;
				if (variant == 7 || variant == 10) {
					context->epochSerial = UINT32_MAX;
					for (unsigned i = 0; i < RV32_BLOCK_CACHE_SIZE; ++i) {
						context->blocks[i].verifiedEpoch = 1;
						context->blocks[i].branchEpoch = 1;
					}
				}
				for (unsigned slice = 0; slice < 3; ++slice) {
					if (slice) actual.nextEvent = actual.cycles + 83 + trial;
					before = gba; memcpy(savedCode, code, sizeof(code));
					struct ARMCore expected = actual;
					gba.cpu = &expected;
					ARMRunLoopLegacy(&expected);
					expectedGBA = gba; expectedGBA.cpu = &actual;
					memcpy(expectedCode, code, sizeof(code));
					gba = before; memcpy(code, savedCode, sizeof(code));
					if ((variant == 9 || variant == 10) && !slice) {
						/* Bypass the C lookup: the direct A -> B edge itself
						 * must reject a changed instruction inside target B. */
						if (variant == 9) context->codeEpoch = 0;
						struct RV32Block* first = &context->blocks[_blockIndex(start)];
						((bool (*)(struct ARMCore*)) (uintptr_t) first->nativeCode)(&actual);
						if (!context->blocks[_blockIndex(start + 0x80)].valid) ++nativeInvalidations;
						if (variant == 10 && context->epochSerial < UINT32_MAX) ++nativeWraps;
					}
					ARMRunLoop(&actual);
					if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba)) ||
					    memcmp(code, expectedCode, sizeof(code))) {
						printf("FAIL writable arm=%u variant=%u trial=%u slice=%u pc=%08lx/%08lx cycles=%ld/%ld prefetch=%08lx,%08lx/%08lx,%08lx\n",
						       arm, variant, trial, slice, (unsigned long) actual.gprs[ARM_PC], (unsigned long) expected.gprs[ARM_PC],
						       (long) actual.cycles, (long) expected.cycles, (unsigned long) actual.prefetch[0], (unsigned long) actual.prefetch[1],
						       (unsigned long) expected.prefetch[0], (unsigned long) expected.prefetch[1]);
						armCompare(&actual, &expected, 0, trial); exit(1);
					}
					++checks;
				}
			}
		}
	}
	RV32Invalidate(&actual);
	invalidateOnRead = false;
	if (!nativeInvalidations || !nativeWraps) { puts("FAIL writable direct-edge invalidation/native epoch wrap was not reached"); exit(1); }
	printf("PASS: %u writable ARM/Thumb slices, ROM aliases, host/callback/event patches, preserved prefetch, epoch wrap (%u native), %u direct-edge invalidations\n", checks, nativeWraps, nativeInvalidations);
}

static void eepromEvent(struct ARMCore* cpu) {
	struct GBA* gba = (struct GBA*) cpu->master;
	mTimingDeschedule(&gba->timing, &gba->memory.savedata.dust);
	cpu->nextEvent = cpu->cycles + 200;
}

static void checkEEPROM(void) {
	static const uint16_t opcodes[] = {0x8808, 0x8809, 0x5A88, 0x5E88};
	static const int8_t remaining[] = {-1, 0, 1, 63, 64, 65, 68, 127};
	static struct GBA gba, before, expectedGBA;
	static uint8_t savedata[GBA_SIZE_EEPROM];
	static uint32_t code[256];
	static struct RV32Block block;
	struct mTimingEvent other = {0};
	struct ARMCore configuration = {0}, actual = {0};
	configuration.memory.load16 = GBALoad16;
	unsigned checks = 0, accelerated = 0;
	for (unsigned i = 0; i < sizeof(savedata); ++i) savedata[i] = randomWord();
	for (unsigned op = 0; op < 4; ++op) {
		memset(&block, 0, sizeof(block));
		block.start = 0x08000100; block.length = 1;
		block.opcodes[0] = opcodes[op]; block.opcodes[1] = 0x2800; block.opcodes[2] = 0xD0FC;
		if (!_emitExecutionBlock(&block, &configuration)) { puts("FAIL EEPROM emission"); exit(1); }
		for (unsigned type = 0; type < 2; ++type) {
			for (unsigned command = 0; command < 5; ++command) {
				for (unsigned trial = 0; trial < 64; ++trial) {
					memset(&gba, 0, sizeof(gba));
					actual = configuration;
					for (unsigned r = 0; r < 16; ++r) actual.gprs[r] = randomWord();
					actual.master = &gba.d; gba.cpu = &actual;
					actual.executionMode = MODE_THUMB;
					actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
					actual.gprs[1] = 0x0D000000 + (trial & 3) + (trial & 4 ? 0x00FFFFFC : 0);
					actual.gprs[2] = 0;
					actual.gprs[ARM_PC] = block.start + 2;
					actual.prefetch[0] = opcodes[op]; actual.prefetch[1] = block.opcodes[1];
					actual.cycles = trial & 1 ? INT32_MIN : -9; actual.nextEvent = actual.cycles + 1;
					actual.memory.activeSeqCycles16 = trial & 7;
					actual.memory.activeNonseqCycles16 = (trial >> 3) & 7;
					gba.memory.waitstatesNonseq16[GBA_REGION_ROM2_EX] = (trial * 17) & 255;
					gba.memory.savedata.p = &gba; gba.memory.savedata.data = savedata;
					gba.memory.savedata.type = type ? GBA_SAVEDATA_EEPROM512 : GBA_SAVEDATA_EEPROM;
					gba.memory.savedata.command = command;
					gba.memory.savedata.readBitsRemaining = remaining[trial & 7];
					gba.memory.savedata.readAddress = trial & 15;
					gba.memory.prefetch = trial & 1; gba.memory.lastPrefetchedPc = block.start + trial;
					gba.memory.activeRegion = GBA_REGION_ROM0;
					other.next = trial & 8 ? &gba.memory.savedata.dust : NULL;
					switch (trial % 4) {
					case 0: break;
					case 1: gba.timing.root = &gba.memory.savedata.dust; break;
					case 2: gba.timing.root = &other; break;
					case 3: gba.timing.reroot = &other; break;
					}
					if (trial % 13 == 0) actual.memory.load16 = pollingRead;
					before = gba;
					struct ARMCore expected = actual;
					gba.cpu = &expected;
					expected.prefetch[0] = block.opcodes[1]; expected.prefetch[1] = block.opcodes[2];
					expected.gprs[ARM_PC] += 2;
					_thumbTable[opcodes[op] >> 6](&expected, opcodes[op]);
					expectedGBA = gba; expectedGBA.cpu = &actual;
					gba = before;
					bool complete = ((bool (*)(struct ARMCore*)) (uintptr_t) block.nativeCode)(&actual);
					if (!complete || memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
						printf("FAIL EEPROM op=%04x type=%u command=%u trial=%u\n", opcodes[op], type, command, trial);
						armCompare(&actual, &expected, opcodes[op], trial); exit(1);
					}
					++checks;
				}
			}
		}
	}
	/* Busy status is stable until an event. READ commands consume bits and
	 * must never take that shortcut, even when the stored data is all zero. */
	memset(savedata, 0, sizeof(savedata));
	for (unsigned variant = 0; variant < 4; ++variant) {
		for (unsigned trial = 0; trial < 64; ++trial) {
			for (unsigned i = 0; i < 256; ++i) code[i] = 0xE7FEE7FE;
			STORE_16(0x8808, 0x100, code); STORE_16(0x2800, 0x102, code); STORE_16(0xD0FC, 0x104, code);
			memset(&gba, 0, sizeof(gba)); memset(&actual, 0, sizeof(actual));
			actual.master = &gba.d; gba.cpu = &actual;
			uint32_t start = 0x08000100 + (trial % 3) * 0x02000000;
			actual.executionMode = MODE_THUMB; actual.cpsr.packed = ((trial & 15) << 28) | 0x3F;
			actual.gprs[1] = 0x0D000000;
			actual.gprs[ARM_PC] = start + 2; actual.prefetch[0] = 0x8808; actual.prefetch[1] = 0x2800;
			actual.memory.load16 = GBALoad16; actual.memory.setActiveRegion = GBASetActiveRegion;
			actual.memory.activeRegion = code; actual.memory.activeMask = sizeof(code) - 2;
			actual.memory.activeSeqCycles16 = trial & 3; actual.memory.activeNonseqCycles16 = (trial >> 2) & 3;
			actual.cycles = trial & 8 ? INT32_MIN : -13;
			actual.nextEvent = actual.cycles + (variant ? 1000 : 100000);
			actual.irqh.processEvents = eepromEvent;
			gba.memory.rom = code; gba.memory.romSize = sizeof(code); gba.memory.romMask = sizeof(code) - 1;
			gba.isPristine = !(trial & 32); gba.idleOptimization = IDLE_LOOP_IGNORE;
			gba.memory.activeRegion = start >> BASE_OFFSET;
			gba.memory.prefetch = trial & 1; gba.memory.lastPrefetchedPc = start + trial;
			gba.memory.waitstatesNonseq16[GBA_REGION_ROM2_EX] = trial & 7;
			gba.memory.savedata.p = &gba; gba.memory.savedata.data = savedata;
			gba.memory.savedata.type = trial & 1 ? GBA_SAVEDATA_EEPROM512 : GBA_SAVEDATA_EEPROM;
			gba.memory.savedata.command = variant == 1 ? EEPROM_COMMAND_READ : EEPROM_COMMAND_NULL;
			gba.memory.savedata.readBitsRemaining = 68;
			gba.timing.root = &gba.memory.savedata.dust;
			if (variant == 3) { gba.timing.root = NULL; gba.timing.reroot = &gba.memory.savedata.dust; }
			RV32Invalidate(&actual);
			struct RV32Context* context = _contextFor(&actual, &gba);
			struct RV32Block* cached = primeBlock(context, &actual, start);
			if (!cached || !cached->executable || !cached->pollLoop) { puts("FAIL EEPROM poll setup"); exit(1); }
			if (variant == 2) actual.memory.load16 = pollingRead;
			for (unsigned slice = 0; slice < 2; ++slice) {
				before = gba;
				struct ARMCore expected = actual;
				gba.cpu = &expected;
				uint32_t referenceInstructions = instructionsRetired();
				ARMRunLoopLegacy(&expected);
				referenceInstructions = instructionsRetired() - referenceInstructions;
				expectedGBA = gba; expectedGBA.cpu = &actual;
				gba = before;
				uint32_t nativeInstructions = instructionsRetired();
				ARMRunLoop(&actual);
				nativeInstructions = instructionsRetired() - nativeInstructions;
				if (memcmp(&actual, &expected, sizeof(actual)) || memcmp(&gba, &expectedGBA, sizeof(gba))) {
					printf("FAIL EEPROM poll variant=%u trial=%u slice=%u\n", variant, trial, slice);
					armCompare(&actual, &expected, 0, trial); exit(1);
				}
				if (!variant && !slice) {
					if (nativeInstructions >= referenceInstructions / 8) { puts("FAIL EEPROM busy loop not coalesced"); exit(1); }
					++accelerated;
				}
				++checks;
			}
		}
	}
	RV32Invalidate(&actual);
	printf("PASS: %u EEPROM load/event states, READ bit consumption, callbacks, odd/signed loads, event-list status; %u busy loops coalesced\n", checks, accelerated);
}

int main(void) {
#ifdef MGBA_RV32_TRACE_POLL
	checkTracePolling();
#ifdef RV32_TEST_TRACE_ONLY
	return 0;
#endif
#endif
	struct mLogger logger = {.log = quietLog};
	mLogSetDefaultLogger(&logger);
#ifdef RV32_TEST_DATA_ONLY
	checkStoreContinuation();
	sharedDataTest = true;
	checkRAMLoads();
	checkIOLoads();
	checkRAMStores();
	checkARMLoads();
	return 0;
#endif
#ifdef RV32_TEST_EEPROM_ONLY
	checkEEPROM();
	checkPollingLoops();
	return 0;
#endif
#ifdef RV32_TEST_EDGES_ONLY
	checkChaining();
	checkDirectEdges(false);
	checkDirectEdges(true);
	checkChainedSideEffects();
	checkWritableCode();
	return 0;
#endif
#ifdef RV32_TEST_WRITABLE_ONLY
	checkARMLoads();
	checkARMPollingLoops();
	checkChainedSideEffects();
	checkWritableCode();
	return 0;
#endif
#ifdef RV32_TEST_ARM_ONLY
	checkARMArithmetic();
	checkARMLoads();
	checkARMBranches();
	checkDirectEdges(true);
	checkARMRunner();
	checkARMPollingLoops();
	return 0;
#endif
#ifdef RV32_TEST_POLLING_ONLY
	checkPollingLoops();
	checkARMPollingLoops();
	checkEEPROM();
	return 0;
#endif
	struct ARMCore cpu = {0};
	cpu.memory.activeSeqCycles16 = 2;
	static struct RV32Block block;
	unsigned opcodes = 0, checks = 0, residentChecks = 0;
	for (unsigned opcode = 0; opcode <= 0xFFFF; ++opcode) {
		struct ARMInstructionInfo info;
		struct RV32NativeOp op;
		ARMDecodeThumb(opcode, &info);
		if (!_decodeNativeOp(&info, &op)) continue;
		memset(&block, 0, sizeof(block));
		block.length = 1;
		block.opcodes[0] = opcode;
		block.opcodes[1] = 0x1234;
		block.opcodes[2] = 0x5678;
		if (!_emitNativeBlock(&block, &cpu) || block.spans[0].length != 1) {
			printf("FAIL accepted opcode %04x could not be emitted\n", opcode);
			return 1;
		}
		for (unsigned trial = 0; trial < 32; ++trial) { checkNative(&block, 0, trial); ++checks; }
		block.start = 0x08000100;
		if (!_emitExecutionBlock(&block, &cpu)) { puts("FAIL resident arithmetic emission"); return 1; }
		for (unsigned trial = 0; trial < 32; ++trial) { checkNative(&block, 0, trial); ++residentChecks; }
		++opcodes;
	}
	printf("PASS: %u Thumb encodings, %u RV32/interpreter state comparisons and event guards\n", opcodes, checks);
	printf("PASS: %u resident-register arithmetic state comparisons\n", residentChecks);
	/* Random mixtures exercise flag dependencies and buffer-prefix rollback. */
	for (unsigned trial = 0; trial < 1024; ++trial) {
		memset(&block, 0, sizeof(block));
		block.length = 1 + trial % RV32_BLOCK_MAX_INSTRUCTIONS;
		for (unsigned i = 0; i < block.length; ++i) {
			struct ARMInstructionInfo info;
			struct RV32NativeOp op;
			do {
				block.opcodes[i] = randomWord();
				ARMDecodeThumb(block.opcodes[i], &info);
			} while (!_decodeNativeOp(&info, &op));
		}
		if (!_emitNativeBlock(&block, &cpu)) { puts("FAIL mixed block emission"); return 1; }
		for (unsigned i = 0; i < block.length; ++i) {
			if (block.spans[i].length) checkNative(&block, i, trial);
		}
	}
	puts("PASS: 1024 mixed native blocks");
	checkLoads();
	checkRAMLoads();
	checkIOLoads();
	checkRAMStores();
	checkLiteralLoads();
	checkBranches();
	checkStandardBranches();
	checkRunner();
	checkLongBlocks();
	checkChaining();
	checkDirectEdges(false);
	checkChainedSideEffects();
	checkPollingLoops();
	checkContexts();
	checkProfileLocations();
	checkARMArithmetic();
	checkARMLoads();
	checkARMBranches();
	checkDirectEdges(true);
	checkARMRunner();
	checkARMPollingLoops();
	checkWritableCode();
	checkEEPROM();
	sharedDataTest = true;
	checkRAMLoads();
	checkIOLoads();
	checkRAMStores();
	checkARMLoads();
	checkStoreContinuation();
	puts("PASS: shared native memory routines preserve the complete CPU/device state");
	return 0;
}
