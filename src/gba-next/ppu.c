/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include <string.h>
#include "ppu-internal.h"

/* Decode backgrounds a scanline at a time. Transparent texels keep a separate
 * bit so opaque RGB555 black remains visible. Only the closest two visible
 * layers participate in effects; background priority is sorted once per line. */


static uint16_t half(const uint8_t* data, unsigned offset) {
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	const uint8_t* bytes = data + offset;
	if (!((uintptr_t) bytes & 1)) {
		uint16_t value;
		/* Preserve byte-backed storage's aliasing contract while allowing a
		 * single aligned halfword load. Unaligned caller storage stays legal. */
		__builtin_memcpy(&value, __builtin_assume_aligned(bytes, 2), sizeof(value));
		return value;
	}
#endif
	return (uint16_t) (data[offset] | (unsigned) data[offset + 1] << 8);
}

static int32_t signed28(uint32_t value) {
	value &= 0x0fffffff;
	return (int32_t) value - (value & 0x08000000 ? 0x10000000 : 0);
}

void gbn_ppu_reference(struct Gbn* m, unsigned offset) {
	unsigned id = (offset - 0x28) / 16, axis = (offset & 4) / 4;
	unsigned base = (0x28 + id * 16 + axis * 4) / 2;
	m->devices->ppu.reference[id][axis] = signed28(m->devices->io[base] | (uint32_t) m->devices->io[base + 1] << 16);
}

bool gbn_attach_pixels(struct Gbn* m, uint16_t* pixels, unsigned stride) {
	if (!m->devices || (pixels && stride < GBN_SCREEN_WIDTH)) return false;
	struct GbnPpu* p = &m->devices->ppu;
	p->pixels = pixels; p->stride = stride;
	p->frames = p->lines = 0; memset(p->seen, 0, sizeof(p->seen));
	for (unsigned id = 0; id < 2; ++id) for (unsigned axis = 0; axis < 2; ++axis) gbn_ppu_reference(m, 0x28 + id * 16 + axis * 4);
	return true;
}

static void insert(struct Pixel* top, struct Pixel* second, struct Pixel value) {
	if (value.key < top->key) { *second = *top; *top = value; }
	else if (value.key < second->key) *second = value;
}



static void fill_span(uint16_t* output, unsigned count, uint16_t color) {
	if (count == 8) {
		output[0] = color; output[1] = color; output[2] = color; output[3] = color;
		output[4] = color; output[5] = color; output[6] = color; output[7] = color;
	} else for (unsigned i = 0; i < count; ++i) output[i] = color;
}

static inline __attribute__((always_inline)) uint16_t text_half(const uint8_t* data, unsigned offset, bool aligned) {
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if (aligned) {
		uint16_t value;
		__builtin_memcpy(&value, __builtin_assume_aligned(data + offset, 2), sizeof(value));
		return value;
	}
#else
	(void) aligned;
#endif
	return (uint16_t) (data[offset] | (unsigned) data[offset + 1] << 8);
}

/* False proves the entire row transparent. True is conservative for partial
 * tile rows and 8bpp backgrounds, so omitted layers can never contain pixels. */
