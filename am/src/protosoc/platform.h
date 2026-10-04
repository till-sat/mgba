/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_PROTOSOC_PLATFORM_H
#define AM_PROTOSOC_PLATFORM_H
#include <stdint.h>

/* proto-soc address-map v3, shared by the driver and the Spike device model. */
#define AM_SOC_CLINT       0x02000000u
#define AM_SOC_UART        0x10000000u
#define AM_SOC_GPIO        0x10002000u
#define AM_SOC_SYSCTRL     0x1000f000u
#define AM_SOC_SDRAM       0xa0000000u
#define AM_SOC_SDRAM_SIZE  0x04000000u
#define AM_SOC_MAP_VERSION 3u

void am_platform_boot(void);
void am_platform_putch(char ch);
uint64_t am_platform_ticks(void);
unsigned am_platform_hz(void);
__attribute__((noreturn)) void am_platform_exit(int code);
#endif
