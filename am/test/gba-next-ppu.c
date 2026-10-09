/* SPDX-License-Identifier: MPL-2.0 */
/* Renderer oracle only; the production core has no dependency on mGBA. */
#include <gba-next/core.h>
#include "gba-next/internal.h"
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba-util/image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Gbn m;
static struct GbnDevices d;
static struct GBAVideoSoftwareRenderer reference;
static uint8_t ewram[GBN_EWRAM_SIZE], iwram[GBN_IWRAM_SIZE];
static uint16_t memory[(GBN_PALETTE_SIZE + GBN_VRAM_SIZE + GBN_OAM_SIZE) / 2];
static uint16_t pixels[248 * 160 + 1];
static mColor expected[248 * 160];
static unsigned random_state = 0x1928b4d7, comparisons;
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static unsigned random_value(void) {
	random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5;
	return random_state;
}

static void store(uint8_t* data, unsigned offset, uint16_t value) {
	data[offset] = (uint8_t) value; data[offset + 1] = (uint8_t) (value >> 8);
}

static void reg(unsigned offset, uint16_t value) {
	uint32_t cycles;
	CHECK(gbn_write(&m, 0x04000000 + offset, 2, value, &cycles) == GBN_STEP);
	reference.d.writeVideoRegister(&reference.d, offset, value);
}

static void setup(void) {
	gbn_init(&m, ewram, iwram);
	uint8_t* video = (uint8_t*) memory;
	CHECK(gbn_attach_devices(&m, &d, video, video + GBN_PALETTE_SIZE, video + GBN_PALETTE_SIZE + GBN_VRAM_SIZE));
	for (unsigned i = 0; i < sizeof(memory) / 2; ++i) memory[i] = (uint16_t) random_value();
	for (unsigned id = 0; id < 128; ++id) store(d.oam, id * 8, 0x200);
	GBAVideoSoftwareRendererCreate(&reference);
	reference.d.palette = (uint16_t*) d.palette; reference.d.vram = (uint16_t*) d.vram; reference.d.oam = (union GBAOAM*) d.oam;
	reference.outputBuffer = expected; reference.outputBufferStride = 248;
	reference.d.init(&reference.d);
	for (unsigned i = 0; i < sizeof(pixels) / sizeof(*pixels); ++i) pixels[i] = 0xbeef;
	CHECK(!gbn_attach_pixels(&m, pixels, 239));
	CHECK(gbn_attach_pixels(&m, pixels, 248));
	for (unsigned offset = 8; offset < 0x56; offset += 2) if (offset != 0x4e) reg(offset, d.io[offset / 2]);
}

static void compare(unsigned mode, unsigned trial, unsigned effect) {
	for (unsigned y = 0; y < 160; ++y) {
		CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
		reference.d.drawScanline(&reference.d, (int) y);
		for (unsigned x = 0; x < 240; ++x) {
			uint16_t want = (uint16_t) mColorConvert(expected[y * 248 + x], mCOLOR_NATIVE, mCOLOR_BGR5) & 0x7fff;
			if (pixels[y * 248 + x] != want) {
				fprintf(stderr, "PPU mode=%u trial=%u effect=%u pixel=%u,%u color=%04x/%04x native=%08x dispcnt=%04x\n",
					mode,trial,effect,x,y,pixels[y * 248 + x],want,(unsigned) expected[y * 248 + x],d.io[0]);
				fprintf(stderr, "bitmap=%04x refs=%d,%d old=%d,%d control=%04x windows=%d,%d\n", (unsigned) ((uint16_t*)d.vram)[0],
					d.ppu.reference[0][0],d.ppu.reference[0][1],reference.bg[2].sx,reference.bg[2].sy,d.io[6],reference.winN[0].on,reference.winN[1].on); exit(1);
			}
			++comparisons;
		}
		for (unsigned x = 240; x < 248; ++x) CHECK(pixels[y * 248 + x] == 0xbeef);
	}
	CHECK(pixels[248 * 160] == 0xbeef && d.ppu.frames && d.ppu.lines % 160 == 0);
	reference.d.finishFrame(&reference.d);
}

/* The former independent pixel sampler is retained as an oracle for tile
 * spans. It computes each output separately, with no span/line predecode. */
