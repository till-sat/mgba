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
#endif
