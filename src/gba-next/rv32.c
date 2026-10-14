/* SPDX-License-Identifier: MPL-2.0 */
/* Independent ARM/Thumb -> RV32 backend. Native chains retain r0-r7, CPSR,
 * guest time and prefetch state in one register frame across direct edges.
 * Generated code has no C calls or instruction decoding. */
#include <gba-next/rv32.h>
#ifndef GBN_RV32_FAST
#define GBN_RV32_FAST 0
#endif
#include "internal.h"
#include "ppu-internal.h"
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

#if (GBN_RV32_FAST & 4194304) && NATIVE_RV32
static void hub_init(void);
#endif
void gbn_rv32_init(struct GbnRv32* backend, struct Gbn* m) {
	memset(backend, 0, sizeof(*backend));
	backend->machine = m;
#if (GBN_RV32_FAST & 4194304) && NATIVE_RV32
	hub_init();
#endif
}

#if NATIVE_RV32
_Static_assert(offsetof(struct Gbn, prefetched_pc) < 2048, "Native state offsets must fit load/store immediates");
_Static_assert(GBN_RV32_SLOTS >= 2 && !(GBN_RV32_SLOTS & (GBN_RV32_SLOTS - 1)), "Cache slots must be a power of two");
_Static_assert(GBN_RV32_WAYS && !(GBN_RV32_WAYS & (GBN_RV32_WAYS - 1)) &&
	GBN_RV32_WAYS <= 256 && GBN_RV32_WAYS <= GBN_RV32_SLOTS, "Cache ways must be a power of two and fit replacement counters");
_Static_assert(GBN_RV32_SEGMENT_WORDS >= GBN_RV32_WORDS, "A block must fit in one arena segment");
_Static_assert(offsetof(struct GbnRv32Block, generation) == 16 && sizeof(struct GbnRv32Block) % 16 == 0,
	"Native edge and proof fields must share an aligned 16-byte prefix");
/* NOW holds the modular difference now - deadline throughout a native frame.
 * Event guards can test its sign directly; only entry/return convert time. */
enum { ZERO = 0, RA = 1, SP = 2, LHS = 5, RHS = 6, RESULT = 7,
	NOW = 8, HOST_S1 = 9, MACHINE = 10, LIMIT = 11, FLAGS = 12, TEMP = 13,
	PREFETCH = 18, WAIT = 19, HOST_S4 = 20, NEGATIVE = 21, IS_ZERO = 22,
	RAM_EPOCH = 25, CONTEXT = 26, PROLOGUE_WORDS = 38,
	FRAME_BYTES = 144, SNAPSHOT_FLAGS = 80, SNAPSHOT_PREFETCH = 84, SNAPSHOT_NOW = 88,
    STACK_TARGET = 92, SAVED_NEGATIVE = 96, SAVED_ZERO = 100,
    SAVED_EPOCH = 104, SAVED_CONTEXT = 108, SAVED_S7 = 112, SAVED_S8 = 116,
    SAVED_S11 = 120, SAVED_RA = 124, INITIAL_CAP = 128, INITIAL_DEADLINE = 132 };
/* Guest SP stays in the machine. Every other integer register shares one
 * native frame across chains, including LR. gp/tp retain their ABI values. */
static const unsigned guest[] = {14, 15, 16, 17, 28, 29, 30, 31, 23, 24, 27, 20, 9, 0, 1};
static bool resident(unsigned r) { return r < 15 && r != 13; }
struct Emit {
	struct GbnRv32Block* block;
	unsigned count;
	struct { unsigned at, instruction; bool fallback; } stops[10 * GBN_RV32_INSNS];
	unsigned stop_count;
	unsigned returns[GBN_RV32_INSNS + 2], return_count;
	uint32_t written;
	bool read_only, read_seen, dynamic_pc, skip_shifter;
	unsigned loop_jumps[GBN_RV32_INSNS + 1], loop_jump_count;
	unsigned body_offsets[GBN_RV32_INSNS], source_count;
	uint32_t thumb_literal[8];
	unsigned thumb_literal_mask;
	bool io_guarded;
#if GBN_RV32_FAST & 8
	bool cv_dead; /* the instruction's C/V are overwritten in this block before any read */
#endif
#if GBN_RV32_FAST & 16384
	uint8_t live; /* flags read after the current instruction: N 8, Z 4, C 2, V 1 */
#endif
#if GBN_RV32_FAST & 16
	unsigned link_count, link_at[GBN_RV32_INSNS + 2]; /* exit sites that may link directly */
	uint32_t link_pc[GBN_RV32_INSNS + 2];
#endif
#if GBN_RV32_FAST & 512
	/* Cold paths, appended after the epilogue (cold_begin/cold_end). */
	uint32_t* hot_code;
	unsigned hot_count, cold_count, cold_stops, cold_returns, cold_link_count;
	unsigned cold_in_count, cold_in[2 * GBN_RV32_INSNS][2]; /* hot branch, cold target */
	unsigned cold_out_count, cold_out[2 * GBN_RV32_INSNS][2]; /* cold jump, hot target */
	uint32_t cold[GBN_RV32_WORDS];
#endif
#if GBN_RV32_FAST & 4194304
	unsigned access_start; /* first word of the current load/store's address */
#endif
#if GBN_RV32_FAST & 8388608
	unsigned insn_start; /* first word of the current instruction */
	/* A Thumb branch ending the block: taken when branch_reg is nonzero
	 * (zero with branch_clear), or always; else LIMIT bit 31 carries it. */
	bool branch_direct, branch_always, branch_clear;
	uint8_t branch_reg;
	unsigned extra_time;
#endif
#if GBN_RV32_FAST & 1024
	/* Thumb fetch time not yet added to NOW, and its value at each
	 * instruction's start (stops resume the interpreter there). */
	unsigned deferred, prefix_filled;
	uint16_t prefix[GBN_RV32_INSNS + 1];
#endif
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
#if GBN_RV32_FAST & 512
/* Fast mode: a rarely taken path goes to a separate buffer that finish_body
 * appends after the epilogue, so the hot path stays dense in the I-cache.
 * The hot branch at hot_at (its condition already encoded) enters it; the
 * cold code ends with a jump back to the hot code emitted next. Its stops
 * and returns are marked and relocated when the buffer is appended. */
#define COLD_AT 0x80000000u
static void cold_begin(struct Emit* e, unsigned hot_at) {
	e->cold_in[e->cold_in_count][0] = hot_at; e->cold_in[e->cold_in_count++][1] = e->cold_count;
	e->hot_code = e->block->code; e->hot_count = e->count;
	e->block->code = e->cold; e->count = e->cold_count;
	e->cold_stops = e->stop_count; e->cold_returns = e->return_count;
}
#endif
#if GBN_RV32_FAST & 1024
/* Fast mode: a Thumb block adds its instructions' fetch time once, where
 * time is observed: at its exits (time_flush) and in its stop stubs. */
static void time_flush(struct Emit* e) {
	unsigned d = e->prefix[e->block->instructions];
#if GBN_RV32_FAST & 8388608
	d += e->extra_time; e->extra_time = 0; /* a taken branch's time, the same add */
#endif
	if (d) imm(e, NOW, NOW, 0, (int) d);
}
#endif
#if GBN_RV32_FAST & 512
static void cold_end(struct Emit* e) {
	e->cold_out[e->cold_out_count][0] = e->count; e->cold_out[e->cold_out_count++][1] = e->hot_count;
	word(e, 0);
	for (unsigned i = e->cold_stops; i < e->stop_count; ++i) e->stops[i].at |= COLD_AT;
	for (unsigned i = e->cold_returns; i < e->return_count; ++i) e->returns[i] |= COLD_AT;
	e->cold_count = e->count;
	e->block->code = e->hot_code; e->count = e->hot_count;
}
#endif
static void get(struct Emit* e, unsigned dest, unsigned r, uint32_t pc) {
	if (resident(r)) imm(e, dest, guest[r], 0, 0);
	else if (r == 15) constant(e, dest, pc + (e->block->thumb ? 4 : 8));
	else load(e, dest, offsetof(struct Gbn, cpu.r) + 4 * r);
}
/* All non-SP integer guest registers reside in host registers. Use them
 * directly as read operands; SP and PC use the designated scratch register. */
static unsigned source(struct Emit* e, unsigned scratch, unsigned r, uint32_t pc) {
	if (resident(r)) return guest[r];
	get(e, scratch, r, pc);
	return scratch;
}
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 512)
/* Fast mode, Thumb: instead of copying RESULT into a guest register, let
 * the instruction that computed it write the register, and its later readers
 * (flag computations) read it there. Only when that is provably the same:
 * the definition is an ALU word of this instruction, every word after it
 * an ALU word that neither reads nor writes the register, and no branch of
 * this instruction or cold path lands after the definition. */
static bool direct_result(struct Emit* e, unsigned reg) {
	uint32_t* code = e->block->code;
	unsigned n = e->count, start = e->insn_start, k = n;
	if (!e->block->thumb || code == e->cold || start == ~0u || n <= start) return false;
	for (unsigned j = n; j-- > start;) {
		uint32_t w = code[j], o = w & 127;
		bool writes = o != 0x63 && o != 0x23 && (w >> 7 & 31) == RESULT;
		if (writes) { k = j; break; }
		if ((o != 0x13 && o != 0x33) || (w >> 7 & 31) == reg || (w >> 15 & 31) == reg ||
		    (o == 0x33 && (w >> 20 & 31) == reg)) return false;
	}
	if (k == n || ((code[k] & 127) != 0x13 && (code[k] & 127) != 0x33 && (code[k] & 127) != 0x37)) return false;
	for (unsigned j = start; j < k; ++j) {
		uint32_t w = code[j];
		int32_t delta;
		if ((w & 127) == 0x63) delta = (int32_t) ((w >> 31) << 12 | (w >> 7 & 1) << 11 | (w >> 25 & 63) << 5 | (w >> 8 & 15) << 1) << 19 >> 19;
		else if ((w & 127) == 0x6f) delta = (int32_t) ((w >> 31) << 20 | (w >> 12 & 255) << 12 | (w >> 20 & 1) << 11 | (w >> 21 & 1023) << 1) << 11 >> 11;
		else continue;
		int32_t target = (int32_t) j + delta / 4;
		if (target > (int32_t) k && target <= (int32_t) n) return false;
	}
	for (unsigned i = 0; i < e->cold_out_count; ++i)
		if (e->cold_out[i][1] > k && e->cold_out[i][1] <= n) return false;
	code[k] = (code[k] & ~(31u << 7)) | reg << 7;
	for (unsigned j = k + 1; j < n; ++j) {
		uint32_t w = code[j];
		if ((w >> 15 & 31) == RESULT) w = (w & ~(31u << 15)) | reg << 15;
		if ((w & 127) == 0x33 && (w >> 20 & 31) == RESULT) w = (w & ~(31u << 20)) | reg << 20;
		code[j] = w;
	}
	return true;
}
#endif
static void put_value(struct Emit* e, unsigned r, unsigned value) {
	e->written |= 1u << r;
	e->thumb_literal_mask &= ~(1u << r);
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 512)
	if (resident(r) && value == RESULT && direct_result(e, guest[r])) return;
#endif
	if (resident(r)) { if (guest[r] != value) imm(e, guest[r], value, 0, 0); }
	else store(e, value, offsetof(struct Gbn, cpu.r) + 4 * r);
}
static void put(struct Emit* e, unsigned r) { put_value(e, r, RESULT); }
static void nz_of(struct Emit* e, unsigned value) {
	/* Keep independent booleans: entry CPSR may legally contain N=Z=1. */
#if GBN_RV32_FAST & 16384
	if (e->live & 8)
#endif
	imm(e, NEGATIVE, value, 5, 31);
#if GBN_RV32_FAST & 16384
	if (e->live & 4)
#endif
	imm(e, IS_ZERO, value, 3, 1);
}
static void nz(struct Emit* e) { nz_of(e, RESULT); }
/* FLAGS retains bits 0..29. Materialize N/Z only when the complete CPSR is
 * observed, including fixed-point proofs and every native return reason. */
static void packed_flags(struct Emit* e, unsigned dest) {
	imm(e, dest, NEGATIVE, 1, 31); imm(e, LHS, IS_ZERO, 1, 30);
	op(e, dest, dest, LHS, 6, 0); op(e, dest, FLAGS, dest, 6, 0);
}
/* Inputs may alias their assigned LHS/RHS scratch registers, respectively;
 * neither is overwritten until all uses of that original operand complete. */
static void arithmetic(struct Emit* e, bool subtract, unsigned a, unsigned b) {
#if GBN_RV32_FAST & 16384
    if (e->live != 15) {
        /* Only the flags a later instruction reads. C before V: V may reuse
         * the scratch operands. N/Z come from RESULT alone. */
        op(e, RESULT, a, b, 0, subtract ? 0x20 : 0);
        if (e->live & 3) {
            if (e->live & 2) {
                op(e, TEMP, subtract ? a : RESULT, subtract ? b : a, 3, 0);
                if (subtract) imm(e, TEMP, TEMP, 4, 1);
            }
            if (e->live & 1) {
                if (subtract) { op(e, LHS, a, b, 2, 0); imm(e, RHS, RESULT, 5, 31); }
                else { op(e, LHS, RESULT, a, 2, 0); imm(e, RHS, b, 5, 31); }
                op(e, LHS, LHS, RHS, 4, 0);
            }
            imm(e, FLAGS, FLAGS, 1, 8); imm(e, FLAGS, FLAGS, 5, 8);
            if (e->live & 1) { imm(e, LHS, LHS, 1, 28); op(e, FLAGS, FLAGS, LHS, 6, 0); }
            if (e->live & 2) { imm(e, TEMP, TEMP, 1, 29); op(e, FLAGS, FLAGS, TEMP, 6, 0); }
        }
        nz(e);
        return;
    }
#endif
#if GBN_RV32_FAST & 8
    if (e->cv_dead) { op(e, RESULT, a, b, 0, subtract ? 0x20 : 0); nz(e); return; }
#endif
    op(e,RESULT,a,b,0,subtract?0x20:0);
    op(e,TEMP,subtract?a:RESULT,subtract?b:a,3,0);
    if(subtract)imm(e,TEMP,TEMP,4,1);
    /* Overflow from signed order, with the original operands still live:
     * ADD: (signed result < signed a) XOR sign(b).
     * SUB: (signed a < signed b) XOR sign(result). */
    if(subtract) {
        op(e,LHS,a,b,2,0);nz(e);op(e,LHS,LHS,NEGATIVE,4,0);
    } else {
        op(e,LHS,RESULT,a,2,0);imm(e,RHS,b,5,31);
        op(e,LHS,LHS,RHS,4,0);nz(e);
    }
    imm(e,LHS,LHS,1,28);imm(e,TEMP,TEMP,1,29);
    imm(e,FLAGS,FLAGS,1,8);imm(e,FLAGS,FLAGS,5,8);
    op(e,FLAGS,FLAGS,LHS,6,0);op(e,FLAGS,FLAGS,TEMP,6,0);
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
    /* Complement negative operands, then count significant byte groups.
     * Each ARM7 early-exit class gets a short path; gp/tp remain untouched. */
    constant(e, WAIT, accumulate ? 2 : 1);
    imm(e, TEMP, multiplier, 5, 0x400 | 31);
    op(e, TEMP, TEMP, multiplier, 4, 0);
    unsigned exits[3];
    for (unsigned i = 0; i < 3; ++i) {
        imm(e, TEMP, TEMP, 5, 8);
        exits[i] = e->count; word(e, 0);
        imm(e, WAIT, WAIT, 0, 1);
    }
    for (unsigned i = 0; i < 3; ++i)
        e->block->code[exits[i]] = beq(TEMP, ZERO, 4 * (e->count - exits[i]));
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
#if GBN_RV32_FAST & 1
	/* Fast mode: prefetch hides the fetch break; only the data wait counts. */
	(void) pc;
	op(e, NOW, NOW, WAIT, 0, 0);
	return;
#endif
	unsigned shift = 0;
	while ((1u << shift) < seq) ++shift;
	constant(e, TEMP, pc + (e->block->thumb ? 4 : 8));
	op(e, LHS, PREFETCH, TEMP, 0, 0x20);
	imm(e, RESULT, LHS, 3, 16);
	op(e, RESULT, ZERO, RESULT, 0, 0x20);
	imm(e, LHS, LHS, 5, 1); op(e, LHS, LHS, RESULT, 7, 0); /* previous */
	/* Short waits add no prefetch slots and no uncovered CPU time.
	 * Longer waits either fit the queue (also zero uncovered time), or
	 * saturate it. Only the saturated path needs a residual-time update. */
	imm(e, RESULT, WAIT, 3, (int) seq + 2);
	unsigned short_wait = e->count; word(e, 0);
	imm(e, RHS, WAIT, 0, -2); imm(e, RHS, RHS, 5, (int) shift);
	constant(e, RESULT, 7); op(e, RESULT, RESULT, LHS, 0, 0x20);
	unsigned fits = e->count; word(e, 0);
	imm(e, RHS, RESULT, 1, (int) shift); imm(e, RHS, RHS, 0, (int) seq + 1);
	op(e, WAIT, WAIT, RHS, 0, 0x20); op(e, NOW, NOW, WAIT, 0, 0);
	imm(e, PREFETCH, TEMP, 0, 14);
	unsigned full_done = e->count; word(e, 0);
	e->block->code[fits] = beq(RESULT, RHS, 4 * (e->count - fits)) | 7u << 12;
	op(e, LHS, LHS, RHS, 0, 0); imm(e, LHS, LHS, 1, 1);
	op(e, PREFETCH, TEMP, LHS, 0, 0);
	unsigned fits_done = e->count; word(e, 0);
	e->block->code[short_wait] = beq(RESULT, ZERO, 4 * (e->count - short_wait)) | 1u << 12;
	imm(e, LHS, LHS, 1, 1); op(e, PREFETCH, TEMP, LHS, 0, 0);
	e->block->code[full_done] = beq(ZERO, ZERO, 4 * (e->count - full_done));
	e->block->code[fits_done] = beq(ZERO, ZERO, 4 * (e->count - fits_done));
}

/* Device accesses use a one- or two-cycle bus wait.  With ROM prefetch on and
 * at least one sequential code wait, both values always take ram_wait's short
 * path: no uncovered NOW time is added and only the preceding prefetch slot
 * is advanced.  Emit that path directly; the zero-wait/WAIT=2 corner and all
 * RAM/non-prefetch cases retain the full formula. */
static void io_wait(struct Emit* e, const struct Gbn* m, uint32_t pc, unsigned wait) {
	if (m->code_region < 8 || !(m->waitcnt & 0x4000) || (wait == 2 && !m->code_wait)) {
		ram_wait(e, m, pc);
		return;
	}
#if GBN_RV32_FAST & 1
	return; /* fast mode: the prefetch slot model is gone, the device wait is hidden */
#endif
	constant(e, TEMP, pc + (e->block->thumb ? 4 : 8));
	op(e, LHS, PREFETCH, TEMP, 0, 0x20);
	imm(e, RESULT, LHS, 3, 16);
	op(e, RESULT, ZERO, RESULT, 0, 0x20);
	imm(e, LHS, LHS, 5, 1);
	op(e, LHS, LHS, RESULT, 7, 0);
	imm(e, LHS, LHS, 1, 1);
	op(e, PREFETCH, TEMP, LHS, 0, 0);
}

