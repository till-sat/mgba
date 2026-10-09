/* SPDX-License-Identifier: MPL-2.0 */
#include <gba-next/core.h>
#include <gba-next/rv32.h>
#include "internal.h"
#include <stddef.h>
#include <string.h>

#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u

static uint32_t ror(uint32_t x, unsigned n) {
	n &= 31;
	return n ? (x >> n) | (x << (32 - n)) : x;
}

static uint32_t asr(uint32_t x, unsigned n) {
	if (!n) return x;
	uint32_t sign = 0u - (x >> 31);
	return n >= 32 ? sign : (x >> n) | (sign << (32 - n));
}

static uint32_t raw_read(const uint8_t* p, unsigned width) {
	uint32_t v = p[0];
	if (width >= 2) v |= (uint32_t) p[1] << 8;
	if (width == 4) v |= (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
	return v;
}

static bool ram(const struct Gbn* m, uint32_t address, uint8_t** base, uint32_t* mask, unsigned* wait) {
	switch (address >> 24) {
	case 2: *base = m->ewram; *mask = GBN_EWRAM_SIZE - 1; *wait = m->ewram_wait; break;
	case 3: *base = m->iwram; *mask = GBN_IWRAM_SIZE - 1; *wait = 0; break;
	default: return false;
	}
	return *base != NULL;
}

static bool cartridge(uint32_t address) { return (address >> 24) >= 8 && (address >> 24) <= 13; }

static unsigned rom_wait(const struct Gbn* m, uint32_t address, unsigned width, bool sequential) {
	unsigned window = ((address >> 24) - 8) / 2;
	unsigned n = m->rom_nonseq[window], s = m->rom_seq[window];
	return width == 4 ? (sequential ? s : n) + s + 1 : sequential ? s : n;
}

static uint32_t rom_read(const struct Gbn* m, uint32_t address, unsigned width) {
	uint32_t offset = address & (GBN_ROM_MAX_SIZE - width);
	if (offset < m->rom_size && width <= m->rom_size - offset) return raw_read(m->rom + offset, width);
	/* Unpopulated cartridge addresses expose the halfword address lines. */
	if (width == 1) return (address >> (1 + (address & 1) * 8)) & 255;
	uint32_t value = (address >> 1) & 0xffff;
	if (width == 4) value |= ((address + 2) >> 1) << 16;
	return value;
}

static bool executable(const struct Gbn* m, uint32_t address) {
	uint8_t* base; uint32_t mask; unsigned wait;
	if (ram(m, address, &base, &mask, &wait)) return true;
	if ((address >> 24) == 0) return m->bios != NULL;
	return cartridge(address) && m->rom && (address & (GBN_ROM_MAX_SIZE - 1)) < m->rom_size;
}

static uint32_t fetch(const struct Gbn* m, uint32_t address, unsigned width) {
	uint32_t offset = address & m->code_mask & ~(width - 1u);
	if (m->code_region >= 8) return rom_read(m, offset, width);
	return raw_read(m->code + offset, width);
}

uint32_t gbn_open_bus(const struct Gbn* m) {
	unsigned width = m->cpu.cpsr & 0x20 ? 2 : 4;
	uint32_t visible_pc = m->cpu.pc + (m->cpu_access ? 2 : 1) * width;
	if (m->devices && (m->devices->dma_access || (m->devices->dma_bus_valid && visible_pc - m->devices->dma_pc == width))) return m->devices->dma_bus;
	uint32_t latest = m->cpu_access ? fetch(m, m->cpu.pc + 2 * width, width) : m->cpu.pipe[1];
	if (width == 4) return latest;
	uint32_t previous = m->cpu.pipe[m->cpu_access ? 1 : 0];
	uint32_t pc = m->cpu.pc + (m->cpu_access ? 2 : 1) * width;
	if (m->code_region == 0 || m->code_region == 7) return latest << 16 | previous;
	if (m->code_region == 3) return pc & 2 ? latest << 16 | previous : previous << 16 | latest;
	return latest << 16 | latest;
}

static void code_waits(struct Gbn* m) {
	if (m->code_region >= 8 && m->code_region <= 13) {
		uint32_t address = (uint32_t) m->code_region << 24;
		m->code_wait = (uint8_t) rom_wait(m, address, 2, true);
		m->code_word_wait = (uint8_t) rom_wait(m, address, 4, true);
		m->code_nonseq = (uint8_t) rom_wait(m, address, 2, false);
		m->code_word_nonseq = (uint8_t) rom_wait(m, address, 4, false);
	} else {
		m->code_wait = m->code_region == 2 ? m->ewram_wait : 0;
		m->code_word_wait = m->code_region == 2 ? (uint8_t) (2 * m->ewram_wait + 1) : 0;
		m->code_nonseq = m->code_wait;
		m->code_word_nonseq = m->code_word_wait;
	}
}

void gbn_set_waitcnt(struct Gbn* m, uint16_t value) {
	static const uint8_t waits[] = {4, 3, 2, 8};
	m->waitcnt = value & 0x7fff;
	if (m->devices) m->devices->io[0x204 / 2] = m->waitcnt;
	m->rom_nonseq[0] = waits[value >> 2 & 3];
	m->rom_nonseq[1] = waits[value >> 5 & 3];
	m->rom_nonseq[2] = waits[value >> 8 & 3];
	m->rom_seq[0] = value & 0x10 ? 1 : 2;
	m->rom_seq[1] = value & 0x80 ? 1 : 4;
	m->rom_seq[2] = value & 0x400 ? 1 : 8;
	code_waits(m);
}

bool gbn_attach_rom(struct Gbn* m, const uint8_t* image, uint32_t size) {
	if (!image || !size || size > GBN_ROM_MAX_SIZE) return false;
	uint32_t span = 1;
	while (span < size) span <<= 1;
	m->rom = image; m->rom_size = size; m->rom_mask = span - 1;
	++m->rom_generation;
	m->code = NULL; m->code_region = 255;
	return true;
}

bool gbn_attach_bios(struct Gbn* m, const uint8_t* image, uint32_t size) {
	if (!image || size != GBN_BIOS_SIZE) return false;
	m->bios = image;
	m->builtin_bios = false;
	m->code = NULL; m->code_region = 255;
	return true;
}

enum GbnStatus gbn_read(const struct Gbn* m, uint32_t address, unsigned width, struct GbnAccess* out) {
	if (width != 1 && width != 2 && width != 4) return GBN_INVALID_ARGUMENT;
	uint8_t* base;
	uint32_t mask;
	unsigned wait;
	uint32_t value;
	if (gbn_save_mapped(m, address, width)) {
		out->value = gbn_save_read(m, address, width);
		if (width != 1) out->value = ror(out->value, (address & (width - 1)) * 8);
		out->cycles = 1 + gbn_save_wait(m, address);
		return GBN_STEP;
	}
	if ((address >> 24) >= 4 && (address >> 24) <= 7) {
		enum GbnStatus status = gbn_device_read(m, address & ~(width - 1u), width, out);
		if (status != GBN_STEP) return status;
		value = out->value;
	} else if (cartridge(address) && m->rom) {
		value = rom_read(m, address & ~(width - 1u), width);
		out->cycles = 1 + rom_wait(m, address, width, false);
	} else if (address < GBN_BIOS_SIZE && m->bios) {
		uint32_t offset = address & ~(width - 1u);
		value = m->code_region == 0 ? raw_read(m->bios + offset, width) :
			m->bios_latch >> ((offset & 3) * 8);
		if (width == 1) value &= 255;
		if (width == 2) value &= 65535;
		out->cycles = 1;
	} else {
		if (!ram(m, address, &base, &mask, &wait)) return GBN_UNSUPPORTED_ADDRESS;
		value = raw_read(base + (address & mask & ~(width - 1u)), width);
		out->cycles = (wait + 1) * ((address >> 24) == 2 && width == 4 ? 2 : 1);
	}
	if (width != 1) value = ror(value, (address & (width - 1)) * 8);
	out->value = value;
	return GBN_STEP;
}

enum GbnStatus gbn_write(struct Gbn* m, uint32_t address, unsigned width, uint32_t value, uint32_t* cycles) {
	if (width != 1 && width != 2 && width != 4) return GBN_INVALID_ARGUMENT;
	if (gbn_save_mapped(m, address, width)) {
		gbn_save_write(m, address, width, value);
		*cycles = 1 + gbn_save_wait(m, address);
		return GBN_STEP;
	}
	if ((address >> 24) >= 4 && (address >> 24) <= 7) return gbn_device_write(m, address & ~(width - 1u), width, value, cycles);
	uint8_t* base;
	uint32_t mask;
	unsigned wait;
	if (!ram(m, address, &base, &mask, &wait)) return GBN_UNSUPPORTED_ADDRESS;
	uint8_t* p = base + (address & mask & ~(width - 1u));
	for (unsigned i = 0; i < width; ++i) p[i] = (uint8_t) (value >> (8 * i));
	*cycles = (wait + 1) * ((address >> 24) == 2 && width == 4 ? 2 : 1);
	return GBN_STEP;
}

bool gbn_bus_writable(const struct Gbn* m, uint32_t address, unsigned width) {
	uint8_t* base; uint32_t mask; unsigned wait;
	return ram(m, address, &base, &mask, &wait) || gbn_device_writable(m, address, width) || gbn_save_mapped(m, address, width);
}

void gbn_init(struct Gbn* m, uint8_t* ewram, uint8_t* iwram) {
	memset(m, 0, sizeof(*m));
	m->ewram = ewram;
	m->iwram = iwram;
	m->ewram_wait = 2;
	m->code_region = 255;
	gbn_set_waitcnt(m, 0);
	m->cpu.cpsr = 0x3f; /* System mode, Thumb. Full reset is a later stage. */
	m->next_event = -1;
	m->event_head = GBN_EVENT_COUNT;
	memset(m->event_prev, GBN_EVENT_COUNT, sizeof(m->event_prev));
	memset(m->event_next, GBN_EVENT_COUNT, sizeof(m->event_next));
}

static enum GbnStatus enter_code(struct Gbn* m, uint32_t pc, bool thumb) {
	if (!executable(m, pc)) return GBN_UNSUPPORTED_ADDRESS;
	const uint8_t* base;
	uint32_t mask;
	unsigned region = pc >> 24;
	if (region == 0) { base = m->bios; mask = GBN_BIOS_SIZE - 1; }
	else if (region == 2) { base = m->ewram; mask = GBN_EWRAM_SIZE - 1; }
	else if (region == 3) { base = m->iwram; mask = GBN_IWRAM_SIZE - 1; }
	else { base = m->rom; mask = m->rom_mask; }
	unsigned width = thumb ? 2 : 4;
	/* ARM7 retains address bit 1 in PC while the ARM fetch bus is word aligned. */
	pc &= ~1u;
	if (m->code_region == 0 && region != 0) m->bios_latch = m->cpu.pipe[1];
	m->prefetched_pc = 0;
	m->cpu.pc = pc;
	m->cpu.cpsr = (m->cpu.cpsr & ~0x20u) | (thumb ? 0x20 : 0);
	mask &= ~(width - 1u);
	m->code = base;
	m->code_mask = mask;
	m->code_region = (uint8_t) region;
	code_waits(m);
	m->cpu.pipe[0] = fetch(m, pc, width);
	m->cpu.pipe[1] = fetch(m, pc + width, width);
	return GBN_STEP;
}

enum GbnStatus gbn_enter_thumb(struct Gbn* m, uint32_t pc) { return enter_code(m, pc, true); }
enum GbnStatus gbn_enter_arm(struct Gbn* m, uint32_t pc) { return enter_code(m, pc, false); }

static bool event_before(const struct GbnEvent* a, const struct GbnEvent* b) {
	return (int32_t) (a->when - b->when) < 0 || (a->when == b->when && a->priority < b->priority);
}

/* Canonical list order resolves equal keys by event ID. next_event is a
 * separate cached winner: equal non-head updates retain its historical winner. */
static bool event_key_before(const struct GbnEvent* a, unsigned aid, const struct GbnEvent* b, unsigned bid) {
	return event_before(a, b) || (a->when == b->when && a->priority == b->priority && aid < bid);
}

static void event_remove(struct Gbn* m, unsigned id) {
	unsigned prev = m->event_prev[id], next = m->event_next[id];
	if (prev < GBN_EVENT_COUNT) m->event_next[prev] = (uint8_t) next;
	else m->event_head = (uint8_t) next;
	if (next < GBN_EVENT_COUNT) m->event_prev[next] = (uint8_t) prev;
	m->event_prev[id] = m->event_next[id] = GBN_EVENT_COUNT;
}

static void event_insert(struct Gbn* m, unsigned id) {
	const struct GbnEvent* value = &m->events[id];
	unsigned prev = GBN_EVENT_COUNT, next = m->event_head;
	while (next < GBN_EVENT_COUNT) {
		const struct GbnEvent* other = &m->events[next];
		if (event_key_before(value, id, other, next)) break;
		prev = next; next = m->event_next[next];
	}
	m->event_prev[id] = (uint8_t) prev; m->event_next[id] = (uint8_t) next;
	if (prev < GBN_EVENT_COUNT) m->event_next[prev] = (uint8_t) id;
	else m->event_head = (uint8_t) id;
	if (next < GBN_EVENT_COUNT) m->event_prev[next] = (uint8_t) id;
}

static bool event_due(const struct Gbn* m) {
	return m->next_event >= 0 && (int32_t) (m->now - m->events[m->next_event].when) >= 0;
}

bool gbn_schedule(struct Gbn* m, unsigned id, uint32_t delay, uint8_t priority) {
	if (id >= GBN_EVENT_COUNT || delay > GBN_MAX_DELAY) return false;
	return gbn_schedule_at(m, id, m->now + delay, priority);
}

bool gbn_schedule_at(struct Gbn* m, unsigned id, uint32_t when, uint8_t priority) {
	int32_t distance = (int32_t) (when - m->now);
	if (id >= GBN_EVENT_COUNT || distance > (int32_t) GBN_MAX_DELAY || distance < -(int32_t) GBN_MAX_DELAY) return false;
	int old = m->next_event;
	bool was_next = old == (int) id;
	bool linked = m->events[id].active;
	struct GbnEvent value = { when, priority, true };
	/* An empty public event state starts a fresh list. Unreachable links
	 * need no clearing; insertion initializes both links of each new node. */
	if (old < 0) { m->event_head = GBN_EVENT_COUNT; linked = false; }
	if (linked && !was_next) {
		unsigned prev = m->event_prev[id], next = m->event_next[id];
		if ((prev == GBN_EVENT_COUNT || event_key_before(&m->events[prev], prev, &value, id)) &&
		    (next == GBN_EVENT_COUNT || event_key_before(&value, id, &m->events[next], next))) {
			/* Preserve the position for updates still between their neighbors. */
			m->events[id] = value;
			goto select_cached;
		}
	}
	if (linked) event_remove(m, id);
	m->events[id] = value;
	event_insert(m, id);
select_cached:
	if (old < 0 || was_next) m->next_event = (int) m->event_head;
	else if (event_before(&m->events[id], &m->events[old])) m->next_event = (int) id;
	return true;
}

void gbn_cancel(struct Gbn* m, unsigned id) {
	if (id >= GBN_EVENT_COUNT) return;
	bool was_next = m->next_event == (int) id;
	if (m->events[id].active) event_remove(m, id);
	m->events[id].active = false;
	if (was_next) m->next_event = m->event_head < GBN_EVENT_COUNT ? (int) m->event_head : -1;
}

int gbn_take_event(struct Gbn* m, uint32_t* lateness) {
	if (!event_due(m)) return -1;
	int id = m->next_event;
	if (lateness) *lateness = m->now - m->events[id].when;
	gbn_cancel(m, (unsigned) id);
	return id;
}

static void nz(struct GbnCpu* c, uint32_t value) {
	c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z)) | (value & FLAG_N) | (value ? 0 : FLAG_Z);
}

