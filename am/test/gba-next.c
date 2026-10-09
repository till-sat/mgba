/* SPDX-License-Identifier: MPL-2.0 */
/* mGBA is linked only into this oracle adapter, never the new core library. */
#include <gba-next/core.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/dma.h>
#include "gba-next/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "gba-next-events.h"

static struct Gbn actual;
static uint8_t ewram[GBN_EWRAM_SIZE], iwram[GBN_IWRAM_SIZE];
static struct ARMCore* reference;
static struct GBA* gba;
static uint32_t seed = 0x85459163;
static unsigned comparisons;
static uint8_t test_rom[8192], test_bios[GBN_BIOS_SIZE];
static struct GbnDevices devices;
static uint8_t palette[GBN_PALETTE_SIZE], vram[GBN_VRAM_SIZE], oam[GBN_OAM_SIZE];

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static uint32_t random_word(void) {
	seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
	return seed;
}

static void quiet(struct mLogger* logger, int category, enum mLogLevel level, const char* fmt, va_list args) {
	(void) logger; (void) category; (void) level; (void) fmt; (void) args;
}

static void no_events(struct ARMCore* cpu) { cpu->nextEvent = INT32_MAX; }
static void no_psr_event(struct ARMCore* cpu) { (void) cpu; }
static void undefined_instruction(struct ARMCore* cpu, uint32_t op) { (void) op; ARMRaiseUndefined(cpu); }

static void put16(uint32_t address, uint16_t value) {
	uint32_t ignored;
	CHECK(gbn_write(&actual, address, 2, value, &ignored) == GBN_STEP);
	GBAStore16(reference, address, (int16_t) value, NULL);
}

static void put32(uint32_t address, uint32_t value) {
	uint32_t ignored;
	CHECK(gbn_write(&actual, address, 4, value, &ignored) == GBN_STEP);
	GBAStore32(reference, address, (int32_t) value, NULL);
}

static void prepare(uint16_t op, unsigned trial, uint32_t pc) {
	static const uint32_t edge[] = {0, 1, 31, 32, 33, 255, 256, 0xffffffff,
		0x7fffffff, 0x80000000, 0x80000001, 0xffff, 0xffffff00, 0x55555555, 0xaaaaaaaa, 0xff000000};
	gbn_init(&actual, ewram, iwram);
	actual.ewram_wait = (uint8_t) (trial % 15 + 1);
	/* Set the equivalent supported EWRAM configuration in the oracle. */
	gba->memory.waitstatesSeq16[2] = gba->memory.waitstatesNonseq16[2] = (char) actual.ewram_wait;
	gba->memory.waitstatesSeq32[2] = gba->memory.waitstatesNonseq32[2] = (char) (2 * actual.ewram_wait + 1);
	for (unsigned i = 0; i < 15; ++i) actual.cpu.r[i] = trial < 16 ? edge[(trial + i) & 15] : random_word();
	actual.cpu.cpsr = ((trial & 15) << 28) | (random_word() & 0x0fffff00) | 0x3f;
	put16(pc, op);
	put16((pc & 0xff000000u) | ((pc + 2) & 0xffffff), 0x2707);
	put16((pc & 0xff000000u) | ((pc + 4) & 0xffffff), 0x2631);
}

static void enter_mode(uint32_t pc, bool thumb) {
	CHECK((thumb ? gbn_enter_thumb(&actual, pc) : gbn_enter_arm(&actual, pc)) == GBN_STEP);
	for (unsigned i = 0; i < 15; ++i) reference->gprs[i] = (int32_t) actual.cpu.r[i];
	reference->gprs[15] = (int32_t) pc;
	reference->cpsr.packed = (int32_t) actual.cpu.cpsr;
	reference->executionMode = thumb ? MODE_THUMB : MODE_ARM;
	reference->privilegeMode = (enum PrivilegeMode) (actual.cpu.cpsr & 31);
	reference->cycles = 0;
	reference->nextEvent = INT32_MAX;
	reference->halted = 0;
	reference->shifterCarryOut = (int32_t) actual.cpu.shifter_carry;
	reference->spsr.packed = (int32_t) actual.cpu.spsr;
	for (unsigned b = 0; b < 6; ++b) {
		reference->bankedRegisters[b][0] = (int32_t) actual.cpu.banks[b].sp;
		reference->bankedRegisters[b][1] = (int32_t) actual.cpu.banks[b].lr;
		reference->bankedSPSRs[b] = (int32_t) actual.cpu.banks[b].spsr;
	}
	for (unsigned b = 0; b < 2; ++b) for (unsigned r = 0; r < 5; ++r) {
		reference->bankedRegisters[b][r + 2] = (int32_t) actual.cpu.high_banks[b][r];
	}
	gba->memory.activeRegion = -1;
	if (thumb) ThumbWritePC(reference);
	else ARMWritePC(reference);
}

static void enter(uint32_t pc) { enter_mode(pc, true); }

static void prepare_arm(uint32_t op, unsigned trial, uint32_t pc) {
	prepare(0, trial, pc);
	put32(pc, op);
	put32(pc + 4, 0xe1a06006);
	put32(pc + 8, 0xe1a07007);
	actual.cpu.cpsr &= ~0x20u;
}

static void compare(uint32_t elapsed, unsigned op, unsigned trial) {
	unsigned width = actual.cpu.cpsr & 0x20 ? 2 : 4;
	bool same = actual.cpu.pc + width == (uint32_t) reference->gprs[15] &&
		actual.cpu.cpsr == (uint32_t) reference->cpsr.packed && elapsed == (uint32_t) reference->cycles &&
		((actual.cpu.cpsr >> 5) & 1) == (unsigned) reference->executionMode &&
		actual.cpu.shifter_carry == ((uint32_t) reference->shifterCarryOut & 1) &&
		actual.cpu.pipe[0] == reference->prefetch[0] && actual.cpu.pipe[1] == reference->prefetch[1];
	for (unsigned i = 0; i < 15; ++i) same &= actual.cpu.r[i] == (uint32_t) reference->gprs[i];
	unsigned active = (unsigned) ARMSelectBank(reference->privilegeMode);
	same &= actual.cpu.spsr == (uint32_t) reference->spsr.packed;
	same &= (actual.cpu.cpsr & 31) == (unsigned) reference->privilegeMode;
	/* Ignore inactive backing copies of the live bank: the architectures may
	 * cache them differently. Compare all architecturally reachable values. */
	for (unsigned b = 0; b < 6; ++b) if (b != active) {
		same &= actual.cpu.banks[b].sp == (uint32_t) reference->bankedRegisters[b][0];
		same &= actual.cpu.banks[b].lr == (uint32_t) reference->bankedRegisters[b][1];
		same &= actual.cpu.banks[b].spsr == (uint32_t) reference->bankedSPSRs[b];
	}
	for (unsigned r = 0; r < 5; ++r) {
		unsigned inactive = active == 1 ? 0 : 1;
		same &= actual.cpu.high_banks[inactive][r] == (uint32_t) reference->bankedRegisters[inactive][r + 2];
	}
	if (!same) {
		fprintf(stderr, "FAIL differential op=%04x trial=%u pc=%08" PRIx32 "/%08" PRIx32
			" cpsr=%08" PRIx32 "/%08" PRIx32 " cycles=%" PRIu32 "/%" PRIu32 "\n", op, trial,
			actual.cpu.pc, (uint32_t) reference->gprs[15] - width, actual.cpu.cpsr,
			(uint32_t) reference->cpsr.packed, elapsed, (uint32_t) reference->cycles);
		for (unsigned i = 0; i < 15; ++i) if (actual.cpu.r[i] != (uint32_t) reference->gprs[i]) {
			fprintf(stderr, "r%u=%08" PRIx32 "/%08" PRIx32 "\n", i, actual.cpu.r[i], (uint32_t) reference->gprs[i]);
		}
		exit(1);
	}
	++comparisons;
}

static void step(unsigned op, unsigned trial) {
	enum GbnStatus status = gbn_step(&actual);
	if (status != GBN_STEP) {
		fprintf(stderr, "FAIL unexpected stop: op=%08x trial=%u status=%u\n", op, trial, (unsigned) status);
		exit(1);
	}
	ARMRun(reference);
	compare(actual.now, op, trial);
}

static void check_batch(void) {
	static const uint16_t thumb_program[] = {0x3001, 0x6008, 0x680a, 0x2807, 0xd1fa, 0xe7fe};
	static const uint32_t arm_program[] = {0xe2800001, 0xe5810000, 0xe5912000, 0xe3500007, 0x1afffffa, 0xeafffffe};
	static const uint32_t caps[] = {1, 2, 7, 256};
	unsigned before = comparisons;
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned location = 0; location < 2; ++location) {
		uint32_t pc = location ? 0x02000100 : 0x03000100;
		for (unsigned distance = 0; distance < 80; ++distance) for (unsigned cap = 0; cap < 4; ++cap) {
			if (thumb) prepare(thumb_program[0], 1, pc);
			else prepare_arm(arm_program[0], 1, pc);
			for (unsigned i = 0; i < 6; ++i) {
				if (thumb) put16(pc + 2 * i, thumb_program[i]);
				else put32(pc + 4 * i, arm_program[i]);
			}
			actual.cpu.r[0] = 0; actual.cpu.r[1] = 0x02fffffd;
			enter_mode(pc, thumb != 0);
			uint32_t start = distance & 1 ? 0xfffffff0 : 0;
			actual.now = start;
			CHECK(gbn_schedule(&actual, 3, distance, 1));
			uint32_t expected = 0, executed = UINT32_MAX;
			while ((uint32_t) reference->cycles < distance && expected < caps[cap]) {
				ARMRun(reference);
				++expected;
			}
			enum GbnStatus status = gbn_run_batch(&actual, caps[cap], &executed);
			CHECK(executed == expected);
			CHECK(status == (expected == caps[cap] ? GBN_STEP : GBN_EVENT));
			compare(actual.now - start, thumb ? thumb_program[0] : arm_program[0], distance);
			CHECK(!memcmp(ewram + GBN_EWRAM_SIZE - 4, (uint8_t*) gba->memory.wram + GBN_EWRAM_SIZE - 4, 4));
			/* A due event is idempotent; consuming it permits exactly one more
			 * instruction. This also covers event/cap coincidence and wrap. */
			if ((int32_t) (actual.now - start - distance) >= 0) {
				struct Gbn snapshot = actual;
				CHECK(gbn_run_batch(&actual, 256, &executed) == GBN_EVENT && executed == 0);
				CHECK(!memcmp(&actual, &snapshot, sizeof(actual)));
				CHECK(gbn_take_event(&actual, NULL) == 3);
			}
			CHECK(gbn_run_batch(&actual, 1, &executed) == GBN_STEP && executed == 1);
			ARMRun(reference);
			compare(actual.now - start, thumb ? thumb_program[0] : arm_program[0], distance);
		}
	}
	/* A failed instruction is excluded from the count, without discarding the
	 * earlier successful instructions or repeatedly retrying the failure. */
	prepare(0x2001, 0, 0x03000100);
	put16(0x03000102, 0x3002); put16(0x03000104, 0xb100);
	enter(0x03000100);
	uint32_t executed = UINT32_MAX;
	CHECK(gbn_run_batch(&actual, 256, &executed) == GBN_UNSUPPORTED_INSTRUCTION);
	CHECK(executed == 2 && actual.cpu.r[0] == 3 && actual.cpu.pc == 0x03000104);
	struct Gbn snapshot = actual;
	CHECK(gbn_run_batch(&actual, 256, &executed) == GBN_UNSUPPORTED_INSTRUCTION && executed == 0);
	CHECK(!memcmp(&actual, &snapshot, sizeof(actual)));
	CHECK(gbn_run_batch(&actual, 0, &executed) == GBN_INVALID_ARGUMENT && executed == 0);
	CHECK(gbn_run_batch(&actual, 1, NULL) == GBN_INVALID_ARGUMENT);
	CHECK(!memcmp(&actual, &snapshot, sizeof(actual)));
	CHECK(gbn_attach_devices(&actual, &devices, palette, vram, oam));
	for (unsigned block = 0; block < 3; ++block) {
		devices.halted = block == 0; devices.stopped = block == 1; devices.dma_blocked = block == 2;
		snapshot = actual;
		CHECK(gbn_run_batch(&actual, 256, &executed) == GBN_EVENT && executed == 0);
		CHECK(!memcmp(&actual, &snapshot, sizeof(actual)));
	}
	printf("PASS batch execution: %u ARM/Thumb cap/event/wrap/resume comparisons; partial errors and HALT/STOP/DMA boundaries\n", comparisons - before);
}

static void check_alu(void) {
	unsigned before = comparisons;
	for (unsigned op = 0; op < 0x4400; ++op) {
		for (unsigned trial = 0; trial < 32; ++trial) {
			uint32_t pc = trial & 1 ? 0x02000100 : 0x03000102;
			prepare((uint16_t) op, trial, pc);
			enter(pc);
			step(op, trial);
		}
	}
	for (unsigned op = 0x4400; op < 0x4700; ++op) {
		unsigned rd = (op & 7) | (op >> 4 & 8);
		if (rd == 15 && (op >> 8 & 3) != 1) continue; /* Branches below. */
		for (unsigned trial = 0; trial < 32; ++trial) {
			prepare((uint16_t) op, trial, 0x03000100);
			enter(0x03000100);
			step(op, trial);
		}
	}
	for (unsigned op = 0xa000; op < 0xb100; ++op) {
		prepare((uint16_t) op, op & 31, 0x03000102);
		enter(0x03000102);
		step(op, op & 31);
	}
	printf("PASS ALU: %u state/pipeline/cycle comparisons\n", comparisons - before);
}

static void check_bus(void) {
	unsigned count = 0;
	static const uint32_t addresses[] = {0x02000000, 0x0203fffc, 0x02fffffc, 0x03000000, 0x03007ffc, 0x03fffffc};
	gbn_init(&actual, ewram, iwram);
	gba->memory.activeRegion = 3;
	for (unsigned wait = 1; wait <= 15; ++wait) {
		actual.ewram_wait = (uint8_t) wait;
		gba->memory.waitstatesNonseq16[2] = (char) wait;
		gba->memory.waitstatesNonseq32[2] = (char) (wait * 2 + 1);
		for (unsigned a = 0; a < sizeof(addresses) / sizeof(*addresses); ++a) {
			for (unsigned offset = 0; offset < 4; ++offset) for (unsigned width = 1; width <= 4; width *= 2) {
				uint32_t address = addresses[a] + offset;
				uint32_t value = random_word(), cycles;
				int expected_cycles = 0;
				CHECK(gbn_write(&actual, address, width, value, &cycles) == GBN_STEP);
				if (width == 1) GBAStore8(reference, address, (int8_t) value, &expected_cycles);
				else if (width == 2) GBAStore16(reference, address, (int16_t) value, &expected_cycles);
				else GBAStore32(reference, address, (int32_t) value, &expected_cycles);
				CHECK(cycles == (uint32_t) expected_cycles);
				struct GbnAccess access;
				CHECK(gbn_read(&actual, address, width, &access) == GBN_STEP);
				expected_cycles = 0;
				uint32_t expected = width == 1 ? GBALoad8(reference, address, &expected_cycles) :
					width == 2 ? GBALoad16(reference, address, &expected_cycles) : GBALoad32(reference, address, &expected_cycles);
				CHECK(access.value == expected && access.cycles + 1 == (uint32_t) expected_cycles);
				++count;
			}
		}
	}
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(!memcmp(iwram, gba->memory.iwram, sizeof(iwram)));
	printf("PASS bus: %u load/store value, mirror, alignment and timing comparisons\n", count);
}

