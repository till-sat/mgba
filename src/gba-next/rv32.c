/* SPDX-License-Identifier: MPL-2.0 */
/* Independent ARM/Thumb -> RV32 backend. Native chains retain r0-r7, CPSR,
 * guest time and prefetch state in one register frame across direct edges.
 * Generated code has no C calls or instruction decoding. */
#include <gba-next/rv32.h>
#include "internal.h"
#include <stddef.h>
#include <string.h>

#ifndef GBN_RV32_STATS
#define GBN_RV32_STATS 0
#endif
#if defined(__riscv) && __riscv_xlen == 32 && defined(__riscv_mul) && defined(__riscv_zifencei) && !defined(__riscv_abi_rve)
#define NATIVE_RV32 1
#else
#define NATIVE_RV32 0
#endif

bool gbn_rv32_available(void) { return NATIVE_RV32 != 0; }

void gbn_rv32_init(struct GbnRv32* backend, struct Gbn* m) {
	memset(backend, 0, sizeof(*backend));
	backend->machine = m;
}

#if NATIVE_RV32
_Static_assert(offsetof(struct Gbn, prefetched_pc) < 2048, "Native state offsets must fit load/store immediates");
_Static_assert(GBN_RV32_SLOTS >= 2 && !(GBN_RV32_SLOTS & (GBN_RV32_SLOTS - 1)), "Cache slots must be a power of two");
_Static_assert(GBN_RV32_WAYS && !(GBN_RV32_WAYS & (GBN_RV32_WAYS - 1)) &&
	GBN_RV32_WAYS <= 256 && GBN_RV32_WAYS <= GBN_RV32_SLOTS, "Cache ways must be a power of two and fit replacement counters");
_Static_assert(GBN_RV32_SEGMENT_WORDS >= GBN_RV32_WORDS, "A block must fit in one arena segment");
enum { ZERO = 0, RA = 1, SP = 2, LHS = 5, RHS = 6, RESULT = 7,
	NOW = 8, DEADLINE = 9, MACHINE = 10, LIMIT = 11, FLAGS = 12, TEMP = 13,
	PREFETCH = 18, WAIT = 19, COMPLETED = 20, NEGATIVE = 21, IS_ZERO = 22,
	PROLOGUE_WORDS = 27,
	FRAME_BYTES = 80, SNAPSHOT_FLAGS = 52, SNAPSHOT_PREFETCH = 56, SNAPSHOT_NOW = 60,
	STACK_TARGET = 64 };
static const unsigned guest[] = {14, 15, 16, 17, 28, 29, 30, 31};
struct Emit {
	struct GbnRv32Block* block;
	unsigned count;
	struct { unsigned at, instruction; bool fallback; } stops[10 * GBN_RV32_INSNS];
	unsigned stop_count;
	unsigned returns[2], return_count;
	uint32_t written;
	bool read_only, read_seen, dynamic_pc;
	unsigned loop_jump;
};

static void word(struct Emit* e, uint32_t v) { e->block->code[e->count++] = v; }
static void imm(struct Emit* e, unsigned rd, unsigned rs, unsigned fn, int v) {
	word(e, ((uint32_t) v & 4095) << 20 | rs << 15 | fn << 12 | rd << 7 | 0x13);
}
static void op(struct Emit* e, unsigned rd, unsigned a, unsigned b, unsigned fn, unsigned ext) {
	word(e, ext << 25 | b << 20 | a << 15 | fn << 12 | rd << 7 | 0x33);
}
static void constant(struct Emit* e, unsigned rd, uint32_t v) {
	uint32_t upper = (v + 0x800) & ~0xfffu;
	if (upper) word(e, upper | rd << 7 | 0x37);
	imm(e, rd, upper ? rd : ZERO, 0, (int32_t) (v - upper));
}
static void load(struct Emit* e, unsigned rd, unsigned offset) {
	word(e, offset << 20 | MACHINE << 15 | 2 << 12 | rd << 7 | 3);
}
static void store(struct Emit* e, unsigned rs, unsigned offset) {
	word(e, (offset >> 5) << 25 | rs << 20 | MACHINE << 15 | 2 << 12 | (offset & 31) << 7 | 0x23);
}
static uint32_t beq(unsigned a, unsigned b, unsigned delta) {
	return ((delta >> 12) & 1) << 31 | ((delta >> 5) & 63) << 25 | b << 20 | a << 15 |
		((delta >> 1) & 15) << 8 | ((delta >> 11) & 1) << 7 | 0x63;
}
static void stop(struct Emit* e, unsigned a, unsigned b, unsigned fn, bool fallback) {
	e->stops[e->stop_count].at = e->count;
	e->stops[e->stop_count].instruction = e->block->instructions;
	e->stops[e->stop_count++].fallback = fallback;
	word(e, b << 20 | a << 15 | fn << 12 | 0x63);
}
static void guard(struct Emit* e, unsigned reg, bool reject_zero) {
	stop(e, reg, ZERO, reject_zero ? 0 : 1, true);
}
static void get(struct Emit* e, unsigned dest, unsigned r, uint32_t pc) {
	if (r < 8) imm(e, dest, guest[r], 0, 0);
	else if (r == 15) constant(e, dest, pc + (e->block->thumb ? 4 : 8));
	else load(e, dest, offsetof(struct Gbn, cpu.r) + 4 * r);
}
/* Low guest registers already reside in host registers. Use them directly
 * as read operands; high registers and PC use the designated scratch register. */
static unsigned source(struct Emit* e, unsigned scratch, unsigned r, uint32_t pc) {
	if (r < 8) return guest[r];
	get(e, scratch, r, pc);
	return scratch;
}
static void put_value(struct Emit* e, unsigned r, unsigned value) {
	e->written |= 1u << r;
	if (r < 8) { if (guest[r] != value) imm(e, guest[r], value, 0, 0); }
	else store(e, value, offsetof(struct Gbn, cpu.r) + 4 * r);
}
static void put(struct Emit* e, unsigned r) { put_value(e, r, RESULT); }
static void nz(struct Emit* e) {
	/* Keep independent booleans: entry CPSR may legally contain N=Z=1. */
	imm(e, NEGATIVE, RESULT, 5, 31);
	imm(e, IS_ZERO, RESULT, 3, 1);
}
/* FLAGS retains bits 0..29. Materialize N/Z only when the complete CPSR is
 * observed, including fixed-point proofs and every native return reason. */
static void packed_flags(struct Emit* e, unsigned dest) {
	imm(e, dest, NEGATIVE, 1, 31); imm(e, LHS, IS_ZERO, 1, 30);
	op(e, dest, dest, LHS, 6, 0); op(e, dest, FLAGS, dest, 6, 0);
}
/* Inputs may alias their assigned LHS/RHS scratch registers, respectively;
 * neither is overwritten until all uses of that original operand complete. */
static void arithmetic(struct Emit* e, bool subtract, unsigned a, unsigned b) {
	op(e, RESULT, a, b, 0, subtract ? 0x20 : 0);
	/* Carry first, while both operands are intact. */
	op(e, TEMP, subtract ? a : RESULT, subtract ? b : a, 3, 0);
	if (subtract) imm(e, TEMP, TEMP, 4, 1);
	op(e, RHS, a, b, 4, 0);
	if (!subtract) imm(e, RHS, RHS, 4, -1);
	op(e, LHS, a, RESULT, 4, 0); op(e, LHS, LHS, RHS, 7, 0);
	imm(e, LHS, LHS, 5, 31); imm(e, LHS, LHS, 1, 28);
	imm(e, TEMP, TEMP, 1, 29);
	/* ARM7 ADD/SUB also clear reserved PSR bits 24..27 in our oracle. */
	imm(e, FLAGS, FLAGS, 1, 8); imm(e, FLAGS, FLAGS, 5, 8);
	op(e, FLAGS, FLAGS, LHS, 6, 0); op(e, FLAGS, FLAGS, TEMP, 6, 0);
	nz(e);
}
static void arithmetic_carry(struct Emit* e, bool subtract, unsigned a, unsigned b) {
	imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1);
	op(e, RESULT, a, b, 0, subtract ? 0x20 : 0);
	op(e, TEMP, subtract ? a : RESULT, subtract ? b : a, 3, 0);
	if (subtract) {
		imm(e, TEMP, TEMP, 4, 1);
		word(e, beq(a, b, 8) | 1u << 12); /* a != b: C = a >= b */
		op(e, TEMP, TEMP, WAIT, 7, 0); /* a == b: C = carry in */
		imm(e, WAIT, WAIT, 4, 1); op(e, RESULT, RESULT, WAIT, 0, 0x20);
	} else {
		op(e, RESULT, RESULT, WAIT, 0, 0);
		op(e, WAIT, RESULT, WAIT, 3, 0); op(e, TEMP, TEMP, WAIT, 6, 0);
	}
	op(e, RHS, a, b, 4, 0);
	if (!subtract) imm(e, RHS, RHS, 4, -1);
	op(e, LHS, a, RESULT, 4, 0); op(e, LHS, LHS, RHS, 7, 0);
	imm(e, LHS, LHS, 5, 31); imm(e, LHS, LHS, 1, 28);
	imm(e, TEMP, TEMP, 1, 29);
	imm(e, FLAGS, FLAGS, 1, subtract ? 4 : 8); imm(e, FLAGS, FLAGS, 5, subtract ? 4 : 8);
	op(e, FLAGS, FLAGS, LHS, 6, 0); op(e, FLAGS, FLAGS, TEMP, 6, 0);
	nz(e);
}
static void multiply_wait(struct Emit* e, unsigned multiplier, bool accumulate) {
	/* ARM7 early termination: each upper signed byte group equal to 0 or
	 * -1 removes one cycle. This also handles positive 0xff and -0x100. */
	constant(e, WAIT, accumulate ? 5 : 4);
	for (unsigned shift = 8; shift <= 24; shift += 8) {
		imm(e, TEMP, multiplier, 5, 0x400 | (int) shift);
		imm(e, TEMP, TEMP, 0, 1); imm(e, TEMP, TEMP, 3, 2);
		op(e, WAIT, WAIT, TEMP, 0, 0x20);
	}
}
static void condition(struct Emit* e, unsigned cond) {
	/* Boolean branch result in TEMP; N and Z already have boolean form. */
	if (cond < 2) imm(e, TEMP, IS_ZERO, 0, 0);
	else if (cond >= 4 && cond < 6) imm(e, TEMP, NEGATIVE, 0, 0);
	else if (cond < 8) { imm(e, TEMP, FLAGS, 5, cond < 4 ? 29 : 28); imm(e, TEMP, TEMP, 7, 1); }
	else {
		imm(e, LHS, IS_ZERO, 0, 0);
		if (cond < 10) {
			imm(e, TEMP, FLAGS, 5, 29); imm(e, TEMP, TEMP, 7, 1);
			imm(e, LHS, LHS, 4, 1); op(e, TEMP, TEMP, LHS, 7, 0);
		} else {
			imm(e, TEMP, NEGATIVE, 0, 0); imm(e, RHS, FLAGS, 5, 28);
			op(e, TEMP, TEMP, RHS, 4, 0); imm(e, TEMP, TEMP, 7, 1);
			imm(e, TEMP, TEMP, 4, 1);
			if (cond >= 12) { imm(e, LHS, LHS, 4, 1); op(e, TEMP, TEMP, LHS, 7, 0); }
		}
	}
	if (cond & 1) imm(e, TEMP, TEMP, 4, 1);
}