static void writeback(struct Emit* e, unsigned rn, int adjustment) {
	if (rn == 15) return;
	if (adjustment >= -2048 && adjustment < 2048) imm(e, RESULT, TEMP, 0, adjustment);
	else { constant(e, RHS, (uint32_t) adjustment); op(e, RESULT, TEMP, RHS, 0, 0); }
	put(e, rn);
}
static void access_value(struct Emit* e, uint32_t pc, unsigned rd, unsigned kind, unsigned width, unsigned rn, int adjustment) {
    (void)kind; /* Loads use the guarded shared leaf path. */
    unsigned fn=width==4?2:width==2?1:0;
    get(e,RESULT,rd,pc);
    word(e,RESULT<<20|LHS<<15|fn<<12|0x23);
    writeback(e,rn,adjustment);
}

/* Effective address is in LHS. All guards precede data or base writeback. */
/* Bits 0/1 permit stored byte/halfword and word reads. Bits 2/3 permit
 * zero DMA count reads and count/control words with the low half masked.
 * Bits 4/5 permit pure open-bus byte/halfword and word reads.
 * Dynamic timers/audio/SIO, WAITCNT and EEPROM keep the bus path. */
static const uint8_t native_io_readable[0x210 / 2] = {
#if (GBN_RV32_FAST & 65536) && (GBN_RV32_FAST & 134217728)
	[0] = 3, [1] = 3, [2] = 0, [3] = 0, /* DISPSTAT, VCOUNT: merged lines applied in C */
#elif GBN_RV32_FAST & 65536
	[0] = 3, [1] = 3, [2] = 0, [3] = 3, /* DISPSTAT: HBlank flag computed in C */
#else
	[0] = 3, [1] = 3, [2] = 3, [3] = 3,
#endif
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

#include "dma-start-native.h"

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

#if GBN_RV32_FAST & 4194304
/* Fast mode: the memory leaves sit together within reach of tp (see
 * hub_call), and a read reports success as 0, its wait already added to
 * NOW, so a single branch leaves for the miss (1) and open-bus (2) paths. */
#define LEAF __attribute__((naked, noinline, used, section(".text.gbn_hub")))
#define LEAF_OK "0"
#define LEAF_ROM "0"
#define LEAF_MISS "1"
#define LEAF_TIME "bnez t1,1f\n\tadd s0,s0,t0\n\t1: " /* NOW += wait */
#else
#define LEAF __attribute__((naked, noinline, used))
#define LEAF_OK "1"
#define LEAF_ROM "-1"
#define LEAF_MISS "0"
#define LEAF_TIME ""
#endif
/* Preserve the resident guest frame. a3 is the original data address;
 * t2 returns an unsigned value, t0 the bus/internal wait and t1 the kind:
 * 0=miss, 1=RAM/stored IO, -1=ROM, 2=open bus handled with the caller PC.
 * s3 is the private link; gp/tp, guest LR and all resident state stay intact. */
LEAF static void read_byte(void) {
    __asm__ volatile(
        "srli t1,a3,24\n\t"
        "addi t1,t1,-2\n\t"
        "sltiu t2,t1,2\n\t"
        "beqz t2,3f\n\t"
        "beqz t1,0f\n\t"
        "lw t2,%c[iwram](a0)\n\t"
        "li t0,32767\n\t"
        "li t1,2\n\t"
        "j 2f\n\t"
        "0: lw t2,%c[ewram](a0)\n\t"
        "li t0,262143\n\t"
        "lbu t1,%c[ewram_wait](a0)\n\t"
        "addi t1,t1,1\n\t"
        "addi t1,t1,1\n\t"
        "2: beqz t2,9f\n\t"
        "and t0,a3,t0\n\t"
        "add t0,t0,t2\n\t"
        "lbu t2,0(t0)\n\t"
        "mv t0,t1\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "3: srli t1,a3,24\n\t"
        "addi t1,t1,-8\n\t"
        "sltiu t2,t1,6\n\t"
        "beqz t2,4f\n\t"
        "lw t2,%c[rom](a0)\n\t"
        "beqz t2,9f\n\t"
        "li t0,33554431\n\t"
        "and t0,a3,t0\n\t"
        "lw t1,%c[rom_size](a0)\n\t"
        "addi t0,t0,1\n\t"
        "bltu t1,t0,9f\n\t"
        "addi t0,t0,-1\n\t"
        "add t0,t0,t2\n\t"
        "lbu t2,0(t0)\n\t"
        "srli t0,a3,25\n\t"
        "addi t0,t0,-4\n\t"
        "add t0,a0,t0\n\t"
        "lbu t1,%c[rom_nonseq](t0)\n\t"
        "addi t0,t1,2\n\t"
        "li t1," LEAF_ROM "\n\t"
        "j 8f\n\t"
        "4: srli t1,a3,24\n\t"
        "addi t1,t1,-4\n\t"
        "bnez t1,9f\n\t"
        "li t0,67108864\n\t"
        "sub t0,a3,t0\n\t"
        "sltiu t1,t0,%c[io_bytes]\n\t"
        "beqz t1,9f\n\t"
        "srli t1,t0,1\n\t"
        "la t2,%[table]\n\t"
        "add t1,t1,t2\n\t"
        "lbu t1,0(t1)\n\t"
        "andi t1,t1,21\n\t"
        "beqz t1,9f\n\t"
        "lw t2,%c[devices](a0)\n\t"
        "beqz t2,9f\n\t"
        "andi t2,t1,16\n\t"
        "bnez t2,7f\n\t"
        "andi t2,t1,4\n\t"
        "bnez t2,6f\n\t"
        "lw t2,%c[devices](a0)\n\t"
        "add t0,t0,t2\n\t"
        "lbu t2,0(t0)\n\t"
        "li t0,2\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "6: li t2,0\n\t"
        "li t0,2\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "7: li t0,2\n\t"
        "li t1,2\n\t"
        "j 8f\n\t"
        "9: li t1," LEAF_MISS "\n\t"
        "8: " LEAF_TIME "jalr zero,s3,0\n\t"
        : : [iwram] "i" (offsetof(struct Gbn, iwram)),
            [ewram] "i" (offsetof(struct Gbn, ewram)),
            [ewram_wait] "i" (offsetof(struct Gbn, ewram_wait)),
            [rom] "i" (offsetof(struct Gbn, rom)), [rom_size] "i" (offsetof(struct Gbn, rom_size)),
            [rom_nonseq] "i" (offsetof(struct Gbn, rom_nonseq)),
            [rom_seq] "i" (offsetof(struct Gbn, rom_seq)),
            [devices] "i" (offsetof(struct Gbn, devices)),
            [io_bytes] "i" (sizeof(native_io_readable) * 2), [table] "i" (native_io_readable));
}

/* Preserve the resident guest frame. a3 is the original data address;
 * t2 returns an unsigned value, t0 the bus/internal wait and t1 the kind:
 * 0=miss, 1=RAM/stored IO, -1=ROM, 2=open bus handled with the caller PC.
 * s3 is the private link; gp/tp, guest LR and all resident state stay intact. */
LEAF static void read_half(void) {
    __asm__ volatile(
        "andi t0,a3,1\n\t"
        "bnez t0,9f\n\t"
        "srli t1,a3,24\n\t"
        "addi t1,t1,-2\n\t"
        "sltiu t2,t1,2\n\t"
        "beqz t2,3f\n\t"
        "beqz t1,0f\n\t"
        "lw t2,%c[iwram](a0)\n\t"
        "li t0,32767\n\t"
        "li t1,2\n\t"
        "j 2f\n\t"
        "0: lw t2,%c[ewram](a0)\n\t"
        "li t0,262143\n\t"
        "lbu t1,%c[ewram_wait](a0)\n\t"
        "addi t1,t1,1\n\t"
        "addi t1,t1,1\n\t"
        "2: beqz t2,9f\n\t"
        "and t0,a3,t0\n\t"
        "add t0,t0,t2\n\t"
        "andi t2,t0,1\n\t"
        "bnez t2,9f\n\t"
        "lhu t2,0(t0)\n\t"
        "mv t0,t1\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "3: srli t1,a3,24\n\t"
        "addi t1,t1,-8\n\t"
        "sltiu t2,t1,6\n\t"
        "beqz t2,4f\n\t"
        "li t2,5\n\t"
        "beq t1,t2,9f\n\t"
        "lw t2,%c[rom](a0)\n\t"
        "beqz t2,9f\n\t"
        "li t0,33554431\n\t"
        "and t0,a3,t0\n\t"
        "lw t1,%c[rom_size](a0)\n\t"
        "addi t0,t0,2\n\t"
        "bltu t1,t0,9f\n\t"
        "addi t0,t0,-2\n\t"
        "add t0,t0,t2\n\t"
        "andi t2,t0,1\n\t"
        "bnez t2,9f\n\t"
        "lhu t2,0(t0)\n\t"
        "srli t0,a3,25\n\t"
        "addi t0,t0,-4\n\t"
        "add t0,a0,t0\n\t"
        "lbu t1,%c[rom_nonseq](t0)\n\t"
        "addi t0,t1,2\n\t"
        "li t1," LEAF_ROM "\n\t"
        "j 8f\n\t"
        "4: srli t1,a3,24\n\t"
        "addi t1,t1,-4\n\t"
        "bnez t1,9f\n\t"
        "li t0,67108864\n\t"
        "sub t0,a3,t0\n\t"
        "sltiu t1,t0,%c[io_bytes]\n\t"
        "beqz t1,9f\n\t"
        "srli t1,t0,1\n\t"
        "la t2,%[table]\n\t"
        "add t1,t1,t2\n\t"
        "lbu t1,0(t1)\n\t"
        "andi t1,t1,21\n\t"
        "beqz t1,9f\n\t"
        "lw t2,%c[devices](a0)\n\t"
        "beqz t2,9f\n\t"
        "andi t2,t1,16\n\t"
        "bnez t2,7f\n\t"
        "andi t2,t1,4\n\t"
        "bnez t2,6f\n\t"
        "lw t2,%c[devices](a0)\n\t"
        "add t0,t0,t2\n\t"
        "lhu t2,0(t0)\n\t"
        "li t0,2\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "6: li t2,0\n\t"
        "li t0,2\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "7: li t0,2\n\t"
        "li t1,2\n\t"
        "j 8f\n\t"
        "9: li t1," LEAF_MISS "\n\t"
        "8: " LEAF_TIME "jalr zero,s3,0\n\t"
        : : [iwram] "i" (offsetof(struct Gbn, iwram)),
            [ewram] "i" (offsetof(struct Gbn, ewram)),
            [ewram_wait] "i" (offsetof(struct Gbn, ewram_wait)),
            [rom] "i" (offsetof(struct Gbn, rom)), [rom_size] "i" (offsetof(struct Gbn, rom_size)),
            [rom_nonseq] "i" (offsetof(struct Gbn, rom_nonseq)),
            [rom_seq] "i" (offsetof(struct Gbn, rom_seq)),
            [devices] "i" (offsetof(struct Gbn, devices)),
            [io_bytes] "i" (sizeof(native_io_readable) * 2), [table] "i" (native_io_readable));
}

/* Preserve the resident guest frame. a3 is the original data address;
 * t2 returns an unsigned value, t0 the bus/internal wait and t1 the kind:
 * 0=miss, 1=RAM/stored IO, -1=ROM, 2=open bus handled with the caller PC.
 * s3 is the private link; gp/tp, guest LR and all resident state stay intact. */
LEAF static void read_word(void) {
    __asm__ volatile(
        "andi t0,a3,3\n\t"
        "bnez t0,9f\n\t"
        "srli t1,a3,24\n\t"
        "addi t1,t1,-2\n\t"
        "sltiu t2,t1,2\n\t"
        "beqz t2,3f\n\t"
        "beqz t1,0f\n\t"
        "lw t2,%c[iwram](a0)\n\t"
        "li t0,32767\n\t"
        "li t1,2\n\t"
        "j 2f\n\t"
        "0: lw t2,%c[ewram](a0)\n\t"
        "li t0,262143\n\t"
        "lbu t1,%c[ewram_wait](a0)\n\t"
        "addi t1,t1,1\n\t"
        "slli t1,t1,1\n\t"
        "addi t1,t1,1\n\t"
        "2: beqz t2,9f\n\t"
        "and t0,a3,t0\n\t"
        "add t0,t0,t2\n\t"
        "andi t2,t0,3\n\t"
        "bnez t2,9f\n\t"
        "lw t2,0(t0)\n\t"
        "mv t0,t1\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "3: srli t1,a3,24\n\t"
        "addi t1,t1,-8\n\t"
        "sltiu t2,t1,6\n\t"
        "beqz t2,4f\n\t"
        "lw t2,%c[rom](a0)\n\t"
        "beqz t2,9f\n\t"
        "li t0,33554431\n\t"
        "and t0,a3,t0\n\t"
        "lw t1,%c[rom_size](a0)\n\t"
        "addi t0,t0,4\n\t"
        "bltu t1,t0,9f\n\t"
        "addi t0,t0,-4\n\t"
        "add t0,t0,t2\n\t"
        "andi t2,t0,3\n\t"
        "bnez t2,9f\n\t"
        "lw t2,0(t0)\n\t"
        "srli t0,a3,25\n\t"
        "addi t0,t0,-4\n\t"
        "add t0,a0,t0\n\t"
        "lbu t1,%c[rom_nonseq](t0)\n\t"
        "lbu t0,%c[rom_seq](t0)\n\t"
        "add t1,t1,t0\n\t"
        "addi t0,t1,3\n\t"
        "li t1," LEAF_ROM "\n\t"
        "j 8f\n\t"
        "4: srli t1,a3,24\n\t"
        "addi t1,t1,-4\n\t"
        "bnez t1,9f\n\t"
        "li t0,67108864\n\t"
        "sub t0,a3,t0\n\t"
        "sltiu t1,t0,%c[io_bytes]\n\t"
        "beqz t1,9f\n\t"
        "srli t1,t0,1\n\t"
        "la t2,%[table]\n\t"
        "add t1,t1,t2\n\t"
        "lbu t1,0(t1)\n\t"
        "andi t1,t1,42\n\t"
        "beqz t1,9f\n\t"
        "lw t2,%c[devices](a0)\n\t"
        "beqz t2,9f\n\t"
        "andi t2,t1,32\n\t"
        "bnez t2,7f\n\t"
        "andi t2,t1,8\n\t"
        "bnez t2,6f\n\t"
        "lw t2,%c[devices](a0)\n\t"
        "add t0,t0,t2\n\t"
        "lw t2,0(t0)\n\t"
        "li t0,2\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "6: lw t2,%c[devices](a0)\n\t"
        "add t0,t0,t2\n\t"
        "lw t2,0(t0)\n\t"
        "srli t2,t2,16\n\t"
        "slli t2,t2,16\n\t"
        "li t0,2\n\t"
        "li t1," LEAF_OK "\n\t"
        "j 8f\n\t"
        "7: li t0,2\n\t"
        "li t1,2\n\t"
        "j 8f\n\t"
        "9: li t1," LEAF_MISS "\n\t"
        "8: " LEAF_TIME "jalr zero,s3,0\n\t"
        : : [iwram] "i" (offsetof(struct Gbn, iwram)),
            [ewram] "i" (offsetof(struct Gbn, ewram)),
            [ewram_wait] "i" (offsetof(struct Gbn, ewram_wait)),
            [rom] "i" (offsetof(struct Gbn, rom)), [rom_size] "i" (offsetof(struct Gbn, rom_size)),
            [rom_nonseq] "i" (offsetof(struct Gbn, rom_nonseq)),
            [rom_seq] "i" (offsetof(struct Gbn, rom_seq)),
            [devices] "i" (offsetof(struct Gbn, devices)),
            [io_bytes] "i" (sizeof(native_io_readable) * 2), [table] "i" (native_io_readable));
}

#if GBN_RV32_FAST & 4194304
/* Fast mode, ROM blocks: RAM stores through shared leaves. a3 is the data
 * address and t2 the value; t1 returns 0 when stored (its wait added to
 * NOW), else nonzero: not RAM, absent RAM or a misaligned address. s3 is
 * the link. */
_Static_assert(GBN_IWRAM_SIZE == 32768 && GBN_EWRAM_SIZE == 262144, "write leaf masks");
#define WRITE_LEAF(name, align, store, word) \
LEAF static void name(void) { \
    __asm__ volatile( \
        "andi t1,a3," #align "\n\t" \
        "bnez t1,9f\n\t" \
        "srli t1,a3,24\n\t" \
        "addi t1,t1,-2\n\t" \
        "sltiu t0,t1,2\n\t" \
        "beqz t0,9f\n\t" \
        "beqz t1,0f\n\t" \
        "lw t1,%c[iwram](a0)\n\t" \
        "li t0,32767\n\t" \
        "beqz t1,9f\n\t" \
        "and t0,a3,t0\n\t" \
        "add t1,t1,t0\n\t" \
        "andi t0,t1," #align "\n\t" \
        "bnez t0,9f\n\t" \
        store " t2,0(t1)\n\t" \
        "addi s0,s0,1\n\t" \
        "li t1,0\n\t" \
        "jalr zero,s3,0\n\t" \
        "0: lw t1,%c[ewram](a0)\n\t" \
        "li t0,262143\n\t" \
        "beqz t1,9f\n\t" \
        "and t0,a3,t0\n\t" \
        "add t1,t1,t0\n\t" \
        "andi t0,t1," #align "\n\t" \
        "bnez t0,9f\n\t" \
        store " t2,0(t1)\n\t" \
        "lbu t0,%c[ewram_wait](a0)\n\t" \
        "addi t0,t0,1\n\t" \
        word \
        "add s0,s0,t0\n\t" \
        "li t1,0\n\t" \
        "jalr zero,s3,0\n\t" \
        "9: li t1,1\n\t" \
        "jalr zero,s3,0\n\t" \
        : : [iwram] "i" (offsetof(struct Gbn, iwram)), [ewram] "i" (offsetof(struct Gbn, ewram)), \
            [ewram_wait] "i" (offsetof(struct Gbn, ewram_wait))); \
}
WRITE_LEAF(write_byte, 0, "sb", "")
WRITE_LEAF(write_half, 1, "sh", "")
WRITE_LEAF(write_word, 3, "sw", "slli t0,t0,1\n\t")
#undef WRITE_LEAF
/* Fast mode, ROM blocks: a Thumb PUSH/POP span. a3 is its lowest address,
 * t2 its bytes plus 1 for POP (consumed); t1 returns the host address of the
 * lowest word, the span's time added to NOW (per word 1 in IWRAM, 2 * (wait
 * + 1) in EWRAM, plus 1 for POP), or 0: not RAM, absent or misaligned host
 * RAM, or a span over the mirror end (bytes + pop - 2 past the offset
 * reaching the size). */
LEAF static void stack_span(void) {
    __asm__ volatile(
        "srli t1,a3,24\n\t"
        "addi t1,t1,-2\n\t"
        "sltiu t0,t1,2\n\t"
        "beqz t0,9f\n\t"
        "bnez t1,3f\n\t"
        "lw t1,%c[ewram](a0)\n\t"
        "beqz t1,9f\n\t"
        "andi t0,t1,3\n\t"
        "bnez t0,9f\n\t"
        "li t0,262140\n\t"
        "and t0,a3,t0\n\t"
        "add t1,t1,t0\n\t"
        "add t0,t0,t2\n\t"
        "addi t0,t0,-2\n\t"
        "srli t0,t0,18\n\t"
        "bnez t0,9f\n\t"
        "add s0,s0,t2\n\t"
        "lbu t0,%c[ewram_wait](a0)\n\t"
        "slli t0,t0,1\n\t"
        "addi t0,t0,-2\n\t"
        "srli t2,t2,2\n\t"
        "mul t2,t2,t0\n\t"
        "add s0,s0,t2\n\t"
        "jalr zero,s3,0\n\t"
        "3: lw t1,%c[iwram](a0)\n\t"
        "beqz t1,9f\n\t"
        "andi t0,t1,3\n\t"
        "bnez t0,9f\n\t"
        "li t0,32764\n\t"
        "and t0,a3,t0\n\t"
        "add t1,t1,t0\n\t"
        "add t0,t0,t2\n\t"
        "addi t0,t0,-2\n\t"
        "srli t0,t0,15\n\t"
        "bnez t0,9f\n\t"
        "srli t0,t2,2\n\t"
        "add s0,s0,t0\n\t"
        "andi t2,t2,1\n\t"
        "add s0,s0,t2\n\t"
        "jalr zero,s3,0\n\t"
        "9: li t1,0\n\t"
        "jalr zero,s3,0\n\t"
        : : [iwram] "i" (offsetof(struct Gbn, iwram)), [ewram] "i" (offsetof(struct Gbn, ewram)),
            [ewram_wait] "i" (offsetof(struct Gbn, ewram_wait)));
}

/* Fast mode, ROM blocks: an indirect target in t2 (raw), its block's
 * region in t0 (shifted to bits 24..31). t0 returns 0 when the target lies
 * in that region and inside the ROM, its Thumb address stored for the
 * indirect tail; else nonzero (nothing that matters stored). */
LEAF static void code_target(void) {
    __asm__ volatile(
        "xor t0,t0,t2\n\t"
        "srli t0,t0,24\n\t"
        "bnez t0,9f\n\t"
        "slli t0,t2,7\n\t"
        "srli t0,t0,7\n\t"
        "andi t2,t2,-2\n\t"
        "sw t2,%c[target](sp)\n\t"
        "lw t2,%c[rom_size](a0)\n\t"
        "sltu t0,t0,t2\n\t"
        "xori t0,t0,1\n\t"
        "jalr zero,s3,0\n\t"
        "9: li t0,1\n\t"
        "jalr zero,s3,0\n\t"
        : : [target] "i" (STACK_TARGET), [rom_size] "i" (offsetof(struct Gbn, rom_size)));
}
/* code_target for BX, which also needs an odd (Thumb) target; t1 is free. */
LEAF static void code_target_odd(void) {
    __asm__ volatile(
        "andi t1,t2,1\n\t"
        "beqz t1,9f\n\t"
        "xor t0,t0,t2\n\t"
        "srli t0,t0,24\n\t"
        "bnez t0,9f\n\t"
        "slli t0,t2,7\n\t"
        "srli t0,t0,7\n\t"
        "andi t2,t2,-2\n\t"
        "sw t2,%c[target](sp)\n\t"
        "lw t2,%c[rom_size](a0)\n\t"
        "sltu t0,t0,t2\n\t"
        "xori t0,t0,1\n\t"
        "jalr zero,s3,0\n\t"
        "9: li t0,1\n\t"
        "jalr zero,s3,0\n\t"
        : : [target] "i" (STACK_TARGET), [rom_size] "i" (offsetof(struct Gbn, rom_size)));
}
_Static_assert(GBN_ROM_MAX_SIZE == 1u << 25, "code_target keeps 25 offset bits");
/* The indirect tail's lookup (one backend's table, hub_lookup): a hit
 * enters the target's chain, a miss returns to the block's own exit. */
static uint32_t hub_lookup;
#if GBN_RV32_FAST & 8388608
#define HUB_CONTEXT "lw a3,%c[context](t0)\n\tbne a3,s10,9f\n\t" /* s10 = CONTEXT */
_Static_assert(CONTEXT == 26, "HUB_CONTEXT names s10");
#else
#define HUB_CONTEXT
#endif
LEAF static void indirect_lookup(void) {
    __asm__ volatile(
        "lw t2,%c[target](sp)\n\t"
        "srli a3,t2,1\n\t"
        "li t0,0x9e3779b1\n\t"
        "mul a3,a3,t0\n\t"
        "srli a3,a3,%c[shift]\n\t"
        "slli a3,a3,2\n\t"
        "lui t0,%%hi(%[lookup])\n\t"
        "lw t0,%%lo(%[lookup])(t0)\n\t"
        "add t0,t0,a3\n\t"
        "lw t0,0(t0)\n\t"
        "beqz t0,9f\n\t"
        "lw t1,%c[chain](t0)\n\t"
        "beqz t1,9f\n\t"
        "lw a3,%c[pc](t0)\n\t"
        "bne a3,t2,9f\n\t"
        HUB_CONTEXT
        "jr t1\n\t"
        "9: jalr zero,s3,0\n\t"
        : : [target] "i" (STACK_TARGET), [shift] "i" (GBN_RV32_HASH_SHIFT), [lookup] "s" (&hub_lookup),
            [chain] "i" (offsetof(struct GbnRv32Block, chain)), [pc] "i" (offsetof(struct GbnRv32Block, pc)),
            [context] "i" (offsetof(struct GbnRv32Block, context_tag)));
}

/* tp addresses the leaves: tp itself is unused by the C code, so native
 * code sets it on entry and calls a leaf with one jalr s3, offset(tp). */
static uint32_t hub_base(void) { return (uint32_t) (uintptr_t) read_byte + 2048; }
static bool hub_reaches(void) {
    static const void* const leaves[] = {read_byte, read_half, read_word, write_byte, write_half, write_word, stack_span, code_target, code_target_odd, indirect_lookup};
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(leaves[0]); ++i) {
        int32_t offset = (int32_t) ((uint32_t) (uintptr_t) leaves[i] - hub_base());
        if (offset < -2048 || offset > 2047) return false;
    }
    return true;
}
static void hub_call(struct Emit* e, void (*leaf)(void)) {
    uint32_t offset = (uint32_t) (uintptr_t) leaf - hub_base();
    word(e, (offset & 4095) << 20 | 4u << 15 | WAIT << 7 | 0x67);
}
static bool hub_ready;
static void hub_init(void) { hub_ready = hub_reaches(); }
/* The leaves take the address in TEMP; memory()/arm_memory() computed it in
 * LHS with their last words. Retarget the last one, folding a copy of a
 * guest register into it, instead of adding a move. */
