/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include <stddef.h>
#include <string.h>

enum {
	DISPCNT = 0x000 / 2, DISPSTAT = 0x004 / 2, VCOUNT = 0x006 / 2,
	KEYINPUT = 0x130 / 2, KEYCNT = 0x132 / 2,
	IE = 0x200 / 2, IF = 0x202 / 2, WAITCNT = 0x204 / 2, IME = 0x208 / 2
};

bool gbn_attach_devices(struct Gbn* m, struct GbnDevices* d, uint8_t* palette, uint8_t* vram, uint8_t* oam) {
	if (!d || !palette || !vram || !oam) return false;
	memset(d, 0, sizeof(*d));
	d->palette = palette; d->vram = vram; d->oam = oam;
	d->io[DISPCNT] = 0x80;
	d->io[0x20 / 2] = d->io[0x26 / 2] = d->io[0x30 / 2] = d->io[0x36 / 2] = 0x100;
	d->io[KEYINPUT] = 0x3ff;
	d->io[0x134 / 2] = 0x8000;
	d->io[0x88 / 2] = 0x200;
	d->io[WAITCNT] = m->waitcnt;
	m->devices = d;
	d->active_dma = -1;
	d->sio.mode = 8;
	for (unsigned i = 0; i < 4; ++i) d->dma[i].count = i == 3 ? 0x10000 : 0x4000;
	for (unsigned i = GBN_EVENT_SAVE; i < GBN_EVENT_COUNT; ++i) gbn_cancel(m, i);
	return true;
}

void gbn_update_irq(struct Gbn* m, uint32_t when) {
	if (!m->devices) return;
	const uint16_t* io = m->devices->io;
	if ((io[IE] & io[IF]) && !m->events[GBN_EVENT_IRQ].active) gbn_schedule_at(m, GBN_EVENT_IRQ, when + 7, 0);
}

void gbn_irq_at(struct Gbn* m, uint16_t mask, uint32_t when) {
	m->devices->io[IF] |= mask & 0x3fff;
	gbn_update_irq(m, when);
}

void gbn_request_irq(struct Gbn* m, uint16_t mask) {
	if (m->devices) gbn_irq_at(m, mask, m->now);
}

static void check_keys(struct Gbn* m) {
	uint16_t control = m->devices->io[KEYCNT];
	unsigned selected = control & 0x3ff;
	unsigned pressed = ~m->devices->io[KEYINPUT] & selected;
	if ((control & 0x4000) && (control & 0x8000 ? pressed == selected : pressed != 0)) gbn_request_irq(m, 0x1000);
}

void gbn_set_keys(struct Gbn* m, uint16_t pressed) {
	if (!m->devices) return;
	m->devices->io[KEYINPUT] = ~pressed & 0x3ff;
	check_keys(m);
}

static void vcount_match(struct Gbn* m, uint32_t when) {
	uint16_t* io = m->devices->io;
	bool old = (io[DISPSTAT] & 4) != 0;
	bool match = io[VCOUNT] == (io[DISPSTAT] >> 8);
	io[DISPSTAT] = (io[DISPSTAT] & ~4u) | (match ? 4 : 0);
	if (match && !old && (io[DISPSTAT] & 0x20)) gbn_irq_at(m, 4, when);
}

void gbn_video_start(struct Gbn* m, bool skip_bios) {
	if (!m->devices) return;
	struct GbnDevices* d = m->devices;
	d->video_running = true;
	d->postflag = skip_bios ? 1 : 0;
	d->frames = 0;
	d->io[VCOUNT] = skip_bios ? 126 : 0;
	d->io[DISPSTAT] &= ~7u;
	vcount_match(m, m->now);
	gbn_schedule(m, GBN_EVENT_VIDEO, skip_bios ? 120 : 1008, 8);
}

