/* SPDX-License-Identifier: MPL-2.0 */
/* Fixed PPU work for RTL/board cycle comparisons; this is not game FPS. */
#include <gba-next/core.h>
#include "gba-next/internal.h"
#include <am-counters.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static struct Gbn machine;
static struct GbnDevices devices;
_Alignas(16) static uint8_t palette[GBN_PALETTE_SIZE], vram[GBN_VRAM_SIZE], oam[GBN_OAM_SIZE];
static uint16_t pixels[GBN_SCREEN_WIDTH * GBN_SCREEN_HEIGHT];
static uint32_t random_state = 0x58129acf;

static uint32_t random_word(void) {
	random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5;
	return random_state;
}
static void half(uint8_t* bytes, unsigned offset, unsigned value) {
	bytes[offset] = (uint8_t) value; bytes[offset + 1] = (uint8_t) (value >> 8);
}
static bool draw(void) {
	for (unsigned y = 0; y < GBN_SCREEN_HEIGHT; ++y) if (gbn_ppu_line(&machine, y) != GBN_STEP) return false;
	return true;
}
int main(void) {
	gbn_init(&machine, NULL, NULL);
	if (!gbn_attach_devices(&machine, &devices, palette, vram, oam) || !gbn_attach_pixels(&machine, pixels, GBN_SCREEN_WIDTH)) return 1;
	for (unsigned i = 0; i < sizeof(palette); i += 2) half(palette, i, random_word() & 0x7fff);
	for (unsigned i = 0; i < sizeof(vram); ++i) vram[i] = (uint8_t) random_word();
	for (unsigned id = 0; id < 4; ++id) {
		devices.io[4 + id] = (uint16_t) ((16 + id) << 8 | id);
		devices.io[8 + 2 * id] = (uint16_t) (id * 17);
		devices.io[9 + 2 * id] = (uint16_t) (id * 11);
		for (unsigned i = 0; i < 0x800; i += 2) half(vram, 0x8000 + id * 0x800 + i, random_word() & 0xfcff);
	}
	for (unsigned id = 0; id < 128; ++id) half(oam, id * 8, 0x200);
	for (unsigned id = 0; id < 8; ++id) {
		half(oam, id * 8, id * 17); half(oam, id * 8 + 2, 20 + id * 19);
		half(oam, id * 8 + 4, id << 10 | id * 8);
	}
	devices.io[0x40 / 2] = 0x20d0; devices.io[0x42 / 2] = 0x50a0;
	devices.io[0x44 / 2] = devices.io[0x46 / 2] = 0x00a0;
	devices.io[0x48 / 2] = 0x2d3f; devices.io[0x4a / 2] = 0x3f3f;
	static const unsigned configurations[] = {0x0080, 0x0100, 0x7f40, 0x7f40, 0x7f40};
	puts("PPU microbenchmark: 2 measured frames; no guest CPU/APU; no game FPS claim");
	for (unsigned test = 0; test < sizeof(configurations) / sizeof(*configurations); ++test) {
		/* A separate sparse/solid fixture: 64% transparent tiles, 8% solid,
		 * 28% random. Keep the four random-data cases unchanged. */
		if (test == 4) for (unsigned tile = 0; tile < 185; ++tile)
			memset(vram + tile * 32, tile < 164 ? 0 : (int) ((tile % 15 + 1) * 17), 32);
		devices.io[0] = (uint16_t) configurations[test];
		devices.io[0x50 / 2] = test == 3 ? 0x3f7f : 0x00ff;
		devices.io[0x52 / 2] = 0x0808; devices.io[0x54 / 2] = 0;
		if (!draw()) return 2;
		uint64_t cycles = am_counter_cycles(), retired = am_counter_retired();
		if (!draw() || !draw()) return 2;
		retired = am_counter_retired() - retired; cycles = am_counter_cycles() - cycles;
		uint32_t crc = 0x811c9dc5;
		for (unsigned i = 0; i < sizeof(pixels) / sizeof(*pixels); ++i) crc = (crc ^ pixels[i]) * 0x01000193;
		printf("PPU micro: case=%u; frames=2; cycles=%" PRIu64 "; instret=%" PRIu64 "; hash=%08" PRIx32 "\n", test, cycles, retired, crc);
	}
	return 0;
}