static uint32_t fetch(const struct Gbn* m, uint32_t pc, unsigned width) {
	uint32_t offset = pc & m->code_mask;
	if (m->code_region < 8) {
		uint32_t value = m->code[offset] | (uint32_t) m->code[offset + 1] << 8;
		if (width == 4) value |= (uint32_t) m->code[offset + 2] << 16 | (uint32_t) m->code[offset + 3] << 24;
		return value;
	}
	if (offset < m->rom_size && m->rom_size - offset >= width) {
		uint32_t value = m->rom[offset] | (uint32_t) m->rom[offset + 1] << 8;
		if (width == 4) value |= (uint32_t) m->rom[offset + 2] << 16 | (uint32_t) m->rom[offset + 3] << 24;
		return value;
	}
	return (uint16_t) (offset >> 1) | (width == 4 ? ((offset + 2) >> 1) << 16 : 0);
}

/* Update the ROM prefetch overlap after a RAM bus access. WAIT contains its
 * bus duration (plus the load internal cycle); NOW/PREFETCH stay resident.
 * This is the cpu_stall + fetch_break formula, specialized for stable timing
 * configuration. The following instruction checks the new event boundary. */
static void ram_wait(struct Emit* e, const struct Gbn* m, uint32_t pc) {
	unsigned seq = m->code_wait;
	if (m->code_region < 8 || !(m->waitcnt & 0x4000)) {
		imm(e, NOW, NOW, 0, (int) m->code_nonseq - (int) seq);
		op(e, NOW, NOW, WAIT, 0, 0);
		return;
	}
	unsigned shift = 0;
	while ((1u << shift) < seq) ++shift;
	constant(e, TEMP, pc + (e->block->thumb ? 4 : 8));
	op(e, LHS, PREFETCH, TEMP, 0, 0x20);
	imm(e, RESULT, LHS, 3, 16);
	op(e, RESULT, ZERO, RESULT, 0, 0x20);
	imm(e, LHS, LHS, 5, 1); op(e, LHS, LHS, RESULT, 7, 0); /* previous */
	/* extra = wait > seq+1 ? (wait-2)/seq : 0; seq is 1,2,4 or 8. */
	imm(e, RHS, WAIT, 0, -2); imm(e, RHS, RHS, 5, (int) shift);
	imm(e, RESULT, WAIT, 3, (int) seq + 2); imm(e, RESULT, RESULT, 0, -1);
	op(e, RHS, RHS, RESULT, 7, 0);
	constant(e, RESULT, 7); op(e, RESULT, RESULT, LHS, 0, 0x20);
	word(e, beq(RESULT, RHS, 8) | 7u << 12); /* bgeu remaining,extra,+8 */
	imm(e, RHS, RESULT, 0, 0);
	op(e, LHS, LHS, RHS, 0, 0); imm(e, LHS, LHS, 1, 1);
	op(e, PREFETCH, TEMP, LHS, 0, 0);
	imm(e, RHS, RHS, 1, (int) shift); imm(e, RHS, RHS, 0, (int) seq + 1);
	op(e, WAIT, WAIT, RHS, 0, 0x20);
	imm(e, RESULT, WAIT, 5, 0x41f); imm(e, RESULT, RESULT, 4, -1);
	op(e, WAIT, WAIT, RESULT, 7, 0); op(e, NOW, NOW, WAIT, 0, 0);
}

static void writeback(struct Emit* e, unsigned rn, int adjustment) {
	if (rn == 15) return;
	if (adjustment >= -2048 && adjustment < 2048) imm(e, RESULT, TEMP, 0, adjustment);
	else { constant(e, RHS, (uint32_t) adjustment); op(e, RESULT, TEMP, RHS, 0, 0); }
	put(e, rn);
}
static void access_value(struct Emit* e, uint32_t pc, unsigned rd, unsigned kind, unsigned width, unsigned rn, int adjustment) {
	unsigned fn = width == 4 ? 2 : width == 2 ? 1 : 0;
	if (kind <= 2) {
		get(e, RESULT, rd, pc);
		word(e, RESULT << 20 | LHS << 15 | fn << 12 | 0x23);
		writeback(e, rn, adjustment);
	} else {
		/* When base == destination the loaded value wins over writeback. */
		writeback(e, rn, adjustment);
		if (width < 4 && kind != 3 && kind != 7) fn |= 4;
		word(e, LHS << 15 | fn << 12 | RESULT << 7 | 3);
		put(e, rd);
	}
}

/* Effective address is in LHS. All guards precede data or base writeback. */
/* Bits 0/1 permit stored byte/halfword and word reads. Bits 2/3 permit
 * zero DMA count reads and count/control words with the low half masked.
 * Bits 4/5 permit pure open-bus byte/halfword and word reads.
 * Dynamic timers/audio/SIO, WAITCNT and EEPROM keep the bus path. */
static const uint8_t native_io_readable[0x210 / 2] = {
	[0] = 3, [1] = 3, [2] = 3, [3] = 3,
	[4] = 3, [5] = 3, [6] = 3, [7] = 1,
	[0x48 / 2] = 3, [0x4a / 2] = 1,
	[0x50 / 2] = 3, [0x52 / 2] = 1,
	[0x130 / 2] = 3, [0x132 / 2] = 3, [0x134 / 2] = 1,
	[0x200 / 2] = 3, [0x202 / 2] = 1, [0x208 / 2] = 1,
	/* DMA count reads are zero, even while the stored reload is nonzero.
	 * Word reads join zero with the live control halfword. */
	[0xb8 / 2] = 12, [0xba / 2] = 1,
	[0xc4 / 2] = 12, [0xc6 / 2] = 1,
	[0xd0 / 2] = 12, [0xd2 / 2] = 1,
	[0xdc / 2] = 12, [0xde / 2] = 1,
	[0x10 / 2] = 48,
	[0x12 / 2] = 16,
	[0x14 / 2] = 48,
	[0x16 / 2] = 16,
	[0x18 / 2] = 48,
	[0x1a / 2] = 16,
	[0x1c / 2] = 48,
	[0x1e / 2] = 16,
	[0x20 / 2] = 48,
	[0x22 / 2] = 16,
	[0x24 / 2] = 48,
	[0x26 / 2] = 16,
	[0x28 / 2] = 48,
	[0x2a / 2] = 16,
	[0x2c / 2] = 48,
	[0x2e / 2] = 16,
	[0x30 / 2] = 48,
	[0x32 / 2] = 16,
	[0x34 / 2] = 48,
	[0x36 / 2] = 16,
	[0x38 / 2] = 48,
	[0x3a / 2] = 16,
	[0x3c / 2] = 48,
	[0x3e / 2] = 16,
	[0x40 / 2] = 48,
	[0x42 / 2] = 16,
	[0x44 / 2] = 48,
	[0x46 / 2] = 16,
	[0x4c / 2] = 48,
	[0x4e / 2] = 16,
	[0x54 / 2] = 48,
	[0x56 / 2] = 16,
	[0x58 / 2] = 48,
	[0x5a / 2] = 16,
	[0x5c / 2] = 48,
	[0x5e / 2] = 16,
	[0xb0 / 2] = 48,
	[0xb2 / 2] = 16,
	[0xb4 / 2] = 48,
	[0xb6 / 2] = 16,
	[0xbc / 2] = 48,
	[0xbe / 2] = 16,
	[0xc0 / 2] = 48,
	[0xc2 / 2] = 16,
	[0xc8 / 2] = 48,
	[0xca / 2] = 16,
	[0xcc / 2] = 48,
	[0xce / 2] = 16,
	[0xd4 / 2] = 48,
	[0xd6 / 2] = 16,
	[0xd8 / 2] = 48,
	[0xda / 2] = 16
};
_Static_assert(offsetof(struct GbnDevices, io) == 0 && _Alignof(struct GbnDevices) >= 4,
	"Native IO loads require an aligned register array at the device base");

/* Whole-word DMA address writes have no scheduling side effects. Counts and
 * controls remain on the bus path. The flag selects the 27-bit address mask. */
#define DMA_FIELD(channel, member, narrow) (offsetof(struct GbnDevices, dma[channel].member) | ((narrow) ? 0x8000u : 0))
static const uint16_t native_dma_address_field[12] = {
	DMA_FIELD(0, source, 1), DMA_FIELD(0, dest, 1), 0,
	DMA_FIELD(1, source, 0), DMA_FIELD(1, dest, 1), 0,
	DMA_FIELD(2, source, 0), DMA_FIELD(2, dest, 1), 0,
	DMA_FIELD(3, source, 0), DMA_FIELD(3, dest, 0), 0
};
#undef DMA_FIELD
_Static_assert(offsetof(struct GbnDevices, dma[3].dest) < 2048, "DMA field offsets must fit the descriptor");
static void dma_address_store(struct Emit* e, const struct Gbn* m, uint32_t pc, unsigned rd, unsigned rn, int adjustment) {
	constant(e, LHS, 0x040000b0u); op(e, LHS, TEMP, LHS, 0, 0x20);
	imm(e, RESULT, LHS, 3, 48); guard(e, RESULT, true);
	imm(e, RHS, TEMP, 7, 3); guard(e, RHS, false);
	imm(e, RHS, LHS, 5, 1); constant(e, RESULT, (uint32_t) (uintptr_t) native_dma_address_field);
	op(e, RHS, RHS, RESULT, 0, 0);
	word(e, RHS << 15 | 5 << 12 | RHS << 7 | 3);
	guard(e, RHS, true);
	load(e, RESULT, offsetof(struct Gbn, devices)); guard(e, RESULT, true);
	imm(e, WAIT, RHS, 7, 2047); op(e, RESULT, RESULT, WAIT, 0, 0);
	imm(e, WAIT, RHS, 5, 15); imm(e, WAIT, WAIT, 1, 27);
	constant(e, RHS, 0x0ffffffeu); op(e, RHS, RHS, WAIT, 4, 0);
	get(e, WAIT, rd, pc); op(e, WAIT, WAIT, RHS, 7, 0);
	load(e, RHS, offsetof(struct Gbn, devices));
	imm(e, LHS, LHS, 0, 0xb0); op(e, LHS, LHS, RHS, 0, 0);
	/* Update programmed address and both IO halfwords, preserving the
	 * current transfer cursor, latch, count and scheduled event. */
	word(e, WAIT << 20 | RESULT << 15 | 2 << 12 | 0x23);
	word(e, WAIT << 20 | LHS << 15 | 2 << 12 | 0x23);
	writeback(e, rn, adjustment);
	constant(e, WAIT, 1); ram_wait(e, m, pc);
}



