/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_COUNTERS_H
#define AM_COUNTERS_H

#include <stdint.h>

/* Machine-mode RV32 counters. Retrying the high half handles wraparound;
 * the memory clobber keeps benchmark work inside the measurement interval. */
static inline uint64_t am_counter_cycles(void) {
	uint32_t hi, lo, again;
	do {
		__asm__ volatile("csrr %0, mcycleh; csrr %1, mcycle; csrr %2, mcycleh"
		                 : "=r"(hi), "=r"(lo), "=r"(again) : : "memory");
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
}

static inline uint64_t am_counter_retired(void) {
	uint32_t hi, lo, again;
	do {
		__asm__ volatile("csrr %0, minstreth; csrr %1, minstret; csrr %2, minstreth"
		                 : "=r"(hi), "=r"(lo), "=r"(again) : : "memory");
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
}

#endif