static inline __attribute__((always_inline)) bool text_line_body(const struct GbnDevices* d, unsigned id, unsigned y, uint16_t* output, bool aligned) {
	unsigned control = d->io[4 + id], size = control >> 14;
	uint32_t opaque = 0;
	bool transparent_tail = false;
	unsigned mosaic = 1;
	if (control & 0x40) {
		mosaic = (d->io[0x4c / 2] & 15) + 1;
		y -= y % ((d->io[0x4c / 2] >> 4 & 15) + 1);
	}
	y = (y + d->io[9 + id * 2]) & (size & 2 ? 511 : 255);
	unsigned map_row = (control >> 8 & 31) * 0x800 + (y >> 8) * (size & 1 ? 2 : 1) * 0x800 + (y & 248) * 8;
	unsigned xmask = size & 1 ? 511 : 255;
	unsigned scroll = d->io[8 + id * 2];
	unsigned character = (control >> 2 & 3) * 0x4000;
	bool depth8 = (control & 0x80) != 0;
	for (unsigned x = 0; x < 240;) {
		unsigned sx = (x + scroll) & xmask;
		unsigned tile = text_half(d->vram, (map_row + (sx >> 8) * 0x800 + (sx & 248) / 4) & 0xffff, aligned);
		unsigned tx = sx & 7, ty = (y & 7) ^ (tile & 0x800 ? 7 : 0);
		bool flip = (tile & 0x400) != 0;
		unsigned address = character + (tile & 0x3ff) * (depth8 ? 64 : 32) + ty * (depth8 ? 8 : 4);
		unsigned count = mosaic > 1 ? mosaic : 8 - tx;
		if (count > 240 - x) count = 240 - x;
		if (address >= 0x10000) {
			fill_span(output + x, count, TRANSPARENT);
		} else if (mosaic > 1) {
			/* Mosaic repeats the screen-aligned first texel, even when its
			 * group crosses a tile or map boundary. */
			unsigned index;
			if (flip) tx = 7 - tx;
			if (depth8) index = d->vram[address + tx];
			else index = d->vram[address + tx / 2] >> ((tx & 1) * 4) & 15;
			uint16_t color = index ? text_half(d->palette, (index + (depth8 ? 0 : (tile >> 12) * 16)) * 2, aligned) & 0x7fff : TRANSPARENT;
			opaque |= index;
			fill_span(output + x, count, color);
		} else if (depth8) {
			opaque = 1;
			for (unsigned i = 0; i < count; ++i) {
				unsigned index = d->vram[address + ((tx + i) ^ (flip ? 7 : 0))];
				output[x + i] = index ? text_half(d->palette, index * 2, aligned) & 0x7fff : TRANSPARENT;
			}
		} else {
			/* A tile row is four bytes. Decode it once for up to eight
			 * output pixels, without unaligned or aliasing-prone loads. */
			uint32_t packed = (uint32_t) text_half(d->vram, address, aligned) | (uint32_t) text_half(d->vram, address + 2, aligned) << 16;
			opaque |= packed;
			const uint8_t* palette = d->palette + (tile >> 12) * 32;
			/* Uniform rows need one palette lookup, including transparent index
			 * zero. This also covers clipped and horizontally flipped tile rows. */
			if (!packed) {
				/* Only the high bit marks transparency. Fill the not-yet
				 * decoded suffix once; later opaque tiles overwrite it. */
				if (!transparent_tail) {
					memset(output + x, 0x80, (GBN_SCREEN_WIDTH - x) * sizeof(*output));
					transparent_tail = true;
				}
			} else if (packed == (packed & 15u) * 0x11111111u) {
				unsigned index = packed & 15;
				uint16_t color = index ? text_half(palette, index * 2, aligned) & 0x7fff : TRANSPARENT;
				fill_span(output + x, count, color);
			} else
#if defined(AM_RVV) && (GBN_PPU_RVV & 1)
			if (!((uintptr_t) palette & 1)) gbn_ppu_row4_vector(packed, palette, output + x, tx, count, flip);
			else
#endif
			if (count == 8) {
				/* Full rows start at texel zero. Constant shifts expose
				 * independent pixels and remove the per-pixel loop walk. */
#define ROW_PIXEL(i, shift) do { \
	unsigned index = packed >> (shift) & 15; \
	output[x + (i)] = index ? text_half(palette, index * 2, aligned) & 0x7fff : TRANSPARENT; \
} while (0)
				if (flip) {
					ROW_PIXEL(0, 28);
					ROW_PIXEL(1, 24);
					ROW_PIXEL(2, 20);
					ROW_PIXEL(3, 16);
					ROW_PIXEL(4, 12);
					ROW_PIXEL(5, 8);
					ROW_PIXEL(6, 4);
					ROW_PIXEL(7, 0);
				} else {
					ROW_PIXEL(0, 0);
					ROW_PIXEL(1, 4);
					ROW_PIXEL(2, 8);
					ROW_PIXEL(3, 12);
					ROW_PIXEL(4, 16);
					ROW_PIXEL(5, 20);
					ROW_PIXEL(6, 24);
					ROW_PIXEL(7, 28);
				}
#undef ROW_PIXEL
			} else if (flip) {
				packed <<= tx * 4;
				for (unsigned i = 0; i < count; ++i) {
					unsigned index = packed >> 28; packed <<= 4;
					output[x + i] = index ? text_half(palette, index * 2, aligned) & 0x7fff : TRANSPARENT;
				}
			} else {
				packed >>= tx * 4;
				for (unsigned i = 0; i < count; ++i) {
					unsigned index = packed & 15; packed >>= 4;
					output[x + i] = index ? text_half(palette, index * 2, aligned) & 0x7fff : TRANSPARENT;
				}
			}
		}
		x += count;
	}
	return opaque != 0;
}

/* Keep the two kernels separate: pointer alignment is invariant for the whole
 * scanline and must not become another branch at each palette/map access. */
static __attribute__((noinline)) bool text_line_unaligned(const struct GbnDevices* d, unsigned id, unsigned y, uint16_t* output) {
	return text_line_body(d, id, y, output, false);
}
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static __attribute__((noinline)) bool text_line_aligned(const struct GbnDevices* d, unsigned id, unsigned y, uint16_t* output) {
	return text_line_body(d, id, y, output, true);
}
#endif
static bool text_line(const struct GbnDevices* d, unsigned id, unsigned y, uint16_t* output) {
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	if (!(((uintptr_t) d->vram | (uintptr_t) d->palette) & 1)) return text_line_aligned(d, id, y, output);
#endif
	return text_line_unaligned(d, id, y, output);
}

static void affine_line(const struct GbnDevices* d, unsigned id, unsigned mode, unsigned y, uint16_t* output) {
	unsigned control = d->io[4 + id], base = (0x20 + (id - 2) * 16) / 2;
	int32_t rx = d->ppu.reference[id - 2][0], ry = d->ppu.reference[id - 2][1];
	unsigned mosaic = 1;
	if (control & 0x40) {
		mosaic = (d->io[0x4c / 2] & 15) + 1;
		unsigned back = y % ((d->io[0x4c / 2] >> 4 & 15) + 1);
		rx -= (int16_t) d->io[base + 1] * (int32_t) back;
		ry -= (int16_t) d->io[base + 3] * (int32_t) back;
	}
	int32_t dx = (int16_t) d->io[base] * (int32_t) mosaic;
	int32_t dy = (int16_t) d->io[base + 2] * (int32_t) mosaic;
	if (mode < 3) {
		unsigned size = 128u << (control >> 14);
		unsigned map_base = (control >> 8 & 31) * 0x800, character = (control >> 2 & 3) * 0x4000;
		for (unsigned x = 0; x < 240;) {
			int32_t px = rx >> 8, py = ry >> 8;
			uint16_t color = TRANSPARENT;
			if ((control & 0x2000) || ((uint32_t) px < size && (uint32_t) py < size)) {
				unsigned tx = (uint32_t) px & (size - 1), ty = (uint32_t) py & (size - 1);
				unsigned map = (map_base + ty / 8 * (size / 8) + tx / 8) & 0xffff;
				unsigned address = character + d->vram[map] * 64 + (ty & 7) * 8 + (tx & 7);
				if (address < 0x10000) {
					unsigned index = d->vram[address];
					if (index) color = half(d->palette, index * 2) & 0x7fff;
				}
			}
			unsigned end = x + mosaic; if (end > 240) end = 240;
			do { output[x++] = color; } while (x < end);
			rx += dx; ry += dy;
		}
	} else {
		unsigned width = mode == 5 ? 160 : 240, height = mode == 5 ? 128 : 160;
		unsigned page = mode != 3 && (d->io[0] & 16) ? 0xa000 : 0;
		for (unsigned x = 0; x < 240;) {
			int32_t px = rx >> 8, py = ry >> 8;
			uint16_t color = TRANSPARENT;
			if ((uint32_t) px < width && (uint32_t) py < height) {
				unsigned address = (unsigned) py * width + (unsigned) px;
				if (mode == 4) {
					unsigned index = d->vram[page + address];
					if (index) color = half(d->palette, index * 2) & 0x7fff;
				} else color = half(d->vram, page + address * 2) & 0x7fff;
			}
			unsigned end = x + mosaic; if (end > 240) end = 240;
			do { output[x++] = color; } while (x < end);
			rx += dx; ry += dy;
		}
	}
}

