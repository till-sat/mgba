/* SPDX-License-Identifier: MPL-2.0 */
/* Execute emitted machine code on RV32, compare against the independently
 * validated interpreter. No mGBA linkage and no emulation of generated code. */
#include <gba-next/rv32.h>
#include <am-counters.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !GBN_RV32_STATS
#error This diagnostic requires GBN_RV32_STATS=1
#endif
#define CHECK(x) do { if (!(x)) { printf("FAIL RV32 line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct Gbn machine, reference;
static struct GbnRv32 backend;
static struct GbnDevices devices;
_Alignas(4) static uint8_t rom[4096], alternate[4096];
_Alignas(4) static uint8_t ewram[GBN_EWRAM_SIZE + 4], iwram[GBN_IWRAM_SIZE + 4];
static uint8_t palette[GBN_PALETTE_SIZE], vram[GBN_VRAM_SIZE], oam[GBN_OAM_SIZE];
static uint32_t random_state = 0x8124acf7;
static unsigned comparisons;
static uint32_t random_word(void) {
	random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5;
	return random_state;
}
static void put16(unsigned offset, unsigned code) { rom[offset] = (uint8_t) code; rom[offset + 1] = (uint8_t) (code >> 8); }
static void put32(unsigned offset, uint32_t code) { put16(offset, code); put16(offset + 2, code >> 16); }
static void setup(void) {
	gbn_init(&machine, ewram, iwram);
	for (unsigned i = 0; i < sizeof(rom); i += 2) put16(i, 0xbe00);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	gbn_rv32_init(&backend, &machine);
}
static void compare_memory(uint32_t cap, unsigned tag, uint8_t* changed) {
	reference = machine;
	uint8_t before[4], after[4];
	if (changed) memcpy(before, changed, 4);
	uint32_t want = 0, got = 0;
	enum GbnStatus a = gbn_run_batch(&reference, cap, &want);
	if (changed) { memcpy(after, changed, 4); memcpy(changed, before, 4); }
	enum GbnStatus b = gbn_rv32_run_batch(&backend, cap, &got);
	if (a != b || want != got || memcmp(&machine, &reference, sizeof(machine)) || (changed && memcmp(changed, after, 4))) {
		printf("FAIL native differential tag=%08x cap=%" PRIu32 " count=%" PRIu32 "/%" PRIu32
			" status=%u/%u PC=%08" PRIx32 "/%08" PRIx32 " flags=%08" PRIx32 "/%08" PRIx32
			" cycles=%" PRIu32 "/%" PRIu32 "\n", tag, cap, got, want, b, a,
			machine.cpu.pc, reference.cpu.pc, machine.cpu.cpsr, reference.cpu.cpsr, machine.now, reference.now);
		for (unsigned r = 0; r < 15; ++r) if (machine.cpu.r[r] != reference.cpu.r[r])
			printf("r%u=%08" PRIx32 "/%08" PRIx32 "\n", r, machine.cpu.r[r], reference.cpu.r[r]);
		exit(1);
	}
	++comparisons;
}
static void compare(uint32_t cap, unsigned tag) { compare_memory(cap, tag, NULL); }
static void encodings(void) {
	static const uint32_t edges[] = {0, 1, 31, 32, 0x7fffffff, 0x80000000, 0xffffffff, 0xaaaaaaaa};
	setup();
	for (unsigned code = 0; code < 65536; ++code) {
		/* ALU/ADR/SP/direct-branch encodings, including deliberately unsupported
		 * register shifts and writes to PC to exercise interpreter fallback. */
		if (!(code < 0x4700 || (code >= 0xa000 && code < 0xb100) ||
		      (code >= 0xd000 && code < 0xde00) || (code >= 0xe000 && code < 0xe800))) continue;
		put16(0, code);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned trial = 0; trial < 4; ++trial) {
			for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = trial < 2 ? edges[(r + trial) & 7] : random_word();
			machine.cpu.cpsr = (random_word() & ~31u) | 31;
			machine.cpu.shifter_carry = trial & 1;
			machine.now = trial & 1 ? 0xfffffff0 : 0;
			gbn_set_waitcnt(&machine, (uint16_t) random_word());
			CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
			compare(1, code);
		}
	}
	CHECK(backend.native_instructions > 50000 && backend.fallback_instructions > 0);
	CHECK(backend.compiled_blocks > GBN_RV32_SLOTS && backend.code_used <= GBN_RV32_CODE_WORDS);
	printf("PASS native Thumb encodings: %u comparisons; native=%" PRIu64 "; fallback=%" PRIu64 "\n",
		comparisons, backend.native_instructions, backend.fallback_instructions);
}
static void streams(void) {
	static const uint16_t program[] = {0x2001, 0x2103, 0x3007, 0x1842, 0x405a, 0x1e52,
		0x2a01, 0x43d3, 0x005b, 0x40cb, 0xa501, 0x46ac, 0x4465, 0xb081, 0xb001, 0xe7ef};
	static const uint32_t caps[] = {1, 2, 7, 15, 16, 17, 31, 32, 33, 127, 256};
	setup();
	for (unsigned i = 0; i < sizeof(program) / sizeof(*program); ++i) put16(2 * i, program[i]);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	unsigned before = comparisons;
	for (unsigned region = 8; region <= 12; region += 2) for (unsigned distance = 0; distance < 85; ++distance)
	for (unsigned k = 0; k < sizeof(caps) / sizeof(*caps); ++k) {
		machine.next_event = -1;
		memset(machine.events, 0, sizeof(machine.events));
		machine.now = distance & 1 ? 0xfffffff0 : 0;
		for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
		machine.cpu.cpsr = (random_word() & ~31u) | 31;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_thumb(&machine, region << 24) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 1));
		compare(caps[k], 0x10000 | distance);
	}
	/* Every condition and NZCV combination, taken and untaken, with caps
	 * before/after the branch; no dependency on a particular game. */
	machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
	for (unsigned cond = 0; cond < 14; ++cond) {
		put16(0, 0xd000 | cond << 8); put16(2, 0x2002); put16(4, 0x2004);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned flags = 0; flags < 16; ++flags) for (unsigned cap = 1; cap < 5; ++cap) {
			machine.cpu.cpsr = flags << 28 | 0x3f;
			CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
			compare(cap, 0x20000 | cond << 4 | flags);
		}
	}
	printf("PASS native mixed blocks, event deadlines, clock wrap, WAITCNT, caps and branches: %u comparisons\n", comparisons - before);
}
static void invalidation(void) {
	setup(); put16(0, 0x2001); put16(2, 0x3001); put16(4, 0xe7fc);
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(33, 0x30000);
	/* Replace the same allocation: cached machine code must be regenerated. */
	put16(0, 0x2007); put16(2, 0x3003);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(33, 0x30001);
	memcpy(alternate, rom, sizeof(rom)); alternate[0] = 42;
	CHECK(gbn_attach_rom(&machine, alternate, sizeof(alternate)));
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(33, 0x30002);
	/* A stale prefetched opcode is executed from the pipeline, never replaced
	 * by the ROM bytes merely because a native block exists for this PC. */
	machine.cpu.pipe[0] = 0x2031; compare(1, 0x30003);
	/* Instruction fetch wraps within a non-power-of-two cartridge mapping. */
	put16(58, 0x2001); put16(60, 0x3001);
	CHECK(gbn_attach_rom(&machine, rom, 63));
	CHECK(gbn_enter_thumb(&machine, 0x0800003a) == GBN_STEP); compare(5, 0x30004);
	/* Stale prefetched RAM code stays in the interpreter until consumed. */
	iwram[0] = 1; iwram[1] = 0x20; iwram[2] = 1; iwram[3] = 0x30;
	CHECK(gbn_enter_thumb(&machine, 0x03000000) == GBN_STEP);
	iwram[0] = 99; iwram[2] = 77;
	uint64_t previous = backend.native_instructions;
	compare(2, 0x30005); CHECK(backend.native_instructions == previous);
	/* Device ownership and invalid CPU modes must stop before a cached block. */
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
	machine.devices = &devices;
	devices.halted = true; compare(32, 0x30006); devices.halted = false;
	devices.dma_blocked = true; compare(32, 0x30007); devices.dma_blocked = false;
	devices.stopped = true; compare(32, 0x30008); devices.stopped = false;
	machine.cpu.cpsr = 0x20; compare(32, 0x30009);
	machine.devices = NULL;
	puts("PASS native ROM replacement, pipeline preservation, RAM fallback and blocked CPU");
}
static void memory_paths(void) {
	setup();
	unsigned before = comparisons;
	for (unsigned i = 0; i < sizeof(ewram); ++i) ewram[i] = (uint8_t) random_word();
	for (unsigned i = 0; i < sizeof(iwram); ++i) iwram[i] = (uint8_t) random_word();
	for (unsigned kind = 0; kind < 8; ++kind) {
		/* [r1+r2], r0: test all load/store widths and signs. */
		unsigned code = 0x5000 | kind << 9 | 2 << 6 | 1 << 3;
		put16(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned location = 0; location < 2; ++location) for (unsigned wait = 0; wait < 16; ++wait)
		for (unsigned offset = 0; offset < 8; ++offset) for (unsigned prefetch = 0; prefetch < 2; ++prefetch) {
			uint32_t address = (location ? 0x03fffff8u : 0x02fffff8u) + offset;
			machine.ewram_wait = (uint8_t) wait;
			machine.cpu.r[0] = random_word(); machine.cpu.r[1] = address - 3; machine.cpu.r[2] = 3;
			machine.cpu.cpsr = (random_word() & ~31u) | 31;
			gbn_set_waitcnt(&machine, (uint16_t) ((prefetch ? 0x4000 : 0) | (wait << 2)));
			CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
			machine.prefetched_pc = machine.cpu.pc + 4 + 2 * (offset % 8);
			uint8_t* bytes = location ? iwram + (address & (GBN_IWRAM_SIZE - 4)) : ewram + (address & (GBN_EWRAM_SIZE - 4));
			compare_memory(1, 0x50000 | code, bytes);
		}
	}
	CHECK(backend.native_instructions > 0 && backend.fallback_instructions > 0);
	/* All immediate/SP/literal load-store encodings. Their effective address
	 * is either a word in the test data window or read-only ROM. */
	for (unsigned code = 0x4800; code < 0xa000; ++code) {
		if (code >= 0x5000 && code < 0x6000) continue;
		put16(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		unsigned rd = code & 7, rs = code >> 3 & 7;
		for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = 0x03000100;
		unsigned offset = code < 0x8000 ? (code >> 6 & 31) * (code & 0x1000 ? 1 : 4) : (code >> 6 & 31) * 2;
		if (code >= 0x9000) { offset = (code & 255) * 4; rs = 13; rd = code >> 8 & 7; }
		if (rd != rs) machine.cpu.r[rd] = random_word();
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		compare_memory(1, 0x60000 | code, code < 0x5000 ? NULL : iwram + ((0x100 + offset) & (GBN_IWRAM_SIZE - 4)));
	}
	/* Continuous read/write blocks still stop at instruction boundaries.
	 * Exercise every event cut, unaligned fallback, and immediate reload. */
	static const unsigned program[] = {0x3001, 0x6008, 0x680a, 0x4050, 0xe7fa};
	for (unsigned i = 0; i < 5; ++i) put16(2 * i, program[i]);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned distance = 0; distance < 80; ++distance) for (unsigned cap = 1; cap <= 32; ++cap) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		machine.now = 0xfffffff0;
		machine.cpu.r[0] = random_word(); machine.cpu.r[1] = 0x02000100 | (distance & 3);
		gbn_set_waitcnt(&machine, (uint16_t) (distance & 1 ? 0x4000 : 0));
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0));
		compare_memory(cap, 0x70000 | distance, ewram + 0x100);
	}
	machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
	put16(0, 0x6008); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	/* NULL / misaligned host buffers and unmapped or IO guest addresses must
	 * take the regular interpreter path without a host fault or extra access. */
	machine.cpu.r[1] = 0x02000100; machine.ewram = NULL;
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(1, 0x80000);
	machine.ewram = ewram + 1;
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare_memory(1, 0x80001, ewram + 0x101);
	machine.ewram = ewram;
	machine.cpu.r[1] = 0x04000204;
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(1, 0x80002);
	machine.cpu.r[1] = 0x08000200;
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(1, 0x80003);
	printf("PASS native RAM/literal accesses: %u comparisons, all widths/signs/offsets, waits/prefetch, mirrors and event cuts\n", comparisons - before);
}
static void resident_blocks(void) {
	setup();
	unsigned before = comparisons;
	/* Long memory-heavy blocks exercise code-capacity splits, event exits
	 * after committed stores, and a cap spanning more than one compiled block.
	 * All stores target one word so both engines start with identical RAM. */
	for (unsigned i = 0; i < 48; ++i) {
		static const unsigned code[] = {0x3001, 0x6008, 0x680a, 0x4050};
		put16(2 * i, code[i & 3]);
	}
	put16(96, 0xe7ce);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	static const unsigned caps[] = {1, 2, 3, 7, 16, 31, 32, 33, 48, 49, 50, 64, 127};
	static const unsigned waits[] = {0, 0x001c, 0x00e0, 0x0700, 0x4000, 0x401c, 0x40e0, 0x4700};
	for (unsigned region = 8; region <= 12; region += 2)
	for (unsigned w = 0; w < sizeof(waits) / sizeof(*waits); ++w)
	for (unsigned distance = 0; distance < 120; distance += 3)
	for (unsigned cap = 0; cap < sizeof(caps) / sizeof(*caps); ++cap) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		machine.now = 0xffffffe0;
		machine.cpu.r[0] = random_word(); machine.cpu.r[1] = 0x02000100;
		machine.cpu.cpsr = (random_word() & ~31u) | 31;
		machine.ewram_wait = (uint8_t) (distance & 15);
		gbn_set_waitcnt(&machine, (uint16_t) waits[w]);
		CHECK(gbn_enter_thumb(&machine, region << 24) == GBN_STEP);
		machine.prefetched_pc = machine.cpu.pc + 4 + 2 * (distance & 7);
		CHECK(gbn_schedule(&machine, 0, distance, 0));
		compare_memory(caps[cap], 0x90000 | distance, ewram + 0x100);
	}
	/* A slow access midway through a warm block must commit its native prefix
	 * exactly once and retry only the failing instruction in the interpreter. */
	put16(0, 0x3001); put16(2, 0x6008); put16(4, 0x490e);
	put16(6, 0x680a); put16(8, 0x3002); put16(10, 0xe7f9);
	for (unsigned location = 0; location < 3; ++location) {
		uint32_t address = location == 0 ? 0x04000204 : location == 1 ? 0x02000101 : 0x03000100;
		for (unsigned i = 0; i < 4; ++i) rom[64 + i] = (uint8_t) (address >> (8 * i));
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned distance = 0; distance < 80; ++distance) for (unsigned cap = 1; cap < 12; ++cap) {
			machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
			machine.now = 0xffffffe0; machine.cpu.r[0] = random_word(); machine.cpu.r[1] = 0x02000100;
			gbn_set_waitcnt(&machine, (uint16_t) waits[distance & 7]);
			CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
			CHECK(gbn_schedule(&machine, 0, distance, 0));
			/* Stop before a second iteration could write through the new base. */
			compare_memory(cap > 6 ? 6 : cap, 0xa0000 | distance, ewram + 0x100);
		}
	}
	puts("PASS resident RAM blocks: capacity splits, all ROM wait regions, cached timing changes, prefix exits and clock wrap");
	printf("Resident block state comparisons: %u\n", comparisons - before);
}
static void arm_encodings(void) {
	setup();
	unsigned before = comparisons;
	static const uint32_t edges[] = {0, 1, 0xffffffff, 0x7fffffff, 0x80000000, 31, 32, 255};
	static const unsigned regs[] = {0, 7, 8, 14, 15};
	for (unsigned kind = 0; kind < 16; ++kind) for (unsigned set = 0; set < 2; ++set)
	for (unsigned operand = 0; operand < 176; ++operand) for (unsigned trial = 0; trial < 8; ++trial) {
		unsigned rd = regs[trial % 5], rn = regs[(trial + 2) % 5], rm = regs[(trial + 1) % 5];
		uint32_t rhs = operand < 128 ? (operand >> 5) << 5 | (operand & 31) << 7 | rm :
			operand < 160 ? 0x02000000 | (operand & 15) << 8 | (operand & 16 ? 0xa5 : 1) :
			(operand & 15) << 8 | rm | 0x10;
		unsigned cond = trial < 4 ? 14 : trial == 7 ? 15 : operand & 15;
		uint32_t code = cond << 28 | kind << 21 | set << 20 | rn << 16 | rd << 12 | rhs;
		put32(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = trial & 1 ? random_word() : edges[(r + trial) & 7];
		machine.cpu.cpsr = (random_word() & ~63u) | 31;
		machine.cpu.shifter_carry = trial & 1;
		machine.now = 0xfffffff0;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
		compare(1, code);
	}
	/* Carry-in/borrow edge cases, including a==b and unsigned overflow. */
	for (unsigned kind = 5; kind <= 7; ++kind) for (unsigned a = 0; a < 8; ++a)
	for (unsigned b = 0; b < 8; ++b) for (unsigned c = 0; c < 2; ++c) {
		put32(0, 0xe0100002 | kind << 21 | 1 << 16);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		machine.cpu.r[1] = edges[a]; machine.cpu.r[2] = edges[b]; machine.cpu.cpsr = c << 29 | 0x0f00001f;
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP); compare(1, 0xb0000 | kind);
	}
	CHECK(backend.native_instructions > 10000 && backend.fallback_instructions > 0);
	printf("PASS ARM ALU/operand/carry encodings: %u state comparisons; native=%" PRIu64 "; fallback=%" PRIu64 "\n",
		comparisons - before, backend.native_instructions, backend.fallback_instructions);
}
static void arm_streams(void) {
	setup();
	unsigned before = comparisons;
	static const uint32_t code[] = {0xe2800001, 0xe5810000, 0xe5912000, 0xe0503002,
		0x12803001, 0x03a04002, 0xe2a48000, 0xe1a08068, 0xe0d89008, 0xeafffff5};
	for (unsigned i = 0; i < sizeof(code) / sizeof(*code); ++i) put32(4 * i, code[i]);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	static const unsigned caps[] = {1, 2, 3, 7, 9, 10, 11, 31, 32, 33, 127};
	for (unsigned region = 8; region <= 12; region += 2) for (unsigned distance = 0; distance < 96; ++distance)
	for (unsigned cap = 0; cap < sizeof(caps) / sizeof(*caps); ++cap) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		machine.now = 0xfffffff0; machine.ewram_wait = (uint8_t) (distance & 15);
		for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
		machine.cpu.r[1] = 0x02000100 | (distance & 3);
		machine.cpu.cpsr = (random_word() & ~63u) | 31;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_arm(&machine, region << 24) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0));
		compare_memory(caps[cap], 0xc0000 | distance, ewram + 0x100);
	}
	machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
	for (unsigned cond = 0; cond < 16; ++cond) for (unsigned link = 0; link < 2; ++link) {
		put32(0, cond << 28 | 0x0a000000 | link << 24); put32(4, 0xe3a00002); put32(8, 0xe3a00004);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned flags = 0; flags < 16; ++flags) for (unsigned cap = 1; cap <= 3; ++cap) {
			machine.cpu.cpsr = flags << 28 | 31;
			CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
			compare(cap, 0xd0000 | cond << 4 | flags);
		}
	}
	/* All address-shift encodings on RAM: memory must not latch shifter carry. */
	for (unsigned byte = 0; byte < 2; ++byte) for (unsigned load = 0; load < 2; ++load)
	for (unsigned shift = 0; shift < 128; ++shift) for (unsigned offset = 0; offset < 4; ++offset) {
		put32(0, 0xe7810002 | byte << 22 | load << 20 | (shift >> 5) << 5 | (shift & 31) << 7);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		machine.cpu.r[0] = random_word(); machine.cpu.r[1] = 0x03fffff8 + offset; machine.cpu.r[2] = 0;
		machine.cpu.cpsr = 31; machine.cpu.shifter_carry = 1;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
		compare_memory(1, 0xe0000 | shift, iwram + GBN_IWRAM_SIZE - 8);
	}
	puts("PASS ARM continuous blocks, conditional effects, direct B/BL, RAM shifts, event cuts and clock wrap");
	printf("ARM stream state comparisons: %u\n", comparisons - before);
}
static void cost(void) {
	setup();
	for (unsigned i = 0; i < 7; ++i) put16(2 * i, 0x3001 + ((i & 7) << 8));
	put16(14, 0xe7f7);
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
	compare(64, 0x40000); /* Warm code generation before measuring execution. */
	reference = machine;
	uint32_t a, b;
	uint64_t start = am_counter_retired();
	CHECK(gbn_run_batch(&reference, 100000, &a) == GBN_STEP);
	uint64_t interpreted = am_counter_retired() - start;
	start = am_counter_retired();
	CHECK(gbn_rv32_run_batch(&backend, 100000, &b) == GBN_STEP);
	uint64_t compiled = am_counter_retired() - start;
	CHECK(a == b && !memcmp(&machine, &reference, sizeof(machine)));
	CHECK(!backend.fallback_instructions && backend.native_instructions == 100064);
	CHECK(backend.loop_instructions == 100064 && backend.loop_entries == 2);
	printf("Native ALU microbenchmark (100000 guest instructions, warm cache, diagnostic counters): interpreted=%" PRIu64
		"; native=%" PRIu64 " host instructions; no hardware FPS claim\n", interpreted, compiled);
}
static void interpreter_bridge(void) {
	setup();
	put16(0, 0x3001); put16(2, 0x4708); put16(4, 0x3201); put16(6, 0xe7fb);
	iwram[0] = 1; iwram[1] = 0x34; iwram[2] = 0x18; iwram[3] = 0x47;
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned cap = 1; cap < 40; ++cap) for (unsigned distance = 0; distance < 80; ++distance) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		machine.now = 0xffffffe0; machine.cpu.r[1] = 0x03000001; machine.cpu.r[3] = 0x08000005;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0));
		compare(cap, 0xf0000 | distance);
	}
	CHECK(backend.native_instructions && backend.fallback_kind[2]);
	puts("PASS ROM/RAM interpreter transitions with all event cuts and instruction caps");
}
static void ram_code(void) {
	setup();
	unsigned before = comparisons;
	static const uint16_t thumb[] = {0x3001, 0x6008, 0x680a, 0x4050, 0xe7fa};
	static const uint32_t arm[] = {0xe2800001, 0xe5810000, 0xe5912000, 0xe0503002,
		0x12803001, 0x03a04002, 0xe2a48000, 0xe1a08068, 0xe0d89008, 0xeafffff5};
	static const unsigned caps[] = {1, 2, 3, 4, 5, 9, 10, 11, 16, 31, 32, 33, 127};
	for (unsigned region = 2; region <= 3; ++region) for (unsigned t = 0; t < 2; ++t) {
		uint8_t* code = (region == 2 ? ewram : iwram) + 0x1000;
		memset(code, 0, 256);
		if (t) for (unsigned i = 0; i < sizeof(thumb) / sizeof(*thumb); ++i) {
			code[2 * i] = (uint8_t) thumb[i]; code[2 * i + 1] = (uint8_t) (thumb[i] >> 8);
		} else for (unsigned i = 0; i < sizeof(arm) / sizeof(*arm); ++i)
			for (unsigned b = 0; b < 4; ++b) code[4 * i + b] = (uint8_t) (arm[i] >> (8 * b));
		for (unsigned target = 2; target <= 3; ++target) for (unsigned distance = 0; distance < 128; distance += 3)
		for (unsigned c = 0; c < sizeof(caps) / sizeof(*caps); ++c) {
			machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
			machine.now = 0xffffffe0; machine.ewram_wait = (uint8_t) (distance & 15);
			for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
			machine.cpu.r[1] = target << 24 | 0x200;
			machine.cpu.cpsr = (random_word() & ~63u) | 31;
			gbn_set_waitcnt(&machine, (uint16_t) random_word());
			CHECK((t ? gbn_enter_thumb(&machine, region << 24 | 0x1000) : gbn_enter_arm(&machine, region << 24 | 0x1000)) == GBN_STEP);
			CHECK(gbn_schedule(&machine, 0, distance, 0));
			compare_memory(caps[c], 0x100000 | region << 8 | distance, (target == 2 ? ewram : iwram) + 0x200);
		}
	}
	CHECK(backend.native_instructions > 0 && backend.loop_instructions > 0);
	/* Self-modifying stores, including a RAM mirror and words overlapping
	 * the two-instruction pipeline. Reuse the cache across all replacements. */
	static const uint16_t modify[] = {0x6008, 0x2211, 0x2322, 0x2433, 0xe7fa};
	for (unsigned location = 0; location < 3; ++location) for (unsigned distance = 0; distance < 80; ++distance)
	for (unsigned cap = 1; cap < 20; ++cap) {
		for (unsigned i = 0; i < sizeof(modify) / sizeof(*modify); ++i) {
			iwram[0x1000 + 2 * i] = (uint8_t) modify[i]; iwram[0x1001 + 2 * i] = (uint8_t) (modify[i] >> 8);
		}
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		machine.now = 0xfffffff0;
		machine.cpu.r[0] = 0x24042303;
		unsigned offset = location == 0 ? 0 : location == 1 ? 4 : 8;
		machine.cpu.r[1] = 0x03f01000 + offset;
		CHECK(gbn_enter_thumb(&machine, 0x03001000) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0));
		compare_memory(cap, 0x110000 | location << 8 | distance, iwram + 0x1000 + offset);
	}
	printf("PASS native ARM/Thumb RAM code: %u comparisons, same-bank data stores, mirrors, self modification and stale pipeline\n", comparisons - before);
}
static void ram_dma(void) {
	setup();
	CHECK(gbn_attach_devices(&machine, &devices, palette, vram, oam));
	iwram[0x1000] = 1; iwram[0x1001] = 0x20;
	iwram[0x1002] = 1; iwram[0x1003] = 0x30;
	iwram[0x1004] = 0xfc; iwram[0x1005] = 0xe7;
	CHECK(gbn_enter_thumb(&machine, 0x03001000) == GBN_STEP); compare(30, 0x120000);
	CHECK(backend.native_instructions == 30);
	ewram[0x200] = 7; ewram[0x201] = 0x20; ewram[0x202] = 4; ewram[0x203] = 0x30;
	uint32_t cycles;
	CHECK(gbn_write(&machine, 0x040000b0, 4, 0x02000200, &cycles) == GBN_STEP);
	CHECK(gbn_write(&machine, 0x040000b4, 4, 0x03001000, &cycles) == GBN_STEP);
	CHECK(gbn_write(&machine, 0x040000b8, 4, 0x84000001, &cycles) == GBN_STEP);
	unsigned serviced = 0;
	while (machine.next_event >= 0) {
		CHECK(++serviced < 16);
		machine.now = machine.events[machine.next_event].when;
		CHECK(gbn_service_events(&machine) == GBN_STEP);
	}
	CHECK(!memcmp(ewram + 0x200, iwram + 0x1000, 4));
	/* The old prefetched MOV/ADD must execute before the replaced code. */
	uint64_t native = backend.native_instructions;
	compare(2, 0x120001); CHECK(backend.native_instructions == native);
	compare(31, 0x120002); CHECK(machine.cpu.r[0] == 11 && backend.native_instructions > native);
	/* RAM literal pools are live data, even when the surrounding code and
	 * cached native block remain unchanged. */
	iwram[0x1000] = 1; iwram[0x1001] = 0x48;
	iwram[0x1002] = 0xfd; iwram[0x1003] = 0xe7;
	for (unsigned trial = 0; trial < 8; ++trial) {
		uint32_t value = random_word();
		for (unsigned b = 0; b < 4; ++b) iwram[0x1008 + b] = (uint8_t) (value >> (8 * b));
		CHECK(gbn_enter_thumb(&machine, 0x03001000) == GBN_STEP); compare(64, 0x120003 | trial << 8);
		CHECK(machine.cpu.r[0] == value);
	}
	/* The instruction fetch and literal address wrap at different places:
	 * retain the raw visible PC while applying the RAM mask to the bus. */
	iwram[GBN_IWRAM_SIZE - 4] = 0; iwram[GBN_IWRAM_SIZE - 3] = 0x48;
	iwram[GBN_IWRAM_SIZE - 2] = 0; iwram[GBN_IWRAM_SIZE - 1] = 0xbe;
	CHECK(gbn_enter_thumb(&machine, 0x03007ffc) == GBN_STEP); compare(1, 0x120004);
	puts("PASS DMA replacement of cached RAM code, stale pipeline, live literal pools and RAM wrap");
}
static void arm_self_modification(void) {
	setup();
	unsigned before = comparisons;
	static const uint32_t code[] = {0xe5810000, 0xe2822001, 0xe3a03003, 0xeafffffb};
	static const unsigned targets[] = {0, 4, 8, 12, 128, 132, 136};
	for (unsigned bit1 = 0; bit1 < 2; ++bit1) for (unsigned target = 0; target < sizeof(targets) / sizeof(*targets); ++target)
	for (unsigned distance = 0; distance < 96; distance += 3) for (unsigned cap = 1; cap < 40; ++cap) {
		memset(ewram + 0x1000, 0, 256);
		for (unsigned i = 0; i < sizeof(code) / sizeof(*code); ++i)
			for (unsigned b = 0; b < 4; ++b) ewram[0x1000 + 4 * i + b] = (uint8_t) (code[i] >> (8 * b));
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		machine.now = 0xffffffe0; machine.ewram_wait = (uint8_t) (distance & 15);
		machine.cpu.r[0] = 0xe3a02007; machine.cpu.r[1] = 0x02f01000 + targets[target];
		machine.cpu.cpsr = (random_word() & ~63u) | 31;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_arm(&machine, 0x02001000 | bit1 << 1) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0));
		compare_memory(cap, 0x130000 | target << 8 | distance, ewram + 0x1000 + targets[target]);
	}
	printf("PASS ARM code writes: %u comparisons, stale words, RAM mirrors, visible PC bit 1 and prefetch range edges\n", comparisons - before);
}
static void arm_multiply(void) {
	setup();
	unsigned before = comparisons;
	static const uint32_t values[] = {0, 1, 0xff, 0x100, 0xffff, 0x10000, 0xffffff, 0x1000000,
		0xffffffff, 0xffffff00, 0xfffffeff, 0xffff0000, 0xfffeffff, 0xff000000, 0xfeffffff, 0x80000000};
	static const unsigned rd[] = {0, 1, 2, 3, 8, 12, 15, 0};
	for (unsigned accumulate = 0; accumulate < 2; ++accumulate) for (unsigned set = 0; set < 2; ++set)
	for (unsigned v = 0; v < sizeof(values) / sizeof(*values); ++v) for (unsigned r = 0; r < sizeof(rd) / sizeof(*rd); ++r)
	for (unsigned config = 0; config < 16; ++config) {
		unsigned rn = r == 7 ? 15 : 3, rm = config == 15 ? 15 : 1, rs = config == 14 ? 15 : 2;
		uint32_t code = 0xe0000090 | accumulate << 21 | set << 20 | rd[r] << 16 | rn << 12 | rs << 8 | rm;
		put32(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned i = 0; i < 15; ++i) machine.cpu.r[i] = random_word();
		machine.cpu.r[2] = values[v]; machine.cpu.shifter_carry = config & 1;
		machine.cpu.cpsr = (random_word() & ~63u) | 31; machine.now = 0xfffffff0;
		gbn_set_waitcnt(&machine, (uint16_t) ((config << 2) | (config & 1 ? 0x4000 : 0)));
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
		machine.prefetched_pc = machine.cpu.pc + 8 + 2 * (config & 7);
		compare(1, code);
	}
	/* MULS followed by conditional execution, with every event cut and cap. */
	put32(0, 0xe0100291); put32(4, 0x10800003); put32(8, 0xe0220190); put32(12, 0xeafffffb);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned distance = 0; distance < 80; ++distance) for (unsigned cap = 1; cap < 33; ++cap) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events)); machine.now = 0xfffffff0;
		machine.cpu.r[1] = values[distance & 15]; machine.cpu.r[2] = values[(distance >> 1) & 15];
		machine.cpu.shifter_carry = distance & 1;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0)); compare(cap, 0x140000 | distance);
	}
	printf("PASS native MUL/MLA: %u comparisons, signed byte cycle boundaries, overlap, PC, flags, prefetch and event cuts\n", comparisons - before);
}
static void arm_transfers(void) {
	setup();
	unsigned before = comparisons;
	static const unsigned regions[] = {2, 3, 8, 10, 12, 13, 4};
	static const unsigned widths[] = {4, 2, 1, 1, 4, 2, 1, 2};
	for (unsigned i = 0x100; i < 0x400; ++i) rom[i] = ewram[i] = iwram[i] = (uint8_t) random_word();
	for (unsigned kind = 0; kind < 8; ++kind) for (unsigned mode = 0; mode < 8; ++mode)
	for (unsigned reg_offset = 0; reg_offset < 2; ++reg_offset) for (unsigned alias = 0; alias < 4; ++alias)
	for (unsigned trial = 0; trial < 4; ++trial) for (unsigned loc = 0; loc < sizeof(regions) / sizeof(*regions); ++loc) {
		unsigned rn = alias & 2 ? 8 : 1, rd = alias & 1 ? rn : 0;
		bool pre = (mode & 4) != 0, up = (mode & 2) != 0, wb = (mode & 1) != 0;
		bool half = kind == 1 || kind == 3 || kind == 5 || kind == 7;
		unsigned offset = trial & 1 ? 3 : trial & 2 ? 244 : 4;
		uint32_t code = 0xe0000000 | (unsigned) pre << 24 | (unsigned) up << 23 | (unsigned) wb << 21 |
			(unsigned) (kind > 2) << 20 | rn << 16 | rd << 12;
		if (half) {
			unsigned form = kind == 1 || kind == 5 ? 1 : kind == 3 ? 2 : 3;
			code |= 0x90 | form << 5 | (unsigned) !reg_offset << 22;
			code |= reg_offset ? 2 : (offset & 0xf0) << 4 | (offset & 15);
		} else code |= 0x04000000 | reg_offset << 25 | (unsigned) (widths[kind] == 1) << 22 | (reg_offset ? 2 : offset);
		put32(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
		uint32_t base = regions[loc] << 24 | 0x200;
		machine.cpu.r[rn] = base; machine.cpu.r[2] = offset;
		machine.cpu.cpsr = (random_word() & ~63u) | 31; machine.now = 0xffffffe0;
		machine.ewram_wait = trial * 4;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
		machine.prefetched_pc = machine.cpu.pc + 8 + 2 * trial;
		uint32_t address = pre ? base + (up ? offset : 0u - offset) : base;
		uint8_t* changed = regions[loc] == 2 ? ewram + (address & (GBN_EWRAM_SIZE - 4)) :
			regions[loc] == 3 ? iwram + (address & (GBN_IWRAM_SIZE - 4)) : NULL;
		uint64_t native = backend.native_instructions;
		compare_memory(1, code, changed);
		bool fast = (pre || !reg_offset) && !(address & (widths[kind] - 1));
		if (fast && (regions[loc] < 4 || (kind > 2 && regions[loc] >= 8 && regions[loc] <= 12)))
			CHECK(backend.native_instructions == native + 1);
	}
	/* ROM data reads through a caller-provided unaligned host allocation. */
	put32(0, 0xe5910000); memcpy(alternate + 1, rom, sizeof(rom) - 1);
	CHECK(gbn_attach_rom(&machine, alternate + 1, sizeof(rom) - 1));
	machine.cpu.r[1] = 0x08000200;
	CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP); compare(1, 0x150000);
	printf("PASS native ARM transfers: %u comparisons, all widths/signs, pre/post/writeback, base aliases, ROM waits and slow bus guards\n", comparisons - before);
}
static void io_reads(void) {
	setup();
	CHECK(gbn_attach_devices(&machine, &devices, palette, vram, oam));
	for (unsigned i = 0; i < sizeof(devices.io) / sizeof(*devices.io); ++i) devices.io[i] = (uint16_t) random_word();
	unsigned before = comparisons;
	/* Sweep the readable, dynamic, open-bus and unsupported IO map, including
	 * odd byte lanes and unaligned halfwords/words. The interpreter is the
	 * oracle; no duplicated permission table is used by this sweep. */
	for (unsigned kind = 3; kind < 8; ++kind) for (unsigned offset = 0; offset < 0x240; ++offset)
	for (unsigned config = 0; config < 4; ++config) {
		put16(0, 0x5000 | kind << 9 | 2 << 6 | 1 << 3);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		machine.cpu.r[0] = random_word(); machine.cpu.r[1] = 0x04000000 + offset; machine.cpu.r[2] = 0;
		machine.cpu.cpsr = (random_word() & ~31u) | 31; machine.now = 0xfffffff0;
		gbn_set_waitcnt(&machine, (uint16_t) ((config & 1 ? 0x4000 : 0) | (config << 2)));
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		machine.prefetched_pc = machine.cpu.pc + 4 + 2 * config;
		compare(1, 0x230000 | kind << 12 | offset);
	}
	/* ARM writeback and base/destination aliases exercise the same fast path. */
	for (unsigned alias = 0; alias < 2; ++alias) for (unsigned config = 0; config < 4; ++config) {
		put32(0, alias ? 0xe5b11004 : 0xe5b10004); /* LDR r0/r1,[r1,#4]! */
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		machine.cpu.r[1] = 0x04000000; machine.now = 0xfffffff0;
		gbn_set_waitcnt(&machine, (uint16_t) ((config & 1 ? 0x4000 : 0) | (config << 2)));
		CHECK(gbn_enter_arm(&machine, 0x08000000) == GBN_STEP);
		uint64_t native = backend.native_instructions;
		compare(1, 0x240000 | alias << 8 | config); CHECK(backend.native_instructions == native + 1);
	}
	/* Live IO values must not be compiled as constants, and native polling
	 * must stop at every event/cap boundary even across a clock wrap. */
	put16(0, 0x8808); put16(2, 0x2800); put16(4, 0xd0fc); put16(6, 0xe7fb);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned distance = 0; distance < 80; ++distance) for (unsigned cap = 1; cap < 33; ++cap) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events)); machine.now = 0xfffffff0;
		devices.io[0x06 / 2] = distance & 1 ? 0 : (uint16_t) random_word();
		machine.cpu.r[1] = 0x04000006;
		gbn_set_waitcnt(&machine, (uint16_t) random_word());
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		CHECK(gbn_schedule(&machine, 0, distance, 0)); compare(cap, 0x250000 | distance);
	}
	printf("PASS native IO reads: %u comparisons, complete IO map, signed/unaligned loads, ARM aliases, live state and event cuts\n", comparisons - before);
}