enum GbnStatus gbn_service_events(struct Gbn* m) {
	if (!m->devices) return GBN_INVALID_ARGUMENT;
	struct GbnDevices* d = m->devices;
	if (d->stopped) return GBN_UNSUPPORTED_DEVICE;
	bool advanced = false;
	for (;;) {
		int id = m->next_event;
		if (id < 0) return d->halted ? GBN_EVENT : GBN_STEP;
		if ((int32_t) (m->now - m->events[id].when) < 0) {
			if (!d->dma_blocked && (!d->halted || advanced)) return GBN_STEP;
			m->now = m->events[id].when;
			advanced = true;
		}
		if (id < (int) GBN_EVENT_SAVE) return GBN_EVENT;
		uint32_t when = m->events[id].when;
		if (id == GBN_EVENT_SAVE) {
			gbn_cancel(m, GBN_EVENT_SAVE);
			if (m->save) m->save->busy = false;
			continue;
		}
		if (id == GBN_EVENT_SIO) {
			enum GbnStatus status = gbn_sio_service(m, when);
			if (status != GBN_STEP) return status;
			continue;
		}
		if (id >= (int) GBN_EVENT_TIMER0 && id < (int) GBN_EVENT_AUDIO) {
			gbn_timer_service(m, (unsigned) id - GBN_EVENT_TIMER0, when);
			continue;
		}
		if (id == GBN_EVENT_AUDIO_FRAME) { gbn_audio_frame(m, when); continue; }
		if (id == GBN_EVENT_AUDIO) { gbn_audio_service(m, when); continue; }
		if (id == GBN_EVENT_DMA) {
			uint32_t before = m->now;
			enum GbnStatus status = gbn_dma_service(m);
			advanced |= before != m->now;
			if (status != GBN_STEP) return status;
			continue;
		}
		if (id == GBN_EVENT_IRQ) {
			if (d->io[IE] & d->io[IF]) d->halted = false;
			if (d->io[IME] && (d->io[IE] & d->io[IF]) && !(m->cpu.cpsr & 0x80)) {
				enum GbnStatus status = gbn_raise_irq(m);
				if (status != GBN_STEP) return status; /* Keep the request reviewable. */
			}
			gbn_cancel(m, GBN_EVENT_IRQ);
			continue;
		}
		if (d->video_running && !(d->io[DISPSTAT] & 2) && d->io[VCOUNT] < 160) {
			enum GbnStatus status = gbn_ppu_line(m, d->io[VCOUNT]);
			if (status != GBN_STEP) return status;
		}
		gbn_cancel(m, GBN_EVENT_VIDEO);
		if (!d->video_running) continue;
		if (!(d->io[DISPSTAT] & 2)) {
			d->io[DISPSTAT] |= 2;
			/* The reference LCD interrupt path becomes visible to the IRQ
			 * controller six cycles after the HBlank status transition. */
			if (d->io[DISPSTAT] & 0x10) gbn_irq_at(m, 2, when + 6);
			if (d->io[VCOUNT] < 160) gbn_dma_trigger(m, 2, when);
			if (d->io[VCOUNT] >= 2 && d->io[VCOUNT] < 162) gbn_dma_trigger(m, 3, when);
			gbn_schedule_at(m, GBN_EVENT_VIDEO, when + 224, 8);
		} else {
			d->io[DISPSTAT] &= ~2u;
			if (++d->io[VCOUNT] == 228) d->io[VCOUNT] = 0;
			if (!d->io[VCOUNT]) check_keys(m);
			vcount_match(m, when);
			if (d->io[VCOUNT] == 160) {
				d->io[DISPSTAT] |= 1;
				++d->frames;
				gbn_dma_trigger(m, 1, when);
				if (d->io[DISPSTAT] & 8) gbn_irq_at(m, 1, when);
			} else if (d->io[VCOUNT] == 227) d->io[DISPSTAT] &= ~1u;
			gbn_schedule_at(m, GBN_EVENT_VIDEO, when + 1008, 8);
		}
	}
}

