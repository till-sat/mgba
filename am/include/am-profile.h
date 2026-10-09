/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_PROFILE_H
#define AM_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

struct am_profile_location {
	uint32_t kind;
	uint32_t address;
	uint32_t offset;
};

/* Called in the timer ISR for PCs outside static text. Must not allocate,
 * print, or mutate the interrupted program; kind must be nonzero on success. */
typedef bool (*am_profile_resolver)(uintptr_t pc, struct am_profile_location* location, void* context);
/* Set before prepare; keep the resolver/context alive until report returns. */
void am_profile_set_resolver(am_profile_resolver resolver, void* context);

/* Optional proto-soc PC sampling; output is outside the timed loop. */
void am_profile_prepare(void);
void am_profile_start(void);
void am_profile_stop(void);
void am_profile_report(void);

#endif
