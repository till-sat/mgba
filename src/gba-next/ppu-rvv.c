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

#if GBN_PPU_RVV & 32
unsigned gbn_ppu_row_decode_vector(const uint8_t* vram, const uint16_t* row_map,
    unsigned character, unsigned sy7, uint8_t* indices, uint16_t* address, uint32_t* words) {
    size_t vl = __riscv_vsetvl_e32m8(31);
    vuint32m8_t e = __riscv_vzext_vf2_u32m8(__riscv_vle16_v_u16m4(row_map, vl), vl);
    vbool4_t vflip = __riscv_vmsne_vx_u32m8_b4(__riscv_vand_vx_u32m8(e, 0x800, vl), 0, vl);
    vbool4_t hflip = __riscv_vmsne_vx_u32m8_b4(__riscv_vand_vx_u32m8(e, 0x400, vl), 0, vl);
    vuint32m8_t ty = __riscv_vmerge_vxm_u32m8(__riscv_vmv_v_x_u32m8(sy7, vl), 7 - sy7, vflip, vl);
    vuint32m8_t at = __riscv_vsll_vx_u32m8(__riscv_vand_vx_u32m8(e, 0x3ff, vl), 5, vl);
    at = __riscv_vadd_vx_u32m8(__riscv_vadd_vv_u32m8(at, __riscv_vsll_vx_u32m8(ty, 2, vl), vl), character, vl);
    if (__riscv_vcpop_m_b4(__riscv_vmsltu_vx_u32m8_b4(at, 0x10000, vl), vl) != vl) return 0;
    vuint32m8_t w = __riscv_vluxei32_v_u32m8((const uint32_t*) vram, at, vl);
    __riscv_vse32_v_u32m8(words, w, vl);
    __riscv_vse16_v_u16m4(address, __riscv_vnsrl_wx_u16m4(at, 2, vl), vl);
    /* Horizontal flip: nibbles in reverse order (swap within bytes, then
     * reverse the bytes). */
    vuint32m8_t s = __riscv_vor_vv_u32m8(
        __riscv_vand_vx_u32m8(__riscv_vsrl_vx_u32m8(w, 4, vl), 0x0f0f0f0fu, vl),
        __riscv_vsll_vx_u32m8(__riscv_vand_vx_u32m8(w, 0x0f0f0f0fu, vl), 4, vl), vl);
    vuint32m8_t r = __riscv_vor_vv_u32m8(__riscv_vsrl_vx_u32m8(s, 24, vl), __riscv_vsll_vx_u32m8(s, 24, vl), vl);
    r = __riscv_vor_vv_u32m8(r, __riscv_vand_vx_u32m8(__riscv_vsrl_vx_u32m8(s, 8, vl), 0xff00u, vl), vl);
    r = __riscv_vor_vv_u32m8(r, __riscv_vand_vx_u32m8(__riscv_vsll_vx_u32m8(s, 8, vl), 0xff0000u, vl), vl);
    vuint32m8_t src = __riscv_vmerge_vvm_u32m8(w, r, hflip, vl);
    /* Spread four nibbles into byte lanes, as the scalar row decoder does. */
    vuint32m8_t lo = __riscv_vand_vx_u32m8(src, 0xffff, vl), hi = __riscv_vsrl_vx_u32m8(src, 16, vl);
    lo = __riscv_vand_vx_u32m8(__riscv_vor_vv_u32m8(lo, __riscv_vsll_vx_u32m8(lo, 8, vl), vl), 0x00ff00ffu, vl);
    lo = __riscv_vand_vx_u32m8(__riscv_vor_vv_u32m8(lo, __riscv_vsll_vx_u32m8(lo, 4, vl), vl), 0x0f0f0f0fu, vl);
    hi = __riscv_vand_vx_u32m8(__riscv_vor_vv_u32m8(hi, __riscv_vsll_vx_u32m8(hi, 8, vl), vl), 0x00ff00ffu, vl);
    hi = __riscv_vand_vx_u32m8(__riscv_vor_vv_u32m8(hi, __riscv_vsll_vx_u32m8(hi, 4, vl), vl), 0x0f0f0f0fu, vl);
    vuint32m8_t bank = __riscv_vmul_vx_u32m8(__riscv_vsll_vx_u32m8(__riscv_vsrl_vx_u32m8(e, 12, vl), 4, vl), 0x01010101u, vl);
    vbool4_t opaque = __riscv_vmsne_vx_u32m8_b4(w, 0, vl);
    vuint32m8_t zero = __riscv_vmv_v_x_u32m8(0, vl);
    lo = __riscv_vmerge_vvm_u32m8(zero, __riscv_vor_vv_u32m8(lo, bank, vl), opaque, vl);
    hi = __riscv_vmerge_vvm_u32m8(zero, __riscv_vor_vv_u32m8(hi, bank, vl), opaque, vl);
    __riscv_vsse32_v_u32m8((uint32_t*) indices, 8, lo, vl);
    __riscv_vsse32_v_u32m8((uint32_t*) (indices + 4), 8, hi, vl);
    unsigned flags = 1;
    if (__riscv_vcpop_m_b4(opaque, 30)) flags |= 2;
    if (__riscv_vcpop_m_b4(opaque, vl)) flags |= 4;
    return flags;
}
#endif