static void check_memory(void) {
	unsigned before = comparisons;
	static const uint32_t addresses[] = {0x02004000, 0x0203ffff, 0x02ffffff, 0x03004000,
		0x03007fff, 0x03ffffff, 0x02003ffd, 0x03003ffe};
	for (unsigned op = 0x4800; op < 0xa000; ++op) {
		for (unsigned trial = 0; trial < 8; ++trial) {
			uint32_t pc = trial & 1 ? 0x02001000 : 0x03001002;
			prepare((uint16_t) op, trial, pc);
			uint32_t address = addresses[trial];
			if (op >= 0x5000 && op < 0x6000) {
				unsigned base = op >> 3 & 7, offset = op >> 6 & 7;
				if (base == offset) { address &= ~1u; actual.cpu.r[base] = address >> 1; }
				else { actual.cpu.r[base] = address - trial; actual.cpu.r[offset] = trial; }
			} else if (op >= 0x6000 && op < 0x9000) {
				unsigned scale = op >= 0x8000 ? 2 : op & 0x1000 ? 1 : 4;
				actual.cpu.r[op >> 3 & 7] = address - (op >> 6 & 31) * scale;
			} else if (op >= 0x9000) actual.cpu.r[13] = address - (op & 255) * 4;
			else address = ((pc + 4) & ~3u) + (op & 255) * 4;
			enter(pc);
			step(op, trial);
			/* Compare the touched word including bytes outside narrow stores. */
			struct GbnAccess access;
			CHECK(gbn_read(&actual, address & ~3u, 4, &access) == GBN_STEP);
			CHECK(access.value == GBALoad32(reference, address & ~3u, NULL));
		}
	}
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(!memcmp(iwram, gba->memory.iwram, sizeof(iwram)));
	printf("PASS memory instructions: %u state/pipeline/cycle comparisons\n", comparisons - before);
}

static void check_branches(void) {
	unsigned before = comparisons;
	for (unsigned op = 0xd000; op < 0xde00; ++op) {
		for (unsigned flags = 0; flags < 16; ++flags) {
			uint32_t pc = flags & 1 ? 0x02001000 : 0x03001002;
			prepare((uint16_t) op, flags, pc);
			enter(pc);
			step(op, flags);
		}
	}
	for (unsigned op = 0xe000; op < 0x10000; ++op) {
		if (op >= 0xe800 && op < 0xf000) continue;
		prepare((uint16_t) op, op & 31, 0x03002000);
		actual.cpu.r[14] = 0x02004000;
		enter(0x03002000);
		step(op, op & 31);
	}
	/* High-register writes to PC and BX, including cross-RAM-region branches. */
	static const uint16_t branches[] = {0x4487, 0x4687, 0x4700, 0x46ff};
	for (unsigned i = 0; i < sizeof(branches) / sizeof(*branches); ++i) {
		prepare(branches[i], i, 0x03001000);
		actual.cpu.r[0] = i ? 0x02002001 : 0xff000ffd;
		enter(0x03001000);
		step(branches[i], i);
	}
	printf("PASS branches: %u conditions, refills and link comparisons\n", comparisons - before);
}

static void check_events(void) {
	/* RAM loop: add, store, load, compare, conditional branch. */
	static const uint16_t program[] = {0x3001, 0x6008, 0x680a, 0x2807, 0xd1fa, 0xe7fe};
	unsigned count = 0;
	for (unsigned location = 0; location < 2; ++location) {
		uint32_t pc = location ? 0x02000100 : 0x03000100;
		for (unsigned distance = 0; distance < 160; ++distance) {
			prepare(program[0], 1, pc);
			for (unsigned i = 0; i < sizeof(program) / sizeof(*program); ++i) put16(pc + 2 * i, program[i]);
			actual.cpu.r[0] = 0; actual.cpu.r[1] = 0x02fffffd;
			enter(pc);
			uint32_t start = distance & 1 ? 0xfffffff0 : 0;
			actual.now = start;
			CHECK(gbn_schedule(&actual, 3, distance, 1));
			CHECK(gbn_run_for(&actual, 1000) == GBN_EVENT);
			while ((uint32_t) reference->cycles < distance) ARMRun(reference);
			compare(actual.now - start, program[0], distance);
			struct Gbn snapshot = actual;
			CHECK(gbn_step(&actual) == GBN_EVENT && !memcmp(&actual, &snapshot, sizeof(actual)));
			uint32_t late;
			CHECK(gbn_take_event(&actual, &late) == 3);
			CHECK(late == actual.now - start - distance);
			/* Resuming after event acknowledgement preserves execution state. */
			CHECK(gbn_step(&actual) == GBN_STEP);
			ARMRun(reference);
			compare(actual.now - start, program[0], distance);
			++count;
		}
	}
	/* A host execution budget is also an instruction-boundary deadline, even
	 * when no emulated device event is scheduled. */
	for (unsigned budget = 0; budget < 80; ++budget) {
		prepare(0x6808, 1, 0x03000100);
		put16(0x03000102, 0xe7fd); /* load + branch back */
		actual.cpu.r[1] = 0x02000101;
		enter(0x03000100);
		uint32_t start = 0xfffffff0;
		actual.now = start;
		CHECK(gbn_run_for(&actual, budget) == GBN_DEADLINE);
		while ((uint32_t) reference->cycles < budget) ARMRun(reference);
		compare(actual.now - start, 0x6808, budget);
	}
	gbn_init(&actual, ewram, iwram);
	actual.now = 0xfffffffe;
	CHECK(gbn_schedule(&actual, 4, 2, 1));
	CHECK(gbn_schedule(&actual, 3, 2, 1));
	CHECK(gbn_schedule(&actual, 7, 2, 0));
	CHECK(gbn_schedule(&actual, 2, 1, 0));
	gbn_cancel(&actual, 2);
	/* Advance via valid instructions across timer wrap. */
	put16(0x03000000, 0x2001); put16(0x03000002, 0x2002);
	CHECK(gbn_enter_thumb(&actual, 0x03000000) == GBN_STEP);
	CHECK(gbn_run_for(&actual, 2) == GBN_EVENT);
	CHECK(actual.now == 0);
	CHECK(gbn_take_event(&actual, NULL) == 7);
	CHECK(gbn_take_event(&actual, NULL) == 3);
	CHECK(gbn_take_event(&actual, NULL) == 4);
	CHECK(gbn_take_event(&actual, NULL) == -1);
	CHECK(!gbn_schedule(&actual, GBN_EVENT_COUNT, 0, 0));
	CHECK(!gbn_schedule(&actual, 0, GBN_MAX_DELAY + 1, 0));
	CHECK(gbn_schedule(&actual, 5, 9, 0));
	CHECK(gbn_schedule(&actual, 5, 0, 0));
	CHECK(gbn_run_for(&actual, 0) == GBN_EVENT);
	CHECK(gbn_take_event(&actual, NULL) == 5);
	CHECK(gbn_run_for(&actual, 0) == GBN_DEADLINE);
	CHECK(gbn_run_for(&actual, GBN_MAX_DELAY + 1) == GBN_INVALID_ARGUMENT);
	printf("PASS events: %u event + 80 budget cases; overshoot, resume, wrap, order, cancel, replace\n", count);
}

static void check_streams(void) {
	unsigned before = comparisons;
	for (unsigned program = 0; program < 128; ++program) {
		uint32_t pc = program & 1 ? 0x03003000 : 0x02003000;
		prepare(0, program, pc);
		uint16_t instructions[64];
		for (unsigned i = 0; i < 64; ++i) {
			instructions[i] = (uint16_t) (random_word() % 0x4400);
			put16(pc + i * 2, instructions[i]);
		}
		enter(pc);
		for (unsigned i = 0; i < 64; ++i) step(instructions[i], program);
	}
	printf("PASS instruction streams: %u dependent ALU/flag/shift comparisons\n", comparisons - before);
}

static unsigned popcount(uint32_t value) {
	unsigned count = 0;
	for (unsigned bit = 0; bit < 16; ++bit) count += (value >> bit) & 1;
	return count;
}

static void fill_multiple(uint32_t base, uint32_t list, bool up, bool before, bool thumb) {
	unsigned count = popcount(list);
	uint32_t address = up ? base + (before ? 4 : 0) : base - count * 4 + (before ? 0 : 4);
	uint32_t region = address & 0xff000000u;
	uint32_t transfers = list ? list : 0x8000;
	for (unsigned r = 0; r < 16; ++r) {
		if (!(transfers & (1u << r))) continue;
		uint32_t value = r == 15 ? (thumb ? 0x02006003 : 0x03006002) : random_word();
		put32(region | (address & 0xffffff), value);
		address += 4;
	}
}

static void check_thumb_multiple(void) {
	unsigned before = comparisons;
	static const uint32_t addresses[] = {0x02003000, 0x0203fffd, 0x02ffffe2, 0x03003000,
		0x03007ffc, 0x03ffffe3, 0x02010001, 0x03002002};
	for (unsigned op = 0xb400; op < 0xd000; ++op) {
		bool stack = (op & 0xf600) == 0xb400;
		if (!stack && (op & 0xf000) != 0xc000) continue;
		bool load = (op & 0x800) != 0;
		unsigned rn = stack ? 13 : op >> 8 & 7;
		uint32_t list = op & 255;
		if (stack && (op & 0x100)) list |= 1u << (load ? 15 : 14);
		bool up = !stack || load;
		for (unsigned trial = 0; trial < 8; ++trial) {
			prepare((uint16_t) op, trial, 0x03001000);
			actual.cpu.r[rn] = addresses[trial];
			fill_multiple(addresses[trial], list, up, !up, true);
			enter(0x03001000);
			step(op, trial);
		}
	}
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(!memcmp(iwram, gba->memory.iwram, sizeof(iwram)));
	printf("PASS Thumb multiple: %u stack/list/alias/mirror/cycle comparisons\n", comparisons - before);
}

static void check_arm_alu(void) {
	unsigned before = comparisons;
	for (unsigned kind = 0; kind < 16; ++kind) {
		for (unsigned form = 0; form < 9; ++form) {
			for (unsigned trial = 0; trial < (form ? 256u : 4096u); ++trial) {
				unsigned rn = trial >> 4 & 15, rd = trial % 15;
				uint32_t op = 0xe0000000u | kind << 21 | rn << 16 | rd << 12;
				if ((trial & 1) || (kind >= 8 && kind <= 11)) op |= 1u << 20;
				if (!form) op |= 0x2000000 | trial;
				else {
					op |= (form - 1) % 4 << 5 | (trial & 15);
					if (form <= 4) op |= (trial >> 3 & 31) << 7;
					else op |= 16 | (trial >> 1 & 15) << 8;
				}
				uint32_t pc = trial & 1 ? 0x02001000 : 0x03001002;
				prepare_arm(op, trial & 31, pc);
				enter_mode(pc, false);
				step(op, trial);
			}
		}
	}
	/* Every condition, including NV, with every NZCV combination. */
	for (unsigned cond = 0; cond < 16; ++cond) for (unsigned flags = 0; flags < 16; ++flags) {
		uint32_t op = cond << 28 | 0x02910001;
		prepare_arm(op, flags, 0x03001000);
		enter_mode(0x03001000, false);
		step(op, flags);
	}
	/* Writes to PC exercise the ARM refill with both bit-1 alignments. */
	for (unsigned s = 0; s < 2; ++s) for (unsigned low = 0; low < 4; ++low) {
		uint32_t op = 0xe1a0f000 | s << 20;
		prepare_arm(op, low, 0x03001000);
		actual.cpu.r[0] = 0x02003000 | low;
		enter_mode(0x03001000, false);
		step(op, low);
	}
	printf("PASS ARM ALU: %u operands, shifts, conditions, flags and PC comparisons\n", comparisons - before);
}

static void check_arm_multiple(void) {
	unsigned before = comparisons;
	static const uint16_t lists[] = {0, 1, 2, 0xff, 0x8000, 0x8001, 0xaaaa, 0x5555, 0xffff, 0x6000, 0x1040};
	static const uint32_t addresses[] = {0x02004000, 0x0203ffe1, 0x02ffffe2, 0x03003000, 0x03007fe3, 0x03ffffe0};
	for (unsigned mode = 0; mode < 16; ++mode) for (unsigned rn = 0; rn < 15; ++rn) {
		bool before_address = (mode & 8) != 0, up = (mode & 4) != 0, wb = (mode & 2) != 0, load = (mode & 1) != 0;
		for (unsigned l = 0; l < sizeof(lists) / sizeof(*lists); ++l) for (unsigned a = 0; a < 6; ++a) {
			uint32_t op = 0xe8000000 | before_address << 24 | up << 23 | wb << 21 | load << 20 | rn << 16 | lists[l];
			prepare_arm(op, a, 0x03001000);
			actual.cpu.r[rn] = addresses[a];
			fill_multiple(addresses[a], lists[l], up, before_address, false);
			enter_mode(0x03001000, false);
			step(op, mode);
		}
	}
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(!memcmp(iwram, gba->memory.iwram, sizeof(iwram)));
	printf("PASS ARM multiple: %u directions, lists, writebacks and alias comparisons\n", comparisons - before);
}

static void check_arm_multiply(void) {
	unsigned before = comparisons;
	for (unsigned kind = 0; kind < 6; ++kind) for (unsigned s = 0; s < 2; ++s) {
		for (unsigned trial = 0; trial < 512; ++trial) {
			unsigned rm = trial & 15, rs = trial >> 4 & 15;
			uint32_t op = 0xe0000090 | s << 20 | 2 << 16 | 3 << 12 | rs << 8 | rm;
			if (kind < 2) op |= kind << 21;
			else op |= 0x800000 | ((kind - 2) & 1) << 21 | ((kind - 2) >> 1) << 22;
			prepare_arm(op, trial & 31, trial & 1 ? 0x02001000 : 0x03001000);
			actual.cpu.shifter_carry = trial & 1;
			enter_mode(trial & 1 ? 0x02001000 : 0x03001000, false);
			step(op, trial);
		}
	}
	printf("PASS ARM multiply: %u signed/unsigned/accumulate/flag/cycle comparisons\n", comparisons - before);
}

static uint32_t address_offset(uint32_t value, unsigned kind, unsigned amount, uint32_t flags) {
	if (!kind) return value << amount;
	if (kind == 1) return amount ? value >> amount : 0;
	if (kind == 2) return (uint32_t) ((int32_t) value >> (amount ? amount : 31));
	if (!amount) return (flags & 0x20000000 ? 0x80000000 : 0) | (value >> 1);
	return value >> amount | value << (32 - amount);
}

static void check_arm_memory(void) {
	unsigned before = comparisons;
	static const uint32_t addresses[] = {0x02004000, 0x0203ffff, 0x02ffffff, 0x03004000,
		0x03007fff, 0x03ffffff, 0x02003ffd, 0x03003ffe};
	/* Word/byte immediate, four shifted register forms each, and immediate /
	 * register halfword and signed byte/halfword forms. */
	for (unsigned form = 0; form < 16; ++form) for (unsigned mode = 0; mode < 16; ++mode) {
		bool pre = (mode & 8) != 0, up = (mode & 4) != 0, wb = (mode & 2) != 0, load = (mode & 1) != 0;
		bool half = form >= 10, reg_offset = (form >= 2 && form < 10) || form >= 13;
		unsigned half_kind = half ? (form - 10) % 3 + 1 : 0;
		if (half_kind >= 2 && !load) continue;
		unsigned width = half ? (half_kind == 2 ? 1 : 2) : (form == 1 || (form >= 6 && form < 10) ? 1 : 4);
		for (unsigned trial = 0; trial < 32; ++trial) {
			unsigned rn = trial % 15, rm = (rn + 1) % 15;
			unsigned rd = trial & 2 ? rn : trial & 1 ? rm : 0;
			if (!(trial & 7) && (!load || width == 4)) rd = 15;
			unsigned shift_kind = form >= 2 && form < 10 ? (form - 2) % 4 : 0;
			unsigned amount = trial;
			uint32_t op = 0xe0000000 | pre << 24 | up << 23 | wb << 21 | load << 20 | rn << 16 | rd << 12;
			if (half) {
				op |= 0x90 | half_kind << 5;
				if (reg_offset) op |= rm;
				else op |= 0x400000 | 0x300 | 5;
			} else {
				op |= 0x4000000 | (width == 1 ? 0x400000 : 0);
				if (reg_offset) op |= 0x2000000 | amount << 7 | shift_kind << 5 | rm;
				else op |= 0x135;
			}
			uint32_t pc = trial & 1 ? 0x02001000 : 0x03001000;
			prepare_arm(op, trial, pc);
			uint32_t offset = half ? 0x35 : 0x135;
			if (reg_offset) {
				uint32_t input = trial & 1 ? 0xfedc00ff : trial;
				actual.cpu.r[rm] = input;
				offset = half ? input : address_offset(input, shift_kind, amount, actual.cpu.cpsr);
			}
			uint32_t address = addresses[trial & 7];
			actual.cpu.r[rn] = pre ? address - (up ? offset : 0u - offset) : address;
			uint32_t data = random_word();
			if (rd == 15 && load) {
				unsigned rotation = (address & 3) * 8;
				uint32_t target = 0x03006002;
				data = rotation ? target << rotation | target >> (32 - rotation) : target;
			}
			put32(address, data);
			enter_mode(pc, false);
			step(op, trial);
			struct GbnAccess access;
			CHECK(gbn_read(&actual, address & ~3u, 4, &access) == GBN_STEP);
			CHECK(access.value == GBALoad32(reference, address & ~3u, NULL));
		}
	}
	for (unsigned byte = 0; byte < 2; ++byte) for (unsigned trial = 0; trial < 64; ++trial) {
		unsigned rd = trial % 3, rm = (trial / 3) % 3;
		uint32_t op = 0xe1010090 | byte << 22 | rd << 12 | rm;
		prepare_arm(op, trial & 31, 0x03001000);
		actual.cpu.r[1] = addresses[trial & 7];
		enter_mode(0x03001000, false);
		step(op, trial);
	}
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(!memcmp(iwram, gba->memory.iwram, sizeof(iwram)));
	printf("PASS ARM memory: %u addressing, width, sign, alias, writeback and SWP comparisons\n", comparisons - before);
}

