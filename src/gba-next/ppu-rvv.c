/* SPDX-License-Identifier: MPL-2.0 */
#include "ppu-internal.h"
#if defined(__riscv_vector) && GBN_PPU_RVV
#include <riscv_vector.h>
#endif

#if defined(__riscv_vector) && GBN_PPU_RVV
/* Integer vectors only (Zve32x). Select one of the packed word's two halves
 * per lane, then decode entirely at e16,m1, avoiding repeated vtype changes.
 * VLMAX is not assumed. */
void gbn_ppu_row4_vector(uint32_t packed, const uint8_t* palette, uint16_t* output,
	unsigned tx, unsigned count, bool flip) {
	for (unsigned i = 0; i < count;) {
		size_t vl = __riscv_vsetvl_e16m1(count - i);
		vuint16m1_t shift = __riscv_vid_v_u16m1(vl);
		shift = __riscv_vadd_vx_u16m1(shift, tx + i, vl);
		if (flip) shift = __riscv_vrsub_vx_u16m1(shift, 7, vl);
		vbool16_t upper = __riscv_vmsgtu_vx_u16m1_b16(shift, 3, vl);
		vuint16m1_t indices = __riscv_vmv_v_x_u16m1((uint16_t) packed, vl);
		indices = __riscv_vmerge_vxm_u16m1(indices, packed >> 16, upper, vl);
		shift = __riscv_vand_vx_u16m1(shift, 3, vl);
		shift = __riscv_vsll_vx_u16m1(shift, 2, vl);
		indices = __riscv_vsrl_vv_u16m1(indices, shift, vl);
		indices = __riscv_vand_vx_u16m1(indices, 15, vl);
		vbool16_t transparent = __riscv_vmseq_vx_u16m1_b16(indices, 0, vl);
		vuint16m1_t offsets = indices;
		offsets = __riscv_vsll_vx_u16m1(offsets, 1, vl);
		vuint16m1_t colors = __riscv_vluxei16_v_u16m1((const uint16_t*) palette, offsets, vl);
		colors = __riscv_vand_vx_u16m1(colors, 0x7fff, vl);
		colors = __riscv_vmerge_vxm_u16m1(colors, TRANSPARENT, transparent, vl);
		__riscv_vse16_v_u16m1(output + i, colors, vl);
		i += (unsigned) vl;
	}
}

/* No color effects: traverse backgrounds from farthest to closest, then
 * insert the already-selected OBJ if its priority wins. Every mask retains
 * the scalar compositor's transparency, window and tie-breaking rules. */
void gbn_ppu_compose_vector(const struct Background* layers, unsigned count,
	const struct Pixel* objects, const uint8_t* masks, uint16_t backdrop, uint16_t* output) {
	for (unsigned x = 0; x < 240;) {
		size_t vl = __riscv_vsetvl_e16m1(240 - x);
		vuint8mf2_t windows = __riscv_vle8_v_u8mf2(masks + x, vl);
		vuint16m1_t colors = __riscv_vmv_v_x_u16m1(backdrop, vl);
		vuint16m1_t keys = __riscv_vmv_v_x_u16m1(39, vl);
		for (unsigned i = count; i > 0; --i) {
			const struct Background* layer = layers + i - 1;
			vuint16m1_t candidate = __riscv_vle16_v_u16m1(layer->colors + x, vl);
			vbool16_t allowed = __riscv_vmsne_vx_u8mf2_b16(__riscv_vand_vx_u8mf2(windows, layer->mask, vl), 0, vl);
			vbool16_t opaque = __riscv_vmsltu_vx_u16m1_b16(candidate, TRANSPARENT, vl);
			allowed = __riscv_vmand_mm_b16(allowed, opaque, vl);
			colors = __riscv_vmerge_vvm_u16m1(colors, candidate, allowed, vl);
			if (objects) keys = __riscv_vmerge_vxm_u16m1(keys, layer->key, allowed, vl);
		}
		if (objects) {
			vuint8mf2_t obj_keys8 = __riscv_vlse8_v_u8mf2(&objects[x].key, sizeof(*objects), vl);
			vuint16m1_t obj_keys = __riscv_vzext_vf2_u16m1(obj_keys8, vl);
			vbool16_t allowed = __riscv_vmsne_vx_u8mf2_b16(__riscv_vand_vx_u8mf2(windows, 16, vl), 0, vl);
			allowed = __riscv_vmand_mm_b16(allowed, __riscv_vmsltu_vv_u16m1_b16(obj_keys, keys, vl), vl);
			vuint16m1_t candidate = __riscv_vlse16_v_u16m1(&objects[x].color, sizeof(*objects), vl);
			colors = __riscv_vmerge_vvm_u16m1(colors, candidate, allowed, vl);
		}
		__riscv_vse16_v_u16m1(output + x, colors, vl);
		x += (unsigned) vl;
	}
}

/* Window permissions were resolved once for this span. Keep palette indices
 * and priorities at e8,m4, then widen only the winning indices for the color
 * gather. Palette zero supplies the backdrop, including its RGB555 mask. */