static void address_to_temp(struct Emit* e) {
    uint32_t* code = e->block->code;
    unsigned n = e->count, start = e->access_start;
    e->access_start = ~0u;
    uint32_t last = n ? code[n - 1] : 0;
    bool alu = ((last & 127) == 0x13 || (last & 127) == 0x33) && (last >> 7 & 31) == LHS;
    if (start == ~0u || n < start + 1 || !alu) { imm(e, TEMP, LHS, 0, 0); return; }
    last = (last & ~(31u << 7)) | TEMP << 7;
    uint32_t prev = n >= start + 2 ? code[n - 2] : 0;
    bool copy = (prev & 0xfff07fffu) == (0x13u | LHS << 7); /* addi LHS, x, 0 */
    bool reads_rs2 = (last & 127) == 0x33 && (last >> 20 & 31) == LHS;
    if (copy && (last >> 15 & 31) == LHS && !reads_rs2) {
        last = (last & ~(31u << 15)) | (prev >> 15 & 31) << 15;
        code[n - 2] = last; e->count = n - 1;
    } else code[n - 1] = last;
}
#endif

static void loaded_value(struct Emit* e, unsigned rd, unsigned kind, unsigned rn, int adjustment) {
    if (kind == 3 || kind == 7) {
        unsigned shift = kind == 3 ? 24 : 16;
        imm(e, RESULT, RESULT, 1, shift); imm(e, RESULT, RESULT, 5, 0x400 | shift);
    }
    if (rn != 15) {
        /* Writeback uses RESULT; preserve the loaded value until afterward.
         * ARM base/destination aliases keep the value loaded from memory. */
        imm(e, WAIT, RESULT, 0, 0);
        writeback(e, rn, adjustment);
        put_value(e, rd, WAIT);
    } else put(e, rd);
}

/* Return the immutable classification for an immediate IO read.  The table
 * is compile-time metadata only; the emitted path still checks the live
 * devices pointer and the exact guest address before its first effect. */
static bool static_io_info(uint32_t address, unsigned kind, unsigned* width,
        unsigned* offset, unsigned* flags) {
	if (address >> 24 != 4) return false;
	*width = kind == 4 ? 4 : kind == 5 || kind == 7 ? 2 : 1;
	*offset = address - 0x04000000u;
	if (*offset >= sizeof(native_io_readable) * 2 ||
			*offset + *width > sizeof(((struct GbnDevices*)0)->io)) return false;
	if (*width == 4 && (address & 3)) return false;
	if (*width == 2 && (address & 1)) return false;
	*flags = native_io_readable[*offset >> 1];
	unsigned stored = *width == 4 ? 2u : 1u;
	unsigned fixed = *width == 4 ? 8u : 4u;
	unsigned open = *width == 4 ? 32u : 16u;
	return (*flags & (stored | fixed | open)) != 0;
}

/* The programmed DMA addresses are ordinary masked stores. Prove the exact
 * address before either store, and preserve the active cursor, latch and
 * schedule. Count/control writes still use the existing DMA startup path. */
static bool static_dma_store(struct Emit* e, const struct Gbn* m, uint32_t pc,
        uint32_t address, unsigned rd, bool exact) {
	unsigned offset = address - 0x040000b0u;
	if (offset >= 48 || (offset & 3)) return false;
	unsigned field = native_dma_address_field[offset >> 2];
	if (!field) return false;
	if (!exact) {
		constant(e, RESULT, address);
		stop(e, LHS, RESULT, 1, true);
	}
	unsigned devices = RESULT;
	load(e, devices, offsetof(struct Gbn, devices));
	if (!e->io_guarded) { guard(e, devices, true); e->io_guarded = true; }
	get(e, WAIT, rd, pc);
	constant(e, RHS, field & 0x8000 ? 0x07fffffeu : 0x0ffffffeu);
	op(e, WAIT, WAIT, RHS, 7, 0);
	unsigned displacement = field & 2047;
	word(e, (displacement >> 5) << 25 | WAIT << 20 | devices << 15 |
		2 << 12 | (displacement & 31) << 7 | 0x23);
	displacement = offset + 0xb0;
	word(e, (displacement >> 5) << 25 | WAIT << 20 | devices << 15 |
		2 << 12 | (displacement & 31) << 7 | 0x23);
	e->read_only = false;
	constant(e, WAIT, 1); io_wait(e, m, pc, 1);
	return true;
}

/* Specialize a Thumb immediate IO load, guarded against its actual address.
 * Stored values stay live; open-bus reads still select the current DMA latch
 * or pipeline halfword. Dynamic registers retain the complete bus path. */
static bool static_io_read(struct Emit* e, const struct Gbn* m, uint32_t pc,
        uint32_t address, unsigned rd, unsigned kind, unsigned rn, int adjustment,
	bool exact) {
	unsigned width, offset, flags;
	if (!static_io_info(address, kind, &width, &offset, &flags)) return false;
	unsigned fixed = width == 4 ? 8u : 4u;
	unsigned open = width == 4 ? 32u : 16u;
	e->read_seen = true;
	if (!exact) {
		constant(e, RESULT, address);
		stop(e, LHS, RESULT, 1, true);
	}
	unsigned devices = RESULT;
	load(e, devices, offsetof(struct Gbn, devices));
	if (!e->io_guarded) { guard(e, devices, true); e->io_guarded = true; }
	if (flags & open) {
		constant(e, LHS, address);
		imm(e, TEMP, LHS, 0, 0);
		open_bus_value(e, m, pc, rd, kind, width, rn, adjustment);
		constant(e, WAIT, 2);
		loaded_value(e, rd, kind, rn, adjustment);
		io_wait(e, m, pc, 2);
		return true;
	}
	if (flags & fixed) {
		if (width == 4) {
			/* DMA count/control word: the low count half reads as zero,
			 * while the high control half remains visible. */
			word(e, (offsetof(struct GbnDevices, io) + offset) << 20 |
				devices << 15 | 2 << 12 | RESULT << 7 | 3);
			imm(e, RESULT, RESULT, 5, 16);
			imm(e, RESULT, RESULT, 1, 16);
		} else constant(e, RESULT, 0);
	} else {
		unsigned fn = width == 4 ? 2 : width == 2 ? (kind == 7 ? 1 : 5) : (kind == 3 ? 0 : 4);
		word(e, (offsetof(struct GbnDevices, io) + offset) << 20 |
			devices << 15 | fn << 12 | RESULT << 7 | 3);
	}
	loaded_value(e, rd, kind, rn, adjustment);
	constant(e, WAIT, 2);
	io_wait(e, m, pc, 2);
	return true;
}

/* One body for a consecutive run of pure word IO operations. The entry
 * proves every address and instruction boundary before any guest effect.
 * A miss executes the first instruction through the interpreter; there is
 * no duplicated native slow body or persistent proof register. */
static unsigned thumb_io_group(struct Emit* e, struct GbnRv32* backend,
        const struct Gbn* m, unsigned first, unsigned first_code) {
	if (!e->block->thumb || m->code_region < 8 || !(m->waitcnt & 0x4000) || !m->code_wait) return 0;
	struct { unsigned code, offset, flags, field; } ops[8];
	uint32_t pc = e->block->pc + 2 * first;
	unsigned n = 0, rs = first_code >> 3 & 7;
	bool exact = (e->thumb_literal_mask & 1u << rs) != 0, open = false;
	if (!exact && (e->written & 1u << rs)) return 0;
	uint32_t base = exact ? e->thumb_literal[rs] : m->cpu.r[rs];
	if ((base + (first_code >> 6 & 31) * 4) >> 24 != 4) return 0;
	while (n < 8 && first + n < GBN_RV32_INSNS) {
		uint32_t address = pc + 2 * n;
		unsigned offset = address & m->code_mask;
		if (address >> 24 != m->code_region || offset >= m->rom_size || m->rom_size - offset < 2) break;
		unsigned code = n ? fetch(m, address, 2) : first_code, rn = code >> 3 & 7, rd = code & 7;
		if ((code & 0xf000) != 0x6000 || ((code & 0x800) && rd == rn)) break;
		if (rn != rs) break;
		uint32_t effective = base + (code >> 6 & 31) * 4;
		unsigned flags = 0, field = 0;
		if (code & 0x800) {
			unsigned width;
			if (!static_io_info(effective, 4, &width, &offset, &flags)) break;
		} else {
			offset = effective - 0x040000b0u;
			if (offset >= 48 || (offset & 3) || !(field = native_dma_address_field[offset >> 2])) break;
			offset += 0xb0;
		}
		ops[n].code = code; ops[n].offset = offset;
		ops[n].flags = flags; ops[n].field = field;
		open |= (flags & 32) != 0;
		++n;
	}
	/* Bound the body plus the existing tail, exit stubs and entry proof.
	 * Each operation uses at most nine words; the fixed prefix/suffix uses
	 * fewer than 80, including optional diagnostics and large immediates. */
	if (n < 3 || e->count + 180 + 340 + 13 * (first + n) >= GBN_RV32_WORDS) return 0;
	constant(e, RESULT, first + n); stop(e, LIMIT, RESULT, 6, true);
	unsigned last_start = (n - 1) * (1u + m->code_wait);
	if (last_start < 2048) imm(e, RESULT, NOW, 0, last_start);
	else { constant(e, RESULT, last_start); op(e, RESULT, NOW, RESULT, 0, 0); }
	stop(e, RESULT, ZERO, 5, true);
	if (!exact) { constant(e, RESULT, base); stop(e, guest[rs], RESULT, 1, true); }
	load(e, TEMP, offsetof(struct Gbn, devices)); guard(e, TEMP, true); e->io_guarded = true;
	/* These pure accesses cannot change DMA access/valid/PC/bus. Preserve
	 * the snapshot in scratch registers only for the duration of this run. */
	if (open) {
		word(e, offsetof(struct GbnDevices, dma_access) << 20 | TEMP << 15 | 4 << 12 | WAIT << 7 | 3);
		word(e, offsetof(struct GbnDevices, dma_bus_valid) << 20 | TEMP << 15 | 4 << 12 | RESULT << 7 | 3);
		word(e, offsetof(struct GbnDevices, dma_pc) << 20 | TEMP << 15 | 2 << 12 | LHS << 7 | 3);
		word(e, beq(RESULT, ZERO, 8) | 1u << 12); constant(e, LHS, 0);
		word(e, offsetof(struct GbnDevices, dma_bus) << 20 | TEMP << 15 | 5 << 12 | RHS << 7 | 3);
		imm(e, RESULT, RHS, 1, 16); op(e, RHS, RHS, RESULT, 6, 0);
	}
	for (unsigned j = 0; j < n; ++j) {
		unsigned rd = ops[j].code & 7, off = ops[j].offset;
		if (!(ops[j].code & 0x800)) {
			constant(e, RESULT, ops[j].field & 0x8000 ? 0x07fffffeu : 0x0ffffffeu);
			op(e, RESULT, guest[rd], RESULT, 7, 0);
			unsigned field = ops[j].field & 2047;
			word(e, (field >> 5) << 25 | RESULT << 20 | TEMP << 15 | 2 << 12 | (field & 31) << 7 | 0x23);
			word(e, (off >> 5) << 25 | RESULT << 20 | TEMP << 15 | 2 << 12 | (off & 31) << 7 | 0x23);
			e->read_only = false;
		} else {
			if (ops[j].flags & 32) {
				constant(e, RESULT, pc + 2 * j + 2);
				unsigned active = e->count; word(e, 0);
				unsigned visible = e->count; word(e, 0);
				uint32_t value = fetch(m, pc + 2 * j + 4, 2) & 65535;
				constant(e, guest[rd], value | value << 16);
				unsigned done = e->count; word(e, 0);
				e->block->code[active] = beq(WAIT, ZERO, 4 * (e->count - active)) | 1u << 12;
				e->block->code[visible] = beq(LHS, RESULT, 4 * (e->count - visible));
				imm(e, guest[rd], RHS, 0, 0);
				e->block->code[done] = beq(ZERO, ZERO, 4 * (e->count - done));
			} else if (ops[j].flags & 8) {
				word(e, (off + 2) << 20 | TEMP << 15 | 5 << 12 | guest[rd] << 7 | 3);
				imm(e, guest[rd], guest[rd], 1, 16);
			} else word(e, off << 20 | TEMP << 15 | 2 << 12 | guest[rd] << 7 | 3);
			put_value(e, rd, guest[rd]); e->read_seen = true;
		}
	}
	/* Repeated short waits clamp the same original queue against increasing
	 * fetch PCs. At the final PC, only 16-2*(n-1) bytes of that initial range
	 * can survive. Eight accesses drain it completely. */
	uint32_t fetch_pc = pc + 2 * (n - 1) + 4;
	if (n == 8) constant(e, PREFETCH, fetch_pc);
	else {
		constant(e, TEMP, fetch_pc); op(e, LHS, PREFETCH, TEMP, 0, 0x20);
		imm(e, RESULT, LHS, 3, 16 - 2 * (n - 1)); op(e, RESULT, ZERO, RESULT, 0, 0x20);
		op(e, LHS, LHS, RESULT, 7, 0); imm(e, LHS, LHS, 7, -2); op(e, PREFETCH, TEMP, LHS, 0, 0);
	}
	unsigned elapsed = n * (1u + m->code_wait);
	if (elapsed < 2048) imm(e, NOW, NOW, 0, elapsed);
	else { constant(e, RESULT, elapsed); op(e, NOW, NOW, RESULT, 0, 0); }
#if GBN_RV32_STATS
	constant(e, LHS, (uint32_t)(uintptr_t)&backend->fused_thumb_io_instructions);
	word(e, LHS << 15 | 2 << 12 | TEMP << 7 | 3); imm(e, RESULT, TEMP, 0, n);
	op(e, WAIT, RESULT, TEMP, 3, 0); word(e, RESULT << 20 | LHS << 15 | 2 << 12 | 0x23);
	word(e, 4u << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3); op(e, TEMP, TEMP, WAIT, 0, 0);
	word(e, TEMP << 20 | LHS << 15 | 2 << 12 | 4u << 7 | 0x23);
#else
	(void)backend;
#endif
	e->block->instructions += n;
	return n;
}