static inline __attribute__((always_inline)) void objects_body(const struct GbnDevices* d, unsigned y, struct Pixel* output, bool* window, bool aligned) {
	static const uint8_t widths[3][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32}};
	static const uint8_t heights[3][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64}};
	unsigned dispcnt = d->io[0];
	if (!(dispcnt & 0x1000)) return;
	unsigned mosaic_control = d->io[0x4c / 2];
	unsigned mosaic_h = (mosaic_control >> 8 & 15) + 1;
	unsigned mosaic_v = (mosaic_control >> 12 & 15) + 1;
	unsigned mosaic_y = y % mosaic_v;
	bool one_dimensional = (dispcnt & 0x40) != 0;
	int budget = dispcnt & 0x20 ? 954 : 1210;
	for (unsigned id = 0; id < 128; ++id) {
		budget -= 2;
		if (budget <= 0) break;
		unsigned a = text_half(d->oam, id * 8, aligned), b = text_half(d->oam, id * 8 + 2, aligned), c = text_half(d->oam, id * 8 + 4, aligned);
		bool affine = (a & 0x100) != 0;
		unsigned shape = a >> 14, kind = a >> 10 & 3;
		if (shape == 3 || kind == 3 || (!affine && (a & 0x200))) continue;
		if (kind == 2 && !(dispcnt & 0x8000)) continue;
		if ((dispcnt & 7) >= 3 && (c & 0x3ff) < 512) continue;
		unsigned width = widths[shape][b >> 14], height = heights[shape][b >> 14];
		unsigned bw = width << (affine && (a & 0x200)), bh = height << (affine && (a & 0x200));
		unsigned local_y = (y - (a & 255)) & 255;
		if (local_y >= bh) continue;
		int left = (int) (b & 511); if (left >= 256) left -= 512;
		if (left >= 240 || left + (int) bw <= 0) continue;
		unsigned depth = a & 0x2000 ? 8 : 4;
		unsigned tile = c & 0x3ff;
		if (depth == 8 && !one_dimensional) tile &= ~1u;
		unsigned stride = one_dimensional ? width * depth / 8 : 128;
		unsigned key = (c >> 10 & 3) * 8;
		int end = left + (int) bw;
		if (a & 0x1000) {
			if (end % (int) mosaic_h) end += mosaic_h - end % (int) mosaic_h;
		}
		int cost = affine ? 2 : 1;
		if (affine) budget -= 10;
		for (int screen = left < 0 ? 0 : left; screen < end && screen < 240; ++screen) {
			budget -= cost;
			if (budget < 0) break;
			int ox = screen - left, oy = (int) local_y;
			if (a & 0x1000) {
				ox -= screen % (int) mosaic_h;
				oy -= mosaic_y;
				if (ox < 0) ox = 0;
				if (oy < 0) oy = 0;
			}
			int tx, ty;
			if (affine) {
				unsigned mat = (b >> 9 & 31) * 32;
				int cx = ox - (int) bw / 2, cy = oy - (int) bh / 2;
				tx = ((int16_t) text_half(d->oam, mat + 6, aligned) * cx + (int16_t) text_half(d->oam, mat + 14, aligned) * cy + (int) width * 128) >> 8;
				ty = ((int16_t) text_half(d->oam, mat + 22, aligned) * cx + (int16_t) text_half(d->oam, mat + 30, aligned) * cy + (int) height * 128) >> 8;
			} else {
				tx = b & 0x1000 ? (int) width - 1 - ox : ox;
				ty = b & 0x2000 ? (int) height - 1 - oy : oy;
			}
			if (tx < 0 || ty < 0 || tx >= (int) width || ty >= (int) height) continue;
			unsigned xpart = (unsigned) tx / 8 * depth * 8 + ((unsigned) tx & 7) * depth / 8;
			unsigned lo = tile * 32 + xpart;
			unsigned address = (unsigned) ty / 8 * stride * 8 + ((unsigned) ty & 7) * depth;
			if (one_dimensional) address += lo & 0x7fff;
			else address += (lo & 0x3ff) + (tile * 32 & 0x7c00);
			address = 0x10000 + (address & 0x7fff);
			unsigned index = d->vram[address];
			if (depth == 4) index = index >> ((tx & 1) * 4) & 15;
			if (!index) continue;
			if (kind == 2) { window[screen] = true; continue; }
			if (key >= output[screen].key) continue;
			if (depth == 4) index += (c >> 12) * 16;
			output[screen] = (struct Pixel) {text_half(d->palette, 0x200 + index * 2, aligned) & 0x7fff, (uint8_t) key, 4, kind == 1};
		}
	}
}

