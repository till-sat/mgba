/* SPDX-License-Identifier: MPL-2.0 */
#ifndef AM_YSYXSOC_PLATFORM_H
#define AM_YSYXSOC_PLATFORM_H

#include <stdint.h>

/* ysyxSoC memory map used by the upstream SoC and NPC simulator. */
#define AM_YSYXSOC_CLINT       0x02000000u
#define AM_YSYXSOC_SRAM        0x0f000000u
#define AM_YSYXSOC_UART        0x10000000u
#define AM_YSYXSOC_GPIO        0x10002000u
#define AM_YSYXSOC_VGACTL      0x21000000u
#define AM_YSYXSOC_FB          0x21001000u
#define AM_YSYXSOC_FB_WIDTH    400u
#define AM_YSYXSOC_FB_HEIGHT   300u
#define AM_YSYXSOC_FLASH       0x30000000u
#define AM_YSYXSOC_PSRAM       0x80000000u
#define AM_YSYXSOC_SDRAM       0xa0000000u
#define AM_YSYXSOC_SDRAM_SIZE  0x02000000u

void am_platform_boot(void);
void am_platform_putch(char ch);
uint64_t am_platform_ticks(void);
unsigned am_platform_hz(void);
__attribute__((noreturn)) void am_platform_exit(int code);

#endif