static void check_interworking(void) {
	unsigned before = comparisons;
	for (unsigned location = 0; location < 2; ++location) {
		uint32_t pc = location ? 0x02001000 : 0x03001000;
		prepare_arm(0xe12fff10, location, pc); /* BX r0 -> Thumb */
		actual.cpu.r[0] = 0x03002001;
		actual.cpu.r[1] = 0x02003002;
		put16(0x03002000, 0x227f);
		put16(0x03002002, 0x4708); /* BX r1 -> ARM, bit 1 retained */
		put32(0x02003000, 0xe0823002);
		put32(0x02003004, 0xe12fff1e); /* BX lr -> Thumb */
		actual.cpu.r[14] = 0x03002005;
		put16(0x03002004, 0xe7fe);
		enter_mode(pc, false);
		for (unsigned i = 0; i < 8; ++i) step(0xe12fff10, i);
	}
	for (unsigned link = 0; link < 2; ++link) for (unsigned trial = 0; trial < 64; ++trial) {
		uint32_t offset = trial & 1 ? trial * 4 : 0x1000000 - trial * 4;
		uint32_t op = 0xea000000 | link << 24 | (offset & 0xffffff);
		prepare_arm(op, trial & 31, 0x03001000);
		enter_mode(0x03001000, false);
		step(op, trial);
	}
	printf("PASS interworking/branches: %u ARM/Thumb state, pipeline, link and cycle comparisons\n", comparisons - before);
}

static void seed_banks(unsigned mode, uint32_t saved) {
	actual.cpu.cpsr = (actual.cpu.cpsr & ~31u) | mode;
	actual.cpu.spsr = saved;
	for (unsigned b = 0; b < 6; ++b) {
		actual.cpu.banks[b].sp = random_word();
		actual.cpu.banks[b].lr = random_word();
		actual.cpu.banks[b].spsr = random_word();
	}
	for (unsigned b = 0; b < 2; ++b) for (unsigned r = 0; r < 5; ++r) actual.cpu.high_banks[b][r] = random_word();
}

static void check_psr(void) {
	static const unsigned modes[] = {16, 17, 18, 19, 23, 27, 31};
	unsigned before = comparisons;
	for (unsigned source = 0; source < 7; ++source) for (unsigned dest = 0; dest < 7; ++dest) {
		for (unsigned fields = 0; fields < 16; ++fields) for (unsigned variant = 0; variant < 8; ++variant) {
			bool saved = (variant & 1) != 0, immediate = (variant & 2) != 0, thumb = (variant & 4) != 0;
			uint32_t op = 0xe120f000 | saved << 22 | fields << 16;
			uint32_t operand = modes[dest] | (thumb ? 0x20 : 0) | (dest & 1 ? 0xc0 : 0);
			if (immediate) op |= 0x2000000 | operand;
			prepare_arm(op, fields, 0x03001000);
			seed_banks(modes[source], random_word());
			actual.cpu.r[0] = operand | (random_word() & 0xffffff00);
			enter_mode(0x03001000, false);
			step(op, variant);
		}
		for (unsigned rd = 0; rd < 15; ++rd) for (unsigned saved = 0; saved < 2; ++saved) {
			uint32_t op = 0xe10f0000 | saved << 22 | rd << 12;
			prepare_arm(op, rd, 0x03001000);
			seed_banks(modes[source], modes[dest] | 0xa0000000);
			enter_mode(0x03001000, false);
			step(op, rd);
		}
	}
	for (unsigned immediate = 0; immediate < 4096; ++immediate) {
		uint32_t op = 0xe328f000 | immediate; /* MSR CPSR_f, rotated immediate */
		prepare_arm(op, immediate & 31, 0x03001000);
		seed_banks(modes[immediate % 7], random_word());
		enter_mode(0x03001000, false);
		step(op, immediate);
	}
	for (unsigned source = 1; source < 6; ++source) for (unsigned dest = 0; dest < 7; ++dest) {
		for (unsigned thumb = 0; thumb < 2; ++thumb) {
			prepare_arm(0xe1b0f000, source, 0x03001000); /* MOVS pc, r0 */
			seed_banks(modes[source], (random_word() & 0xffffffc0) | modes[dest] | thumb << 5);
			actual.cpu.r[0] = 0x02004003;
			enter_mode(0x03001000, false);
			step(0xe1b0f000, thumb);
		}
	}
	/* S-bit block transfers target user banks, or restore SPSR when loading PC. */
	static const uint16_t lists[] = {0, 0xffff, 0x6000, 0x1f00, 0x8000, 0xaaaa, 0x5555};
	static const unsigned bases[] = {0, 8, 13, 14};
	for (unsigned source = 0; source < 7; ++source) for (unsigned mode = 0; mode < 16; ++mode) {
		bool before_address = (mode & 8) != 0, up = (mode & 4) != 0, wb = (mode & 2) != 0, load = (mode & 1) != 0;
		for (unsigned l = 0; l < 7; ++l) for (unsigned b = 0; b < 4; ++b) {
			unsigned rn = bases[b];
			uint32_t op = 0xe8400000 | before_address << 24 | up << 23 | wb << 21 | load << 20 | rn << 16 | lists[l];
			prepare_arm(op, mode, 0x03001000);
			seed_banks(modes[source], 0xa000001f | (mode & 2 ? 0x20 : 0));
			actual.cpu.r[rn] = 0x02002003;
			fill_multiple(0x02002003, lists[l], up, before_address, false);
			enter_mode(0x03001000, false);
			step(op, mode);
		}
	}
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(!memcmp(iwram, gba->memory.iwram, sizeof(iwram)));
	printf("PASS PSR/banks: %u status, privilege, FIQ, user-transfer and exception-return comparisons\n", comparisons - before);
}

static void check_self_modify(void) {
	/* Store over the very next instruction or the fetch performed by STRH.
	 * Both old opcodes must execute, until a branch refills the pipeline. */
	for (unsigned offset = 2; offset <= 4; offset += 2) {
		prepare(0x8008, offset, 0x03001000);
		actual.cpu.r[0] = 0x2777;
		actual.cpu.r[1] = 0x03001000 + offset;
		put16(0x03001006, 0xe7fb); /* Refill from the modified instruction. */
		enter(0x03001000);
		for (unsigned i = 0; i < 8; ++i) step(0x8008, i);
	}
	/* Sequential fetch at the end of a RAM mirror retains the active mapping. */
	prepare(0x2001, 0, 0x03fffffe);
	enter(0x03fffffe);
	for (unsigned i = 0; i < 3; ++i) step(0x2001, i);
	/* An ARM store preserves the pending prefetched opcode. A subsequent
	 * MSR explicitly refreshes that pipeline in the reference implementation. */
	for (unsigned refresh = 0; refresh < 2; ++refresh) {
		prepare_arm(0xe5810000, refresh, 0x03001000);
		actual.cpu.r[0] = 0xe3a07077;
		actual.cpu.r[1] = 0x03001008;
		actual.cpu.r[2] = 0;
		put32(0x03001004, refresh ? 0xe128f002 : 0xe1a06006);
		put32(0x03001008, 0xe3a07011);
		put32(0x0300100c, 0xe1a06006);
		put32(0x03001010, 0xe1a06006);
		enter_mode(0x03001000, false);
		for (unsigned i = 0; i < 3; ++i) step(0xe5810000, i);
		CHECK(actual.cpu.r[7] == (refresh ? 0x77u : 0x11u));
	}
	puts("PASS self-modifying code and sequential RAM mirror fetch");
}

static void check_refusals(void) {
	static const struct { uint16_t op; uint32_t r0; enum GbnStatus result; } cases[] = {
		{0xdf00, 0, GBN_UNSUPPORTED_INSTRUCTION}, {0xb600, 0, GBN_UNSUPPORTED_INSTRUCTION},
		{0x4780, 0x03000001, GBN_UNSUPPORTED_INSTRUCTION},
		{0x4700, 0x08000001, GBN_UNSUPPORTED_ADDRESS}, {0x4687, 0x08000000, GBN_UNSUPPORTED_ADDRESS},
		{0x6801, 0x04000000, GBN_UNSUPPORTED_ADDRESS}, {0x6001, 0x04000000, GBN_UNSUPPORTED_ADDRESS}
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
		prepare(cases[i].op, i, 0x03000100);
		actual.cpu.r[0] = cases[i].r0;
		enter(0x03000100);
		struct Gbn before = actual;
		CHECK(gbn_step(&actual) == cases[i].result);
		CHECK(!memcmp(&actual, &before, sizeof(actual)));
	}
	struct Gbn before = actual;
	struct GbnAccess access;
	uint32_t ignored;
	CHECK(gbn_enter_thumb(&actual, 0x08000000) == GBN_UNSUPPORTED_ADDRESS);
	CHECK(gbn_read(&actual, 0x03000000, 3, &access) == GBN_INVALID_ARGUMENT);
	CHECK(gbn_write(&actual, 0x03000000, 3, 0, &ignored) == GBN_INVALID_ARGUMENT);
	CHECK(!memcmp(&actual, &before, sizeof(actual)));
	static const struct { uint32_t op, r0, psr; enum GbnStatus result; } arm_cases[] = {
		{0xef000000, 0, 0x1f, GBN_UNSUPPORTED_INSTRUCTION},
		{0xee110f10, 0, 0x1f, GBN_UNSUPPORTED_INSTRUCTION},
		{0xe121f000, 0x14, 0x1f, GBN_UNSUPPORTED_MODE},
		{0xe1b0f000, 0x03004000, 0x14, GBN_UNSUPPORTED_MODE},
		{0xe1b0f000, 0x04000000, 0x3f, GBN_UNSUPPORTED_ADDRESS},
		{0xe8900003, 0x04000000, 0x1f, GBN_UNSUPPORTED_ADDRESS},
		{0xe8800003, 0x04000000, 0x1f, GBN_UNSUPPORTED_ADDRESS},
		{0xe5901000, 0x04000000, 0x1f, GBN_UNSUPPORTED_ADDRESS},
		{0xe5801000, 0x04000000, 0x1f, GBN_UNSUPPORTED_ADDRESS},
		{0xe1001092, 0x04000000, 0x1f, GBN_UNSUPPORTED_ADDRESS}
	};
	for (unsigned i = 0; i < sizeof(arm_cases) / sizeof(*arm_cases); ++i) {
		prepare_arm(arm_cases[i].op, i, 0x03001000);
		seed_banks(19, arm_cases[i].psr);
		actual.cpu.r[0] = arm_cases[i].r0;
		enter_mode(0x03001000, false);
		before = actual;
		CHECK(gbn_step(&actual) == arm_cases[i].result);
		CHECK(!memcmp(&actual, &before, sizeof(actual)));
	}
	puts("PASS unsupported instructions, regions and modes leave state unchanged");
}

static void image_put(uint8_t* image, uint32_t offset, uint32_t value, unsigned width) {
	for (unsigned i = 0; i < width; ++i) image[offset + i] = (uint8_t) (value >> (i * 8));
}

static void bind_images(uint16_t waitcnt) {
	CHECK(gbn_attach_rom(&actual, test_rom, sizeof(test_rom)));
	CHECK(gbn_attach_bios(&actual, test_bios, sizeof(test_bios)));
	gbn_set_waitcnt(&actual, waitcnt);
	gba->memory.rom = (uint32_t*) test_rom;
	gba->memory.romSize = sizeof(test_rom);
	gba->memory.romMask = sizeof(test_rom) - 1;
	gba->memory.bios = (uint32_t*) test_bios;
	gba->memory.fullBios = 1;
	gba->memory.activeRegion = 3;
	GBAAdjustWaitstates(gba, waitcnt);
}

static void compare_bus_state(void) {
	CHECK(actual.prefetched_pc == gba->memory.lastPrefetchedPc);
	CHECK(actual.bios_latch == gba->memory.biosPrefetch);
}

