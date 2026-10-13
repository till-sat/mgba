/* SPDX-License-Identifier: MPL-2.0 */
#include "internal.h"
#include "ppu-internal.h" /* RVV helpers */
#include <stddef.h>
#if GBN_DMA_STATS
uint64_t gbn_dma_first_span_hits;
#endif

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

/* Select the next DMA bus boundary. Raw spans below can defer scheduling
 * unobserved beats, preserving CPU blocking and peripheral event ordering. */
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

void gbn_dma_write(struct Gbn* m, unsigned offset, uint16_t value) {
	struct GbnDevices* d = m->devices;
#if GBN_RV32_FAST & 64
	gbn_timer_catchup_now(m); /* a sound DMA change must follow earlier overflows */
#endif
	unsigned channel = (offset - 0xb0) / 12, part = (offset - 0xb0) % 12;
	struct GbnDma* c = &d->dma[channel];
	if (part < 8) {
		bool dest = part >= 4;
		uint32_t address = dest ? c->dest : c->source;
		unsigned shift = (part & 2) * 8;
		address = (address & ~(0xffffu << shift)) | ((uint32_t) value << shift);
		address &= (dest ? channel == 3 : channel != 0) ? 0x0ffffffe : 0x07fffffe;
		if (dest) c->dest = address;
		else c->source = address;
		unsigned base = (0xb0 + channel * 12 + (dest ? 4 : 0)) / 2;
		d->io[base] = (uint16_t) address; d->io[base + 1] = (uint16_t) (address >> 16);
		return;
	}
	if (part == 8) { d->io[offset / 2] = value; return; }
	bool was_enabled = (c->control & 0x8000) != 0;
	c->control = value & (channel == 3 ? 0xffe0 : 0xf7e0);
	d->io[offset / 2] = c->control;
#if GBN_RV32_FAST & 65536
	gbn_hblank_observe(m);
#endif
	if (!(c->control & 0x8000)) {
		c->remaining = 0; c->finishing = false; c->fifo = false;
		select_dma(m);
		return;
	}
	if (was_enabled) return;
	unsigned width = c->control & 0x400 ? 4 : 2;
	c->next_source = c->source & ~(width - 1u);
	c->next_dest = c->dest & ~(width - 1u);
	c->finishing = false;
	c->fifo = false;
	reload_count(d, channel);
	if (!(c->control & 0x3000)) {
		c->remaining = c->count;
		c->when = m->now + 3;
		select_dma(m);
	}
}

void gbn_dma_trigger(struct Gbn* m, unsigned timing, uint32_t when) {
	struct GbnDevices* d = m->devices;
	for (unsigned i = 0; i < 4; ++i) {
		/* Channels 1/2 custom requests come from audio FIFOs, not the LCD. */
		if (timing == 3 && i != 3) continue;
		struct GbnDma* c = &d->dma[i];
		if (!(c->control & 0x8000) || ((c->control >> 12) & 3) != timing) continue;
		c->when = when + 3;
		if (!c->remaining && !c->finishing) c->remaining = c->count;
	}
	select_dma(m);
}

