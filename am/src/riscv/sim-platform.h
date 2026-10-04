/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_SIM_PLATFORM_H
#define AM_SIM_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
bool am_platform_valid(void);
uint32_t am_platform_buttons(void);
uint64_t am_platform_ticks(void);
unsigned am_platform_hz(void);
#endif