/* Shared native leaf routines. This private register convention retains
 * a0=machine and a3=original data address; t1 supplies the default halfword
 * or live RAM address, t2 the DMA PC to match, and s3 the return address.
 * Only t0/t2 are scratched. No C ABI call, state spill, or new stack slot.
 * Use only asm with immediate operands inside these naked functions. */
#define OPEN_BUS_DMA_SELECT \
    "lw t0, %c[devices](a0)\n\t" \
    "lbu t0, %c[access](t0)\n\t" \
    "bnez t0, 2f\n\t" \
    "lw t0, %c[devices](a0)\n\t" \
    "lbu t0, %c[valid](t0)\n\t" \
    "beqz t0, 1f\n\t" \
    "lw t0, %c[devices](a0)\n\t" \
    "lw t0, %c[pc](t0)\n\t" \
    "bne t0, t2, 1f\n\t" \
    "2: lw t0, %c[devices](a0)\n\t" \
    "lhu t2, %c[bus](t0)\n\t" \
    "jalr zero, s3, 0\n\t" \
    "1: "
#define OPEN_BUS_OFFSETS \
    [devices] "i" (offsetof(struct Gbn, devices)), \
    [access] "i" (offsetof(struct GbnDevices, dma_access)), \
    [valid] "i" (offsetof(struct GbnDevices, dma_bus_valid)), \
    [pc] "i" (offsetof(struct GbnDevices, dma_pc)), \
    [bus] "i" (offsetof(struct GbnDevices, dma_bus))
__attribute__((naked, noinline, used)) static void open_bus_literal(void) {
    __asm__ volatile(OPEN_BUS_DMA_SELECT "mv t2, t1\n\tjalr zero, s3, 0" : : OPEN_BUS_OFFSETS);
}
__attribute__((naked, noinline, used)) static void open_bus_ram(void) {
    __asm__ volatile(OPEN_BUS_DMA_SELECT
        "lbu t2, 0(t1)\n\tlbu t0, 1(t1)\n\tslli t0, t0, 8\n\tor t2, t2, t0\n\tjalr zero, s3, 0"
        : : OPEN_BUS_OFFSETS);
}
#undef OPEN_BUS_DMA_SELECT
#undef OPEN_BUS_OFFSETS

/* Each IO register exposes the low open-bus halfword. ROM fetch values may
 * be folded, while RAM bytes beyond the block snapshot must remain live.
 * The leaf preserves TEMP for odd byte lanes and uses WAIT only as its link. */
static void open_bus_value(struct Emit* e, const struct Gbn* m, uint32_t pc,
                           unsigned rd, unsigned kind, unsigned width,
                           unsigned rn, int adjustment) {
    /* At a warm C entry the omitted IWRAM tail can still be prefetched
     * from before an external write. Reject mismatched bytes before any
     * writeback, preserving that old pipeline for the interpreter. A chain
     * entry with freshly fetched bytes is also safe under this guard. */
    bool previous=e->block->thumb && m->code_region==3 && (pc&2) && !e->block->instructions;
    if(previous) {
        uintptr_t host=(uintptr_t)m->code+((pc+2)&m->code_mask);
        constant(e,LHS,(uint32_t)host);
        if(!(host&1))word(e,LHS<<15 | 5<<12 | RESULT<<7 | 3);
        else {
            word(e,LHS<<15 | 4<<12 | RESULT<<7 | 3);
            word(e,1u<<20 | LHS<<15 | 4<<12 | RHS<<7 | 3);
            imm(e,RHS,RHS,1,8);op(e,RESULT,RESULT,RHS,6,0);
        }
        constant(e,RHS,fetch(m,pc+2,2));stop(e,RESULT,RHS,1,true);
    }
    writeback(e, rn, adjustment);
    unsigned fetch_width=e->block->thumb ? 2 : 4;
    uint32_t address=pc+2*fetch_width;
    if(e->block->thumb && m->code_region==3 && (pc&2))address=pc+2;
    if(m->code_region>=8 || previous) {
        constant(e,RHS,fetch(m,address,fetch_width)&65535);
        constant(e,WAIT,(uint32_t)(uintptr_t)open_bus_literal);
    } else {
        constant(e,RHS,(uint32_t)(uintptr_t)(m->code+(address&m->code_mask)));
        constant(e,WAIT,(uint32_t)(uintptr_t)open_bus_ram);
    }
    constant(e,RESULT,pc+fetch_width);
    word(e,WAIT<<15 | WAIT<<7 | 0x67); /* jalr s3,s3,0; RA stays intact */
    if(width==4) { imm(e,RHS,RESULT,1,16);op(e,RESULT,RESULT,RHS,6,0); }
    else if(width==1) {
        imm(e,RHS,TEMP,7,1);imm(e,RHS,RHS,1,3);
        op(e,RESULT,RESULT,RHS,5,0);imm(e,RESULT,RESULT,7,255);
    }
    if(kind==3 || kind==7) {
        unsigned shift=kind==3 ? 24 : 16;
        imm(e,RESULT,RESULT,1,(int)shift);imm(e,RESULT,RESULT,5,(int)(0x400|shift));
    }
    put(e,rd);
}

static void direct_access(struct Emit* e, const struct Gbn* m, uint32_t pc, unsigned rd, unsigned kind, unsigned rn, int adjustment) {
	if (kind <= 2) e->read_only = false;
	else e->read_seen = true;
	static const unsigned widths[] = {4, 2, 1, 1, 4, 2, 1, 2};
	unsigned width = widths[kind];
	imm(e, TEMP, LHS, 0, 0); /* original address */
	imm(e, RHS, LHS, 5, 24); imm(e, RHS, RHS, 0, -2);
	imm(e, RESULT, RHS, 3, 2); /* region 2 or 3 */
	unsigned cartridge = 0;
	if (kind > 2 || width == 4) { cartridge = e->count; word(e, 0); }
	else guard(e, RESULT, true);
	if (kind <= 2 && m->code_region < 8) {
		/* A RAM block may store data in its own RAM bank, but a write near
		 * this block or its prefetched tail must use the interpreter. This
		 * covers code changed during a native self-loop too. Modular distance
		 * catches mirrored addresses and a prefetch tail wrapping RAM. */
		constant(e, RESULT, m->code_region - 2);
		unsigned other_bank = e->count; word(e, 0);
		unsigned mask = m->code_region == 2 ? GBN_EWRAM_SIZE - 1 : GBN_IWRAM_SIZE - 1;
		constant(e, WAIT, e->block->pc & mask & ~3u);
		op(e, RESULT, TEMP, WAIT, 0, 0x20);
		constant(e, WAIT, mask); op(e, RESULT, RESULT, WAIT, 7, 0);
		imm(e, RESULT, RESULT, 3, (GBN_RV32_INSNS + 2) * (e->block->thumb ? 2 : 4) + (e->block->pc & 3));
		guard(e, RESULT, false);
		e->block->code[other_bank] = beq(RHS, RESULT, 4 * (e->count - other_bank)) | 1u << 12;
		/* RESULT was the bank discriminator at the branch, before clobbering. */
	}
	load(e, RESULT, offsetof(struct Gbn, ewram)); constant(e, LHS, GBN_EWRAM_SIZE - 1);
	constant(e, WAIT, (m->ewram_wait + 1u) * (width == 4 ? 2u : 1u) + (kind > 2));
	unsigned choose = e->count; word(e, 0);
	load(e, RESULT, offsetof(struct Gbn, iwram)); constant(e, LHS, GBN_IWRAM_SIZE - 1);
	constant(e, WAIT, 1u + (kind > 2));
	e->block->code[choose] = beq(RHS, ZERO, 4 * (e->count - choose));
	guard(e, RESULT, true); /* absent RAM */
	if (width > 1) {
		/* Caller-provided RAM need not be host aligned. Bytewise C accesses
		 * remain the fallback for either kind of misalignment. */
		op(e, RHS, RESULT, TEMP, 6, 0); imm(e, RHS, RHS, 7, (int) width - 1);
		guard(e, RHS, false);
	}
	op(e, LHS, TEMP, LHS, 7, 0); op(e, LHS, LHS, RESULT, 0, 0);
	access_value(e, pc, rd, kind, width, rn, adjustment);
	ram_wait(e, m, pc);
	if (kind <= 2) {
		if (width == 4) {
			unsigned done = e->count; word(e, 0);
			e->block->code[cartridge] = beq(RESULT, ZERO, 4 * (e->count - cartridge));
			dma_address_store(e, m, pc, rd, rn, adjustment);
			e->block->code[done] = beq(ZERO, ZERO, 4 * (e->count - done));
		}
		return;
	}
	unsigned done = e->count; word(e, 0);
	e->block->code[cartridge] = beq(RESULT, ZERO, 4 * (e->count - cartridge));
	imm(e, RHS, TEMP, 5, 24); imm(e, RHS, RHS, 0, -8);
	imm(e, RESULT, RHS, 3, 6);
	unsigned not_cartridge = e->count; word(e, 0);
	if (width == 2) {
		/* WS2 halfword reads may be EEPROM transactions. Keep the complete
		 * window on the bus path, including changes of attached save type. */
		constant(e, RESULT, 5); stop(e, RHS, RESULT, 0, true);
	}
	load(e, RESULT, offsetof(struct Gbn, rom)); guard(e, RESULT, true);
	if (width > 1) {
		op(e, RHS, RESULT, TEMP, 6, 0); imm(e, RHS, RHS, 7, (int) width - 1); guard(e, RHS, false);
	}
	constant(e, LHS, GBN_ROM_MAX_SIZE - 1); op(e, LHS, TEMP, LHS, 7, 0);
	load(e, RHS, offsetof(struct Gbn, rom_size)); imm(e, WAIT, LHS, 0, (int) width);
	op(e, RHS, RHS, WAIT, 3, 0); guard(e, RHS, false); /* ROM bounds; open bus stays in C. */
	op(e, LHS, LHS, RESULT, 0, 0);
	/* Data and instruction fetches may use different ROM wait windows.
	 * Read the data window waits at runtime so cached blocks need no extra
	 * invalidation when another window's WAITCNT fields change. */
	imm(e, RHS, TEMP, 5, 25); imm(e, RHS, RHS, 0, -4); op(e, RHS, MACHINE, RHS, 0, 0);
	word(e, offsetof(struct Gbn, rom_nonseq) << 20 | RHS << 15 | 4 << 12 | WAIT << 7 | 3);
	if (width == 4) {
		word(e, offsetof(struct Gbn, rom_seq) << 20 | RHS << 15 | 4 << 12 | RESULT << 7 | 3);
		op(e, WAIT, WAIT, RESULT, 0, 0);
	}
	imm(e, WAIT, WAIT, 0, width == 4 ? 3 : 2);
	access_value(e, pc, rd, kind, width, rn, adjustment);
	imm(e, NOW, NOW, 0, (int) m->code_nonseq - (int) m->code_wait);
	op(e, NOW, NOW, WAIT, 0, 0);

	unsigned rom_done = e->count; word(e, 0);
	e->block->code[not_cartridge] = beq(RESULT, ZERO, 4 * (e->count - not_cartridge));
	imm(e, RHS, TEMP, 5, 24); imm(e, RHS, RHS, 0, -4);
	guard(e, RHS, false);
	/* Reads of stored IO state need no C call. Events are checked before
	 * every guest instruction, so LCD/IRQ/key state is current at this read. */
	constant(e, LHS, 0x04000000u); op(e, LHS, TEMP, LHS, 0, 0x20);
	imm(e, RESULT, LHS, 3, (int) sizeof(native_io_readable) * 2); guard(e, RESULT, true);
	if (width > 1) { imm(e, RHS, TEMP, 7, (int) width - 1); guard(e, RHS, false); }
	imm(e, RHS, LHS, 5, 1); constant(e, RESULT, (uint32_t) (uintptr_t) native_io_readable);
	op(e, RHS, RHS, RESULT, 0, 0);
	word(e, RHS << 15 | 4 << 12 | RHS << 7 | 3);
	imm(e, RHS, RHS, 7, width == 4 ? 42 : 21); guard(e, RHS, true);
	load(e, RESULT, offsetof(struct Gbn, devices)); guard(e, RESULT, true);
	op(e, LHS, LHS, RESULT, 0, 0);
	imm(e, WAIT, RHS, 7, width == 4 ? 32 : 16);
	unsigned open_bus = e->count; word(e, 0);
	imm(e, RHS, RHS, 7, width == 4 ? 8 : 4);
	unsigned zero_count = e->count; word(e, 0);
	access_value(e, pc, rd, kind, width, rn, adjustment);
	unsigned io_done = e->count; word(e, 0);
	e->block->code[zero_count] = beq(RHS, ZERO, 4 * (e->count - zero_count)) | 1u << 12;
	writeback(e, rn, adjustment);
	if (width == 4) {
		word(e, LHS << 15 | 2 << 12 | RESULT << 7 | 3);
		imm(e, RESULT, RESULT, 5, 16); imm(e, RESULT, RESULT, 1, 16);
	} else constant(e, RESULT, 0);
	put(e, rd);
    unsigned zero_done = e->count; word(e, 0);
    e->block->code[open_bus] = beq(WAIT, ZERO, 4 * (e->count - open_bus)) | 1u << 12;
    open_bus_value(e, m, pc, rd, kind, width, rn, adjustment);
    e->block->code[zero_done] = beq(ZERO, ZERO, 4 * (e->count - zero_done));
	e->block->code[io_done] = beq(ZERO, ZERO, 4 * (e->count - io_done));
	constant(e, WAIT, 2); ram_wait(e, m, pc);
	e->block->code[done] = beq(ZERO, ZERO, 4 * (e->count - done));
	e->block->code[rom_done] = beq(ZERO, ZERO, 4 * (e->count - rom_done));
}