static void compare_device_write(uint32_t cap,unsigned tag) {
	static struct GbnDevices saved,after;
	reference=machine; saved=devices;
	uint32_t want=0,got=0;
	enum GbnStatus a=gbn_run_batch(&reference,cap,&want);
	after=devices; devices=saved;
	enum GbnStatus b=gbn_rv32_run_batch(&backend,cap,&got);
	if(a!=b || want!=got || memcmp(&machine,&reference,sizeof(machine)) || memcmp(&devices,&after,sizeof(devices))) {
		printf("FAIL native device write tag=%08x status=%u/%u count=%u/%u\n",tag,b,a,got,want);exit(1);
	}
	++comparisons;
}
static void dma_writes(void) {
	setup();unsigned before=comparisons;
	for(unsigned kind=0;kind<3;++kind) {
		put16(0,0x5000 | kind<<9 | 2<<6 | 1<<3);
		CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
		for(unsigned offset=0;offset<48;++offset)for(unsigned trial=0;trial<4;++trial) {
			memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
			CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
			for(unsigned c=0;c<4;++c) {
				devices.dma[c].source=random_word();devices.dma[c].dest=random_word();
				devices.dma[c].next_source=random_word();devices.dma[c].next_dest=random_word();
				devices.dma[c].remaining=17;devices.dma[c].control=0x8200;
			}
			machine.cpu.r[0]=random_word();machine.cpu.r[1]=0x040000b0+offset;machine.cpu.r[2]=0;
			machine.cpu.cpsr=(random_word()&~63u)|63;machine.now=trial&1 ? 0xfffffff0 : 0;
			gbn_set_waitcnt(&machine,(uint16_t)random_word());
			CHECK(gbn_enter_thumb(&machine,0x08000000)==GBN_STEP);
			uint64_t native=backend.native_instructions;
			compare_device_write(1,0x2b0000|kind<<12|offset<<4|trial);
			if(kind==0 && !(offset&3) && offset%12<8)CHECK(backend.native_instructions==native+1);
		}
	}
	for(unsigned form=0;form<3;++form)for(unsigned c=0;c<4;++c)for(unsigned dest=0;dest<2;++dest) {
		put32(0,form==0 ? 0xe5a10004 : form==1 ? 0xe5a11004 : 0xe4810004);
		CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
		memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
		CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
		machine.cpu.r[0]=random_word();machine.cpu.r[1]=0x040000b0+12*c+4*dest-(form<2 ? 4 : 0);
		CHECK(gbn_enter_arm(&machine,0x08000000)==GBN_STEP);
		uint64_t native=backend.native_instructions;
		compare_device_write(1,0x2c0000|form<<8|c<<4|dest);CHECK(backend.native_instructions==native+1);
	}
	put16(0,0x6008);put16(2,0x3301);put16(4,0xe7fc);
	CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
	for(unsigned config=0;config<4;++config)for(unsigned distance=0;distance<32;++distance)for(unsigned cap=0;cap<9;++cap) {
		memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
		CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
		machine.cpu.r[0]=random_word();machine.cpu.r[1]=0x040000b0+12*(config&3);machine.cpu.r[3]=random_word();
		machine.now=0xfffffff0;gbn_set_waitcnt(&machine,(uint16_t)((config&1 ? 0x4000 : 0)|config<<2));
		CHECK(gbn_enter_thumb(&machine,0x08000000)==GBN_STEP);
		CHECK(gbn_schedule(&machine,0,distance,1));
		compare_device_write(cap,0x2d0000|config<<12|distance<<4|cap);
	}
	printf("PASS native DMA address writes: %u comparisons; full device snapshots, all channels/masks/widths, active transfer preservation and ARM writeback aliases\n",comparisons-before);
}