static __attribute__((noinline)) void objects_unaligned(const struct GbnDevices* d,
        unsigned y, struct Pixel* output, bool* window) {
    objects_body(d, y, output, window, false);
}
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static __attribute__((noinline)) void objects_aligned(const struct GbnDevices* d,
        unsigned y, struct Pixel* output, bool* window) {
    objects_body(d, y, output, window, true);
}
#endif
static void objects(const struct GbnDevices* d, unsigned y, struct Pixel* output, bool* window) {
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (!(((uintptr_t) d->oam | (uintptr_t) d->palette) & 1)) {
        objects_aligned(d, y, output, window); return;
    }
#endif
    objects_unaligned(d, y, output, window);
}

static bool inside(unsigned point, unsigned bounds) {
	unsigned start = bounds >> 8, end = bounds & 255;
	return start <= end ? point >= start && point < end : point >= start || point < end;
}

static void window_span(uint8_t* masks, unsigned bounds, uint8_t mask) {
	unsigned start = bounds >> 8, end = bounds & 255;
	if (end > GBN_SCREEN_WIDTH) end = GBN_SCREEN_WIDTH;
	if (start <= (bounds & 255)) {
		if (start < end) memset(masks + start, mask, end - start);
	} else {
		if (end) memset(masks, mask, end);
		if (start < GBN_SCREEN_WIDTH) memset(masks + start, mask, GBN_SCREEN_WIDTH - start);
	}
}

/* A window mask is constant for the rest of a pixel's composition.  Build it
 * once per scanline instead of re-reading DISPCNT and the window registers
 * from the inner layer-selection loop.  The no-window case is particularly
 * common and does not need any coordinate checks at all. */
static unsigned line_masks(const struct GbnDevices* d, unsigned y, const bool* object_window, bool have_objects, uint8_t* masks) {
	if (!(d->io[0] & 0xe000)) {
		memset(masks, 0x3f, GBN_SCREEN_WIDTH);
		return 0x13f;
	}
	unsigned outside = d->io[0x4a / 2] & 0x3f;
	unsigned uniform = 0x100 | outside;
	if (have_objects && (d->io[0] & 0x8000)) {
		unsigned object = d->io[0x4a / 2] >> 8 & 0x3f;
		if (object != outside) uniform = 0;
		for (unsigned x = 0; x < GBN_SCREEN_WIDTH; ++x) masks[x] = (uint8_t) (object_window[x] ? object : outside);
	} else memset(masks, (int) outside, GBN_SCREEN_WIDTH);
	/* Rectangles override the outside/OBJ mask; WIN0 has priority over WIN1.
	 * Vertical membership is constant for this entire line. Horizontal wraps
	 * produce two spans, including the offscreen 240..255 coordinate range. */
	for (unsigned order = 0; order < 2; ++order) {
		unsigned id = 1 - order;
		if ((d->io[0] & (0x2000u << id)) && inside(y, d->io[0x44 / 2 + id])) {
			unsigned mask = d->io[0x48 / 2] >> (id * 8) & 0x3f;
			if (mask != outside) uniform = 0;
			window_span(masks, d->io[0x40 / 2 + id], (uint8_t) mask);
		}
	}
	/* A nonzero tag proves every mask equal. Zero is conservative when
	 * rectangles cover up differences or an OBJ window has no pixels. */
	return uniform;
}

static unsigned capped(unsigned value) { return value > 16 ? 16 : value; }

static uint16_t effect_values(unsigned control, unsigned alpha_control, unsigned brightness_control, struct Pixel top, struct Pixel second, bool enabled) {
	if (!enabled) return top.color;
	unsigned kind = control >> 6 & 3;
	bool blend = ((control & (1u << top.layer)) && kind == 1) || top.alpha;
	bool second_target = second.key != 255 && (control & (0x100u << second.layer)) != 0;
	if (blend && second_target) kind = 1;
	else if (!(control & (1u << top.layer)) || kind == 1) return top.color;
	unsigned a = capped(alpha_control & 31), b = capped(alpha_control >> 8 & 31), intensity = capped(brightness_control & 31);
	if (kind >= 2 && !intensity) return top.color;
	uint16_t color = 0;
	for (unsigned shift = 0; shift < 15; shift += 5) {
		unsigned value = top.color >> shift & 31;
		if (kind == 1) { value = (value * a + (second.color >> shift & 31) * b) / 16; if (value > 31) value = 31; }
		else if (kind == 2) value += (31 - value) * intensity / 16;
		else if (kind == 3) value -= value * intensity / 16;
		color |= (uint16_t) (value << shift);
	}
	return color;
}

#if !defined(AM_RVV) || !(GBN_PPU_RVV & 2)
/* Window membership and enabled layers are fixed for this span. First find
 * the closest opaque BG, then compare its priority with the closest OBJ. */
