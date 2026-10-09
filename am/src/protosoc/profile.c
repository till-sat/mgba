/* SPDX-License-Identifier: MPL-2.0 */
#include <am-profile.h>
#include <am-counters.h>
#include "platform.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef AM_PROFILE_HZ
#define AM_PROFILE_HZ 500
#endif
_Static_assert(AM_PROFILE_HZ > 0, "PC profile rate must be positive");

extern char _text_start[], _text_end[];
extern void am_trap_fatal(uint32_t cause, uint32_t pc, uint32_t value) __attribute__((noreturn));

static volatile uint32_t* histogram;
static volatile uint32_t samples, outside;
static uint32_t bins, random_state, interval;
static int active;
static uintptr_t old_vector;
static uint32_t old_mie, old_mstatus;
static uint64_t cycles_start, retired_start, cycles, retired;

#define JIT_BINS 8192
#define JIT_MAX_PROBE 8
struct jit_entry {
	struct am_profile_location location;
	uint32_t count;
};
static volatile struct jit_entry* jit;
static volatile uint32_t generated, dropped;
static am_profile_resolver resolve;
static void* resolve_context;

void am_profile_set_resolver(am_profile_resolver resolver, void* context) {
	resolve = resolver;
	resolve_context = context;
}

static void sample_generated(uintptr_t pc) {
	struct am_profile_location location;
	if (!jit || !resolve(pc, &location, resolve_context) || !location.kind) return;
	++generated;
	uint32_t hash = location.address ^ (location.offset * 0x9e3779b9u) ^ (location.kind * 0x85ebca6bu);
	hash ^= hash >> 16;
	/* Keep ISR time bounded even when the table fills. Report lost samples
	 * explicitly, so the retained entries cannot masquerade as full coverage. */
	for (unsigned probe = 0; probe < JIT_MAX_PROBE; ++probe) {
		volatile struct jit_entry* entry = &jit[(hash + probe) & (JIT_BINS - 1)];
		if (!entry->count) {
			entry->location = location;
			entry->count = 1;
			return;
		}
		if (entry->location.kind == location.kind && entry->location.address == location.address &&
		    entry->location.offset == location.offset) {
			++entry->count;
			return;
		}
	}
	++dropped;
}

static void compare_at(uint64_t ticks) {
	volatile uint32_t* compare = (volatile uint32_t*) (AM_SOC_CLINT + 0x4000);
	compare[1] = UINT32_MAX;
	compare[0] = (uint32_t) ticks;
	compare[1] = ticks >> 32;
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

static void next_sample(void) {
	/* Deterministic jitter avoids a periodic workload
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
	else { ++outside; sample_generated(pc); }
	++samples;
	next_sample();
}

void am_profile_prepare(void) {
	bins = ((uintptr_t) _text_end - (uintptr_t) _text_start + 3) / 4;
	histogram = calloc(bins, sizeof(*histogram));
	if (!histogram) { fputs("PC profile allocation failed\n", stderr); exit(1); }
	jit = resolve ? calloc(JIT_BINS, sizeof(*jit)) : NULL;
	if (resolve && !jit) { fputs("JIT profile allocation failed\n", stderr); exit(1); }
	interval = am_platform_hz() / AM_PROFILE_HZ;
	if (interval < 4) { fputs("PC profile timer unavailable\n", stderr); exit(1); }
	random_state = 0x91e10da5;
	samples = outside = generated = dropped = 0;
	active = 0;
}

void am_profile_start(void) {
	__asm__ volatile("csrr %0, mstatus; csrci mstatus, 8; csrr %1, mie; csrr %2, mtvec"
	                 : "=r"(old_mstatus), "=r"(old_mie), "=r"(old_vector) : : "memory");
	__asm__ volatile("csrw mtvec, %0; csrw mie, %1"
	                 : : "r"(sample_interrupt), "r"(128) : "memory");
	next_sample();
	cycles_start = am_counter_cycles();
	retired_start = am_counter_retired();
	active = 1;
	__asm__ volatile("csrsi mstatus, 8" ::: "memory");
}

void am_profile_stop(void) {
	if (!active) return;
	__asm__ volatile("csrci mstatus, 8" ::: "memory");
	retired = am_counter_retired() - retired_start;
	cycles = am_counter_cycles() - cycles_start;
	compare_at(UINT64_MAX);
	__asm__ volatile("csrw mtvec, %0; csrw mie, %1; csrw mstatus, %2"
	                 : : "r"(old_vector), "r"(old_mie), "r"(old_mstatus) : "memory");
	active = 0;
}

void am_profile_report(void) {
	printf("PC profile: samples=%" PRIu32 "; outside=%" PRIu32 "; cycles=%" PRIu64
	       "; instret=%" PRIu64 "; nominal_hz=%u; bin_bytes=4\n", samples, outside, cycles, retired, (unsigned) AM_PROFILE_HZ);
	for (uint32_t i = 0; i < bins; ++i) {
		if (histogram[i]) printf("PC %08" PRIxPTR " %" PRIu32 "\n", (uintptr_t) _text_start + i * 4, histogram[i]);
	}
	printf("JIT profile: samples=%" PRIu32 "; dropped=%" PRIu32 "; bins=%u; max_probe=%u\n",
	       generated, dropped, JIT_BINS, JIT_MAX_PROBE);
	if (jit) for (unsigned i = 0; i < JIT_BINS; ++i) {
		if (jit[i].count) printf("JIT %" PRIu32 " %08" PRIx32 " %08" PRIx32 " %" PRIu32 "\n",
		                        jit[i].location.kind, jit[i].location.address, jit[i].location.offset, jit[i].count);
	}
	puts("PC profile end");
	free((void*) histogram);
	histogram = NULL;
	free((void*) jit);
	jit = NULL;
	resolve = NULL;
	resolve_context = NULL;
}