static void check_rom_bios(void) {
	unsigned before = comparisons, reads = 0;
	uint32_t* old_bios = gba->memory.bios;
	void (*old_illegal)(struct ARMCore*, uint32_t) = reference->irqh.hitIllegal;
	for (unsigned i = 0; i < sizeof(test_rom); ++i) test_rom[i] = (uint8_t) random_word();
	for (unsigned i = 0; i < sizeof(test_bios); ++i) test_bios[i] = (uint8_t) random_word();
	prepare(0x2001, 0, 0x03000100);
	bind_images(0);
	enter(0x03000100);
	/* All three waitstate selectors and both sequence settings, at every bus
	 * width, including the unpopulated upper half of the six ROM regions. */
	for (unsigned configuration = 0; configuration < 2048; ++configuration) {
		gbn_set_waitcnt(&actual, (uint16_t) configuration);
		GBAAdjustWaitstates(gba, (uint16_t) configuration);
		for (unsigned region = 8; region <= 13; ++region) for (unsigned width = 1; width <= 4; width *= 2) {
			uint32_t address = region << 24 | (configuration & 8191);
			struct GbnAccess access;
			CHECK(gbn_read(&actual, address, width, &access) == GBN_STEP);
			int cycles = 0;
			uint32_t value = width == 1 ? GBALoad8(reference, address, &cycles) : width == 2 ?
				GBALoad16(reference, address, &cycles) : GBALoad32(reference, address, &cycles);
			CHECK(access.value == value && access.cycles + 1 == (uint32_t) cycles);
			++reads;
		}
	}
	static const uint16_t thumb_ops[] = {0x6808, 0x7808, 0x8808, 0x6008, 0x7008, 0x8008,
		0x4348, 0xc105, 0xc905, 0x4800, 0x4708, 0xe000, 0x2001};
	static const uint32_t arm_ops[] = {0xe5910000, 0xe5d10000, 0xe1d100b0, 0xe5810000,
		0xe5c10000, 0xe1c100b0, 0xe0000293, 0xe0012394, 0xe8910005, 0xe8810005,
		0xe59f0000, 0xe12fff11, 0xea000000, 0xe3a00001};
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned window = 0; window < 3; ++window) {
		for (unsigned config = 0; config < 128; ++config) {
			uint16_t waitcnt = (uint16_t) ((config * 0x53u & 0x7ff) | (config & 1 ? 0x4000 : 0));
			unsigned count = thumb ? sizeof(thumb_ops) / sizeof(*thumb_ops) : sizeof(arm_ops) / sizeof(*arm_ops);
			for (unsigned index = 0; index < count; ++index) {
				uint32_t op = thumb ? thumb_ops[index] : arm_ops[index];
				uint32_t pc = (8 + window * 2) << 24 | 0x100;
				prepare(0x2001, config & 31, 0x03000100);
				bind_images(waitcnt);
				unsigned width = thumb ? 2 : 4;
				image_put(test_rom, 0x100, op, width);
				image_put(test_rom, 0x100 + width, thumb ? 0x2707 : 0xe1a07007, width);
				image_put(test_rom, 0x100 + 2 * width, thumb ? 0x2631 : 0xe1a06006, width);
				actual.cpu.r[1] = config & 2 ? 0x02001000 : 0x03001000;
				bool load = thumb ? index < 3 || index == 8 : index < 3 || index == 8;
				if (load && (config & 4)) actual.cpu.r[1] = 0x0a000500;
				bool bx = thumb ? index == 10 : index == 11;
				if (bx) actual.cpu.r[1] = (config & 4 ? 0x03001000 : 0x0c000300) | (config & 8 ? 1 : 0);
				put32(0x02001000, 0x13245768); put32(0x02001004, 0x76543210);
				put32(0x03001000, 0x13245768); put32(0x03001004, 0x76543210);
				enter_mode(pc, thumb != 0);
				actual.prefetched_pc = gba->memory.lastPrefetchedPc = pc + 2 * width + (config % 10) * 2;
				actual.bios_latch = gba->memory.biosPrefetch = 0x89abcdef;
				step(op, config);
				compare_bus_state();
			}
		}
	}
	/* SWI, undefined and external IRQ enter real guest BIOS instructions and
	 * return through SPSR. No host HLE callback executes the handler. */
	image_put(test_bios, 4, 0xe1b0f00e, 4);
	image_put(test_bios, 8, 0xe1b0f00e, 4);
	image_put(test_bios, 0x18, 0xe25ef004, 4);
	reference->irqh.hitIllegal = undefined_instruction;
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned kind = 0; kind < 3; ++kind) {
		for (unsigned trial = 0; trial < 32; ++trial) {
			uint32_t pc = trial & 1 ? 0x08000100 : 0x03000100;
			uint32_t op = thumb ? (kind == 1 ? 0xde00 : 0xdf06) : (kind == 1 ? 0xe6000010 : 0xef060000);
			prepare(0x2001, trial, 0x03000100);
			bind_images((uint16_t) (trial * 19));
			if (pc >> 24 == 8) image_put(test_rom, 0x100, op, thumb ? 2 : 4);
			else if (thumb) put16(pc, (uint16_t) op);
			else put32(pc, op);
			enter_mode(pc, thumb != 0);
			actual.bios_latch = gba->memory.biosPrefetch = 0;
			if (kind == 2) {
				if (trial & 2) { actual.cpu.cpsr |= 0x80; reference->cpsr.i = 1; }
				CHECK(gbn_raise_irq(&actual) == GBN_STEP);
				ARMRaiseIRQ(reference);
				compare(actual.now, op, trial);
				if (trial & 2) continue;
			} else step(op, trial);
			compare_bus_state();
			step(0xe1b0f00e, trial);
			compare_bus_state();
			CHECK(actual.cpu.pc == pc + (kind == 2 ? 0 : thumb ? 2 : 4));
			for (unsigned address = 0; address < 4; ++address) for (unsigned width = 1; width <= 4; width *= 2) {
				struct GbnAccess access;
				CHECK(gbn_read(&actual, address, width, &access) == GBN_STEP);
				uint32_t value = width == 1 ? GBALoad8(reference, address, NULL) : width == 2 ?
					GBALoad16(reference, address, NULL) : GBALoad32(reference, address, NULL);
				CHECK(access.value == value);
				++reads;
			}
		}
	}
	reference->irqh.hitIllegal = old_illegal;
	gba->memory.rom = NULL; gba->memory.romSize = 0; gba->memory.romMask = 0;
	gba->memory.bios = old_bios; gba->memory.fullBios = 0;
	printf("PASS ROM/BIOS: %u CPU/prefetch/exception comparisons and %u bus reads\n", comparisons - before, reads);
}


static void bind_devices(void) {
	CHECK(gbn_attach_devices(&actual, &devices, palette, vram, oam));
	memcpy(gba->memory.io, devices.io, sizeof(devices.io));
	gba->video.stallMask = 0; /* Contention is a separate, not-yet-implemented stage. */
}

static void check_video_bus(void) {
	unsigned count = 0, before = comparisons;
	static const uint32_t addresses[] = {0x05000000, 0x050003fc, 0x05fffffc,
		0x06000000, 0x0600fffc, 0x06010000, 0x06013ffc, 0x06014000, 0x06017ffc,
		0x06018000, 0x0601bffc, 0x0601c000, 0x0601fffc, 0x06fffffc,
		0x07000000, 0x070003fc, 0x07fffffc};
	gbn_init(&actual, ewram, iwram);
	bind_devices();
	gba->memory.activeRegion = 3;
	gba->memory.prefetch = false;
	for (unsigned i = 0; i < sizeof(palette); ++i) palette[i] = (uint8_t) random_word();
	for (unsigned i = 0; i < sizeof(vram); ++i) vram[i] = (uint8_t) random_word();
	for (unsigned i = 0; i < sizeof(oam); ++i) oam[i] = (uint8_t) random_word();
	memcpy(gba->video.palette, palette, sizeof(palette));
	memcpy(gba->video.vram, vram, sizeof(vram));
	memcpy(gba->video.oam.raw, oam, sizeof(oam));
	for (unsigned mode = 0; mode < 8; ++mode) {
		devices.io[0] = gba->memory.io[0] = (uint16_t) mode;
		for (unsigned a = 0; a < sizeof(addresses) / sizeof(*addresses); ++a) {
			for (unsigned offset = 0; offset < 4; ++offset) for (unsigned width = 1; width <= 4; width *= 2) {
				uint32_t address = addresses[a] + offset, value = random_word(), cycles;
				int expected_cycles = 0;
				CHECK(gbn_write(&actual, address, width, value, &cycles) == GBN_STEP);
				if (width == 1) GBAStore8(reference, address, (int8_t) value, &expected_cycles);
				else if (width == 2) GBAStore16(reference, address, (int16_t) value, &expected_cycles);
				else GBAStore32(reference, address, (int32_t) value, &expected_cycles);
				CHECK(cycles == (uint32_t) expected_cycles);
				struct GbnAccess access;
				CHECK(gbn_read(&actual, address, width, &access) == GBN_STEP);
				expected_cycles = 0;
				uint32_t expected = width == 1 ? GBALoad8(reference, address, &expected_cycles) :
					width == 2 ? GBALoad16(reference, address, &expected_cycles) : GBALoad32(reference, address, &expected_cycles);
				CHECK(access.value == expected && access.cycles + 1 == (uint32_t) expected_cycles);
				++count;
			}
		}
		CHECK(!memcmp(palette, gba->video.palette, sizeof(palette)));
		CHECK(!memcmp(vram, gba->video.vram, sizeof(vram)));
		CHECK(!memcmp(oam, gba->video.oam.raw, sizeof(oam)));
	}
	static const uint16_t thumbs[] = {0x6808, 0x8808, 0x7808, 0x6008, 0x8008, 0x7008, 0xc105, 0xc905};
	static const uint32_t arms[] = {0xe5910000, 0xe1d100b0, 0xe5d10000, 0xe5810000, 0xe1c100b0,
		0xe5c10000, 0xe8810005, 0xe8910005, 0xe1010092, 0xe1410092};
	uint32_t* old_bios = gba->memory.bios;
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned trial = 0; trial < 16; ++trial) {
		for (unsigned region = 4; region <= 7; ++region) {
			unsigned n = thumb ? sizeof(thumbs) / sizeof(*thumbs) : sizeof(arms) / sizeof(*arms);
			for (unsigned i = 0; i < n; ++i) {
				uint32_t op = thumb ? thumbs[i] : arms[i];
				uint32_t pc = trial & 4 ? 0x08000100 : 0x03000100;
				prepare(0, trial, 0x03000100);
				bind_devices();
				bind_images(trial & 8 ? 0x4014 : 0);
				if (pc >> 24 == 8) {
					image_put(test_rom, 0x100, op, thumb ? 2 : 4);
					image_put(test_rom, 0x100 + (thumb ? 2 : 4), thumb ? 0x2631 : 0xe1a06006, thumb ? 2 : 4);
				} else if (thumb) put16(pc, (uint16_t) op);
				else put32(pc, op);
				actual.cpu.r[1] = region << 24 | (region == 4 ? 8 : 0x100) | (trial & 3);
				actual.cpu.r[0] = 0x5835; actual.cpu.r[2] = 0x7227;
				devices.io[0] = gba->memory.io[0] = trial & 7;
				enter_mode(pc, thumb != 0);
				actual.bios_latch = gba->memory.biosPrefetch = 0;
				step(op, trial | region << 8);
				CHECK(!memcmp(palette, gba->video.palette, sizeof(palette)));
				CHECK(!memcmp(vram, gba->video.vram, sizeof(vram)));
				CHECK(!memcmp(oam, gba->video.oam.raw, sizeof(oam)));
				compare_bus_state();
			}
		}
	}
	gba->memory.rom = NULL; gba->memory.romSize = 0; gba->memory.romMask = 0;
	gba->memory.bios = old_bios; gba->memory.fullBios = 0;
	printf("PASS video/IO bus: %u raw accesses and %u CPU comparisons\n", count, comparisons - before);
}

static void device_write(uint32_t offset, unsigned width, uint32_t value) {
	uint32_t cycles;
	CHECK(gbn_write(&actual, 0x04000000 + offset, width, value, &cycles) == GBN_STEP);
}

static uint32_t device_read(uint32_t offset, unsigned width) {
	struct GbnAccess access;
	CHECK(gbn_read(&actual, 0x04000000 + offset, width, &access) == GBN_STEP);
	return access.value;
}

static void check_device_registers(void) {
	gbn_init(&actual, ewram, iwram);
	bind_devices();
	CHECK(device_read(0, 2) == 0x80 && device_read(0x130, 2) == 0x3ff);
	device_write(0, 2, 0xffff); CHECK(device_read(0, 2) == 0xfff7);
	device_write(4, 2, 0xffff); CHECK(device_read(4, 2) == 0xff38);
	device_write(6, 2, 150); CHECK(device_read(6, 2) == 0);
	device_write(0x200, 2, 0xffff); CHECK(device_read(0x200, 2) == 0x3fff);
	gbn_request_irq(&actual, 0x3fff);
	device_write(0x202, 1, 3); CHECK(device_read(0x202, 2) == 0x3ffc);
	device_write(0x203, 1, 0x12); CHECK(device_read(0x202, 2) == 0x2dfc);
	device_write(0x202, 2, 0x2dfc); CHECK(device_read(0x202, 2) == 0);
	device_write(0x208, 2, 0xffff); CHECK(device_read(0x208, 2) == 1);
	device_write(0x204, 2, 0xffff); CHECK(device_read(0x204, 2) == 0x5fff && actual.waitcnt == 0x5fff);
	device_write(0x132, 2, 0x4003);
	gbn_set_keys(&actual, 4); CHECK(device_read(0x130, 2) == 0x3fb && !device_read(0x202, 2));
	gbn_set_keys(&actual, 1); CHECK(device_read(0x202, 2) == 0x1000);
	device_write(0x202, 2, 0x1000);
	device_write(0x132, 2, 0xc003);
	gbn_set_keys(&actual, 1); CHECK(!device_read(0x202, 2));
	gbn_set_keys(&actual, 3); CHECK(device_read(0x202, 2) == 0x1000);
	/* A word crossing from WAITCNT to an unimplemented register is rejected
	 * before either half is committed. */
	struct Gbn snapshot = actual; struct GbnDevices before_d = devices;
	uint32_t cycles = 123;
	CHECK(gbn_write(&actual, 0x04000204, 4, 0, &cycles) == GBN_UNSUPPORTED_ADDRESS);
	CHECK(cycles == 123 && !memcmp(&snapshot, &actual, sizeof(actual)) && !memcmp(&before_d, &devices, sizeof(devices)));
	/* CPU open-bus reads use the fetch belonging to the instruction's bus
	 * phase; observer reads must not advance the pipeline. */
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned offset = 0; offset < 4; ++offset) {
		uint32_t pc = 0x03000100 + (thumb ? offset & 2 : 0);
		uint32_t op = thumb ? 0x6808 : 0xe5910000;
		prepare(0, offset, pc); bind_devices();
		if (thumb) put16(pc, (uint16_t) op); else put32(pc, op);
		actual.cpu.r[1] = 0x04000010 + offset;
		enter_mode(pc, thumb != 0);
		step(op, offset);
	}
	uint32_t* old_bios = gba->memory.bios;
	unsigned before = comparisons;
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned trial = 0; trial < 64; ++trial) {
		uint32_t op = thumb ? 0x8008 : 0xe1c100b0;
		prepare(0, trial, 0x03000100); bind_devices();
		bind_images((uint16_t) (trial * 97 & 0x5fff));
		image_put(test_rom, 0x100, op, thumb ? 2 : 4);
		actual.cpu.r[1] = 0x04000204;
		actual.cpu.r[0] = (trial * 0x13b & 0x1fff) | (trial & 1 ? 0x4000 : 0);
		enter_mode(0x08000100, thumb != 0);
		actual.bios_latch = gba->memory.biosPrefetch = 0;
		step(op, trial);
		CHECK(actual.waitcnt == gba->memory.io[0x204 / 2]);
		compare_bus_state();
	}
	gba->memory.rom = NULL; gba->memory.romSize = 0; gba->memory.romMask = 0;
	gba->memory.bios = old_bios; gba->memory.fullBios = 0;
	printf("PASS device registers: masks, byte W1C, keypad IRQ, atomic refusal, open bus and %u WAITCNT CPU comparisons\n", comparisons - before);
}

static void check_video_events(void) {
	unsigned checks = 0;
	for (unsigned trial = 0; trial < 4; ++trial) {
		gbn_init(&actual, ewram, iwram); bind_devices();
		actual.now = trial & 2 ? 0xffff0000 : 0;
		uint32_t epoch = actual.now;
		bool skip = trial & 1;
		mTimingClear(&gba->timing);
		gba->timing.masterCycles = actual.now; reference->cycles = 0;
		gba->memory.fullBios = !skip;
		memset(gba->memory.io, 0, sizeof(gba->memory.io));
		gba->memory.io[0] = 0x80;
		GBAVideoReset(&gba->video);
		gbn_video_start(&actual, skip);
		/* Writing DISPSTAT recomputes VCount match in both machines. */
		device_write(4, 2, 0x9f38); GBAIOWrite(gba, 4, 0x9f38);
		for (unsigned tick = 0; tick < 228 * 6; ++tick) {
			uint32_t target = actual.events[GBN_EVENT_VIDEO].when + tick % 17;
			uint32_t elapsed = target - actual.now;
			actual.now = target;
			CHECK(gbn_service_events(&actual) == GBN_STEP);
			mTimingTick(&gba->timing, (int32_t) elapsed);
			CHECK(devices.io[2] == gba->memory.io[2]);
			CHECK(devices.io[3] == gba->memory.io[3]);
			CHECK(devices.io[0x101] == gba->memory.io[0x101]);
			CHECK(devices.frames == (uint32_t) gba->video.frameCounter);
			CHECK(actual.events[GBN_EVENT_VIDEO].when == gba->video.event.when);
			++checks;
		}
		/* Catch up several overdue scanline edges without shifting phase. */
		actual.now += 1232 * 3 + 19;
		CHECK(gbn_service_events(&actual) == GBN_STEP);
		mTimingTick(&gba->timing, 1232 * 3 + 19);
		CHECK(devices.io[2] == gba->memory.io[2] && devices.io[3] == gba->memory.io[3]);
		CHECK(actual.events[GBN_EVENT_VIDEO].when == gba->video.event.when);
		CHECK(actual.now != epoch);
	}
	mTimingClear(&gba->timing); gba->memory.fullBios = 0;
	/* IRQ request/delivery uses the same exception path already checked
	 * against ARM7: delayed, maskable and acknowledged separately in IF. */
	prepare(0x2001, 0, 0x03000100); bind_devices();
	CHECK(gbn_attach_bios(&actual, test_bios, sizeof(test_bios)));
	CHECK(gbn_enter_thumb(&actual, 0x03000100) == GBN_STEP);
	device_write(0x200, 2, 1); device_write(0x208, 2, 1);
	gbn_request_irq(&actual, 1);
	CHECK(actual.events[GBN_EVENT_IRQ].when == 7);
	actual.now = 6; CHECK(gbn_service_events(&actual) == GBN_STEP && actual.cpu.pc == 0x03000100);
	actual.now = 7; CHECK(gbn_service_events(&actual) == GBN_STEP && actual.cpu.pc == 0x18);
	CHECK(actual.cpu.r[14] == 0x03000104 && (actual.cpu.cpsr & 0xbf) == 0x92);
	CHECK(device_read(0x202, 2) == 1);
	device_write(0x202, 2, 1); CHECK(device_read(0x202, 2) == 0);
	CHECK(gbn_schedule_at(&actual, 3, actual.now - 2, 0));
	CHECK(gbn_service_events(&actual) == GBN_EVENT && actual.events[3].active);
	uint32_t late;
	CHECK(gbn_take_event(&actual, &late) == 3 && late == 2);
	CHECK(!gbn_schedule_at(&actual, 0, actual.now + GBN_MAX_DELAY + 1, 0));
	CHECK(!gbn_schedule_at(&actual, 0, actual.now - GBN_MAX_DELAY - 1, 0));
	printf("PASS video timing: %u edge comparisons, late catch-up, wraparound and IRQ delivery\n", checks);
}