static void dma_reads(void) {
	setup(); CHECK(gbn_attach_devices(&machine, &devices, palette, vram, oam));
	unsigned before=comparisons;
	static const unsigned widths[]={4,2,1,1,4,2,1,2};
	for(unsigned kind=3;kind<8;++kind) {
		put16(0,0x5000 | kind<<9 | 2<<6 | 1<<3);
		CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
		for(unsigned channel=0;channel<4;++channel)for(unsigned part=8;part<12;++part) {
			unsigned offset=0xb0+12*channel+part;
			if(offset&(widths[kind]-1))continue;
			/* Reuse one compiled entry while changing the live registers.
			 * Reload count is deliberately nonzero; reads still return zero. */
			for(unsigned trial=0;trial<4;++trial) {
				devices.io[(0xb8+12*channel)/2]=(uint16_t)(0xffff-trial);
				devices.io[(0xba+12*channel)/2]=(uint16_t)random_word();
				machine.cpu.r[1]=0x04000000+offset;machine.cpu.r[2]=0;
				machine.cpu.cpsr=(random_word()&~63u)|63;machine.now=0xfffffff0;
				gbn_set_waitcnt(&machine,(uint16_t)random_word());
				CHECK(gbn_enter_thumb(&machine,0x08000000)==GBN_STEP);
				uint64_t native=backend.native_instructions,slow=backend.fallback_instructions;
				compare(1,0x290000 | channel<<8 | part<<4 | trial);
				CHECK(backend.native_instructions==native+1 && backend.fallback_instructions==slow);
			}
		}
	}
	for(unsigned alias=0;alias<2;++alias)for(unsigned channel=0;channel<4;++channel) {
		put32(0,alias ? 0xe5b11004 : 0xe5b10004);
		CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
		machine.cpu.r[1]=0x040000b4+12*channel;
		devices.io[(0xb8+12*channel)/2]=0xabcd;
		devices.io[(0xba+12*channel)/2]=(uint16_t)random_word();
		CHECK(gbn_enter_arm(&machine,0x08000000)==GBN_STEP);
		uint64_t native=backend.native_instructions;
		compare(1,0x2a0000|alias<<8|channel);CHECK(backend.native_instructions==native+1);
	}
	printf("PASS native DMA count/control reads: %u comparisons; all channels, widths/signs, live values, zero reload reads and ARM writeback aliases\n",comparisons-before);
}