static void carry(struct GbnCpu* c, unsigned value) {
	c->cpsr = (c->cpsr & ~FLAG_C) | (value ? FLAG_C : 0);
}

static uint32_t add(struct GbnCpu* c, uint32_t a, uint32_t b, unsigned cin) {
	uint32_t sum = a + b, out = sum + cin;
	unsigned cout = sum < a || out < sum;
	c->cpsr = (c->cpsr & 0x00ffffffu) | (cout ? FLAG_C : 0) |
	          ((~(a ^ b) & (a ^ out)) >> 3 & FLAG_V);
	nz(c, out);
	return out;
}

static uint32_t sub(struct GbnCpu* c, uint32_t a, uint32_t b, unsigned borrow, bool sbc) {
	uint32_t out = a - b - borrow;
	unsigned cout = a >= b && (a != b || !borrow);
	/* SBC preserves reserved PSR bits just as the reference ARM7 path does. */
	c->cpsr = (c->cpsr & (sbc ? 0x0fffffffu : 0x00ffffffu)) | (cout ? FLAG_C : 0) |
	          (((a ^ b) & (a ^ out)) >> 3 & FLAG_V);
	nz(c, out);
	return out;
}

static uint32_t shift(struct GbnCpu* c, uint32_t value, unsigned amount, unsigned kind) {
	if (!amount) return value;
	if (kind == 0) {
		carry(c, amount <= 32 ? (value >> (32 - amount)) & 1 : 0);
		return amount < 32 ? value << amount : 0;
	}
	if (kind == 1) {
		carry(c, amount <= 32 ? (value >> (amount - 1)) & 1 : 0);
		return amount < 32 ? value >> amount : 0;
	}
	if (kind == 2) {
		carry(c, (value >> (amount < 32 ? amount - 1 : 31)) & 1);
		return asr(value, amount);
	}
	uint32_t out = ror(value, amount);
	carry(c, out >> 31);
	return out;
}