/* Direct RAM operations stay in this block, retaining the live registers and
 * prefetch/time state. Guards precede all guest effects and exit at this
 * instruction for IO, unaligned or unmapped accesses. */
static bool memory(struct Emit* e, const struct Gbn* m, unsigned code, uint32_t pc) {
	unsigned rd = code & 7, rs = code >> 3 & 7, kind;
	if ((code & 0xf800) == 0x4800) {
		e->read_seen = true;
		kind = 4; rd = code >> 8 & 7;
		uint32_t address = ((pc + 4) & ~3u) + (code & 255) * 4;
		if (m->code_region < 8) {
			constant(e, LHS, address); direct_access(e, m, pc, rd, kind, 15, 0);
			return true;
		}
		uint32_t offset = address & (GBN_ROM_MAX_SIZE - 4);
		if (address >> 24 != m->code_region || offset >= m->rom_size || m->rom_size - offset < 4) return false;
		uint32_t value = m->rom[offset] | (uint32_t) m->rom[offset + 1] << 8 |
			(uint32_t) m->rom[offset + 2] << 16 | (uint32_t) m->rom[offset + 3] << 24;
		constant(e, RESULT, value); put(e, rd);
		/* ROM word read + internal cycle + fetch_break, same ROM window. */
		imm(e, NOW, NOW, 0, 3 + 2 * m->code_nonseq);
		return true;
	} else if ((code & 0xf000) == 0x5000) {
		kind = code >> 9 & 7;
		get(e, LHS, rs, pc); get(e, RHS, code >> 6 & 7, pc); op(e, LHS, LHS, RHS, 0, 0);
	} else if (code >= 0x6000 && code < 0x9000) {
		unsigned scale;
		if (code < 0x8000) { kind = (code & 0x1000 ? 2 : 0) + (code & 0x800 ? 4 : 0); scale = code & 0x1000 ? 1 : 4; }
		else { kind = code & 0x800 ? 5 : 1; scale = 2; }
		get(e, LHS, rs, pc); imm(e, LHS, LHS, 0, (int) (code >> 6 & 31) * (int) scale);
	} else if ((code & 0xf000) == 0x9000) {
		kind = code & 0x800 ? 4 : 0; rd = code >> 8 & 7;
		get(e, LHS, 13, pc); imm(e, LHS, LHS, 0, (int) (code & 255) * 4);
	} else return false;
	direct_access(e, m, pc, rd, kind, 15, 0);
	return true;
}

/* Stack transfers use one RAM mapping and one aggregate bus stall, as the
 * interpreter's multiple() does. Validate the whole span before committing
 * SP, low registers or memory. Rare mirrored-end spans use the bus path. */
static bool thumb_stack(struct Emit* e, const struct Gbn* m, unsigned code, uint32_t pc) {
	bool pop = (code & 0x800) != 0, extra = (code & 0x100) != 0;
	unsigned list = code & 255, count = extra;
	for (unsigned r = 0; r < 8; ++r) count += list >> r & 1;
	if (!count) return false; /* ARM7 empty-list PC/writeback quirks stay in C. */
	unsigned bytes = 4 * count;
	if (pop) e->read_seen = true;
	else e->read_only = false;
	get(e, TEMP, 13, pc);
	imm(e, LHS, TEMP, 0, pop ? 0 : -(int) bytes);
	imm(e, RHS, LHS, 5, 24); imm(e, RHS, RHS, 0, -2);
	imm(e, RESULT, RHS, 3, 2); guard(e, RESULT, true);
	if (!pop && m->code_region < 8) {
		constant(e, RESULT, m->code_region - 2);
		unsigned other_bank = e->count; word(e, 0);
		unsigned mask = m->code_region == 2 ? GBN_EWRAM_SIZE - 1 : GBN_IWRAM_SIZE - 1;
		imm(e, RESULT, LHS, 7, -4);
		constant(e, WAIT, (e->block->pc & mask & ~3u) - (bytes - 4));
		op(e, RESULT, RESULT, WAIT, 0, 0x20);
		constant(e, WAIT, mask); op(e, RESULT, RESULT, WAIT, 7, 0);
		imm(e, RESULT, RESULT, 3, (GBN_RV32_INSNS + 2) * 2 + (e->block->pc & 3) + bytes - 4);
		guard(e, RESULT, false);
		e->block->code[other_bank] = beq(RHS, RESULT, 4 * (e->count - other_bank)) | 1u << 12;
	}
	load(e, RESULT, offsetof(struct Gbn, ewram)); constant(e, WAIT, GBN_EWRAM_SIZE - 1);
	constant(e, LHS, count * 2 * (m->ewram_wait + 1u) + pop);
	unsigned choose = e->count; word(e, 0);
	load(e, RESULT, offsetof(struct Gbn, iwram)); constant(e, WAIT, GBN_IWRAM_SIZE - 1);
	constant(e, LHS, count + pop);
	e->block->code[choose] = beq(RHS, ZERO, 4 * (e->count - choose));
	guard(e, RESULT, true);
	imm(e, RHS, RESULT, 7, 3); guard(e, RHS, false);
	/* Multiple transfers align data addresses down, but preserve SP's low
 * bits in writeback. Caller-provided host RAM may still be unaligned. */
	imm(e, RHS, TEMP, 0, pop ? 0 : -(int) bytes);
	imm(e, RHS, RHS, 7, -4); op(e, RHS, RHS, WAIT, 7, 0);
	imm(e, WAIT, WAIT, 0, -(int) (bytes - 1));
	op(e, WAIT, WAIT, RHS, 3, 0); guard(e, WAIT, false);
	op(e, RHS, RHS, RESULT, 0, 0);
	if (pop && extra) {
		/* Same-region returns keep the current fetch mapping. Check the raw
 * target before changing state; other regions and invalid targets fall back. */
		word(e, (bytes - 4) << 20 | RHS << 15 | 2 << 12 | RESULT << 7 | 3);
		imm(e, WAIT, RESULT, 5, 24); imm(e, WAIT, WAIT, 0, -(int) m->code_region);
		guard(e, WAIT, false);
		if (m->code_region >= 8) {
			constant(e, WAIT, GBN_ROM_MAX_SIZE - 1); op(e, WAIT, RESULT, WAIT, 7, 0);
			constant(e, TEMP, m->rom_size); stop(e, WAIT, TEMP, 7, true);
			get(e, TEMP, 13, pc);
		}
		imm(e, RESULT, RESULT, 7, -2);
		word(e, (STACK_TARGET >> 5) << 25 | RESULT << 20 | SP << 15 | 2 << 12 | (STACK_TARGET & 31) << 7 | 0x23);
		e->dynamic_pc = true;
	}
	imm(e, TEMP, TEMP, 0, pop ? (int) bytes : -(int) bytes); put_value(e, 13, TEMP);
	unsigned offset = 0;
	for (unsigned r = 0; r < 8; ++r) if (list & (1u << r)) {
		if (pop) {
			word(e, offset << 20 | RHS << 15 | 2 << 12 | guest[r] << 7 | 3);
			e->written |= 1u << r;
		} else word(e, (offset >> 5) << 25 | guest[r] << 20 | RHS << 15 | 2 << 12 | (offset & 31) << 7 | 0x23);
		offset += 4;
	}
	if (!pop && extra) {
		get(e, RESULT, 14, pc);
		word(e, (offset >> 5) << 25 | RESULT << 20 | RHS << 15 | 2 << 12 | (offset & 31) << 7 | 0x23);
	}
	imm(e, WAIT, LHS, 0, 0); ram_wait(e, m, pc);
	return true;
}

