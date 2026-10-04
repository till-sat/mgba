/* SPDX-License-Identifier: MPL-2.0 */
/* Host devices only: every emulator instruction executes on the RTL core. */
#include "media-device.h"
#include "rtl-media.h"

static AMMediaDevice media("mGBA / proto-core RTL");
static uint32_t buttons;

void am_rtl_tick() {
	media.tick();
	buttons = am_input_read().buttons;
}

uint32_t am_rtl_buttons() { return buttons; }

extern "C" int am_rtl_read(unsigned address, unsigned* value) {
	uint8_t bytes[4] = {};
	bool ok = media.load(address - AM_SIM_BASE, sizeof(bytes), bytes);
	*value = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
	         uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
	return ok ? 0 : 2; /* AXI OKAY / SLVERR */
}

extern "C" int am_rtl_write(unsigned address, unsigned value) {
	uint8_t bytes[4];
	for (unsigned i = 0; i < 4; ++i) bytes[i] = value >> (8 * i);
	bool ok = media.store(address - AM_SIM_BASE, sizeof(bytes), bytes);
	/* A presentation can inject input; make it visible at the next GPIO read. */
	if (address < AM_SIM_BASE + AM_SIM_FB) am_rtl_tick();
	return ok ? 0 : 2;
}