static void fixed_loops(void) {
	setup(); CHECK(gbn_attach_devices(&machine, &devices, palette, vram, oam));
	put16(0, 0x8808); put16(2, 0x280c); put16(4, 0xd1fc); put16(6, 0x3701); put16(8, 0xe7fa);
	static const unsigned regions[] = {2, 3, 8, 10, 12};
	static const uint32_t caps[] = {1, 2, 3, 4, 5, 8, 31, 32, 33, 127, 256, 1001, 65535};
	static const uint32_t distances[] = {0, 1, 2, 7, 15, 31, 128, 4096, 1000000};
	unsigned before = comparisons;
	for (unsigned region = 0; region < sizeof(regions) / sizeof(*regions); ++region) {
		memcpy(ewram, rom, 16); memcpy(iwram, rom, 16);
		for (unsigned value = 0; value < 3; ++value) for (unsigned config = 0; config < 4; ++config)
		for (unsigned cap = 0; cap < sizeof(caps) / sizeof(*caps); ++cap)
		for (unsigned distance = 0; distance < sizeof(distances) / sizeof(*distances); ++distance) {
			machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events)); machine.now = 0xffffffe0;
			devices.io[3] = value == 0 ? 11 : value == 1 ? 12 : 65535;
			machine.cpu.r[0] = random_word(); machine.cpu.r[1] = 0x04000006; machine.cpu.r[7] = 0;
			gbn_set_waitcnt(&machine, (uint16_t) ((config & 1 ? 0x4000 : 0) | config << 2));
			CHECK(gbn_enter_thumb(&machine, regions[region] << 24) == GBN_STEP);
			CHECK(gbn_schedule(&machine, 0, distances[distance], 0));
			compare(caps[cap], 0x260000 | region << 12 | value << 8 | distance);
		}
	}
	CHECK(backend.merged_instructions > 100000);
	/* A counter loop can have an unchanged loaded value/flags while its
	 * counter changes. Every written register must participate in the proof. */
	uint64_t merged = backend.merged_instructions;
	put16(0, 0x8808); put16(2, 0x3301); put16(4, 0x2bff); put16(6, 0xd1fb);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned trial = 0; trial < 16; ++trial) {
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events)); machine.now = 0xffffffe0;
		machine.cpu.r[1] = 0x04000006; machine.cpu.r[3] = 0xffff0000 + trial;
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(4096, 0x270000 | trial);
	}
	CHECK(backend.merged_instructions == merged);
	/* Dynamic timers and writes must never be merged, even if a stored
	 * value happens to equal its old value. */
	put16(0, 0x8808); put16(2, 0xe7fd); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	machine.cpu.r[1] = 0x04000100;
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare(4096, 0x280000);
	CHECK(backend.merged_instructions == merged);
	put16(0, 0x6008); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	machine.cpu.r[1] = 0x02001000; machine.cpu.r[0] = 0;
	CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP); compare_memory(4096, 0x280001, ewram + 0x1000);
	CHECK(backend.merged_instructions == merged);
	printf("PASS fixed-point loops: %u comparisons; merged=%" PRIu64 "; exact counts/time, live IO, events, large caps, RAM/ROM, counters and side effects\n",
		comparisons - before, merged);
}