static int text_reference(unsigned x, unsigned y) {
	unsigned control = d.io[4], size = control >> 14;
	if (control & 0x40) {
		x -= x % ((d.io[0x4c / 2] & 15) + 1);
		y -= y % ((d.io[0x4c / 2] >> 4 & 15) + 1);
	}
	x = (x + d.io[8]) & (size & 1 ? 511 : 255);
	y = (y + d.io[9]) & (size & 2 ? 511 : 255);
	unsigned block = (x >> 8) + (y >> 8) * (size & 1 ? 2 : 1);
	unsigned map = ((control >> 8 & 31) * 0x800 + block * 0x800 + ((y & 255) / 8 * 32 + (x & 255) / 8) * 2) & 0xffff;
	unsigned tile = d.vram[map] | (unsigned) d.vram[map + 1] << 8;
	x &= 7; y &= 7;
	if (tile & 0x400) x = 7 - x;
	if (tile & 0x800) y = 7 - y;
	unsigned address = (control >> 2 & 3) * 0x4000, index;
	if (control & 0x80) {
		address += (tile & 0x3ff) * 64 + y * 8 + x;
		if (address >= 0x10000) return -1;
		index = d.vram[address];
	} else {
		address += (tile & 0x3ff) * 32 + y * 4 + x / 2;
		if (address >= 0x10000) return -1;
		index = d.vram[address] >> ((x & 1) * 4) & 15;
		if (index) index += (tile >> 12) * 16;
	}
	if (!index) return -1;
	return (d.palette[index * 2] | (unsigned) d.palette[index * 2 + 1] << 8) & 0x7fff;
}

static void text_edges(void) {
	static const unsigned offsets[] = {0,1,7,8,15,247,248,255,256,511};
	static const unsigned mosaics[] = {1,2,7,16};
	unsigned before = comparisons;
	setup(); reg(0, 0x0100);
	for (unsigned depth = 0; depth < 2; ++depth) for (unsigned size = 0; size < 4; ++size)
	for (unsigned mi = 0; mi < 4; ++mi) for (unsigned base = 0; base < 2; ++base)
	for (unsigned character = 0; character < 2; ++character) for (unsigned trial = 0; trial < 10; ++trial) {
		reg(8, (uint16_t) (size << 14 | (base ? 31 : 0) << 8 | depth << 7 | 0x40 | (character ? 3 : 0) << 2));
		reg(0x10, (uint16_t) offsets[trial]); reg(0x12, (uint16_t) offsets[(trial + size) % 10]);
		reg(0x4c, (uint16_t) ((mosaics[mi] - 1) * 0x11));
		unsigned y = (trial * 17 + size * 29) % 160;
		CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
		for (unsigned x = 0; x < 240; ++x) {
			int want = text_reference(x, y);
			if (want < 0) want = memory[0] & 0x7fff;
			if (pixels[y * 248 + x] != want) {
				fprintf(stderr, "text span depth=%u size=%u mosaic=%u base=%u char=%u scroll=%u,%u xy=%u,%u got=%04x want=%04x\n", depth, size, mosaics[mi], base, character, d.io[8], d.io[9], x, y, pixels[y * 248 + x], want);
				exit(1);
			}
			++comparisons;
		}
		for (unsigned x = 240; x < 248; ++x) CHECK(pixels[y * 248 + x] == 0xbeef);
	}
	reference.d.deinit(&reference.d);
	printf("PASS text spans: %u pixel comparisons, partial tiles, scroll/map wrap, 4/8-bit flips, mosaic and character bounds\n", comparisons - before);
}

/* Exercise the indexed path directly: mosaic and effects stay disabled.
 * Unlike text_edges, sweep every horizontal scroll value, including all
 * partial-tile offsets, with all four host VRAM alignment residues. */
