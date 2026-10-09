/* Frozen one-beat DMA oracle, before batching. Register writers/triggers omitted.
 * Original dma.c SHA256: 2070b4e7c4a5e4a478000d0c2cba5cb53a856dc92d317032bca7adcd41793769 */
/* SPDX-License-Identifier: MPL-2.0 */
#include "gba-next/internal.h"
#include <stddef.h>

static uint32_t dma_raw_read(const uint8_t* data, unsigned width) {
	uint32_t value = data[0];
	if (width >= 2) value |= (uint32_t) data[1] << 8;
	if (width == 4) value |= (uint32_t) data[2] << 16 | (uint32_t) data[3] << 24;
	return value;
}

static uint8_t* dma_video_pointer(const struct GbnDevices* d, uint32_t address) {
	uint32_t offset = address & 0x1ffff;
	if (offset >= 0x18000) {
		if (offset < 0x1c000 && (d->io[0] & 7) >= 3) return NULL;
		offset -= 0x8000;
	}
	switch (address >> 24) {
	case 5: return d->palette + (address & (GBN_PALETTE_SIZE - 1));
	case 6: return d->vram + offset;
	case 7: return d->oam + (address & (GBN_OAM_SIZE - 1));
	default: return NULL;
	}
}

/* DMA uses aligned halfword/word beats. Return a raw backing pointer only for
 * memory regions whose normal read/write path has no observable side effects.
 * IO, save devices and invalid cartridge holes continue through gbn_read/write
 * so open-bus and mapper behavior remain exact. */
static const uint8_t* dma_read_pointer(const struct Gbn* m, uint32_t address, unsigned width) {
	switch (address >> 24) {
	case 2: return m->ewram ? m->ewram + (address & (GBN_EWRAM_SIZE - 1) & ~(width - 1u)) : NULL;
	case 3: return m->iwram ? m->iwram + (address & (GBN_IWRAM_SIZE - 1) & ~(width - 1u)) : NULL;
	case 5: case 6: case 7: return dma_video_pointer(m->devices, address & ~(width - 1u));
	default:
		if ((address >> 24) >= 8 && (address >> 24) <= 13 && m->rom) {
			uint32_t offset = address & (GBN_ROM_MAX_SIZE - width);
			if (offset < m->rom_size && width <= m->rom_size - offset) return m->rom + offset;
		}
		return NULL;
	}
}

static uint8_t* dma_write_pointer(const struct Gbn* m, uint32_t address, unsigned width) {
	switch (address >> 24) {
	case 2: return m->ewram ? m->ewram + (address & (GBN_EWRAM_SIZE - 1) & ~(width - 1u)) : NULL;
	case 3: return m->iwram ? m->iwram + (address & (GBN_IWRAM_SIZE - 1) & ~(width - 1u)) : NULL;
	case 5: case 6: case 7: return dma_video_pointer(m->devices, address & ~(width - 1u));
	default: return NULL;
	}
}

/* One scheduled bus beat at a time. CPU time advances in the device dispatcher
 * while DMA holds the bus, so LCD/IRQ events can run between transfers. */
static void select_dma(struct Gbn* m) {
	struct GbnDevices* d = m->devices;
	int best = -1;
	for (unsigned i = 0; i < 4; ++i) {
		struct GbnDma* c = &d->dma[i];
		if (!(c->control & 0x8000) || (!c->remaining && !c->finishing)) continue;
		if (best < 0 || (int32_t) (c->when - d->dma[best].when) < 0) best = (int) i;
	}
	d->active_dma = best;
	gbn_cancel(m, GBN_EVENT_DMA);
	if (best < 0) d->dma_blocked = false;
	else gbn_schedule_at(m, GBN_EVENT_DMA, d->dma[best].when, 0x40);
}

static void reload_count(struct GbnDevices* d, unsigned channel) {
	uint32_t count = d->io[(0xb8 + channel * 12) / 2];
	if (channel < 3) count &= 0x3fff;
	d->dma[channel].count = count ? count : channel == 3 ? 0x10000 : 0x4000;
}

static unsigned wait_cycles(const struct Gbn* m, uint32_t address, unsigned width, bool sequential) {
	unsigned region = address >> 24;
	if (region == 2) return width == 4 ? 2 * m->ewram_wait + 1 : m->ewram_wait;
	if (region == 5 || region == 6) return width == 4 ? 1 : 0;
	if (region >= 8 && region < 14) {
		unsigned window = (region - 8) / 2;
		unsigned seq = m->rom_seq[window], first = sequential ? seq : m->rom_nonseq[window];
		return width == 4 ? first + seq + 1 : first;
	}
	if (region == 14 || region == 15) return gbn_save_wait(m, address);
	return 0;
}

