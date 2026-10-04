/* SPDX-License-Identifier: MPL-2.0 */
#include <am-profile.h>
#include "platform.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

extern char _text_start[], _text_end[];
extern void am_trap_fatal(uint32_t cause, uint32_t pc, uint32_t value) __attribute__((noreturn));

static volatile uint32_t* histogram;
static volatile uint32_t samples, outside;
static uint32_t bins, random_state, interval;
static int active;
static uintptr_t old_vector;
static uint32_t old_mie, old_mstatus;
static uint64_t cycles_start, retired_start, cycles, retired;

static inline uint64_t counter_cycle(void) {
	uint32_t hi, lo, again;
	do {
		__asm__ volatile("csrr %0, mcycleh; csrr %1, mcycle; csrr %2, mcycleh"
		                 : "=r"(hi), "=r"(lo), "=r"(again));
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
}

static inline uint64_t counter_retired(void) {
	uint32_t hi, lo, again;
	do {
		__asm__ volatile("csrr %0, minstreth; csrr %1, minstret; csrr %2, minstreth"
		                 : "=r"(hi), "=r"(lo), "=r"(again));
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
}

static void compare_at(uint64_t ticks) {
	volatile uint32_t* compare = (volatile uint32_t*) (AM_SOC_CLINT + 0x4000);
	compare[1] = UINT32_MAX;
	compare[0] = (uint32_t) ticks;
	compare[1] = ticks >> 32;
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

static void next_sample(void) {
	/* Roughly 500 Hz, with deterministic jitter to avoid a periodic workload
	 * repeatedly landing at the same sample phase. Schedule after the ISR. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	compare_at(am_platform_ticks() + interval - interval / 4 + random_state % (interval / 2));
}

__attribute__((interrupt("machine"), aligned(4)))
static void sample_interrupt(void) {
	uint32_t cause, pc, value;
	__asm__ volatile("csrr %0, mcause; csrr %1, mepc; csrr %2, mtval"
	                 : "=r"(cause), "=r"(pc), "=r"(value));
	if (cause != 0x80000007u) am_trap_fatal(cause, pc, value);
	uint32_t offset = pc - (uintptr_t) _text_start;
	if (!(pc & 3) && offset / 4 < bins) ++histogram[offset / 4];
	else ++outside;
	++samples;
	next_sample();
}

void am_profile_prepare(void) {
	bins = ((uintptr_t) _text_end - (uintptr_t) _text_start + 3) / 4;
	histogram = calloc(bins, sizeof(*histogram));
	if (!histogram) { fputs("PC profile allocation failed\n", stderr); exit(1); }
	interval = am_platform_hz() / 500;
	if (interval < 4) { fputs("PC profile timer unavailable\n", stderr); exit(1); }
	random_state = 0x91e10da5;
	samples = outside = 0;
	active = 0;
}

void am_profile_start(void) {
	__asm__ volatile("csrr %0, mstatus; csrci mstatus, 8; csrr %1, mie; csrr %2, mtvec"
	                 : "=r"(old_mstatus), "=r"(old_mie), "=r"(old_vector) : : "memory");
	__asm__ volatile("csrw mtvec, %0; csrw mie, %1"
	                 : : "r"(sample_interrupt), "r"(128) : "memory");
	next_sample();
	cycles_start = counter_cycle();
	retired_start = counter_retired();
	active = 1;
	__asm__ volatile("csrsi mstatus, 8" ::: "memory");
}

void am_profile_stop(void) {
	if (!active) return;
	__asm__ volatile("csrci mstatus, 8" ::: "memory");
	retired = counter_retired() - retired_start;
	cycles = counter_cycle() - cycles_start;
	compare_at(UINT64_MAX);
	__asm__ volatile("csrw mtvec, %0; csrw mie, %1; csrw mstatus, %2"
	                 : : "r"(old_vector), "r"(old_mie), "r"(old_mstatus) : "memory");
	active = 0;
}

void am_profile_report(void) {
	printf("PC profile: samples=%" PRIu32 "; outside=%" PRIu32 "; cycles=%" PRIu64
	       "; instret=%" PRIu64 "; nominal_hz=500; bin_bytes=4\n", samples, outside, cycles, retired);
	for (uint32_t i = 0; i < bins; ++i) {
		if (histogram[i]) printf("PC %08" PRIxPTR " %" PRIu32 "\n", (uintptr_t) _text_start + i * 4, histogram[i]);
	}
	puts("PC profile end");
	free((void*) histogram);
	histogram = NULL;
}