static void dma_prepare(unsigned trial, bool video) {
	prepare(0x2001, 0, 0x03000100); bind_devices(); bind_images((uint16_t) (trial * 0x53 & 0x5fff));
	actual.ewram_wait = 2;
	gba->memory.waitstatesNonseq16[2] = gba->memory.waitstatesSeq16[2] = 2;
	gba->memory.waitstatesNonseq32[2] = gba->memory.waitstatesSeq32[2] = 5;
	enter(0x03000100);
	mTimingClear(&gba->timing);
	actual.now = trial & 1 ? 0xfffffff0 : 0;
	gba->timing.masterCycles = actual.now;
	reference->cycles = 0; reference->nextEvent = INT32_MAX;
	gba->cpuBlocked = false; gba->performingDMA = 0; gba->earlyExit = false;
	GBADMAReset(gba);
	gba->bus = devices.dma_bus = 0;
	for (unsigned i = 0; i < 4; ++i) devices.dma[i].latch = gba->memory.dma[i].latch = 0x13572468 + i;
	if (video) {
		gba->memory.fullBios = 1;
		GBAVideoReset(&gba->video);
		memset(palette, 0, sizeof(palette)); memset(vram, 0, sizeof(vram)); memset(oam, 0, sizeof(oam));
		gbn_video_start(&actual, false);
		device_write(4, 2, 0x7038); GBAIOWrite(gba, 4, 0x7038);
	}
}

static void dma_reg(unsigned channel, unsigned part, unsigned width, uint32_t value) {
	uint32_t offset = 0xb0 + channel * 12 + part;
	device_write(offset, width, value);
	if (width == 4) GBAIOWrite32(gba, offset, value);
	else if (width == 2) GBAIOWrite(gba, offset, (uint16_t) value);
	else GBAIOWrite8(gba, offset, (uint8_t) value);
}

static void dma_configure(unsigned channel, uint32_t source, uint32_t dest, unsigned count, unsigned control) {
	dma_reg(channel, 0, 4, source);
	dma_reg(channel, 4, 4, dest);
	dma_reg(channel, 8, 4, (control << 16) | count);
}

static unsigned dma_checks;
static void dma_compare(void) {
	CHECK(actual.now == gba->timing.masterCycles);
	CHECK(devices.dma_blocked == gba->cpuBlocked);
	CHECK(devices.active_dma == gba->memory.activeDMA);
	CHECK(devices.dma_bus == gba->bus);
	for (unsigned i = 0; i < 4; ++i) {
		struct GbnDma* a = &devices.dma[i];
		struct GBADMA* r = &gba->memory.dma[i];
		bool same = a->source == r->source && a->dest == r->dest && a->count == (uint32_t) r->count &&
			a->next_source == r->nextSource && a->next_dest == r->nextDest && a->control == r->reg &&
			a->remaining == ((uint32_t) r->nextCount & 0x7fffffff) && a->finishing == (r->nextCount < 0) &&
			a->when == r->when && a->latch == r->latch;
		if (!same) {
			fprintf(stderr,"DMA mismatch ch=%u ctrl=%04x/%04x count=%u/%u live=%08x/%08x -> %08x/%08x left=%u/%u when=%u/%u latch=%08x/%08x\n",
				i,a->control,r->reg,a->count,r->count,a->next_source,r->nextSource,a->next_dest,r->nextDest,
				a->remaining,r->nextCount,a->when,r->when,a->latch,r->latch);
			exit(1);
		}
		/* Source/destination are write-only. The reference keeps unmasked
		 * shadow halves after narrow writes; compare the actual address latches
		 * above, not those inaccessible implementation copies. */
		CHECK(!memcmp(&devices.io[(0xb8 + 12 * i) / 2], &gba->memory.io[(0xb8 + 12 * i) / 2], 4));
	}
	++dma_checks;
}

static void dma_events(uint32_t target) {
	uint32_t elapsed = target - actual.now;
	actual.now = target;
	enum GbnStatus status = gbn_service_events(&actual);
	if (status != GBN_STEP) {
		int channel = devices.active_dma;
		fprintf(stderr, "DMA stop=%u channel=%d now=%u source=%08x dest=%08x control=%04x\n", status, channel,
			actual.now, channel < 0 ? 0 : devices.dma[channel].next_source,
			channel < 0 ? 0 : devices.dma[channel].next_dest, channel < 0 ? 0 : devices.dma[channel].control);
		exit(1);
	}
	int32_t next = mTimingTick(&gba->timing, (int32_t) elapsed);
	unsigned guard = 0;
	while (gba->cpuBlocked) {
		CHECK(++guard < 1000000);
		next = mTimingTick(&gba->timing, next > 0 ? next : 0);
	}
	dma_compare();
}

static void check_dma(void) {
	uint32_t* old_bios = gba->memory.bios;
	memcpy(gba->memory.wram, ewram, sizeof(ewram));
	memcpy(gba->memory.iwram, iwram, sizeof(iwram));
	memcpy(gba->video.palette, palette, sizeof(palette));
	memcpy(gba->video.vram, vram, sizeof(vram));
	memcpy(gba->video.oam.raw, oam, sizeof(oam));
	static const unsigned sr[] = {2, 3, 8, 10, 12, 0};
	static const unsigned dr[] = {3, 2, 5, 6, 7, 3};
	for (unsigned channel = 0; channel < 4; ++channel) for (unsigned word = 0; word < 2; ++word) {
		for (unsigned source_mode = 0; source_mode < 4; ++source_mode) for (unsigned dest_mode = 0; dest_mode < 4; ++dest_mode) {
			for (unsigned trial = 0; trial < 6; ++trial) {
				dma_prepare(trial, false);
				uint32_t source = sr[trial] << 24 | 0x400 | (trial & 3);
				/* DMA0 cannot address Game Pak. Use its BIOS/open-bus alias
				 * instead of aliasing WS2 into unimplemented extended IO. */
				if (!channel && trial == 4) source = 0x08000400 | (trial & 3);
				uint32_t dest = dr[trial] << 24 | 0x200 | (trial & 3);
				unsigned control = 0xc000 | word << 10 | source_mode << 7 | dest_mode << 5;
				dma_configure(channel, source, dest, 17, control);
				CHECK(devices.dma[channel].when == actual.now + 3);
				dma_events(actual.now + 3 + trial * 3); /* Instruction-boundary lateness. */
				CHECK(device_read(0xb8 + channel * 12, 2) == 0);
				CHECK(devices.io[0x101] == (0x100u << channel));
				for (unsigned i = 0; i < 36; ++i) {
					uint32_t address = (dest & ~3u) - 68 + i * 4;
					struct GbnAccess access;
					CHECK(gbn_read(&actual, address, 4, &access) == GBN_STEP);
					CHECK(access.value == GBALoad32(reference, address, NULL));
				}
			}
		}
	}
	/* Register masks and count latching through individual byte writes. */
	for (unsigned channel = 0; channel < 4; ++channel) {
		dma_prepare(0, false);
		for (unsigned part = 0; part < 10; ++part) {
			dma_reg(channel, part, 1, 0xff);
			dma_compare();
		}
		dma_reg(channel, 0, 4, 0x03000400); dma_reg(channel, 4, 4, 0x02000400);
		dma_reg(channel, 8, 2, 0); dma_reg(channel, 10, 2, 0x9000);
		CHECK(devices.dma[channel].count == (channel == 3 ? 0x10000u : 0x4000u));
		dma_reg(channel, 8, 2, 7);
		CHECK(devices.dma[channel].count == (channel == 3 ? 0x10000u : 0x4000u));
		dma_compare();
	}
	/* Maximum DMA3 length: fixed source, complete EWRAM clear, release timing. */
	dma_prepare(0, false); put32(0x03000400, 0x12345678);
	dma_configure(3, 0x03000400, 0x02000000, 0, 0x8500);
	dma_events(actual.now + 3);
	CHECK(!memcmp(ewram, gba->memory.wram, sizeof(ewram)));
	CHECK(ewram[0] == 0x78 && ewram[GBN_EWRAM_SIZE - 1] == 0x12);
	/* Equal start times must service lower channel numbers first. */
	dma_prepare(0, false);
	for (unsigned c = 0; c < 4; ++c) put32(0x03000400 + c * 4, 0x11111111u * (c + 1));
	for (unsigned c = 4; c-- > 0;) dma_configure(c, 0x03000400 + c * 4, 0x02000400, 3, 0x8540);
	dma_events(actual.now + 3);
	struct GbnAccess access;
	CHECK(gbn_read(&actual, 0x02000400, 4, &access) == GBN_STEP && access.value == 0x44444444);
	/* A long transfer crosses several LCD edges. No CPU instruction executes
	 * during those events; LCD state and DMA completion time still match. */
	dma_prepare(0, true);
	struct GbnCpu saved_cpu = actual.cpu;
	dma_configure(3, 0x02002000, 0x06002000, 1024, 0xc400);
	dma_events(actual.now + 3);
	CHECK(!memcmp(&saved_cpu, &actual.cpu, sizeof(saved_cpu)));
	CHECK(devices.io[2] == gba->memory.io[2] && devices.io[3] == gba->memory.io[3]);
	CHECK(devices.io[3] > 3 && devices.io[0x101] == gba->memory.io[0x101]);
	CHECK(!memcmp(vram, gba->video.vram, sizeof(vram)));
	/* Repeating HBlank transfers reload destination and the newly written
	 * count, while retaining the live source cursor between scanlines. */
	dma_prepare(0, true);
	dma_configure(0, 0x02004000, 0x05000000, 3, 0xe660);
	for (unsigned line = 0; line < 4; ++line) {
		while (actual.now < line * 1232 + 1011) dma_events(actual.events[actual.next_event].when);
		CHECK(devices.dma[0].next_dest == 0x05000000 && (devices.dma[0].control & 0x8000));
		dma_reg(0, 8, 2, line + 4);
	}
	CHECK(!memcmp(palette, gba->video.palette, sizeof(palette)));
	/* VBlank and DMA3 display-start triggers are driven by the LCD itself. */
	for (unsigned custom = 0; custom < 2; ++custom) {
		dma_prepare(0, true);
		dma_configure(3, 0x02004000, 0x06000000, 2, custom ? 0xf640 : 0xd400);
		unsigned guard = 0;
		while (devices.dma[3].control & 0x8000) {
			CHECK(++guard < 1000);
			dma_events(actual.events[actual.next_event].when);
		}
		CHECK(devices.io[0x101] == gba->memory.io[0x101]);
		CHECK(!memcmp(vram, gba->video.vram, sizeof(vram)));
	}
	/* Guest STM programs source, destination and count/control together.
	 * The three-cycle start can become due before that instruction retires. */
	for (unsigned thumb = 0; thumb < 2; ++thumb) {
		dma_prepare(0, false);
		uint32_t op = thumb ? 0xc00e : 0xe8a0000e;
		if (thumb) put16(0x03000100, (uint16_t) op); else put32(0x03000100, op);
		actual.cpu.r[0] = 0x040000d4; actual.cpu.r[1] = 0x03000400;
		actual.cpu.r[2] = 0x06000400; actual.cpu.r[3] = 0x84000004;
		enter_mode(0x03000100, thumb != 0);
		CHECK(gbn_step(&actual) == GBN_STEP); ARMRun(reference);
		compare(actual.now, op, thumb);
		CHECK(gbn_service_events(&actual) == GBN_STEP);
		int32_t cycles = reference->cycles; reference->cycles = 0;
		int32_t next = mTimingTick(&gba->timing, cycles);
		while (gba->cpuBlocked) next = mTimingTick(&gba->timing, next > 0 ? next : 0);
		dma_compare();
		CHECK(!memcmp(vram + 0x400, (uint8_t*) gba->video.vram + 0x400, 16));
	}

	/* Paused DMA yields an external event, and gbn_step must not execute
	 * even after that event is consumed but before DMA releases the bus. */
	dma_prepare(0, false);
	dma_configure(3, 0x02002000, 0x03002000, 8, 0x8400);
	CHECK(gbn_schedule(&actual, 0, 5, 0));
	actual.now += 3;
	CHECK(gbn_service_events(&actual) == GBN_EVENT && actual.now == 5 && devices.dma_blocked);
	CHECK(gbn_take_event(&actual, NULL) == 0);
	struct Gbn snapshot = actual;
	CHECK(gbn_step(&actual) == GBN_EVENT && !memcmp(&snapshot, &actual, sizeof(actual)));
	CHECK(gbn_service_events(&actual) == GBN_STEP && !devices.dma_blocked);
	/* An unsupported second destination retains the completed first beat
	 * and its live cursors, while retrying the failed beat has no effects. */
	dma_prepare(0, false);
	put32(0x03000400, 0x12345678);
	dma_configure(3, 0x03000400, 0x07fffffc, 2, 0x8400);
	actual.now += 3;
	CHECK(gbn_service_events(&actual) == GBN_UNSUPPORTED_ADDRESS);
	CHECK(devices.dma[3].remaining == 1 && devices.dma[3].next_dest == 0x08000000);
	CHECK(oam[0x3fc] == 0x78 && oam[0x3ff] == 0x12);
	snapshot = actual; struct GbnDevices device_snapshot = devices;
	CHECK(gbn_service_events(&actual) == GBN_UNSUPPORTED_ADDRESS);
	CHECK(!memcmp(&snapshot, &actual, sizeof(actual)) && !memcmp(&device_snapshot, &devices, sizeof(devices)));
	device_write(0xde, 2, 0);
	CHECK(!devices.dma_blocked && !actual.events[GBN_EVENT_DMA].active);
	/* Self-modifying DMA leaves already-prefetched instructions intact. */
	dma_prepare(0, false);
	put32(0x03000400, 0x21092108);
	dma_configure(3, 0x03000400, 0x03000102, 2, 0x8000);
	dma_events(actual.now + 3);
	uint32_t started = actual.now;
	for (unsigned i = 0; i < 3; ++i) {
		uint32_t op = actual.cpu.pipe[0];
		CHECK(gbn_step(&actual) == GBN_STEP);
		ARMRun(reference); compare(actual.now - started, op, i);
	}
	CHECK(actual.cpu.r[7] == 7 && actual.cpu.r[1] == 9);
	/* The last DMA beat drives open-bus data during the first resumed CPU
	 * instruction, including reads of write-only IO. */
	dma_prepare(0, false); put16(0x03000100, 0x6808);
	actual.cpu.r[1] = 0x04000010;
	enter(0x03000100);
	put32(0x03000400, 0x1234abcd);
	dma_configure(3, 0x03000400, 0x02000400, 1, 0x8400);
	dma_events(actual.now + 3);
	started = actual.now;
	CHECK(gbn_step(&actual) == GBN_STEP); ARMRun(reference);
	compare(actual.now - started, 0x6808, 0);
	CHECK(actual.cpu.r[0] == 0xabcdabcd);

	mTimingClear(&gba->timing);
	gba->memory.rom = NULL; gba->memory.romSize = 0; gba->memory.romMask = 0;
	gba->memory.bios = old_bios; gba->memory.fullBios = 0;
	printf("PASS DMA: %u register/transfer/event comparisons; four channels, widths, addressing, repeat, LCD triggers, priority and full-length transfer\n", dma_checks);
}