static void indexed_map_edges(void) {
    _Alignas(4) static uint8_t video[GBN_VRAM_SIZE + 4];
    static const unsigned bases[] = {0, 15, 30, 31};
    unsigned before = comparisons;
    for (unsigned alignment = 0; alignment < 4; ++alignment) {
        setup();
        reg(0, 0x0100); reg(0x50, 0);
        memcpy(video + alignment, d.vram, GBN_VRAM_SIZE);
        d.vram = video + alignment;
        for (unsigned size = 0; size < 4; ++size)
        for (unsigned base = 0; base < 4; ++base)
        for (unsigned scroll = 0; scroll < 512; ++scroll) {
            unsigned character = (scroll / 128 + base) & 3;
            unsigned y = scroll % GBN_SCREEN_HEIGHT;
            unsigned vertical = (scroll * 17 + base * 73 + size * 109) & 511;
            reg(8, (uint16_t) (size << 14 | bases[base] << 8 | character << 2));
            reg(0x10, (uint16_t) scroll); reg(0x12, (uint16_t) vertical);
            CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
            for (unsigned x = 0; x < GBN_SCREEN_WIDTH; ++x) {
                int want = text_reference(x, y);
                if (want < 0) want = memory[0] & 0x7fff;
                if (pixels[y * 248 + x] != want) {
                    fprintf(stderr, "indexed map align=%u size=%u base=%u char=%u scroll=%u,%u xy=%u,%u got=%04x want=%04x\n",
                        alignment, size, bases[base], character, scroll, vertical,
                        x, y, pixels[y * 248 + x], want);
                    exit(1);
                }
                ++comparisons;
            }
            for (unsigned x = 240; x < 248; ++x) CHECK(pixels[y * 248 + x] == 0xbeef);
        }
        CHECK(pixels[248 * 160] == 0xbeef);
        reference.d.deinit(&reference.d);
    }
    printf("PASS indexed map sweep: %u pixel comparisons, every horizontal scroll, four map sizes/bases, character bounds and all VRAM alignments\n", comparisons - before);
}

static void unaligned_video(void) {
	_Alignas(4) static uint8_t palette[GBN_PALETTE_SIZE + 4], vram[GBN_VRAM_SIZE + 4], oam[GBN_OAM_SIZE + 4];
	static uint16_t wanted[248 * 160 + 1];
	unsigned before = comparisons;
	for (unsigned mode = 0; mode < 6; ++mode) for (unsigned effect = 0; effect < 2; ++effect)
	for (unsigned format = 0; format < (mode ? 1 : 2); ++format) {
		setup();
		reg(0, (uint16_t) (mode | 0x7f40));
		for (unsigned id = 0; id < 4; ++id) {
			/* All-4bpp mode 0 also covers the indexed compositor's palette
			 * and VRAM alignment paths; the mixed-format case stays intact. */
			reg(8 + id * 2, (uint16_t) ((16 + id) << 8 | id | (!format && (id & 1) ? 0x80 : 0)));
			reg(0x10 + id * 4, (uint16_t) (id * 17 + 3));
			reg(0x12 + id * 4, (uint16_t) (id * 11 + 7));
		}
		reg(0x20, 0x100); reg(0x22, 0); reg(0x24, 0); reg(0x26, 0x100);
		reg(0x30, 0x100); reg(0x32, 0); reg(0x34, 0); reg(0x36, 0x100);
		reg(0x28, 0); reg(0x2a, 0); reg(0x2c, 0); reg(0x2e, 0);
		reg(0x38, 0); reg(0x3a, 0); reg(0x3c, 0); reg(0x3e, 0);
		reg(0x40, 0x2080); reg(0x42, 0x70d0); reg(0x44, 0x1090); reg(0x46, 0x309f);
		reg(0x48, 0x2d3f); reg(0x4a, 0x3f3f);
		reg(0x50, (uint16_t) (effect ? 0x3f7f : 0)); reg(0x52, 0x0808);
		store(d.oam, 31 * 32 + 6, 0x100); store(d.oam, 31 * 32 + 14, 0x40);
		store(d.oam, 31 * 32 + 22, 0xffc0); store(d.oam, 31 * 32 + 30, 0x100);
		for (unsigned id = 0; id < 8; ++id) {
			store(d.oam, id * 8, (uint16_t) (id * 17 | (id & 1 ? 0x400 : 0)));
			store(d.oam, id * 8 + 2, (uint16_t) (20 + id * 19));
			store(d.oam, id * 8 + 4, (uint16_t) (id << 10 | (mode >= 3 ? 512 : 0) | id * 8));
			if (id >= 6) {
				store(d.oam, id * 8, (uint16_t) (id * 17 | 0x100 | (id == 7 ? 0x200 : 0)));
				store(d.oam, id * 8 + 2, (uint16_t) ((20 + id * 19) | (31 << 9)));
			}
		}
		for (unsigned y = 0; y < 160; ++y) CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
		memcpy(wanted, pixels, sizeof(wanted));
		uint16_t registers[sizeof(d.io) / sizeof(*d.io)]; memcpy(registers, d.io, sizeof(registers));
		const uint8_t* video = (const uint8_t*) memory;
		for (unsigned odd = 1; odd < 8; ++odd) {
			uint8_t* p = palette + (odd & 1 ? 1 : 2);
			uint8_t* v = vram + (odd & 2 ? 1 : 2);
			uint8_t* o = oam + (odd & 4 ? 1 : 2);
			memcpy(p, video, GBN_PALETTE_SIZE);
			memcpy(v, video + GBN_PALETTE_SIZE, GBN_VRAM_SIZE);
			memcpy(o, video + GBN_PALETTE_SIZE + GBN_VRAM_SIZE, GBN_OAM_SIZE);
			CHECK(gbn_attach_devices(&m, &d, p, v, o));
			memcpy(d.io, registers, sizeof(registers));
			CHECK(gbn_attach_pixels(&m, pixels, 248));
			for (unsigned y = 0; y < 160; ++y) {
				CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
				for (unsigned x = 0; x < 240; ++x) { CHECK(pixels[y * 248 + x] == wanted[y * 248 + x]); ++comparisons; }
				for (unsigned x = 240; x < 248; ++x) CHECK(pixels[y * 248 + x] == 0xbeef);
			}
			CHECK(pixels[248 * 160] == 0xbeef);
		}
		reference.d.deinit(&reference.d);
	}
	printf("PASS unaligned video: %u pixel comparisons, modes 0-5, palette/VRAM/OAM alignment combinations and color effects\n", comparisons - before);
}