static inline __attribute__((always_inline)) void compose_span_body(
		const struct Background* layers, unsigned count,
		const struct Pixel* objects, unsigned start, unsigned end,
		uint16_t backdrop, uint16_t* output) {
	const uint16_t* a = count > 0 ? layers[0].colors : NULL;
	const uint16_t* b = count > 1 ? layers[1].colors : NULL;
	const uint16_t* c = count > 2 ? layers[2].colors : NULL;
	const uint16_t* d = count > 3 ? layers[3].colors : NULL;
	unsigned ka = count > 0 ? layers[0].key : 39;
	unsigned kb = count > 1 ? layers[1].key : 39;
	unsigned kc = count > 2 ? layers[2].key : 39;
	unsigned kd = count > 3 ? layers[3].key : 39;
	for (unsigned x = start; x < end; ++x) {
		int color = backdrop;
		unsigned key = 39;
		int candidate;
		if (count > 0 && (candidate = (int16_t) a[x]) >= 0) { color = candidate; key = ka; goto background_selected; }
		if (count > 1 && (candidate = (int16_t) b[x]) >= 0) { color = candidate; key = kb; goto background_selected; }
		if (count > 2 && (candidate = (int16_t) c[x]) >= 0) { color = candidate; key = kc; goto background_selected; }
		if (count > 3 && (candidate = (int16_t) d[x]) >= 0) { color = candidate; key = kd; }
	background_selected:
		if (objects && objects[x].key < key) color = objects[x].color;
		output[x] = color;
	}
}

static __attribute__((noinline)) void compose_span(
		const struct Background* layers, unsigned count,
		const struct Pixel* objects, unsigned start, unsigned end,
		uint16_t backdrop, uint16_t* output) {
	/* Instantiate only the ten bounded kernels. Their inner loops have no
	 * layer-count, window-permission, or object-presence branches. */
#define SPAN_CASE(n) case n: \
	if (objects) compose_span_body(layers, n, objects, start, end, backdrop, output); \
	else compose_span_body(layers, n, NULL, start, end, backdrop, output); \
	break
	switch (count) {
	SPAN_CASE(0); SPAN_CASE(1); SPAN_CASE(2); SPAN_CASE(3); SPAN_CASE(4);
	}
#undef SPAN_CASE
}

static __attribute__((noinline)) void compose_windows(const struct Background* layers, unsigned count,
		const struct Pixel* objects, const uint8_t* masks, unsigned uniform,
		uint16_t backdrop, uint16_t* output) {
	for (unsigned start = 0; start < GBN_SCREEN_WIDTH;) {
		unsigned mask = masks[start], end = start + 1;
		if (uniform) end = GBN_SCREEN_WIDTH;
		else while (end < GBN_SCREEN_WIDTH && masks[end] == mask) ++end;
		struct Background visible[4]; unsigned active = 0;
		for (unsigned i = 0; i < count; ++i)
			if (mask & layers[i].mask) visible[active++] = layers[i];
		compose_span(visible, active, mask & 16 ? objects : NULL,
					 start, end, backdrop, output);
		start = end;
	}
}
#endif

#if !defined(AM_RVV) || !(GBN_PPU_RVV & 2)
/* Keep the raw palette bank/nibble in one byte until priority selection.
 * A zero low nibble is transparent even when the bank bits are nonzero.
 * Colors are read only for the winning background; OBJ colors stay RGB555. */
static inline __attribute__((always_inline)) uint32_t indexed_tile(
        const struct GbnDevices* d, unsigned tile, unsigned row, unsigned character,
        uint8_t* output, unsigned tx, unsigned count, unsigned remaining,
        bool aligned, bool* transparent_tail) {
    unsigned x = 0, ty = row ^ (tile & 0x800 ? 7 : 0);
    bool flip = (tile & 0x400) != 0;
    unsigned address = character + (tile & 0x3ff) * 32 + ty * 4;
    uint32_t result = 0;
    if (address >= 0x10000) {
        memset(output + x, 0, count);
    } else {
        /* A tile row is four bytes. Decode it once for up to eight
         * output pixels, without unaligned or aliasing-prone loads. */
        uint32_t packed;
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        /* This instantiation receives a four-byte-aligned VRAM base; every
         * 4bpp tile-row address is also a multiple of four. */
        if (aligned) __builtin_memcpy(&packed,
            __builtin_assume_aligned(d->vram + address, 4), sizeof(packed));
        else
#endif
        packed = (uint32_t) text_half(d->vram, address, false)
            | (uint32_t) text_half(d->vram, address + 2, false) << 16;
        result = packed;
        unsigned bank = (tile >> 12) * 16;
        /* Uniform rows need one byte fill, including transparent index
         * zero. This also covers clipped and horizontally flipped tile rows. */
        if (!packed) {
            /* A zero low nibble marks transparency. Fill the not-yet
             * decoded suffix once; later opaque tiles overwrite it. */
            if (!*transparent_tail) {
                memset(output + x, 0, remaining);
                *transparent_tail = true;
            }
        } else if (packed == (packed & 15u) * 0x11111111u) {
            unsigned index = packed & 15;
            memset(output + x, (int) (bank | index), count);
        } else

        if (count == 8) {
            /* Full rows start at texel zero. Constant shifts expose
             * independent pixels and remove the per-pixel loop walk. */
#define ROW_PIXEL(i, shift) do { \
    unsigned index = packed >> (shift) & 15; \
    output[x + (i)] = (uint8_t) (bank | index); \
} while (0)
            if (flip) {
                ROW_PIXEL(0, 28);
                ROW_PIXEL(1, 24);
                ROW_PIXEL(2, 20);
                ROW_PIXEL(3, 16);
                ROW_PIXEL(4, 12);
                ROW_PIXEL(5, 8);
                ROW_PIXEL(6, 4);
                ROW_PIXEL(7, 0);
            } else {
                /* Spread four nibbles into byte lanes, then write each
                 * aligned group once. The caller biases its row by the
                 * scroll texel, making every full tile start aligned. */
                uint32_t lo = packed & 0xffff, hi = packed >> 16;
                lo = (lo | lo << 8) & 0x00ff00ffu;
                lo = (lo | lo << 4) & 0x0f0f0f0fu;
                hi = (hi | hi << 8) & 0x00ff00ffu;
                hi = (hi | hi << 4) & 0x0f0f0f0fu;
                uint32_t banks = bank * 0x01010101u;
                lo |= banks; hi |= banks;
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                __builtin_memcpy(__builtin_assume_aligned(output + x, 4), &lo, 4);
                __builtin_memcpy(__builtin_assume_aligned(output + x + 4, 4), &hi, 4);
#else
                ROW_PIXEL(0, 0); ROW_PIXEL(1, 4); ROW_PIXEL(2, 8); ROW_PIXEL(3, 12);
                ROW_PIXEL(4, 16); ROW_PIXEL(5, 20); ROW_PIXEL(6, 24); ROW_PIXEL(7, 28);
#endif
            }
#undef ROW_PIXEL
        } else if (flip) {
            packed <<= tx * 4;
            for (unsigned i = 0; i < count; ++i) {
                unsigned index = packed >> 28; packed <<= 4;
                output[x + i] = (uint8_t) (bank | index);
            }
        } else {
            packed >>= tx * 4;
            for (unsigned i = 0; i < count; ++i) {
                unsigned index = packed & 15; packed >>= 4;
                output[x + i] = (uint8_t) (bank | index);
            }
        }
    }
    return result;
}