static void leaf_read(struct Emit* e, const struct Gbn* m, uint32_t pc,
        unsigned rd, unsigned kind, unsigned rn, int adjustment) {
    unsigned width = kind == 4 ? 4 : kind == 5 || kind == 7 ? 2 : 1;
    e->read_seen = true;
#if (GBN_RV32_FAST & 4194304) && (GBN_RV32_FAST & 512) && (GBN_RV32_FAST & 2)
    if (hub_ready) {
        /* One call through tp and one branch for the miss and open bus. */
        address_to_temp(e);
        hub_call(e, width == 4 ? read_word : width == 2 ? read_half : read_byte);
        unsigned slow = e->count; word(e, beq(RHS, ZERO, 0) | 1u << 12);
        loaded_value(e, rd, kind, rn, adjustment);
        cold_begin(e, slow);
        imm(e, WAIT, RHS, 0, -1); guard(e, WAIT, true);
        open_bus_value(e, m, pc, rd, kind, width, rn, adjustment);
        constant(e, WAIT, 2); ram_wait(e, m, pc);
        cold_end(e);
        return;
    }
#endif
    imm(e, TEMP, LHS, 0, 0);
    constant(e, WAIT, (uint32_t) (uintptr_t) (width == 4 ? read_word : width == 2 ? read_half : read_byte));
    word(e, WAIT << 15 | WAIT << 7 | 0x67);
    guard(e, RHS, true);
#if GBN_RV32_FAST & 2
    /* Fast mode: a cartridge read counts like a RAM read, with the helper's
     * wait (LHS). Open bus (class 2) stays native: the interpreter's bus has
     * no open-bus path for unmapped regions. */
    {
        constant(e, WAIT, 2);
#if GBN_RV32_FAST & 512
        unsigned to_open_bus = e->count; word(e, beq(RHS, WAIT, 0));
        loaded_value(e, rd, kind, rn, adjustment);
        op(e, NOW, NOW, LHS, 0, 0);
        cold_begin(e, to_open_bus);
        open_bus_value(e, m, pc, rd, kind, width, rn, adjustment);
        constant(e, WAIT, 2); ram_wait(e, m, pc);
        cold_end(e);
        return;
#endif
        unsigned open = e->count; word(e, 0);
        loaded_value(e, rd, kind, rn, adjustment);
        op(e, NOW, NOW, LHS, 0, 0);
        unsigned done = e->count; word(e, 0);
        e->block->code[open] = beq(RHS, WAIT, 4 * (e->count - open));
        open_bus_value(e, m, pc, rd, kind, width, rn, adjustment);
        constant(e, WAIT, 2); ram_wait(e, m, pc);
        e->block->code[done] = beq(ZERO, ZERO, 4 * (e->count - done));
        return;
    }
#endif
    constant(e, WAIT, 2);
    unsigned open = e->count; word(e, 0);
    unsigned cartridge = 0;
    bool prefetched = m->code_region >= 8 && (m->waitcnt & 0x4000);
    if (prefetched) { cartridge = e->count; word(e, 0); }
    loaded_value(e, rd, kind, rn, adjustment);
    imm(e, WAIT, LHS, 0, 0); ram_wait(e, m, pc);
    unsigned done[2], done_count = 0;
    done[done_count++] = e->count; word(e, 0);
    if (prefetched) {
        e->block->code[cartridge] = beq(RHS, ZERO, 4 * (e->count - cartridge)) | 4u << 12;
        loaded_value(e, rd, kind, rn, adjustment);
        imm(e, NOW, NOW, 0, (int) m->code_nonseq - (int) m->code_wait);
        op(e, NOW, NOW, LHS, 0, 0);
        done[done_count++] = e->count; word(e, 0);
    }
    e->block->code[open] = beq(RHS, WAIT, 4 * (e->count - open));
    open_bus_value(e, m, pc, rd, kind, width, rn, adjustment);
    constant(e, WAIT, 2); ram_wait(e, m, pc);
    for (unsigned i = 0; i < done_count; ++i)
        e->block->code[done[i]] = beq(ZERO, ZERO, 4 * (e->count - done[i]));
}

static void direct_access(struct Emit* e, const struct Gbn* m, uint32_t pc, unsigned rd, unsigned kind, unsigned rn, int adjustment) {
    if (kind > 2) { leaf_read(e, m, pc, rd, kind, rn, adjustment); return; }
	e->read_only = false;
	static const unsigned widths[] = {4, 2, 1, 1, 4, 2, 1, 2};
	unsigned width = widths[kind];
#if (GBN_RV32_FAST & 4194304) && (GBN_RV32_FAST & 512)
	if (hub_ready && m->code_region >= 8) {
		/* ROM code: a RAM store is one leaf call (no code to protect);
		 * anything else leaves as before, word stores through the DMA
		 * register paths. */
		address_to_temp(e);
		get(e, RESULT, rd, pc);
		hub_call(e, width == 4 ? write_word : width == 2 ? write_half : write_byte);
		if (width == 4) {
			unsigned other = e->count; word(e, beq(RHS, ZERO, 0) | 1u << 12);
			writeback(e, rn, adjustment);
			cold_begin(e, other);
			dma_start_emit(e, m, pc, rd, rn, adjustment);
			dma_address_store(e, m, pc, rd, rn, adjustment);
			cold_end(e);
		} else {
			guard(e, RHS, false);
			writeback(e, rn, adjustment);
		}
		return;
	}
#endif
	imm(e, TEMP, LHS, 0, 0); /* original address */
	imm(e, RHS, LHS, 5, 24); imm(e, RHS, RHS, 0, -2);
	imm(e, RESULT, RHS, 3, 2); /* region 2 or 3 */
	unsigned cartridge = 0;
#if GBN_RV32_FAST & 512
	if (width == 4) { cartridge = e->count; word(e, beq(RESULT, ZERO, 0)); }
#else
	if (width == 4) { cartridge = e->count; word(e, 0); }
#endif
	else guard(e, RESULT, true);
	if (m->code_region < 8) {
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
		/* Other-bank stores skip this invalidation. A later guard failure
		 * may conservatively advance the epoch without committing a store. */
		imm(e, RAM_EPOCH, RAM_EPOCH, 0, 1);
		e->block->code[other_bank] = beq(RHS, RESULT, 4 * (e->count - other_bank)) | 1u << 12;
		/* RESULT was the bank discriminator at the branch, before clobbering. */
	}
	load(e, RESULT, offsetof(struct Gbn, ewram)); constant(e, LHS, GBN_EWRAM_SIZE - 1);
	constant(e, WAIT, (m->ewram_wait + 1u) * (width == 4 ? 2u : 1u));
	unsigned choose = e->count; word(e, 0);
	load(e, RESULT, offsetof(struct Gbn, iwram)); constant(e, LHS, GBN_IWRAM_SIZE - 1);
	constant(e, WAIT, 1u);
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
#if GBN_RV32_FAST & 512
		if (width == 4) {
			cold_begin(e, cartridge);
			dma_start_emit(e, m, pc, rd, rn, adjustment);
			dma_address_store(e, m, pc, rd, rn, adjustment);
			cold_end(e);
		}
		return;
#endif
		if (width == 4) {
			unsigned done = e->count; word(e, 0);
			e->block->code[cartridge] = beq(RESULT, ZERO, 4 * (e->count - cartridge));
			dma_start_emit(e, m, pc, rd, rn, adjustment);
			dma_address_store(e, m, pc, rd, rn, adjustment);
			e->block->code[done] = beq(ZERO, ZERO, 4 * (e->count - done));
		}
		return;
}

/* Direct RAM operations stay in this block, retaining the live registers and
 * prefetch/time state. Guards precede all guest effects and exit at this
 * instruction for IO, unaligned or unmapped accesses. */
static bool memory(struct Emit* e, const struct Gbn* m, unsigned code, uint32_t pc) {
	unsigned rd = code & 7, rs = code >> 3 & 7, kind;
#if GBN_RV32_FAST & 4194304
	e->access_start = e->count;
#endif
	uint32_t effective = 0, base_value = 0;
	bool thumb_immediate = false;
	bool thumb_exact = false;
	unsigned thumb_scale = 0;
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
		e->thumb_literal[rd] = value; e->thumb_literal_mask |= 1u << rd;
		/* ROM word read + internal cycle + fetch_break, same ROM window. */
#if (GBN_RV32_FAST & 4194304) && (GBN_RV32_FAST & 1024)
		e->deferred += 3 + 2 * m->code_nonseq; /* a constant: with the fetch time */
#else
		imm(e, NOW, NOW, 0, 3 + 2 * m->code_nonseq);
#endif
		return true;
	} else if ((code & 0xf000) == 0x5000) {
		kind = code >> 9 & 7;
#if GBN_RV32_FAST & 4194304
		if (hub_ready) {
			unsigned a = source(e, LHS, rs, pc);
			op(e, LHS, a, source(e, RHS, code >> 6 & 7, pc), 0, 0);
		} else
#endif
		{ get(e, LHS, rs, pc); get(e, RHS, code >> 6 & 7, pc); op(e, LHS, LHS, RHS, 0, 0); }
	} else if (code >= 0x6000 && code < 0x9000) {
		thumb_exact = (e->thumb_literal_mask & 1u << rs) != 0;
		thumb_immediate = thumb_exact || !(e->written & 1u << rs);
		if (code < 0x8000) { kind = (code & 0x1000 ? 2 : 0) + (code & 0x800 ? 4 : 0); thumb_scale = code & 0x1000 ? 1 : 4; }
		else { kind = code & 0x800 ? 5 : 1; thumb_scale = 2; }
		base_value = e->thumb_literal_mask & 1u << rs ? e->thumb_literal[rs] : m->cpu.r[rs];
		effective = base_value + (code >> 6 & 31) * thumb_scale;
		if (!thumb_exact) {
			get(e, LHS, rs, pc);
			imm(e, LHS, LHS, 0, (int) (code >> 6 & 31) * (int) thumb_scale);
		}
	} else if ((code & 0xf000) == 0x9000) {
		kind = code & 0x800 ? 4 : 0; rd = code >> 8 & 7;
		get(e, LHS, 13, pc); imm(e, LHS, LHS, 0, (int) (code & 255) * 4);
	} else return false;
	if (thumb_immediate) {
		if (kind > 2 && static_io_read(e, m, pc, effective, rd, kind, 15, 0, thumb_exact)) return true;
		if (kind == 0 && static_dma_store(e, m, pc, effective, rd, thumb_exact)) return true;
	}
	if (thumb_exact) {
		get(e, LHS, rs, pc);
		imm(e, LHS, LHS, 0, (int) (code >> 6 & 31) * (int) thumb_scale);
	}
	direct_access(e, m, pc, rd, kind, 15, 0);
	return true;
}

/* Stack transfers use one RAM mapping and one aggregate bus stall, as the
 * interpreter's multiple() does. Validate the whole span before committing
 * SP, low registers or memory. Rare mirrored-end spans use the bus path. */
static void indirect_target(struct Emit* e, const struct Gbn* m);
static bool thumb_stack(struct Emit* e, const struct Gbn* m, unsigned code, uint32_t pc) {
	bool pop = (code & 0x800) != 0, extra = (code & 0x100) != 0;
	unsigned list = code & 255, count = extra;
	for (unsigned r = 0; r < 8; ++r) count += list >> r & 1;
	if (!count) return false; /* ARM7 empty-list PC/writeback quirks stay in C. */
	unsigned bytes = 4 * count;
	if (pop) e->read_seen = true;
	else e->read_only = false;
#if (GBN_RV32_FAST & 4194304) && (GBN_RV32_FAST & 2)
	if (hub_ready && m->code_region >= 8) {
		/* ROM code: the span's checks and time in one leaf call. */
		get(e, TEMP, 13, pc);
		if (!pop) imm(e, TEMP, TEMP, 0, -(int) bytes);
		constant(e, RESULT, bytes | pop);
		hub_call(e, stack_span);
		guard(e, RHS, true);
		if (pop && extra) {
			word(e, (bytes - 4) << 20 | RHS << 15 | 2 << 12 | RESULT << 7 | 3);
			indirect_target(e, m);
		}
		if (pop) imm(e, TEMP, TEMP, 0, (int) bytes);
		put_value(e, 13, TEMP);
		unsigned offset = 0;
		for (unsigned r = 0; r < 8; ++r) if (list & (1u << r)) {
			if (pop) {
				word(e, offset << 20 | RHS << 15 | 2 << 12 | guest[r] << 7 | 3);
				e->written |= 1u << r;
				e->thumb_literal_mask &= ~(1u << r);
			} else word(e, (offset >> 5) << 25 | guest[r] << 20 | RHS << 15 | 2 << 12 | (offset & 31) << 7 | 0x23);
			offset += 4;
		}
		if (!pop && extra)
			word(e, (offset >> 5) << 25 | guest[14] << 20 | RHS << 15 | 2 << 12 | (offset & 31) << 7 | 0x23);
		return true;
	}
#endif
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
		imm(e, RAM_EPOCH, RAM_EPOCH, 0, 1);
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
			e->thumb_literal_mask &= ~(1u << r);
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
#if GBN_RV32_FAST & 4194304
    if (hub_ready && m->code_region >= 8) {
        word(e, m->code_region << 24 | LHS << 7 | 0x37);
        hub_call(e, code_target); guard(e, LHS, false);
        e->dynamic_pc = true;
        return;
    }
#endif
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
#if GBN_RV32_FAST & 16384
			if (e->live & 2)
#endif
			{
			imm(e, RHS, a, 5, (int) (kind ? amount - 1 : 32 - amount));
			imm(e, RHS, RHS, 7, 1); imm(e, RHS, RHS, 1, 29);
			constant(e, TEMP, ~0x20000000u); op(e, FLAGS, FLAGS, TEMP, 7, 0);
			op(e, FLAGS, FLAGS, RHS, 6, 0);
			}
			if (amount == 32 && kind == 1) imm(e, RESULT, ZERO, 0, 0);
			else imm(e, RESULT, a, kind ? 5 : 1, (int) (amount == 32 ? 31 : amount) | (kind == 2 ? 0x400 : 0));
		}
		nz(e); put(e, rd);
	} else if (code < 0x2000) {
		unsigned a = source(e, LHS, rs, pc), b;
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 16384)
		if ((code & 0x400) && !(e->live & 3)) {
			/* C and V dead: an immediate add/sub is one addi. */
			int v = (int) (code >> 6 & 7);
			imm(e, RESULT, a, 0, code & 0x200 ? -v : v); nz(e); put(e, rd);
			return true;
		}
#endif
		if (code & 0x400) { constant(e, RHS, code >> 6 & 7); b = RHS; }
		else b = source(e, RHS, code >> 6 & 7, pc);
		arithmetic(e, (code & 0x200) != 0, a, b); put(e, rd);
	} else if (code < 0x4000) {
		rd = code >> 8 & 7;
		unsigned kind = code >> 11 & 3;
		if (!kind) { constant(e, RESULT, code & 255); nz(e); }
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 16384)
		else if (kind == 1 && !(e->live & 3) && !(code & 255)) nz_of(e, source(e, LHS, rd, pc)); /* CMP #0 */
		else if (!(e->live & 3)) {
			int v = (int) (code & 255);
			imm(e, RESULT, source(e, LHS, rd, pc), 0, kind == 2 ? v : -v); nz(e);
		}
#endif
		else { unsigned a = source(e, LHS, rd, pc); constant(e, RHS, code & 255); arithmetic(e, kind != 2, a, RHS); }
		if (kind != 1) put(e, rd);
	} else if (code < 0x4400) {
		unsigned kind = code >> 6 & 15;
#if (GBN_RV32_FAST & 131072) && (GBN_RV32_FAST & 16384)
		if (kind >= 2 && kind <= 4 && !(e->live & 2)) {
			/* LSL/LSR/ASR by register with a dead C: amounts 32..255 give
			 * zero (LSL/LSR) or the sign (ASR); one internal cycle. */
			unsigned a = source(e, LHS, rd, pc), b = source(e, RHS, rs, pc);
			imm(e, TEMP, b, 7, 255);
			imm(e, WAIT, TEMP, 3, 32);
			if (kind == 4) {
				word(e, beq(WAIT, ZERO, 8) | 1u << 12);
				imm(e, TEMP, ZERO, 0, 31);
				op(e, RESULT, a, TEMP, 5, 0x20);
			} else {
				op(e, RESULT, a, TEMP, kind == 2 ? 1 : 5, 0);
				op(e, WAIT, ZERO, WAIT, 0, 0x20);
				op(e, RESULT, RESULT, WAIT, 7, 0);
			}
			nz(e); put(e, rd);
			imm(e, NOW, NOW, 0, 1);
			return true;
		}
#endif
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
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 4194304)
        if (hub_ready && m->code_region >= 8) {
            word(e, m->code_region << 24 | LHS << 7 | 0x37);
            hub_call(e, code_target_odd); guard(e, LHS, false);
            e->dynamic_pc = true;
            return true;
        }