static bool io_supported(uint32_t offset) {
	return offset < 0x60 || gbn_audio_register(offset) || gbn_sio_register(offset) || (offset >= 0xb0 && offset < 0xe0) ||
		(offset >= 0x100 && offset < 0x110) || offset == 0x130 || offset == 0x132 || offset == 0x134 ||
		offset == 0x200 || offset == 0x202 || offset == 0x204 || offset == 0x208 || offset == 0x300;
}

static uint16_t io_read(const struct Gbn* m, uint32_t offset) {
	if (offset == 0x300) return m->devices->postflag;
	if (offset == 0x128) return m->devices->sio.control;
	if (gbn_audio_register(offset)) return gbn_audio_reg_read(m, offset);
	if (offset >= 0x100 && offset < 0x110 && !(offset & 2))
		/* The reference load path observes the timer two cycles before its
		 * current CPU/DMA boundary. Inspection uses the actual current time. */
		return gbn_timer_read(m, (offset - 0x100) / 4, m->now - (m->cpu_access || m->devices->dma_access ? 2u : 0u));
	if (offset >= 0xb0 && offset < 0xe0) {
		unsigned part = (offset - 0xb0) % 12;
		if (part < 8) return (uint16_t) gbn_open_bus(m);
		if (part == 8) return 0;
	}
	if ((offset >= 0x10 && offset < 0x48) || offset == 0x4c || offset == 0x4e || (offset >= 0x54 && offset < 0x60)) {
		return (uint16_t) gbn_open_bus(m);
	}
	return offset == 0x204 ? m->waitcnt : m->devices->io[offset / 2];
}

static void io_write(struct Gbn* m, uint32_t offset, uint16_t value, uint16_t mask) {
	if (offset == 0x300) {
		if (m->code_region != 0) return; /* BIOS-only power control. */
		struct GbnDevices* d = m->devices;
		if ((mask & 0xff00) && d->postflag) {
			d->stopped = (value & 0x8000) != 0;
			d->halted = !d->stopped;
			gbn_update_irq(m, m->now);
		}
		if (mask & 0xff) d->postflag = (uint8_t) value;
		return;
	}
	if (offset >= 0x100 && offset < 0x110) {
		gbn_timer_write(m, (offset - 0x100) / 4, (offset & 2) != 0, value, mask);
		return;
	}
	uint16_t* io = m->devices->io;
	uint16_t old = io[offset / 2];
	if (offset == 0x202) { /* IF is write-one-to-clear, including byte writes. */
		io[IF] &= ~(value & mask);
		gbn_update_irq(m, m->now);
		return;
	}
	value = (old & ~mask) | (value & mask);
	if (offset >= 0xb0 && offset < 0xe0) {
		gbn_dma_write(m, offset, value);
		return;
	}
	if (offset >= 0x10 && offset <= 0x1e) value &= 0x1ff;
	if (offset == 0x2a || offset == 0x2e || offset == 0x3a || offset == 0x3e) value &= 0xfff;
	switch (offset) {
	case 0: value &= 0xfff7; break;
	case 2: value &= 1; break;
	case 4:
		io[DISPSTAT] = (old & 7) | (value & 0xff38);
		vcount_match(m, m->now);
		return;
	case 6: case 0x130: return; /* Read-only. */
	case 8: case 10: value &= 0xdfff; break;
	case 0x48: case 0x4a: value &= 0x3f3f; break;
	case 0x50: value &= 0x3fff; break;
	case 0x52: value &= 0x1f1f; break;
	case 0x54: value &= 0x1f; break;
	case 0x4e: case 0x56: case 0x58: case 0x5a: case 0x5c: case 0x5e: return;
	case 0x132: value &= 0xc3ff; break;
	case 0x200: value &= 0x3fff; break;
	case 0x204: value &= 0x5fff; gbn_set_waitcnt(m, value); break;
	case 0x208: value &= 1; break;
	default: break;
	}
	io[offset / 2] = value;
	if ((offset >= 0x28 && offset < 0x30) || (offset >= 0x38 && offset < 0x40)) gbn_ppu_reference(m, offset);
	if (offset == 0x200 || offset == 0x208) gbn_update_irq(m, m->now);
	if (offset == 0x132) check_keys(m);
}