#if GBN_RV32_FAST & 32
static void fifo_burst(struct Gbn*, unsigned channel, unsigned fifo, uint32_t when);
#endif
void gbn_dma_fifo(struct Gbn* m, unsigned fifo, uint32_t when) {
	struct GbnDevices* d = m->devices;
	for (unsigned i = 1; i <= 2; ++i) {
		struct GbnDma* c = &d->dma[i];
		if ((c->control & 0xb000) != 0xb000 || c->dest != 0x040000a0u + 4 * fifo) continue;
		if (c->remaining || c->finishing) continue;
#if GBN_RV32_FAST & 32
		fifo_burst(m, i, fifo, when);
		continue;
#endif
		c->fifo = true; c->remaining = 4; c->when = when;
		c->next_source &= ~3u;
		c->next_dest = c->dest;
		++d->audio.fifo[fifo].requests;
	}
	select_dma(m);
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

/* Bound a raw-memory run before any mirror, region or video-map transition.
 * A fixed address has no boundary; decrementing includes the current beat. */
static unsigned span_limit(const struct Gbn* m, uint32_t address, unsigned width, int step) {
	if (!step) return UINT32_MAX;
	unsigned region = address >> 24;
	uint32_t offset, size;
	switch (region) {
	case 2: size = GBN_EWRAM_SIZE; offset = address & (size - 1); break;
	case 3: size = GBN_IWRAM_SIZE; offset = address & (size - 1); break;
	case 5: case 7: size = GBN_PALETTE_SIZE; offset = address & (size - 1); break;
	case 6:
		offset = address & 0x1ffff;
		if (offset < 0x18000) size = 0x18000;
		else if (offset < 0x1c000) { offset -= 0x18000; size = 0x4000; }
		else { offset -= 0x1c000; size = 0x4000; }
		break;
	default:
		offset = address & (GBN_ROM_MAX_SIZE - 1);
		size = m->rom_size;
		if (size > GBN_ROM_MAX_SIZE) size = GBN_ROM_MAX_SIZE;
		break;
	}
	/* Memory-region changes also end runs in small mirrored RAM windows. */
	uint32_t region_offset = address & 0xffffff;
	unsigned shift = width == 4 ? 2 : 1; /* beats are halfwords or words */
	unsigned a = step > 0 ? (size - offset) >> shift : (offset >> shift) + 1;
	unsigned b = step > 0 ? (0x1000000u - region_offset) >> shift : (region_offset >> shift) + 1;
	return a < b ? a : b;
}

static enum GbnStatus dma_finish(struct Gbn* m,unsigned channel) {
    struct GbnDevices* d=m->devices;
    struct GbnDma* c=&d->dma[channel];
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

#if GBN_RV32_FAST & 32
/* Relaxed timing: a sound FIFO request moves its four words at once, without
 * DMA events, and the CPU loses the burst's bus time. */
static void fifo_burst(struct Gbn* m, unsigned channel, unsigned fifo, uint32_t when) {
	struct GbnDevices* d = m->devices;
	struct GbnDma* c = &d->dma[channel];
	struct GbnFifo* f = &d->audio.fifo[fifo];
	uint32_t source = c->next_source & ~3u, value = c->latch;
	unsigned sm = source >= 0x08000000 && source < 0x0e000000 ? 0 : c->control >> 7 & 3;
	unsigned cycles = 2;
	for (unsigned beat = 0; beat < 4; ++beat) {
		const uint8_t* p = dma_read_pointer(m, source, 4);
		if (p) value = dma_raw_read(p, 4);
		else if (source >= 0x02000000) {
			struct GbnAccess access;
			d->dma_access = true;
			if (gbn_read(m, source, 4, &access) == GBN_STEP) value = access.value;
			d->dma_access = false;
		}
		for (unsigned i = 0; i < 4 && f->size < 32; ++i)
			f->data[(f->read + f->size++) & 31] = (uint8_t) (value >> (8 * i));
		cycles += 2 + wait_cycles(m, source, 4, beat != 0) + wait_cycles(m, c->dest, 4, beat != 0);
		source = advance(source, sm, 4);
	}
	++f->requests;
	c->next_source = source; c->next_dest = c->dest;
	c->latch = value; d->dma_bus = value; d->dma_bus_valid = true;
	d->dma_pc = m->cpu.pc + (m->cpu.cpsr & 0x20 ? 2 : 4);
	if (!(c->control & 0x200)) {
		c->control &= ~0x8000u;
		d->io[(0xba + channel * 12) / 2] &= ~0x8000u;
	}
	if ((int32_t) (when + cycles - m->now) > 0) m->now = when + cycles;
	if (c->control & 0x4000) gbn_irq_at(m, (uint16_t) (0x100u << channel), m->now);
}
#endif

/* Fuse a lone DMA, including its exact late first beat, through raw aligned
 * memory, strictly before any other event. Every bus beat still reads then
 * writes in order, including overlapping/fixed/decrementing transfers. */
static __attribute__((noinline)) bool dma_span(struct Gbn* m, unsigned channel) {
#if defined(__GNUC__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	struct GbnDevices* d = m->devices;
	struct GbnDma* c = &d->dma[channel];
    if (c->finishing || !c->remaining || (c->control & 0x800)) return false;
    bool first = c->remaining == (c->fifo ? 4 : c->count);
    if (!first && !d->dma_blocked) return false;
	for (unsigned i = 0; i < 4; ++i) if (i != channel &&
	    (d->dma[i].control & 0x8000) && (d->dma[i].remaining || d->dma[i].finishing)) return false;
	unsigned width = c->fifo || (c->control & 0x400) ? 4 : 2;
	uint32_t source = c->next_source, dest = c->next_dest;
	if ((source | dest) & (width - 1)) return false;
	const uint8_t* src = dma_read_pointer(m, source, width);
	uint8_t* dst = dma_write_pointer(m, dest, width);
	if (!src || !dst || (((uintptr_t) src | (uintptr_t) dst) & (width - 1))) return false;
#if GBN_RV32_FAST & 2048
	gbn_ppu_before_write(m, dest);
#endif
	unsigned sm = source >= 0x08000000 && source < 0x0e000000 ? 0 : c->control >> 7 & 3;
	unsigned dm = c->fifo ? 2 : c->control >> 5 & 3;
	int ss = sm == 2 ? 0 : sm == 1 ? -(int) width : (int) width;
	int ds = dm == 2 ? 0 : dm == 1 ? -(int) width : (int) width;
	unsigned beats = c->remaining, limit = span_limit(m, source, width, ss);
	if (beats > limit) beats = limit;
	limit = span_limit(m, dest, width, ds);
	if (beats > limit) beats = limit;
    unsigned elapsed = 2 + wait_cycles(m, source, width, true) + wait_cycles(m, dest, width, true);
    unsigned first_elapsed = first ? 2 + wait_cycles(m, source, width, false)
        + wait_cycles(m, dest, width, false) : elapsed;
    uint32_t when = first ? m->now : c->when;
    unsigned other = m->event_head;
	if (other == GBN_EVENT_DMA) other = m->event_next[other];
#if GBN_RV32_FAST & 32
	/* Relaxed timing: the whole run at once; later events wait for it. */
	other = GBN_EVENT_COUNT;
#endif
	if (other < GBN_EVENT_COUNT) {
        int32_t distance = (int32_t) (m->events[other].when - when);
        if (distance <= 0 || (uint32_t) distance <= first_elapsed) return false;
        limit = ((uint32_t) distance - 1 - first_elapsed) / elapsed + 2;
		if (beats > limit) beats = limit;
	}
    if(beats<2) {
        /* A lone last beat is useful only when its release is also unobserved. */
        if(beats!=1 || c->remaining!=1 || (other<GBN_EVENT_COUNT &&
            (int32_t)(when+first_elapsed+2-m->events[other].when)>=0))return false;
    }
	uint32_t value = 0;
#if GBN_RV32_FAST & 32768
	/* VRAM pages count as written only when their bytes change. */
	uint32_t changed = 0;
	if ((dest >> 24) == 6 && d->ppu.cache) {
		if (width == 4) {
			for (unsigned i = 0; i < beats; ++i) {
				uint32_t old;
				__builtin_memcpy(&value, __builtin_assume_aligned(src + (ptrdiff_t) i * ss, 4), 4);
				__builtin_memcpy(&old, __builtin_assume_aligned(dst + (ptrdiff_t) i * ds, 4), 4);
				changed |= old ^ value;
				__builtin_memcpy(__builtin_assume_aligned(dst + (ptrdiff_t) i * ds, 4), &value, 4);
			}
		} else {
			uint16_t half = 0, old;
			for (unsigned i = 0; i < beats; ++i) {
				__builtin_memcpy(&half, __builtin_assume_aligned(src + (ptrdiff_t) i * ss, 2), 2);
				__builtin_memcpy(&old, __builtin_assume_aligned(dst + (ptrdiff_t) i * ds, 2), 2);
				changed |= (uint32_t) (old ^ half);
				__builtin_memcpy(__builtin_assume_aligned(dst + (ptrdiff_t) i * ds, 2), &half, 2);
			}
			value = (uint32_t) half * 0x10001u;
		}
	} else
#endif
	if (width == 4) {
		for (unsigned i = 0; i < beats; ++i) {
			__builtin_memcpy(&value, __builtin_assume_aligned(src + (ptrdiff_t) i * ss, 4), 4);
			__builtin_memcpy(__builtin_assume_aligned(dst + (ptrdiff_t) i * ds, 4), &value, 4);
		}
	} else {
		uint16_t half = 0;
		for (unsigned i = 0; i < beats; ++i) {
			__builtin_memcpy(&half, __builtin_assume_aligned(src + (ptrdiff_t) i * ss, 2), 2);
			__builtin_memcpy(__builtin_assume_aligned(dst + (ptrdiff_t) i * ds, 2), &half, 2);
		}
		value = (uint32_t) half * 0x10001u;
	}
    if ((dest >> 24) == 7) gbn_ppu_invalidate_oam(m);
#if GBN_RV32_FAST & 32768
    if ((dest >> 24) == 6 && changed) {
        uint32_t a = (uint32_t) (dst - d->vram), b = a + (uint32_t) ((int) (beats - 1) * ds);
        gbn_vram_written(d, a < b ? a : b, (a < b ? b : a) + width);
    }
#endif

    /* First beats begin at the observed CPU boundary, not the original
     * requested timestamp, and retain nonsequential source/destination waits.
     * Raw RAM/video destinations cannot notify the EEPROM DMA detector. */
    uint32_t last = beats>1 ? when + first_elapsed + (beats - 2) * elapsed : when;
    if ((int32_t) (last - m->now) > 0) m->now = last;
    c->next_source += beats * (uint32_t) ss; c->next_dest += beats * (uint32_t) ds;
    c->when = when + first_elapsed + (beats - 1) * elapsed; c->remaining -= beats;
    d->dma_blocked = true;
	c->latch = value; d->dma_bus = value; d->dma_bus_valid = true;
    d->dma_pc = m->cpu.pc + (m->cpu.cpsr & 0x20 ? 2 : 4);
#if GBN_DMA_STATS
    if(first)++gbn_dma_first_span_hits;
#endif
    if (!c->remaining && (c->control & 0x8000)) {
		c->finishing = true;
		/* Raw destinations are RAM/video; the bus release adds two cycles. */
		c->when += 2;
	}
	gbn_cancel(m, GBN_EVENT_DMA);
    gbn_schedule_at(m, GBN_EVENT_DMA, c->when, 0x40);
    /* The DMA event remains active while completing its normal finalizer,
     * including IRQ scheduling and historical queue ties. Only advance to
     * release when no other channel/event can observe that boundary first. */
    if(c->finishing && (other>=GBN_EVENT_COUNT ||
        (int32_t)(c->when-m->events[other].when)<0)) {
        if((int32_t)(c->when-m->now)>0)m->now=c->when;
        (void)dma_finish(m,channel);
    }
    return true;
#else
	(void) m; (void) channel;
	return false;
#endif
}

#if (GBN_RV32_FAST & 262144) && (GBN_RV32_FAST & 1048576)
/* The common immediate DMA in one step: no repeat or cartridge request, an
 * incrementing destination in RAM/video memory, an incrementing or fixed
 * source with a raw pointer, the only active channel. Same beats, time,
 * latch and finish as dma_span + dma_finish, without the event round trips. */
static bool dma_now_lean(struct Gbn* m) {
	struct GbnDevices* d = m->devices;
	int channel = d->active_dma;
	if (channel < 0) return false;
	struct GbnDma* c = &d->dma[channel];
	uint16_t control = c->control;
	if ((control & 0xba00) != 0x8000 || c->fifo || c->finishing || !c->remaining || c->remaining != c->count) return false;
	for (int i = 0; i < 4; ++i) if (i != channel &&
	    (d->dma[i].control & 0x8000) && (d->dma[i].remaining || d->dma[i].finishing)) return false;
	unsigned width = control & 0x400 ? 4 : 2;
	uint32_t source = c->next_source, dest = c->next_dest;
	unsigned sm = source >= 0x08000000 && source < 0x0e000000 ? 0 : control >> 7 & 3, dm = control >> 5 & 3;
	if (((source | dest) & (width - 1)) || (sm != 0 && sm != 2) || (dm != 0 && dm != 3)) return false;
	const uint8_t* src = dma_read_pointer(m, source, width);
	uint8_t* dst = dma_write_pointer(m, dest, width);
	if (!src || !dst || (((uintptr_t) src | (uintptr_t) dst) & (width - 1))) return false;
	int ss = sm == 2 ? 0 : (int) width;
	unsigned beats = c->remaining;
	if ((ss && beats > span_limit(m, source, width, ss)) || beats > span_limit(m, dest, width, (int) width)) return false;
#if GBN_RV32_FAST & 2048
	gbn_ppu_before_write(m, dest);
#endif
	uint32_t value = 0;
#if GBN_RV32_FAST & 32768
	bool video = (dest >> 24) == 6 && d->ppu.cache;
	uint32_t changed = 0;
#else
	const bool video = false;
	uint32_t changed = 0;
#endif
#if defined(AM_RVV) && (GBN_PPU_RVV & 128)
	/* Separate incrementing buffers: whole vector copies (no beat order to keep). */
	if (ss && (dst + beats * width <= src || src + beats * width <= dst)) {
		if (video) changed = gbn_rvv_copy_changed(dst, src, beats * width);
		else gbn_rvv_copy(dst, src, beats * width);
		if (width == 4) __builtin_memcpy(&value, __builtin_assume_aligned(src + 4 * (beats - 1), 4), 4);
		else { uint16_t half; __builtin_memcpy(&half, __builtin_assume_aligned(src + 2 * (beats - 1), 2), 2); value = (uint32_t) half * 0x10001u; }
	} else
#endif
	if (width == 4) {
		for (unsigned i = 0; i < beats; ++i) {
			uint32_t old;
			__builtin_memcpy(&value, __builtin_assume_aligned(src + (ptrdiff_t) i * ss, 4), 4);
			if (video) { __builtin_memcpy(&old, __builtin_assume_aligned(dst + 4 * i, 4), 4); changed |= old ^ value; }
			__builtin_memcpy(__builtin_assume_aligned(dst + 4 * i, 4), &value, 4);
		}
	} else {
		uint16_t half = 0, old;
		for (unsigned i = 0; i < beats; ++i) {
			__builtin_memcpy(&half, __builtin_assume_aligned(src + (ptrdiff_t) i * ss, 2), 2);
			if (video) { __builtin_memcpy(&old, __builtin_assume_aligned(dst + 2 * i, 2), 2); changed |= (uint32_t) (old ^ half); }
			__builtin_memcpy(__builtin_assume_aligned(dst + 2 * i, 2), &half, 2);
		}
		value = (uint32_t) half * 0x10001u;
	}
#if GBN_RV32_FAST & 32768
	if (changed) gbn_vram_written(d, (uint32_t) (dst - d->vram), (uint32_t) (dst - d->vram) + beats * width);
#else
	(void) changed;
#endif
	if ((dest >> 24) == 7) gbn_ppu_invalidate_oam(m);
	unsigned elapsed = 2 + wait_cycles(m, source, width, true) + wait_cycles(m, dest, width, true);
	unsigned first_elapsed = 2 + wait_cycles(m, source, width, false) + wait_cycles(m, dest, width, false);
	/* First beat at the observed boundary; raw destinations release in two. */
	m->now += first_elapsed + (beats - 1) * elapsed + 2;
	c->when = m->now;
	c->next_source = source + beats * (uint32_t) ss;
	c->next_dest = dm == 3 ? c->dest : dest + beats * width;
	c->remaining = 0; c->finishing = false; c->fifo = false;
	c->latch = value; d->dma_bus = value; d->dma_bus_valid = true;
	d->dma_pc = m->cpu.pc + (m->cpu.cpsr & 0x20 ? 2 : 4);
	c->control &= ~0x8000u;
	d->io[(0xba + channel * 12) / 2] &= ~0x8000u;
	if (control & 0x4000) gbn_irq_at(m, (uint16_t) (0x100u << channel), m->now);
	d->active_dma = -1;
	d->dma_blocked = false;
	gbn_cancel(m, GBN_EVENT_DMA);
	return true;
}
#endif

#if GBN_RV32_FAST & 262144
/* Native code just started an immediate DMA, whose event comes next: run it
 * to completion now. True when native code may go on: the DMA finished,
 * time is still before deadline and no event became due before it. */
__attribute__((used, noinline)) bool gbn_dma_now(struct Gbn* m, uint32_t deadline) {
	struct GbnDevices* d = m->devices;
	for (unsigned step = 0; step < 8; ++step) {
		if (m->next_event != (int) GBN_EVENT_DMA) return false;
		uint32_t when = m->events[GBN_EVENT_DMA].when;
		if ((int32_t) (m->now - when) < 0) m->now = when;
#if GBN_RV32_FAST & 1048576
		if (!step && dma_now_lean(m)) {
			int next = gbn_cpu_event(m);
			return (int32_t) (m->now - deadline) < 0 &&
				(next < 0 || (int32_t) (m->events[next].when - deadline) >= 0);
		}
#endif
		if (gbn_dma_service(m) != GBN_STEP) return false;
		if (!m->events[GBN_EVENT_DMA].active && d->active_dma < 0) {
			int next = gbn_cpu_event(m);
			return (int32_t) (m->now - deadline) < 0 &&
				(next < 0 || (int32_t) (m->events[next].when - deadline) >= 0);
		}
	}
	return false;
}
#endif

enum GbnStatus gbn_dma_service(struct Gbn* m) {
	struct GbnDevices* d = m->devices;
	if (!d || d->active_dma < 0) return GBN_INVALID_ARGUMENT;
	unsigned channel = (unsigned) d->active_dma;
	struct GbnDma* c = &d->dma[channel];
	/* Keep IO transfers and empty/releasing channels out of raw span setup.
	 * The span proves which first, sequential and release boundaries fit. */
    if (c->remaining && (c->next_dest >> 24) != 4 &&
        dma_span(m, channel)) return GBN_STEP;
	if (c->finishing) return dma_finish(m,channel);
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
#if GBN_RV32_FAST & 2048
	if (dest_pointer) gbn_ppu_before_write(m, dest);
#endif
	if (dest_pointer) {
		uint32_t stored = width == 2 ? value >> ((dest & 2) * 8) : value;
#if GBN_RV32_FAST & 32768
		bool changed = false;
		for (unsigned i = 0; i < width; ++i) changed |= dest_pointer[i] != (uint8_t) (stored >> (8 * i));
		if ((dest >> 24) == 6 && changed) gbn_vram_written(d, (uint32_t) (dest_pointer - d->vram), (uint32_t) (dest_pointer - d->vram) + width);
#endif
		for (unsigned i = 0; i < width; ++i) dest_pointer[i] = (uint8_t) (stored >> (8 * i));
        if ((dest >> 24) == 7) gbn_ppu_invalidate_oam(m);
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