static bool window_contains(unsigned point, unsigned bounds) {
	unsigned begin = bounds >> 8, end = bounds & 255;
	return begin <= end ? point >= begin && point < end : point >= begin || point < end;
}

static void palette_pairs(void) {
	setup();
	reg(0, 0x0100); reg(8, 0x1000); reg(0x12, 0);
	for (unsigned tile = 0; tile < 32; ++tile) for (unsigned y = 0; y < 8; ++y)
		memset(d.vram + tile * 32 + y * 4, (int) (tile * 8 + y), 4);
	unsigned before = comparisons;
	for (unsigned bank = 0; bank < 16; ++bank) for (unsigned trial = 0; trial < 4; ++trial) {
		/* Revisit warm rows unchanged, then change colors through raw storage
		 * and the emulated bus. Neither path may retain stale expanded pairs. */
		if (trial != 1) for (unsigned i = 0; i < 16; ++i) {
			uint16_t color = i % 4 ? (uint16_t) random_value() : 0x8000;
			if (trial == 3) {
				uint32_t cycles;
				CHECK(gbn_write(&m, 0x05000000 + (bank * 16 + i) * 2, 2, color, &cycles) == GBN_STEP);
			} else store(d.palette, (bank * 16 + i) * 2, color);
		}
		for (unsigned flip = 0; flip < 2; ++flip) {
			for (unsigned i = 0; i < 0x800; i += 2) store(d.vram, 0x8000 + i, (uint16_t) (bank << 12 | flip << 10 | i / 2 % 32));
			for (unsigned scroll = 0; scroll < 9; ++scroll) {
				reg(0x10, (uint16_t) (scroll == 8 ? 16 : scroll));
				for (unsigned y = 0; y < 8; ++y) {
					CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
					for (unsigned x = 0; x < 240; ++x) {
						int color = text_reference(x, y);
						uint16_t want = color < 0 ? (d.palette[0] | (unsigned) d.palette[1] << 8) & 0x7fff : (uint16_t) color;
						CHECK(pixels[y * 248 + x] == want); ++comparisons;
					}
					for (unsigned x = 240; x < 248; ++x) CHECK(pixels[y * 248 + x] == 0xbeef);
				}
			}
		}
	}
	reference.d.deinit(&reference.d);
	printf("PASS palette pairs: %u pixel comparisons, all byte pairs/banks, warm reuse, raw/bus palette writes, flips and clipped spans\n", comparisons - before);
}