/* VRAM mirrors its final 32 KiB into 0x18000..0x1ffff. Bitmap modes
 * suppress the first 16 KiB of that mirror. Byte writes only reach BG VRAM. */
static uint8_t* video_address(const struct GbnDevices* d, uint32_t address) {
	switch (address >> 24) {
	case 5: return d->palette + (address & (GBN_PALETTE_SIZE - 1));
	case 7: return d->oam + (address & (GBN_OAM_SIZE - 1));
	default: {
		uint32_t offset = address & 0x1ffff;
		if (offset >= 0x18000) {
			if (offset < 0x1c000 && (d->io[DISPCNT] & 7) >= 3) return NULL;
			offset -= 0x8000;
		}
		return d->vram + offset;
	}
	}
}

enum GbnStatus gbn_device_read(const struct Gbn* m, uint32_t address, unsigned width, struct GbnAccess* out) {
	if (!m->devices) return GBN_UNSUPPORTED_ADDRESS;
	unsigned region = address >> 24;
	uint32_t value = 0;
	if (region == 4) {
		uint32_t offset = address & 0xffffff;
		if (!io_supported(offset & ~1u) || (width == 4 && !io_supported(offset + 2))) return GBN_UNSUPPORTED_ADDRESS;
		value = io_read(m, offset & ~1u);
		if (width == 1) value = value >> ((offset & 1) * 8) & 255;
		else if (width == 4) value |= (uint32_t) io_read(m, offset + 2) << 16;
	} else {
		const uint8_t* p = video_address(m->devices, address);
		if (p) for (unsigned b = 0; b < width; ++b) value |= (uint32_t) p[b] << (8 * b);
	}
	out->value = value;
	out->cycles = width == 4 && (region == 5 || region == 6) ? 2 : 1;
	return GBN_STEP;
}

bool gbn_device_writable(const struct Gbn* m, uint32_t address, unsigned width) {
	if (!m->devices) return false;
	unsigned region = address >> 24;
	if (region >= 5 && region <= 7) return true;
	if (region != 4) return false;
	uint32_t offset = address & 0xffffff & ~(width - 1u);
	return io_supported(offset & ~1u) && (width != 4 || io_supported(offset + 2));
}

enum GbnStatus gbn_device_write(struct Gbn* m, uint32_t address, unsigned width, uint32_t value, uint32_t* cycles) {
	if (!gbn_device_writable(m, address, width)) return GBN_UNSUPPORTED_ADDRESS;
	unsigned region = address >> 24;
	*cycles = width == 4 && (region == 5 || region == 6) ? 2 : 1;
	if (region == 4) {
		uint32_t offset = address & 0xffffff;
		if (gbn_audio_register(offset & ~1u)) {
			gbn_audio_write(m, offset, width, value);
			return GBN_STEP;
		}
		if (gbn_sio_register(offset & ~1u)) return gbn_sio_write(m, offset, width, value);
		if (width == 1) io_write(m, offset & ~1u, (uint16_t) (value << ((offset & 1) * 8)), (uint16_t) (255 << ((offset & 1) * 8)));
		else {
			io_write(m, offset, (uint16_t) value, 0xffff);
			if (width == 4) io_write(m, offset + 2, (uint16_t) (value >> 16), 0xffff);
		}
		return GBN_STEP;
	}
	if (width == 1) {
		if (region == 7 || (region == 6 && (address & 0x1ffff) >= ((m->devices->io[DISPCNT] & 7) >= 3 ? 0x14000u : 0x10000u))) return GBN_STEP;
		/* The reference charges an extra cycle for its palette byte-to-halfword
		 * store path. Keep that CPU timing convention during differential bring-up. */
		if (region == 5) ++*cycles;
		value = (value & 255) * 0x101;
		width = 2; address &= ~1u;
	}
	uint8_t* p = video_address(m->devices, address);
	if (p) for (unsigned b = 0; b < width; ++b) p[b] = (uint8_t) (value >> (8 * b));
	return GBN_STEP;
}
