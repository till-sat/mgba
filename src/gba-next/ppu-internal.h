/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_NEXT_PPU_INTERNAL_H
#define GBA_NEXT_PPU_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#ifndef GBN_PPU_RVV
#define GBN_PPU_RVV 0
#endif
/* One aligned word keeps the color, priority and effect metadata together. */
struct Pixel { uint16_t color; uint8_t key; uint8_t layer : 7; bool alpha : 1; }
    __attribute__((aligned(4)));
_Static_assert(sizeof(struct Pixel) == 4, "Pixel must occupy one word");
_Static_assert(_Alignof(struct Pixel) == 4, "Pixel arrays must support word stores");
#define TRANSPARENT 0x8000u
struct Background { const uint16_t* colors; uint8_t key, layer, mask; };
struct IndexedBackground { const uint8_t* indices; uint8_t key, layer, mask; };
void gbn_ppu_row4_vector(uint32_t packed, const uint8_t* palette, uint16_t* output,
    unsigned tx, unsigned count, bool flip);
void gbn_ppu_compose_vector(const struct Background* layers, unsigned count,
    const struct Pixel* objects, const uint8_t* masks, uint16_t backdrop, uint16_t* output);
void gbn_ppu_indexed_vector(const struct IndexedBackground* layers, unsigned count,
    const struct Pixel* objects, unsigned start, unsigned end,
    const uint8_t* palette, uint16_t* output);
/* Decode a 31-tile 4bpp text row (map entries row_map, tile row sy7 of the
 * character block at character) into indices (8 per tile), the tile word
 * index and word of every tile, as the scalar decoder does. Returns 0 when a
 * tile lies outside BG VRAM (the caller decodes then), else 1 | 2 when
 * tiles 0..29 have an opaque pixel | 4 when tiles 0..30 do. */
unsigned gbn_ppu_row_decode_vector(const uint8_t* vram, const uint16_t* row_map,
    unsigned character, unsigned sy7, uint8_t* indices, uint16_t* address, uint32_t* words);
/* Copy n bytes between non-overlapping buffers; the second form returns
 * whether any destination byte changed. */
void gbn_rvv_copy(uint8_t* dst, const uint8_t* src, unsigned n);
bool gbn_rvv_copy_changed(uint8_t* dst, const uint8_t* src, unsigned n);
/* Store value into n consecutive words. */
void gbn_rvv_fill32(uint32_t* p, uint32_t value, unsigned n);
/* n bytes at a and b are equal (cache validation of translated RAM code). */
bool gbn_rvv_equal(const uint8_t* a, const uint8_t* b, unsigned n);
/* A cached text row still matches VRAM: its first `first` map entries at map,
 * the rest at next_map, and its sampled tile words (word indices in address). */
bool gbn_ppu_row_matches_vector(const uint8_t* vram, const uint8_t* map, unsigned first,
    const uint8_t* next_map, const uint16_t* row_map, const uint16_t* address,
    const uint32_t* words, unsigned samples);
#endif