static void uniform_rows(void) {
	setup(); reg(0, 0x0300); reg(8, 0x1000); reg(10, 0x1101); reg(0x50, 0);
	reg(0x12, 7); reg(0x14, 0); reg(0x16, 0);
	for (unsigned i = 0; i < 256; ++i) store(d.palette, i * 2, (uint16_t) (0x8000 | (i & 15 ? (i & 15) == 1 ? 0 : (i * 1337) & 0x7fff : 0x7c00)));
	store(d.palette, (15 * 16 + 2) * 2, 0x03e0);
	memset(d.vram + 32, 0x22, 32);
	for (unsigned i = 0; i < 0x800; i += 2) store(d.vram, 0x8800 + i, 0xf001);
	unsigned before = comparisons;
	for (unsigned bank = 0; bank < 16; ++bank) for (unsigned index = 0; index < 16; ++index)
	for (unsigned flip = 0; flip < 2; ++flip) {
		memset(d.vram, (int) (index * 17), 32);
		for (unsigned i = 0; i < 0x800; i += 2) store(d.vram, 0x8000 + i, (uint16_t) (bank << 12 | flip << 10));
		uint16_t want = index ? (d.palette[(bank * 16 + index) * 2] | (unsigned) d.palette[(bank * 16 + index) * 2 + 1] << 8) & 0x7fff : 0x03e0;
		for (unsigned scroll = 0; scroll < 8; ++scroll) {
			reg(0x10, (uint16_t) scroll);
			unsigned y = (bank * 7 + index * 11 + flip) % 160;
			CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
			for (unsigned x = 0; x < 240; ++x) { CHECK(pixels[y * 248 + x] == want); ++comparisons; }
			for (unsigned x = 240; x < 248; ++x) CHECK(pixels[y * 248 + x] == 0xbeef);
		}
	}
	reference.d.deinit(&reference.d);
	printf("PASS uniform rows: %u pixel comparisons, all palette banks/indices, opaque black, transparent layers, flips and clipped spans\n", comparisons - before);
}

static void window_edges(void) {
	setup();
	memset(d.vram, 0x11, 32); memset(d.vram + 0x10000, 0x11, 32);
	for (unsigned i = 0; i < 0x800; i += 2) { store(d.vram, 0x8000 + i, 0); store(d.vram, 0x8800 + i, 0x1000); }
	store(d.palette, 0, 0x7c00); store(d.palette, 2, 0x001f); store(d.palette, 34, 0x03e0);
	reg(8, 0x1000); reg(10, 0x1101);
	for (unsigned offset = 0x10; offset < 0x20; offset += 2) reg(offset, 0);
	reg(0x48, 0x0201); reg(0x4a, 0x0300); reg(0x50, 0);
	store(d.oam, 0, 0x0800 | 90); store(d.oam, 2, 100); store(d.oam, 4, 0);
	static const unsigned edges[] = {0, 1, 15, 90, 159, 160, 239, 240, 241, 254, 255};
	static const unsigned vertical[] = {0x00a0, 0x9f01, 0xa05a, 0x5a5a, 0xf000};
	unsigned before = comparisons, n = sizeof(edges) / sizeof(*edges);
	for (unsigned a = 0; a < n * n; ++a) for (unsigned b = 0; b < n * n; ++b) {
		unsigned trial = a * n * n + b, y = trial % 4 == 1 ? 0 : trial % 4 == 2 ? 159 : 90;
		unsigned h0 = edges[a / n] << 8 | edges[a % n], h1 = edges[b / n] << 8 | edges[b % n];
		unsigned v0 = vertical[trial % 5], v1 = vertical[trial / 5 % 5];
		unsigned control = 0xf300 & ~(trial & 0x20 ? 0x2000 : 0) & ~(trial & 0x40 ? 0x4000 : 0);
		reg(0, (uint16_t) control); reg(0x40, (uint16_t) h0); reg(0x42, (uint16_t) h1);
		reg(0x44, (uint16_t) v0); reg(0x46, (uint16_t) v1);
		CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
		for (unsigned x = 0; x < 240; ++x) {
			unsigned mask;
			if ((control & 0x2000) && window_contains(x, h0) && window_contains(y, v0)) mask = 1;
			else if ((control & 0x4000) && window_contains(x, h1) && window_contains(y, v1)) mask = 2;
			else mask = y >= 90 && y < 98 && x >= 100 && x < 108 ? 3 : 0;
			uint16_t want = mask & 1 ? 0x001f : mask & 2 ? 0x03e0 : 0x7c00;
			if (pixels[y * 248 + x] != want) {
				fprintf(stderr, "FAIL window spans trial=%u pixel=%u,%u got=%04x want=%04x h=%04x/%04x v=%04x/%04x\n",
					trial, x, y, pixels[y * 248 + x], want, h0, h1, v0, v1); exit(1);
			}
			++comparisons;
		}
	}
	reference.d.deinit(&reference.d);
	printf("PASS window spans: %u pixel comparisons, empty/wrapped/offscreen bounds, vertical membership and WIN0/WIN1/OBJ priority\n", comparisons - before);
}