static bool condition(uint32_t flags, unsigned cond) {
	if (cond >= 14) return cond == 14;
	bool n = (flags & FLAG_N) != 0, z = (flags & FLAG_Z) != 0;
	bool c = (flags & FLAG_C) != 0, v = (flags & FLAG_V) != 0;
	bool value;
	switch (cond >> 1) {
	case 0: value = z; break;
	case 1: value = c; break;
	case 2: value = n; break;
	case 3: value = v; break;
	case 4: value = c && !z; break;
	case 5: value = n == v; break;
	default: value = !z && n == v; break;
	}
	return value != (bool) (cond & 1);
}

static uint32_t reg(const struct GbnCpu* c, unsigned r) {
	return r == 15 ? c->pc + (c->cpsr & 0x20 ? 4 : 8) : c->r[r];
}

/* A branch validates its destination before changing any guest state. */
static enum GbnStatus branch_mode(struct Gbn* m, uint32_t target, bool thumb, uint32_t* cycles, bool* jumped) {
	enum GbnStatus status = enter_code(m, target, thumb);
	if (status != GBN_STEP) return status;
	*cycles += 2u + (thumb ? m->code_wait + m->code_nonseq : m->code_word_wait + m->code_word_nonseq);
	*jumped = true;
	return GBN_STEP;
}

static enum GbnStatus branch(struct Gbn* m, uint32_t target, uint32_t* cycles, bool* jumped) {
	return branch_mode(m, target, (m->cpu.cpsr & 0x20) != 0, cycles, jumped);
}

static unsigned bits(uint32_t mask) {
	unsigned count = 0;
	while (mask) { mask &= mask - 1; ++count; }
	return count;
}

static int bank(unsigned mode) {
	switch (mode & 31) {
	case 16: case 31: return 0;
	case 17: return 1;
	case 18: return 2;
	case 19: return 3;
	case 23: return 4;
	case 27: return 5;
	default: return -1;
	}
}

static void set_cpsr(struct GbnCpu* c, uint32_t value) {
	unsigned old = (unsigned) bank(c->cpsr), next = (unsigned) bank(value);
	if (old != next) {
		if (old == 1 || next == 1) {
			for (unsigned r = 0; r < 5; ++r) {
				c->high_banks[old == 1][r] = c->r[r + 8];
				c->r[r + 8] = c->high_banks[next == 1][r];
			}
		}
		c->banks[old].sp = c->r[13];
		c->banks[old].lr = c->r[14];
		c->banks[old].spsr = c->spsr;
		c->r[13] = c->banks[next].sp;
		c->r[14] = c->banks[next].lr;
		c->spsr = c->banks[next].spsr;
	}
	c->cpsr = value;
}

static enum GbnStatus exception(struct Gbn* m, uint32_t vector, unsigned mode, uint32_t link, uint32_t* cycles) {
	if (!m->bios) return GBN_UNSUPPORTED_INSTRUCTION;
	uint32_t saved = m->cpu.cpsr;
	set_cpsr(&m->cpu, (saved & ~0x3fu) | mode | 0x80);
	m->cpu.spsr = saved;
	m->cpu.r[14] = link;
	enter_code(m, vector, false);
	*cycles += 2;
	return GBN_STEP;
}

enum GbnStatus gbn_raise_irq(struct Gbn* m) {
	if (bank(m->cpu.cpsr) < 0) return GBN_UNSUPPORTED_MODE;
	if (m->cpu.cpsr & 0x80) return GBN_STEP;
	uint32_t cycles = 0;
	enum GbnStatus status = exception(m, 0x18, 0x12, m->cpu.pc + 4, &cycles);
	if (status == GBN_STEP) m->now += cycles;
	return status;
}