static void timer_prepare(uint32_t epoch) {
	gbn_init(&actual, ewram, iwram); bind_devices();
	actual.now = epoch;
	mTimingClear(&gba->timing); gba->timing.masterCycles = epoch;
	reference->cycles = 0; reference->nextEvent = INT32_MAX;
	gba->audio.enable = false;
	GBATimerInit(gba);
}

static void timer_compare(unsigned id) {
	GBATimerUpdateRegister(gba, (int) id, 0);
	unsigned offset = 0x100 + 4 * id;
	uint16_t value = (uint16_t) device_read(offset, 2);
	if (value != gba->memory.io[offset / 2]) {
		fprintf(stderr, "timer %u at %u value=%04x/%04x epoch=%u reload=%04x control=%04x\n", id, actual.now,
			value, gba->memory.io[offset / 2], devices.timers[id].epoch, devices.timers[id].reload, devices.timers[id].control);
		exit(1);
	}
	CHECK(device_read(offset + 2, 2) == gba->memory.io[(offset + 2) / 2]);
	CHECK(devices.timers[id].reload == gba->timers[id].reload);
	if (actual.events[GBN_EVENT_TIMER0 + id].active) CHECK(actual.events[GBN_EVENT_TIMER0 + id].when == gba->timers[id].event.when);
	CHECK(devices.io[0x202 / 2] == gba->memory.io[0x202 / 2]);
}

static void timer_write(unsigned offset, uint16_t value) {
	device_write(offset, 2, value); GBAIOWrite(gba, offset, value);
}

static void timer_tick(uint32_t target) {
	uint32_t elapsed = target - actual.now;
	actual.now = target;
	CHECK(gbn_service_events(&actual) == GBN_STEP);
	mTimingTick(&gba->timing, (int32_t) elapsed);
}

static void check_timers(void) {
	unsigned cases = 0;
	static const uint16_t reloads[] = {0, 0xff00, 0xfffc, 0xffff};
	static const unsigned shifts[] = {0, 6, 8, 10};
	for (unsigned id = 0; id < 4; ++id) for (unsigned divider = 0; divider < 4; ++divider) {
		for (unsigned wrap = 0; wrap < 2; ++wrap) for (unsigned r = 0; r < 4; ++r) {
			timer_prepare((wrap ? 0xfff00000 : 0) + 37);
			unsigned offset = 0x100 + 4 * id;
			timer_write(offset, reloads[r]);
			CHECK(device_read(offset, 2) == 0); /* Reload write does not start the counter. */
			timer_write(offset + 2, (uint16_t) (0xc0 | divider));
			for (unsigned n = 0; n < 20; ++n) {
				uint32_t target = actual.now + (1u << shifts[divider]) * (n % 5 + 1);
				timer_tick(target); timer_compare(id); ++cases;
				if (n == 5) timer_write(offset, (uint16_t) (reloads[r] ^ 0x17));
				if (n == 10) timer_write(offset + 2, (uint16_t) (0xc0 | ((divider + 1) & 3)));
				if (n == 15) timer_write(offset + 2, 0);
				if (n == 16) timer_write(offset + 2, (uint16_t) (0xc0 | divider));
			}
		}
	}
	/* A four-stage chain overflows at different rates. Compare at each source
 * edge so the reference's late-event coalescing cannot lose cascade pulses. */
	for (unsigned wrap = 0; wrap < 2; ++wrap) {
		timer_prepare(wrap ? 0xfffffff0 : 0);
		for (unsigned id = 0; id < 4; ++id) {
			timer_write(0x100 + 4 * id, (uint16_t) (0xfffd + (id & 1)));
			timer_write(0x102 + 4 * id, (uint16_t) (id ? 0xc4 : 0xc0));
		}
		for (unsigned n = 0; n < 96; ++n) {
			timer_tick(actual.events[GBN_EVENT_TIMER0].when);
			for (unsigned id = 0; id < 4; ++id) { timer_compare(id); ++cases; }
		}
	}
	/* Independent late catch-up preserves every cascade pulse, even when the
 * caller crosses multiple overflows in one instruction/device boundary. */
	timer_prepare(0);
	device_write(0x100, 4, 0x0080fffe); device_write(0x104, 4, 0x00840000);
	actual.now = 101; CHECK(gbn_service_events(&actual) == GBN_STEP);
	CHECK(device_read(0x100, 2) == 0xffff && device_read(0x104, 2) == 50);
	CHECK(actual.events[GBN_EVENT_TIMER0].when == 102);
	/* Byte writes merge the reload latch, not the currently running counter. */
	device_write(0x100, 1, 0x78); device_write(0x101, 1, 0x56);
	CHECK(devices.timers[0].reload == 0x5678 && device_read(0x100, 2) == 0xffff);
	device_write(0x102, 1, 0); device_write(0x102, 1, 0x80);
	CHECK(device_read(0x100, 2) == 0x5678);
	unsigned before = comparisons;
	static const uint16_t thumb_ops[] = {0x8808, 0x7808, 0x6808};
	static const uint32_t arm_ops[] = {0xe1d100b0, 0xe5d10000, 0xe5910000};
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned kind = 0; kind < 3; ++kind) {
		for (unsigned divider = 0; divider < 4; ++divider) for (unsigned trial = 0; trial < 4; ++trial) {
			unsigned op = thumb ? thumb_ops[kind] : arm_ops[kind];
			prepare(0, trial, 0x03000100); bind_devices();
			mTimingClear(&gba->timing); GBATimerInit(gba); gba->audio.enable = false;
			if (thumb) put16(0x03000100, (uint16_t) op); else put32(0x03000100, op);
			actual.cpu.r[1] = 0x04000100;
			enter_mode(0x03000100, thumb != 0);
			timer_write(0x100, 0xf000); timer_write(0x102, (uint16_t) (0x80 | divider));
			actual.now = reference->cycles = (int32_t) (65 + 257 * trial);
			step(op, trial);
		}
	}
	printf("PASS timers: %u divider/reload/control/cascade/IRQ comparisons; late catch-up, byte latches and wraparound\n", cases);
	printf("PASS timer CPU reads: %u ARM/Thumb byte/halfword/word comparisons\n", comparisons - before);
}

static void audio_prepare(void) {
	timer_prepare(0);
	GBAAudioReset(&gba->audio);
	/* Its device reset leaves the shared GB PSG power flag untouched. Give
	 * each independent fixture a fresh power transition and sequencer phase. */
	gba->audio.psg.enable = false; gba->audio.psg.skipFrame = false;
	mTimingClear(&gba->timing);
}

static void audio_write(unsigned offset, unsigned width, uint32_t value) {
	device_write(offset, width, value);
	if (width == 4) GBAIOWrite32(gba, offset, value);
	else if (width == 2) GBAIOWrite(gba, offset, (uint16_t) value);
	else GBAIOWrite8(gba, offset, (uint8_t) value);
}

static void audio_register_compare(void) {
	static const unsigned offsets[] = {0x60, 0x62, 0x64, 0x68, 0x6c, 0x70, 0x72, 0x74, 0x78, 0x7c, 0x80, 0x82, 0x84, 0x88};
	for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
		uint32_t value = device_read(offsets[i], 2), expected = GBAIORead(gba, offsets[i]);
		if (value != expected) {
			fprintf(stderr, "audio register %02x value=%04x/%04x\n", offsets[i], value, expected); exit(1);
		}
	}
}

static int16_t audio_expected(int sample, unsigned bias) {
	sample += (int) bias;
	if (sample < 0) sample = 0;
	if (sample > 1023) sample = 1023;
	sample = (sample - (int) bias) * 48;
	if (sample < -32768) sample = -32768;
	if (sample > 32767) sample = 32767;
	return (int16_t) sample;
}

static void check_audio(void) {
	unsigned registers = 0, samples = 0;
	static const unsigned offsets[] = {0x60, 0x62, 0x64, 0x68, 0x6c, 0x70, 0x72, 0x74, 0x78, 0x7c, 0x80, 0x82, 0x84, 0x88};
	for (unsigned trial = 0; trial < 96; ++trial) {
		audio_prepare(); audio_write(0x84, 2, 0x80);
		for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
			unsigned width = 1u << (trial % 3);
			unsigned offset = offsets[i];
			if (width == 1) offset += trial >> 2 & 1;
			else if (width == 4) offset &= ~3u;
			audio_write(offset, width, random_word()); audio_register_compare(); ++registers;
		}
		audio_write(0x84, 2, 0); audio_register_compare(); ++registers;
	}
	/* Compare actual oscillator output with the old PSG at fixed observation
 * points. Its frontend batching/latency is deliberately outside this test. */
	for (unsigned id = 0; id < 4; ++id) for (unsigned trial = 0; trial < 8; ++trial) {
		audio_prepare(); audio_write(0x84, 2, 0x80);
		audio_write(0x80, 2, 0x0077 | (0x1100u << id)); audio_write(0x82, 2, 2);
		if (id == 2) {
			for (unsigned bank = 0; bank < 2; ++bank) {
				audio_write(0x70, 2, bank ? 0 : 0x40);
				for (unsigned i = 0; i < 16; i += 4) audio_write(0x90 + i, 4, 0xfedcba98u ^ (0x1234567u * (i + bank)));
			}
			audio_write(0x70, 2, 0x80 | (trial & 1 ? 0x20 : 0) | (trial & 2 ? 0x40 : 0));
			audio_write(0x72, 2, 0x2000u * (1 + trial % 7) | 0xe0);
			audio_write(0x74, 2, 0xc641);
		} else {
			unsigned base = id == 0 ? 0x62 : id == 1 ? 0x68 : 0x78;
			audio_write(base, 2, (trial & 1 ? 0x8900 : 0xf100) | (trial & 3) << 6 | 0x20);
			if (!id) audio_write(0x60, 2, trial & 4 ? 0x1a : 0);
			audio_write(base + (id == 1 ? 4 : id == 3 ? 4 : 2), 2, id == 3 ? 0xc035 | (trial & 2 ? 8 : 0) : 0xc241);
		}
		gbn_audio_start(&actual); gbn_cancel(&actual, GBN_EVENT_AUDIO); gbn_cancel(&actual, GBN_EVENT_AUDIO_FRAME);
		for (unsigned tick = 0; tick < 512; ++tick) {
			actual.now = tick * 4096;
			gba->timing.masterCycles = actual.now;
			if (!(tick & 7)) {
				gbn_audio_frame(&actual, actual.now); gbn_cancel(&actual, GBN_EVENT_AUDIO_FRAME);
				GBAudioUpdateFrame(&gba->audio.psg);
			}
			gbn_audio_service(&actual, actual.now); gbn_cancel(&actual, GBN_EVENT_AUDIO);
			GBAudioRun(&gba->audio.psg, (int32_t) actual.now, 0xf);
			int16_t left = 0, right = 0;
			GBAudioSamplePSG(&gba->audio.psg, &left, &right);
			/* The old helper retains a cached sample after length expires.
			 * Status comparisons below still check the channel's actual lifetime;
			 * an inactive hardware channel contributes silence to the new mixer. */
			if (!(GBAIORead(gba, 0x84) & (1u << id))) left = right = 0;
			struct GbnStereo output;
			CHECK(gbn_audio_read(&actual, &output, 1) == 1);
			int16_t expected_left = audio_expected(left >> 2, 512), expected_right = audio_expected(right >> 2, 512);
			if (output.left != expected_left || output.right != expected_right) {
				fprintf(stderr, "PSG id=%u trial=%u at=%u output=%d/%d expected=%d/%d phase=%u volume=%u sample=%u length=%u enabled=%u\n",
					id,trial,actual.now,output.left,output.right,expected_left,expected_right,
					devices.audio.psg[id].phase,devices.audio.psg[id].volume,devices.audio.psg[id].sample,
					devices.audio.psg[id].length,devices.audio.psg[id].enabled); exit(1);
			}
			audio_register_compare(); ++samples;
		}
	}
	/* DMA FIFO requests force four words and a fixed destination even if the
 * game programs a halfword width/count. Distinct signed bytes test ordering. */
	for (unsigned fifo = 0; fifo < 2; ++fifo) for (unsigned channel = 1; channel <= 2; ++channel) {
		gbn_init(&actual, ewram, iwram); bind_devices();
		device_write(0x84, 2, 0x80);
		device_write(0x82, 2, 0x300u << (fifo * 4) | 4u << fifo);
		for (unsigned i = 0; i < 128; ++i) iwram[0x400 + i] = (uint8_t) (i * 7 + 0x80);
		device_write(0xb0 + channel * 12, 4, 0x03000400);
		device_write(0xb4 + channel * 12, 4, 0x040000a0 + 4 * fifo);
		device_write(0xb8 + channel * 12, 4, 0xf2000001); /* Repeat, custom, IRQ, deliberately width=16/count=1. */
		device_write(0x100, 4, 0x0080fc00); /* Overflow every 1024 cycles. */
		for (unsigned n = 0; n < 48; ++n) {
			actual.now = actual.events[GBN_EVENT_TIMER0].when;
			CHECK(gbn_service_events(&actual) == GBN_STEP);
			struct GbnFifo* f = &devices.audio.fifo[fifo];
			CHECK(f->sample == (n ? (int8_t) ((n - 1) * 7 + 0x80) : 0));
			CHECK(f->size >= 16 && f->size <= 32);
			CHECK(devices.dma[channel].next_dest == 0x040000a0u + 4 * fifo);
			CHECK(devices.dma[channel].count == 1 && (devices.dma[channel].control & 0x8000));
			CHECK(devices.io[0x202 / 2] & (0x100u << channel));
			gbn_audio_service(&actual, actual.now); gbn_cancel(&actual, GBN_EVENT_AUDIO);
			struct GbnStereo output; CHECK(gbn_audio_read(&actual, &output, 1) == 1);
			CHECK(output.left == audio_expected(f->sample * 4, 512) && output.right == output.left);
		}
		CHECK(devices.audio.fifo[fifo].requests == 4);
	}
	gbn_init(&actual, ewram, iwram); bind_devices(); gbn_audio_start(&actual);
	actual.now = 512 * (GBN_AUDIO_CAPACITY + 9); CHECK(gbn_service_events(&actual) == GBN_STEP);
	CHECK(devices.audio.produced == GBN_AUDIO_CAPACITY + 10 && devices.audio.dropped == 10);
	device_write(0x88, 2, 0xc200);
	CHECK(gbn_audio_rate(&actual) == 262144 && actual.events[GBN_EVENT_AUDIO].when == actual.now + 64);
	printf("PASS audio: %u register comparisons, %u PSG stereo observations; timer/FIFO DMA, signed PCM, buffer bounds and rate changes\n", registers, samples);
}