/* RESULT holds the raw target. Validate before committing LR or any guest
 * state; other regions use the interpreter's complete mapping transition. */
static void indirect_target(struct Emit* e, const struct Gbn* m) {
    imm(e, WAIT, RESULT, 5, 24);
    imm(e, WAIT, WAIT, 0, -(int) m->code_region); guard(e, WAIT, false);
    if (m->code_region >= 8) {
        constant(e, WAIT, GBN_ROM_MAX_SIZE - 1); op(e, WAIT, RESULT, WAIT, 7, 0);
        constant(e, TEMP, m->rom_size); stop(e, WAIT, TEMP, 7, true);
    }
    imm(e, RESULT, RESULT, 7, -2);
    word(e, (STACK_TARGET >> 5) << 25 | RESULT << 20 | SP << 15 | 2 << 12 | (STACK_TARGET & 31) << 7 | 0x23);
    e->dynamic_pc = true;
}

/* False means a C fallback begins here, before any native state change. */
static bool instruction(struct Emit* e, const struct Gbn* m, unsigned code, uint32_t pc) {
	unsigned rd = code & 7, rs = code >> 3 & 7;
	if (code < 0x1800) {
		unsigned kind = code >> 11, amount = code >> 6 & 31;
		unsigned a = source(e, LHS, rs, pc);
		if (!amount && kind) amount = 32;
		if (!amount) imm(e, RESULT, a, 0, 0);
		else {
			imm(e, RHS, a, 5, (int) (kind ? amount - 1 : 32 - amount));
			imm(e, RHS, RHS, 7, 1); imm(e, RHS, RHS, 1, 29);
			constant(e, TEMP, ~0x20000000u); op(e, FLAGS, FLAGS, TEMP, 7, 0);
			op(e, FLAGS, FLAGS, RHS, 6, 0);
			if (amount == 32 && kind == 1) imm(e, RESULT, ZERO, 0, 0);
			else imm(e, RESULT, a, kind ? 5 : 1, (int) (amount == 32 ? 31 : amount) | (kind == 2 ? 0x400 : 0));
		}
		nz(e); put(e, rd);
	} else if (code < 0x2000) {
		unsigned a = source(e, LHS, rs, pc), b;
		if (code & 0x400) { constant(e, RHS, code >> 6 & 7); b = RHS; }
		else b = source(e, RHS, code >> 6 & 7, pc);
		arithmetic(e, (code & 0x200) != 0, a, b); put(e, rd);
	} else if (code < 0x4000) {
		rd = code >> 8 & 7;
		unsigned kind = code >> 11 & 3;
		if (!kind) { constant(e, RESULT, code & 255); nz(e); }
		else { unsigned a = source(e, LHS, rd, pc); constant(e, RHS, code & 255); arithmetic(e, kind != 2, a, RHS); }
		if (kind != 1) put(e, rd);
	} else if (code < 0x4400) {
		unsigned kind = code >> 6 & 15;
		if ((kind >= 2 && kind <= 4) || kind == 7) return false;
		unsigned a = source(e, LHS, rd, pc), b = source(e, RHS, rs, pc);
		if (kind == 13) {
			/* Thumb MUL timing uses the old destination as multiplier. */
			multiply_wait(e, a, false); op(e, RESULT, a, b, 0, 1);
			nz(e); put(e, rd); ram_wait(e, m, pc);
			return true;
		}
		if (kind == 5 || kind == 6) arithmetic_carry(e, kind == 6, a, b);
		else if (kind == 9) arithmetic(e, true, ZERO, b);
		else if (kind == 10 || kind == 11) arithmetic(e, kind == 10, a, b);
		else {
			if (kind == 14 || kind == 15) { imm(e, RHS, b, 4, -1); b = RHS; }
			if (kind == 15) imm(e, RESULT, b, 0, 0);
			else op(e, RESULT, a, b, kind == 1 ? 4 : kind == 12 ? 6 : 7, 0);
			nz(e);
		}
		if (kind != 8 && kind != 10 && kind != 11) put(e, rd);
	} else if (code < 0x4700) {
		rd |= code >> 4 & 8; rs = code >> 3 & 15;
		unsigned kind = code >> 8 & 3;
		if (rd == 15 && kind != 1) return false;
		unsigned b = source(e, RHS, rs, pc);
		if (kind == 1) { unsigned a = source(e, LHS, rd, pc); arithmetic(e, true, a, b); }
		else if (kind == 0) {
			unsigned a = source(e, LHS, rd, pc); op(e, RESULT, a, b, 0, 0);
			put(e, rd);
		} else put_value(e, rd, b);
    } else if (code >= 0x4700 && code < 0x4780) {
        get(e, RESULT, code >> 3 & 15, pc);
        imm(e, WAIT, RESULT, 7, 1); guard(e, WAIT, true);
        indirect_target(e, m); /* BX remains Thumb only for an odd target. */
	} else if (code >= 0x4800 && code < 0xa000) {
		return memory(e, m, code, pc);
	} else if ((code & 0xf000) == 0xa000) {
		rd = code >> 8 & 7;
		if (code & 0x800) get(e, LHS, 13, pc);
		else constant(e, LHS, (pc + 4) & ~3u);
		imm(e, RESULT, LHS, 0, (int) (code & 255) * 4); put(e, rd);
	} else if ((code & 0xff00) == 0xb000) {
		get(e, LHS, 13, pc);
		int offset = (int) (code & 127) * 4;
		imm(e, RESULT, LHS, 0, code & 128 ? -offset : offset); put(e, 13);
	} else if ((code & 0xf600) == 0xb400) {
		return thumb_stack(e, m, code, pc);
	} else if ((code & 0xf800) == 0xe000 || ((code & 0xf000) == 0xd000 && (code >> 8 & 15) < 14)) {
		bool unconditional = (code & 0xf800) == 0xe000;
		int32_t offset = unconditional ? (int32_t) (code << 21) >> 20 : (int32_t) (code << 24) >> 23;
		uint32_t target = pc + 4 + (uint32_t) offset;
		if (target >> 24 != m->code_region || (m->code_region >= 8 && (target & (GBN_ROM_MAX_SIZE - 1)) >= m->rom_size)) return false;
		e->block->target = target; e->block->branch = true;
		if (unconditional) constant(e, TEMP, 1);
		else condition(e, code >> 8 & 15);
		imm(e, TEMP, TEMP, 1, 31); op(e, LIMIT, LIMIT, TEMP, 6, 0);
	} else if ((code & 0xf800) == 0xf000) {
        /* BL's first half is a real instruction. Commit LR here so caps,
         * events and an independently entered suffix retain their semantics. */
        int32_t upper = (int32_t) (code << 21) >> 9;
        constant(e, RESULT, pc + 4 + (uint32_t) upper);
        put_value(e, 14, RESULT);
    } else if ((code & 0xf800) == 0xf800 && e->block->instructions &&
               (fetch(m, pc - 2, 2) & 0xf800) == 0xf000) {
        /* Only a prefix actually executed in this native block proves LR.
         * A suffix reached after an event/cap split still uses the bus path. */
        uint32_t prefix = fetch(m, pc - 2, 2);
        int32_t upper = (int32_t) (prefix << 21) >> 9;
        uint32_t target = pc + 2 + (uint32_t) upper + 2 * (code & 2047);
        if (target >> 24 != m->code_region || (m->code_region >= 8 &&
            (target & (GBN_ROM_MAX_SIZE - 1)) >= m->rom_size)) return false;
        constant(e, RESULT, (pc + 2) | 1); put_value(e, 14, RESULT);
        e->block->target = target; e->block->branch = true;
        constant(e, TEMP, 0x80000000u); op(e, LIMIT, LIMIT, TEMP, 6, 0);
    } else if ((code & 0xf800) == 0xf800) {
        /* Independently entered suffix: LR may have changed after an event. */
        get(e, RESULT, 14, pc);
        constant(e, WAIT, 2 * (code & 2047)); op(e, RESULT, RESULT, WAIT, 0, 0);
        indirect_target(e, m);
        constant(e, RESULT, (pc + 2) | 1); put_value(e, 14, RESULT);
    } else return false;
	return true;
}

/* ARM immediate shifts: RHS is the value, WAIT the carry output. Keeping
 * CPSR untouched also permits address shifts and ALU operations without S. */
static void arm_shift_imm(struct Emit* e, unsigned kind, unsigned amount) {
	imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1);
	if (!kind && !amount) return;
	if (kind == 3 && !amount) {
		imm(e, TEMP, WAIT, 1, 31); imm(e, WAIT, RHS, 7, 1);
		imm(e, RHS, RHS, 5, 1); op(e, RHS, RHS, TEMP, 6, 0);
	} else if (kind == 3) {
		imm(e, TEMP, RHS, 1, (int) (32 - amount));
		imm(e, RHS, RHS, 5, (int) amount); op(e, RHS, RHS, TEMP, 6, 0);
		imm(e, WAIT, RHS, 5, 31);
	} else {
		if (!amount) amount = 32;
		imm(e, WAIT, RHS, 5, (int) (kind ? amount - 1 : 32 - amount));
		imm(e, WAIT, WAIT, 7, 1);
		if (kind == 1 && amount == 32) constant(e, RHS, 0);
		else imm(e, RHS, RHS, kind ? 5 : 1,
			(int) (amount == 32 ? 31 : amount) | (kind == 2 ? 0x400 : 0));
	}
}

static bool arm_memory(struct Emit* e, const struct Gbn* m, uint32_t code, uint32_t pc, bool half) {
	unsigned rn = code >> 16 & 15, rd = code >> 12 & 15, form = code >> 5 & 3;
	bool pre = (code & 0x1000000) != 0, load_value = (code & 0x100000) != 0;
	bool wb = !pre || (code & 0x200000) != 0;
	bool immediate = half ? (code & 0x400000) != 0 : !(code & 0x2000000);
	if (rd == 15 || (wb && rn == 15) || (!pre && !immediate)) return false;
	if (half && (!form || (!load_value && form != 1))) return false;
	if (!half && (code & 0x2000010) == 0x2000010) return false;
	unsigned offset = half ? (code >> 4 & 0xf0) | (code & 15) : code & 4095;
	if (pre) {
		if (immediate) constant(e, RHS, offset);
		else {
			get(e, RHS, code & 15, pc);
			if (!half) arm_shift_imm(e, code >> 5 & 3, code >> 7 & 31);
		}
		get(e, LHS, rn, pc); op(e, LHS, LHS, RHS, 0, code & 0x800000 ? 0 : 0x20);
	} else get(e, LHS, rn, pc);
	unsigned kind = half ? (load_value ? form == 1 ? 5 : form == 2 ? 3 : 7 : 1) :
		(code & 0x400000 ? 2 : 0) + (load_value ? 4 : 0);
	int adjustment = pre ? 0 : code & 0x800000 ? (int) offset : -(int) offset;
	direct_access(e, m, pc, rd, kind, wb ? rn : 15, adjustment);
	return true;
}

