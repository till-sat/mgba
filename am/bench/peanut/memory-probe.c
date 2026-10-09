/* SPDX-License-Identifier: MPL-2.0 */
/* A 44-byte position-independent pointer chase fits even the parked TCMs.
 * This measures dependent-load latency, not whole-program memory costs. */
#include <am-counters.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../src/protosoc/platform.h"
extern const uint8_t chase_start[], chase_end[];
__asm__(
".pushsection .text.chase,\"ax\",@progbits\n"
".balign 32\n"
".global chase_start, chase_end\n"
"chase_start:\n"
"1:\n"
".rept 8\n"
"lw a0, 0(a0)\n"
".endr\n"
"addi a1, a1, -1\n"
"bnez a1, 1b\n"
"ret\n"
"chase_end:\n"
".popsection\n");
static uint32_t ddr[16] __attribute__((aligned(64)));
static uint32_t working[262144] __attribute__((aligned(32768)));
typedef uintptr_t (*chase_fn)(uintptr_t, unsigned);
static void init_chain(volatile uint32_t* p) {
	for (unsigned i = 0; i < 16; ++i) p[i] = (uintptr_t)&p[(i + 7) & 15];
}
static uint32_t jump(int offset) {
	uint32_t x = (uint32_t)offset;
	return ((x & 0x100000) << 11) | ((x & 0x7fe) << 20) |
	       ((x & 0x800) << 9) | (x & 0xff000) | 0x6f;
}
void memory_probe(void) {
	unsigned code_size = (uintptr_t)chase_end - (uintptr_t)chase_start;
	unsigned isize = *(volatile uint32_t*)(AM_SOC_SYSCTRL + 0x18);
	unsigned dsize = *(volatile uint32_t*)(AM_SOC_SYSCTRL + 0x1c);
	if (isize < code_size || dsize < 64) return;
	init_chain(ddr);
	init_chain((volatile uint32_t*)0x80100000u);
	memcpy((void*)0x80000000u, chase_start, code_size);
	__asm__ volatile("fence iorw,iorw; fence.i" ::: "memory");
	for (unsigned rep = 0; rep < 3; ++rep) {
		for (unsigned i = 0; i < 4; ++i) {
			chase_fn fn = (chase_fn)(i & 2 ? (uintptr_t)0x80000000u : (uintptr_t)chase_start);
			uintptr_t data = i & 1 ? (uintptr_t)0x80100000u : (uintptr_t)ddr;
			uintptr_t result = fn(data, 1024);
			uint64_t cycles = am_counter_cycles(), retired = am_counter_retired();
			result = fn(result, 16384);
			retired = am_counter_retired() - retired;
			cycles = am_counter_cycles() - cycles;
			printf("Memory probe: rep=%u; code=%s; data=%s; loads=131072; cycles=%" PRIu64
			       "; instret=%" PRIu64 "; valid=%u\n", rep, i & 2 ? "ITCM" : "DDR",
			       i & 1 ? "DTCM" : "DDR", cycles, retired, result == data);
			if (result != data) abort();
		}
	}
	/* One dependent read per cache line, using a full-period permutation.
	 * The instruction sequence and retirement count stay fixed across sizes. */
	for (unsigned bytes = 4096; bytes <= sizeof(working); bytes *= 2) {
		unsigned nodes = bytes / 32;
		for (unsigned n = 0; n < nodes; ++n)
			working[n * 8] = (uintptr_t)&working[((n * 5 + 1) & (nodes - 1)) * 8];
		chase_fn fn = (chase_fn)(uintptr_t)chase_start;
		uintptr_t data = (uintptr_t)working;
		fn(data, 8192);
		for (unsigned rep = 0; rep < 2; ++rep) {
			uint64_t cycles = am_counter_cycles(), retired = am_counter_retired();
			uintptr_t result = fn(data, 16384);
			retired = am_counter_retired() - retired;
			cycles = am_counter_cycles() - cycles;
			printf("Data footprint: bytes=%u; rep=%u; loads=131072; cycles=%" PRIu64
			       "; instret=%" PRIu64 "; valid=%u\n", bytes, rep, cycles, retired, result == data);
			if (result != data) abort();
		}
	}
	/* Four retired instructions per block. Only instruction working-set size
	 * changes; no data load or store occurs in the measured kernel. */
	for (unsigned bytes = 4096; bytes <= 262144; bytes *= 2) {
		unsigned nodes = bytes / 32;
		for (unsigned n = 0; n < nodes; ++n) {
			unsigned next = (n * 5 + 1) & (nodes - 1);
			uint32_t* p = &working[n * 8];
			p[0] = 0x00150513; /* addi a0,a0,1 */
			p[1] = 0xfff58593; /* addi a1,a1,-1 */
			p[2] = 0x00058463; /* beq a1,zero,+8 */
			p[3] = jump(((int)next - (int)n) * 32 - 12);
			p[4] = 0x00008067; /* ret */
			p[5] = p[6] = p[7] = 0x00000013;
		}
		for (unsigned n = 0; n < bytes / 4; n += 8)
			__asm__ volatile("cbo.flush (%0)" :: "r"(&working[n]) : "memory");
		__asm__ volatile("fence iorw,iorw; fence.i" ::: "memory");
		chase_fn fn = (chase_fn)(uintptr_t)working;
		fn(0, 32768);
		for (unsigned rep = 0; rep < 2; ++rep) {
			uint64_t cycles = am_counter_cycles(), retired = am_counter_retired();
			uintptr_t result = fn(0, 32768);
			retired = am_counter_retired() - retired;
			cycles = am_counter_cycles() - cycles;
			printf("Code footprint: bytes=%u; rep=%u; blocks=32768; cycles=%" PRIu64
			       "; instret=%" PRIu64 "; valid=%u\n", bytes, rep, cycles, retired, result == 32768);
			if (result != 32768) abort();
		}
	}
}