static void chain_program(bool thumb) {
	if (thumb) {
		put16(0, 0x3001); put16(2, 0xe00d);
		put16(32, 0x3703); put16(34, 0xe00d);
		put16(64, 0x3202); put16(66, 0x280a); put16(68, 0xd1dc);
		put16(70, 0xe7eb);
	} else {
		put32(0, 0xe2800001); put32(4, 0xea000005);
		put32(32, 0xe2877003); put32(36, 0xea000005);
		put32(64, 0xe2822002); put32(68, 0xe350000a); put32(72, 0x1affffec);
		put32(76, 0xeafffff3);
	}
}
static void native_chains(void) {
	unsigned before = comparisons;
	const unsigned regions[] = {2, 3, 8, 10, 12};
	const unsigned caps[] = {1, 2, 3, 4, 5, 6, 7, 9, 17, 32, 127, 256};
	for (unsigned r = 0; r < 5; ++r) for (unsigned t = 0; t < 2; ++t)
	for (unsigned unaligned = 0; unaligned < (regions[r] < 8 ? 2u : 1u); ++unaligned) {
		setup(); chain_program(t);
		/* The public RAM-buffer contract does not require host alignment. */
		gbn_init(&machine, ewram + unaligned, iwram + unaligned);
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		uint32_t base = regions[r] << 24;
		if (regions[r] < 8) {
			base += 0x1000;
			memcpy((regions[r] == 2 ? machine.ewram : machine.iwram) + 0x1000, rom, 128);
		}
		gbn_set_waitcnt(&machine, 0x4000);
		CHECK((t ? gbn_enter_thumb(&machine, base) : gbn_enter_arm(&machine, base)) == GBN_STEP);
		compare(512, 0x160000 | r << 8 | t);
		CHECK(backend.chained_blocks > 0);
		CHECK((t ? gbn_enter_thumb(&machine, base) : gbn_enter_arm(&machine, base)) == GBN_STEP);
		machine.cpu.r[0] = 0;
		uint64_t entries = backend.native_entries, chains = backend.chained_blocks;
		compare(384, 0x160100 | r << 4 | t);
		CHECK(backend.native_entries == entries + 1 && backend.chained_blocks > chains + 100);
		for (unsigned distance = 0; distance < 160; ++distance) for (unsigned cap = 0; cap < 12; ++cap) {
			machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
			machine.now = distance & 1 ? 0xfffffff0 : 0;
			for (unsigned reg = 0; reg < 15; ++reg) machine.cpu.r[reg] = random_word();
			machine.cpu.r[0] = cap & 1 ? 0 : 9;
			machine.cpu.cpsr = (random_word() & ~63u) | 31;
			CHECK((t ? gbn_enter_thumb(&machine, base) : gbn_enter_arm(&machine, base)) == GBN_STEP);
			CHECK(gbn_schedule(&machine, 0, distance, 0));
			compare(caps[cap], 0x170000 | r << 12 | t << 11 | distance);
		}
		machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
		gbn_set_waitcnt(&machine, 0x4317); machine.ewram_wait = 7;
		CHECK((t ? gbn_enter_thumb(&machine, base) : gbn_enter_arm(&machine, base)) == GBN_STEP);
		compare(384, 0x180000 | r << 4 | t); /* Old successor timing must be rejected. */
		/* Recycle the arena while cached edges still name its descriptors. */
		uint32_t compiled = backend.compiled_blocks;
		backend.code_used = GBN_RV32_CODE_WORDS - 1;
		backend.code_segment = GBN_RV32_SEGMENTS - 1;
		machine.ewram_wait = 6;
		CHECK((t ? gbn_enter_thumb(&machine, base + 32) : gbn_enter_arm(&machine, base + 32)) == GBN_STEP);
		gbn_set_waitcnt(&machine, 0x4000);
		compare(384, 0x180100 | r << 4 | t);
		CHECK(backend.compiled_blocks > compiled && backend.cache_flushes > 0);
	}
	/* A native store changes a different, already compiled RAM block. The
	 * old target must fail its byte guard before any stale instruction runs. */
	for (unsigned region = 2; region <= 3; ++region) {
		setup();
		put16(0, 0x6008); put16(2, 0xe07d);
		put16(256, 0x3703); put16(258, 0xe07d);
		put16(512, 0x3202); put16(514, 0xe6fd);
		uint8_t* bytes = (region == 2 ? ewram : iwram) + 0x1000;
		memcpy(bytes, rom, 1024);
		machine.cpu.r[0] = 0xe6fd3202; machine.cpu.r[1] = region << 24 | 0x1200;
		CHECK(gbn_enter_thumb(&machine, region << 24 | 0x1000) == GBN_STEP);
		compare_memory(120, 0x190000 | region, bytes + 512);
		CHECK(backend.chained_blocks > 0);
		CHECK(gbn_enter_thumb(&machine, region << 24 | 0x1000) == GBN_STEP);
		machine.cpu.r[0] = 0xe6fd3263;
		uint32_t compiled = backend.compiled_blocks;
		compare_memory(127, 0x190010 | region, bytes + 512);
		CHECK(backend.compiled_blocks > compiled);
	}
	printf("PASS native cross-block chains: %u comparisons; warm resident execution, caps/events, unaligned host RAM, timing changes, arena recycle and cross-block code writes\n", comparisons - before);
}