void gbn_rvv_copy(uint8_t* dst, const uint8_t* src, unsigned n) {
    for (unsigned i = 0; i < n;) {
        size_t vl = __riscv_vsetvl_e8m8(n - i);
        __riscv_vse8_v_u8m8(dst + i, __riscv_vle8_v_u8m8(src + i, vl), vl);
        i += (unsigned) vl;
    }
}

bool gbn_rvv_copy_changed(uint8_t* dst, const uint8_t* src, unsigned n) {
    unsigned changed = 0;
    for (unsigned i = 0; i < n;) {
        size_t vl = __riscv_vsetvl_e8m8(n - i);
        vuint8m8_t value = __riscv_vle8_v_u8m8(src + i, vl);
        changed |= __riscv_vcpop_m_b1(__riscv_vmsne_vv_u8m8_b1(value, __riscv_vle8_v_u8m8(dst + i, vl), vl), vl);
        __riscv_vse8_v_u8m8(dst + i, value, vl);
        i += (unsigned) vl;
    }
    return changed != 0;
}

void gbn_rvv_fill32(uint32_t* p, uint32_t value, unsigned n) {
    for (unsigned i = 0; i < n;) {
        size_t vl = __riscv_vsetvl_e32m8(n - i);
        __riscv_vse32_v_u32m8(p + i, __riscv_vmv_v_x_u32m8(value, vl), vl);
        i += (unsigned) vl;
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

#if GBN_PPU_RVV & 16
/* With objects: 32-pixel chunks, so that one unit-stride e32 load brings the
 * object pixels (color, key) in and two narrowing shifts split them, instead
 * of two strided loads. */
static void indexed_vector_objects(const struct IndexedBackground* layers, unsigned count,
    const struct Pixel* objects, unsigned start, unsigned end,
    const uint8_t* palette, uint16_t* output) {
    for (unsigned x = start; x < end;) {
        size_t vl = __riscv_vsetvl_e8m2(end - x);
        vuint8m2_t indices = __riscv_vmv_v_x_u8m2(0, vl);
        vuint8m2_t keys = __riscv_vmv_v_x_u8m2(39, vl);
        for (unsigned i = count; i > 0; --i) {
            const struct IndexedBackground* layer = layers + i - 1;
            vuint8m2_t candidate = __riscv_vle8_v_u8m2(layer->indices + x, vl);
            vbool4_t opaque = __riscv_vmsne_vx_u8m2_b4(__riscv_vand_vx_u8m2(candidate, 15, vl), 0, vl);
            indices = __riscv_vmerge_vvm_u8m2(indices, candidate, opaque, vl);
            keys = __riscv_vmerge_vxm_u8m2(keys, layer->key, opaque, vl);
        }
        vuint16m4_t offsets = __riscv_vsll_vx_u16m4(__riscv_vzext_vf2_u16m4(indices, vl), 1, vl);
        vuint16m4_t colors = __riscv_vluxei16_v_u16m4((const uint16_t*) palette, offsets, vl);
        colors = __riscv_vand_vx_u16m4(colors, 0x7fff, vl);
        vuint32m8_t pixels = __riscv_vle32_v_u32m8((const uint32_t*) (objects + x), vl);
        vuint16m4_t obj_colors = __riscv_vnsrl_wx_u16m4(pixels, 0, vl);
        vuint8m2_t obj_keys = __riscv_vnsrl_wx_u8m2(__riscv_vnsrl_wx_u16m4(pixels, 16, vl), 0, vl);
        vbool4_t allowed = __riscv_vmsltu_vv_u8m2_b4(obj_keys, keys, vl);
        colors = __riscv_vmerge_vvm_u16m4(colors, obj_colors, allowed, vl);
        __riscv_vse8_v_u8m4((uint8_t*) (output + x), __riscv_vreinterpret_v_u16m4_u8m4(colors), 2 * vl);
        x += (unsigned) vl;
    }
}
#endif

void gbn_ppu_indexed_vector(const struct IndexedBackground* layers, unsigned count,
    const struct Pixel* objects, unsigned start, unsigned end,
    const uint8_t* palette, uint16_t* output) {
#if GBN_PPU_RVV & 16
    if (objects) indexed_vector_objects(layers, count, objects, start, end, palette, output);
#else
    if (objects) indexed_vector_body(layers, count, objects, start, end, palette, output);
#endif
    else indexed_vector_body(layers, count, NULL, start, end, palette, output);
}
#endif