/* Prefetch overlap is accounted separately from raw bus access. Keeping this
 * state outside gbn_read lets debugger/loader reads avoid changing CPU time. */
static int32_t cpu_stall(struct Gbn* m, uint32_t wait) {
	if (m->code_region < 8 || m->code_region > 13 || !(m->waitcnt & 0x4000)) return (int32_t) wait;
	uint32_t pc = m->cpu.pc + (m->cpu.cpsr & 0x20 ? 4 : 8);
	uint32_t distance = m->prefetched_pc - pc;
	unsigned previous = distance < 16 ? distance >> 1 : 0;
	unsigned seq = m->code_wait;
	unsigned covered = seq + 1;
	unsigned extra = wait > covered ? (wait - covered + seq - 1) / seq : 0;
	if (extra > 7 - previous) extra = 7 - previous;
	covered += extra * seq;
	m->prefetched_pc = pc + 2 * (previous + extra);
	return (int32_t) (wait > covered ? wait - covered : 0) - ((int32_t) m->code_nonseq - m->code_wait);
}

static int32_t fetch_break(const struct Gbn* m, bool thumb) {
	return thumb ? (int32_t) m->code_nonseq - m->code_wait : (int32_t) m->code_word_nonseq - m->code_word_wait;
}

static uint32_t store_stall(struct Gbn* m, uint32_t address, unsigned width, uint32_t elapsed) {
	if (address >= 0x08000000) return elapsed;
	if ((address >> 24) == 5 && width == 1) {
		/* The oracle's palette byte expansion has two separate bus phases. */
		uint32_t first = (uint32_t) cpu_stall(m, 1);
		return first + (uint32_t) cpu_stall(m, 1);
	}
	return (uint32_t) cpu_stall(m, elapsed);
}

static uint32_t user_reg(const struct GbnCpu* c, unsigned r) {
	if (r >= 8 && r < 13 && bank(c->cpsr) == 1) return c->high_banks[0][r - 8];
	if (r == 13 && bank(c->cpsr)) return c->banks[0].sp;
	if (r == 14 && bank(c->cpsr)) return c->banks[0].lr;
	return reg(c, r);
}

static void set_user_reg(struct GbnCpu* c, unsigned r, uint32_t value) {
	if (r >= 8 && r < 13 && bank(c->cpsr) == 1) c->high_banks[0][r - 8] = value;
	else if (r == 13 && bank(c->cpsr)) c->banks[0].sp = value;
	else if (r == 14 && bank(c->cpsr)) c->banks[0].lr = value;
	else c->r[r] = value;
}

/* RAM block transfers share one mapping and align each word without the
 * single-load rotation. Validate a loaded PC before committing any registers.
 * Empty-list behaviour follows the reference ARM7 implementation. */
static enum GbnStatus multiple(struct Gbn* m, unsigned rn, uint32_t list, bool load,
                              bool up, bool before, bool writeback, bool refill_pc,
                              bool user, bool restore,
                              uint32_t* cycles, bool* jumped) {
	struct GbnCpu* c = &m->cpu;
	unsigned count = bits(list);
	uint32_t base_address = reg(c, rn);
	uint32_t address = up ? base_address + (before ? 4 : 0) : base_address - count * 4 + (before ? 0 : 4);
	uint8_t* base = NULL;
	uint32_t mask = 0;
	unsigned wait = 0;
	bool plain_ram = ram(m, address, &base, &mask, &wait);
	unsigned region = address >> 24;
	bool device = region >= 4 && region <= 7 && m->devices;
	if (!plain_ram && !device && !(load && ((cartridge(address) && m->rom) || (region == 0 && m->bios)))) return GBN_UNSUPPORTED_ADDRESS;
	unsigned bus = plain_ram ? (region == 2 ? 2 * (wait + 1) : 1) : region < 8 ? (region == 5 || region == 6 ? 2 : 1) : 1 + rom_wait(m, address, 4, true);
	int32_t initial_wait = !plain_ram && region >= 8 ? (int32_t) rom_wait(m, address, 4, true) - (int32_t) rom_wait(m, address, 4, false) : 0;
	bool was_thumb = (c->cpsr & 0x20) != 0;
	address &= ~3u;
	uint32_t transfer_list = list ? list : 0x8000;
	uint32_t target = 0;
	uint32_t restored_psr = c->spsr;
	if (restore && bank(restored_psr) < 0) return GBN_UNSUPPORTED_MODE;
	uint32_t values[16];
	if (load) {
		uint32_t at = address;
		for (unsigned r = 0; r < 16; ++r) {
			if (!(transfer_list & (1u << r))) continue;
			if (plain_ram) values[r] = raw_read(base + (at & mask), 4);
			else {
				struct GbnAccess access;
				enum GbnStatus status = gbn_read(m, at, 4, &access);
				if (status != GBN_STEP) return status;
				values[r] = access.value;
			}
			at += 4;
		}
	}
	if (load && (transfer_list & 0x8000)) {
		target = values[15];
		if (refill_pc && !executable(m, target)) return GBN_UNSUPPORTED_ADDRESS;
	}
	if (!load && device) {
		uint32_t at = address;
		for (unsigned r = 0; r < 16; ++r) if (transfer_list & (1u << r)) {
			if (!gbn_device_writable(m, at, 4)) return GBN_UNSUPPORTED_ADDRESS;
			at += 4;
		}
	}
	for (unsigned r = 0; r < 16; ++r) {
		if (!(transfer_list & (1u << r))) continue;
		if (load) {
			if (r != 15) {
				if (user) set_user_reg(c, r, values[r]);
				else c->r[r] = values[r];
			}
		} else {
			uint32_t value = r == 15 ? reg(c, 15) + (list ? 4 : c->cpsr & 0x20 ? 2 : 4) : user ? user_reg(c, r) : c->r[r];
			if (plain_ram) {
				uint8_t* p = base + (address & mask);
				for (unsigned b = 0; b < 4; ++b) p[b] = (uint8_t) (value >> (8 * b));
			} else {
				uint32_t ignored;
				gbn_write(m, address, 4, value, &ignored); /* Entire transfer validated above. */
			}
		}
		address += 4;
	}
	if (writeback && (!load || !(list & (1u << rn)))) {
		uint32_t updated = list ? base_address + (up ? count * 4 : 0u - count * 4) : base_address + 64;
		if (rn != 15) {
			if (user) set_user_reg(c, rn, updated);
			else c->r[rn] = updated;
		}
	}
	uint32_t elapsed = (uint32_t) initial_wait + (count ? count * bus : bus + 15) + (load ? 1 : 0);
	*cycles += region < 8 ? (uint32_t) cpu_stall(m, elapsed) : elapsed;
	*cycles += (uint32_t) fetch_break(m, was_thumb);
	if (restore) set_cpsr(c, restored_psr);
	if (load && (transfer_list & 0x8000)) {
		if (refill_pc) return branch(m, target, cycles, jumped);
		/* Empty Thumb POP keeps the old fetch mapping/pipeline in the oracle. */
		c->pc = target - 4;
	}
	return GBN_STEP;
}

