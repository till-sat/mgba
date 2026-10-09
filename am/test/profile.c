/* SPDX-License-Identifier: MPL-2.0 */
/* Include the collector to exercise bounded overflow without filling it via
 * hours of timer interrupts. The second half uses real CLINT interrupts. */
#include "../src/protosoc/profile.c"
#include <am.h>

#define CHECK(expr) do { if (!(expr)) { printf("FAIL profile line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static uint32_t current_block;
static bool test_resolver(uintptr_t pc, struct am_profile_location* location, void* context) {
	(void) context;
	if (pc != 0x12340000) return false;
	*location = (struct am_profile_location) { .kind = 1, .address = current_block, .offset = 4 };
	return true;
}

int main(void) {
	struct am_config config = {0};
	CHECK(am_init(&config));
	am_profile_set_resolver(test_resolver, NULL);
	jit = calloc(JIT_BINS, sizeof(*jit));
	CHECK(jit);
	current_block = 0x08000100;
	sample_generated(0x12340000);
	sample_generated(0x12340000);
	current_block = 0x08000200;
	sample_generated(0x12340000);
	unsigned old_count = 0, new_count = 0;
	for (unsigned i = 0; i < JIT_BINS; ++i) {
		if (jit[i].location.address == 0x08000100) old_count += jit[i].count;
		if (jit[i].location.address == 0x08000200) new_count += jit[i].count;
		jit[i].location = (struct am_profile_location) { .kind = 7 };
		jit[i].count = 1;
	}
	CHECK(old_count == 2 && new_count == 1 && generated == 3 && !dropped);
	sample_generated(0x12340000);
	CHECK(generated == 4 && dropped == 1);
	sample_generated(0);
	CHECK(generated == 4 && dropped == 1);
	free((void*) jit);
	jit = NULL;
	am_profile_prepare();
	CHECK(!generated && !dropped && !samples);
	uint32_t vector_before, mie_before, status_before, vector_after, mie_after, status_after;
	__asm__ volatile("csrr %0, mtvec; csrr %1, mie; csrr %2, mstatus"
	                 : "=r"(vector_before), "=r"(mie_before), "=r"(status_before));
	am_profile_start();
	while (samples < 3) __asm__ volatile("nop");
	am_profile_stop();
	__asm__ volatile("csrr %0, mtvec; csrr %1, mie; csrr %2, mstatus"
	                 : "=r"(vector_after), "=r"(mie_after), "=r"(status_after));
	CHECK(vector_before == vector_after && mie_before == mie_after && ((status_before ^ status_after) & 8) == 0);
	CHECK(cycles && retired && !outside && !generated && !dropped);
	uint32_t collected = 0;
	for (uint32_t i = 0; i < bins; ++i) collected += histogram[i];
	CHECK(collected == samples && samples >= 3);
	am_profile_report();
	CHECK(!jit && !histogram && !resolve);
	am_shutdown();
	puts("PASS: profile slot reuse, bounded overflow, CLINT sampling, counters and interrupt-state restoration");
	return 0;
}