/* Exercise identity composition independently of the decoder: equal/rotated
 * BG priorities, opaque OBJ barriers, uniform window exclusions and formats
 * which require the general renderer all use mGBA's final-pixel oracle. */
static unsigned priority_composition(void) {
	unsigned before = comparisons;
	for (unsigned trial = 0; trial < 32; ++trial) {
		setup(); reg(0, 0x3f40); reg(2, trial & 2 ? 1 : 0);
		for (unsigned id = 0; id < 4; ++id) {
			unsigned priority = trial & 4 ? (id + trial) & 3 : trial & 3;
			unsigned format = (trial & 16) && (id & 1) ? 0x80 : 0;
			reg(8 + 2 * id, (uint16_t) ((16 + id) << 8 | (id & 1) << 2 | priority | format));
			reg(0x10 + 4 * id, (uint16_t) (id * 73 + trial));
			reg(0x12 + 4 * id, (uint16_t) (id * 29 + trial * 11));
		}
		reg(0x4c, 0x4321);
		unsigned mask = 0x3f;
		if (trial & 8) mask &= ~(1u << (trial & 3));
		if ((trial & 11) == 9) mask &= ~16u;
		reg(0x40, 0x00f0); reg(0x44, 0x00a0);
		reg(0x48, (uint16_t) mask); reg(0x4a, (uint16_t) mask);
		reg(0x50, trial & 1 ? 0x3f7f : 0); reg(0x52, 0x0010);
		for (unsigned id = 0; id < 16; ++id) {
			store(d.oam, id * 8, (uint16_t) (id * 9 | (id & 1 ? 0x400 : 0)));
			store(d.oam, id * 8 + 2, (uint16_t) (0x4000 | (id * 17 + trial * 3) % 240 | (id & 2 ? 0x1000 : 0)));
			store(d.oam, id * 8 + 4, (uint16_t) (id * 8 | ((id + trial) & 3) << 10 | (id & 15) << 12));
		}
		compare(0, 300 + trial, 0);
		reference.d.deinit(&reference.d);
	}
	printf("PASS priority composition: %u pixel comparisons, four BG/OBJ, equal/rotated priorities, uniform masks and mixed formats\n", comparisons - before);
	return 32;
}