static inline __attribute__((always_inline)) bool indexed_text_body(
        const struct GbnDevices* d, unsigned id, unsigned y, uint8_t* output, bool aligned) {
    unsigned control = d->io[4 + id], size = control >> 14;
    y = (y + d->io[9 + id * 2]) & (size & 2 ? 511 : 255);
    unsigned map_row = (control >> 8 & 31) * 0x800
        + (y >> 8) * (size & 1 ? 2 : 1) * 0x800 + (y & 248) * 8;
    unsigned xmask = size & 1 ? 511 : 255;
    unsigned sx = d->io[8 + id * 2] & xmask;
    unsigned character = (control >> 2 & 3) * 0x4000;
    uint32_t opaque = 0;
    bool transparent_tail = false;
    unsigned x = 0;
    if (sx & 7) {
        unsigned count = 8 - (sx & 7);
        unsigned tile = text_half(d->vram,
            (map_row + (sx >> 8) * 0x800 + (sx & 248) / 4) & 0xffff, aligned);
        opaque |= indexed_tile(d, tile, y & 7, character, output,
            sx & 7, count, GBN_SCREEN_WIDTH, aligned, &transparent_tail);
        x += count; sx = (sx + count) & xmask;
    }
    /* Each screen-block row is 32 consecutive map entries. Clip once at its
     * end and at the last complete output tile, then increment the map pointer.
     * The first/last partial tiles keep their original texel clipping. */
    while (x + 8 <= GBN_SCREEN_WIDTH) {
        unsigned tiles = (256 - (sx & 255)) / 8;
        unsigned available = (GBN_SCREEN_WIDTH - x) / 8;
        if (tiles > available) tiles = available;
        const uint8_t* map = d->vram
            + ((map_row + (sx >> 8) * 0x800 + (sx & 248) / 4) & 0xffff);
        unsigned end = x + tiles * 8;
        sx = (sx + tiles * 8) & xmask;
        do {
            unsigned tile = text_half(map, 0, aligned);
            opaque |= indexed_tile(d, tile, y & 7, character, output + x,
                0, 8, GBN_SCREEN_WIDTH - x, aligned, &transparent_tail);
            map += 2; x += 8;
        } while (x < end);
    }
    if (x < GBN_SCREEN_WIDTH) {
        unsigned tile = text_half(d->vram,
            (map_row + (sx >> 8) * 0x800 + (sx & 248) / 4) & 0xffff, aligned);
        opaque |= indexed_tile(d, tile, y & 7, character, output + x,
            0, GBN_SCREEN_WIDTH - x, GBN_SCREEN_WIDTH - x, aligned, &transparent_tail);
    }
    return opaque != 0;
}