#endif
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
	} else if ((code & 0xf000) == 0xc000 && (code & 255) && !((code & 255) & ((code & 255) - 1))) {
        /* A one-register RAM LDM/STM has the same bus/internal/fetch cost as
         * the corresponding word transfer. LDM's loaded base wins writeback.
         * ROM multiple timing differs, so preserve its interpreter path. */
        unsigned base=code>>8&7,reg=(unsigned)__builtin_ctz(code&255);
        get(e,LHS,base,pc);
        imm(e,RHS,LHS,5,24);imm(e,RHS,RHS,0,-2);imm(e,RESULT,RHS,3,2);
        guard(e,RESULT,true);
        direct_access(e,m,pc,reg,(code&0x800)?4:0,base,4);
	} else if ((code & 0xf800) == 0xe000 || ((code & 0xf000) == 0xd000 && (code >> 8 & 15) < 14)) {
		bool unconditional = (code & 0xf800) == 0xe000;
		int32_t offset = unconditional ? (int32_t) (code << 21) >> 20 : (int32_t) (code << 24) >> 23;
		uint32_t target = pc + 4 + (uint32_t) offset;
		if (target >> 24 != m->code_region || (m->code_region >= 8 && (target & (GBN_ROM_MAX_SIZE - 1)) >= m->rom_size)) return false;
		e->block->target = target; e->block->branch = true;
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 128)
		/* The tail tests the condition itself: N and Z directly. */
		e->branch_direct = true; e->branch_always = unconditional;
		unsigned cond = code >> 8 & 15;
		if (unconditional) ;
		else if (cond < 2 || (cond >= 4 && cond < 6)) {
			e->branch_reg = cond < 2 ? IS_ZERO : NEGATIVE; e->branch_clear = cond & 1;
		} else { condition(e, cond); e->branch_reg = TEMP; e->branch_clear = false; }
		return true;
#endif
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
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 128)
        e->branch_direct = e->branch_always = true;
        return true;
#endif
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

/* A proven pure segment may discard an intermediate shifter carry.
 * Preserve the shifted operand, including RRX's incoming CPSR carry. */
static void arm_shift_value(struct Emit* e, unsigned kind, unsigned amount) {
    if (!kind) { if (amount) imm(e, RHS, RHS, 1, (int) amount); }
    else if (kind == 3) {
        if (!amount) arm_shift_imm(e, kind, amount);
        else {
            imm(e, TEMP, RHS, 1, (int) (32 - amount));
            imm(e, RHS, RHS, 5, (int) amount); op(e, RHS, RHS, TEMP, 6, 0);
        }
    } else if (kind == 1 && !amount) constant(e, RHS, 0);
    else imm(e, RHS, RHS, 5, (int) (amount ? amount : 31) | (kind == 2 ? 0x400 : 0));
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
#if GBN_RV32_FAST & 4194304
	e->access_start = e->count;
#endif
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

/* ARM immediates can use RV32 immediates directly. Carry remains an exact
 * separately observable ARM shifter result even when S is clear. */
static bool arm_immediate(struct Emit* e, uint32_t code, uint32_t pc) {
    if(!(code&0x2000000) || (code&0x100000)) return false;
    unsigned kind=code>>21&15,rd=code>>12&15,rn=code>>16&15;
    if(rd==15 || (kind>=5 && kind<=11)) return false;
    unsigned rotation=code>>7&30;
    uint32_t value=code&255;
    if(rotation) value=value>>rotation | value<<(32-rotation);
    uint32_t operand=kind==2 ? 0u-value : kind==14 ? ~value : value;
    int immediate=(int32_t)operand;
    bool simple=immediate>=-2048 && immediate<2048;
    bool mask=(kind==0 || kind==14) && operand;
    unsigned direction=0,shift=0;
    if(mask && !(operand&(operand+1))) {direction=1;shift=(unsigned)__builtin_clz(operand);}
    else if(mask && !(~operand&(~operand+1))) {direction=2;shift=(unsigned)__builtin_ctz(operand);}
    if(kind!=13 && kind!=15 && !((kind==0 || kind==1 || kind==2 || kind==4 || kind==12 || kind==14) && simple)
            && !direction) return false;
    if(!e->skip_shifter) {
    if(rotation) constant(e,WAIT,value>>31);
    else {imm(e,WAIT,FLAGS,5,29);imm(e,WAIT,WAIT,7,1);}
    store(e,WAIT,offsetof(struct Gbn,cpu.shifter_carry));
    }
    unsigned dest=resident(rd)?guest[rd]:RESULT;
    if(kind==13 || kind==15) constant(e,dest,kind==13?value:~value);
    else {
        unsigned a=source(e,LHS,rn,pc);
        if(simple) imm(e,dest,a,kind==0 || kind==14 ? 7 : kind==1 ? 4 : kind==12 ? 6 : 0,immediate);
        else {
            imm(e,dest,a,direction==1?1:5,(int)shift);
            imm(e,dest,dest,direction==1?5:1,(int)shift);
        }
    }
    put_value(e,rd,dest);return true;
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
			unsigned a = source(e, LHS, code & 15, pc), b = source(e, RHS, code >> 8 & 15, pc);
            multiply_wait(e, b, accumulate); op(e, RESULT, a, b, 0, 1);
			if (accumulate) { unsigned addend = source(e, TEMP, rn, pc); op(e, RESULT, RESULT, addend, 0, 0); }
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
        if(arm_immediate(e,code,pc))goto arm_done;
        bool arithmetic_op = (kind >= 2 && kind <= 7) || kind == 10 || kind == 11;
        bool need_carry = !e->skip_shifter || (set && !arithmetic_op);
        unsigned a, b = RHS;
        if (code & 0x2000000) {
			unsigned rotation = code >> 7 & 30;
			uint32_t value = code & 255;
			if (rotation) value = value >> rotation | value << (32 - rotation);
			constant(e, RHS, value);
            if (need_carry) {
                if (rotation) constant(e, WAIT, value >> 31);
                else { imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1); }
            }
        } else if (!(code & 0xff0)) {
            b = source(e, RHS, code & 15, pc);
            if (need_carry) { imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1); }
        } else {
            get(e, RHS, code & 15, pc);
            if (need_carry) arm_shift_imm(e, code >> 5 & 3, code >> 7 & 31);
            else arm_shift_value(e, code >> 5 & 3, code >> 7 & 31);
        }
        if (!e->skip_shifter) store(e, WAIT, offsetof(struct Gbn, cpu.shifter_carry));
        a = source(e, LHS, rn, pc);
		if (arithmetic_op) {
			bool reverse = kind == 3 || kind == 7;
			bool subtract = kind == 2 || kind == 3 || kind == 6 || kind == 7 || kind == 10;
			bool carry_in = kind >= 5 && kind <= 7;
			if (reverse) { imm(e, TEMP, a, 0, 0); imm(e, LHS, b, 0, 0); imm(e, RHS, TEMP, 0, 0); a = LHS; b = RHS; }
			if (set) {
				if (carry_in) arithmetic_carry(e, subtract, a, b); else arithmetic(e, subtract, a, b);
			} else {
				op(e, RESULT, a, b, 0, subtract ? 0x20 : 0);
				if (carry_in) {
					imm(e, WAIT, FLAGS, 5, 29); imm(e, WAIT, WAIT, 7, 1);
					if (subtract) imm(e, WAIT, WAIT, 4, 1);
					op(e, RESULT, RESULT, WAIT, 0, subtract ? 0x20 : 0);
				}
			}
		} else {
			if (kind == 14 || kind == 15) { imm(e, RHS, b, 4, -1); b = RHS; }
			if (kind == 13 || kind == 15) imm(e, RESULT, b, 0, 0);
			else op(e, RESULT, a, b, kind == 1 || kind == 9 ? 4 : kind == 12 ? 6 : 7, 0);
			if (set) {
				constant(e, TEMP, ~0x20000000u); op(e, FLAGS, FLAGS, TEMP, 7, 0);
				imm(e, WAIT, WAIT, 1, 29); op(e, FLAGS, FLAGS, WAIT, 6, 0); nz(e);
			}
		}
		if (!test) put(e, rd);
	} else return false;
arm_done:
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
	op(e, RHS, ZERO, NOW, 0, 0x20); op(e, RHS, RHS, TEMP, 5, 1); /* divu: full time intervals */
	constant(e, WAIT, e->block->instructions); op(e, LHS, LIMIT, WAIT, 5, 1);
	word(e, beq(LHS, RHS, 8) | 6u << 12); /* bltu cap,time,+8 */
	imm(e, LHS, RHS, 0, 0);
	op(e, TEMP, LHS, TEMP, 0, 1); op(e, NOW, NOW, TEMP, 0, 0);
	op(e, RESULT, LHS, WAIT, 0, 1);
	op(e, LIMIT, LIMIT, RESULT, 0, 0x20);
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

/* All native entries share one register frame, including the immutable
 * context tag and RAM-write epoch. Edges check the destination PC and live
 * chain pointer; the target entry proves its context and RAM bytes. Replacing
 * a descriptor or recycling code needs no executable patching or reverse links. */
/* Dynamic successors use the same keyed descriptors and RAM snapshot entry
 * as direct edges. STACK_TARGET preserves the guest PC through all scratch
 * registers and miss paths; the entry proof is shared with direct edges. */
/* The initial cap minus the remaining cap gives the completed chain count.
 * Compute it only on return; x20 can hold guest r11 instead of a second counter. */
