/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_PERF_H
#define AM_PERF_H

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

/* Diagnostic quad-issue-rvv-perf event map, version 1. Each CSR selector
 * accepts only zero or its counter number. Ordinary FPGA images omit this
 * interface; probing the previous core's zero stubs fails cleanly. */
#define AM_PERF_EVENTS(X) \
	X(3, cycles) \
	X(4, issue0) X(5, issue1) X(6, issue2) X(7, issue3) X(8, issue4) \
	X(9, zero_flush) X(10, zero_hold) X(11, zero_lsu_window) \
	X(12, zero_empty) X(13, zero_score) X(14, zero_unit_busy) X(15, zero_other) \
	X(16, partial_empty) X(17, partial_decode_slot0) X(18, partial_control) \
	X(19, partial_lsu_group) X(20, partial_mul_group) X(21, partial_score) \
	X(22, partial_unit_busy) X(23, partial_other) \
	X(24, ibus_wait) X(25, dbus_wait) X(26, conditional_redirect) \
	X(27, all_branch_redirect) X(28, load_issue) X(29, store_issue) \
	X(30, mdu_issue) X(31, vector_hold)

static inline void am_perf_stop(void) {
	uint32_t mask = UINT32_C(0xFFFFFFF8);
	__asm__ volatile("csrw 0x320, %0" : : "r"(mask) : "memory");
}

static inline bool am_perf_prepare(void) {
	am_perf_stop();
#define PREPARE(N, NAME) do { \
	uint32_t readback; \
	__asm__ volatile("csrw %1, %2; csrr %0, %1" \
	                 : "=r"(readback) : "i"(0x320 + N), "r"((uint32_t) N) : "memory"); \
	if (readback != N) return false; \
	__asm__ volatile("csrw %0, zero; csrw %1, zero" \
	                 : : "i"(0xB00 + N), "i"(0xB80 + N) : "memory"); \
} while (0);
	AM_PERF_EVENTS(PREPARE)
#undef PREPARE
	return true;
}

static inline void am_perf_start(void) {
	__asm__ volatile("csrw 0x320, zero" : : : "memory");
}

/* All HPM counters are frozen by one CSR write before reading either half.
 * The PMU's events are registered together, so their partitions are exact
 * even though the window includes a few boundary instructions. Bus waits
 * and redirects overlap dispatch categories and must not be added to them. */
static inline bool am_perf_report(void) {
	uint64_t counts[32] = { 0 };
#define READ(N, NAME) do { \
	uint32_t hi, lo; \
	__asm__ volatile("csrr %0, %2; csrr %1, %3" \
	                 : "=r"(hi), "=r"(lo) : "i"(0xB80 + N), "i"(0xB00 + N) : "memory"); \
	counts[N] = ((uint64_t) hi << 32) | lo; \
} while (0);
	AM_PERF_EVENTS(READ)
#undef READ
	uint64_t widths = 0, zero = 0, partial = 0;
	for (unsigned n = 4; n <= 8; ++n) widths += counts[n];
	for (unsigned n = 9; n <= 15; ++n) zero += counts[n];
	for (unsigned n = 16; n <= 23; ++n) partial += counts[n];
	bool valid = counts[3] && widths == counts[3] && zero == counts[4] &&
	             partial == counts[5] + counts[6] + counts[7];
	printf("PMU: version=1; diagnostic=1; partitions=%s\n", valid ? "PASS" : "FAIL");
#define PRINT(N, NAME) printf("PMU %u " #NAME "=%" PRIu64 "\n", (unsigned) N, counts[N]);
	AM_PERF_EVENTS(PRINT)
#undef PRINT
	return valid;
}

#endif