static void cache_pressure(void) {
	setup();
	unsigned before = comparisons;
	const unsigned bytes = 8 * GBN_RV32_SLOTS;
	CHECK(bytes <= GBN_EWRAM_SIZE);
	for (unsigned i = 0; i < bytes; i += 4) {
		ewram[i] = (uint8_t) (i >> 2); ewram[i + 1] = 0x30;
		ewram[i + 2] = 0xfd; ewram[i + 3] = 0xe7;
	}
	/* More live entry addresses than cache slots, then revisit them in a
	 * different order. No game addresses or expected hash layout are used. */
	for (unsigned pass = 0; pass < 2; ++pass) for (unsigned i = 0; i < bytes / 4; ++i) {
		unsigned offset = (pass ? (i * 509) & (bytes / 4 - 1) : i) * 4;
		CHECK(gbn_enter_thumb(&machine, 0x02000000 + offset) == GBN_STEP);
		compare(8, 0x200000 | pass << 16 | i);
	}
	CHECK(backend.compiled_blocks > GBN_RV32_SLOTS && !backend.fallback_instructions);
	CHECK(backend.code_used <= GBN_RV32_CODE_WORDS);
	/* Explicitly populate two segments: descriptor pressure need not fill
	 * a larger arena. Reclaim segment zero while preserving segment one. */
	gbn_rv32_init(&backend, &machine);
	CHECK(gbn_enter_thumb(&machine, 0x02000000) == GBN_STEP); compare(8, 0x220000);
	struct GbnRv32Block* victim = backend.segments[0];
	CHECK(victim && victim->valid && victim->pc == 0x02000000 && victim->chain);
	backend.code_used = GBN_RV32_SEGMENT_WORDS; backend.code_segment = 1;
	CHECK(gbn_enter_thumb(&machine, 0x02000004) == GBN_STEP); compare(8, 0x220001);
	struct GbnRv32Block* survivor = backend.segments[1];
	CHECK(survivor && survivor->valid && survivor->pc == 0x02000004 && survivor->chain);
	uint32_t* code = survivor->code;
	backend.code_used = GBN_RV32_CODE_WORDS - 1; backend.code_segment = GBN_RV32_SEGMENTS - 1;
	CHECK(gbn_enter_thumb(&machine, 0x02000002) == GBN_STEP);
	compare(1, 0x220002);
	CHECK(backend.cache_flushes == 1 && !victim->valid && !victim->chain && !victim->arena_linked);
	CHECK(survivor->valid && survivor->arena_linked && survivor->chain);
	uint32_t compiled = backend.compiled_blocks;
	CHECK(gbn_enter_thumb(&machine, survivor->pc) == GBN_STEP); compare(8, 0x220003);
	CHECK(backend.compiled_blocks == compiled && survivor->code == code);
	CHECK(gbn_enter_thumb(&machine, 0x02000000) == GBN_STEP); compare(8, 0x220004);
	CHECK(backend.compiled_blocks == compiled + 1 && !backend.fallback_instructions);

	printf("PASS cache pressure: %u comparisons; replacement, arena wraps and surviving warm segments\n", comparisons - before);
}

/* Multiple transfers can modify more than one word or wrap a RAM mirror.
 * Restore the entire affected window before running the second engine. */