static void finish_count(struct Emit* e, unsigned partial, bool fallback) {
    stack(e, TEMP, INITIAL_CAP, false);
    op(e, TEMP, TEMP, LIMIT, 0, 0x20);
    constant(e, LIMIT, partial | (fallback ? 0x80000000u : 0));
    op(e, LIMIT, LIMIT, TEMP, 0, 0);
}
static void indirect_tail(struct Emit* e, struct GbnRv32* backend) {
    struct GbnRv32Block* b = e->block;
#if GBN_RV32_FAST & 1024
    time_flush(e);
#endif
    imm(e, LIMIT, LIMIT, 0, -(int) b->instructions);
    unsigned misses[8], n = 0;
    misses[n++] = e->count; word(e, beq(LIMIT, ZERO, 0));
    misses[n++] = e->count; word(e, beq(NOW, ZERO, 0) | 5u << 12);
#if (GBN_RV32_FAST & 4194304) && !GBN_RV32_STATS
    uint32_t table = (uint32_t) (uintptr_t) backend->lookup;
    if (hub_ready && (!hub_lookup || hub_lookup == table)) {
        hub_lookup = table;
        hub_call(e, indirect_lookup); /* returns only on a miss */
        for (unsigned i = 0; i < n; ++i)
            b->code[misses[i]] |= beq(0, 0, 4 * (e->count - misses[i])) & 0xfe000f80u;
        finish_count(e, 0, false); stack(e, RHS, STACK_TARGET, false);
        e->returns[e->return_count++] = e->count; word(e, 0);
        return;
    }
#endif
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
#if GBN_RV32_FAST & 8388608
    word(e, offsetof(struct GbnRv32Block, context_tag) << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
    misses[n++] = e->count; word(e, beq(TEMP, CONTEXT, 0) | 1u << 12);
#endif
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
    finish_count(e, 0, false); stack(e, RHS, STACK_TARGET, false);
    e->returns[e->return_count++] = e->count; word(e, 0);
}

#if (GBN_RV32_FAST & 128) && !GBN_RV32_STATS
static void far_jump(struct Emit*, uint32_t target, unsigned through, unsigned link);
static uint32_t exit_stub(struct GbnRv32*, bool ram, bool miss);
#endif
#if GBN_RV32_FAST & 8192
static bool mixer_head_at(const struct Gbn* m, uint32_t pc);
#endif
#if GBN_RV32_FAST & 524288
static bool mixer_fix_at(const struct Gbn* m, uint32_t pc);
#endif
static void tail(struct Emit* e, struct GbnRv32* backend, uint32_t pc, bool fallback) {
	struct GbnRv32Block* b = e->block;
	bool unlinked = fallback;
#if GBN_RV32_FAST & 8192
	/* Every edge into the mixer loop head returns to C (mixer_hle). */
	if (!b->thumb && mixer_head_at(backend->machine, pc)) unlinked = true;
#if GBN_RV32_FAST & 524288
	if (!b->thumb && mixer_fix_at(backend->machine, pc)) unlinked = true;
#endif
#endif
#if GBN_RV32_FAST & 1024
	time_flush(e);
#endif
    /* The entry proof covers the entire contiguous source span. Native
     * stores overlapping it already fall back, so an internal ARM edge can
     * jump directly to the target's exact cap/event guards. Rebase LIMIT to
     * keep initial_cap - LIMIT + source_index equal to retired instructions. */
    uint32_t offset = pc - b->pc;
    if (!unlinked && !b->thumb && offset && !(offset & 3)
            && offset / 4 < e->source_count) {
        unsigned target = offset / 4;
        imm(e, LIMIT, LIMIT, 0, (int) target - (int) b->instructions);
#if GBN_RV32_FAST & 1
        /* Fast mode has no per-instruction checks inside the body: a loop
         * back into it takes the event and needs budget for the rest. */
        unsigned late = e->count; word(e, beq(NOW, ZERO, 0) | 5u << 12);
        imm(e, TEMP, ZERO, 0, (int) b->instructions);
        unsigned short_budget = e->count; word(e, beq(LIMIT, TEMP, 0) | 4u << 12);
        word(e, beq(ZERO, ZERO, 4 * (e->body_offsets[target] - e->count)));
        b->code[late] |= beq(0, 0, 4 * (e->count - late)) & 0xfe000f80u;
        b->code[short_budget] |= beq(0, 0, 4 * (e->count - short_budget)) & 0xfe000f80u;
        finish_count(e, target, false);
        constant(e, RHS, pc);
        e->returns[e->return_count++] = e->count; word(e, 0);
#else
        word(e, beq(ZERO, ZERO, 4 * (e->body_offsets[target] - e->count)));
#endif
        return;
    }
	imm(e, LIMIT, LIMIT, 0, -(int) b->instructions);
	unsigned misses[10], n = 0;
	if (!unlinked && (pc >> 24) == (b->pc >> 24)) {
		misses[n++] = e->count; word(e, beq(LIMIT, ZERO, 0));
		misses[n++] = e->count; word(e, beq(NOW, ZERO, 0) | 5u << 12);
		if (pc == b->pc) {
			/* Stores overlapping this block already leave through the bus.
			 * A self-loop therefore needs neither another key nor byte check. */
			if (mergeable(e)) {
				merge_loop(e, backend);
				misses[n++] = e->count; word(e, beq(LIMIT, ZERO, 0));
				misses[n++] = e->count; word(e, beq(NOW, ZERO, 0) | 5u << 12);
			}
			e->loop_jumps[e->loop_jump_count++] = e->count; word(e, 0);
		} else {
			unsigned slot = block_index(pc);
#if GBN_RV32_FAST & 16
			{
				/* Always two words: a direct link to the target replaces them. */
#if (GBN_RV32_FAST & 128) && !GBN_RV32_STATS
				if (e->link_count < GBN_RV32_INSNS + 2) { e->link_at[e->link_count] = e->count; e->link_pc[e->link_count++] = pc; }
				(void) slot;
				bool ram = backend->machine->code_region < 8;
				far_jump(e, exit_stub(backend, ram, false), TEMP, TEMP);
				for (unsigned i = 0; i < n; ++i)
					b->code[misses[i]] |= beq(0, 0, 4 * (e->count - misses[i])) & 0xfe000f80u;
				far_jump(e, exit_stub(backend, ram, true), TEMP, TEMP);
				word(e, pc);
				return;
#else
				uint32_t a = (uint32_t) (uintptr_t) &backend->lookup[slot], hi = (a + 0x800) >> 12;
				if (e->link_count < GBN_RV32_INSNS + 2) { e->link_at[e->link_count] = e->count; e->link_pc[e->link_count++] = pc; }
				word(e, hi << 12 | LHS << 7 | 0x37);
				word(e, (a - (hi << 12)) << 20 | LHS << 15 | LHS << 7 | 0x13);
#endif
			}
#else
			constant(e, LHS, (uint32_t) (uintptr_t) &backend->lookup[slot]);
#endif
			word(e, LHS << 15 | 2 << 12 | LHS << 7 | 3);
			misses[n++] = e->count; word(e, beq(LHS, ZERO, 0));
			word(e, offsetof(struct GbnRv32Block, chain) << 20 | LHS << 15 | 2 << 12 | RHS << 7 | 3);
			misses[n++] = e->count; word(e, beq(RHS, ZERO, 0));
			word(e, offsetof(struct GbnRv32Block, pc) << 20 | LHS << 15 | 2 << 12 | TEMP << 7 | 3);
			constant(e, WAIT, pc);
			misses[n++] = e->count; word(e, beq(TEMP, WAIT, 0) | 1u << 12);
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
	finish_count(e, 0, fallback);
	constant(e, RHS, pc);
	e->returns[e->return_count++] = e->count; word(e, 0);
}

#if GBN_RV32_FAST & 4
/* Fast mode: one prologue and one epilogue per kind of block (RAM blocks
 * also carry RAM_EPOCH), shared by every block instead of a copy in each.
 * A block enters the prologue with its return address in LHS (free at the C
 * entry) and leaves through the epilogue jumping with TEMP (free there). */
static uint32_t shared_frame_code[2][2][64] __attribute__((aligned(64)));
static bool shared_frame_ready[2][2];
static void prologue_body(struct Emit* e, bool ram) {
	imm(e, SP, SP, 0, -FRAME_BYTES);
	stack(e, NOW, 0, true); stack(e, HOST_S1, 4, true);
	stack(e, PREFETCH, 8, true); stack(e, WAIT, 12, true); stack(e, HOST_S4, 16, true);
	stack(e, NEGATIVE, SAVED_NEGATIVE, true); stack(e, IS_ZERO, SAVED_ZERO, true);
	if (ram) { stack(e, RAM_EPOCH, SAVED_EPOCH, true); imm(e, RAM_EPOCH, TEMP, 0, 0); }
	stack(e, CONTEXT, SAVED_CONTEXT, true); imm(e, CONTEXT, 14, 0, 0);
	stack(e, 23, SAVED_S7, true); stack(e, 24, SAVED_S8, true);
	stack(e, 27, SAVED_S11, true); stack(e, RA, SAVED_RA, true);
	stack(e, LIMIT, INITIAL_CAP, true); stack(e, FLAGS, INITIAL_DEADLINE, true);
	load(e, NOW, offsetof(struct Gbn, now));
	op(e, NOW, NOW, FLAGS, 0, 0x20);
#if GBN_RV32_FAST & 4194304
	if (hub_ready) constant(e, 4, hub_base()); /* tp: the memory leaves */
#endif
	load(e, PREFETCH, offsetof(struct Gbn, prefetched_pc));
	for (unsigned i = 0; i < 15; ++i) if (resident(i)) load(e, guest[i], offsetof(struct Gbn, cpu.r) + 4 * i);
	load(e, FLAGS, offsetof(struct Gbn, cpu.cpsr));
	imm(e, NEGATIVE, FLAGS, 5, 31); imm(e, IS_ZERO, FLAGS, 5, 30);
	imm(e, IS_ZERO, IS_ZERO, 7, 1);
	imm(e, FLAGS, FLAGS, 1, 2); imm(e, FLAGS, FLAGS, 5, 2);
	word(e, LHS << 15 | 0x67);
}
static void epilogue_body(struct Emit* e, bool ram) {
	for (unsigned i = 0; i < 15; ++i) if (resident(i)) store(e, guest[i], offsetof(struct Gbn, cpu.r) + 4 * i);
	packed_flags(e, TEMP); store(e, TEMP, offsetof(struct Gbn, cpu.cpsr));
	stack(e, TEMP, INITIAL_DEADLINE, false);
	op(e, NOW, NOW, TEMP, 0, 0);
	store(e, NOW, offsetof(struct Gbn, now));
	store(e, PREFETCH, offsetof(struct Gbn, prefetched_pc));
	imm(e, MACHINE, LIMIT, 0, 0);
	imm(e, LIMIT, RHS, 0, 0);
	stack(e, NOW, 0, false); stack(e, HOST_S1, 4, false);
	stack(e, PREFETCH, 8, false); stack(e, WAIT, 12, false); stack(e, HOST_S4, 16, false);
	stack(e, NEGATIVE, SAVED_NEGATIVE, false); stack(e, IS_ZERO, SAVED_ZERO, false);
	if (ram) stack(e, RAM_EPOCH, SAVED_EPOCH, false);
	stack(e, CONTEXT, SAVED_CONTEXT, false);
	stack(e, 23, SAVED_S7, false); stack(e, 24, SAVED_S8, false);
	stack(e, 27, SAVED_S11, false); stack(e, RA, SAVED_RA, false);
	imm(e, SP, SP, 0, FRAME_BYTES); word(e, RA << 15 | 0x67);
}
/* kind 0: prologue, 1: epilogue */
static uint32_t shared_frame(unsigned kind, bool ram) {
	if (!shared_frame_ready[kind][ram]) {
		static struct GbnRv32Block routine; /* Emit is ~4 KiB: not on the compile stack */
		static struct Emit r;
		memset(&r, 0, sizeof(r));
		routine.code = shared_frame_code[kind][ram];
		r.block = &routine;
		if (kind) epilogue_body(&r, ram); else prologue_body(&r, ram);
		shared_frame_ready[kind][ram] = true; /* allocate_code's fence.i publishes it */
	}
	return (uint32_t) (uintptr_t) shared_frame_code[kind][ram];
}
/* lui link, hi(target); jalr link_to, lo(target)(link) */
static void far_jump(struct Emit* e, uint32_t target, unsigned through, unsigned link) {
	uint32_t hi = (target + 0x800) >> 12;
	word(e, hi << 12 | through << 7 | 0x37);
	word(e, (target - (hi << 12)) << 20 | through << 15 | link << 7 | 0x67);
}
#endif

#if (GBN_RV32_FAST & 128) && !GBN_RV32_STATS
#if !(GBN_RV32_FAST & 4) || !(GBN_RV32_FAST & 16)
#error "GBN_RV32_FAST bit 128 needs bits 4 and 16"
#endif
/* Fast mode: compact exit edges. After its budget and event checks an exit
 * is a two-word link site, a two-word call of the shared miss routine and
 * the target pc:
 *     L: lui a3, lookup; jalr a3, a3   (a direct link patches both words)
 *     M: lui a3, miss;   jalr a3, a3
 *     P: .word pc
 * The lookup routine reads the pc 8 bytes past its return address (M), the
 * miss routine at its return address (P). Both exist per block kind (RAM
 * blocks restore RAM_EPOCH) and per backend (the lookup table address). */
static uint32_t exit_stub_code[2][48] __attribute__((aligned(64)));
static const struct GbnRv32* exit_stub_backend[2];
static unsigned exit_stub_miss[2];
static uint32_t exit_stub(struct GbnRv32* backend, bool ram, bool miss) {
	if (exit_stub_backend[ram] != backend) {
		static struct GbnRv32Block routine; /* Emit is ~4 KiB: not on the compile stack */
		static struct Emit r;
		memset(&r, 0, sizeof(r));
		routine.code = exit_stub_code[ram];
		r.block = &routine;
		struct Emit* e = &r;
		word(e, 8u << 20 | TEMP << 15 | 2 << 12 | WAIT << 7 | 3); /* pc */
		imm(e, LHS, WAIT, 5, 1); constant(e, RHS, 0x9e3779b1u);
		op(e, LHS, LHS, RHS, 0, 1); imm(e, LHS, LHS, 5, GBN_RV32_HASH_SHIFT);
		imm(e, LHS, LHS, 1, 2);
		constant(e, RHS, (uint32_t) (uintptr_t) backend->lookup);
		op(e, LHS, LHS, RHS, 0, 0);
		word(e, LHS << 15 | 2 << 12 | LHS << 7 | 3);
		unsigned misses[4], n = 0;
		misses[n++] = e->count; word(e, beq(LHS, ZERO, 0));
		word(e, offsetof(struct GbnRv32Block, chain) << 20 | LHS << 15 | 2 << 12 | RHS << 7 | 3);
		misses[n++] = e->count; word(e, beq(RHS, ZERO, 0));
		word(e, offsetof(struct GbnRv32Block, pc) << 20 | LHS << 15 | 2 << 12 | RESULT << 7 | 3);
		misses[n++] = e->count; word(e, beq(RESULT, WAIT, 0) | 1u << 12);
#if GBN_RV32_FAST & 8388608
		word(e, offsetof(struct GbnRv32Block, context_tag) << 20 | LHS << 15 | 2 << 12 | RESULT << 7 | 3);
		misses[n++] = e->count; word(e, beq(RESULT, CONTEXT, 0) | 1u << 12);
#endif
		word(e, RHS << 15 | 0x67); /* tail entry: no call, spill or prologue */
		for (unsigned i = 0; i < n; ++i)
			routine.code[misses[i]] |= beq(0, 0, 4 * (e->count - misses[i])) & 0xfe000f80u;
		imm(e, TEMP, TEMP, 0, 8);
		exit_stub_miss[ram] = e->count;
		/* finish_count(e, 0, false), then the pc, as tail's own miss path */
		word(e, TEMP << 15 | 2 << 12 | RHS << 7 | 3);
		stack(e, TEMP, INITIAL_CAP, false);
		op(e, LIMIT, TEMP, LIMIT, 0, 0x20);
		far_jump(e, shared_frame(1, ram), TEMP, ZERO);
		exit_stub_backend[ram] = backend; /* allocate_code's fence.i publishes it */
	}
	return (uint32_t) (uintptr_t) (exit_stub_code[ram] + (miss ? exit_stub_miss[ram] : 0));
}
#endif

static void unlink_code(struct GbnRv32* backend, struct GbnRv32Block* b) {
	if (!b->arena_linked) return;
	if (b->arena_prev) b->arena_prev->arena_next = b->arena_next;
	else backend->segments[(b->code - backend->code) / GBN_RV32_SEGMENT_WORDS] = b->arena_next;
	if (b->arena_next) b->arena_next->arena_prev = b->arena_prev;
	b->arena_linked = false;
	b->arena_prev = b->arena_next = NULL;
}

#if GBN_RV32_FAST & 16
/* Direct block links. An exit whose target block exists jumps straight to the
 * target's chained entry (which still proves the context) instead of loading
 * the lookup table. Each link is on the source's outgoing list and on either
 * the target's incoming list or the pending bucket of its pc: retiring a
 * target restores its incoming sites to the lookup and makes them pending;
 * retiring a source forgets its links. Patches precede the compile's fence.i. */
struct GbnLink {
	uint32_t* site;
	uint32_t pc, context, restore[2];
	struct GbnLink **head, *next, *prev, *next_out;
};
#define GBN_LINKS 65536u
#define GBN_LINK_BUCKETS 4096u
static struct GbnLink link_pool[GBN_LINKS], *link_free;
static unsigned link_used;
static struct GbnLink *link_in[GBN_RV32_SLOTS], *link_out[GBN_RV32_SLOTS], *link_pending[GBN_LINK_BUCKETS];
static unsigned link_site_count, link_site_at[GBN_RV32_INSNS + 2];
static uint32_t link_site_pc[GBN_RV32_INSNS + 2];
static unsigned link_bucket(uint32_t pc) { return ((pc >> 1) * 0x9e3779b1u) >> 20; }
static void link_insert(struct GbnLink* l, struct GbnLink** head) {
	l->head = head; l->prev = NULL; l->next = *head;
	if (*head) (*head)->prev = l;
	*head = l;
}
static void link_remove(struct GbnLink* l) {
	if (l->prev) l->prev->next = l->next; else *l->head = l->next;
	if (l->next) l->next->prev = l->prev;
}
static void link_patch(struct GbnLink* l, const struct GbnRv32Block* t) {
	uint32_t target = (uint32_t) (uintptr_t) t->chain, hi = (target + 0x800) >> 12;
	l->site[0] = hi << 12 | WAIT << 7 | 0x37;
	l->site[1] = (target - (hi << 12)) << 20 | WAIT << 15 | 0x67;
}
static void link_retire(struct GbnRv32* backend, struct GbnRv32Block* b) {
	unsigned slot = (unsigned) (b - backend->blocks);
	while (link_in[slot]) {
		struct GbnLink* l = link_in[slot];
		link_remove(l);
		l->site[0] = l->restore[0]; l->site[1] = l->restore[1];
		link_insert(l, &link_pending[link_bucket(l->pc)]);
	}
	for (struct GbnLink *l = link_out[slot], *n; l; l = n) {
		n = l->next_out;
		link_remove(l);
		l->next = link_free; link_free = l;
	}
	link_out[slot] = NULL;
}
static struct GbnRv32Block* link_target(struct GbnRv32* backend, uint32_t pc, uint32_t context) {
	unsigned index = block_index(pc);
	struct GbnRv32Block* b = backend->lookup[index];
	if (b && b->valid && b->chain && b->pc == pc && b->context_tag == context) return b;
	b = &backend->blocks[(index & (GBN_RV32_SETS - 1)) * GBN_RV32_WAYS];
	for (unsigned way = 0; way < GBN_RV32_WAYS; ++way)
		if (b[way].valid && b[way].chain && b[way].pc == pc && b[way].context_tag == context) return b + way;
	return NULL;
}
static void link_publish(struct GbnRv32* backend, struct GbnRv32Block* block) {
	unsigned slot = (unsigned) (block - backend->blocks);
	for (unsigned k = 0; k < link_site_count; ++k) {
		struct GbnLink* l = link_free;
		if (l) link_free = l->next;
		else if (link_used < GBN_LINKS) l = &link_pool[link_used++];
		else break;
		l->site = block->code + link_site_at[k]; l->pc = link_site_pc[k]; l->context = block->context_tag;
		l->restore[0] = l->site[0]; l->restore[1] = l->site[1];
		l->next_out = link_out[slot]; link_out[slot] = l;
		struct GbnRv32Block* t = link_target(backend, l->pc, l->context);
		if (t && t != block) { link_patch(l, t); link_insert(l, &link_in[t - backend->blocks]); }
		else link_insert(l, &link_pending[link_bucket(l->pc)]);
	}
	for (struct GbnLink *l = link_pending[link_bucket(block->pc)], *n; l; l = n) {
		n = l->next;
		if (l->pc != block->pc || l->context != block->context_tag) continue;
		link_remove(l); link_patch(l, block); link_insert(l, &link_in[slot]);
	}
}
#endif

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
#if GBN_RV32_FAST & 16
			link_retire(backend, old);
#endif
			old->valid = false; old->chain = NULL; old->arena_linked = false;
			old->arena_prev = old->arena_next = NULL;
			old = next;
		}
		backend->segments[backend->code_segment] = NULL;
	}
    const uint32_t* generated = block->code;
	block->code = backend->code + backend->code_used;
	memcpy(block->code, generated, block->words * sizeof(uint32_t));
	backend->code_used += block->words;
	block->arena_prev = NULL;
	block->arena_next = backend->segments[backend->code_segment];
	if (block->arena_next) block->arena_next->arena_prev = block;
	backend->segments[backend->code_segment] = block;
	block->arena_linked = true;
#if GBN_RV32_FAST & 16
	block->chain = block->code + chain;
	link_publish(backend, block);
	__asm__ volatile("fence.i" ::: "memory");
#else
	__asm__ volatile("fence.i" ::: "memory");
	block->chain = block->code + chain;
#endif
}

/* Tags are assigned only after comparing the complete immutable key.
 * A hash collision evicts the intern-table entry, never aliases two keys:
 * a repeated evicted key gets a new tag (a conservative chain miss).
 * Before tag wrap, retire every descriptor so an old immediate cannot match. */
static uint32_t context_tag(struct GbnRv32* backend, uint32_t generation, uint32_t mask, uint32_t key) {
	uint32_t hash = generation * 0x9e3779b1u ^ mask * 0x85ebca6bu ^ key * 0xc2b2ae35u;
	struct GbnRv32Key* entry = &backend->keys[(hash ^ (hash >> 16)) & (GBN_RV32_KEYS - 1)];
	if (entry->tag && entry->generation == generation && entry->mask == mask && entry->timing == key)
		return entry->tag;
	if (backend->next_context_tag == UINT32_MAX) {
		for (unsigned i = 0; i < GBN_RV32_SLOTS; ++i) {
#if GBN_RV32_FAST & 16
			link_retire(backend, &backend->blocks[i]);
#endif
			backend->blocks[i].valid = false; backend->blocks[i].chain = NULL;
		}
		memset(backend->keys, 0, sizeof(backend->keys));
		backend->next_context_tag = 0;
	}
	*entry = (struct GbnRv32Key) {generation, mask, key, ++backend->next_context_tag};
	return entry->tag;
}

struct ArmEdge { unsigned at, count; uint32_t pc, target; bool link; };

/* One finalizer serves exact size probes and the executable block. */
static unsigned finish_body(struct GbnRv32* backend, struct GbnRv32Block* block,
        struct Emit* e, unsigned prologue_words, struct ArmEdge* edges,
        unsigned edge_count, unsigned* entry) {
    const struct Gbn* m = backend->machine;
    unsigned width = block->thumb ? 2 : 4;
#if GBN_RV32_FAST & 16
    e->link_count = 0;
#endif
	e->source_count = block->instructions;
	block->loop = block->branch && block->target == block->pc;
	if (e->dynamic_pc) {
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 1024)
		e->extra_time = 2 + m->code_wait + m->code_nonseq; /* added by indirect_tail's time_flush */
#else
		imm(e, NOW, NOW, 0, 2 + m->code_wait + m->code_nonseq);
#endif
		constant(e, PREFETCH, 0);
        indirect_tail(e, backend);
	} else if (block->branch) {
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 128)
		if (e->branch_direct) {
			unsigned untaken = e->count;
			if (!e->branch_always) word(e, beq(e->branch_reg, ZERO, 0) | (e->branch_clear ? 1u : 0u) << 12);
#if GBN_RV32_FAST & 1024
			e->extra_time = 2 + m->code_wait + m->code_nonseq; /* added by tail's time_flush */
#else
			imm(e, NOW, NOW, 0, 2 + m->code_wait + m->code_nonseq);
#endif
			constant(e, PREFETCH, 0);
			tail(e, backend, block->target, false);
			if (e->branch_always) goto tails_done; /* no fall-through edge */
			block->code[untaken] |= beq(0, 0, 4 * (e->count - untaken)) & 0xfe000f80u;
		} else
#endif
		{
		unsigned untaken = e->count; word(e, 0);
		imm(e, LIMIT, LIMIT, 1, 1); imm(e, LIMIT, LIMIT, 5, 1);
		imm(e, NOW, NOW, 0, 2 + (block->thumb ? m->code_wait + m->code_nonseq : m->code_word_wait + m->code_word_nonseq));
		constant(e, PREFETCH, 0);
		tail(e, backend, block->target, false);
		block->code[untaken] = beq(LIMIT, ZERO, 4 * (e->count - untaken)) | 5u << 12;
		}
	}
	if (!e->dynamic_pc) tail(e, backend, block->pc + width * block->instructions, block->fallback);
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 128)
tails_done:;
#endif

    unsigned total_instructions = block->instructions;
    for (unsigned i = 0; i < edge_count; ++i) {
        struct ArmEdge* edge = &edges[i];
        block->code[edge->at] = beq(TEMP, ZERO, 4 * (e->count - edge->at)) | 1u << 12;
        block->instructions = edge->count;
        if (edge->link) { constant(e, RESULT, edge->pc + 4); put(e, 14); }
        imm(e, NOW, NOW, 0, 2 + m->code_word_wait + m->code_word_nonseq);
        constant(e, PREFETCH, 0);
        tail(e, backend, edge->target, false);
    }
    block->instructions = total_instructions;

	unsigned loop_entry = prologue_words;
	if (mergeable(e)) {
		loop_entry = e->count;
		for (unsigned i = 0; i < 8; ++i) if (e->written & (1u << i)) stack(e, guest[i], 20 + 4 * i, true);
		packed_flags(e, RHS); stack(e, RHS, SNAPSHOT_FLAGS, true);
		stack(e, PREFETCH, SNAPSHOT_PREFETCH, true); stack(e, NOW, SNAPSHOT_NOW, true);
		word(e, beq(ZERO, ZERO, 4 * (prologue_words - e->count)));
	}
	for (unsigned i = 0; i < e->loop_jump_count; ++i)
        block->code[e->loop_jumps[i]] = beq(ZERO, ZERO, 4 * (loop_entry - e->loop_jumps[i]));
	unsigned chain = loop_entry;