static enum GbnStatus memory(struct Gbn* m, uint32_t address, unsigned rd, unsigned kind, uint32_t* cycles) {
	/* Kinds match the eight Thumb register-offset load/store operations. */
	static const uint8_t widths[8] = {4, 2, 1, 1, 4, 2, 1, 2};
	unsigned width = widths[kind];
	uint32_t elapsed;
	if (kind <= 2) {
		enum GbnStatus status = gbn_write(m, address, width, m->cpu.r[rd], &elapsed);
		if (status != GBN_STEP) return status;
	} else {
		struct GbnAccess access;
		enum GbnStatus status = gbn_read(m, address, width, &access);
		if (status != GBN_STEP) return status;
		uint32_t value = access.value;
		if (kind == 3 || (kind == 7 && (address & 1))) value = asr(value << 24, 24);
		else if (kind == 7) value = asr(value << 16, 16);
		m->cpu.r[rd] = value;
		elapsed = access.cycles + 1;
	}
	*cycles += kind <= 2 ? store_stall(m, address, width, elapsed) : address < 0x08000000 ? (uint32_t) cpu_stall(m, elapsed) : elapsed;
	*cycles += (uint32_t) fetch_break(m, true);
	return GBN_STEP;
}

static enum GbnStatus thumb_step(struct Gbn* m) {
	struct GbnCpu* c = &m->cpu;
	unsigned op = c->pipe[0];
	/* Fetch precedes stores, preserving already-prefetched instructions. */
	uint16_t ahead = (uint16_t) fetch(m, c->pc + 4, 2);
	uint32_t cycles = 1u + m->code_wait;
	bool jumped = false;
	enum GbnStatus status = GBN_STEP;
	unsigned rd = op & 7, rs = op >> 3 & 7;
	/* A dense top-six-bit switch gives the compiler a single jump table.
	 * Only miscellaneous/conditional groups need a second encoding check. */
	switch (op >> 10) {
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: {
		unsigned kind = op >> 11, amount = op >> 6 & 31;
		if (!amount && kind) amount = 32;
		c->r[rd] = shift(c, c->r[rs], amount, kind);
		nz(c, c->r[rd]);
	} break;
	case 0x06: case 0x07: {
		uint32_t a = c->r[rs], b = op >> 6 & 7;
		if (!(op & 0x400)) b = c->r[b];
		c->r[rd] = op & 0x200 ? sub(c, a, b, 0, false) : add(c, a, b, 0);
	} break;
	case 0x08: case 0x09: case 0x0a: case 0x0b:
	case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
		rd = op >> 8 & 7;
		uint32_t immediate = op & 255;
		switch (op >> 11 & 3) {
		case 0: c->r[rd] = immediate; nz(c, immediate); break;
		case 1: sub(c, c->r[rd], immediate, 0, false); break;
		case 2: c->r[rd] = add(c, c->r[rd], immediate, 0); break;
		case 3: c->r[rd] = sub(c, c->r[rd], immediate, 0, false); break;
		}
	} break;
	case 0x10: {
		uint32_t a = c->r[rd], b = c->r[rs], value;
		unsigned kind = op >> 6 & 15;
		switch (kind) {
		case 0: value = a & b; break;
		case 1: value = a ^ b; break;
		case 2: case 3: case 4: value = shift(c, a, b & 255, kind - 2); ++cycles; break;
		case 5: value = add(c, a, b, (c->cpsr & FLAG_C) != 0); break;
		case 6: value = sub(c, a, b, !(c->cpsr & FLAG_C), true); break;
		case 7: value = shift(c, a, b & 255, 3); ++cycles; break;
		case 8: value = a & b; break;
		case 9: value = sub(c, 0, b, 0, false); break;
		case 10: value = sub(c, a, b, 0, false); break;
		case 11: value = add(c, a, b, 0); break;
		case 12: value = a | b; break;
		case 13: {
			uint32_t upper = a;
			unsigned ticks = 1;
			while (ticks < 4) {
				upper = asr(upper, 8);
				if (!upper || upper == UINT32_MAX) break;
				++ticks;
			}
			cycles += (uint32_t) cpu_stall(m, ticks) + (uint32_t) fetch_break(m, true);
			value = a * b;
			break;
		}
		case 14: value = a & ~b; break;
		default: value = ~b; break;
		}
		nz(c, value);
		if (kind != 8 && kind != 10 && kind != 11) c->r[rd] = value;
	} break;
	case 0x11: {
		rd |= (op >> 4) & 8;
		rs = op >> 3 & 15;
		unsigned kind = op >> 8 & 3;
		uint32_t value = reg(c, rs);
		if (kind == 1) sub(c, reg(c, rd), value, 0, false);
		else if (kind == 3) {
			if (op & 0x80) return GBN_UNSUPPORTED_INSTRUCTION; /* ARMv5 BLX encoding */
			status = branch_mode(m, value, (value & 1) != 0, &cycles, &jumped);
		} else {
			if (!kind) value += reg(c, rd);
			if (rd == 15) status = branch(m, value, &cycles, &jumped);
			else c->r[rd] = value;
		}
	} break;
	case 0x12: case 0x13: {
		status = memory(m, ((c->pc + 4) & ~3u) + (op & 255) * 4, op >> 8 & 7, 4, &cycles);
	} break;
	case 0x14: case 0x15: case 0x16: case 0x17: {
		status = memory(m, c->r[rs] + c->r[op >> 6 & 7], rd, op >> 9 & 7, &cycles);
	} break;
	case 0x18: case 0x19: case 0x1a: case 0x1b:
	case 0x1c: case 0x1d: case 0x1e: case 0x1f:
	case 0x20: case 0x21: case 0x22: case 0x23: {
		unsigned kind, scale;
		if (op < 0x8000) { kind = (op & 0x1000 ? 2 : 0) + (op & 0x800 ? 4 : 0); scale = op & 0x1000 ? 1 : 4; }
		else { kind = op & 0x800 ? 5 : 1; scale = 2; }
		status = memory(m, c->r[rs] + (op >> 6 & 31) * scale, rd, kind, &cycles);
	} break;
	case 0x24: case 0x25: case 0x26: case 0x27: {
		status = memory(m, c->r[13] + (op & 255) * 4, op >> 8 & 7, op & 0x800 ? 4 : 0, &cycles);
	} break;
	case 0x28: case 0x29: case 0x2a: case 0x2b: {
		c->r[op >> 8 & 7] = (op & 0x800 ? c->r[13] : (c->pc + 4) & ~3u) + (op & 255) * 4;
	} break;
	case 0x2c: {
		if ((op & 0xff00) != 0xb000) return GBN_UNSUPPORTED_INSTRUCTION;
		uint32_t offset = (op & 127) * 4;
		c->r[13] += op & 128 ? 0u - offset : offset;
	} break;
	case 0x2d: case 0x2f: {
		if (op & 0x200) return GBN_UNSUPPORTED_INSTRUCTION;
		bool pop = (op & 0x800) != 0;
		uint32_t list = op & 255;
		if (op & 0x100) list |= 1u << (pop ? 15 : 14);
		status = multiple(m, 13, list, pop, pop, !pop, true, (op & 0x100) != 0, false, false, &cycles, &jumped);
	} break;
	case 0x30: case 0x31: case 0x32: case 0x33: {
		status = multiple(m, op >> 8 & 7, op & 255, (op & 0x800) != 0, true, false, true, true, false, false, &cycles, &jumped);
	} break;
	case 0x34: case 0x35: case 0x36: case 0x37: {
		if (op >= 0xdf00) {
			status = exception(m, 8, 0x13, c->pc + 2, &cycles);
			jumped = status == GBN_STEP;
		} else if (op >= 0xde00) return GBN_UNSUPPORTED_INSTRUCTION;
		else if (condition(c->cpsr, op >> 8 & 15)) {
			uint32_t offset = (op & 127) * 2 - (op & 128 ? 256u : 0);
			status = branch(m, c->pc + 4 + offset, &cycles, &jumped);
		}
	} break;
	case 0x38: case 0x39: {
		uint32_t offset = (op & 1023) * 2 - (op & 1024 ? 2048u : 0);
		status = branch(m, c->pc + 4 + offset, &cycles, &jumped);
	} break;
	case 0x3c: case 0x3d: {
		uint32_t offset = (op & 1023) * 4096 - (op & 1024 ? 4194304u : 0);
		c->r[14] = c->pc + 4 + offset;
	} break;
	case 0x3e: case 0x3f: {
		uint32_t link = (c->pc + 2) | 1;
		status = branch(m, c->r[14] + (op & 2047) * 2, &cycles, &jumped);
		if (status == GBN_STEP) c->r[14] = link;
	} break;
	default: return GBN_UNSUPPORTED_INSTRUCTION;
	}
	if (status != GBN_STEP) return status;
	if (!jumped) {
		c->pc += 2;
		c->pipe[0] = c->pipe[1];
		c->pipe[1] = ahead;
	}
	m->now += cycles;
	return GBN_STEP;
}

