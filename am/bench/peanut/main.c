/* SPDX-License-Identifier: MPL-2.0 */
/* The Pico-GB core, pinned verbatim in vendor/. No frame skipping, no audio,
 * no physical display. Pixel conversion is included; validation is untimed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "hedley.h"
#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_BIOS 0
#include "peanut_gb.h"
#ifdef AM_BAREMETAL
#include <am.h>
#include <am-counters.h>
#include "../../src/protosoc/platform.h"
#ifdef BENCH_MEMORY_PROBE
void memory_probe(void);
#endif
#else
#include <time.h>
static uint64_t am_uptime_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#endif
#ifndef BENCH_FRAMES
#define BENCH_FRAMES 120
#endif
#ifndef BENCH_WARMUP
#define BENCH_WARMUP 30
#endif
#ifndef BENCH_INPUT
#define BENCH_INPUT 0
#endif
extern const uint8_t _rom_start[], _rom_end[];
static struct gb_s gb;
static uint8_t cart_ram[32768];
static uint16_t pixels[LCD_HEIGHT][LCD_WIDTH];
static unsigned lines;
static void run_frame(unsigned frame) {
	/* Deterministic optional play sequence: Start at frame 60, followed by
	 * alternating direction presses every 20 frames. Active-low Pico mapping. */
	uint8_t key = 0;
	if (BENCH_INPUT && frame >= 60 && frame < 65) key = 0x08;
	if (BENCH_INPUT && frame >= 100 && frame % 20 < 5) key = 0x10u << ((frame / 20) & 3);
	gb.direct.joypad = (uint8_t)~key;
	gb_run_frame(&gb);
}

static uint8_t read_rom(struct gb_s* context, uint_fast32_t addr) {
	(void)context;
	return addr < (size_t)(_rom_end - _rom_start) ? _rom_start[addr] : 0xff;
}
static uint8_t read_ram(struct gb_s* context, uint_fast32_t addr) {
	(void)context;
	return cart_ram[addr & (sizeof(cart_ram) - 1)];
}
static void write_ram(struct gb_s* context, uint_fast32_t addr, uint8_t value) {
	(void)context;
	cart_ram[addr & (sizeof(cart_ram) - 1)] = value;
}
static void error(struct gb_s* context, enum gb_error_e code, uint16_t addr) {
	(void)context;
	printf("GB error: %u at %04x\n", (unsigned)code, (unsigned)addr);
	exit(1);
}
static void draw_line(struct gb_s* context, const uint8_t* shades, uint_fast8_t line) {
	(void)context;
	static const uint16_t palette[] = {0xffff, 0xad55, 0x52aa, 0x0000};
	if (line >= LCD_HEIGHT) abort();
	for (unsigned x = 0; x < LCD_WIDTH; ++x) pixels[line][x] = palette[shades[x] & 3];
	++lines;
}
static uint32_t crc32_bytes(const void* data, size_t size) {
	const uint8_t* p = data;
	uint32_t crc = ~0u;
	while (size--) {
		crc ^= *p++;
		for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
	}
	return ~crc;
}
int main(void) {
#ifdef AM_BAREMETAL
	struct am_config config = {0};
	if (!am_init(&config)) return 1;
	printf("Hardware: hz=%u; itcm=%lu; dtcm=%lu\n", am_platform_hz(),
	       (unsigned long)*(volatile uint32_t*)(AM_SOC_SYSCTRL + 0x18),
	       (unsigned long)*(volatile uint32_t*)(AM_SOC_SYSCTRL + 0x1c));
#ifdef BENCH_MEMORY_PROBE
	memory_probe();
#endif
#endif
	printf("Peanut-GB: Pico-GB 68dbc870; audio=0; lcd=1; RGB565=1; skip=0; input=%u\n", BENCH_INPUT);
	printf("ROM: bytes=%lu; CRC32=%08lx\n", (unsigned long)(_rom_end - _rom_start),
	       (unsigned long)crc32_bytes(_rom_start, _rom_end - _rom_start));
	memset(cart_ram, 0xff, sizeof(cart_ram));
	if (gb_init(&gb, read_rom, read_ram, write_ram, error, NULL) != GB_INIT_NO_ERROR) return 1;
	gb_init_lcd(&gb, draw_line);
	gb.direct.frame_skip = false;
	gb.direct.interlace = false;
	gb.direct.joypad = 0xff;
	for (unsigned i = 0; i < BENCH_WARMUP; ++i) run_frame(i);
	lines = 0;
	uint64_t start = am_uptime_us();
#ifdef AM_BAREMETAL
	uint64_t cycles = am_counter_cycles(), retired = am_counter_retired();
#endif
	for (unsigned i = 0; i < BENCH_FRAMES; ++i) run_frame(BENCH_WARMUP + i);
#ifdef AM_BAREMETAL
	retired = am_counter_retired() - retired;
	cycles = am_counter_cycles() - cycles;
#endif
	uint64_t elapsed = am_uptime_us() - start;
	if (!elapsed || lines != BENCH_FRAMES * LCD_HEIGHT) {
		printf("Incomplete run: elapsed=%" PRIu64 "; lines=%u\n", elapsed, lines);
		return 1;
	}
	uint64_t fps = (uint64_t)BENCH_FRAMES * 1000000000 / elapsed;
	printf("Benchmark: warmup=%u; frames=%u; elapsed_us=%" PRIu64 "; FPS=%" PRIu64 ".%03" PRIu64 "\n",
	       BENCH_WARMUP, BENCH_FRAMES, elapsed, fps / 1000, fps % 1000);
#ifdef AM_BAREMETAL
	printf("Counters: cycles=%" PRIu64 "; instret=%" PRIu64 "; sampling=0\n", cycles, retired);
#endif
	printf("Validation: lines=%u; framebuffer=%08lx; wram=%08lx; vram=%08lx; pc=%04x\n",
	       lines, (unsigned long)crc32_bytes(pixels, sizeof(pixels)),
	       (unsigned long)crc32_bytes(gb.wram, sizeof(gb.wram)),
	       (unsigned long)crc32_bytes(gb.vram, sizeof(gb.vram)), (unsigned)gb.cpu_reg.pc.reg);
#ifndef AM_BAREMETAL
	const char* dump = getenv("PEANUT_FRAMEBUFFER");
	if (dump) {
		FILE* f = fopen(dump, "wb");
		if (!f) return 1;
		int ok = fwrite(pixels, sizeof(pixels), 1, f) == 1;
		if (fclose(f) || !ok) return 1;
	}
#else
	am_shutdown();
#endif
	return 0;
}