static bool arm_instruction(struct Emit* e, const struct Gbn* m, uint32_t code, uint32_t pc) {
	unsigned cond = code >> 28;
	if (cond == 15) return true; /* ARMv4 NV never executes, including undefined opcodes. */
	unsigned skip = 0;
	if (cond < 14) { condition(e, cond); skip = e->count; word(e, 0); }
	if ((code & 0x0e000000) == 0x0a000000) {
		uint32_t target = pc + 8 + (uint32_t) ((int32_t) (code << 8) >> 6);
		if (target >> 24 != m->code_region || (m->code_region >= 8 && (target & (GBN_ROM_MAX_SIZE - 1)) >= m->rom_size)) return false;
		e->block->target = target; e->block->branch = true;
		if (code & 0x1000000) { constant(e, RESULT, pc + 4); put(e, 14); }
		constant(e, TEMP, 0x80000000u); op(e, LIMIT, LIMIT, TEMP, 6, 0);
	} else if ((code & 0x0fc000f0) == 0x00000090) {
		unsigned rd = code >> 16 & 15, rn = code >> 12 & 15;
		bool accumulate = (code & 0x200000) != 0;
		if (rd != 15 && (!accumulate || rn != 15)) {
			get(e, LHS, code & 15, pc); get(e, RHS, code >> 8 & 15, pc);
			multiply_wait(e, RHS, accumulate); op(e, RESULT, LHS, RHS, 0, 1);
			if (accumulate) { get(e, TEMP, rn, pc); op(e, RESULT, RESULT, TEMP, 0, 0); }
			put(e, rd);
			if (code & 0x100000) {
				nz(e); load(e, TEMP, offsetof(struct Gbn, cpu.shifter_carry));
				op(e, TEMP, ZERO, TEMP, 3, 0); imm(e, TEMP, TEMP, 1, 29);
				constant(e, LHS, ~0x20000000u); op(e, FLAGS, FLAGS, LHS, 7, 0);
				op(e, FLAGS, FLAGS, TEMP, 6, 0);
			}
			ram_wait(e, m, pc);
		}
	} else if (((code & 0x0e000090) == 0x00000090 && (code & 0x60)) ||
	           (code & 0x0c000000) == 0x04000000) {
		if (!arm_memory(e, m, code, pc, (code & 0x0c000000) == 0)) return false;
	} else if ((code & 0x0c000000) == 0) {
		unsigned kind = code >> 21 & 15, rd = code >> 12 & 15, rn = code >> 16 & 15;
		bool set = (code & 0x100000) != 0, test = kind >= 8 && kind <= 11;
		/* PSR transfers, interworking and register-controlled shifts
		 * retain the interpreter's banking and exception semantics for now. */
		if ((test && !set) || rd == 15 || (!(code & 0x2000000) && (code & 16))) return false;
		if (code & 0x2000000) {
			unsigned rotation = code >> 7 & 30;
			uint32_t value = code & 255;
			if (rotation) value = value >> rotation | value << (32 - rotation);
			constant(e, RHS, value);
			if (rotation) constant(e, WAIT, value >> 31);
			else { imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1); }
		} else {
			get(e, RHS, code & 15, pc);
			arm_shift_imm(e, code >> 5 & 3, code >> 7 & 31);
		}
		store(e, WAIT, offsetof(struct Gbn, cpu.shifter_carry));
		get(e, LHS, rn, pc);
		bool arithmetic_op = (kind >= 2 && kind <= 7) || kind == 10 || kind == 11;
		if (arithmetic_op) {
			bool reverse = kind == 3 || kind == 7;
			bool subtract = kind == 2 || kind == 3 || kind == 6 || kind == 7 || kind == 10;
			bool carry_in = kind >= 5 && kind <= 7;
			if (reverse) { imm(e, TEMP, LHS, 0, 0); imm(e, LHS, RHS, 0, 0); imm(e, RHS, TEMP, 0, 0); }
			if (set) {
				if (carry_in) arithmetic_carry(e, subtract, LHS, RHS); else arithmetic(e, subtract, LHS, RHS);
			} else {
				op(e, RESULT, LHS, RHS, 0, subtract ? 0x20 : 0);
				if (carry_in) {
					imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1);
					if (subtract) imm(e, WAIT, WAIT, 4, 1);
					op(e, RESULT, RESULT, WAIT, 0, subtract ? 0x20 : 0);
				}
			}
		} else {
			if (kind == 14 || kind == 15) imm(e, RHS, RHS, 4, -1);
			if (kind == 13 || kind == 15) imm(e, RESULT, RHS, 0, 0);
			else op(e, RESULT, LHS, RHS, kind == 1 || kind == 9 ? 4 : kind == 12 ? 6 : 7, 0);
			if (set) {
				constant(e, TEMP, ~0x20000000u); op(e, FLAGS, FLAGS, TEMP, 7, 0);
				imm(e, WAIT, WAIT, 1, 29); op(e, FLAGS, FLAGS, WAIT, 6, 0); nz(e);
			}
		}
		if (!test) put(e, rd);
	} else return false;
	if (skip) e->block->code[skip] = beq(TEMP, ZERO, 4 * (e->count - skip));
	return true;
}

static uint32_t timing(const struct Gbn* m) {
	return m->code_wait | (uint32_t) m->code_nonseq << 8 | (uint32_t) m->ewram_wait << 16 |
		(uint32_t) !!(m->waitcnt & 0x4000) << 24 | (uint32_t) !!(m->cpu.cpsr & 0x20) << 25;
}

static unsigned block_index(uint32_t pc) {
	/* Mix the bank and upper cartridge bits as well as the local address.
	 * ROM and RAM entries at the same low offset must not always collide. */
	return ((pc >> 1) * 0x9e3779b1u) >> GBN_RV32_HASH_SHIFT;
}

static void stack(struct Emit* e, unsigned r, unsigned offset, bool save) {
	word(e, save ? (offset >> 5) << 25 | r << 20 | SP << 15 | 2 << 12 | (offset & 31) << 7 | 0x23 :
		offset << 20 | SP << 15 | 2 << 12 | r << 7 | 3);
}

static bool mergeable(const struct Emit* e) {
	/* Avoid snapshot overhead on ordinary arithmetic counter loops. Reads
	 * distinguish polling candidates; a register-free idle branch also fits. */
	return e->block->thumb && e->block->loop && e->read_only &&
		(e->read_seen || !e->written) && !(e->written & ~255u);
}

/* A read-only Thumb self-loop is deterministic between scheduled events.
 * Compare every modified register, CPSR and prefetch state with its entry.
 * Equality proves a fixed point: repeating the body cannot change any guest
 * state except time and the retired-instruction count. All native loads are
 * ordinary RAM/ROM or pure IO (stored state/open bus); dynamic/side-effectful reads exit before
 * this tail. Batch only complete iterations ending no later than the next
 * event, then let the ordinary per-instruction guards handle the remainder. */