static void compare_stack(uint32_t cap, unsigned tag, uint8_t* bytes, unsigned mask,
                          unsigned offset, unsigned size) {
	uint8_t saved[128], after[128];
	CHECK(size <= sizeof(saved));
	for (unsigned i = 0; i < size; ++i) saved[i] = bytes[(offset + i) & mask];
	reference = machine;
	uint32_t want = 0, got = 0;
	enum GbnStatus a = gbn_run_batch(&reference, cap, &want);
	for (unsigned i = 0; i < size; ++i) {
		after[i] = bytes[(offset + i) & mask];
		bytes[(offset + i) & mask] = saved[i];
	}
	enum GbnStatus b = gbn_rv32_run_batch(&backend, cap, &got);
	bool equal = a == b && want == got && !memcmp(&machine, &reference, sizeof(machine));
	for (unsigned i = 0; i < size; ++i) equal &= bytes[(offset + i) & mask] == after[i];
	if (!equal) {
		printf("FAIL stack tag=%08x cap=%u status=%u/%u count=%u/%u pc=%08lx/%08lx cycles=%lu/%lu prefetch=%08lx/%08lx\n",
			tag, cap, b, a, got, want, machine.cpu.pc, reference.cpu.pc,
			machine.now, reference.now, machine.prefetched_pc, reference.prefetched_pc);
		for (unsigned r = 0; r < 15; ++r) if (machine.cpu.r[r] != reference.cpu.r[r])
			printf("r%u=%08lx/%08lx\n", r, machine.cpu.r[r], reference.cpu.r[r]);
		exit(1);
	}
	++comparisons;
}
#ifndef EXPECT_NATIVE_STACK
#define EXPECT_NATIVE_STACK 1
#endif
static unsigned stack_count(unsigned code) {
	unsigned count = (code >> 8) & 1;
	for (unsigned r = 0; r < 8; ++r) count += code >> r & 1;
	return count;
}
static void stack_encodings(void) {
	setup();
	unsigned before = comparisons;
	for (unsigned form = 0; form < 4; ++form) for (unsigned list = 0; list < 256; ++list) {
		unsigned code = 0xb400 | (form >> 1) << 11 | (form & 1) << 8 | list;
		unsigned count = stack_count(code);
		put16(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned bank = 0; bank < 2; ++bank) for (unsigned trial = 0; trial < 4; ++trial) {
			unsigned region = 2 + bank, mask = bank ? GBN_IWRAM_SIZE - 1 : GBN_EWRAM_SIZE - 1;
			uint8_t* bytes = bank ? iwram : ewram;
			for (unsigned i = 0; i < 128; ++i) bytes[0xc0 + i] = (uint8_t) random_word();
			for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
			machine.cpu.r[13] = region << 24 | 0x100 | trial;
			machine.cpu.cpsr = (random_word() & ~63u) | 63;
			machine.cpu.shifter_carry = trial & 1;
			machine.now = trial & 1 ? 0xfffffff0 : 0;
			machine.ewram_wait = random_word() & 15;
			gbn_set_waitcnt(&machine, (uint16_t) random_word());
			CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
			machine.prefetched_pc = machine.cpu.pc + 4 + 2 * (random_word() & 7);
			if (form == 3) {
				uint32_t target = 0x08000100 | (trial & 1);
				unsigned at = 0x100 + 4 * (count - 1);
				for (unsigned b = 0; b < 4; ++b) bytes[at + b] = target >> (8 * b);
			}
			uint64_t native = backend.native_instructions;
			compare_stack(1, 0x300000 | code, bytes, mask, 0xc0, 128);
			CHECK(backend.native_instructions == native + (EXPECT_NATIVE_STACK && count != 0));
		}
	}
	printf("PASS Thumb PUSH/POP masks, LR/PC, SP low bits, both RAM banks and random waits: %u comparisons\n", comparisons - before);
}
static void stack_boundaries(void) {
	static const unsigned codes[] = {0xb400, 0xb401, 0xb500, 0xb5ff, 0xbc00, 0xbc81, 0xbcff, 0xbd00, 0xbdff};
	static const unsigned offsets[] = {0, 1, 3, 4, 32, 36, 64, 0x7fdc, 0x7ffc, 0x7fff, 0x3ffdc, 0x3fffc, 0x3ffff, 0xfffffc};
	setup(); unsigned before = comparisons;
	for (unsigned c = 0; c < sizeof(codes) / sizeof(*codes); ++c) {
		unsigned code = codes[c], count = stack_count(code);
		put16(0, code); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned region = 2; region <= 3; ++region)
		for (unsigned o = 0; o < sizeof(offsets) / sizeof(*offsets); ++o) for (unsigned host = 0; host < 2; ++host) {
			machine.ewram = ewram + host; machine.iwram = iwram + host;
			uint32_t sp = region << 24 | offsets[o];
			uint32_t at = (code & 0x800) ? sp : sp - 4 * count;
			/* The interpreter selects its mapping from the first address, then
			 * wraps each word within that bank even across a 24-bit boundary. */
			unsigned actual_region = at >> 24;
			uint8_t* bytes = actual_region == 3 ? machine.iwram : machine.ewram;
			unsigned mask = actual_region == 3 ? GBN_IWRAM_SIZE - 1 : GBN_EWRAM_SIZE - 1;
			unsigned begin = ((at & ~3u) - 32) & mask;
			for (unsigned i = 0; i < 128; ++i) bytes[(begin + i) & mask] = random_word();
			if ((code & 0x900) == 0x900) {
				uint32_t target = 0x08000101;
				for (unsigned b = 0; b < 4; ++b) bytes[((at & ~3u) + 4 * (count - 1) + b) & mask] = target >> (8 * b);
			}
			for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
			machine.cpu.r[13] = sp; machine.now = 0xfffffff0;
			machine.ewram_wait = o & 15; gbn_set_waitcnt(&machine, (uint16_t) random_word());
			CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
			uint64_t native = backend.native_instructions;
			compare_stack(1, 0x310000 | c << 12 | region << 8 | o << 1 | host, bytes, mask, begin, 128);
			bool fast = EXPECT_NATIVE_STACK && count && !host && actual_region >= 2 && actual_region <= 3 && (at & mask & ~3u) + count * 4 <= mask + 1;
			CHECK(backend.native_instructions == native + fast);
		}
	}
	machine.ewram = ewram; machine.iwram = iwram;
	/* Unsupported/other-region return targets and NULL RAM must leave the
	 * complete instruction to the interpreter before modifying SP or r0-r7. */
	static const uint32_t targets[] = {0, 0x02000101, 0x03000101, 0x04000000, 0x08001000, 0x08000fff, 0x0a000100};
	put16(0, 0xbdff); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned t = 0; t < sizeof(targets) / sizeof(*targets); ++t) {
		for (unsigned b = 0; b < 4; ++b) ewram[0x120 + b] = targets[t] >> (8 * b);
		machine.cpu.r[13] = 0x02000100;
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		uint64_t native = backend.native_instructions;
		compare_stack(1, 0x320000 | t, ewram, GBN_EWRAM_SIZE - 1, 0x100, 36);
		CHECK(backend.native_instructions == native + (EXPECT_NATIVE_STACK && t == 5));
	}
	for (unsigned pop = 0; pop < 2; ++pop) {
		put16(0, pop ? 0xbcff : 0xb5ff); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		machine.cpu.r[13] = 0x02000100; machine.ewram = NULL;
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		compare_stack(1, 0x330000 | pop, ewram, GBN_EWRAM_SIZE - 1, 0xc0, 128);
		machine.ewram = ewram;
	}
	printf("PASS stack RAM mirrors, bank edges, host alignment, absent RAM and invalid/different return targets: %u comparisons\n", comparisons - before);
}
static void stack_streams(void) {
	static const uint16_t program[] = {0xb5ff, 0x3001, 0xbcff, 0xbc04, 0x3303, 0xe7f9};
	static const unsigned caps[] = {0, 1, 2, 3, 4, 5, 6, 7, 31, 32, 33, 127};
	setup(); unsigned before = comparisons;
	for (unsigned i = 0; i < sizeof(program) / 2; ++i) put16(2 * i, program[i]);
	CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned code_region = 2; code_region <= 12; code_region += code_region < 8 ? (code_region == 2 ? 1 : 5) : 2) {
		if (code_region < 8) {
			uint8_t* code = (code_region == 2 ? ewram : iwram) + 0x1000;
			memcpy(code, rom, 128);
		}
		for (unsigned bank = 0; bank < 2; ++bank) for (unsigned distance = 0; distance < 128; ++distance)
		for (unsigned c = 0; c < sizeof(caps) / sizeof(*caps); ++c) {
			uint8_t* bytes = bank ? iwram : ewram;
			machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
			machine.now = 0xffffffe0; machine.ewram_wait = distance & 15;
			for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
			machine.cpu.r[13] = (bank ? 0x03000100 : 0x02000100) | (distance & 3);
			machine.cpu.cpsr = (random_word() & ~63u) | 63;
			gbn_set_waitcnt(&machine, (uint16_t) random_word());
			CHECK(gbn_enter_thumb(&machine, code_region << 24 | (code_region < 8 ? 0x1000 : 0)) == GBN_STEP);
			machine.prefetched_pc = machine.cpu.pc + 4 + 2 * (distance & 7);
			CHECK(gbn_schedule(&machine, 0, distance, 0));
			compare_stack(caps[c], 0x340000 | code_region << 12 | distance << 4 | c, bytes,
				bank ? GBN_IWRAM_SIZE - 1 : GBN_EWRAM_SIZE - 1, 0xc0, 128);
		}
	}
	machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
	/* A long sequence of small stack instructions forces code-capacity splits. */
	for (unsigned i = 0; i < 50; ++i) put16(2 * i, i & 1 ? 0xbc01 : 0xb401);
	put16(100, 0xe7cc); CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
	for (unsigned cap = 1; cap < 132; ++cap) {
		machine.cpu.r[13] = 0x03000100;
		CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
		compare_stack(cap, 0x350000 | cap, iwram, GBN_IWRAM_SIZE - 1, 0xc0, 128);
	}
	printf("PASS stack resident blocks, all event/cap cuts, clock wrap, RAM/ROM code and capacity splits: %u comparisons\n", comparisons - before);
}
static void stack_self_modification(void) {
	static const int offsets[] = {-40, -36, -32, -4, 0, 4, 32, 64, 68, 72};
	setup(); unsigned before = comparisons;
	for (unsigned region = 2; region <= 3; ++region) for (unsigned code_offset = 0; code_offset <= 2; code_offset += 2)
	for (unsigned o = 0; o < sizeof(offsets) / sizeof(*offsets); ++o) for (unsigned cap = 1; cap < 6; ++cap) {
		uint8_t* bytes = region == 2 ? ewram : iwram;
		unsigned mask = region == 2 ? GBN_EWRAM_SIZE - 1 : GBN_IWRAM_SIZE - 1;
		for (unsigned i = 0xfc0; i < 0x1100; i += 2) { bytes[i] = 0xc0; bytes[i + 1] = 0x46; }
		bytes[0x1000 + code_offset] = 0xff; bytes[0x1001 + code_offset] = 0xb5;
		for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = 0x46c046c0;
		machine.cpu.r[13] = region << 24 | 0xf00000 | (0x1000 + code_offset + offsets[o] + 36);
		CHECK(gbn_enter_thumb(&machine, region << 24 | (0x1000 + code_offset)) == GBN_STEP);
		compare_stack(cap, 0x360000 | region << 12 | code_offset << 8 | o << 4 | cap,
			bytes, mask, ((machine.cpu.r[13] - 36) & ~3u) - 16, 80);
	}
	printf("PASS multiword writes overlapping RAM code and prefetched tail, mirrored SP and aligned/halfword code entries: %u comparisons\n", comparisons - before);
}
static void stack_returns(void) {
	static const unsigned lists[] = {0, 1, 0x81, 0xff};
	static const unsigned caps[] = {1, 2, 3, 4, 31};
	setup(); unsigned before = comparisons;
	for (unsigned region = 2; region <= 3; ++region) for (unsigned bank = 0; bank < 2; ++bank)
	for (unsigned l = 0; l < sizeof(lists) / sizeof(*lists); ++l) {
		uint8_t* code = region == 2 ? ewram : iwram;
		unsigned opcode = 0xbd00 | lists[l], count = stack_count(opcode);
		code[0x1000] = opcode; code[0x1001] = opcode >> 8;
		code[0x1002] = 0xc0; code[0x1003] = 0x46;
		/* Return to a mirrored address near the end of the code bank. */
		unsigned mask = region == 2 ? GBN_EWRAM_SIZE - 1 : GBN_IWRAM_SIZE - 1;
		for (unsigned b = 0; b < 128; b += 2) { code[(mask - 15 + b) & mask] = 0xc0; code[(mask - 14 + b) & mask] = 0x46; }
		for (unsigned odd = 0; odd < 2; ++odd) for (unsigned c = 0; c < sizeof(caps) / sizeof(*caps); ++c) {
			uint8_t* bytes = bank ? iwram : ewram;
			uint32_t target = region << 24 | 0xf00000 | (mask - 15) | odd;
			for (unsigned b = 0; b < 4; ++b) bytes[0x100 + 4 * (count - 1) + b] = target >> (8 * b);
			for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
			machine.cpu.r[13] = (bank ? 0x03000100 : 0x02000100) | odd;
			machine.now = 0xfffffff0; machine.ewram_wait = c & 15;
			gbn_set_waitcnt(&machine, (uint16_t) random_word());
			CHECK(gbn_enter_thumb(&machine, region << 24 | 0x1000) == GBN_STEP);
			uint64_t native = backend.native_instructions;
			compare_stack(caps[c], 0x370000 | region << 12 | bank << 11 | l << 8 | odd << 7 | c,
				bytes, bank ? GBN_IWRAM_SIZE - 1 : GBN_EWRAM_SIZE - 1, 0x100, 36);
			if (EXPECT_NATIVE_STACK && backend.native_instructions != native + caps[c]) {
                printf("FAIL native return coverage region=%u bank=%u list=%x odd=%u cap=%u native=%llu\n", region, bank, lists[l], odd, caps[c], backend.native_instructions - native);
                exit(1);
            }
		}
	}
	printf("PASS native POP PC in RAM, same/other stack banks, mirrored returns, both target low bits and code-bank fetch wrap: %u comparisons\n", comparisons - before);
}