#if GBN_RV32_FAST & 256
	if (block->instructions && m->code_region < 8) {
#else
	if (block->instructions) {
#endif
		chain = e->count;
		if (m->code_region < 8) {
			constant(e, LHS, (uint32_t) (uintptr_t) &block->checked_epoch);
			word(e, LHS << 15 | 2 << 12 | TEMP << 7 | 3);
			word(e, beq(TEMP, RAM_EPOCH, 4 * (loop_entry - e->count)));
		}
		/* The live frame's tag proves the complete generation/mask/timing
		 * tuple-> A RAM epoch hit additionally proves unchanged code bytes. */
		constant(e, TEMP, block->context_tag);
		stop(e, TEMP, CONTEXT, 1, false); e->stops[e->stop_count - 1].instruction = 0;
		if (m->code_region < 8) {
			const uint8_t* bytes = m->code + (block->pc & m->code_mask);
			constant(e, LHS, (uint32_t) (uintptr_t) bytes);
			for (unsigned off = 0, size = (block->instructions > block->proof_instructions ? block->instructions : block->proof_instructions) * width; off < size;) {
				uintptr_t address = (uintptr_t) bytes + off;
				unsigned size_left = size - off;
				unsigned chunk = !(address & 3) && size_left >= 4 ? 4 : !(address & 1) && size_left >= 2 ? 2 : 1;
				unsigned fn = chunk == 4 ? 2 : chunk == 2 ? 5 : 4;
				word(e, off << 20 | LHS << 15 | fn << 12 | RHS << 7 | 3);
				uint32_t value = 0;
				for (unsigned i = 0; i < chunk; ++i) value |= (uint32_t) bytes[off + i] << (8 * i);
				constant(e, TEMP, value);
				stop(e, RHS, TEMP, 1, false); e->stops[e->stop_count - 1].instruction = 0;
				off += chunk;
			}
			constant(e, LHS, (uint32_t) (uintptr_t) &block->checked_epoch);
			word(e, RAM_EPOCH << 20 | LHS << 15 | 2 << 12 | 0x23);
		}
		word(e, beq(ZERO, ZERO, 4 * (loop_entry - e->count)));
	}
	unsigned epilogue = e->count;
#if GBN_RV32_FAST & 4
	far_jump(e, shared_frame(1, m->code_region < 8), TEMP, ZERO);
	if (0)
#endif
	for (unsigned i = 0; i < 15; ++i) if (resident(i)) store(e, guest[i], offsetof(struct Gbn, cpu.r) + 4 * i);
#if !(GBN_RV32_FAST & 4)
	packed_flags(e, TEMP); store(e, TEMP, offsetof(struct Gbn, cpu.cpsr));
	stack(e, TEMP, INITIAL_DEADLINE, false);
	op(e, NOW, NOW, TEMP, 0, 0);
	store(e, NOW, offsetof(struct Gbn, now));
	store(e, PREFETCH, offsetof(struct Gbn, prefetched_pc));
	imm(e, MACHINE, LIMIT, 0, 0);
	imm(e, LIMIT, RHS, 0, 0); /* uint64 return: a0=count/reason, a1=next guest PC */
	stack(e, NOW, 0, false); stack(e, HOST_S1, 4, false);
	stack(e, PREFETCH, 8, false); stack(e, WAIT, 12, false); stack(e, HOST_S4, 16, false);
	stack(e, NEGATIVE, SAVED_NEGATIVE, false); stack(e, IS_ZERO, SAVED_ZERO, false);
	if (m->code_region < 8) stack(e, RAM_EPOCH, SAVED_EPOCH, false);
	stack(e, CONTEXT, SAVED_CONTEXT, false);
    stack(e, 23, SAVED_S7, false); stack(e, 24, SAVED_S8, false);
    stack(e, 27, SAVED_S11, false); stack(e, RA, SAVED_RA, false);
	imm(e, SP, SP, 0, FRAME_BYTES); word(e, RA << 15 | 0x67);
#endif
#if GBN_RV32_FAST & 512
	{
		unsigned base = e->count;
		memcpy(block->code + base, e->cold, e->cold_count * sizeof(uint32_t));
		e->count += e->cold_count;
		for (unsigned i = 0; i < e->cold_in_count; ++i) {
			unsigned at = e->cold_in[i][0];
			block->code[at] |= beq(0, 0, 4 * (base + e->cold_in[i][1] - at)) & 0xfe000f80u;
		}
		for (unsigned i = 0; i < e->cold_out_count; ++i) {
			unsigned at = base + e->cold_out[i][0];
			block->code[at] = beq(ZERO, ZERO, 4 * (e->cold_out[i][1] - at));
		}
		for (unsigned i = 0; i < e->stop_count; ++i)
			if (e->stops[i].at & COLD_AT) e->stops[i].at = base + (e->stops[i].at & ~COLD_AT);
		for (unsigned i = 0; i < e->return_count; ++i)
			if (e->returns[i] & COLD_AT) e->returns[i] = base + (e->returns[i] & ~COLD_AT);
	}
#endif
	for (unsigned i = 0; i < e->return_count; ++i)
		block->code[e->returns[i]] = beq(ZERO, ZERO, 4 * (epilogue - e->returns[i]));
	unsigned stubs[GBN_RV32_INSNS][2];
	memset(stubs, 0, sizeof(stubs));
	for (unsigned i = 0; i < e->stop_count; ++i) {
		unsigned index = e->stops[i].instruction, slow = e->stops[i].fallback;
		if (!stubs[index][slow]) {
			stubs[index][slow] = e->count;
#if GBN_RV32_FAST & 1024
			if (e->prefix[index]) imm(e, NOW, NOW, 0, (int) e->prefix[index]);
#endif
			finish_count(e, index, slow);
			constant(e, RHS, block->pc + width * index);
			word(e, beq(ZERO, ZERO, 4 * (epilogue - e->count)));
		}
		unsigned at = e->stops[i].at;
		block->code[at] |= beq(0, 0, 4 * (stubs[index][slow] - at)) & 0xfe000f80u;
	}
	block->words = (uint16_t) e->count;
    *entry = loop_entry;
    return chain;
}

/* Probes use isolated output: finalization patches guard/edge offsets in
 * the prefix. No executable code or live descriptor is published here. */
static bool body_fits(struct GbnRv32* backend, const struct Emit* body,
        unsigned prologue_words, struct ArmEdge* edges, unsigned edge_count) {
    uint32_t code[4096];
    struct GbnRv32Block block = *body->block;
    struct Emit probe = *body;
    block.code = code; probe.block = &block;
    memcpy(code, body->block->code, body->count * sizeof(*code));
    unsigned entry;
    finish_body(backend, &block, &probe, prologue_words, edges, edge_count, &entry);
    return block.words <= GBN_RV32_WORDS;
}

static bool arm_pure(uint32_t code) {
    if (code >> 28 != 14) return false;
    if ((code & 0x0fc000f0) == 0x00000090) {
        return (code >> 16 & 15) != 15 && (!(code & 0x200000) || (code >> 12 & 15) != 15);
    }
    if ((code & 0x0c000000) || (code >> 12 & 15) == 15) return false;
    if (!(code & 0x2000000) && (code & 16)) return false;
    unsigned kind = code >> 21 & 15;
    return !(kind >= 8 && kind <= 11 && !(code & 0x100000));
}

#include "mixer-native-emitter.h"
#include "mixer-packet-emitter.h"
#if GBN_RV32_FAST & 8192
#include "mixer-hle.h"
#endif

/* A short fast body has no observable instruction boundary inside it. The
 * original body remains native and exact when cap/time proof fails. All
 * candidates are lowered into temporary storage before committing, so the
 * arena/branch bounds include both bodies and every existing exit stub. */
static unsigned arm_bulk(struct Emit* e, const struct Gbn* m, unsigned first,
        unsigned ram_guard_words, unsigned edge_count) {
    if (e->block->thumb) return 0;
    uint32_t ops[8], words[GBN_RV32_WORDS];
    unsigned n = 0, size = m->code_region >= 8 ? m->rom_size :
        m->code_region == 2 ? GBN_EWRAM_SIZE : GBN_IWRAM_SIZE;
    uint32_t pc = e->block->pc + 4 * first;
    while (n < 8 && first + n < GBN_RV32_INSNS) {
        uint32_t address = pc + 4 * n, offset = address & m->code_mask;
        if (address >> 24 != m->code_region || offset >= size || size - offset < 4 ||
                (m->code_region < 8 && offset != (e->block->pc & m->code_mask) + 4 * (first + n))) break;
        ops[n] = fetch(m, address, 4);
        if (!arm_pure(ops[n])) break;
        ++n;
    }
    while (n >= 3) {
        struct GbnRv32Block temporary = *e->block;
        temporary.code = words;
        struct Emit probe = *e;
        probe.block = &temporary; probe.count = probe.stop_count = 0;
        probe.skip_shifter = false;
        unsigned j;
        for (j = 0; j < n; ++j) {
            if (!arm_instruction(&probe, m, ops[j], pc + 4 * j)) break;
            imm(&probe, NOW, NOW, 0, 1 + m->code_word_wait);
            ++temporary.instructions;
        }
        if (j != n) { n = j; continue; }
        unsigned full_words = probe.count;
        bool carry_live[8], need = true;
        unsigned worst = n * (1 + m->code_word_wait);
        for (j = n; j--;) {
            bool multiply = (ops[j] & 0x0fc000f0) == 0x00000090;
            carry_live[j] = false;
            if (multiply) {
                if (ops[j] & 0x100000) need = true; /* MULS reads the preceding carry. */
                worst += (ops[j] & 0x200000) ? 5 : 4;
                if (m->code_nonseq > m->code_wait) worst += m->code_nonseq - m->code_wait;
            } else { carry_live[j] = need; need = false; }
        }
        probe.count = probe.stop_count = 0;
        temporary.instructions = e->block->instructions;
        for (j = 0; j < n; ++j) {
            probe.skip_shifter = !carry_live[j];
            (void) arm_instruction(&probe, m, ops[j], pc + 4 * j);
            imm(&probe, NOW, NOW, 0, 1 + m->code_word_wait);
            ++temporary.instructions;
        }
        unsigned fast_words = probe.count;
        unsigned reserve = 340 + 64 * edge_count + (13 + ram_guard_words) * (first + n);
        if (e->count + 9 + fast_words + full_words + 3 * (n - 1) + reserve >= GBN_RV32_WORDS) {
            --n; continue;
        }
        constant(e, TEMP, first + n);
        unsigned cap = e->count; word(e, 0);
        if (worst < 2048) imm(e, TEMP, NOW, 0, (int) worst);
        else { constant(e, TEMP, worst); op(e, TEMP, NOW, TEMP, 0, 0); }
        unsigned time = e->count; word(e, 0);
        memcpy(e->block->code + e->count, words, fast_words * sizeof(*words));
        e->count += fast_words;
        unsigned done = e->count; word(e, 0);
        unsigned slow = e->count;
        e->block->code[cap] = beq(LIMIT, TEMP, 4 * (slow - cap)) | 6u << 12;
        e->block->code[time] = beq(TEMP, ZERO, 4 * (slow - time)) | 5u << 12;
        for (j = 0; j < n; ++j) {
            if (j) e->body_offsets[first + j] = e->count;
            if (j) {
                constant(e, TEMP, first + j); stop(e, LIMIT, TEMP, 0, false);
                stop(e, NOW, ZERO, 5, false);
            }
            (void) arm_instruction(e, m, ops[j], pc + 4 * j);
            imm(e, NOW, NOW, 0, 1 + m->code_word_wait);
            ++e->block->instructions;
        }
        e->block->code[done] = beq(ZERO, ZERO, 4 * (e->count - done));
        return n;
    }
    return 0;
}

#if GBN_RV32_FAST & 8
/* What a Thumb instruction does to C and V, for dead-flag elimination:
 * bit 0 writes C, bit 1 writes V, bit 2 reads C or V (or might), bit 3 ends
 * the block or leaves the straight line (flags then count as live). */
enum { CV_WC = 1, CV_WV = 2, CV_READ = 4, CV_END = 8 };
static unsigned thumb_cv(unsigned op) {
	if (op >> 11 == 0x03) return CV_WC | CV_WV;                    /* add/sub reg/imm3 */
	if (op >> 13 == 0) return ((op >> 6) & 31) || (op >> 11) ? CV_WC : 0; /* LSL #0 keeps C */
	if (op >> 11 == 0x04) return 0;                                /* mov imm8: N, Z */
	if (op >> 13 == 1) return CV_WC | CV_WV;                       /* cmp/add/sub imm8 */
	if (op >> 10 == 0x10) {
		switch ((op >> 6) & 15) {
		case 0: case 1: case 8: case 12: case 14: case 15: return 0;      /* and eor tst orr bic mvn */
		case 9: case 10: case 11: return CV_WC | CV_WV;                   /* neg cmp cmn */
		default: return CV_READ;                                          /* shifts by reg, adc, sbc, mul */
		}
	}
	if (op >> 10 == 0x11) {
		unsigned kind = op >> 8 & 3, rd = (op & 7) | ((op >> 4) & 8);
		if (kind == 1) return CV_WC | CV_WV;                           /* hi-reg cmp */
		if (kind == 3 || rd == 15) return CV_END;                       /* bx, or pc written */
		return 0;
	}
	if (op >> 11 == 0x09 || (op >> 12 >= 5 && op >> 12 <= 10) || op >> 12 == 12) return 0; /* loads/stores, add pc/sp, ldm/stm */
	if (op >> 8 == 0xb0) return 0;                                 /* sp adjust */
	if ((op & 0xf600) == 0xb400) return (op & 0x0800) && (op & 0x100) ? CV_END : 0; /* push, pop (pc ends) */
	return CV_END;                                                 /* branches, swi, the rest */
}
#endif

#if GBN_RV32_FAST & 16384
/* Flag liveness for Thumb code: N 8, Z 4, C 2, V 1. A flag a later
 * instruction overwrites before any read is not computed. Readers are the
 * guest's own: conditions, ADC/SBC, shifts that may keep C. Exits to C need
 * no more: an event or interrupt there resumes the same instruction stream,
 * whose dead flags stay unread (SPSR saves and restores them unchanged). */
enum { FLAGS_ALL = 15, FLOW_NEXT = 0, FLOW_BRANCH, FLOW_JUMP, FLOW_CALL, FLOW_STOP };
static unsigned thumb_flow(unsigned op, unsigned next, uint32_t pc, unsigned* reads, unsigned* writes, uint32_t* target) {
	static const uint8_t condition_reads[14] = {4, 4, 2, 2, 8, 8, 1, 1, 6, 6, 9, 9, 13, 13};
	*reads = *writes = 0;
	if (op >> 11 == 0x03) { *writes = 15; return FLOW_NEXT; }                 /* add/sub reg/imm3 */
	if (op >> 13 == 0) { *writes = op >> 6 & 31 || op >> 11 ? 14 : 12; return FLOW_NEXT; } /* LSL #0 keeps C */
	if (op >> 11 == 0x04) { *writes = 12; return FLOW_NEXT; }                 /* mov imm8 */
	if (op >> 13 == 1) { *writes = 15; return FLOW_NEXT; }                    /* cmp/add/sub imm8 */
	if (op >> 10 == 0x10) {
		switch (op >> 6 & 15) {
		case 5: case 6: *reads = 2; *writes = 15; break;                       /* adc sbc */
		case 9: case 10: case 11: *writes = 15; break;                         /* neg cmp cmn */
		case 2: case 3: case 4: case 7: *reads = 2; *writes = 12; break;      /* shifts by register may keep C */
		default: *writes = 12; break;                                          /* logical, tst, mul */
		}
		return FLOW_NEXT;
	}
	if (op >> 10 == 0x11) {
		unsigned kind = op >> 8 & 3, rd = (op & 7) | (op >> 4 & 8);
		if (kind == 1) { *writes = 15; return FLOW_NEXT; }                    /* hi-reg cmp */
		return kind == 3 || rd == 15 ? FLOW_STOP : FLOW_NEXT;                  /* bx, pc written */
	}
	if (op >> 11 == 0x09 || (op >> 12 >= 5 && op >> 12 <= 10) || op >> 12 == 12 || op >> 8 == 0xb0) return FLOW_NEXT;
	if ((op & 0xf600) == 0xb400) return (op & 0x0800) && (op & 0x100) ? FLOW_STOP : FLOW_NEXT; /* pop pc */
	if (op >> 12 == 13 && (op >> 8 & 15) < 14) {
		*reads = condition_reads[op >> 8 & 15];
		*target = pc + 4 + (uint32_t) ((int32_t) (op << 24) >> 23);
		return FLOW_BRANCH;
	}
	if (op >> 11 == 0x1c) { *target = pc + 4 + (uint32_t) ((int32_t) (op << 21) >> 20); return FLOW_JUMP; }
	if (op >> 11 == 0x1e && next >> 11 == 0x1f) {                            /* BL pair */
		*target = pc + 4 + (uint32_t) ((int32_t) (op << 21) >> 9) + 2 * (next & 2047);
		return FLOW_CALL;
	}
	return FLOW_STOP;
}
#if GBN_RV32_FAST & 16777216
/* Fast mode: BX, POP {PC} and MOV PC: returns and calls through registers.
 * Compiled code (AAPCS) reads no flags across them; SWI returns restore
 * them and stay excluded. */
static bool thumb_indirect(unsigned op) {
	return (op & 0xff80) == 0x4700 || (op & 0xff00) == 0xbd00 || (op & 0xff87) == 0x4687;
}
#define INDIRECT_LIVE(op) (thumb_indirect(op) ? 0u : FLAGS_ALL)
#else
#define INDIRECT_LIVE(op) FLAGS_ALL
#endif
static bool thumb_rom_code(const struct Gbn* m, uint32_t pc) {
	return pc >> 24 >= 8 && pc >> 24 <= 13 && (pc & 0x01ffffffu) + 4 <= m->rom_size;
}
static unsigned thumb_rom_half(const struct Gbn* m, uint32_t pc) {
	uint32_t offset = pc & 0x01ffffffu;
	return m->rom[offset] | (unsigned) m->rom[offset + 1] << 8;
}
/* Flags that the ROM code at pc may read before writing them. */
static unsigned thumb_needed(const struct Gbn* m, uint32_t pc, unsigned depth) {
	unsigned needed = 0, killed = 0;
	for (unsigned i = 0; i < 24; ++i, pc += 2) {
		if (!thumb_rom_code(m, pc)) break;
		unsigned op = thumb_rom_half(m, pc), reads, writes;
		uint32_t target = 0;
		unsigned flow = thumb_flow(op, thumb_rom_half(m, pc + 2), pc, &reads, &writes, &target);
		needed |= reads & ~killed;
		killed |= writes;
		if (killed == FLAGS_ALL) return needed;
		if (flow == FLOW_NEXT) continue;
		unsigned after = flow == FLOW_STOP ? INDIRECT_LIVE(op) : FLAGS_ALL;
		if (depth && flow == FLOW_BRANCH)
			after = thumb_needed(m, target, depth - 1) | thumb_needed(m, pc + 2, depth - 1);
		else if (depth && (flow == FLOW_JUMP || flow == FLOW_CALL))
			after = thumb_needed(m, target, depth - 1);
		return needed | (after & ~killed);
	}
	return needed | (FLAGS_ALL & ~killed);
}
#endif

#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 4194304) && (GBN_RV32_FAST & 1024) && (GBN_RV32_FAST & 16384)
/* Fast mode, ROM code: POP followed by POP (the return idiom POP {r4-r7};
 * POP {r0}; BX r0) as one span check and SP update; the same loads, the
 * same time. Either guard failing leaves before the first POP. */
static unsigned thumb_pop_pair(struct Emit* e, const struct Gbn* m, unsigned first, unsigned code) {
	if (!hub_ready || m->code_region < 8 || (code & 0xff00) != 0xbc00 || !(code & 255) ||
	    first + 1 >= GBN_RV32_INSNS) return 0;
	uint32_t pc = e->block->pc + 2 * first;
	if (!thumb_rom_code(m, pc + 2)) return 0;
	unsigned next = thumb_rom_half(m, pc + 2);
	if ((next & 0xfe00) != 0xbc00 || !(next & 0x1ff)) return 0;
	bool extra = (next & 0x100) != 0;
	unsigned bytes = 4 * ((unsigned) __builtin_popcount(code & 255) + (unsigned) __builtin_popcount(next & 255) + extra);
	e->read_seen = true;
	get(e, TEMP, 13, pc);
	constant(e, RESULT, bytes | 1);
	hub_call(e, stack_span);
	guard(e, RHS, true);
	if (extra) {
		word(e, (bytes - 4) << 20 | RHS << 15 | 2 << 12 | RESULT << 7 | 3);
		indirect_target(e, m);
	}
	imm(e, TEMP, TEMP, 0, (int) bytes); put_value(e, 13, TEMP);
	unsigned offset = 0;
	for (unsigned pass = 0; pass < 2; ++pass) {
		unsigned list = (pass ? next : code) & 255;
		for (unsigned r = 0; r < 8; ++r) if (list & (1u << r)) {
			word(e, offset << 20 | RHS << 15 | 2 << 12 | guest[r] << 7 | 3);
			e->written |= 1u << r; e->thumb_literal_mask &= ~(1u << r);
			offset += 4;
		}
	}
	/* The leaf adds one POP's internal cycle; the second one and both
	 * fetches go with the block's deferred time. */
	e->deferred += 1 + 2 * (1 + m->code_wait);
	e->block->instructions += 2;
	return 2;
}
#endif