static __attribute__((noinline)) bool indexed_text_unaligned(const struct GbnDevices* d,
        unsigned id, unsigned y, uint8_t* output) {
    return indexed_text_body(d, id, y, output, false);
}
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static __attribute__((noinline)) bool indexed_text_aligned(const struct GbnDevices* d,
        unsigned id, unsigned y, uint8_t* output) {
    return indexed_text_body(d, id, y, output, true);
}
#endif
static bool indexed_text(const struct GbnDevices* d, unsigned id, unsigned y, uint8_t* output) {
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (!((uintptr_t) d->vram & 3)) return indexed_text_aligned(d, id, y, output);
#endif
    return indexed_text_unaligned(d, id, y, output);
}
static inline __attribute__((always_inline)) void indexed_span_body(
        const struct IndexedBackground* layers, unsigned count, const struct Pixel* objects,
        unsigned start, unsigned end, const uint8_t* palette, bool aligned,
        uint16_t backdrop, uint16_t* output) {
    const uint8_t* a = count > 0 ? layers[0].indices : NULL;
    const uint8_t* b = count > 1 ? layers[1].indices : NULL;
    const uint8_t* c = count > 2 ? layers[2].indices : NULL;
    const uint8_t* d = count > 3 ? layers[3].indices : NULL;
    unsigned ka = count > 0 ? layers[0].key : 39;
    unsigned kb = count > 1 ? layers[1].key : 39;
    unsigned kc = count > 2 ? layers[2].key : 39;
    unsigned kd = count > 3 ? layers[3].key : 39;
    for (unsigned x = start; x < end; ++x) {
        unsigned index = 0, key = 39, candidate;
        if (count > 0 && ((candidate = a[x]) & 15)) { index = candidate; key = ka; goto selected; }
        if (count > 1 && ((candidate = b[x]) & 15)) { index = candidate; key = kb; goto selected; }
        if (count > 2 && ((candidate = c[x]) & 15)) { index = candidate; key = kc; goto selected; }
        if (count > 3 && ((candidate = d[x]) & 15)) { index = candidate; key = kd; }
    selected:
        if (objects && objects[x].key < key) output[x] = objects[x].color;
        else output[x] = key == 39 ? backdrop : text_half(palette, index * 2, aligned) & 0x7fff;
    }
}
static __attribute__((noinline)) void indexed_span(const struct IndexedBackground* layers,
        unsigned count, const struct Pixel* objects, unsigned start, unsigned end,
        const uint8_t* palette, uint16_t backdrop, uint16_t* output) {
    bool aligned = !((uintptr_t) palette & 1);
#if defined(AM_RVV) && (GBN_PPU_RVV & 4)
    if (aligned) {
        gbn_ppu_indexed_vector(layers, count, objects, start, end, palette, output);
        return;
    }
#endif
#define INDEX_CASE(n) case n: \
    if (objects) indexed_span_body(layers, n, objects, start, end, palette, aligned, backdrop, output); \
    else indexed_span_body(layers, n, NULL, start, end, palette, aligned, backdrop, output); \
    break
    switch (count) { INDEX_CASE(0); INDEX_CASE(1); INDEX_CASE(2); INDEX_CASE(3); INDEX_CASE(4); }
#undef INDEX_CASE
}
static inline bool indexed_supported(const struct GbnDevices* d, unsigned mode) {
    if (mode) return false;
    unsigned control = d->io[0x50 / 2], alpha = d->io[0x52 / 2];
    unsigned kind = control >> 6 & 3;
    bool identity_alpha = (alpha & 31) >= 16 && !(alpha & 0x1f00);
    if ((kind == 1 && identity_alpha) ||
        ((kind == 0 || (kind >= 2 && !(d->io[0x54 / 2] & 31))) &&
         (!(control & 0x3f00) || identity_alpha))) control = 0;
    if (control & 0x3f3f) return false;
    for (unsigned id = 0; id < 4; ++id)
        if ((d->io[0] & (0x100u << id)) && (d->io[4 + id] & 0xc0)) return false;
    return true;
}
static __attribute__((noinline)) void indexed_line(const struct GbnDevices* d, unsigned y, uint16_t* output) {
    _Alignas(4) uint8_t storage[4][GBN_SCREEN_WIDTH + 8];
    uint8_t* indices[4];
    for (unsigned id = 0; id < 4; ++id) indices[id] = storage[id] + (d->io[8 + id * 2] & 7);
    struct IndexedBackground layers[4]; unsigned count = 0;
    for (unsigned id = 0; id < 4; ++id) {
        if (!(d->io[0] & (0x100u << id)) || !indexed_text(d, id, y, indices[id])) continue;
        struct IndexedBackground layer = {indices[id], (uint8_t) ((d->io[4 + id] & 3) * 8 + id + 1), (uint8_t) id, (uint8_t) (1u << id)};
        unsigned at = count++;
        while (at && layers[at - 1].key > layer.key) { layers[at] = layers[at - 1]; --at; }
        layers[at] = layer;
    }
    struct Pixel obj[GBN_SCREEN_WIDTH]; bool object_window[GBN_SCREEN_WIDTH];
    bool have_objects = (d->io[0] & 0x1000) != 0;
    if (have_objects) {
        memset(object_window, 0, sizeof(object_window));
        for (unsigned x = 0; x < GBN_SCREEN_WIDTH; ++x) obj[x] = (struct Pixel) {0, 255, 4, false};
        objects(d, y, obj, object_window);
    }
    uint8_t masks[GBN_SCREEN_WIDTH];
    unsigned uniform = line_masks(d, y, object_window, have_objects, masks);
    uint16_t backdrop = half(d->palette, 0) & 0x7fff;
    for (unsigned start = 0; start < GBN_SCREEN_WIDTH;) {
        unsigned mask = masks[start], end = start + 1;
        if (uniform) end = GBN_SCREEN_WIDTH;
        else while (end < GBN_SCREEN_WIDTH && masks[end] == mask) ++end;
        struct IndexedBackground visible[4]; unsigned active = 0;
        for (unsigned i = 0; i < count; ++i)
            if (mask & layers[i].mask) visible[active++] = layers[i];
        indexed_span(visible, active, have_objects && (mask & 16) ? obj : NULL, start, end, d->palette, backdrop, output);
        start = end;
    }

}
#endif