static void check_sio(void) {
	unsigned cases = 0;
	static const uint16_t rcnt[] = {0x8000, 0, 0x4000, 0xc000};
	for (unsigned r = 0; r < 4; ++r) for (unsigned serial_mode = 0; serial_mode < 3; ++serial_mode) {
		for (unsigned trial = 0; trial < 64; ++trial) {
			timer_prepare(0); GBASIOReset(&gba->sio);
			uint16_t pins = rcnt[r] | (trial * 23 & 0x1ff);
			device_write(0x134, 2, pins); GBAIOWrite(gba, 0x134, pins);
			unsigned width = trial & 1 ? 1 : 2;
			uint16_t control = (uint16_t) (serial_mode << 12 | (trial * 37 & 0x4f7f));
			device_write(0x128, 2, control); GBAIOWrite(gba, 0x128, control);
			if (width == 1) {
				device_write(0x128, 1, control ^ 3); GBAIOWrite8(gba, 0x128, (uint8_t) (control ^ 3));
			}
			CHECK(device_read(0x128, 2) == GBAIORead(gba, 0x128));
			CHECK(device_read(0x134, 2) == GBAIORead(gba, 0x134));
			static const unsigned data_registers[] = {0x120, 0x122, 0x12a};
			for (unsigned i = 0; i < 3; ++i) {
				unsigned offset = data_registers[i];
				device_write(offset, 2, (uint16_t) (0x8123 + offset)); GBAIOWrite(gba, offset, (uint16_t) (0x8123 + offset));
				if (device_read(offset, 2) != GBAIORead(gba, offset)) {
					fprintf(stderr, "SIO data rcnt=%04x mode=%u trial=%u offset=%03x actual=%04x expected=%04x\n",
						pins,serial_mode,trial,offset,device_read(offset,2),GBAIORead(gba,offset)); exit(1);
				}
			}
			++cases;
		}
	}
	for (unsigned serial_mode = 0; serial_mode < 3; ++serial_mode) for (unsigned speed = 0; speed < 4; ++speed) {
		for (unsigned wrap = 0; wrap < 2; ++wrap) {
			timer_prepare(wrap ? 0xfffffff0 : 0); GBASIOReset(&gba->sio);
			device_write(0x134, 2, 0); GBAIOWrite(gba, 0x134, 0);
			uint16_t control = (uint16_t) (serial_mode << 12 | 0x4080 | speed);
			if (serial_mode < 2) control |= 1; /* Internal clock owner. */
			/* The legacy start helper reads its cached clock control before
			 * applying this write. Program the clock before the start edge. */
			device_write(0x128, 2, control & ~0x80u); GBAIOWrite(gba, 0x128, control & ~0x80u);
			device_write(0x128, 2, control); GBAIOWrite(gba, 0x128, control);
			if (!actual.events[GBN_EVENT_SIO].active || actual.events[GBN_EVENT_SIO].when != gba->sio.completeEvent.when) {
				fprintf(stderr, "SIO deadline mode=%u speed=%u wrap=%u actual=%u expected=%u now=%u\n", serial_mode,speed,wrap,
					actual.events[GBN_EVENT_SIO].when,gba->sio.completeEvent.when,actual.now); exit(1);
			}
			CHECK(device_read(0x128, 2) == GBAIORead(gba, 0x128));
			device_write(0x12a, 2, 0x5a39); GBAIOWrite(gba, 0x12a, 0x5a39);
			uint32_t finish = actual.events[GBN_EVENT_SIO].when;
			timer_tick(finish - 1); CHECK(device_read(0x128, 2) & 0x80);
			timer_tick(finish + 3);
			CHECK(!(device_read(0x128, 2) & 0x80) && !actual.events[GBN_EVENT_SIO].active);
			CHECK(device_read(0x128, 2) == GBAIORead(gba, 0x128));
			CHECK(device_read(0x202, 2) == 0x80 && device_read(0x202, 2) == GBAIORead(gba, 0x202));
			/* The legacy dummy link finishes with zero bytes. A disconnected
			 * physical SI is pulled high; missing multiplayer peers are FFFF. */
			if (serial_mode == 0) CHECK(device_read(0x12a, 2) == 0xff);
			else if (serial_mode == 1) CHECK(device_read(0x120, 4) == 0xffffffff);
			else {
				CHECK(device_read(0x120, 2) == 0x5a39);
				for (unsigned i = 1; i < 4; ++i) CHECK(device_read(0x120 + 2 * i, 2) == 0xffff);
			}
			++cases;
		}
	}
	timer_prepare(0);
	device_write(0x134, 2, 0); device_write(0x128, 2, 0x4080); /* No external clock source. */
	CHECK(!actual.events[GBN_EVENT_SIO].active);
	actual.now = 10000; CHECK(gbn_service_events(&actual) == GBN_STEP && (device_read(0x128, 2) & 0x80));
	device_write(0x128, 2, 0); CHECK(!(device_read(0x128, 2) & 0x80));
	device_write(0x128, 2, 0x4081); CHECK(actual.events[GBN_EVENT_SIO].active);
	device_write(0x134, 2, 0x8000); CHECK(!actual.events[GBN_EVENT_SIO].active);
	device_write(0x134, 2, 0); device_write(0x128, 2, 0x3000);
	CHECK(gbn_service_events(&actual) == GBN_UNSUPPORTED_DEVICE && actual.events[GBN_EVENT_SIO].active);
	printf("PASS disconnected SIO: %u mode/register/deadline/IRQ comparisons; pull-up data, absent peers, external clocks, cancel and UART refusal\n", cases);
}

static struct GbnSave test_save;
static uint8_t save_data[GBN_SAVE_MAX_SIZE];

static void save_prepare(enum GbnSaveType type, bool wrap) {
	timer_prepare(wrap ? 0xfffff000 : 0);
	memset(save_data, 255, sizeof(save_data));
	CHECK(gbn_attach_save(&actual, &test_save, save_data, sizeof(save_data), type));
	enum GBASavedataType old_type = type == GBN_SAVE_EEPROM ? GBA_SAVEDATA_EEPROM512 :
		type == GBN_SAVE_SRAM ? GBA_SAVEDATA_SRAM : type == GBN_SAVE_FLASH64 ? GBA_SAVEDATA_FLASH512 : GBA_SAVEDATA_FLASH1M;
	GBASavedataForceType(&gba->memory.savedata, old_type);
	GBASavedataReset(&gba->memory.savedata);
	memset(gba->memory.savedata.data, 255, type == GBN_SAVE_EEPROM ? 8192 : test_save.size);
	gba->memory.savedata.currentBank = gba->memory.savedata.data;
	gba->memory.savedata.settling = 0;
	gba->memory.savedata.dirty = 0;
	gba->cpuBlocked = false; GBADMAReset(gba);
	GBAIOWrite(gba, 0x204, 0);
}

static void save_byte(uint32_t offset, uint8_t value) {
	uint32_t cycles;
	CHECK(gbn_write(&actual, 0x0e000000 + offset, 1, value, &cycles) == GBN_STEP);
	GBAStore8(reference, 0x0e000000 + offset, (int8_t) value, NULL);
}

static uint32_t save_read(uint32_t address, unsigned width) {
	struct GbnAccess access;
	CHECK(gbn_read(&actual, address, width, &access) == GBN_STEP);
	return access.value;
}

static void flash_command(unsigned command) {
	save_byte(0x5555, 0xaa); save_byte(0x2aaa, 0x55); save_byte(0x5555, (uint8_t) command);
}

static void eeprom_command(bool read, unsigned block, unsigned bits, const uint8_t* data) {
	uint16_t command[81];
	unsigned count = 0;
	command[count++] = 1; command[count++] = read;
	for (unsigned bit = bits; bit; --bit) command[count++] = (uint16_t) (block >> (bit - 1) & 1);
	if (!read) for (unsigned bit = 0; bit < 64; ++bit) command[count++] = data[bit / 8] >> (7 - (bit & 7)) & 1;
	command[count++] = 0;
	for (unsigned i = 0; i < count; ++i) put16(0x03002000 + 2 * i, command[i]);
	dma_configure(3, 0x03002000, 0x0d000000, count, 0x8000);
	dma_events(actual.now + 3);
}

static void check_saves(void) {
	unsigned bus = 0, eeprom = 0, flash = 0;
	CHECK(gbn_detect_save((const uint8_t*) "EEPROM_V124", 11) == GBN_SAVE_EEPROM);
	CHECK(gbn_detect_save((const uint8_t*) "FLASH1M_V102", 12) == GBN_SAVE_FLASH128);
	CHECK(gbn_detect_save((const uint8_t*) "FLASH512_V131", 13) == GBN_SAVE_FLASH64);
	CHECK(gbn_detect_save((const uint8_t*) "SRAM_F_V100", 11) == GBN_SAVE_SRAM);
	CHECK(gbn_detect_save((const uint8_t*) "EEPROM_", 7) == GBN_SAVE_NONE);
	static const unsigned waits[] = {4, 3, 2, 8};
	for (unsigned wait = 0; wait < 4; ++wait) {
		save_prepare(GBN_SAVE_SRAM, false);
		gbn_set_waitcnt(&actual, (uint16_t) wait); GBAIOWrite(gba, 0x204, (uint16_t) wait);
		for (unsigned trial = 0; trial < 128; ++trial) for (unsigned width = 1; width <= 4; width *= 2) {
			uint32_t address = (trial & 4 ? 0x0f000000 : 0x0e000000) + ((trial * 8191) & 0xffff);
			uint32_t value = random_word(), cycles;
			CHECK(gbn_write(&actual, address, width, value, &cycles) == GBN_STEP);
			if (width == 1) GBAStore8(reference, address, (int8_t) value, NULL);
			else if (width == 2) GBAStore16(reference, address, (int16_t) value, NULL);
			else GBAStore32(reference, address, (int32_t) value, NULL);
			struct GbnAccess access;
			CHECK(gbn_read(&actual, address, width, &access) == GBN_STEP && access.cycles == 1 + waits[wait]);
			uint32_t expected = width == 1 ? GBALoad8(reference, address, NULL) : width == 2 ?
				GBALoad16(reference, address, NULL) : GBALoad32(reference, address, NULL);
			if (access.value != expected) {
				fprintf(stderr, "SRAM wait=%u trial=%u width=%u address=%08x write=%08x value=%08x/%08x\n",
					wait,trial,width,address,value,access.value,expected); exit(1);
			}
			CHECK(!memcmp(save_data, gba->memory.savedata.data, test_save.size));
			++bus;
		}
	}
	for (unsigned bits = 6; bits <= 14; bits += 8) for (unsigned wrap = 0; wrap < 2; ++wrap) {
		for (unsigned trial = 0; trial < 16; ++trial) {
			save_prepare(GBN_SAVE_EEPROM, wrap != 0);
			unsigned block = bits == 6 ? trial * 13 & 63 : (trial * 97 + 0x201) & 0x3ff;
			uint8_t data[8]; for (unsigned i = 0; i < 8; ++i) data[i] = (uint8_t) random_word();
			eeprom_command(false, block, bits, data);
			CHECK(test_save.size == (bits == 6 ? 512 : 8192) && test_save.dirty && test_save.busy);
			CHECK(!memcmp(save_data + block * 8, data, 8));
			CHECK(!memcmp(save_data, gba->memory.savedata.data, test_save.size));
			CHECK(save_read(0x0d000000, 2) == 0 && GBASavedataReadEEPROM(&gba->memory.savedata) == 0);
			timer_tick(test_save.busy_until + 1);
			CHECK(save_read(0x0d000000, 2) == 1 && GBASavedataReadEEPROM(&gba->memory.savedata) == 1);
			eeprom_command(true, block, bits, NULL);
			dma_configure(3, 0x0d000000, 0x03003000, 68, 0x8000);
			dma_events(actual.now + 3);
			CHECK(!memcmp(iwram + 0x3000, (uint8_t*) gba->memory.iwram + 0x3000, 136));
			for (unsigned i = 0; i < 68; ++i) {
				uint16_t value = (uint16_t) save_read(0x03003000 + 2 * i, 2);
				CHECK(value == (i < 4 ? 0 : data[(i - 4) / 8] >> (7 - ((i - 4) & 7)) & 1));
			}
			CHECK(test_save.phase == 0 && save_read(0x0d000000, 2) == 1);
			++eeprom;
		}
	}
	for (unsigned large = 0; large < 2; ++large) for (unsigned wrap = 0; wrap < 2; ++wrap) {
		save_prepare(large ? GBN_SAVE_FLASH128 : GBN_SAVE_FLASH64, wrap != 0);
		flash_command(0x90);
		CHECK(save_read(0x0e000000, 2) == GBALoad16(reference, 0x0e000000, NULL));
		CHECK(save_read(0x0e000001, 1) == GBALoad8(reference, 0x0e000001, NULL));
		flash_command(0xf0);
		for (unsigned bank = 0; bank <= large; ++bank) {
			flash_command(0xb0); save_byte(0, (uint8_t) bank);
			for (unsigned i = 0; i < 32; ++i) {
				unsigned offset = i * 109 + 0x100;
				flash_command(0xa0); save_byte(offset, (uint8_t) random_word());
				CHECK(test_save.busy && actual.events[GBN_EVENT_SAVE].when == test_save.busy_until);
				timer_tick(test_save.busy_until + 1);
				uint32_t ours = save_read(0x0e000000 + offset, 1), theirs = GBALoad8(reference, 0x0e000000 + offset, NULL);
				if (ours != theirs) {
					fprintf(stderr, "Flash large=%u bank=%u i=%u wrap=%u value=%02x/%02x command=%02x/%02x time=%u deadline=%u/%u\n",
						large, bank, i, wrap, ours, theirs, test_save.flash_command, gba->memory.savedata.command,
						actual.now, test_save.busy_until, gba->memory.savedata.dust.when); exit(1);
				}
				CHECK(!memcmp(save_data, gba->memory.savedata.data, test_save.size)); ++flash;
			}
			flash_command(0x80); save_byte(0x5555, 0xaa); save_byte(0x2aaa, 0x55); save_byte(0x300, 0x30);
			timer_tick(test_save.busy_until + 1);
			CHECK(!memcmp(save_data, gba->memory.savedata.data, test_save.size)); ++flash;
		}
		flash_command(0x80); save_byte(0x5555, 0xaa); save_byte(0x2aaa, 0x55); save_byte(0x5555, 0x10);
		timer_tick(test_save.busy_until + 1);
		CHECK(!memcmp(save_data, gba->memory.savedata.data, test_save.size)); ++flash;
	}
	/* A programmed zero bit cannot be set back to one without an erase. */
	flash_command(0xa0); save_byte(0x100, 0x12); timer_tick(test_save.busy_until + 1);
	flash_command(0xa0); save_byte(0x100, 0xff); timer_tick(test_save.busy_until + 1);
	CHECK(save_read(0x0e000100, 1) == 0x12);
	save_prepare(GBN_SAVE_EEPROM, false);
	actual.rom_size = GBN_ROM_MAX_SIZE;
	CHECK(!gbn_save_mapped(&actual, 0x0d000000, 2) && gbn_save_mapped(&actual, 0x0dffff00, 2));
	CHECK(!gbn_save_mapped(&actual, 0x0dffff00, 4));
	CHECK(!gbn_attach_save(&actual, &test_save, save_data, 511, GBN_SAVE_EEPROM));
	printf("PASS cartridge saves: %u SRAM bus, %u EEPROM DMA write/read/busy and %u Flash program/bank/erase comparisons; signatures, bit clearing, bounds and wraparound\n", bus,eeprom,flash);
}

static unsigned run_bios_service(uint32_t return_pc) {
	unsigned count = 0;
	do {
		CHECK(++count < 10000);
		CHECK(gbn_service_events(&actual) == GBN_STEP);
		if (devices.halted) continue;
		CHECK(gbn_step(&actual) == GBN_STEP);
	} while (actual.cpu.pc != return_pc || (actual.cpu.cpsr & 31) != 31);
	return count;
}