static void merge_loop(struct Emit* e, struct GbnRv32* backend) {
	unsigned mismatches[10], n = 0;
	for (unsigned i = 0; i < 8; ++i) if (e->written & (1u << i)) {
		stack(e, TEMP, 20 + 4 * i, false);
		mismatches[n++] = e->count; word(e, beq(guest[i], TEMP, 0) | 1u << 12);
	}
	stack(e, TEMP, SNAPSHOT_FLAGS, false);
	packed_flags(e, RHS);
	mismatches[n++] = e->count; word(e, beq(RHS, TEMP, 0) | 1u << 12);
	stack(e, TEMP, SNAPSHOT_PREFETCH, false);
	mismatches[n++] = e->count; word(e, beq(PREFETCH, TEMP, 0) | 1u << 12);
	stack(e, TEMP, SNAPSHOT_NOW, false); op(e, TEMP, NOW, TEMP, 0, 0x20);
	op(e, RHS, DEADLINE, NOW, 0, 0x20); op(e, RHS, RHS, TEMP, 5, 1); /* divu: full time intervals */
	constant(e, WAIT, e->block->instructions); op(e, LHS, LIMIT, WAIT, 5, 1);
	word(e, beq(LHS, RHS, 8) | 6u << 12); /* bltu cap,time,+8 */
	imm(e, LHS, RHS, 0, 0);
	op(e, TEMP, LHS, TEMP, 0, 1); op(e, NOW, NOW, TEMP, 0, 0);
	op(e, RESULT, LHS, WAIT, 0, 1);
	op(e, COMPLETED, COMPLETED, RESULT, 0, 0); op(e, LIMIT, LIMIT, RESULT, 0, 0x20);
#if GBN_RV32_STATS
	constant(e, LHS, (uint32_t) (uintptr_t) &backend->merged_instructions);
	word(e, LHS << 15 | 2 << 12 | TEMP << 7 | 3);
	op(e, RESULT, RESULT, TEMP, 0, 0); op(e, WAIT, RESULT, TEMP, 3, 0);
	word(e, RESULT << 20 | LHS << 15 | 2 << 12 | 0x23);
	word(e, 4u << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
	op(e, TEMP, TEMP, WAIT, 0, 0);
	word(e, TEMP << 20 | LHS << 15 | 2 << 12 | 4u << 7 | 0x23);
#else
	(void) backend;
#endif
	for (unsigned i = 0; i < n; ++i)
		e->block->code[mismatches[i]] |= beq(0, 0, 4 * (e->count - mismatches[i])) & 0xfe000f80u;
}

/* All native entries share a register layout and one stack frame. A warm
 * successor receives the live register file, remaining cap and deadline. The
 * cache descriptor is checked at the edge, so replacing a slot or recycling
 * the arena needs no executable-code patching or reverse-link maintenance. */
/* Dynamic successors use the same keyed descriptors and RAM snapshot entry
 * as direct edges. STACK_TARGET preserves the guest PC through all scratch
 * registers and miss paths; no new cache, write barrier or mutable code. */
static void indirect_tail(struct Emit* e, struct GbnRv32* backend) {
    struct GbnRv32Block* b = e->block;
    imm(e, COMPLETED, COMPLETED, 0, b->instructions);
    imm(e, LIMIT, LIMIT, 0, -(int) b->instructions);
    unsigned misses[8], n = 0;
    misses[n++] = e->count; word(e, beq(LIMIT, ZERO, 0));
    op(e, TEMP, NOW, DEADLINE, 0, 0x20);
    misses[n++] = e->count; word(e, beq(TEMP, ZERO, 0) | 5u << 12);
    stack(e, RESULT, STACK_TARGET, false);
    imm(e, TEMP, RESULT, 5, 1); constant(e, WAIT, 0x9e3779b1u);
    op(e, TEMP, TEMP, WAIT, 0, 1); imm(e, TEMP, TEMP, 5, GBN_RV32_HASH_SHIFT);
    imm(e, TEMP, TEMP, 1, 2);
    constant(e, LHS, (uint32_t) (uintptr_t) backend->lookup);
    op(e, LHS, LHS, TEMP, 0, 0);
    word(e, LHS << 15 | 2 << 12 | LHS << 7 | 3);
    misses[n++] = e->count; word(e, beq(LHS, ZERO, 0));
    word(e, offsetof(struct GbnRv32Block, chain) << 20 | LHS << 15 | 2 << 12 | RHS << 7 | 3);
    misses[n++] = e->count; word(e, beq(RHS, ZERO, 0));
    word(e, offsetof(struct GbnRv32Block, pc) << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
    misses[n++] = e->count; word(e, beq(TEMP, RESULT, 0) | 1u << 12);
    const unsigned offsets[] = {offsetof(struct GbnRv32Block, generation),
        offsetof(struct GbnRv32Block, mask), offsetof(struct GbnRv32Block, timing)};
    const uint32_t values[] = {b->generation, b->mask, b->timing};
    for (unsigned i = 0; i < 3; ++i) {
        word(e, offsets[i] << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
        constant(e, WAIT, values[i]);
        misses[n++] = e->count; word(e, beq(TEMP, WAIT, 0) | 1u << 12);
    }
#if GBN_RV32_STATS
    constant(e, LHS, (uint32_t) (uintptr_t) &backend->chained_blocks);
    word(e, LHS << 15 | 2 << 12 | TEMP << 7 | 3);
    imm(e, RESULT, TEMP, 0, 1); op(e, WAIT, RESULT, TEMP, 3, 0);
    word(e, RESULT << 20 | LHS << 15 | 2 << 12 | 0x23);
    word(e, 4u << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
    op(e, TEMP, TEMP, WAIT, 0, 0);
    word(e, TEMP << 20 | LHS << 15 | 2 << 12 | 4u << 7 | 0x23);
#endif
    word(e, RHS << 15 | 0x67);
    for (unsigned i = 0; i < n; ++i)
        b->code[misses[i]] |= beq(0, 0, 4 * (e->count - misses[i])) & 0xfe000f80u;
    constant(e, LIMIT, 0); stack(e, RHS, STACK_TARGET, false);
    e->returns[e->return_count++] = e->count; word(e, 0);
}

static void tail(struct Emit* e, struct GbnRv32* backend, uint32_t pc, bool fallback) {
	struct GbnRv32Block* b = e->block;
	imm(e, COMPLETED, COMPLETED, 0, b->instructions);
	imm(e, LIMIT, LIMIT, 0, -(int) b->instructions);
	unsigned misses[10], n = 0;
	if (!fallback && (pc >> 24) == (b->pc >> 24)) {
		misses[n++] = e->count; word(e, beq(LIMIT, ZERO, 0));
		op(e, TEMP, NOW, DEADLINE, 0, 0x20);
		misses[n++] = e->count; word(e, beq(TEMP, ZERO, 0) | 5u << 12);
		if (pc == b->pc) {
			/* Stores overlapping this block already leave through the bus.
			 * A self-loop therefore needs neither another key nor byte check. */
			if (mergeable(e)) {
				merge_loop(e, backend);
				misses[n++] = e->count; word(e, beq(LIMIT, ZERO, 0));
				op(e, TEMP, NOW, DEADLINE, 0, 0x20);
				misses[n++] = e->count; word(e, beq(TEMP, ZERO, 0) | 5u << 12);
			}
			e->loop_jump = e->count; word(e, 0);
		} else {
			unsigned slot = block_index(pc);
			constant(e, LHS, (uint32_t) (uintptr_t) &backend->lookup[slot]);
			word(e, LHS << 15 | 2 << 12 | LHS << 7 | 3);
			misses[n++] = e->count; word(e, beq(LHS, ZERO, 0));
			word(e, offsetof(struct GbnRv32Block, chain) << 20 | LHS << 15 | 2 << 12 | RHS << 7 | 3);
			misses[n++] = e->count; word(e, beq(RHS, ZERO, 0));
			const unsigned offsets[] = {offsetof(struct GbnRv32Block, pc), offsetof(struct GbnRv32Block, generation),
				offsetof(struct GbnRv32Block, mask), offsetof(struct GbnRv32Block, timing)};
			const uint32_t values[] = {pc, b->generation, b->mask, b->timing};
			for (unsigned i = 0; i < 4; ++i) {
				word(e, offsets[i] << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
				constant(e, WAIT, values[i]);
				misses[n++] = e->count; word(e, beq(TEMP, WAIT, 0) | 1u << 12);
			}
#if GBN_RV32_STATS
			constant(e, LHS, (uint32_t) (uintptr_t) &backend->chained_blocks);
			word(e, LHS << 15 | 2 << 12 | TEMP << 7 | 3);
			imm(e, RESULT, TEMP, 0, 1); op(e, WAIT, RESULT, TEMP, 3, 0);
			word(e, RESULT << 20 | LHS << 15 | 2 << 12 | 0x23);
			word(e, 4u << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
			op(e, TEMP, TEMP, WAIT, 0, 0);
			word(e, TEMP << 20 | LHS << 15 | 2 << 12 | 4u << 7 | 0x23);
#endif
			word(e, RHS << 15 | 0x67); /* tail entry: no call, spill or prologue */
		}
	}
	for (unsigned i = 0; i < n; ++i)
		b->code[misses[i]] |= beq(0, 0, 4 * (e->count - misses[i])) & 0xfe000f80u;
	constant(e, LIMIT, fallback ? 0x80000000u : 0);
	constant(e, RHS, pc);
	e->returns[e->return_count++] = e->count; word(e, 0);
}

static void unlink_code(struct GbnRv32* backend, struct GbnRv32Block* b) {
	if (!b->arena_linked) return;
	if (b->arena_prev) b->arena_prev->arena_next = b->arena_next;
	else backend->segments[(b->code - backend->code) / GBN_RV32_SEGMENT_WORDS] = b->arena_next;
	if (b->arena_next) b->arena_next->arena_prev = b->arena_prev;
	b->arena_linked = false;
	b->arena_prev = b->arena_next = NULL;
}

static void allocate_code(struct GbnRv32* backend, struct GbnRv32Block* block, unsigned chain) {
	if (backend->code_used + block->words > (backend->code_segment + 1) * GBN_RV32_SEGMENT_WORDS) {
		backend->code_segment = (backend->code_segment + 1) % GBN_RV32_SEGMENTS;
		backend->code_used = backend->code_segment * GBN_RV32_SEGMENT_WORDS;
		struct GbnRv32Block* old = backend->segments[backend->code_segment];
#if GBN_RV32_STATS
		if (old) ++backend->cache_flushes;
#endif
		/* Only the overwritten segment is retired. Unrelated hot
		 * blocks and their guarded edges survive the arena wrapping. */
		while (old) {
			struct GbnRv32Block* next = old->arena_next;
			old->valid = false; old->chain = NULL; old->arena_linked = false;
			old->arena_prev = old->arena_next = NULL;
			old = next;
		}
		backend->segments[backend->code_segment] = NULL;
	}
	block->code = backend->code + backend->code_used;
	memcpy(block->code, backend->scratch, block->words * sizeof(uint32_t));
	backend->code_used += block->words;
	block->arena_prev = NULL;
	block->arena_next = backend->segments[backend->code_segment];
	if (block->arena_next) block->arena_next->arena_prev = block;
	backend->segments[backend->code_segment] = block;
	block->arena_linked = true;
	__asm__ volatile("fence.i" ::: "memory");
	block->chain = block->code + chain;
}

static void compile(struct GbnRv32* backend, struct GbnRv32Block* block) {
	const struct Gbn* m = backend->machine;
	unlink_code(backend, block);
	block->code = backend->scratch; block->chain = NULL;
	block->pc = m->cpu.pc; block->mask = m->code_mask;
	block->generation = m->rom_generation; block->valid = true;
	block->timing = timing(m);
	block->thumb = (m->cpu.cpsr & 0x20) != 0;
	unsigned width = block->thumb ? 2 : 4;
	block->branch = false; block->fallback = false; block->loop = false; block->instructions = 0;
	block->pipe[0] = fetch(m, block->pc, width); block->pipe[1] = fetch(m, block->pc + width, width);
	struct Emit e = {.block = block, .count = PROLOGUE_WORDS, .read_only = true};
	unsigned ram_guard_words = 0;
	if (m->code_region < 8) {
		uintptr_t address = (uintptr_t) m->code + (block->pc & m->code_mask);
		ram_guard_words = (address & 1) ? 3 * width : (address & 3) ? 2 * width : 4;
	}
	for (unsigned i = 0; i < GBN_RV32_INSNS; ++i) {
		uint32_t pc = block->pc + width * i, offset = pc & m->code_mask;
		unsigned size = m->code_region >= 8 ? m->rom_size : m->code_region == 2 ? GBN_EWRAM_SIZE : GBN_IWRAM_SIZE;
		if (pc >> 24 != m->code_region || offset >= size || size - offset < width) break;
		if (m->code_region < 8 && offset != (block->pc & m->code_mask) + width * i) break;
		/* Reserve one lowering, two guarded successor edges, the common
		 * return, both stop reasons at each instruction and RAM validation. */
		if (e.count + 320 + (10 + ram_guard_words) * (i + 1) >= GBN_RV32_WORDS) break;
		unsigned start = e.count, stops = e.stop_count;
		if (i) {
			constant(&e, TEMP, i); stop(&e, LIMIT, TEMP, 0, false);
			op(&e, TEMP, NOW, DEADLINE, 0, 0x20);
			stop(&e, TEMP, ZERO, 5, false);
		}
		uint32_t code = fetch(m, pc, width);
		if (!(block->thumb ? instruction(&e, m, code, pc) : arm_instruction(&e, m, code, pc))) {
			e.count = start; e.stop_count = stops;
			block->fallback = true; break;
		}
		imm(&e, NOW, NOW, 0, 1 + (block->thumb ? m->code_wait : m->code_word_wait));
		++block->instructions;
		if (block->branch || e.dynamic_pc) break;
	}
	block->loop = block->branch && block->target == block->pc;
	if (e.dynamic_pc) {
		imm(&e, NOW, NOW, 0, 2 + m->code_wait + m->code_nonseq);
		constant(&e, PREFETCH, 0);
        indirect_tail(&e, backend);
	} else if (block->branch) {
		unsigned untaken = e.count; word(&e, 0);
		imm(&e, LIMIT, LIMIT, 1, 1); imm(&e, LIMIT, LIMIT, 5, 1);
		imm(&e, NOW, NOW, 0, 2 + (block->thumb ? m->code_wait + m->code_nonseq : m->code_word_wait + m->code_word_nonseq));
		constant(&e, PREFETCH, 0);
		tail(&e, backend, block->target, false);
		block->code[untaken] = beq(LIMIT, ZERO, 4 * (e.count - untaken)) | 5u << 12;
	}
	if (!e.dynamic_pc) tail(&e, backend, block->pc + width * block->instructions, block->fallback);

	unsigned loop_entry = PROLOGUE_WORDS;
	if (mergeable(&e)) {
		loop_entry = e.count;
		for (unsigned i = 0; i < 8; ++i) if (e.written & (1u << i)) stack(&e, guest[i], 20 + 4 * i, true);
		packed_flags(&e, RHS); stack(&e, RHS, SNAPSHOT_FLAGS, true);
		stack(&e, PREFETCH, SNAPSHOT_PREFETCH, true); stack(&e, NOW, SNAPSHOT_NOW, true);
		word(&e, beq(ZERO, ZERO, 4 * (PROLOGUE_WORDS - e.count)));
	}
	if (e.loop_jump) block->code[e.loop_jump] = beq(ZERO, ZERO, 4 * (loop_entry - e.loop_jump));
	unsigned chain = loop_entry;
	if (m->code_region < 8 && block->instructions) {
		/* A previous native block may have changed this RAM code. Check its
		 * snapshot before consuming any instruction. Host allocations need
		 * not be aligned; pick the largest safe host load for each chunk. */
		chain = e.count;
		const uint8_t* bytes = m->code + (block->pc & m->code_mask);
		constant(&e, LHS, (uint32_t) (uintptr_t) bytes);
		for (unsigned off = 0, size = block->instructions * width; off < size;) {
			uintptr_t address = (uintptr_t) bytes + off;
			unsigned size_left = size - off;
			unsigned chunk = !(address & 3) && size_left >= 4 ? 4 : !(address & 1) && size_left >= 2 ? 2 : 1;
			unsigned fn = chunk == 4 ? 2 : chunk == 2 ? 5 : 4;
			word(&e, off << 20 | LHS << 15 | fn << 12 | RHS << 7 | 3);
			uint32_t value = 0;
			for (unsigned i = 0; i < chunk; ++i) value |= (uint32_t) bytes[off + i] << (8 * i);
			constant(&e, TEMP, value);
			stop(&e, RHS, TEMP, 1, false); e.stops[e.stop_count - 1].instruction = 0;
			off += chunk;
		}
		word(&e, beq(ZERO, ZERO, 4 * (loop_entry - e.count)));
	}
	unsigned epilogue = e.count;
	for (unsigned i = 0; i < 8; ++i) store(&e, guest[i], offsetof(struct Gbn, cpu.r) + 4 * i);
	packed_flags(&e, TEMP); store(&e, TEMP, offsetof(struct Gbn, cpu.cpsr));
	store(&e, NOW, offsetof(struct Gbn, now));
	store(&e, PREFETCH, offsetof(struct Gbn, prefetched_pc));
	op(&e, MACHINE, LIMIT, COMPLETED, 0, 0);
	imm(&e, LIMIT, RHS, 0, 0); /* uint64 return: a0=count/reason, a1=next guest PC */
	stack(&e, NOW, 0, false); stack(&e, DEADLINE, 4, false);
	stack(&e, PREFETCH, 8, false); stack(&e, WAIT, 12, false); stack(&e, COMPLETED, 16, false);
	stack(&e, NEGATIVE, 68, false); stack(&e, IS_ZERO, 72, false);
	imm(&e, SP, SP, 0, FRAME_BYTES); word(&e, RA << 15 | 0x67);
	for (unsigned i = 0; i < e.return_count; ++i)
		block->code[e.returns[i]] = beq(ZERO, ZERO, 4 * (epilogue - e.returns[i]));
	unsigned stubs[GBN_RV32_INSNS][2];
	memset(stubs, 0, sizeof(stubs));
	for (unsigned i = 0; i < e.stop_count; ++i) {
		unsigned index = e.stops[i].instruction, slow = e.stops[i].fallback;
		if (!stubs[index][slow]) {
			stubs[index][slow] = e.count;
			constant(&e, LIMIT, index | (slow ? 0x80000000u : 0));
			constant(&e, RHS, block->pc + width * index);
			word(&e, beq(ZERO, ZERO, 4 * (epilogue - e.count)));
		}
		unsigned at = e.stops[i].at;
		block->code[at] |= beq(0, 0, 4 * (stubs[index][slow] - at)) & 0xfe000f80u;
	}
	block->words = (uint16_t) e.count;
	e.count = 0;
	imm(&e, SP, SP, 0, -FRAME_BYTES);
	stack(&e, NOW, 0, true); stack(&e, DEADLINE, 4, true);
	stack(&e, PREFETCH, 8, true); stack(&e, WAIT, 12, true); stack(&e, COMPLETED, 16, true);
	stack(&e, NEGATIVE, 68, true); stack(&e, IS_ZERO, 72, true);
	constant(&e, COMPLETED, 0);
	load(&e, NOW, offsetof(struct Gbn, now));
	imm(&e, DEADLINE, FLAGS, 0, 0);
	load(&e, PREFETCH, offsetof(struct Gbn, prefetched_pc));
	for (unsigned i = 0; i < 8; ++i) load(&e, guest[i], offsetof(struct Gbn, cpu.r) + 4 * i);
	load(&e, FLAGS, offsetof(struct Gbn, cpu.cpsr));
	imm(&e, NEGATIVE, FLAGS, 5, 31); imm(&e, IS_ZERO, FLAGS, 5, 30);
	imm(&e, IS_ZERO, IS_ZERO, 7, 1);
	imm(&e, FLAGS, FLAGS, 1, 2); imm(&e, FLAGS, FLAGS, 5, 2);
	word(&e, beq(ZERO, ZERO, 4 * (loop_entry - e.count)));
	if (m->code_region < 8) {
		unsigned slot = (unsigned) (block - backend->blocks);
		memcpy(backend->source[slot], m->code + (block->pc & m->code_mask),
			(block->instructions ? block->instructions : 1) * width);
	}
	if (block->instructions) allocate_code(backend, block, chain);
	else block->code = NULL;
#if GBN_RV32_STATS
	++backend->compiled_blocks;
#endif
}

static bool matches(const struct GbnRv32Block* b, const struct Gbn* m, uint32_t key) {
	return b && b->valid && b->pc == m->cpu.pc && b->generation == m->rom_generation &&
		b->mask == m->code_mask && b->timing == key;
}

static inline __attribute__((always_inline)) unsigned execute_mode(struct GbnRv32* backend, uint32_t cap, bool thumb) {
	struct Gbn* m = backend->machine;
	unsigned width = thumb ? 2 : 4;
	bool rom = m->code_region >= 8 && m->code_region <= 13;
	const uint8_t* source = rom ? m->rom : m->code_region == 2 ? m->ewram : m->code_region == 3 ? m->iwram : NULL;
	if (!source || m->code != source || m->cpu.pc >> 24 != m->code_region) return 0;
	unsigned index = block_index(m->cpu.pc), key = timing(m);
	struct GbnRv32Block* b = backend->lookup[index];
	bool hit = matches(b, m, key);
	if (!hit) {
		unsigned set = index & (GBN_RV32_SETS - 1);
		struct GbnRv32Block* first = &backend->blocks[set * GBN_RV32_WAYS];
		struct GbnRv32Block* spare = NULL;
		for (unsigned way = 0; way < GBN_RV32_WAYS; ++way) {
			if (matches(first + way, m, key)) { b = first + way; hit = true; break; }
			if (!spare && (!first[way].valid || first[way].generation != m->rom_generation)) spare = first + way;
		}
		if (!hit) {
			b = spare ? spare : first + backend->replacement[set];
			backend->replacement[set] = (uint8_t) (((unsigned) (b - first) + 1) % GBN_RV32_WAYS);
		}
		/* A cheap front table serves both C and generated edges. A conflict
		 * can miss here while retaining the compiled code in another way. */
		backend->lookup[index] = b;
	}
	unsigned slot = (unsigned) (b - backend->blocks);
	/* Validate the bytes of a RAM block at every entry. CPU stores, DMA and
	 * external loaders therefore need no new write barrier. A stale pipeline
	 * is checked separately below and remains in the interpreter until consumed. */
	if (hit && !rom && memcmp(backend->source[slot], source + (m->cpu.pc & m->code_mask),
	    (b->instructions ? b->instructions : 1) * width)) hit = false;
	if (hit && !b->instructions) return 0;
	uint32_t deadline = m->now + GBN_MAX_DELAY;
	if (m->next_event >= 0) {
		deadline = m->events[m->next_event].when;
		int32_t distance = (int32_t) (deadline - m->now);
		if (distance <= 0) return 0;
	}
	/* Immutable image + generation key lets warm entries validate the guest
	 * pipeline without fetching and decoding the ROM bytes again. */
	if (m->cpu.pipe[0] != (hit ? b->pipe[0] : fetch(m, m->cpu.pc, width)) ||
	    m->cpu.pipe[1] != (hit ? b->pipe[1] : fetch(m, m->cpu.pc + width, width))) return 0;
	if (!hit) compile(backend, b);
	if (!b->instructions) return 0;
	/* Keep the reason bit separate from the accumulated native count. */
	if (cap > 0x0fffffffu) cap = 0x0fffffffu;
	typedef uint64_t (*Function)(struct Gbn*, uint32_t, uint32_t);
	uint64_t result = ((Function) (uintptr_t) b->code)(m, cap, deadline);
	uint32_t count = (uint32_t) result & 0x7fffffffu;
#if GBN_RV32_STATS
	++backend->native_entries;
	if (b->loop) { ++backend->loop_entries; backend->loop_instructions += count; }
#endif
	if (!count) return 0x80000000u;
	m->cpu.pc = (uint32_t) (result >> 32);
	m->cpu.pipe[0] = fetch(m, m->cpu.pc, width);
	m->cpu.pipe[1] = fetch(m, m->cpu.pc + width, width);
	return (uint32_t) result;
}

unsigned gbn_rv32_execute(struct GbnRv32* backend, uint32_t cap) {
	return backend->machine->cpu.cpsr & 0x20 ? execute_mode(backend, cap, true) : execute_mode(backend, cap, false);
}
#else
unsigned gbn_rv32_execute(struct GbnRv32* backend, uint32_t cap) {
	(void) backend; (void) cap;
	return 0;
}
#endif