/* Follow N/Z independently through native edges, interpreter bridges and every
 * partial-prefix return. In particular N=Z=1 must survive flag-preserving code. */
static void resident_flag_paths(void) {
    static const unsigned regions[] = {2, 3, 8};
    static const unsigned caps[] = {1, 2, 3, 4, 7, 31, 32, 65, 128};
    static const unsigned distances[] = {0, 1, 7, 31, 128, 4096};
    setup(); unsigned before = comparisons;
    uint64_t chains = backend.chained_blocks, native = backend.native_instructions;
    uint64_t fallback = backend.fallback_instructions;
    for (unsigned cond = 0; cond < 14; ++cond) {
        for (unsigned i = 0; i < 96; ++i) put16(2*i, 0x4600);
        put16(2, 0x8808); /* LDRH r0,[r1], preserving entry flags. */
        put16(4, 0xd000 | cond << 8 | 44); /* Conditional edge to halfword 48. */
        put16(6, 0x2200); put16(8, 0x001a); /* MOVS zero; LSLS r2,r3,#0. */
        put16(10, 0x4162); /* ADCS r2,r4 consumes preserved C. */
        put16(12, 0xe018); /* Unconditional edge to halfword 32. */
        put16(64, 0x42a3); put16(66, 0xd41d); /* CMP r3,r4; BMI halfword 64. */
        put16(68, 0x2300);
        put16(96, 0x2500); put16(98, 0xd50d); /* MOVS zero; BPL halfword 64. */
        put16(128, 0x41e2); /* ROR r2,r4 uses the interpreter and changes flags. */
        put16(190, 0xe79f); /* Back to entry after a code-capacity split. */
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        memcpy(ewram, rom, 192); memcpy(iwram, rom, 192);
        for (unsigned bank = 0; bank < sizeof(regions)/sizeof(*regions); ++bank)
        for (unsigned flags = 0; flags < 16; ++flags)
        for (unsigned c = 0; c < sizeof(caps)/sizeof(*caps); ++c)
        for (unsigned d = 0; d < sizeof(distances)/sizeof(*distances); ++d) {
            machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
            machine.now = (d & 1) ? 0xfffffff0 : 0;
            for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
            machine.cpu.r[1] = 0x02001000;
            machine.cpu.r[3] = (flags & 1) ? 0x80000000 : (flags & 2) ? 0 : 0x7fffffff;
            machine.cpu.r[4] = (flags & 4) ? 0xffffffff : 1;
            machine.cpu.cpsr = flags << 28 | ((c & 1) ? 0x0f00003f : 0x0000003f);
            machine.cpu.shifter_carry = c & 1;
            gbn_set_waitcnt(&machine, (uint16_t)random_word());
            CHECK(gbn_enter_thumb(&machine, regions[bank] << 24) == GBN_STEP);
            CHECK(gbn_schedule(&machine, 0, distances[d], 0));
            compare(caps[c], 0x380000 | cond << 12 | flags << 8 | c << 4 | d);
        }
    }
    CHECK(backend.chained_blocks > chains && backend.native_instructions > native);
    CHECK(backend.fallback_instructions > fallback);
    machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
    machine.cpu.r[1] = 0x02001000;
    CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
    register uint32_t saved_n __asm__("s5") = 0x138ace57;
    register uint32_t saved_z __asm__("s6") = 0xe246bdf0;
    __asm__ volatile("" : "+r"(saved_n), "+r"(saved_z));
    compare(128, 0x390000);
    __asm__ volatile("" : "+r"(saved_n), "+r"(saved_z));
    CHECK(saved_n == 0x138ace57 && saved_z == 0xe246bdf0);
    printf("PASS independent N/Z transport, all NZCV inputs, reserved bits, conditions, native chains, fallback, event/cap cuts, clock wrap and callee-saved ABI: %u comparisons\n", comparisons-before);
}

#include "gba-next-rv32-calls.h"
#include "gba-next-rv32-open-bus.h"

int main(void) {
	CHECK(gbn_rv32_available());
	open_bus_stale_prefetch(); open_bus_reads(); open_bus_boundaries(); open_bus_ram_tail(); open_bus_writeback();
	encodings(); streams(); invalidation(); memory_paths(); resident_blocks(); arm_encodings(); arm_streams(); interpreter_bridge(); ram_code(); ram_dma(); arm_self_modification(); arm_multiply(); arm_transfers(); io_reads(); dma_reads(); dma_writes(); fixed_loops(); native_chains(); cache_pressure(); cost();
	stack_encodings(); stack_boundaries(); stack_streams(); stack_self_modification(); stack_returns();
	resident_flag_paths();
	call_encodings(); call_boundaries();
	return_chains(); return_cache_changes();
	bx_encodings(); bx_calls();
	indirect_wrong_descriptor();
	printf("Native cache storage: %u bytes; code arena=%u bytes\n", (unsigned) sizeof(backend), (unsigned) sizeof(backend.code));
	printf("PASS independent RV32 native backend: %u state comparisons\n", comparisons);
	return 0;
}