enum GbnStatus gbn_ppu_line(struct Gbn* m, unsigned y) {
	if (y >= GBN_SCREEN_HEIGHT || !m->devices) return GBN_INVALID_ARGUMENT;
	struct GbnDevices* d = m->devices;
	struct GbnPpu* p = &d->ppu;
	unsigned mode = d->io[0] & 7;
	if (p->pixels && mode > 5 && !(d->io[0] & 0x80)) return GBN_UNSUPPORTED_DEVICE;
	if (!y) {
		memset(p->seen, 0, sizeof(p->seen));
		for (unsigned id = 0; id < 2; ++id) for (unsigned axis = 0; axis < 2; ++axis) gbn_ppu_reference(m, 0x28 + id * 16 + axis * 4);
	}
	if (p->pixels) {
		uint16_t* row = p->pixels + y * p->stride;
		if (d->io[0] & 0x80) for (unsigned x = 0; x < 240; ++x) row[x] = 0x7fff;
#if !defined(AM_RVV) || !(GBN_PPU_RVV & 2)
        else if (indexed_supported(d, mode)) indexed_line(d, y, row);
#endif
        else {
			uint16_t colors[4][240];
			struct Background layers[4]; unsigned count = 0;
			for (unsigned id = 0; id < 4; ++id) {
				if (!(d->io[0] & (0x100u << id))) continue;
				if (!mode || (mode == 1 && id < 2)) {
					if (!text_line(d, id, y, colors[id])) continue;
				}
				else if ((mode == 1 && id == 2) || (mode == 2 && id >= 2) || (mode >= 3 && id == 2)) affine_line(d, id, mode, y, colors[id]);
				else continue;
				struct Background layer = {colors[id], (uint8_t) ((d->io[4 + id] & 3) * 8 + id + 1), (uint8_t) id, (uint8_t) (1u << id)};
				unsigned at = count++;
				while (at && layers[at - 1].key > layer.key) { layers[at] = layers[at - 1]; --at; }
				layers[at] = layer;
			}
			struct Pixel obj[240]; bool object_window[240];
			bool have_objects = (d->io[0] & 0x1000) != 0;
			if (have_objects) {
				memset(object_window, 0, sizeof(object_window));
				for (unsigned x = 0; x < 240; ++x) obj[x] = (struct Pixel) {0, 255, 4, false};
				objects(d, y, obj, object_window);
			}
			uint16_t backdrop = half(d->palette, 0) & 0x7fff;
			uint8_t masks[GBN_SCREEN_WIDTH];
			unsigned uniform_mask = line_masks(d, y, object_window, have_objects, masks);
			(void) uniform_mask;
			unsigned blend_control = d->io[0x50 / 2];
			unsigned alpha_control = d->io[0x52 / 2];
			unsigned brightness_control = d->io[0x54 / 2];
			/* Normalize identity color equations before choosing the compositor.
			 * Zero brightness change is identity, but a semi-transparent OBJ may
			 * still force alpha blending when second targets are enabled. Alpha
			 * coefficients 16/0 are identity even in that case (after capping). */
			unsigned effect_kind = blend_control >> 6 & 3;
			bool identity_alpha = (alpha_control & 31) >= 16 && !(alpha_control & 0x1f00);
			if ((effect_kind == 1 && identity_alpha) ||
			    ((effect_kind == 0 || (effect_kind >= 2 && !(brightness_control & 31))) &&
			     (!(blend_control & 0x3f00) || identity_alpha))) blend_control = 0;
			/* With no effective blend targets, composition is only a
			 * priority-ordered transparent-pixel search.  This avoids constructing
			 * Pixel pairs and running the effect state machine for every pixel.  An
			 * object has already been reduced to the closest opaque object at each
			 * x coordinate, so it fits the same search without a special pass. */
			if (!(blend_control & 0x3f3f)) {
#if defined(AM_RVV) && (GBN_PPU_RVV & 2)
				gbn_ppu_compose_vector(layers, count, have_objects ? obj : NULL, masks, backdrop, row);
#else
				compose_windows(layers, count, have_objects ? obj : NULL, masks, uniform_mask, backdrop, row);
#endif
			} else for (unsigned x = 0; x < 240; ++x) {
				unsigned mask = masks[x];
				struct Pixel top = {backdrop, 39, 5, false}, second = top;
				second.key = 255;
				if (have_objects && (mask & 16) && obj[x].key != 255) insert(&top, &second, obj[x]);
				/* There are at most four backgrounds.  Expand this hot loop so
				 * priority and window tests do not pay an induction variable and
				 * layer-pointer load for every candidate pixel. */
				if (count > 0 && layers[0].key < second.key && (mask & layers[0].mask)) {
					uint16_t color = layers[0].colors[x];
					if (!(color & TRANSPARENT)) insert(&top, &second, (struct Pixel) {color, layers[0].key, layers[0].layer, false});
				}
				if (count > 1 && layers[1].key < second.key && (mask & layers[1].mask)) {
					uint16_t color = layers[1].colors[x];
					if (!(color & TRANSPARENT)) insert(&top, &second, (struct Pixel) {color, layers[1].key, layers[1].layer, false});
				}
				if (count > 2 && layers[2].key < second.key && (mask & layers[2].mask)) {
					uint16_t color = layers[2].colors[x];
					if (!(color & TRANSPARENT)) insert(&top, &second, (struct Pixel) {color, layers[2].key, layers[2].layer, false});
				}
				if (count > 3 && layers[3].key < second.key && (mask & layers[3].mask)) {
					uint16_t color = layers[3].colors[x];
					if (!(color & TRANSPARENT)) insert(&top, &second, (struct Pixel) {color, layers[3].key, layers[3].layer, false});
				}
				row[x] = effect_values(blend_control, alpha_control, brightness_control, top, second, (mask & 32) != 0);
			}
		}
		if (d->io[1] & 1) for (unsigned x = 0; x < 240; x += 2) {
			uint16_t green = row[x] & 0x3e0;
			row[x] = (row[x] & ~0x3e0u) | (row[x + 1] & 0x3e0);
			row[x + 1] = (row[x + 1] & ~0x3e0u) | green;
		}
		++p->lines; p->seen[y / 32] |= 1u << (y & 31);
		if (y == 159 && p->seen[0] == UINT32_MAX && p->seen[1] == UINT32_MAX && p->seen[2] == UINT32_MAX && p->seen[3] == UINT32_MAX && p->seen[4] == UINT32_MAX) ++p->frames;
	}
	for (unsigned id = 0; id < 2; ++id) for (unsigned axis = 0; axis < 2; ++axis) {
		unsigned offset = (0x22 + id * 16 + axis * 4) / 2;
		p->reference[id][axis] = signed28((uint32_t) p->reference[id][axis] + (uint32_t) (int32_t) (int16_t) d->io[offset]);
	}
	return GBN_STEP;
}