static void compile(struct GbnRv32* backend, struct GbnRv32Block* block) {
	const struct Gbn* m = backend->machine;
#if GBN_RV32_FAST & 8
	bool allow_cv = !(GBN_RV32_FAST & 16384);
restart:;
#endif
	unlink_code(backend, block);
#if GBN_RV32_FAST & 16
	link_retire(backend, block);
#endif
	/* Oversized private workspace permits a rejected next instruction to be
     * lowered safely; only blocks fitting the unchanged 1024-word cap publish. */
    uint32_t work[4096];
    block->code = work; block->chain = NULL;
	block->pc = m->cpu.pc; block->mask = m->code_mask;
	block->generation = m->rom_generation;
	block->timing = timing(m); block->checked_epoch = 0;
	block->context_tag = context_tag(backend, block->generation, block->mask, block->timing);
	block->valid = true;
	block->thumb = (m->cpu.cpsr & 0x20) != 0;
	unsigned width = block->thumb ? 2 : 4;
	block->branch = false; block->fallback = false; block->loop = false; block->instructions = 0; block->proof_instructions = 0;
	block->pipe[0] = fetch(m, block->pc, width); block->pipe[1] = fetch(m, block->pc + width, width);
#if GBN_RV32_FAST & 4
	unsigned prologue_words = 3; /* far call to the shared prologue, then to the body */
#else
	unsigned prologue_words = PROLOGUE_WORDS + 2 + (m->code_region < 8 ? 2 : 0);
#endif
	/* Entries in stops/returns/loop_jumps/body_offsets are written before
	 * use, and their counters delimit the live prefix. Do not clear the
	 * entire 4 KiB scratch table on every block compilation. */
	struct Emit e;
	e.block = block; e.count = prologue_words;
	e.stop_count = e.return_count = e.loop_jump_count = e.source_count = 0;
#if GBN_RV32_FAST & 1024
	e.deferred = e.prefix_filled = 0;
#define TIME_FILL while (e.prefix_filled <= block->instructions) e.prefix[e.prefix_filled++] = (uint16_t) e.deferred;
#define TIME_SAVE unsigned saved_deferred = e.deferred;
#define TIME_RESTORE e.deferred = saved_deferred;
#else
#define TIME_SAVE
#define TIME_RESTORE
#endif
#if GBN_RV32_FAST & 512
	e.cold_count = e.cold_in_count = e.cold_out_count = 0;
#define COLD_WORDS e.cold_count
#define COLD_SAVE unsigned saved_cold[3] = {e.cold_count, e.cold_in_count, e.cold_out_count};
#define COLD_RESTORE e.cold_count = saved_cold[0]; e.cold_in_count = saved_cold[1]; e.cold_out_count = saved_cold[2];
#else
#define COLD_WORDS 0
#define COLD_SAVE
#define COLD_RESTORE
#endif
	e.written = e.thumb_literal_mask = 0;
#if GBN_RV32_FAST & 8388608
	e.insn_start = ~0u;
	e.branch_direct = e.branch_always = e.branch_clear = false;
	e.extra_time = 0;
#endif
	e.read_only = true;
	e.read_seen = e.dynamic_pc = e.skip_shifter = e.io_guarded = false;
	memset(e.thumb_literal, 0, sizeof(e.thumb_literal));
#if GBN_RV32_FAST & 256
	/* Fast mode: a ROM block's chained entry proves its context right before
	 * the body instead of in its tail; C entries and loops start after it. */
	unsigned chain_at = 0;
	if (m->code_region >= 8) {
		chain_at = e.count;
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 128) && !GBN_RV32_STATS
		/* Links join equal contexts; the shared lookups compare it. */
#else
		constant(&e, TEMP, block->context_tag);
		stop(&e, TEMP, CONTEXT, 1, false); e.stops[e.stop_count - 1].instruction = 0;
#endif
		prologue_words = e.count;
	}
#endif
	unsigned ram_guard_words = 0;
	if (m->code_region < 8) {
		uintptr_t address = (uintptr_t) m->code + (block->pc & m->code_mask);
		ram_guard_words = (address & 1) ? 3 * width : (address & 3) ? 2 * width : 4;
	}
    struct ArmEdge edges[GBN_RV32_INSNS];
    unsigned edge_count = 0;
#if GBN_RV32_FAST & 1
    unsigned fast_budget_at = ~0u;
#endif
#if GBN_RV32_FAST & 8
    /* C/V of instruction i are dead when a later instruction of the straight
     * line writes both before anything reads them; cv_killer[i] is that one. */
    uint32_t cv_dead = 0; uint8_t cv_killer[GBN_RV32_INSNS];
    e.cv_dead = false;
    if (allow_cv && block->thumb) for (unsigned i = 0; i < GBN_RV32_INSNS; ++i) {
        unsigned wrote = 0;
        for (unsigned j = i + 1; j < GBN_RV32_INSNS; ++j) {
            unsigned f = thumb_cv(fetch(m, block->pc + 2 * j, 2));
            if (f & (CV_READ | CV_END)) break;
            wrote |= f & (CV_WC | CV_WV);
            if (wrote == (CV_WC | CV_WV)) { cv_dead |= 1u << i; cv_killer[i] = (uint8_t) j; break; }
        }
    }
#endif
#if GBN_RV32_FAST & 16384
    /* ROM Thumb only: the straight line from the block start to its first
     * control transfer, then up to two levels of successors. A block cut
     * short continues into the same straight line, so its exit needs exactly
     * the live-in of the next instruction. */
    uint8_t live_after[GBN_RV32_INSNS]; unsigned live_count = 0;
    if (block->thumb && m->code_region >= 8) {
        unsigned reads[GBN_RV32_INSNS], writes[GBN_RV32_INSNS], end_live = FLAGS_ALL;
        for (unsigned i = 0; i < GBN_RV32_INSNS; ++i) {
            uint32_t at = block->pc + 2 * i, target = 0;
            if (!thumb_rom_code(m, at)) break;
            unsigned flow = thumb_flow(thumb_rom_half(m, at), thumb_rom_half(m, at + 2), at, &reads[i], &writes[i], &target);
            live_count = i + 1;
            if (flow == FLOW_NEXT) {
                if (i + 1 == GBN_RV32_INSNS) end_live = thumb_needed(m, at + 2, 2);
                continue;
            }
            end_live = flow == FLOW_BRANCH ? thumb_needed(m, target, 2) | thumb_needed(m, at + 2, 2)
                : flow == FLOW_JUMP || flow == FLOW_CALL ? thumb_needed(m, target, 2)
                : flow == FLOW_STOP ? INDIRECT_LIVE(thumb_rom_half(m, at)) : FLAGS_ALL;
            break;
        }
        unsigned live = end_live;
        for (unsigned i = live_count; i-- > 0;) { live_after[i] = (uint8_t) live; live = (live & ~writes[i]) | reads[i]; }
    }
#endif
	for (unsigned i = 0; i < GBN_RV32_INSNS; ++i) {
		uint32_t pc = block->pc + width * i, offset = pc & m->code_mask;
		unsigned size = m->code_region >= 8 ? m->rom_size : m->code_region == 2 ? GBN_EWRAM_SIZE : GBN_IWRAM_SIZE;
		if (pc >> 24 != m->code_region || offset >= size || size - offset < width) break;
		if (m->code_region < 8 && offset != (block->pc & m->code_mask) + width * i) break;
#if GBN_RV32_FAST & 8192
		if (i && !block->thumb && mixer_head_at(m, pc)) break;
#if GBN_RV32_FAST & 524288
		if (i && !block->thumb && mixer_fix_at(m, pc)) break;
#endif
#endif
		/* Reserve one lowering, two guarded successor edges, the common
		 * return, both stop reasons at each instruction and RAM validation. */
        bool exact = e.count + COLD_WORDS + 340 + 64 * edge_count
            + (13 + ram_guard_words) * (i + 1) >= GBN_RV32_WORDS;
        if (block->thumb && exact) break;
        /* Lowering appends stop/body arrays. Only these scalar fields and
         * the descriptor need restoring if the exact final size rejects it. */
		uint32_t saved_written = e.written;
		uint32_t saved_thumb_literal[8]; memcpy(saved_thumb_literal, e.thumb_literal, sizeof(saved_thumb_literal));
		unsigned saved_thumb_literal_mask = e.thumb_literal_mask;
		bool saved_io_guarded=e.io_guarded;
        bool saved_read_only = e.read_only, saved_read_seen = e.read_seen;
        bool saved_dynamic_pc = e.dynamic_pc;
        struct GbnRv32Block saved_block = *block;
        unsigned saved_edges = edge_count;
        unsigned saved_returns=e.return_count,saved_loops=e.loop_jump_count;
		unsigned start = e.count, stops = e.stop_count;
		COLD_SAVE TIME_SAVE
		e.body_offsets[i] = start;
#if GBN_RV32_FAST & 4194304
		e.access_start = ~0u;
#endif
#if GBN_RV32_FAST & 8388608
		e.insn_start = start;
#endif
#if GBN_RV32_FAST & 1024
		TIME_FILL /* also the indices inside a preceding instruction group */
#endif
#if GBN_RV32_FAST & 1
		/* Fast mode: events are taken at block exits (up to one block late).
		 * The block runs whole only with budget for all of it, so LIMIT,
		 * whose sign carries a branch condition, never goes negative. */
		if (!i) {
			fast_budget_at = e.count;
			imm(&e, TEMP, ZERO, 0, 0); /* patched with the block's length */
			stop(&e, LIMIT, TEMP, 4, false);
		}
#else
		if (i) {
			constant(&e, TEMP, i); stop(&e, LIMIT, TEMP, 0, false);
			stop(&e, NOW, ZERO, 5, false);
		}
#endif
#if GBN_RV32_FAST & 8
		e.cv_dead = (cv_dead >> i) & 1;
#endif
#if GBN_RV32_FAST & 16384
		e.live = i < live_count ? live_after[i] : FLAGS_ALL;
#endif
		uint32_t code = block->thumb ? fetch(m, pc, width) : 0;
		unsigned bulk_count = block->thumb && (code & 0xf000) == 0x6000 ?
			thumb_io_group(&e,backend,m,i,code) : 0;
		if (bulk_count) { i += bulk_count - 1; goto check_size; }
#if (GBN_RV32_FAST & 8388608) && (GBN_RV32_FAST & 4194304) && (GBN_RV32_FAST & 1024) && (GBN_RV32_FAST & 16384)
		if (block->thumb && (bulk_count = thumb_pop_pair(&e, m, i, code))) { i += bulk_count - 1; goto check_size; }
#endif
		arm_mixer_packet(&e,backend,m,i);
		bulk_count=arm_mixer_bulk(&e,backend,m,i,ram_guard_words,edge_count);
		if(!bulk_count)bulk_count=arm_bulk(&e,m,i,ram_guard_words,edge_count);
		if (bulk_count) { i += bulk_count - 1; goto check_size; }
		if (!block->thumb) code = fetch(m, pc, width);
        /* Keep a conditional branch's fallthrough in this native block.
         * A taken edge gets its own exact partial count and refill below. */
        if (!block->thumb && code >> 28 < 14 && (code & 0x0e000000u) == 0x0a000000u) {
            uint32_t target = pc + 8 + (uint32_t) ((int32_t) (code << 8) >> 6);
            if (target >> 24 != m->code_region || (m->code_region >= 8
                    && (target & (GBN_ROM_MAX_SIZE - 1)) >= m->rom_size)) {
                e.count = start; e.stop_count = stops; COLD_RESTORE TIME_RESTORE block->fallback = true; break;
            }
            exact |= e.count + COLD_WORDS + 340 + 64 * (edge_count + 1)
                    + (13 + ram_guard_words) * (i + 1) >= GBN_RV32_WORDS;
            condition(&e, code >> 28);
            imm(&e, NOW, NOW, 0, 1 + m->code_word_wait);
            struct ArmEdge* edge = &edges[edge_count++];
            *edge = (struct ArmEdge) {.at = e.count, .count = i + 1,
                .pc = pc, .target = target, .link = (code & 0x1000000) != 0};
            word(&e, 0);
            ++block->instructions;
            goto check_size;
        }

		if (!(block->thumb ? instruction(&e, m, code, pc) : arm_instruction(&e, m, code, pc))) {
			e.count = start; e.stop_count = stops; COLD_RESTORE TIME_RESTORE
			block->fallback = true; break;
		}
#if GBN_RV32_FAST & 1024
		if (block->thumb) e.deferred += 1 + m->code_wait; else
#endif
		imm(&e, NOW, NOW, 0, 1 + (block->thumb ? m->code_wait : m->code_word_wait));
		++block->instructions;
    check_size:
        if (exact && !body_fits(backend, &e, prologue_words, edges, edge_count)) {
            e.count = start; e.stop_count = stops; COLD_RESTORE TIME_RESTORE e.written = saved_written;
            memcpy(e.thumb_literal, saved_thumb_literal, sizeof(saved_thumb_literal));
            e.thumb_literal_mask = saved_thumb_literal_mask;
			e.io_guarded=saved_io_guarded;
            e.read_only = saved_read_only; e.read_seen = saved_read_seen;
            e.dynamic_pc = saved_dynamic_pc;
            *block = saved_block; edge_count = saved_edges;
            e.return_count=saved_returns;e.loop_jump_count=saved_loops;
            break;
        }
		if (block->branch || e.dynamic_pc) break;
	}
#if GBN_RV32_FAST & 8388608
	e.insn_start = ~0u;
#endif
    unsigned loop_entry;
#if GBN_RV32_FAST & 8
    e.cv_dead = false;
    for (unsigned i = 0; i < block->instructions && i < GBN_RV32_INSNS; ++i)
        if ((cv_dead >> i & 1) && cv_killer[i] >= block->instructions) {
            allow_cv = false;
            goto restart;
        }
#endif
#if GBN_RV32_FAST & 1
    if (fast_budget_at != ~0u && block->instructions)
        block->code[fast_budget_at] = (uint32_t) block->instructions << 20 | ZERO << 15 | TEMP << 7 | 0x13;
#endif
#if GBN_RV32_FAST & 1024
    TIME_FILL
#endif
    unsigned chain = finish_body(backend, block, &e, prologue_words, edges, edge_count, &loop_entry);
#if GBN_RV32_FAST & 256
	if (m->code_region >= 8) chain = chain_at;
#endif
	e.count = 0;
#if GBN_RV32_FAST & 4
	far_jump(&e, shared_frame(0, m->code_region < 8), LHS, LHS);
	word(&e, beq(ZERO, ZERO, 4 * (loop_entry - e.count)));
	if (0) {
#endif
	imm(&e, SP, SP, 0, -FRAME_BYTES);
	stack(&e, NOW, 0, true); stack(&e, HOST_S1, 4, true);
	stack(&e, PREFETCH, 8, true); stack(&e, WAIT, 12, true); stack(&e, HOST_S4, 16, true);
	stack(&e, NEGATIVE, SAVED_NEGATIVE, true); stack(&e, IS_ZERO, SAVED_ZERO, true);
	if (m->code_region < 8) { stack(&e, RAM_EPOCH, SAVED_EPOCH, true); imm(&e, RAM_EPOCH, TEMP, 0, 0); }
	stack(&e, CONTEXT, SAVED_CONTEXT, true); imm(&e, CONTEXT, 14, 0, 0);
	stack(&e, 23, SAVED_S7, true); stack(&e, 24, SAVED_S8, true);
    stack(&e, 27, SAVED_S11, true); stack(&e, RA, SAVED_RA, true);
    stack(&e, LIMIT, INITIAL_CAP, true); stack(&e, FLAGS, INITIAL_DEADLINE, true);
	load(&e, NOW, offsetof(struct Gbn, now));
	op(&e, NOW, NOW, FLAGS, 0, 0x20);
	load(&e, PREFETCH, offsetof(struct Gbn, prefetched_pc));
	for (unsigned i = 0; i < 15; ++i) if (resident(i)) load(&e, guest[i], offsetof(struct Gbn, cpu.r) + 4 * i);
	load(&e, FLAGS, offsetof(struct Gbn, cpu.cpsr));
	imm(&e, NEGATIVE, FLAGS, 5, 31); imm(&e, IS_ZERO, FLAGS, 5, 30);
	imm(&e, IS_ZERO, IS_ZERO, 7, 1);
	imm(&e, FLAGS, FLAGS, 1, 2); imm(&e, FLAGS, FLAGS, 5, 2);
	word(&e, beq(ZERO, ZERO, 4 * (loop_entry - e.count)));
#if GBN_RV32_FAST & 4
	}
#endif
	if (m->code_region < 8) {
		unsigned slot = (unsigned) (block - backend->blocks);
		memcpy(backend->source[slot], m->code + (block->pc & m->code_mask),
			(block->proof_instructions > block->instructions ? block->proof_instructions : block->instructions ? block->instructions : 1) * width);
	}
#if GBN_RV32_FAST & 16
	link_site_count = e.link_count;
	for (unsigned k = 0; k < e.link_count; ++k) { link_site_at[k] = e.link_at[k]; link_site_pc[k] = e.link_pc[k]; }
#endif
	if (block->instructions) allocate_code(backend, block, chain);
	else block->code = NULL;
#if GBN_RV32_STATS
	++backend->compiled_blocks;
#endif
#undef COLD_WORDS
#undef COLD_SAVE
#undef COLD_RESTORE
#undef TIME_SAVE
#undef TIME_RESTORE
#undef TIME_FILL
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
#if GBN_RV32_FAST & 8192
	if (!thumb && m->code_region == 3) {
		uint32_t done = mixer_hle(m);
		if (done) return done;
#if GBN_RV32_FAST & 524288
		done = mixer_fix_hle(m);
		if (done) return done;
#endif
	}
#endif
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
#if defined(AM_RVV) && (GBN_PPU_RVV & 8)
	if (hit && !rom && !gbn_rvv_equal(backend->source[slot], source + (m->cpu.pc & m->code_mask),
	    (b->proof_instructions > b->instructions ? b->proof_instructions : b->instructions ? b->instructions : 1) * width)) hit = false;
#else
	if (hit && !rom && memcmp(backend->source[slot], source + (m->cpu.pc & m->code_mask),
	    (b->proof_instructions > b->instructions ? b->proof_instructions : b->instructions ? b->instructions : 1) * width)) hit = false;
#endif
	if (hit && !b->instructions) return 0;
	uint32_t deadline = m->now + GBN_MAX_DELAY;
	int next=gbn_cpu_event(m);
	if (next >= 0) {
		deadline = m->events[next].when;
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
	uint32_t epoch = 0;
	if (!rom) {
		/* Reserve one distinct token per possible guest store, plus entry.
		 * No native instruction increments it more than once. Clear proofs
		 * before the reserved range would wrap; external changes are checked
		 * anew at every C entry, including DMA and raw host RAM writes. */
		if (backend->next_epoch > UINT32_MAX - cap - 1) {
			for (unsigned i = 0; i < GBN_RV32_SLOTS; ++i) backend->blocks[i].checked_epoch = 0;
			backend->next_epoch = 0;
		}
		epoch = backend->next_epoch + 1;
		backend->next_epoch += cap + 1;
		b->checked_epoch = epoch; /* The C entry just validated these bytes. */
	}
	typedef uint64_t (*Function)(struct Gbn*, uint32_t, uint32_t, uint32_t, uint32_t);
	uint64_t result = ((Function) (uintptr_t) b->code)(m, cap, deadline, epoch, b->context_tag);
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