static unsigned multiply_cycles(uint32_t value, bool signed_multiply) {
	unsigned cycles = 1;
	while (cycles < 4) {
		value = signed_multiply ? asr(value, 8) : value >> 8;
		if (!value || (signed_multiply && value == UINT32_MAX)) break;
		++cycles;
	}
	return cycles;
}

/* Compute a barrel-shifter result without committing flags. This is shared
 * by data processing and register-offset addressing, with explicit PC rules. */
static uint32_t arm_shift(struct GbnCpu* c, uint32_t value, unsigned kind,
                          unsigned amount, bool register_shift, uint32_t* cout) {
	uint32_t flags = c->cpsr;
	if (!register_shift && !amount) {
		if (kind == 3) {
			*cout = value & 1;
			return (flags & FLAG_C ? FLAG_N : 0) | (value >> 1);
		}
		if (kind) amount = 32;
	}
	uint32_t out = shift(c, value, amount, kind);
	*cout = (c->cpsr & FLAG_C) != 0;
	c->cpsr = flags;
	return out;
}

static enum GbnStatus arm_alu(struct Gbn* m, uint32_t op, uint32_t* cycles, bool* jumped) {
	struct GbnCpu* c = &m->cpu;
	unsigned kind = op >> 21 & 15, rd = op >> 12 & 15, rn = op >> 16 & 15;
	bool set = (op & 0x100000) != 0;
	bool test = kind >= 8 && kind <= 11;
	if (test && !set) return GBN_UNSUPPORTED_INSTRUCTION; /* PSR transfer encodings */
	if (!(op & 0x2000000) && (op & 0x90) == 0x90) return GBN_UNSUPPORTED_INSTRUCTION;
	bool restore = set && rd == 15 && bank(c->cpsr) != 0;
	if (restore && (test || bank(c->spsr) < 0)) return GBN_UNSUPPORTED_MODE;
	uint32_t old_flags = c->cpsr, old_carry = c->shifter_carry;
	uint32_t a = reg(c, rn), b, cout = (old_flags & FLAG_C) != 0;
	if (op & 0x2000000) {
		unsigned rotation = op >> 7 & 30;
		b = ror(op & 255, rotation);
		if (rotation) cout = b >> 31;
	} else {
		bool rs = (op & 16) != 0;
		unsigned rm = op & 15;
		unsigned amount = rs ? reg(c, op >> 8 & 15) & 255 : op >> 7 & 31;
		b = reg(c, rm);
		if (rs) {
			++*cycles;
			if (rm == 15) b += 4;
			if (rn == 15) a += 4;
		}
		b = arm_shift(c, b, op >> 5 & 3, amount, rs, &cout);
	}
	uint32_t value;
	bool arithmetic = false;
	switch (kind) {
	case 0: case 8: value = a & b; break;
	case 1: case 9: value = a ^ b; break;
	case 2: case 10: value = sub(c, a, b, 0, false); arithmetic = true; break;
	case 3: value = sub(c, b, a, 0, false); arithmetic = true; break;
	case 4: case 11: value = add(c, a, b, 0); arithmetic = true; break;
	case 5: value = add(c, a, b, (old_flags & FLAG_C) != 0); arithmetic = true; break;
	case 6: value = sub(c, a, b, !(old_flags & FLAG_C), true); arithmetic = true; break;
	case 7: value = sub(c, b, a, !(old_flags & FLAG_C), true); arithmetic = true; break;
	case 12: value = a | b; break;
	case 13: value = b; break;
	case 14: value = a & ~b; break;
	default: value = ~b; break;
	}
	if (!set) c->cpsr = old_flags;
	else if (!arithmetic) { nz(c, value); carry(c, cout); }
	c->shifter_carry = cout;
	if (test) return GBN_STEP;
	if (rd == 15) {
		if (restore) {
			if (!executable(m, value)) {
				c->cpsr = old_flags; c->shifter_carry = old_carry;
				return GBN_UNSUPPORTED_ADDRESS;
			}
			c->cpsr = old_flags;
			set_cpsr(c, c->spsr);
		}
		enum GbnStatus status = branch(m, value, cycles, jumped);
		if (status != GBN_STEP) { c->cpsr = old_flags; c->shifter_carry = old_carry; }
		return status;
	}
	c->r[rd] = value;
	return GBN_STEP;
}

static enum GbnStatus arm_psr(struct Gbn* m, uint32_t op, uint32_t ahead, bool* jumped) {
	struct GbnCpu* c = &m->cpu;
	if (!(op & 0x200000)) { /* MRS */
		unsigned rd = op >> 12 & 15;
		if (rd == 15) return GBN_UNSUPPORTED_INSTRUCTION;
		c->r[rd] = op & 0x400000 ? c->spsr : c->cpsr;
		return GBN_STEP;
	}
	uint32_t operand = op & 0x2000000 ? ror(op & 255, op >> 7 & 30) : reg(c, op & 15);
	uint32_t mask = (op & 0x80000 ? 0xf0000000u : 0) | (op & 0x10000 ? 0x20 : 0);
	if (op & 0x400000) {
		if (op & 0x10000) mask |= 0xcf;
		c->spsr = (c->spsr & ~mask) | (operand & mask) | 0x10;
		return GBN_STEP;
	}
	if ((op & 0x10000) && (c->cpsr & 31) != 16) mask |= 0xcf;
	uint32_t value = (c->cpsr & ~mask) | (operand & mask);
	if (bank(value) < 0) return GBN_UNSUPPORTED_MODE;
	set_cpsr(c, value);
	if (value & 0x20) {
		/* Preserve the reference ARM7 pipeline response to a direct T-bit MSR.
		 * Normal interworking uses BX; no extra refill cycles are charged here. */
		c->pc += 8;
		c->pipe[0] = 0x46c0;
		c->pipe[1] = ahead & 0xffff;
		m->code_mask |= 2;
	} else {
		c->pc += 4;
		c->pipe[0] = fetch(m, c->pc, 4);
		c->pipe[1] = fetch(m, c->pc + 4, 4);
	}
	*jumped = true;
	return GBN_STEP;
}