static inline __attribute__((always_inline)) void indexed_vector_body(
    const struct IndexedBackground* layers, unsigned count,
    const struct Pixel* objects, unsigned start, unsigned end,
    const uint8_t* palette, uint16_t* output) {
    for (unsigned x = start; x < end;) {
        size_t vl = __riscv_vsetvl_e8m4(end - x);
        vuint8m4_t indices = __riscv_vmv_v_x_u8m4(0, vl);
        vuint8m4_t keys = __riscv_vmv_v_x_u8m4(39, vl);
        for (unsigned i = count; i > 0; --i) {
            const struct IndexedBackground* layer = layers + i - 1;
            vuint8m4_t candidate = __riscv_vle8_v_u8m4(layer->indices + x, vl);
            vuint8m4_t texels = __riscv_vand_vx_u8m4(candidate, 15, vl);
            vbool2_t opaque = __riscv_vmsne_vx_u8m4_b2(texels, 0, vl);
            indices = __riscv_vmerge_vvm_u8m4(indices, candidate, opaque, vl);
            if (objects) keys = __riscv_vmerge_vxm_u8m4(keys, layer->key, opaque, vl);
        }
        vuint16m8_t offsets = __riscv_vzext_vf2_u16m8(indices, vl);
        offsets = __riscv_vsll_vx_u16m8(offsets, 1, vl);
        vuint16m8_t colors = __riscv_vluxei16_v_u16m8((const uint16_t*) palette, offsets, vl);
        colors = __riscv_vand_vx_u16m8(colors, 0x7fff, vl);
        if (objects) {
            vuint8m4_t obj_keys = __riscv_vlse8_v_u8m4(&objects[x].key, sizeof(*objects), vl);
            vbool2_t allowed = __riscv_vmsltu_vv_u8m4_b2(obj_keys, keys, vl);
            vuint16m8_t obj_colors = __riscv_vlse16_v_u16m8(&objects[x].color, sizeof(*objects), vl);
            colors = __riscv_vmerge_vvm_u16m8(colors, obj_colors, allowed, vl);
        }
        /* Unit-stride byte stores use this core's word grouping and preserve
         * arbitrary halfword-aligned output and exact short-span tails. */
        __riscv_vse8_v_u8m8((uint8_t*) (output + x),
            __riscv_vreinterpret_v_u16m8_u8m8(colors), 2 * vl);
        x += (unsigned) vl;
    }
}

bool gbn_rvv_equal(const uint8_t* a, const uint8_t* b, unsigned n) {
    for (unsigned i = 0; i < n;) {
        size_t vl = __riscv_vsetvl_e8m8(n - i);
        vbool1_t differ = __riscv_vmsne_vv_u8m8_b1(__riscv_vle8_v_u8m8(a + i, vl),
            __riscv_vle8_v_u8m8(b + i, vl), vl);
        if (__riscv_vcpop_m_b1(differ, vl)) return false;
        i += (unsigned) vl;
    }
    return true;
}

bool gbn_ppu_row_matches_vector(const uint8_t* vram, const uint8_t* map, unsigned first,
    const uint8_t* next_map, const uint16_t* row_map, const uint16_t* address,
    const uint32_t* words, unsigned samples) {
    /* Map entries are halfword aligned in VRAM; compare them as bytes. */
    size_t vl = __riscv_vsetvl_e8m4(first * 2);
    vbool2_t differ = __riscv_vmsne_vv_u8m4_b2(__riscv_vle8_v_u8m4(map, vl),
        __riscv_vle8_v_u8m4((const uint8_t*) row_map, vl), vl);
    if (__riscv_vcpop_m_b2(differ, vl)) return false;
    if (first < 31) {
        vl = __riscv_vsetvl_e8m4((31 - first) * 2);
        differ = __riscv_vmsne_vv_u8m4_b2(__riscv_vle8_v_u8m4(next_map, vl),
            __riscv_vle8_v_u8m4((const uint8_t*) (row_map + first), vl), vl);
        if (__riscv_vcpop_m_b2(differ, vl)) return false;
    }
    if (!samples) return true;
    vl = __riscv_vsetvl_e32m8(samples);
    vuint32m8_t offsets = __riscv_vsll_vx_u32m8(__riscv_vzext_vf2_u32m8(
        __riscv_vle16_v_u16m4(address, vl), vl), 2, vl);
    vuint32m8_t current = __riscv_vluxei32_v_u32m8((const uint32_t*) vram, offsets, vl);
    vbool4_t changed = __riscv_vmsne_vv_u32m8_b4(current, __riscv_vle32_v_u32m8(words, vl), vl);
    return !__riscv_vcpop_m_b4(changed, vl);
}

void gbn_ppu_indexed_vector(const struct IndexedBackground* layers, unsigned count,
    const struct Pixel* objects, unsigned start, unsigned end,
    const uint8_t* palette, uint16_t* output) {
    if (objects) indexed_vector_body(layers, count, objects, start, end, palette, output);
    else indexed_vector_body(layers, count, NULL, start, end, palette, output);
}
#endif
