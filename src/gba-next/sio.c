/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"

/* Disconnected link port. Serial clocks and IRQs still run; there is no
 * fabricated peer. UART and external Joybus commands need separate backends. */
bool gbn_sio_register(unsigned offset) {
	return (offset >= 0x120 && offset < 0x130) || offset == 0x134;
}

static unsigned mode(uint16_t rcnt, uint16_t control) {
	return rcnt & 0x8000 ? (rcnt >> 12) & 12 : (control >> 12) & 3;
}

static void switch_mode(struct Gbn* m) {
	struct GbnDevices* d = m->devices;
	unsigned next = mode(d->io[0x134 / 2], d->sio.control);
	if (next == d->sio.mode) return;
	gbn_cancel(m, GBN_EVENT_SIO);
	d->sio.mode = (uint8_t) next;
	if (next == 2) d->io[0x134 / 2] &= ~4u; /* Local ID is zero. */
}

static void control_write(struct Gbn* m, uint16_t value) {
	struct GbnDevices* d = m->devices;
	uint16_t old = d->sio.control;
	if ((old ^ value) & 0x3000) {
		d->sio.control = value & 0x3000;
		switch_mode(m);
		old = d->sio.control;
	}
	unsigned cycles = 0;
	if (d->sio.mode < 2) {
		value |= 4; /* SI pull-up, no connected sender. */
		if (value & 1) d->io[0x134 / 2] |= 1;
		if ((value & 0x81) == 0x81 && !(old & 0x80)) cycles = (d->sio.mode ? 32 : 8) * (value & 2 ? 8 : 64);
	} else if (d->sio.mode == 2) {
		value = (value & 0x7f83) | (old & 0xfc) | 0x0c;
		d->io[0x134 / 2] |= 1;
		if ((value & 0x80) && !(old & 0x80)) {
			static const uint16_t duration[] = {31976, 8378, 5750, 3140};
			cycles = duration[value & 3];
			d->io[0x134 / 2] &= ~1u;
			for (unsigned i = 0; i < 4; ++i) d->io[(0x120 + 2 * i) / 2] = 0xffff;
		}
	}
	d->sio.control = value;
	if (!(value & 0x80)) gbn_cancel(m, GBN_EVENT_SIO);
	if (d->sio.mode == 3) gbn_schedule(m, GBN_EVENT_SIO, 0, 0x80);
	if (cycles) {
		d->sio.transfer_mode = d->sio.mode;
		gbn_schedule(m, GBN_EVENT_SIO, cycles, 0x80);
	}
}

enum GbnStatus gbn_sio_write(struct Gbn* m, unsigned offset, unsigned width, uint32_t value) {
	struct GbnDevices* d = m->devices;
	for (unsigned i = 0; i < width; i += width == 1 ? 1 : 2) {
		unsigned part = (offset + i) & ~1u;
		uint16_t data = (uint16_t) (value >> (8 * i));
		if (width == 1) {
			unsigned shift = (offset & 1) * 8;
			data = (uint16_t) ((d->io[part / 2] & ~(255u << shift)) | (data & 255) << shift);
		}
		if (part == 0x128) {
			data &= 0x7fff;
			control_write(m, data);
			d->io[part / 2] = data;
		} else if (part == 0x134) {
			uint16_t old = d->io[part / 2];
			d->io[part / 2] = (old & 0x1ff) | (data & 0xc000);
			switch_mode(m);
			d->io[part / 2] = (data & 0xc000) | (d->sio.mode == 8 ? data & 0x1ff : (data & 0x1f0) | (d->io[part / 2] & 15));
		} else if ((part == 0x120 || part == 0x122) && d->sio.mode == 1) d->io[part / 2] = data;
		else if (part == 0x12a && (d->sio.mode <= 2 || d->sio.mode == 12)) d->io[part / 2] = data;
	}
	return GBN_STEP;
}

enum GbnStatus gbn_sio_service(struct Gbn* m, uint32_t when) {
	struct GbnDevices* d = m->devices;
	/* Leave the event and selected mode visible to the diagnostic frontend. */
	if (d->sio.mode == 3) return GBN_UNSUPPORTED_DEVICE;
	gbn_cancel(m, GBN_EVENT_SIO);
	if (d->sio.mode != d->sio.transfer_mode || !(d->sio.control & 0x80)) return GBN_STEP;
	d->sio.control &= ~0x80u;
	if (d->sio.mode == 2) {
		d->io[0x120 / 2] = d->io[0x12a / 2];
		d->io[0x134 / 2] |= 1;
	} else if (d->sio.mode == 1) d->io[0x120 / 2] = d->io[0x122 / 2] = 0xffff;
	else d->io[0x12a / 2] = 0xff;
	if (d->sio.control & 0x4000) gbn_irq_at(m, 0x80, when);
	return GBN_STEP;
}