static enum GbnStatus arm_memory(struct Gbn* m, uint32_t op, bool half, uint32_t* cycles, bool* jumped) {
	struct GbnCpu* c = &m->cpu;
	unsigned rn = op >> 16 & 15, rd = op >> 12 & 15;
	bool load = (op & 0x100000) != 0, pre = (op & 0x1000000) != 0;
	bool writeback = !pre || (op & 0x200000) != 0;
	unsigned kind = half ? op >> 5 & 3 : 0;
	if (half && (!kind || (!load && kind != 1))) return GBN_UNSUPPORTED_INSTRUCTION;
	if (!half && (op & 0x2000010) == 0x2000010) return GBN_UNSUPPORTED_INSTRUCTION;
	unsigned width = half ? (kind == 2 ? 1 : 2) : (op & 0x400000 ? 1 : 4);
	uint32_t offset;
	if (half) offset = op & 0x400000 ? (op >> 4 & 0xf0) | (op & 15) : reg(c, op & 15);
	else if (op & 0x2000000) {
		uint32_t ignored;
		offset = arm_shift(c, reg(c, op & 15), op >> 5 & 3, op >> 7 & 31, false, &ignored);
	} else offset = op & 4095;
	uint32_t adjusted = reg(c, rn) + (op & 0x800000 ? offset : 0u - offset);
	uint32_t address = pre ? adjusted : reg(c, rn);
	uint32_t value = reg(c, rd);
	/* Post-index user stores in the reference use the visible PC rather than
 * the normal store PC + 4. All supported RAM is accessible in either mode. */
	if (rd == 15 && (half || pre || !(op & 0x200000))) value += 4;
	if (writeback && rn == 15) {
		if (!executable(m, adjusted)) return GBN_UNSUPPORTED_ADDRESS;
	}
	struct GbnAccess access;
	enum GbnStatus status;
	if (load) {
		status = gbn_read(m, address, width, &access);
		if (status != GBN_STEP) return status;
		value = access.value;
		if (half && (kind == 2 || (kind == 3 && (address & 1)))) value = asr(value << 24, 24);
		else if (half && kind == 3) value = asr(value << 16, 16);
		if (rd == 15) {
			if (!executable(m, value)) return GBN_UNSUPPORTED_ADDRESS;
		}
	} else {
		status = gbn_write(m, address, width, value, &access.cycles);
		if (status != GBN_STEP) return status;
	}
	uint32_t elapsed = access.cycles + (load ? 1 : 0);
	*cycles += !load ? store_stall(m, address, width, elapsed) : address < 0x08000000 ? (uint32_t) cpu_stall(m, elapsed) : elapsed;
	if (writeback && (load || rn != rd)) {
		if (rn != 15) c->r[rn] = adjusted;
		else branch(m, adjusted, cycles, jumped); /* Validated above. */
	}
	if (load) {
		*cycles += (uint32_t) fetch_break(m, false);
		if (rd != 15) c->r[rd] = value;
		else branch(m, value, cycles, jumped);
	} else if (writeback && rn == rd) {
		if (rn != 15) c->r[rn] = adjusted;
		else branch(m, adjusted, cycles, jumped);
	}
	if (!load) *cycles += (uint32_t) fetch_break(m, false);
	return GBN_STEP;
}

static enum GbnStatus arm_step(struct Gbn* m) {
	struct GbnCpu* c = &m->cpu;
	uint32_t op = c->pipe[0];
	uint32_t ahead = fetch(m, c->pc + 8, 4);
	uint32_t cycles = 1u + m->code_word_wait;
	bool jumped = false;
	enum GbnStatus status = GBN_STEP;
	if (!condition(c->cpsr, op >> 28)) goto complete;
	if ((op & 0x0f000000) == 0x0f000000) {
		status = exception(m, 8, 0x13, c->pc + 4, &cycles);
		jumped = status == GBN_STEP;
	} else if ((op & 0x0ffffff0) == 0x012fff10) {
		uint32_t target = reg(c, op & 15);
		status = branch_mode(m, target, (target & 1) != 0, &cycles, &jumped);
	} else if ((op & 0x0fbf0fff) == 0x010f0000 ||
	           (op & 0x0fb0fff0) == 0x0120f000 || (op & 0x0fb0f000) == 0x0320f000) {
		status = arm_psr(m, op, ahead, &jumped);
	} else if ((op & 0x0e000000) == 0x0a000000) {
		uint32_t offset = (op & 0x7fffff) * 4 - (op & 0x800000 ? 0x2000000u : 0);
		uint32_t link = c->pc + 4;
		status = branch(m, c->pc + 8 + offset, &cycles, &jumped);
		if (status == GBN_STEP && (op & 0x1000000)) c->r[14] = link;
	} else if ((op & 0x0fc000f0) == 0x00000090) {
		unsigned rd = op >> 16 & 15, rn = op >> 12 & 15;
		if (rd == 15 || ((op & 0x200000) && rn == 15)) goto complete;
		uint32_t rs = reg(c, op >> 8 & 15), rm = reg(c, op & 15);
		uint32_t value = rs * rm;
		unsigned internal = multiply_cycles(rs, true);
		if (op & 0x200000) { value += c->r[rn]; ++internal; }
		cycles += (uint32_t) cpu_stall(m, internal) + (uint32_t) fetch_break(m, false);
		c->r[rd] = value;
		if (op & 0x100000) { nz(c, value); carry(c, c->shifter_carry); }
	} else if ((op & 0x0f8000f0) == 0x00800090) {
		unsigned hi = op >> 16 & 15, lo = op >> 12 & 15;
		if (hi == 15 || lo == 15) goto complete;
		uint32_t rs = reg(c, op >> 8 & 15), rm = reg(c, op & 15);
		bool sign = (op & 0x400000) != 0;
		uint64_t product = sign ? (uint64_t) ((int64_t) (int32_t) rs * (int64_t) (int32_t) rm) : (uint64_t) rs * rm;
		unsigned internal = 1 + multiply_cycles(rs, sign);
		if (op & 0x200000) { product += ((uint64_t) c->r[hi] << 32) | c->r[lo]; ++internal; }
		cycles += (uint32_t) cpu_stall(m, internal) + (uint32_t) fetch_break(m, false);
		c->r[lo] = (uint32_t) product;
		c->r[hi] = (uint32_t) (product >> 32);
		if (op & 0x100000) c->cpsr = (c->cpsr & ~(FLAG_N | FLAG_Z)) | (c->r[hi] & FLAG_N) | (product ? 0 : FLAG_Z);
	} else if ((op & 0x0fb00ff0) == 0x01000090) {
		unsigned rn = op >> 16 & 15, rd = op >> 12 & 15;
		if (rd == 15) return GBN_UNSUPPORTED_INSTRUCTION;
		uint32_t address = reg(c, rn), value = reg(c, op & 15), store_cycles;
		unsigned width = op & 0x400000 ? 1 : 4;
		struct GbnAccess access;
		status = gbn_read(m, address, width, &access);
		if (status != GBN_STEP) return status;
		status = gbn_write(m, address, width, value, &store_cycles);
		if (status != GBN_STEP) return status;
		c->r[rd] = access.value;
		if (address < 0x08000000) {
			cycles += (uint32_t) cpu_stall(m, access.cycles + 1);
			cycles += store_stall(m, address, width, store_cycles);
		}
		else cycles += access.cycles + store_cycles + 1;
	} else if ((op & 0x0e000090) == 0x00000090) {
		status = arm_memory(m, op, true, &cycles, &jumped);
	} else if ((op & 0x0c000000) == 0) {
		status = arm_alu(m, op, &cycles, &jumped);
	} else if ((op & 0x0c000000) == 0x04000000) {
		status = arm_memory(m, op, false, &cycles, &jumped);
	} else if ((op & 0x0e000000) == 0x08000000) {
		unsigned rn = op >> 16 & 15;
		if (rn == 15) return GBN_UNSUPPORTED_INSTRUCTION;
		bool load = (op & 0x100000) != 0, special = (op & 0x400000) != 0;
		bool pc = (op & 0x8000) != 0 || !(op & 0xffff);
		bool user = special && (!load || !pc), restore = special && load && pc && bank(c->cpsr) != 0;
		status = multiple(m, rn, op & 0xffff, load, (op & 0x800000) != 0,
		                  (op & 0x1000000) != 0, (op & 0x200000) != 0, true, user, restore, &cycles, &jumped);
	} else return GBN_UNSUPPORTED_INSTRUCTION;
	if (status != GBN_STEP) return status;
complete:
	if (!jumped) {
		c->pc += 4;
		c->pipe[0] = c->pipe[1];
		c->pipe[1] = ahead;
	}
	m->now += cycles;
	return GBN_STEP;
}