int main(void) {
	unsigned cases = 0;
	for (unsigned mode = 0; mode < 6; ++mode) for (unsigned trial = 0; trial < 8; ++trial) {
		unsigned effects = 1;
#ifdef COLOR_16_BIT
		effects = 4;
#endif
		for (unsigned effect = 0; effect < effects; ++effect) {
			setup();
			/* The old packed RGB555 darkener rounds red differently from green
			 * and blue. Use exact integer products for its integration oracle;
			 * fractional component symmetry is checked independently below. */
			if (effect == 3) {
				for (unsigned i = 0; i < 512; ++i) {
					memory[i] &= 0x4210;
					reference.d.writePalette(&reference.d, i * 2, memory[i]);
				}
				if (mode == 3 || mode == 5) for (unsigned i = 0; i < 0x14000 / 2; ++i) ((uint16_t*) d.vram)[i] &= 0x4210;
			}
			reg(0, (uint16_t) (mode | 0x0f00 | (trial & 1 ? 16 : 0)));
			reg(2, trial & 4 ? 1 : 0);
			for (unsigned id = 0; id < 4; ++id) {
				unsigned size = trial & 3;
				reg(8 + 2 * id, (uint16_t) ((id + trial) % 4 | (id & 1) << 2 | 0x1800 | (trial & 1 ? 0x80 : 0) | (mode == 2 ? 0x2000 : 0) | size << 14));
				reg(0x10 + id * 4, (uint16_t) (trial * 73)); reg(0x12 + id * 4, (uint16_t) (trial * 139));
			}
			for (unsigned id = 0; id < 2; ++id) {
				reg(0x20 + id * 16, trial & 2 ? 0x180 : 0x100);
				reg(0x22 + id * 16, trial & 4 ? 0x40 : 0);
				reg(0x24 + id * 16, trial & 2 ? 0xffc0 : 0);
				reg(0x26 + id * 16, trial & 4 ? 0x100 : 0xe0);
				reg(0x28 + id * 16, (uint16_t) (trial & 1 ? -512 : 256));
				reg(0x2a + id * 16, trial & 1 ? 0xfff : 0);
			}
			reg(0x50, (uint16_t) (0x3f3f | effect << 6)); reg(0x52, 0x0808); reg(0x54, 7);
			compare(mode, trial, effect); ++cases;
			reference.d.deinit(&reference.d);
		}
	}
	for (unsigned trial = 0; trial < 24; ++trial) {
		setup();
		unsigned mode = trial / 4;
		/* Old bitmap mode 3/5 OBJ-window gating is inverted. Those modes use
		 * rectangular windows here; their OBJ-window mask is checked separately. */
		unsigned windows = mode == 3 || mode == 5 ? 0x6000 : 0xe000;
		reg(0, (uint16_t) (mode | 0x1f00 | (trial & 1 ? 0x40 : 0) | (trial & 2 ? windows : 0)));
		for (unsigned id = 0; id < 4; ++id) reg(8 + 2 * id, (uint16_t) (0x1800 | id % 2 << 2 | id % 4 | (trial & 2 ? 0x40 : 0)));
		reg(0x4c, 0x3212);
		reg(0x40, 0x329f); reg(0x42, 0xc828); reg(0x44, 0x1e6e); reg(0x46, 0x8214);
		reg(0x48, 0x1f2d); reg(0x4a, 0x3b3e);
		reg(0x50, 0x3f7f); reg(0x52, 0x0808);
#ifndef COLOR_16_BIT
		reg(0x52, 0x0010); /* Exact native RGB8-to-RGB555 conversion at unit gain. */
#endif
		/* Shared matrix 31 leaves each test sprite's OAM attributes separate. */
		store(d.oam, 31 * 32 + 6, 0x100); store(d.oam, 31 * 32 + 14, trial & 4 ? 0x40 : 0);
		store(d.oam, 31 * 32 + 22, trial & 4 ? 0xffc0 : 0); store(d.oam, 31 * 32 + 30, 0x100);
		for (unsigned id = 0; id < 8; ++id) {
			unsigned y = id == 6 ? 250 : 20 + id * 17;
			unsigned x = id == 7 ? 500 : 20 + id * 23;
			unsigned a = y | (id == 1 ? 0x400 : id == 2 ? 0x800 : 0) | (trial & 1 ? 0x2000 : 0) | (id % 3) << 14;
			unsigned b = x | 0x8000 | (id & 1 ? 0x1000 : 0) | (id & 2 ? 0x2000 : 0);
			if (id == 4 || id == 5) { a |= 0x100 | (id == 5 ? 0x200 : 0); b = x | 0x8000 | 31 << 9; }
			else if (trial & 2) a |= 0x1000;
			store(d.oam, id * 8, (uint16_t) a); store(d.oam, id * 8 + 2, (uint16_t) b);
			store(d.oam, id * 8 + 4, (uint16_t) (0x400 | (mode >= 3 ? 512 : 0) | id * 32 | (id % 16) << 12));
		}
		/* Establish wrapped vertical-window state from the preceding frame. */
		for (unsigned y = 0; y < 160; ++y) {
			CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
			reference.d.drawScanline(&reference.d, (int) y);
		}
		reference.d.finishFrame(&reference.d);
		compare(mode, 100 + trial, 1); ++cases;
		reference.d.deinit(&reference.d);
	}
	setup(); reg(0, 0x80); compare(0, 99, 0); reference.d.deinit(&reference.d);
	/* Bitmap OBJ-window masking has a known inversion in the old mode 3/5
	 * renderer. Check visible colors against a simple geometric fixture. */
	for (unsigned mode = 3; mode <= 5; ++mode) {
		setup();
		store(d.palette, 0, 0x001f); store(d.palette, 2, 0x7c00);
		if (mode == 4) memset(d.vram, 1, 240 * 160);
		else for (unsigned i = 0; i < (mode == 3 ? 240 * 160 : 160 * 128); ++i) store(d.vram, i * 2, 0x7c00);
		memset(d.vram + 0x14000, 0x11, 32);
		store(d.oam, 0, 0x080a); store(d.oam, 2, 10); store(d.oam, 4, 512);
		reg(0, (uint16_t) (0x9400 | mode)); reg(0x4a, 4);
		for (unsigned y = 0; y <= 10; ++y) CHECK(gbn_ppu_line(&m, y) == GBN_STEP);
		CHECK(pixels[10 * 248 + 9] == 0x7c00 && pixels[10 * 248 + 10] == 0x001f && pixels[10 * 248 + 17] == 0x001f && pixels[10 * 248 + 18] == 0x7c00);
		reference.d.deinit(&reference.d);
	}
	setup();
	store(d.palette, 0, 0x7c00); store(d.palette, 0x202, 0x001f); store(d.palette, 0x204, 0x03e0);
	memset(d.vram + 0x10000, 0x11, 32); memset(d.vram + 0x10020, 0x22, 32);
	store(d.oam, 0, 0x400); store(d.oam, 2, 10); store(d.oam, 4, 0);
	store(d.oam, 8, 0); store(d.oam, 10, 10); store(d.oam, 12, 1);
	reg(0, 0x1000); reg(0x50, 0x1000); reg(0x52, 0x0808);
	CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x001f); /* No OBJ-to-OBJ blend. */
	reg(0x50, 0x2000); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x3c0f);
	/* Zero brightness is identity for ordinary pixels, but a semi-transparent
	 * OBJ still forces alpha against an enabled second target. */
	reg(0x54, 0); reg(0x50, 0x20d0);
	CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x3c0f);
	reg(0x50, 0x00d0); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x001f);
	reg(0x50, 0x20d0); reg(0x52, 0);
	CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0);
	reg(0x52, 0x0010); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x001f);
	reg(0x52, 0x001f); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x001f);
	reg(0x50, 0x2070); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x001f);
	reg(0x50, 0x2000); reg(0x52, 0x0808);
	reg(0, 0x3000); reg(0x40, 0x0014); reg(0x44, 0x0014); reg(0x48, 0x1f);
	CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[10] == 0x001f); /* Window masks color effects. */
	reference.d.deinit(&reference.d);
	setup();
	store(d.palette, 0, 0x7fff); reg(0, 0); reg(0x50, 0x00e0); reg(0x54, 8);
	CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[0] == 0x4210); /* 31 - floor(31/2) on each component. */
	reg(0x54, 31); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[0] == 0);
	reg(0x50, 0x00a0); CHECK(gbn_ppu_line(&m, 0) == GBN_STEP && pixels[0] == 0x7fff);
	reference.d.deinit(&reference.d);
	/* Equal and reversed priorities, transparent holes and a masked middle
	 * layer must still select the nearest two layers for alpha blending. */
	for (unsigned trial = 0; trial < 4; ++trial) {
		setup(); reg(0, 0x6f00);
		for (unsigned id = 0; id < 4; ++id) {
			reg(8 + 2 * id, (uint16_t) (0x1800 | id % 2 << 2 | (trial & 1 ? 3 - id : 0)));
			reg(0x10 + id * 4, (uint16_t) (id * 7 + trial));
		}
		reg(0x40, 0x2080); reg(0x44, 0x1090); reg(0x48, 0x3b3d); reg(0x4a, 0x3f);
		reg(0x50, 0x3f7f); reg(0x52, trial & 2 ? 0x0808 : 0x0010);
#ifndef COLOR_16_BIT
		reg(0x52, 0x0010);
#endif
		compare(0, 200 + trial, 1); ++cases;
		reference.d.deinit(&reference.d);
	}
	cases += priority_composition();
	text_edges();
	indexed_map_edges();
	window_edges();
	uniform_rows();
	palette_pairs();
	unaligned_video();
	CHECK(gbn_attach_pixels(&m, NULL, 0));
	printf("PASS independent PPU: %u background/bitmap/affine/effect frames, %u pixel comparisons, forced blank and output stride/bounds\n", cases, comparisons);
	return 0;
}