static void check_waits(void) {
	unsigned cases = 0;
	for (unsigned wrap = 0; wrap < 2; ++wrap) for (unsigned ime = 0; ime < 2; ++ime) {
		for (unsigned masked = 0; masked < 2; ++masked) {
			timer_prepare(wrap ? 0xfffffff0 : 0);
			gbn_use_builtin_bios(&actual); actual.cpu.cpsr = 0x1f | masked * 0x80;
			CHECK(gbn_enter_arm(&actual, 0) == GBN_STEP);
			device_write(0x300, 1, 1); device_write(0x200, 2, 8); device_write(0x208, 2, ime);
			device_write(0x301, 1, 0);
			CHECK(devices.halted && device_read(0x300, 2) == 1);
			struct GbnCpu before = actual.cpu;
			CHECK(gbn_step(&actual) == GBN_EVENT && !memcmp(&before, &actual.cpu, sizeof(before)));
			/* Caller-owned events remain visible while HALT advances time. */
			CHECK(gbn_schedule(&actual, 0, 3, 0));
			CHECK(gbn_service_events(&actual) == GBN_EVENT && devices.halted && actual.events[0].active);
			gbn_cancel(&actual, 0);
			device_write(0x100, 4, 0x00c0fffc);
			uint32_t edge = actual.events[GBN_EVENT_TIMER0].when;
			CHECK(gbn_service_events(&actual) == GBN_STEP && devices.halted && actual.now == edge);
			for (unsigned guard = 0; devices.halted; ++guard) {
				CHECK(guard < 4 && gbn_service_events(&actual) == GBN_STEP);
			}
			if (actual.now != edge + 7 + (ime && !masked ? 2u : 0u)) {
				fprintf(stderr, "HALT wrap=%u ime=%u mask=%u now=%u edge=%u pc=%08x\n", wrap,ime,masked,actual.now,edge,actual.cpu.pc); exit(1);
			}
			CHECK(actual.cpu.pc == (ime && !masked ? 0x18u : before.pc));
			CHECK(device_read(0x202, 2) == 8); ++cases;
		}
	}
	/* Power writes from ROM/RAM are ignored; STOP is an explicit boundary. */
	timer_prepare(0); gbn_use_builtin_bios(&actual); devices.postflag = 1;
	CHECK(gbn_enter_thumb(&actual, 0x03001000) == GBN_STEP);
	device_write(0x301, 1, 0); CHECK(!devices.halted);
	CHECK(gbn_enter_arm(&actual, 0) == GBN_STEP);
	device_write(0x301, 1, 0x80);
	CHECK(devices.stopped && gbn_service_events(&actual) == GBN_UNSUPPORTED_DEVICE);
	static const uint32_t handler[] = {0xe59f0018, 0xe1d010b2, 0xe1c010b2, 0xe59f2010,
		0xe1d230b0, 0xe1833001, 0xe1c230b0, 0xe12fff1e, 0x04000200, 0x03007ff8};
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned kind = 0; kind < 5; ++kind) {
		for (unsigned wrap = 0; wrap < 2; ++wrap) {
			unsigned service = kind == 0 ? 2 : kind == 4 ? 5 : 4;
			prepare(0, 0, 0x03001000); bind_devices(); gbn_use_builtin_bios(&actual);
			actual.now = wrap ? 0xffff0000 : 0;
			actual.cpu.cpsr = 0xa000001f;
			actual.cpu.r[13] = 0x03007f00; actual.cpu.banks[2].sp = 0x03007fa0; actual.cpu.banks[3].sp = 0x03007fe0;
			actual.cpu.r[0] = kind == 3 ? 1 : 0; actual.cpu.r[1] = kind == 4 ? 0 : 8;
			if (thumb) put16(0x03001000, (uint16_t) (0xdf00 | service));
			else put32(0x03001000, 0xef000000 | service << 16);
			put32(0x03007ffc, 0x03003000);
			for (unsigned i = 0; i < sizeof(handler) / sizeof(*handler); ++i) put32(0x03003000 + 4 * i, handler[i]);
			put16(0x03007ff8, (uint16_t) (2 | (kind == 1 || kind == 3 ? 8 : kind == 4 ? 1 : 0)));
			CHECK((thumb ? gbn_enter_thumb(&actual, 0x03001000) : gbn_enter_arm(&actual, 0x03001000)) == GBN_STEP);
			gbn_video_start(&actual, true); gbn_audio_start(&actual);
			device_write(0x200, 2, kind == 4 ? 1 : 8); device_write(0x208, 2, kind == 0 ? 1 : 0);
			if (kind == 4) device_write(4, 2, 8);
			else if (kind != 1) device_write(0x100, 4, 0x00c0f000);
			struct GbnCpu before = actual.cpu;
			run_bios_service(0x03001000 + (thumb ? 2 : 4));
			CHECK(!devices.halted && actual.cpu.cpsr == before.cpsr && device_read(0x208, 2) == 1);
			for (unsigned r = 2; r < 15; ++r) CHECK(actual.cpu.r[r] == before.r[r]);
			CHECK(save_read(0x03007ff8, 2) == (kind == 0 ? 10u : 2u));
			if (kind == 1) CHECK((uint32_t) (actual.now - (wrap ? 0xffff0000 : 0)) < 4096);
			else CHECK(devices.audio.produced > 1);
			++cases;
		}
	}
	printf("PASS HALT/BIOS waits: %u IME/CPSR/wrap, timer/VBlank wake, discard/preserve flags and ARM/Thumb return cases; external events and explicit STOP refusal\n", cases);
}

static void check_lz77(void) {
	unsigned cases = 0, guest_steps = 0;
	static const unsigned lengths[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 63, 64, 95, 96};
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned half = 0; half < 2; ++half) {
		for (unsigned rom = 0; rom < 2; ++rom) for (unsigned trial = 0; trial < 16; ++trial) {
			uint8_t stream[256] = {0}, expected[96] = {0};
			unsigned length = lengths[trial], size = 4, produced = 0;
			stream[0] = 0x10; stream[1] = (uint8_t) length;
			while (produced < length) {
				unsigned flags = size++;
				for (unsigned bit = 0; bit < 8 && produced < length; ++bit) {
					unsigned left = length - produced;
					if (produced >= 2 && left >= 3 && (random_word() & 3)) {
						unsigned distance = 2 + random_word() % (produced - 1);
						unsigned count = 3 + random_word() % (left < 18 ? left - 2 : 16);
						stream[flags] |= (uint8_t) (0x80 >> bit);
						stream[size++] = (uint8_t) ((count - 3) << 4 | (distance - 1) >> 8);
						stream[size++] = (uint8_t) (distance - 1);
						for (unsigned i = 0; i < count; ++i, ++produced) expected[produced] = expected[produced - distance];
					} else { expected[produced++] = stream[size++] = (uint8_t) random_word(); }
				}
			}
			unsigned service = half ? 0x12 : 0x11;
			prepare(0, 0, 0x03001000); bind_devices(); gbn_use_builtin_bios(&actual);
			actual.cpu.cpsr = 0x9000001f;
			actual.cpu.r[13] = 0x03007f00; actual.cpu.banks[3].sp = 0x03007fe0;
			actual.cpu.r[0] = rom ? 0x08000400 : 0x02001000;
			actual.cpu.r[1] = half ? 0x06000000 : trial & 1 ? 0x03002001 : 0x02002000;
			if (rom) {
				memcpy(test_rom + 0x400, stream, size);
				CHECK(gbn_attach_rom(&actual, test_rom, sizeof(test_rom)));
				gba->memory.rom = (uint32_t*) test_rom; gba->memory.romSize = sizeof(test_rom); gba->memory.romMask = sizeof(test_rom) - 1;
			} else for (unsigned i = 0; i < size; ++i) {
				uint32_t cycles; CHECK(gbn_write(&actual, 0x02001000 + i, 1, stream[i], &cycles) == GBN_STEP);
				GBAStore8(reference, 0x02001000 + i, (int8_t) stream[i], NULL);
			}
			uint32_t dest = actual.cpu.r[1], source = actual.cpu.r[0];
			for (unsigned i = 0; i < 128; i += 2) put16((dest & ~1u) + i, 0xb1b1);
			if (thumb) put16(0x03001000, (uint16_t) (0xdf00 | service));
			else put32(0x03001000, 0xef000000 | service << 16);
			enter_mode(0x03001000, thumb != 0);
			struct GbnCpu before = actual.cpu;
			mTimingClear(&gba->timing);
			guest_steps += run_bios_service(0x03001000 + (thumb ? 2 : 4));
			unsigned guard = 0;
			do { CHECK(++guard < 10000); ARMRun(reference); }
			while ((uint32_t) reference->gprs[15] != 0x03001000 + (thumb ? 4u : 8u) || reference->privilegeMode != MODE_SYSTEM);
			CHECK(actual.cpu.cpsr == before.cpsr && actual.cpu.r[0] == source + size && actual.cpu.r[1] == dest + length && actual.cpu.r[3] == 0);
			for (unsigned r = 0; r < 15; ++r) CHECK(actual.cpu.r[r] == (uint32_t) reference->gprs[r]);
			unsigned stored = half ? length & ~1u : length;
			for (unsigned i = 0; i < 100; ++i) {
				uint32_t value = save_read(dest + i, 1);
				CHECK(value == (i < stored ? expected[i] : 0xb1));
				CHECK(value == GBALoad8(reference, dest + i, NULL));
			}
			++cases;
		}
	}
	gba->memory.rom = NULL; gba->memory.romSize = gba->memory.romMask = 0;
	printf("PASS independent LZ77 BIOS: %u byte/halfword, RAM/ROM, literal/overlap, odd-length and ARM/Thumb result/ABI comparisons (%u guest instructions)\n", cases, guest_steps);
}

static void check_builtin_bios(void) {
	unsigned cases = 0, guest_steps = 0;
	uint32_t* old_bios = gba->memory.bios;
	gba->memory.fullBios = 0;
	gba->memory.rom = NULL;
	static const unsigned lengths[] = {0, 1, 2, 7, 8, 9, 16, 31};
	for (unsigned thumb = 0; thumb < 2; ++thumb) for (unsigned fast = 0; fast < 2; ++fast) {
		for (unsigned fill = 0; fill < 2; ++fill) for (unsigned word = 0; word < 2; ++word) {
			for (unsigned trial = 0; trial < 8; ++trial) {
				unsigned service = fast ? 0xc : 0xb;
				uint32_t pc = 0x03001000, op = thumb ? 0xdf00 | service : 0xef000000 | service << 16;
				prepare(0, 0, pc); bind_devices();
				gbn_use_builtin_bios(&actual);
				if (thumb) put16(pc, (uint16_t) op); else put32(pc, op);
				for (unsigned r = 0; r < 15; ++r) actual.cpu.r[r] = 0x12345600 + r;
				actual.cpu.cpsr = (trial & 15) << 28 | 0x1f;
				actual.cpu.r[13] = 0x03007f00; actual.cpu.banks[2].sp = 0x03007fa0; actual.cpu.banks[3].sp = 0x03007fe0;
				actual.cpu.r[0] = 0x02002000;
				/* Include overlapping copies across an eight-word boundary. */
				actual.cpu.r[1] = trial & 1 ? 0x02002004 : trial & 2 ? 0x06000000 : 0x03002000;
				actual.cpu.r[2] = lengths[trial] | fill << 24 | word << 26;
				for (unsigned i = 0; i < 256; i += 4) {
					put32(0x02002000 + i, random_word());
					put32(0x03002000 + i, random_word());
					put32(0x06000000 + i, random_word());
				}
				enter_mode(pc, thumb != 0);
				mTimingClear(&gba->timing);
				guest_steps += run_bios_service(pc + (thumb ? 2 : 4));
				unsigned guard = 0;
				do {
					CHECK(++guard < 10000); ARMRun(reference);
				} while ((uint32_t) reference->gprs[15] != pc + (thumb ? 4 : 8) || reference->privilegeMode != MODE_SYSTEM);
				CHECK(actual.cpu.cpsr == (uint32_t) reference->cpsr.packed);
				for (unsigned r = 0; r < 15; ++r) {
					if (actual.cpu.r[r] != (uint32_t) reference->gprs[r]) {
						fprintf(stderr, "BIOS result service=%x thumb=%u trial=%u word=%u fill=%u r%u=%08x/%08x\n",
							service,thumb,trial,word,fill,r,actual.cpu.r[r],reference->gprs[r]);
						exit(1);
					}
				}
				CHECK(!memcmp(ewram + 0x2000, (uint8_t*) gba->memory.wram + 0x2000, 256));
				CHECK(!memcmp(iwram + 0x2000, (uint8_t*) gba->memory.iwram + 0x2000, 256));
				CHECK(!memcmp(vram, gba->video.vram, 256));
				++cases;
			}
		}
	}
	/* Normal BIOS IRQ dispatch calls the guest handler pointer at 03007FFC,
	 * preserves its saved registers and returns to the interrupted state. */
	for (unsigned thumb = 0; thumb < 2; ++thumb) {
		prepare(0x2001, 0, 0x03001000); bind_devices(); gbn_use_builtin_bios(&actual);
		actual.cpu.cpsr = 0x6000001f;
		actual.cpu.r[13] = 0x03007f00; actual.cpu.banks[2].sp = 0x03007fa0;
		put32(0x03007ffc, 0x03003000);
		put32(0x03003000, 0xe2844001); /* add r4,r4,#1 */
		put32(0x03003004, 0xe12fff1e); /* bx lr */
		enter_mode(0x03001000, thumb != 0);
		struct GbnCpu before = actual.cpu;
		CHECK(gbn_raise_irq(&actual) == GBN_STEP);
		guest_steps += run_bios_service(before.pc);
		CHECK(actual.cpu.cpsr == before.cpsr && actual.cpu.r[4] == before.r[4] + 1);
		for (unsigned r = 0; r < 15; ++r) if (r != 4) CHECK(actual.cpu.r[r] == before.r[r]);
	}
	/* An IRQ pending at SWI entry must be serviced after the BIOS restores
	 * the caller's interrupt mask, then return into the copying routine. */
	prepare(0xdf0b, 0, 0x03001000); bind_devices(); gbn_use_builtin_bios(&actual);
	actual.cpu.r[0] = 0x02002000; actual.cpu.r[1] = 0x03002000; actual.cpu.r[2] = 0x04000020;
	actual.cpu.r[13] = 0x03007f00; actual.cpu.banks[2].sp = 0x03007fa0; actual.cpu.banks[3].sp = 0x03007fe0;
	put32(0x03007ffc, 0x03003000);
	/* Count the interrupt and acknowledge IF before returning to firmware. */
	static const uint32_t handler[] = {0xe59f0018, 0xe5901000, 0xe2811001, 0xe5801000,
		0xe59f000c, 0xe3a01001, 0xe1c010b2, 0xe12fff1e, 0x03000400, 0x04000200};
	for (unsigned i = 0; i < sizeof(handler) / sizeof(*handler); ++i) put32(0x03003000 + 4 * i, handler[i]);
	put32(0x03000400, 0);
	CHECK(gbn_enter_thumb(&actual, 0x03001000) == GBN_STEP);
	device_write(0x200, 2, 1); device_write(0x208, 2, 1); gbn_request_irq(&actual, 1);
	guest_steps += run_bios_service(0x03001002);
	struct GbnAccess access;
	CHECK(gbn_read(&actual, 0x03000400, 4, &access) == GBN_STEP && access.value == 1);
	CHECK(device_read(0x202, 2) == 0);
	CHECK(!memcmp(ewram + 0x2000, iwram + 0x2000, 128));

	prepare(0xdf13, 0, 0x03001000); bind_devices(); gbn_use_builtin_bios(&actual);
	actual.cpu.r[13] = 0x03007f00; actual.cpu.banks[3].sp = 0x03007fe0;
	CHECK(gbn_enter_thumb(&actual, 0x03001000) == GBN_STEP);
	enum GbnStatus status = GBN_STEP;
	for (unsigned i = 0; i < 64 && status == GBN_STEP; ++i) status = gbn_step(&actual);
	CHECK(status == GBN_UNSUPPORTED_DEVICE && actual.cpu.pc == 0x20 && actual.cpu.r[12] == 0x13);
	gba->memory.bios = old_bios; gba->memory.fullBios = 0;
	printf("PASS independent BIOS: %u CpuSet/CpuFastSet result comparisons, IRQ round trips and explicit missing-service trap (%u guest instructions)\n", cases, guest_steps);
}

#include "gba-next-dma.h"

int main(void) {
	struct mLogger logger = {.log = quiet};
	mLogSetDefaultLogger(&logger);
	struct mCore* core = GBACoreCreate();
	CHECK(core && core->init(core));
	mCoreInitConfig(core, NULL);
	reference = core->cpu;
	gba = core->board;
	gba->idleOptimization = IDLE_LOOP_IGNORE;
	gba->memory.prefetch = false;
	reference->irqh.processEvents = no_events;
	reference->irqh.readCPSR = no_psr_event;
	for (unsigned i = 0; i < sizeof(ewram); ++i) ewram[i] = (uint8_t) random_word();
	for (unsigned i = 0; i < sizeof(iwram); ++i) iwram[i] = (uint8_t) random_word();
	memcpy(gba->memory.wram, ewram, sizeof(ewram));
	memcpy(gba->memory.iwram, iwram, sizeof(iwram));
	check_bus();
	check_batch();
	check_alu();
	check_memory();
	check_branches();
	check_events();
	check_event_oracle();
	check_streams();
	check_thumb_multiple();
	check_arm_alu();
	check_arm_multiple();
	check_arm_multiply();
	check_arm_memory();
	check_interworking();
	check_psr();
	check_self_modify();
	check_refusals();
	check_rom_bios();
	check_video_bus();
	check_device_registers();
	check_video_events();
	check_dma();
	check_dma_spans();
	check_builtin_bios();
	check_timers();
	check_audio();
	check_sio();
	check_saves();
	check_waits();
	check_lz77();
	mCoreConfigDeinit(&core->config);
	core->deinit(core);
	printf("PASS independent GBA core: %u CPU differential comparisons\n", comparisons);
	return 0;
}