static uint32_t advance(uint32_t address, unsigned control, unsigned width) {
	return control == 2 ? address : control == 1 ? address - width : address + width;
}

static enum GbnStatus span_ref_service(struct Gbn* m) {
	struct GbnDevices* d = m->devices;
	if (!d || d->active_dma < 0) return GBN_INVALID_ARGUMENT;
	unsigned channel = (unsigned) d->active_dma;
	struct GbnDma* c = &d->dma[channel];
	if (c->finishing) {
		uint32_t when = c->when;
		bool repeat = (c->control & 0x200) && (c->control & 0x3000);
		if (channel == 3 && (c->control & 0x3000) == 0x3000 && d->io[3] == 161) repeat = false;
		c->finishing = false;
		c->fifo = false;
		if (repeat) reload_count(d, channel);
		else {
			c->control &= ~0x8000u;
			d->io[(0xba + channel * 12) / 2] &= ~0x8000u;
		}
		if ((c->control & 0x60) == 0x60) c->next_dest = c->dest;
		if (c->control & 0x4000) gbn_irq_at(m, (uint16_t) (0x100u << channel), when);
		select_dma(m);
		return GBN_STEP;
	}
	if (c->control & 0x800) return GBN_UNSUPPORTED_DEVICE; /* External cartridge DRQ. */
	unsigned width = c->fifo || (c->control & 0x400) ? 4 : 2;
	uint32_t source = c->next_source, dest = c->next_dest;
	if (!gbn_bus_writable(m, dest, width)) return GBN_UNSUPPORTED_ADDRESS;
	if (c->remaining == (c->fifo ? 4 : c->count)) gbn_save_dma(m, dest, width, c->count);
	uint32_t value = c->latch;
	if (source >= 0x02000000) {
		struct GbnAccess access;
		const uint8_t* source_pointer = dma_read_pointer(m, source, width);
		if (source_pointer) value = dma_raw_read(source_pointer, width);
		else {
			d->dma_access = true;
			enum GbnStatus status = gbn_read(m, source, width, &access);
			d->dma_access = false;
			if (status != GBN_STEP) return status;
			value = access.value;
		}
		if (width == 2) value |= value << 16;
	}
	/* Commit only after this beat's source and destination have been checked.
	 * Earlier beats remain committed if a later access is not implemented. */
	bool first = c->remaining == (c->fifo ? 4 : c->count);
	uint32_t when = first ? m->now : c->when;
	unsigned elapsed = 2 + wait_cycles(m, source, width, !first) + wait_cycles(m, dest, width, !first);
	uint16_t control = c->control;
	uint32_t next_source = advance(source, source >= 0x08000000 && source < 0x0e000000 ? 0 : control >> 7 & 3, width);
	uint32_t next_dest = c->fifo ? dest : advance(dest, control >> 5 & 3, width);
	uint32_t ignored;
	uint8_t* dest_pointer = dma_write_pointer(m, dest, width);
	enum GbnStatus status;
	if (dest_pointer) {
		uint32_t stored = width == 2 ? value >> ((dest & 2) * 8) : value;
		for (unsigned i = 0; i < width; ++i) dest_pointer[i] = (uint8_t) (stored >> (8 * i));
		ignored = 0;
		status = GBN_STEP;
	} else {
		d->dma_access = true;
		status = gbn_write(m, dest, width, width == 2 ? value >> ((dest & 2) * 8) : value, &ignored);
		d->dma_access = false;
	}
	if (status != GBN_STEP) return status;
	c->latch = value;
	d->dma_bus = width == 2 ? (value & 0xffff) * 0x10001 : value;
	d->dma_bus_valid = true;
	d->dma_pc = m->cpu.pc + (m->cpu.cpsr & 0x20 ? 2 : 4);
	d->dma_blocked = true;
	c->next_source = next_source; c->next_dest = next_dest;
	c->when = when + elapsed;
	if (c->remaining) --c->remaining;
	/* A newly requested higher-priority channel takes over at the end of a
	 * beat, and resumes the interrupted channel at the same bus boundary. */
	for (unsigned i = 0; i < 4; ++i) {
		struct GbnDma* other = &d->dma[i];
		if ((other->control & 0x8000) && (other->remaining || other->finishing) && (int32_t) (other->when - c->when) < 0) other->when = c->when;
	}
	if (!c->remaining && (c->control & 0x8000)) {
		c->finishing = true;
		if ((source >> 24) < 8 || (dest >> 24) < 8) c->when += 2;
	}
	select_dma(m);
	return GBN_STEP;
}
