/* SPDX-License-Identifier: MPL-2.0 */
#include <gba-next/core.h>

static const uint8_t bios_image[GBN_BIOS_SIZE] = {
#include "bios.inc"
};

void gbn_use_builtin_bios(struct Gbn* m) {
	gbn_attach_bios(m, bios_image, sizeof(bios_image));
	m->builtin_bios = true;
}