/* Execute one instruction after the boundary checks in gbn_step/gbn_run_batch.
 * The batch caller can amortize frontend event polling and statistics while
 * keeping the same architectural checks at every instruction boundary. */
static enum GbnStatus step_instruction(struct Gbn* m) {
	if (!m->code) return GBN_UNSUPPORTED_ADDRESS;
	if (m->builtin_bios && m->code_region == 0 && m->cpu.pc == 0x20) return GBN_UNSUPPORTED_DEVICE;
	bool thumb = (m->cpu.cpsr & 0x20) != 0;
	bool bios = m->code_region == 0;
	uint32_t old_cpsr = m->cpu.cpsr;
	unsigned width = thumb ? 2 : 4;
	uint32_t bios_ahead = bios ? fetch(m, m->cpu.pc + 2 * width, width) : 0;
	m->cpu_access = true;
	enum GbnStatus status = thumb ? thumb_step(m) : arm_step(m);
	m->cpu_access = false;
	if (status == GBN_UNSUPPORTED_INSTRUCTION && m->bios) {
		uint32_t cycles = 1u + (thumb ? m->code_wait : m->code_word_wait);
		status = exception(m, 4, 0x1b, m->cpu.pc + width, &cycles);
		if (status == GBN_STEP) m->now += cycles;
	}
	if (status == GBN_STEP && bios && m->code_region != 0) m->bios_latch = bios_ahead;
	if (status == GBN_STEP && m->devices && (old_cpsr & ~m->cpu.cpsr & 0x80)) gbn_update_irq(m, m->now);
	return status;
}

static enum GbnStatus step_boundary(struct Gbn* m) {
	if (m->devices && (m->devices->dma_blocked || m->devices->halted || m->devices->stopped)) return GBN_EVENT;
	if (event_due(m)) return GBN_EVENT;
	if (bank(m->cpu.cpsr) < 0) return GBN_UNSUPPORTED_MODE;
	return GBN_STEP;
}

enum GbnStatus gbn_step(struct Gbn* m) {
	enum GbnStatus status = step_boundary(m);
	return status == GBN_STEP ? step_instruction(m) : status;
}

enum GbnStatus gbn_run_batch(struct Gbn* m, uint32_t max_instructions, uint32_t* executed) {
	if (!executed) return GBN_INVALID_ARGUMENT;
	*executed = 0;
	if (!max_instructions) return GBN_INVALID_ARGUMENT;
	while (*executed < max_instructions) {
		enum GbnStatus status = step_boundary(m);
		if (status != GBN_STEP) return status;
		status = step_instruction(m);
		if (status != GBN_STEP) return status;
		++*executed;
	}
	return GBN_STEP;
}

enum GbnStatus gbn_rv32_run_batch(struct GbnRv32* backend, uint32_t cap, uint32_t* executed) {
	if (!executed) return GBN_INVALID_ARGUMENT;
	*executed = 0;
	if (!backend || !backend->machine || !cap) return GBN_INVALID_ARGUMENT;
	struct Gbn* m = backend->machine;
	bool fallback = false;
	while (*executed < cap) {
		enum GbnStatus status = step_boundary(m);
		if (status != GBN_STEP) return status;
		unsigned count = fallback ? 0 : gbn_rv32_execute(backend, cap - *executed);
		fallback = (count & 0x80000000u) != 0;
		count &= 0x7fffffffu;
		if (count) {
			*executed += count;
#if GBN_RV32_STATS
			backend->native_instructions += count;
#endif
			continue;
		}
#if GBN_RV32_STATS
		unsigned kind = m->code_region == 0 ? 0 : m->code_region == 2 ? 1 : m->code_region == 3 ? 2 :
			(m->cpu.cpsr & 0x20) ? 4 : 3;
#endif
		/* BIOS has no native translation yet. Stay in the interpreter until
		 * execution returns to ROM/RAM instead of probing at every instruction. */
		for (;;) {
			status = step_instruction(m);
			fallback = false;
			if (status != GBN_STEP) return status;
			++*executed;
#if GBN_RV32_STATS
			++backend->fallback_instructions;
			++backend->fallback_kind[kind];
#endif
			if (*executed == cap || m->code_region == 2 || m->code_region == 3 || (unsigned) (m->code_region - 8) < 6) break;
			status = step_boundary(m);
			if (status != GBN_STEP) return status;
#if GBN_RV32_STATS
			kind = m->code_region == 0 ? 0 : m->code_region == 2 ? 1 : m->code_region == 3 ? 2 :
				(m->cpu.cpsr & 0x20) ? 4 : 3;
#endif
		}
	}
	return GBN_STEP;
}

enum GbnStatus gbn_run_for(struct Gbn* m, uint32_t budget) {
	if (budget > GBN_MAX_DELAY) return GBN_INVALID_ARGUMENT;
	uint32_t deadline = m->now + budget;
	for (;;) {
		if (event_due(m)) return GBN_EVENT;
		if ((int32_t) (m->now - deadline) >= 0) return GBN_DEADLINE;
		enum GbnStatus status = gbn_step(m);
		if (status != GBN_STEP) return status;
	}
}
