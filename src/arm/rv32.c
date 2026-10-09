/* SPDX-License-Identifier: MPL-2.0 */
#include <mgba/internal/arm/rv32.h>

#include <mgba/internal/arm/decoder.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/memory.h>

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#if defined(MGBA_RV32_VERIFY) || defined(MGBA_RV32_STATS)
#include <stdio.h>
#include <inttypes.h>
#endif

#define RV32_BLOCK_MAX_INSTRUCTIONS 32
#ifndef RV32_BLOCK_CACHE_SIZE
#define RV32_BLOCK_CACHE_SIZE 512
#endif
#define RV32_MAX_CONTEXTS 4
#define RV32_NATIVE_MAX_WORDS 768
#define RV32_DISPATCH_MAX_WORDS 128
#define RV32_BRANCH_MAX_WORDS 192
#define RV32_DATA_MAX_WORDS 256
#define RV32_DATA_HELPERS 16
#define RV32_BLOCK_BODY_WORD 26
#define RV32_NATIVE_FRAME_SIZE 64
#define RV32_DEVICE_RETRY_SLICES 32
#define RV32_POLL_END_MAX_WORDS 128

_Static_assert(RV32_BLOCK_CACHE_SIZE >= 2 && RV32_BLOCK_CACHE_SIZE <= 2048 &&
	(RV32_BLOCK_CACHE_SIZE & (RV32_BLOCK_CACHE_SIZE - 1)) == 0,
	"RV32 cache size must be a power of two between 2 and 2048");

#if defined(__riscv) && __riscv_xlen == 32
#define RV32_NATIVE 1
#else
#define RV32_NATIVE 0
#endif

enum RV32Register {
	RV_X0 = 0,
	RV_RA = 1,
	RV_SP = 2,
	RV_S0 = 8,
	RV_S1 = 9,
	RV_S2 = 18,
	RV_S3 = 19,
	RV_S11 = 27,
	RV_T0 = 5,
	RV_T1 = 6,
	RV_T2 = 7,
	RV_T3 = 28,
	RV_T4 = 29,
	RV_T5 = 30,
	RV_T6 = 31,
	RV_A0 = 10,
	RV_A1 = 11,
	RV_A2 = 12,
	RV_A3 = 13,
	RV_A4 = 14,
	RV_A5 = 15,
	RV_A6 = 16,
	RV_A7 = 17
};

enum RV32NativeKind {
	RV32_NATIVE_NONE,
	RV32_NATIVE_MOV_IMM,
	RV32_NATIVE_MOV_REG,
	RV32_NATIVE_ADD_REG,
	RV32_NATIVE_ADD_IMM,
	RV32_NATIVE_SUB_REG,
	RV32_NATIVE_SUB_IMM,
	RV32_NATIVE_AND,
	RV32_NATIVE_EOR,
	RV32_NATIVE_ORR,
	RV32_NATIVE_BIC,
	RV32_NATIVE_MVN,
	RV32_NATIVE_TST,
	RV32_NATIVE_NEG,
	RV32_NATIVE_CMP_REG,
	RV32_NATIVE_CMP_IMM,
	RV32_NATIVE_SHIFT_LSL,
	RV32_NATIVE_SHIFT_LSR,
	RV32_NATIVE_SHIFT_ASR,
};

struct RV32NativeOp {
	enum RV32NativeKind kind;
	uint8_t rd;
	uint8_t rn;
	uint8_t rm;
	uint16_t immediate;
	bool affectsFlags;
};

struct RV32NativeSpan {
	uint16_t offset;
	uint16_t cycles;
	uint8_t length;
};

struct RV32RegisterCache {
	int8_t locations[16];
	uint32_t users[3];
	uint32_t dirty;
};

struct RV32PollSnapshot {
	int32_t cycles, cpsr, shifter, carry;
	uint32_t prefetch;
	int32_t gprs[16];
};

struct RV32Block {
	/* C entries and native edges read this header on every cache hit. Keep
	 * it beside the first prefetched opcodes, ahead of emission-only spans. */
	const uint32_t* region;
	uint32_t mask;
	uint32_t start;
	uint32_t seqCycles;
	uint32_t verifiedEpoch;
	uint32_t branchEpoch; /* Full direct-edge proof, valid until a C/event boundary. */
	bool valid;
	bool executable;
	bool arm;
	bool writable;
	uint8_t length;
	uint16_t nativeWords;
	bool resident; /* Emission mode: shared native register convention. */
	bool sharedData; /* Shared IWRAM and general memory paths. */
	bool pollLoop;
	bool loopPure; /* Cleared by external callbacks while probing one iteration. */
	union {
		uint16_t opcodes[RV32_BLOCK_MAX_INSTRUCTIONS + 2];
		uint32_t armOpcodes[RV32_BLOCK_MAX_INSTRUCTIONS + 2];
	};
	struct RV32Context* context;
	uint32_t pendingStart;
	bool pendingValid;
	const uint32_t* dispatch;
	const uint32_t* branchDispatch;
	struct RV32Block* cache;
	struct RV32RegisterCache* registers; /* Non-NULL only while emitting a span. */
	uint32_t pollWrites;
	struct RV32NativeSpan spans[RV32_BLOCK_MAX_INSTRUCTIONS];
	struct RV32PollSnapshot pollSnapshot;
	uint32_t nativeCode[RV32_NATIVE_MAX_WORDS];
};

struct RV32Context {
	struct ARMCore* cpu;
	const uint32_t* rom;
	uint32_t codeEpoch; /* Zero means a callback/event may have changed code. */
	uint32_t epochSerial;
	unsigned deviceSlices; /* Bounded backoff after ARM device fast-path misses. */
#ifdef MGBA_RV32_TRACE_POLL
	uint32_t traceAllowed, tracePC;
	int32_t traceCycles, traceCPSR, traceGprs[15];
	uint32_t traceHalt;
	uint32_t traceCode[160];
#endif
	bool dispatchReady;
	bool branchReady;
	bool armDispatchReady;
	bool armBranchReady;
	bool dataReady;
	uint32_t dispatchCode[RV32_DISPATCH_MAX_WORDS];
	uint32_t branchCode[RV32_BRANCH_MAX_WORDS];
	uint32_t armDispatchCode[RV32_DISPATCH_MAX_WORDS];
	uint32_t armBranchCode[RV32_BRANCH_MAX_WORDS];
	uint32_t dataCode[RV32_DATA_HELPERS][RV32_DATA_MAX_WORDS];
	struct RV32Block blocks[RV32_BLOCK_CACHE_SIZE];
	/* A copy at either end keeps default-size cache calls in JAL range. */
	uint32_t dataCodeTail[RV32_DATA_HELPERS][RV32_DATA_MAX_WORDS];
};

static uint32_t _blockOpcode(const struct RV32Block* block, unsigned index) {
	return block->arm ? block->armOpcodes[index] : block->opcodes[index];
}

/* Device callbacks, interpreted instructions and event/host boundaries can
 * change ROM or edge-guard state. Each such boundary starts a new proof epoch.
 * Zero requests a new epoch in C; generated callbacks normally increment it
 * directly. No write hook is needed in every cartridge/debugger/DMA path. */
static uint32_t _codeEpoch(struct RV32Context* context) {
	if (!context->codeEpoch) {
		if (!++context->epochSerial) {
			context->epochSerial = 1;
			for (unsigned i = 0; i < RV32_BLOCK_CACHE_SIZE; ++i) {
				context->blocks[i].verifiedEpoch = 0;
				context->blocks[i].branchEpoch = 0;
			}
		}
		context->codeEpoch = context->epochSerial;
	}
	return context->codeEpoch;
}

/* This helper touches cache metadata only: resident CPU state need not be
 * spilled around it. The first two words belong to the guest pipeline and are
 * checked by the cache entry, not reloaded from possibly modified ROM bytes. */
static bool _validateWritableBlock(struct RV32Block* block) {
	struct RV32Context* context = block->context;
	uint32_t epoch = _codeEpoch(context);
	if (block->verifiedEpoch == epoch) return true;
	const struct GBA* gba = (const struct GBA*) context->cpu->master;
	unsigned width = block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB;
	uint32_t last = block->start + (block->length + 1) * width;
	if (block->region != gba->memory.rom || gba->memory.romSize < width ||
	    (last >> BASE_OFFSET) != (block->start >> BASE_OFFSET) ||
	    (last & (GBA_SIZE_ROM0 - 1)) > gba->memory.romSize - width) goto changed;
	for (unsigned i = 2; i < (unsigned) block->length + 2; ++i) {
		uint32_t opcode, offset = (block->start + i * width) & block->mask;
		if (block->arm) { LOAD_32(opcode, offset, block->region); }
		else { LOAD_16(opcode, offset, block->region); }
		if (opcode != _blockOpcode(block, i)) goto changed;
	}
	block->verifiedEpoch = epoch;
	return true;
changed:
	block->branchEpoch = 0;
	block->valid = false;
	block->loopPure = false;
	return false;
}

static struct RV32Context* _contexts[RV32_MAX_CONTEXTS];
static struct RV32Context* _lastContext;

#ifdef MGBA_RV32_STATS
static struct {
	uint32_t slices, lookups, hits, cold, missInvalid, missConflict, missState;
	uint32_t interior, interiorSpan, builds, repeatBuilds, evictions, buildFail, tableFull;
	uint32_t nativeCalls, nativeEvent, nativePartial, nativeReturn;
	uint32_t dispatch[2], dispatchMiss[2], branch[2], branchMiss[2], branchProof[2];
	uint64_t buildCycles, buildRetired;
} _stats;
static struct { uint32_t key, builds, overlaps; } _statsPC[8192];
static uint64_t _statsCounter(bool retired) {
#if RV32_NATIVE
	uint32_t hi, lo, again;
	do {
		if (retired) __asm__ volatile("csrr %0, minstreth; csrr %1, minstret; csrr %2, minstreth" : "=r"(hi), "=r"(lo), "=r"(again));
		else __asm__ volatile("csrr %0, mcycleh; csrr %1, mcycle; csrr %2, mcycleh" : "=r"(hi), "=r"(lo), "=r"(again));
	} while (hi != again);
	return ((uint64_t) hi << 32) | lo;
#else
	UNUSED(retired);
	return 0;
#endif
}
void RV32StatsReset(void) {
	memset(&_stats, 0, sizeof(_stats));
	for (unsigned i = 0; i < 8192; ++i) _statsPC[i].builds = _statsPC[i].overlaps = 0;
}
void RV32StatsReport(void) {
	printf("RV32 stats: slices=%u; lookups=%u; hits=%u; cold=%u; invalid=%u; conflict=%u; state=%u\n",
	       _stats.slices, _stats.lookups, _stats.hits, _stats.cold, _stats.missInvalid, _stats.missConflict, _stats.missState);
	printf("RV32 builds: total=%u; repeated=%u; evictions=%u; failed=%u; interior=%u; span=%u; table_full=%u; cycles=%" PRIu64 "; instret=%" PRIu64 "\n",
	       _stats.builds, _stats.repeatBuilds, _stats.evictions, _stats.buildFail, _stats.interior, _stats.interiorSpan,
	       _stats.tableFull, _stats.buildCycles, _stats.buildRetired);
	printf("RV32 returns: calls=%u; event=%u; partial=%u; other=%u\n", _stats.nativeCalls, _stats.nativeEvent, _stats.nativePartial, _stats.nativeReturn);
	for (unsigned i = 0; i < 2; ++i)
		printf("RV32 edges: arm=%u; dispatch=%u; dispatch_miss=%u; branch=%u; branch_miss=%u; proof_hit=%u\n",
		       i, _stats.dispatch[i], _stats.dispatchMiss[i], _stats.branch[i], _stats.branchMiss[i], _stats.branchProof[i]);
	for (unsigned i = 0; i < 8192; ++i) if (_statsPC[i].builds)
		printf("RV32 compile: pc=%08" PRIx32 "; arm=%u; count=%u; overlaps=%u\n",
		       _statsPC[i].key & ~1u, _statsPC[i].key & 1u, _statsPC[i].builds, _statsPC[i].overlaps);
}
#define STAT_ADD(field) (++_stats.field)
#else
#define STAT_ADD(field) ((void) 0)
#endif

bool RV32ResolveCode(const struct ARMCore* cpu, uintptr_t pc, struct RV32CodeLocation* location) {
	if (!cpu || !location || (pc & 3)) return false;
	for (unsigned i = 0; i < RV32_MAX_CONTEXTS; ++i) {
		const struct RV32Context* context = _contexts[i];
		if (!context || context->cpu != cpu) continue;
		uintptr_t offset = pc - (uintptr_t) context->blocks;
		if (offset < sizeof(context->blocks)) {
			const struct RV32Block* block = &context->blocks[offset / sizeof(*block)];
			offset = pc - (uintptr_t) block->nativeCode;
			if (!block->valid || offset / 4 >= block->nativeWords) return false;
			*location = (struct RV32CodeLocation) {
				.kind = block->arm ? RV32_CODE_ARM : RV32_CODE_THUMB,
				.address = block->start, .offset = offset,
			};
			return true;
		}
		/* Both copies of a shared memory routine use the same logical key. */
		if (context->dataReady) {
			offset = pc - (uintptr_t) context->dataCode;
			if (offset >= sizeof(context->dataCode)) offset = pc - (uintptr_t) context->dataCodeTail;
			if (offset < sizeof(context->dataCode)) {
				*location = (struct RV32CodeLocation) {
					.kind = RV32_CODE_DATA, .address = offset / sizeof(context->dataCode[0]),
					.offset = offset % sizeof(context->dataCode[0]),
				};
				return true;
			}
		}
#define RESOLVE_HELPER(ready, code, codeKind) \
		if (context->ready && pc - (uintptr_t) context->code < sizeof(context->code)) { \
			*location = (struct RV32CodeLocation) { \
				.kind = codeKind, .offset = pc - (uintptr_t) context->code, \
			}; \
			return true; \
		}
		RESOLVE_HELPER(dispatchReady, dispatchCode, RV32_CODE_THUMB_DISPATCH);
		RESOLVE_HELPER(armDispatchReady, armDispatchCode, RV32_CODE_ARM_DISPATCH);
		RESOLVE_HELPER(branchReady, branchCode, RV32_CODE_THUMB_BRANCH);
		RESOLVE_HELPER(armBranchReady, armBranchCode, RV32_CODE_ARM_BRANCH);
#ifdef MGBA_RV32_TRACE_POLL
		RESOLVE_HELPER(branchReady, traceCode, RV32_CODE_THUMB_TRACE);
#endif
#undef RESOLVE_HELPER
	}
	return false;
}

/* Fold the ROM page into the index: aligned functions in different pages
 * otherwise replace each other despite most cache slots remaining free. */
static unsigned _blockIndex(uint32_t address) {
	unsigned bits = __builtin_ctz(RV32_BLOCK_CACHE_SIZE);
	return ((address >> 1) ^ (address >> (1 + bits)) ^ (address >> (1 + 2 * bits))) &
		(RV32_BLOCK_CACHE_SIZE - 1);
}

#if RV32_NATIVE
/* Callee-saved registers survive native block links. C helpers and all exits
 * synchronize this state with ARMCore. High guest registers remain in memory. */
static unsigned _residentRegister(unsigned offset) {
	if (offset == offsetof(struct ARMCore, cycles)) return RV_S1;
	if (offset == offsetof(struct ARMCore, cpsr.packed)) return RV_S2;
	if (offset - offsetof(struct ARMCore, gprs[0]) < 8 * sizeof(int32_t))
		return RV_S3 + (offset - offsetof(struct ARMCore, gprs[0])) / sizeof(int32_t);
	if (offset == offsetof(struct ARMCore, gprs[ARM_SP])) return RV_S11;
	return 0;
}

static bool _spillRegister(struct RV32Block* block, unsigned rd) {
	struct RV32RegisterCache* cache = block->registers;
	uint32_t users = cache->users[rd - RV_T0];
	while (users) {
		unsigned r = __builtin_ctz(users);
		unsigned bit = 1u << r;
		if (cache->dirty & bit) {
			unsigned offset = offsetof(struct ARMCore, gprs[r]);
			uint32_t spill = ((offset & 0xFE0) << 20) | (rd << 20) |
				(RV_A0 << 15) | (2 << 12) | ((offset & 31) << 7) | 0x23;
			unsigned resident = block->resident ? _residentRegister(offset) : 0;
			if (resident) spill = (rd << 15) | (resident << 7) | 0x13;
			if (block->nativeWords >= RV32_NATIVE_MAX_WORDS) return false;
			block->nativeCode[block->nativeWords++] = spill;
		}
		cache->locations[r] = -1;
		cache->dirty &= ~bit;
		users &= ~bit;
	}
	cache->users[rd - RV_T0] = 0;
	return true;
}

static inline __attribute__((always_inline)) bool _emit(struct RV32Block* block, uint32_t instruction) {
	unsigned opcode = instruction & 0x7F;
	unsigned rd = (instruction >> 7) & 31;
	/* Keep the common emitter small so constant opcode/register checks fold
	 * away. Only temporaries with live guest users need spill bookkeeping. */
	if (rd >= RV_T0 && rd <= RV_T2 &&
	    (opcode == 0x33 || opcode == 0x13 || opcode == 0x03 || opcode == 0x37) &&
	    block->registers && block->registers->users[rd - RV_T0] && !_spillRegister(block, rd)) return false;
	if (block->nativeWords >= RV32_NATIVE_MAX_WORDS) return false;
	block->nativeCode[block->nativeWords++] = instruction;
	return true;
}

static uint32_t _rvR(unsigned funct7, unsigned rs2, unsigned rs1, unsigned funct3, unsigned rd) {
	return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | 0x33;
}

static uint32_t _rvI(int immediate, unsigned rs1, unsigned funct3, unsigned rd, unsigned opcode) {
	return ((uint32_t) immediate << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode;
}

static uint32_t _rvS(int immediate, unsigned rs2, unsigned rs1, unsigned funct3) {
	return (((uint32_t) immediate & 0xFE0) << 20) | (rs2 << 20) |
		(((uint32_t) immediate & 0x1F) << 7) | (rs1 << 15) | (funct3 << 12) | 0x23;
}

static uint32_t _rvU(uint32_t immediate, unsigned rd) {
	return (immediate & 0xFFFFF000) | (rd << 7) | 0x37;
}

static bool _emitR(struct RV32Block* block, unsigned funct7, unsigned rs2, unsigned rs1,
	                 unsigned funct3, unsigned rd) {
	return _emit(block, _rvR(funct7, rs2, rs1, funct3, rd));
}

static bool _emitShift(struct RV32Block* block, unsigned funct7, unsigned shamt, unsigned rs1,
	                    unsigned funct3, unsigned rd) {
	if (shamt > 31) return false;
	return _emit(block, (funct7 << 25) | (shamt << 20) | (rs1 << 15) |
		(funct3 << 12) | (rd << 7) | 0x13);
}

static bool _emitI(struct RV32Block* block, int immediate, unsigned rs1, unsigned funct3,
	                 unsigned rd) {
	return immediate >= -2048 && immediate <= 2047 &&
		_emit(block, _rvI(immediate, rs1, funct3, rd, 0x13));
}

#ifdef MGBA_RV32_STATS
/* Only shared helper entry/exit paths use this; T0/T1 are dead there. */
static bool _emitStatsIncrement(struct RV32Block* block, uint32_t* counter) {
	uintptr_t address = (uintptr_t) counter;
	uint32_t upper = (address + 0x800u) & 0xFFFFF000u;
	return _emit(block, upper | (RV_T0 << 7) | 0x37) &&
		_emitI(block, (int32_t) (address - upper), RV_T0, 0, RV_T0) &&
		_emit(block, _rvI(0, RV_T0, 2, RV_T1, 0x03)) &&
		_emitI(block, 1, RV_T1, 0, RV_T1) &&
		_emit(block, _rvS(0, RV_T1, RV_T0, 2));
}
#define EMIT_STAT(block, field) do { if (!_emitStatsIncrement(block, &_stats.field)) return false; } while (0)
#else
#define EMIT_STAT(block, field) ((void) 0)
#endif

static bool _emitLoad(struct RV32Block* block, unsigned rd, int offset) {
	unsigned resident = block->resident ? _residentRegister(offset) : 0;
	if (resident) return rd == resident || _emitI(block, 0, resident, 0, rd);
	return offset >= -2048 && offset <= 2047 && _emit(block, _rvI(offset, RV_A0, 2, rd, 0x03));
}

static bool _emitStore(struct RV32Block* block, unsigned rs, int offset) {
	unsigned resident = block->resident ? _residentRegister(offset) : 0;
	if (resident) return rs == resident || _emitI(block, 0, rs, 0, resident);
	return offset >= -2048 && offset <= 2047 && _emit(block, _rvS(offset, rs, RV_A0, 2));
}

static bool _emitLoadRegister(struct RV32Block* block, unsigned rd, unsigned armRegister) {
	struct RV32RegisterCache* cache = block->registers;
	if (!cache || rd < RV_T0 || rd > RV_T2)
		return _emitLoad(block, rd, offsetof(struct ARMCore, gprs[armRegister]));
	int location = cache->locations[armRegister];
	if (location == (int) rd) return true;
	if (location >= 0) {
		if (!_emitI(block, 0, location, 0, rd)) return false;
	} else if (!_emitLoad(block, rd, offsetof(struct ARMCore, gprs[armRegister]))) return false;
	unsigned bit = 1u << armRegister;
	if (location >= 0) cache->users[location - RV_T0] &= ~bit;
	cache->locations[armRegister] = rd;
	cache->users[rd - RV_T0] |= bit;
	return true;
}

static bool _emitStoreRegister(struct RV32Block* block, unsigned rs, unsigned armRegister) {
	if (block->registers && rs >= RV_T0 && rs <= RV_T2) {
		struct RV32RegisterCache* cache = block->registers;
		unsigned bit = 1u << armRegister;
		int location = cache->locations[armRegister];
		if (location >= 0) cache->users[location - RV_T0] &= ~bit;
		cache->locations[armRegister] = rs;
		cache->users[rs - RV_T0] |= bit;
		cache->dirty |= bit;
		return true;
	}
	return _emitStore(block, rs, offsetof(struct ARMCore, gprs[armRegister]));
}

static bool _emitRegisterFlush(struct RV32Block* block) {
	uint32_t dirty = block->registers->dirty;
	while (dirty) {
		unsigned r = __builtin_ctz(dirty);
		if (!_emitStore(block, block->registers->locations[r], offsetof(struct ARMCore, gprs[r]))) return false;
		dirty &= dirty - 1;
	}
	return true;
}

static bool _emitResidentState(struct RV32Block* block, bool save) {
	if (!block->resident) return true;
	static const unsigned offsets[] = {
		offsetof(struct ARMCore, cycles), offsetof(struct ARMCore, cpsr.packed),
		offsetof(struct ARMCore, gprs[0]), offsetof(struct ARMCore, gprs[1]),
		offsetof(struct ARMCore, gprs[2]), offsetof(struct ARMCore, gprs[3]),
		offsetof(struct ARMCore, gprs[4]), offsetof(struct ARMCore, gprs[5]),
		offsetof(struct ARMCore, gprs[6]), offsetof(struct ARMCore, gprs[7]),
		offsetof(struct ARMCore, gprs[ARM_SP]),
	};
	for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
		unsigned reg = _residentRegister(offsets[i]);
		if (!_emit(block, save ? _rvS(offsets[i], reg, RV_S0, 2) :
		                        _rvI(offsets[i], RV_S0, 2, reg, 0x03))) return false;
	}
	return true;
}

static bool _emitLoadImmediate(struct RV32Block* block, unsigned rd, uint32_t value) {
	uint32_t upper = (value + 0x800) & 0xFFFFF000;
	int lower = (int) (value - upper);
	if (upper && !_emit(block, _rvU(upper, rd))) return false;
	if (lower && !_emitI(block, lower, upper ? rd : RV_X0, 0, rd)) return false;
	return upper || lower || _emitI(block, 0, RV_X0, 0, rd);
}

static bool _emitFlagNZ(struct RV32Block* block, unsigned result) {
	/* a3 = old flags with N/Z cleared; a4/a5 are temporary N/Z values. */
	return _emitR(block, 0, RV_T5, RV_A1, 7, RV_A3) &&
		_emitR(block, 0, RV_T3, result, 7, RV_A4) &&
		_emitI(block, 1, result, 3, RV_A5) &&
		_emitI(block, 30, RV_A5, 1, RV_A5) &&
		_emitR(block, 0, RV_A4, RV_A3, 6, RV_A3) &&
		_emitR(block, 0, RV_A5, RV_A3, 6, RV_A1);
}

static bool _emitShiftFlags(struct RV32Block* block, unsigned result, unsigned carry) {
	/* Clear N/Z/C, retain V and the control bits, then install the shifted
	 * carry before reusing the common N/Z builder. */
	return _emitR(block, 0, RV_T4, RV_T6, 6, RV_A5) &&
		_emitR(block, 0, RV_A5, RV_A1, 7, RV_A3) &&
		_emitI(block, 29, carry, 1, carry) &&
		_emitR(block, 0, carry, RV_A3, 6, RV_A3) &&
		_emitR(block, 0, RV_X0, RV_A3, 0, RV_A1) &&
		_emitFlagNZ(block, result);
}

static bool _emitFlagsAddSub(struct RV32Block* block, unsigned lhs, unsigned rhs, unsigned result,
	                           bool subtraction) {
	/* CPSR is N/Z/C/V in bits 31..28.  Keep the arithmetic here explicit:
	 * the guest operation is modulo 2^32, while C and V are derived from the
	 * unsigned carry and the sign-bit formulas. */
	/* Interleave independent C/V/Z chains: the target in-order core issues
	 * only a contiguous prefix, so adjacent dependent operations waste lanes.
	 * The interpreter clears the entire flags byte, including bits 24..27. */
	if (!_emitShift(block, 0, 8, RV_A1, 1, RV_A3)) return false;
	if (subtraction) {
		/* C = !borrow = !(lhs < rhs). */
		if (!_emitR(block, 0, rhs, lhs, 3, RV_A4)) return false;
	} else if (!_emitR(block, 0, lhs, result, 3, RV_A4)) {
		/* C = (result < lhs) for addition. */
		return false;
	}
	if (!_emitR(block, 0, lhs, rhs, 4, RV_A5) ||
	    !_emitR(block, 0, lhs, result, 4, RV_A6) ||
	    !_emitShift(block, 0, 8, RV_A3, 5, RV_A3)) return false;
	if (subtraction && !_emitI(block, 1, RV_A4, 3, RV_A4)) return false;
	if (!_emitI(block, 29, RV_A4, 1, RV_A4)) return false;
	if (!subtraction && !_emitI(block, -1, RV_A5, 4, RV_A5)) return false;
	if (!_emitI(block, 1, result, 3, RV_A7) ||
	    !_emitR(block, 0, RV_A6, RV_A5, 7, RV_A5) ||
	    !_emitI(block, 30, RV_A7, 1, RV_A7) ||
	    !_emitR(block, 0, RV_A4, RV_A3, 6, RV_A3) ||
	    !_emitShift(block, 0, 3, RV_A5, 5, RV_A5) ||
	    !_emitR(block, 0, RV_T3, result, 7, RV_A6) ||
	    !_emitR(block, 0, RV_T4, RV_A5, 7, RV_A5) ||
	    !_emitR(block, 0, RV_A5, RV_A3, 6, RV_A3) ||
	    !_emitR(block, 0, RV_A6, RV_A3, 6, RV_A3) ||
	    !_emitR(block, 0, RV_A7, RV_A3, 6, RV_A1)) return false;
	return true;
}

/* Thumb immediates admitted here are non-negative and fit in SLTIU. They
 * cannot supply a negative second operand, so overflow and carry simplify. */
static bool _emitFlagsImmediate(struct RV32Block* block, unsigned lhs, unsigned result,
                               unsigned immediate, bool subtraction) {
	if (!_emitShift(block, 0, 8, RV_A1, 1, RV_A3) ||
	    !_emitShift(block, 0, 8, RV_A3, 5, RV_A3) ||
	    !_emitR(block, 0, RV_T3, result, 7, RV_A6) ||
	    !_emitI(block, 1, result, 3, RV_A7) ||
	    !_emitShift(block, 0, 30, RV_A7, 1, RV_A7)) return false;
	if (!immediate) {
		/* +/- zero cannot overflow; SUB/CMP have C=1, ADD has C=0. */
		if (subtraction && (!_emitLoadImmediate(block, RV_A4, 0x20000000u) ||
		    !_emitR(block, 0, RV_A4, RV_A3, 6, RV_A3))) return false;
	} else {
		if (!_emitI(block, immediate, subtraction ? lhs : result, 3, RV_A4)) return false;
		if (subtraction && !_emitI(block, 1, RV_A4, 4, RV_A4)) return false;
		if (!_emitShift(block, 0, 29, RV_A4, 1, RV_A4) ||
		    !_emitI(block, -1, subtraction ? result : lhs, 4, RV_A5) ||
		    !_emitR(block, 0, subtraction ? lhs : result, RV_A5, 7, RV_A5) ||
		    !_emitShift(block, 0, 3, RV_A5, 5, RV_A5) ||
		    !_emitR(block, 0, RV_T4, RV_A5, 7, RV_A5) ||
		    !_emitR(block, 0, RV_A4, RV_A3, 6, RV_A3) ||
		    !_emitR(block, 0, RV_A5, RV_A3, 6, RV_A3)) return false;
	}
	return _emitR(block, 0, RV_A6, RV_A3, 6, RV_A3) &&
		_emitR(block, 0, RV_A7, RV_A3, 6, RV_A1);
}

static unsigned _residentGPR(unsigned reg) {
	return reg < 8 ? RV_S3 + reg : reg == ARM_SP ? RV_S11 : 0;
}

/* Return -1 for an operand that remains in ARMCore. Arithmetic uses the fixed
 * host registers directly, without staging every operand through temporaries. */
static int _emitResidentOp(struct RV32Block* block, const struct RV32NativeOp* op) {
	unsigned dest = _residentGPR(op->rd), lhs = _residentGPR(op->rn), rhs = _residentGPR(op->rm);
	int immediate = op->immediate;
	unsigned result;
	bool sub;
	switch (op->kind) {
	case RV32_NATIVE_MOV_IMM:
		if (!dest) return -1;
		return _emitI(block, immediate, RV_X0, 0, dest) &&
			(!op->affectsFlags || _emitFlagNZ(block, dest));
	case RV32_NATIVE_MOV_REG:
		if (!dest || !rhs) return -1;
		return (dest == rhs || _emitI(block, 0, rhs, 0, dest)) &&
			(!op->affectsFlags || _emitFlagNZ(block, dest));
	case RV32_NATIVE_ADD_REG: case RV32_NATIVE_SUB_REG:
		if (!dest || !lhs || !rhs) return -1;
		sub = op->kind == RV32_NATIVE_SUB_REG;
		result = op->affectsFlags ? RV_T0 : dest;
		return _emitR(block, sub ? 0x20 : 0, rhs, lhs, 0, result) &&
			(!op->affectsFlags || (_emitFlagsAddSub(block, lhs, rhs, result, sub) &&
			                       _emitI(block, 0, result, 0, dest)));
	case RV32_NATIVE_ADD_IMM: case RV32_NATIVE_SUB_IMM:
		if (!dest || !lhs) return -1;
		sub = op->kind == RV32_NATIVE_SUB_IMM;
		result = op->affectsFlags ? RV_T0 : dest;
		return _emitI(block, sub ? -immediate : immediate, lhs, 0, result) &&
			(!op->affectsFlags || (_emitFlagsImmediate(block, lhs, result, immediate, sub) &&
			                       _emitI(block, 0, result, 0, dest)));
	case RV32_NATIVE_AND: case RV32_NATIVE_EOR: case RV32_NATIVE_ORR: case RV32_NATIVE_BIC:
		if (!dest || !lhs || !rhs) return -1;
		if (op->kind == RV32_NATIVE_BIC) {
			if (!_emitI(block, -1, rhs, 4, RV_T0)) return 0;
			rhs = RV_T0;
		}
		return _emitR(block, 0, rhs, lhs, op->kind == RV32_NATIVE_EOR ? 4 :
		              op->kind == RV32_NATIVE_ORR ? 6 : 7, dest) &&
			(!op->affectsFlags || _emitFlagNZ(block, dest));
	case RV32_NATIVE_MVN:
		if (!dest || !rhs) return -1;
		return _emitI(block, -1, rhs, 4, dest) && (!op->affectsFlags || _emitFlagNZ(block, dest));
	case RV32_NATIVE_TST:
		if (!op->affectsFlags) return 1;
		if (!lhs || !rhs) return -1;
		return _emitR(block, 0, rhs, lhs, 7, RV_T0) && _emitFlagNZ(block, RV_T0);
	case RV32_NATIVE_NEG:
		if (!dest || !rhs) return -1;
		result = op->affectsFlags ? RV_T0 : dest;
		return _emitR(block, 0x20, rhs, RV_X0, 0, result) &&
			(!op->affectsFlags || (_emitFlagsAddSub(block, RV_X0, rhs, result, true) &&
			                       _emitI(block, 0, result, 0, dest)));
	case RV32_NATIVE_CMP_REG:
		if (!op->affectsFlags) return 1;
		if (!lhs || !rhs) return -1;
		return _emitR(block, 0x20, rhs, lhs, 0, RV_T0) && _emitFlagsAddSub(block, lhs, rhs, RV_T0, true);
	case RV32_NATIVE_CMP_IMM:
		if (!op->affectsFlags) return 1;
		if (!lhs) return -1;
		return (!immediate || _emitI(block, -immediate, lhs, 0, RV_T0)) &&
			_emitFlagsImmediate(block, lhs, immediate ? RV_T0 : lhs, immediate, true);
	case RV32_NATIVE_SHIFT_LSL: case RV32_NATIVE_SHIFT_LSR: case RV32_NATIVE_SHIFT_ASR: {
		if (!dest || !rhs) return -1;
		bool left = op->kind == RV32_NATIVE_SHIFT_LSL;
		bool arithmetic = op->kind == RV32_NATIVE_SHIFT_ASR;
		unsigned amount = immediate ? immediate : left ? 0 : 32;
		if (op->affectsFlags && amount) {
			if (!_emitShift(block, 0, left ? 32 - amount : amount - 1, rhs, 5, RV_A4) ||
			    !_emitI(block, 1, RV_A4, 7, RV_A4)) return 0;
		}
		if (amount == 32 && !arithmetic) {
			if (!_emitI(block, 0, RV_X0, 0, dest)) return 0;
		} else if (!_emitShift(block, arithmetic ? 0x20 : 0, amount == 32 ? 31 : amount,
		                       rhs, left ? 1 : 5, dest)) return 0;
		return !op->affectsFlags || (amount ? _emitShiftFlags(block, dest, RV_A4) : _emitFlagNZ(block, dest));
	}
	default:
		return -1;
	}
}

static bool _emitNativeOp(struct RV32Block* block, const struct RV32NativeOp* op) {
	if (block->resident) {
		int emitted = _emitResidentOp(block, op);
		if (emitted >= 0) return emitted;
	}
	unsigned rd = op->rd;
	unsigned rn = op->rn;
	unsigned rm = op->rm;
	int immediate = op->immediate;
	bool subtraction = false;
	unsigned result = RV_T0;

	switch (op->kind) {
	case RV32_NATIVE_MOV_IMM:
		if (!_emitI(block, immediate, RV_X0, 0, result) || !_emitStoreRegister(block, result, rd)) return false;
		return !op->affectsFlags || _emitFlagNZ(block, result);
	case RV32_NATIVE_MOV_REG:
		if (!_emitLoadRegister(block, RV_T1, rm) || !_emitStoreRegister(block, RV_T1, rd)) return false;
		return !op->affectsFlags || _emitFlagNZ(block, RV_T1);
	case RV32_NATIVE_ADD_REG:
	case RV32_NATIVE_SUB_REG:
		if (!_emitLoadRegister(block, RV_T1, rn) || !_emitLoadRegister(block, RV_T2, rm)) return false;
		subtraction = op->kind == RV32_NATIVE_SUB_REG;
		if (!_emitR(block, subtraction ? 0x20 : 0, RV_T2, RV_T1, 0, result) ||
		    !_emitStoreRegister(block, result, rd)) return false;
		return !op->affectsFlags || _emitFlagsAddSub(block, RV_T1, RV_T2, result, subtraction);
	case RV32_NATIVE_ADD_IMM:
	case RV32_NATIVE_SUB_IMM:
		if (!_emitLoadRegister(block, RV_T1, rn) ||
		    !_emitI(block, op->kind == RV32_NATIVE_SUB_IMM ? -immediate : immediate, RV_T1, 0, result) ||
		    !_emitStoreRegister(block, result, rd)) return false;
		return !op->affectsFlags || _emitFlagsImmediate(block, RV_T1, result, immediate,
		                                              op->kind == RV32_NATIVE_SUB_IMM);
	case RV32_NATIVE_AND:
	case RV32_NATIVE_EOR:
	case RV32_NATIVE_ORR:
	case RV32_NATIVE_BIC:
		if (!_emitLoadRegister(block, RV_T1, rn) || !_emitLoadRegister(block, RV_T2, rm)) return false;
		if (op->kind == RV32_NATIVE_AND) {
			if (!_emitR(block, 0, RV_T2, RV_T1, 7, result)) return false;
		} else if (op->kind == RV32_NATIVE_EOR) {
			if (!_emitR(block, 0, RV_T2, RV_T1, 4, result)) return false;
		} else if (op->kind == RV32_NATIVE_ORR) {
			if (!_emitR(block, 0, RV_T2, RV_T1, 6, result)) return false;
		} else {
			if (!_emitI(block, -1, RV_T2, 4, RV_T2) ||
			    !_emitR(block, 0, RV_T2, RV_T1, 7, result)) return false;
		}
		if (!_emitStoreRegister(block, result, rd)) return false;
		return !op->affectsFlags || _emitFlagNZ(block, result);
	case RV32_NATIVE_MVN:
		if (!_emitLoadRegister(block, RV_T1, rm) || !_emitI(block, -1, RV_T1, 4, result) ||
		    !_emitStoreRegister(block, result, rd)) return false;
		return !op->affectsFlags || _emitFlagNZ(block, result);
	case RV32_NATIVE_TST:
		if (!op->affectsFlags) return true;
		if (!_emitLoadRegister(block, RV_T1, rn) || !_emitLoadRegister(block, RV_T2, rm) ||
		    !_emitR(block, 0, RV_T2, RV_T1, 7, result)) return false;
		return !op->affectsFlags || _emitFlagNZ(block, result);
	case RV32_NATIVE_NEG:
		if (!_emitLoadRegister(block, RV_T2, rm) || !_emitR(block, 0x20, RV_T2, RV_X0, 0, result) ||
		    !_emitStoreRegister(block, result, rd)) return false;
		return !op->affectsFlags || _emitFlagsAddSub(block, RV_X0, RV_T2, result, true);
	case RV32_NATIVE_CMP_REG:
		if (!op->affectsFlags) return true;
		if (!_emitLoadRegister(block, RV_T1, rn) || !_emitLoadRegister(block, RV_T2, rm) ||
		    !_emitR(block, 0x20, RV_T2, RV_T1, 0, result)) return false;
		return _emitFlagsAddSub(block, RV_T1, RV_T2, result, true);
	case RV32_NATIVE_CMP_IMM:
		if (!op->affectsFlags) return true;
		if (!_emitLoadRegister(block, RV_T1, rn)) return false;
		if (immediate && !_emitI(block, -immediate, RV_T1, 0, result)) return false;
		return _emitFlagsImmediate(block, RV_T1, immediate ? result : RV_T1, immediate, true);
	case RV32_NATIVE_SHIFT_LSL:
	case RV32_NATIVE_SHIFT_LSR:
	case RV32_NATIVE_SHIFT_ASR:
		if (!_emitLoadRegister(block, RV_T1, rm)) return false;
		if (!op->affectsFlags) {
			unsigned amount = immediate;
			if (!amount && op->kind == RV32_NATIVE_SHIFT_LSR) {
				if (!_emitI(block, 0, RV_X0, 0, result)) return false;
			} else {
				if (!amount && op->kind == RV32_NATIVE_SHIFT_ASR) amount = 31;
				if (!_emitShift(block, op->kind == RV32_NATIVE_SHIFT_ASR ? 0x20 : 0,
				                amount, RV_T1, op->kind == RV32_NATIVE_SHIFT_LSL ? 1 : 5, result)) return false;
			}
			return _emitStoreRegister(block, result, rd);
		}
		if (op->kind == RV32_NATIVE_SHIFT_LSL) {
			if (!immediate) {
				if (!_emitR(block, 0, RV_X0, RV_T1, 0, result) ||
				    !_emitStoreRegister(block, result, rd)) return false;
				return _emitFlagNZ(block, result);
			}
			if (!_emitShift(block, 0, immediate, RV_T1, 1, result) ||
			    !_emitShift(block, 0, 32 - immediate, RV_T1, 5, RV_A4) ||
			    !_emitI(block, 1, RV_A4, 7, RV_A4)) return false;
		} else if (op->kind == RV32_NATIVE_SHIFT_LSR) {
			unsigned amount = immediate ? immediate : 32;
			if (amount == 32) {
				if (!_emitI(block, 0, RV_X0, 0, result) ||
				    !_emitShift(block, 0, 31, RV_T1, 5, RV_A4)) return false;
			} else if (!_emitShift(block, 0, amount, RV_T1, 5, result) ||
			           !_emitShift(block, 0, amount - 1, RV_T1, 5, RV_A4)) {
				return false;
			}
			if (!_emitI(block, 1, RV_A4, 7, RV_A4)) return false;
		} else {
			unsigned amount = immediate ? immediate : 32;
			if (amount == 32) {
				if (!_emitShift(block, 0x20, 31, RV_T1, 5, result) ||
				    !_emitShift(block, 0x20, 31, RV_T1, 5, RV_A4)) return false;
			} else if (!_emitShift(block, 0x20, amount, RV_T1, 5, result) ||
			           !_emitShift(block, 0x20, amount - 1, RV_T1, 5, RV_A4)) {
				return false;
			}
			if (!_emitI(block, 1, RV_A4, 7, RV_A4)) return false;
		}
		return _emitStoreRegister(block, result, rd) && _emitShiftFlags(block, result, RV_A4);
	case RV32_NATIVE_NONE:
		break;
	}
	return false;
}

static bool _nativeRegisterOperandsValid(const struct ARMInstructionInfo* info) {
	if ((info->operandFormat & ARM_OPERAND_REGISTER_1) && info->op1.reg == ARM_PC) return false;
	if ((info->operandFormat & ARM_OPERAND_REGISTER_2) && info->op2.reg == ARM_PC) return false;
	if ((info->operandFormat & ARM_OPERAND_REGISTER_3) && info->op3.reg == ARM_PC) return false;
	return true;
}

static bool _decodeNativeOp(const struct ARMInstructionInfo* info, struct RV32NativeOp* op) {
	memset(op, 0, sizeof(*op));
	if (info->branchType || info->traps || (info->operandFormat & ARM_OPERAND_MEMORY) ||
	    !_nativeRegisterOperandsValid(info)) return false;
	if (info->mnemonic != ARM_MN_MOV && info->mnemonic != ARM_MN_ADD && info->mnemonic != ARM_MN_SUB &&
	    info->mnemonic != ARM_MN_AND && info->mnemonic != ARM_MN_EOR && info->mnemonic != ARM_MN_ORR &&
	    info->mnemonic != ARM_MN_BIC && info->mnemonic != ARM_MN_MVN && info->mnemonic != ARM_MN_TST &&
	    info->mnemonic != ARM_MN_CMP && info->mnemonic != ARM_MN_NEG &&
	    info->mnemonic != ARM_MN_LSL && info->mnemonic != ARM_MN_LSR && info->mnemonic != ARM_MN_ASR) return false;
	op->rd = info->op1.reg;
	op->rn = info->op2.reg;
	op->rm = info->op3.reg;
	op->affectsFlags = info->affectsCPSR;

	switch (info->mnemonic) {
	case ARM_MN_MOV:
		if (info->operandFormat & ARM_OPERAND_IMMEDIATE_2) {
			op->kind = RV32_NATIVE_MOV_IMM;
			op->immediate = (uint16_t) info->op2.immediate;
			return true;
		}
		if (info->operandFormat & ARM_OPERAND_REGISTER_2) {
			op->kind = RV32_NATIVE_MOV_REG;
			op->rm = info->op2.reg;
			return true;
		}
		break;
	case ARM_MN_ADD:
		if (info->affectsCPSR) {
			if (info->operandFormat & ARM_OPERAND_REGISTER_3) {
				op->kind = RV32_NATIVE_ADD_REG;
				return true;
			}
			if (info->operandFormat & ARM_OPERAND_REGISTER_2) {
				op->kind = RV32_NATIVE_ADD_IMM;
				op->immediate = (uint16_t) info->op3.immediate;
				op->rn = info->op2.reg;
				return true;
			}
			if (info->operandFormat & ARM_OPERAND_IMMEDIATE_2) {
				op->kind = RV32_NATIVE_ADD_IMM;
				op->rn = op->rd;
				op->immediate = (uint16_t) info->op2.immediate;
				return true;
			}
		} else if (info->operandFormat & ARM_OPERAND_REGISTER_2) {
			op->kind = RV32_NATIVE_ADD_IMM;
			if (info->operandFormat & ARM_OPERAND_IMMEDIATE_3) {
				/* ADD (SP,#imm) is the only non-flagged immediate form
				 * admitted here; ADD (PC,#imm) needs the pipelined PC. */
				if (info->op2.reg != ARM_SP) return false;
				op->immediate = (uint16_t) info->op3.immediate;
				op->rn = info->op2.reg;
				return true;
			}
			op->kind = RV32_NATIVE_ADD_REG;
			op->rn = op->rd;
			op->rm = info->op2.reg;
			return true;
		} else if ((info->operandFormat & ARM_OPERAND_IMMEDIATE_2) && op->rd == ARM_SP) {
			op->kind = RV32_NATIVE_ADD_IMM;
			op->rn = ARM_SP;
			op->immediate = (uint16_t) info->op2.immediate;
			return true;
		}
		break;
	case ARM_MN_SUB:
		if (info->affectsCPSR) {
			if (info->operandFormat & ARM_OPERAND_REGISTER_3) {
				op->kind = RV32_NATIVE_SUB_REG;
				return true;
			}
			if (info->operandFormat & ARM_OPERAND_REGISTER_2) {
				op->kind = RV32_NATIVE_SUB_IMM;
				op->immediate = (uint16_t) info->op3.immediate;
				op->rn = info->op2.reg;
				return true;
			}
			if (info->operandFormat & ARM_OPERAND_IMMEDIATE_2) {
				op->kind = RV32_NATIVE_SUB_IMM;
				op->rn = op->rd;
				op->immediate = (uint16_t) info->op2.immediate;
				return true;
			}
		} else if ((info->operandFormat & ARM_OPERAND_IMMEDIATE_2) && op->rd == ARM_SP) {
			op->kind = RV32_NATIVE_SUB_IMM;
			op->rn = ARM_SP;
			op->immediate = (uint16_t) info->op2.immediate;
			return true;
		}
		break;
	case ARM_MN_AND:
		op->rn = op->rd;
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_AND;
		return true;
	case ARM_MN_EOR:
		op->rn = op->rd;
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_EOR;
		return true;
	case ARM_MN_ORR:
		op->rn = op->rd;
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_ORR;
		return true;
	case ARM_MN_BIC:
		op->rn = op->rd;
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_BIC;
		return true;
	case ARM_MN_MVN:
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_MVN;
		return true;
	case ARM_MN_TST:
		op->rn = op->rd;
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_TST;
		return true;
	case ARM_MN_NEG:
		op->rm = info->op2.reg;
		op->kind = RV32_NATIVE_NEG;
		return true;
	case ARM_MN_CMP:
		if (info->operandFormat & ARM_OPERAND_IMMEDIATE_2) {
			op->kind = RV32_NATIVE_CMP_IMM;
			op->rn = info->op1.reg;
			op->immediate = (uint16_t) info->op2.immediate;
			return true;
		}
		if (info->operandFormat & ARM_OPERAND_REGISTER_2) {
			op->kind = RV32_NATIVE_CMP_REG;
			op->rn = info->op1.reg;
			op->rm = info->op2.reg;
			return true;
		}
		break;
	case ARM_MN_LSL:
	case ARM_MN_LSR:
	case ARM_MN_ASR:
		if (!(info->operandFormat & ARM_OPERAND_IMMEDIATE_3)) return false;
		op->rm = info->op2.reg;
		op->immediate = (uint16_t) info->op3.immediate;
		op->kind = info->mnemonic == ARM_MN_LSL ? RV32_NATIVE_SHIFT_LSL :
			info->mnemonic == ARM_MN_LSR ? RV32_NATIVE_SHIFT_LSR : RV32_NATIVE_SHIFT_ASR;
		return true;
	default:
		break;
	}
	return false;
}

static bool _emitNativePrologue(struct RV32Block* block, const struct RV32NativeOp* ops, unsigned length) {
	bool nz = false, shift = false, arithmetic = false;
	for (unsigned i = 0; i < length; ++i) {
		if (!ops[i].affectsFlags) continue;
		switch (ops[i].kind) {
		case RV32_NATIVE_ADD_REG: case RV32_NATIVE_ADD_IMM:
		case RV32_NATIVE_SUB_REG: case RV32_NATIVE_SUB_IMM:
		case RV32_NATIVE_NEG: case RV32_NATIVE_CMP_REG: case RV32_NATIVE_CMP_IMM:
			arithmetic = true;
			break;
		case RV32_NATIVE_SHIFT_LSL: case RV32_NATIVE_SHIFT_LSR: case RV32_NATIVE_SHIFT_ASR:
			shift = true;
			/* fall through */
		default:
			nz = true;
			break;
		}
	}
	return _emitLoad(block, RV_A1, offsetof(struct ARMCore, cpsr.packed)) &&
		_emitLoad(block, RV_A2, offsetof(struct ARMCore, cycles)) &&
		(!(nz || arithmetic) || _emitLoadImmediate(block, RV_T3, 0x80000000u)) &&
		(!(shift || arithmetic) || _emitLoadImmediate(block, RV_T4, 0x10000000u)) &&
		(!nz || _emitLoadImmediate(block, RV_T5, 0x3FFFFFFFu)) &&
		(!shift || _emitLoadImmediate(block, RV_T6, 0x0FFFFFFFu));
}

/* No admitted operation consumes flags; partial flag writers preserve the
 * other bits. Work backwards so only writes visible at the span exit survive. */
static unsigned _writtenFlags(const struct RV32NativeOp* op) {
	if (!op->affectsFlags) return 0;
	switch (op->kind) {
	case RV32_NATIVE_ADD_REG: case RV32_NATIVE_ADD_IMM:
	case RV32_NATIVE_SUB_REG: case RV32_NATIVE_SUB_IMM:
	case RV32_NATIVE_NEG: case RV32_NATIVE_CMP_REG: case RV32_NATIVE_CMP_IMM:
		return 0xFF;
	case RV32_NATIVE_SHIFT_LSL:
		return op->immediate ? 0xE0 : 0xC0;
	case RV32_NATIVE_SHIFT_LSR: case RV32_NATIVE_SHIFT_ASR:
		return 0xE0;
	default:
		return 0xC0;
	}
}
#endif

static unsigned _emitNativeSpan(struct RV32Block* block, struct ARMCore* cpu, unsigned start) {
#if RV32_NATIVE
	struct RV32NativeOp decoded[RV32_BLOCK_MAX_INSTRUCTIONS];
	unsigned length = 0;
	for (; start + length < block->length; ++length) {
		struct ARMInstructionInfo info;
		ARMDecodeThumb(block->opcodes[start + length], &info);
		if (!_decodeNativeOp(&info, &decoded[length])) break;
	}
	unsigned begin = block->nativeWords;
	while (length) {
		struct RV32NativeOp ops[RV32_BLOCK_MAX_INSTRUCTIONS];
		memcpy(ops, decoded, length * sizeof(*ops));
		unsigned live = 0xFF;
		for (unsigned i = length; i-- > 0;) {
			unsigned written = _writtenFlags(&ops[i]);
			ops[i].affectsFlags = (written & live) != 0;
			live &= ~written;
		}
		block->nativeWords = begin;
		struct RV32RegisterCache cache;
		block->registers = NULL;
		if (length > 1 && !block->resident) {
			memset(cache.locations, -1, sizeof(cache.locations));
			memset(cache.users, 0, sizeof(cache.users));
			cache.dirty = 0;
			block->registers = &cache;
		}
		if (!_emitNativePrologue(block, ops, length)) break;
		unsigned emitted;
		for (emitted = 0; emitted < length; ++emitted) {
			if (!_emitNativeOp(block, &ops[emitted]) || block->nativeWords > RV32_NATIVE_MAX_WORDS - 4) break;
		}
		if (emitted < length) {
			block->registers = NULL;
			/* Recompute liveness for the shorter prefix. Its final flags must
			 * be observable if the next instruction falls back to C. */
			length = emitted;
			continue;
		}
		bool flushed = !block->registers || _emitRegisterFlush(block);
		block->registers = NULL;
		if (!flushed || block->nativeWords > RV32_NATIVE_MAX_WORDS - 4) {
			--length;
			continue;
		}
		uint32_t cycles = length * (1u + cpu->memory.activeSeqCycles16);
		if (cycles > 2047 || !_emitI(block, (int) cycles, RV_A2, 0, RV_A2) ||
		    !_emitStore(block, RV_A1, (int) offsetof(struct ARMCore, cpsr.packed)) ||
		    !_emitStore(block, RV_A2, (int) offsetof(struct ARMCore, cycles)) ||
		    !_emit(block, _rvI(0, RV_RA, 0, RV_X0, 0x67))) break;
		block->spans[start] = (struct RV32NativeSpan) {begin, cycles, length};
		return length;
	}
	block->registers = NULL;
	block->nativeWords = begin;
#else
	UNUSED(block);
	UNUSED(cpu);
	UNUSED(start);
#endif
	return 0;
}

static bool _emitNativeBlock(struct RV32Block* block, struct ARMCore* cpu) {
	block->resident = false;
	block->seqCycles = cpu->memory.activeSeqCycles16;
	for (unsigned i = 0; i < block->length;) {
		unsigned length = _emitNativeSpan(block, cpu, i);
		i += length ? length : 1;
	}
#if RV32_NATIVE
	if (block->nativeWords) __asm__ volatile("fence.i" ::: "memory");
#endif
	return block->nativeWords != 0;
}

#if RV32_NATIVE
static uint32_t _rvB(int offset, unsigned rs2, unsigned rs1, unsigned funct3) {
	unsigned imm = (unsigned) offset;
	return ((imm & 0x1000) << 19) | ((imm & 0x7E0) << 20) |
		(rs2 << 20) | (rs1 << 15) | (funct3 << 12) |
		((imm & 0x1E) << 7) | ((imm & 0x800) >> 4) | 0x63;
}

/* Admit closed, read-only self loops whose register outputs and load
 * addresses do not depend on loop-carried writes. This excludes counters,
 * moving pointers, stores and unsupported instructions before any probing.
 * Runtime equality, callback and timing checks still decide acceleration. */
static bool _isPollingLoop(struct RV32Block* block) {
	if (!block->cache || !block->branchDispatch || !block->length) return false;
	unsigned last = block->length - 1;
	uint16_t opcode = block->opcodes[last];
	bool conditional = (opcode & 0xF000) == 0xD000 && (opcode & 0x0F00) < 0x0E00;
	if (!conditional && (opcode & 0xF800) != 0xE000) return false;
	int displacement = conditional ? (int8_t) opcode * 2 : (int16_t) (opcode << 5) / 16;
	if ((last + 2) * WORD_SIZE_THUMB + displacement != 0) return false;
	uint32_t dependencies[16], written = 0, addresses = 0;
	for (unsigned r = 0; r < 16; ++r) dependencies[r] = 1u << r;
	for (unsigned i = 0; i < last; ++i) {
		struct ARMInstructionInfo info;
		struct RV32NativeOp op;
		ARMDecodeThumb(block->opcodes[i], &info);
		unsigned rd;
		uint32_t inputs;
		if (_decodeNativeOp(&info, &op)) {
			rd = op.rd;
			switch (op.kind) {
			case RV32_NATIVE_TST: case RV32_NATIVE_CMP_REG: case RV32_NATIVE_CMP_IMM:
				continue;
			case RV32_NATIVE_MOV_IMM: inputs = 0; break;
			case RV32_NATIVE_MOV_REG: case RV32_NATIVE_MVN: case RV32_NATIVE_NEG:
			case RV32_NATIVE_SHIFT_LSL: case RV32_NATIVE_SHIFT_LSR: case RV32_NATIVE_SHIFT_ASR:
				inputs = dependencies[op.rm]; break;
			case RV32_NATIVE_ADD_IMM: case RV32_NATIVE_SUB_IMM:
				inputs = dependencies[op.rn]; break;
			default: inputs = dependencies[op.rn] | dependencies[op.rm]; break;
			}
		} else if (info.mnemonic == ARM_MN_LDR && !info.branchType && !info.traps && info.op1.reg != ARM_PC) {
			rd = info.op1.reg;
			inputs = dependencies[info.memory.baseReg];
			if (info.memory.format & ARM_MEMORY_REGISTER_OFFSET) inputs |= dependencies[info.memory.offset.reg];
			addresses |= inputs;
		} else return false;
		dependencies[rd] = inputs;
		written |= 1u << rd;
	}
	if (addresses & written) return false;
	for (unsigned r = 0; r < 16; ++r) if ((written & (1u << r)) && (dependencies[r] & written)) return false;
	block->pollWrites = written;
	return true;
}

/* T5/T6 are caller-saved and carry no C arguments here. Advance the proof
 * epoch before calling out, even when a callback returns an unchanged value.
 * On wrap leave serial at UINT32_MAX and epoch zero; _codeEpoch will clear all
 * cached proofs before reusing epoch 1. No unspilled guest state is touched. */
static bool _emitLoopCallback(struct RV32Block* block) {
	if (block->context) {
		int serial = offsetof(struct RV32Context, epochSerial) - offsetof(struct RV32Context, codeEpoch);
		if (!_emitLoadImmediate(block, RV_T6, (uintptr_t) &block->context->codeEpoch) ||
		    !_emit(block, _rvI(serial, RV_T6, 2, RV_T5, 0x03)) ||
		    !_emitI(block, 1, RV_T5, 0, RV_T5) ||
		    !_emit(block, _rvS(0, RV_T5, RV_T6, 2)) ||
		    !_emit(block, _rvB(8, RV_X0, RV_T5, 0)) ||
		    !_emit(block, _rvS(serial, RV_T5, RV_T6, 2))) return false;
#ifdef MGBA_RV32_TRACE_POLL
		if (!_emit(block, _rvS(offsetof(struct RV32Context, traceAllowed) - offsetof(struct RV32Context, codeEpoch),
		                       RV_X0, RV_T6, 2))) return false;
#endif
	}
	return !block->pollLoop || (_emitLoadImmediate(block, RV_T6, (uintptr_t) &block->loopPure) &&
		_emit(block, _rvS(0, RV_X0, RV_T6, 0)));
}

/* One frame and one register convention cover an entire native block chain. */
static const unsigned _savedRegisters[] = {RV_RA, RV_S0, RV_S1, RV_S2,
	RV_S3, RV_S3 + 1, RV_S3 + 2, RV_S3 + 3, RV_S3 + 4, RV_S3 + 5, RV_S3 + 6, RV_S3 + 7, RV_S11};

static bool _emitEntry(struct RV32Block* block) {
	if (!_emitI(block, -RV32_NATIVE_FRAME_SIZE, RV_SP, 0, RV_SP)) return false;
	for (unsigned i = 0; i < sizeof(_savedRegisters) / sizeof(*_savedRegisters); ++i) {
		if (!_emit(block, _rvS(RV32_NATIVE_FRAME_SIZE - 4 * (i + 1), _savedRegisters[i], RV_SP, 2))) return false;
	}
	return _emitI(block, 0, RV_A0, 0, RV_S0) && _emitResidentState(block, false);
}

static bool _emitReturn(struct RV32Block* block) {
	if (!_emitResidentState(block, true)) return false;
	for (unsigned i = 0; i < sizeof(_savedRegisters) / sizeof(*_savedRegisters); ++i) {
		if (!_emit(block, _rvI(RV32_NATIVE_FRAME_SIZE - 4 * (i + 1), RV_SP, 2, _savedRegisters[i], 0x03))) return false;
	}
	return _emitI(block, RV32_NATIVE_FRAME_SIZE, RV_SP, 0, RV_SP) &&
		_emit(block, _rvI(0, RV_RA, 0, RV_X0, 0x67));
}

/* A cache hit continues at the next block's body without returning through C
 * or allocating another stack frame. Check the same mutable entry state as
 * _findBlock: overwritten/invalid slots are never callable through a link. */
static bool _emitDispatcher(struct RV32Context* context, bool arm) {
	struct RV32Block code = {0};
	code.nativeWords = 0;
	code.registers = NULL;
	code.resident = true;
	EMIT_STAT(&code, dispatch[arm]);
	unsigned exits[17], count = 0;
#define DISPATCH_FAIL_IF(rs2, rs1, condition) do { \
	exits[count++] = code.nativeWords; \
	if (!_emit(&code, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
	if (!_emitLoad(&code, RV_T0, offsetof(struct ARMCore, executionMode)) ||
	    !_emitI(&code, arm ? MODE_ARM : MODE_THUMB, RV_X0, 0, RV_T1)) return false;
	DISPATCH_FAIL_IF(RV_T1, RV_T0, 1);
	if (!_emitLoad(&code, RV_T0, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(&code, RV_T1, offsetof(struct GBA, isPristine)) ||
	    !_emitR(&code, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emit(&code, _rvI(0, RV_T0, 4, RV_T3, 0x03))) return false;
	if (!_emitLoadRegister(&code, RV_T2, ARM_PC) ||
	    !_emitI(&code, arm ? -WORD_SIZE_ARM : -WORD_SIZE_THUMB, RV_T2, 0, RV_T2) ||
	    !_emitShift(&code, 0, 1, RV_T2, 5, RV_T0) ||
	    !_emitShift(&code, 0, 1 + __builtin_ctz(RV32_BLOCK_CACHE_SIZE), RV_T2, 5, RV_T1) ||
	    !_emitR(&code, 0, RV_T1, RV_T0, 4, RV_T0) ||
	    !_emitShift(&code, 0, 1 + 2 * __builtin_ctz(RV32_BLOCK_CACHE_SIZE), RV_T2, 5, RV_T1) ||
	    !_emitR(&code, 0, RV_T1, RV_T0, 4, RV_T0) ||
	    !_emitI(&code, RV32_BLOCK_CACHE_SIZE - 1, RV_T0, 7, RV_T0) ||
	    !_emitLoadImmediate(&code, RV_T1, sizeof(struct RV32Block)) ||
	    !_emitR(&code, 1, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitLoadImmediate(&code, RV_T1, (uintptr_t) context->blocks) ||
	    !_emitR(&code, 0, RV_T1, RV_T0, 0, RV_T0)) return false;
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, writable), RV_T0, 4, RV_T1, 0x03)) ||
	    !_emitR(&code, 0, RV_T3, RV_T1, 6, RV_T1)) return false;
	DISPATCH_FAIL_IF(RV_X0, RV_T1, 0);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, valid), RV_T0, 4, RV_T1, 0x03))) return false;
	DISPATCH_FAIL_IF(RV_X0, RV_T1, 0);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, executable), RV_T0, 4, RV_T1, 0x03))) return false;
	DISPATCH_FAIL_IF(RV_X0, RV_T1, 0);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, arm), RV_T0, 4, RV_T1, 0x03))) return false;
	DISPATCH_FAIL_IF(RV_X0, RV_T1, arm ? 0 : 1);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, start), RV_T0, 2, RV_T1, 0x03))) return false;
	DISPATCH_FAIL_IF(RV_T2, RV_T1, 1);
	const unsigned state[][2] = {
		{offsetof(struct ARMCore, memory.activeRegion), offsetof(struct RV32Block, region)},
		{offsetof(struct ARMCore, memory.activeMask), offsetof(struct RV32Block, mask)},
		{arm ? offsetof(struct ARMCore, memory.activeSeqCycles32) : offsetof(struct ARMCore, memory.activeSeqCycles16), offsetof(struct RV32Block, seqCycles)},
		{offsetof(struct ARMCore, prefetch[0]), arm ? offsetof(struct RV32Block, armOpcodes[0]) : offsetof(struct RV32Block, opcodes[0])},
		{offsetof(struct ARMCore, prefetch[1]), arm ? offsetof(struct RV32Block, armOpcodes[1]) : offsetof(struct RV32Block, opcodes[1])},
	};
	for (unsigned i = 0; i < sizeof(state) / sizeof(*state); ++i) {
		if (!_emitLoad(&code, RV_T1, state[i][0]) ||
		    !_emit(&code, _rvI(state[i][1], RV_T0, arm || i < 3 ? 2 : 5, RV_T2, 0x03))) return false;
		DISPATCH_FAIL_IF(RV_T2, RV_T1, 1);
	}
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, nativeCode) + RV32_BLOCK_BODY_WORD * 4,
	                       RV_T0, 0, RV_X0, 0x67))) return false;
	unsigned end = code.nativeWords;
	EMIT_STAT(&code, dispatchMiss[arm]);
	if (!_emitI(&code, 1, RV_X0, 0, RV_A0) || !_emitReturn(&code) ||
	    code.nativeWords > RV32_DISPATCH_MAX_WORDS) return false;
	for (unsigned i = 0; i < count; ++i) {
		/* Insert only the displacement, preserving each comparison. */
		code.nativeCode[exits[i]] |= _rvB((end - exits[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	}
	memcpy(arm ? context->armDispatchCode : context->dispatchCode, code.nativeCode, code.nativeWords * sizeof(uint32_t));
	__asm__ volatile("fence.i" ::: "memory");
#undef DISPATCH_FAIL_IF
	return true;
}

/* A1 = constant target, A2 = its cache slot, A3 = source fetch waitstate.
 * A miss returns through RA without changing architectural state. A hit
 * performs the complete branch and jumps into the target's lazy pipeline
 * entry, keeping the caller's shared frame and all resident guest registers. */
#ifdef MGBA_RV32_TRACE_POLL
/* A successful backward Thumb edge is an exact logical pipeline boundary.
 * Every callback disables this proof until the next C entry. A RAM store that
 * changes even one byte discards the current snapshot. Thus equal registers
 * and haltPending imply a fixed point of all guest-visible state: branch
 * guards establish the mapping and pipeline, prefetch position is zero, and
 * no other device state can change on these native paths. */
static bool _emitTracePoll(struct RV32Context* context) {
	struct RV32Block code = {0};
	code.resident = true;
	unsigned exits[3], exitCount = 0, changed[20], changedCount = 0;
#define TRACE_BRANCH(list, count, rs2, rs1, condition) do { \
	list[count++] = code.nativeWords; \
	if (!_emit(&code, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
#define TRACE_OFFSET(field) ((int) offsetof(struct RV32Context, field) - (int) offsetof(struct RV32Context, traceAllowed))
	if (!_emitLoadImmediate(&code, RV_T4, (uintptr_t) &context->traceAllowed) ||
	    !_emit(&code, _rvI(0, RV_T4, 2, RV_T0, 0x03))) return false;
	TRACE_BRANCH(exits, exitCount, RV_X0, RV_T0, 0);
	if (!_emitLoad(&code, RV_T5, offsetof(struct ARMCore, nextEvent))) return false;
	TRACE_BRANCH(exits, exitCount, RV_T5, RV_S1, 5);
	if (!_emitLoad(&code, RV_T6, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(&code, RV_T0, offsetof(struct GBA, haltPending)) ||
	    !_emitR(&code, 0, RV_T0, RV_T6, 0, RV_T6) ||
	    !_emit(&code, _rvI(0, RV_T6, 4, RV_T6, 0x03)) ||
	    !_emit(&code, _rvI(TRACE_OFFSET(tracePC), RV_T4, 2, RV_T0, 0x03))) return false;
	TRACE_BRANCH(changed, changedCount, RV_A1, RV_T0, 1);
	if (!_emit(&code, _rvI(TRACE_OFFSET(traceCPSR), RV_T4, 2, RV_T0, 0x03))) return false;
	TRACE_BRANCH(changed, changedCount, RV_S2, RV_T0, 1);
	if (!_emit(&code, _rvI(TRACE_OFFSET(traceHalt), RV_T4, 2, RV_T0, 0x03))) return false;
	TRACE_BRANCH(changed, changedCount, RV_T6, RV_T0, 1);
	for (unsigned r = 0; r < ARM_PC; ++r) {
		unsigned reg = _residentRegister(offsetof(struct ARMCore, gprs[r]));
		if (!reg) {
			reg = RV_T1;
			if (!_emitLoadRegister(&code, reg, r)) return false;
		}
		if (!_emit(&code, _rvI(TRACE_OFFSET(traceGprs[r]), RV_T4, 2, RV_T0, 0x03))) return false;
		TRACE_BRANCH(changed, changedCount, reg, RV_T0, 1);
	}
	if (!_emit(&code, _rvI(TRACE_OFFSET(traceCycles), RV_T4, 2, RV_T0, 0x03))) return false;
	TRACE_BRANCH(changed, changedCount, RV_S1, RV_T0, 5);
	if (!_emitR(&code, 0x20, RV_T0, RV_S1, 0, RV_T1) ||
	    !_emitR(&code, 0x20, RV_S1, RV_T5, 0, RV_T0) ||
	    !_emitR(&code, 1, RV_T1, RV_T0, 5, RV_T0) ||
	    !_emitR(&code, 1, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitR(&code, 0, RV_T0, RV_S1, 0, RV_S1) ||
	    !_emit(&code, _rvS(TRACE_OFFSET(tracePC), RV_X0, RV_T4, 2))) return false;
	TRACE_BRANCH(exits, exitCount, RV_X0, RV_X0, 0);
	for (unsigned i = 0; i < changedCount; ++i)
		code.nativeCode[changed[i]] |= _rvB((code.nativeWords - changed[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emit(&code, _rvS(TRACE_OFFSET(tracePC), RV_A1, RV_T4, 2)) ||
	    !_emit(&code, _rvS(TRACE_OFFSET(traceCycles), RV_S1, RV_T4, 2)) ||
	    !_emit(&code, _rvS(TRACE_OFFSET(traceCPSR), RV_S2, RV_T4, 2)) ||
	    !_emit(&code, _rvS(TRACE_OFFSET(traceHalt), RV_T6, RV_T4, 2))) return false;
	for (unsigned r = 0; r < ARM_PC; ++r) {
		unsigned reg = _residentRegister(offsetof(struct ARMCore, gprs[r]));
		if (!reg) {
			reg = RV_T0;
			if (!_emitLoadRegister(&code, reg, r)) return false;
		}
		if (!_emit(&code, _rvS(TRACE_OFFSET(traceGprs[r]), reg, RV_T4, 2))) return false;
	}
	for (unsigned i = 0; i < exitCount; ++i)
		code.nativeCode[exits[i]] |= _rvB((code.nativeWords - exits[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, nativeCode) + RV32_BLOCK_BODY_WORD * 4,
	                       RV_A2, 0, RV_X0, 0x67)) ||
	    code.nativeWords > sizeof(context->traceCode) / sizeof(*context->traceCode)) return false;
	memcpy(context->traceCode, code.nativeCode, code.nativeWords * 4);
	__asm__ volatile("fence.i" ::: "memory");
#undef TRACE_OFFSET
#undef TRACE_BRANCH
	return true;
}
#endif

static bool _emitBranchDispatcher(struct RV32Context* context, bool arm) {
#ifdef MGBA_RV32_TRACE_POLL
	if (!arm && !_emitTracePoll(context)) return false;
#endif
	struct RV32Block code = {0};
	code.resident = true;
	EMIT_STAT(&code, branch[arm]);
	unsigned exits[18], count = 0;
#define BRANCH_FAIL_IF(rs2, rs1, condition) do { \
	exits[count++] = code.nativeWords; \
	if (!_emit(&code, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
	/* Between callbacks/events the native paths cannot change mappings,
	 * waitstates, ISA, idle policy or ROM code. Reuse the first full proof
	 * for this destination; its tag still guards against cache-slot reuse. */
	unsigned recheck[3];
	if (!_emitLoadImmediate(&code, RV_T0, (uintptr_t) &context->codeEpoch) ||
	    !_emit(&code, _rvI(0, RV_T0, 2, RV_T0, 0x03))) return false;
	recheck[0] = code.nativeWords;
	if (!_emit(&code, _rvB(0, RV_X0, RV_T0, 0)) ||
	    !_emit(&code, _rvI(offsetof(struct RV32Block, branchEpoch), RV_A2, 2, RV_T1, 0x03))) return false;
	recheck[1] = code.nativeWords;
	if (!_emit(&code, _rvB(0, RV_T1, RV_T0, 1)) ||
	    !_emit(&code, _rvI(offsetof(struct RV32Block, start), RV_A2, 2, RV_T0, 0x03))) return false;
	recheck[2] = code.nativeWords;
	if (!_emit(&code, _rvB(0, RV_A1, RV_T0, 1)) ||
	    !_emitLoad(&code, RV_T3, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(&code, RV_T6, offsetof(struct GBA, memory.activeRegion)) ||
	    !_emitR(&code, 0, RV_T3, RV_T6, 0, RV_T6) ||
	    !_emitLoadImmediate(&code, RV_A4, offsetof(struct GBA, idleOptimization)) ||
	    !_emitR(&code, 0, RV_T3, RV_A4, 0, RV_A4) ||
	    !_emit(&code, _rvI(offsetof(struct RV32Block, mask), RV_A2, 2, RV_T5, 0x03))) return false;
	EMIT_STAT(&code, branchProof[arm]);
	unsigned proved = code.nativeWords;
	if (!_emit(&code, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < 3; ++i)
		code.nativeCode[recheck[i]] |= _rvB((code.nativeWords - recheck[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emitLoad(&code, RV_T0, offsetof(struct ARMCore, memory.setActiveRegion)) ||
	    !_emitLoadImmediate(&code, RV_T1, (uintptr_t) GBASetActiveRegion)) return false;
	BRANCH_FAIL_IF(RV_T1, RV_T0, 1);
	if (!_emitLoad(&code, RV_T0, offsetof(struct ARMCore, executionMode)) ||
	    !_emitI(&code, arm ? MODE_ARM : MODE_THUMB, RV_X0, 0, RV_T1)) return false;
	BRANCH_FAIL_IF(RV_T1, RV_T0, 1);
	if (!_emitI(&code, 32, RV_S2, 7, RV_T0)) return false;
	BRANCH_FAIL_IF(RV_X0, RV_T0, arm ? 1 : 0);
	if (!_emitLoad(&code, RV_T3, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(&code, RV_T0, offsetof(struct GBA, isPristine)) ||
	    !_emitR(&code, 0, RV_T3, RV_T0, 0, RV_T0) ||
	    !_emit(&code, _rvI(0, RV_T0, 4, RV_T2, 0x03))) return false;
	if (!_emitLoadImmediate(&code, RV_T6, offsetof(struct GBA, memory.activeRegion)) ||
	    !_emitR(&code, 0, RV_T3, RV_T6, 0, RV_T6) ||
	    !_emit(&code, _rvI(0, RV_T6, 2, RV_T0, 0x03)) ||
	    !_emitShift(&code, 0, BASE_OFFSET, RV_A1, 5, RV_T1)) return false;
	BRANCH_FAIL_IF(RV_T1, RV_T0, 1);
	int romSize = (int) offsetof(struct GBAMemory, romSize) - (int) offsetof(struct GBAMemory, activeRegion);
	if (!_emit(&code, _rvI(romSize, RV_T6, 2, RV_T0, 0x03)) ||
	    !_emitShift(&code, 0, 7, RV_A1, 1, RV_T1) ||
	    !_emitShift(&code, 0, 7, RV_T1, 5, RV_T1)) return false;
	BRANCH_FAIL_IF(RV_T0, RV_T1, 7);
	if (!_emitLoadImmediate(&code, RV_A4, offsetof(struct GBA, idleOptimization)) ||
	    !_emitR(&code, 0, RV_T3, RV_A4, 0, RV_A4) ||
	    !_emit(&code, _rvI(0, RV_A4, 2, RV_T0, 0x03))) return false;
	unsigned ignored = code.nativeWords;
	if (!_emit(&code, _rvB(0, RV_X0, RV_T0, 4))) return false;
	BRANCH_FAIL_IF(RV_X0, RV_T0, 1);
	if (!_emit(&code, _rvI(offsetof(struct GBA, idleLoop) - offsetof(struct GBA, idleOptimization),
	                      RV_A4, 2, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_A1, RV_T0, 0);
	code.nativeCode[ignored] = _rvB((code.nativeWords - ignored) * 4, RV_X0, RV_T0, 4);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, writable), RV_A2, 4, RV_T0, 0x03)) ||
	    !_emitR(&code, 0, RV_T2, RV_T0, 6, RV_T0)) return false;
	BRANCH_FAIL_IF(RV_X0, RV_T0, 0);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, valid), RV_A2, 4, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_X0, RV_T0, 0);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, executable), RV_A2, 4, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_X0, RV_T0, 0);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, arm), RV_A2, 4, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_X0, RV_T0, arm ? 0 : 1);
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, start), RV_A2, 2, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_A1, RV_T0, 1);
	if (!_emitLoad(&code, RV_T4, offsetof(struct ARMCore, memory.activeRegion)) ||
	    !_emit(&code, _rvI(offsetof(struct RV32Block, region), RV_A2, 2, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_T4, RV_T0, 1);
	if (!_emitLoad(&code, RV_T5, offsetof(struct ARMCore, memory.activeMask)) ||
	    !_emitI(&code, arm ? -WORD_SIZE_ARM : WORD_SIZE_THUMB, RV_T5, arm ? 7 : 6, RV_T5) ||
	    !_emit(&code, _rvI(offsetof(struct RV32Block, mask), RV_A2, 2, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_T5, RV_T0, 1);
	if (!_emitLoad(&code, RV_T1, arm ? offsetof(struct ARMCore, memory.activeSeqCycles32) : offsetof(struct ARMCore, memory.activeSeqCycles16)) ||
	    !_emit(&code, _rvI(offsetof(struct RV32Block, seqCycles), RV_A2, 2, RV_T0, 0x03))) return false;
	BRANCH_FAIL_IF(RV_T1, RV_T0, 1);
	BRANCH_FAIL_IF(RV_A3, RV_T1, 1);
	for (unsigned i = 0; i < 2; ++i) {
		if (!_emitI(&code, i * (arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB), RV_A1, 0, RV_T0) ||
		    !_emitR(&code, 0, RV_T5, RV_T0, 7, RV_T0) ||
		    !_emitR(&code, 0, RV_T4, RV_T0, 0, RV_T0) ||
		    !_emit(&code, _rvI(0, RV_T0, arm ? 2 : 5, RV_T0, 0x03)) ||
		    !_emit(&code, _rvI(arm ? offsetof(struct RV32Block, armOpcodes[i]) : offsetof(struct RV32Block, opcodes[i]), RV_A2, arm ? 2 : 5, RV_T1, 0x03))) return false;
		BRANCH_FAIL_IF(RV_T1, RV_T0, 1);
	}
	if (!_emitLoadImmediate(&code, RV_T0, (uintptr_t) &context->codeEpoch) ||
	    !_emit(&code, _rvI(0, RV_T0, 2, RV_T0, 0x03)) ||
	    !_emit(&code, _rvS(offsetof(struct RV32Block, branchEpoch), RV_T0, RV_A2, 2))) return false;
	code.nativeCode[proved] = _rvB((code.nativeWords - proved) * 4, RV_X0, RV_X0, 0);
	/* Commit only after every guard. Refill values have been checked against
	 * the target block; its helper/event exits materialize them when needed. */
	if (!_emit(&code, _rvS(offsetof(struct GBA, lastJump) - offsetof(struct GBA, idleOptimization),
	                      RV_A1, RV_A4, 2)) ||
	    !_emit(&code, _rvS(offsetof(struct GBAMemory, lastPrefetchedPc) - offsetof(struct GBAMemory, activeRegion),
	                      RV_X0, RV_T6, 2)) ||
	    !_emitStore(&code, RV_T5, offsetof(struct ARMCore, memory.activeMask)) ||
	    !_emitLoad(&code, RV_T0, arm ? offsetof(struct ARMCore, memory.activeNonseqCycles32) : offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitShift(&code, 0, 1, RV_A3, 1, RV_T1) ||
	    !_emitR(&code, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitI(&code, 3, RV_T0, 0, RV_T0) ||
	    !_emitR(&code, 0, RV_T0, RV_S1, 0, RV_S1)) return false;
#ifdef MGBA_RV32_TRACE_POLL
	if (!arm) {
		unsigned forward = code.nativeWords;
		if (!_emit(&code, _rvB(0, RV_X0, RV_A6, 0)) ||
		    !_emitLoadImmediate(&code, RV_T0, (uintptr_t) context->traceCode) ||
		    !_emit(&code, _rvI(0, RV_T0, 0, RV_X0, 0x67))) return false;
		code.nativeCode[forward] = _rvB((code.nativeWords - forward) * 4, RV_X0, RV_A6, 0);
	}
#endif
	if (!_emit(&code, _rvI(offsetof(struct RV32Block, nativeCode) + RV32_BLOCK_BODY_WORD * 4,
	                      RV_A2, 0, RV_X0, 0x67))) return false;
	unsigned end = code.nativeWords;
	EMIT_STAT(&code, branchMiss[arm]);
	if (!_emit(&code, _rvI(0, RV_RA, 0, RV_X0, 0x67)) || code.nativeWords > RV32_BRANCH_MAX_WORDS) return false;
	for (unsigned i = 0; i < count; ++i) {
		code.nativeCode[exits[i]] |= _rvB((end - exits[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	}
	memcpy(arm ? context->armBranchCode : context->branchCode, code.nativeCode, code.nativeWords * sizeof(uint32_t));
	__asm__ volatile("fence.i" ::: "memory");
#undef BRANCH_FAIL_IF
	return true;
}

static bool _emitFastBranchCall(struct RV32Block* block, uint32_t target) {
	if (!block->branchDispatch || !block->cache ||
	    (target >> BASE_OFFSET) != (block->start >> BASE_OFFSET) || (target & 1)) return true;
	unsigned probe = 0;
#ifdef MGBA_RV32_TRACE_POLL
	if (!block->arm && !_emitI(block, target <= block->start, RV_X0, 0, RV_A6)) return false;
#endif
	if (block->pollLoop && target == block->start) {
		if (!_emitLoadImmediate(block, RV_T0, (uintptr_t) &block->loopPure) ||
		    !_emit(block, _rvI(0, RV_T0, 4, RV_T0, 0x03))) return false;
		probe = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_T0, 1))) return false;
	}
	bool emitted = _emitLoadImmediate(block, RV_A1, target) &&
		_emitLoadImmediate(block, RV_A2, (uintptr_t) &block->cache[_blockIndex(target)]) &&
		_emitLoadImmediate(block, RV_A3, block->seqCycles) &&
		_emitLoadImmediate(block, RV_T0, (uintptr_t) block->branchDispatch) &&
		_emit(block, _rvI(0, RV_T0, 0, RV_RA, 0x67));
	if (probe) block->nativeCode[probe] = _rvB((block->nativeWords - probe) * 4, RV_X0, RV_T0, 1);
	return emitted;
}

static bool _emitPipeline(struct RV32Block* block, unsigned next) {
	return _emitLoadImmediate(block, RV_T0, _blockOpcode(block, next)) &&
		_emitStore(block, RV_T0, offsetof(struct ARMCore, prefetch[0])) &&
		_emitLoadImmediate(block, RV_T0, _blockOpcode(block, next + 1)) &&
		_emitStore(block, RV_T0, offsetof(struct ARMCore, prefetch[1])) &&
		_emitLoadImmediate(block, RV_T0, block->start + (next + 1) * (block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB)) &&
		_emitStoreRegister(block, RV_T0, ARM_PC);
}

/* Shared native entries start at BODY_WORD, including this validation gate.
 * An invalid target has already committed its branch; restore that target's
 * pipeline before returning so the next lookup can rebuild or interpret it. */
static bool _emitWritableEntry(struct RV32Block* block) {
	if (!block->writable || !block->context) return true;
	if (!_emitLoadImmediate(block, RV_T0, (uintptr_t) &block->context->codeEpoch) ||
	    !_emit(block, _rvI(0, RV_T0, 2, RV_T0, 0x03)) ||
	    !_emitLoadImmediate(block, RV_T1, (uintptr_t) &block->verifiedEpoch) ||
	    !_emit(block, _rvI(0, RV_T1, 2, RV_T1, 0x03))) return false;
	if (!_emit(block, _rvB(8, RV_X0, RV_T0, 0))) return false;
	unsigned cached = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 0)) ||
	    !_emitLoadImmediate(block, RV_A0, (uintptr_t) block) ||
	    !_emitLoadImmediate(block, RV_T0, (uintptr_t) _validateWritableBlock) ||
	    !_emit(block, _rvI(0, RV_T0, 0, RV_RA, 0x67)) ||
	    !_emitI(block, 0, RV_A0, 0, RV_T0) ||
	    !_emitI(block, 0, RV_S0, 0, RV_A0)) return false;
	unsigned valid = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T0, 1)) ||
	    !_emitPipeline(block, 0) || !_emitI(block, 1, RV_X0, 0, RV_A0) ||
	    !_emitReturn(block)) return false;
	block->nativeCode[cached] = _rvB((block->nativeWords - cached) * 4, RV_T1, RV_T0, 0);
	block->nativeCode[valid] = _rvB((block->nativeWords - valid) * 4, RV_X0, RV_T0, 1);
	return true;
}

/* Probe only fields the admitted loop can change. Unwritten GPRs and the
 * Thumb shifter state are invariant without an external callback. Metadata
 * is private to one CPU's cache and never observed by guest instructions. */
static bool _emitPollBegin(struct RV32Block* block) {
	if (!block->pollLoop) return true;
	int pure = (int) offsetof(struct RV32Block, loopPure) - (int) offsetof(struct RV32Block, pollSnapshot);
	if (!_emitLoadImmediate(block, RV_T4, (uintptr_t) &block->pollSnapshot) ||
	    !_emit(block, _rvI(pure, RV_T4, 4, RV_T0, 0x03))) return false;
	unsigned inactive = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T0, 0)) ||
	    !_emit(block, _rvS(offsetof(struct RV32PollSnapshot, cycles), RV_S1, RV_T4, 2)) ||
	    !_emit(block, _rvS(offsetof(struct RV32PollSnapshot, cpsr), RV_S2, RV_T4, 2))) return false;
	for (unsigned r = 0; r < ARM_PC; ++r) {
		if (!(block->pollWrites & (1u << r))) continue;
		unsigned reg = _residentRegister(offsetof(struct ARMCore, gprs[r]));
		if (!reg) {
			reg = RV_T0;
			if (!_emitLoadRegister(block, reg, r)) return false;
		}
		if (!_emit(block, _rvS(offsetof(struct RV32PollSnapshot, gprs[r]), reg, RV_T4, 2))) return false;
	}
	if (block->arm && (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, shifterOperand)) ||
	    !_emit(block, _rvS(offsetof(struct RV32PollSnapshot, shifter), RV_T0, RV_T4, 2)) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, shifterCarryOut)) ||
	    !_emit(block, _rvS(offsetof(struct RV32PollSnapshot, carry), RV_T0, RV_T4, 2)))) return false;
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(block, RV_T1, offsetof(struct GBA, memory.lastPrefetchedPc)) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emit(block, _rvI(0, RV_T0, 2, RV_T0, 0x03)) ||
	    !_emit(block, _rvS(offsetof(struct RV32PollSnapshot, prefetch), RV_T0, RV_T4, 2))) return false;
	block->nativeCode[inactive] = _rvB((block->nativeWords - inactive) * 4, RV_X0, RV_T0, 0);
	return true;
}

/* A complete, pure iteration can advance whole repeats natively. The C
 * entry has already checked monotonic timing. All rejection edges return
 * the actual iteration's state; event/partial-span exits bypass this code. */
static bool _emitPollEnd(struct RV32Block* block) {
	if (!block->pollLoop) return true;
	unsigned begin = block->nativeWords, exits[32], count = 0;
	int pure = (int) offsetof(struct RV32Block, loopPure) - (int) offsetof(struct RV32Block, pollSnapshot);
#define POLL_FAIL(rs2, rs1, condition) do { \
	exits[count++] = block->nativeWords; \
	if (!_emit(block, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
	if (!_emitLoadImmediate(block, RV_T4, (uintptr_t) &block->pollSnapshot) ||
	    !_emit(block, _rvI(pure, RV_T4, 4, RV_T0, 0x03))) return false;
	unsigned inactive = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T0, 0)) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, nextEvent))) return false;
	POLL_FAIL(RV_T0, RV_S1, 5);
	const uint32_t fixed[][2] = {
		{offsetof(struct ARMCore, gprs[ARM_PC]), block->start + (block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB)},
		{offsetof(struct ARMCore, memory.activeMask), block->mask},
		{offsetof(struct ARMCore, prefetch[0]), _blockOpcode(block, 0)},
		{offsetof(struct ARMCore, prefetch[1]), _blockOpcode(block, 1)},
	};
	for (unsigned i = 0; i < sizeof(fixed) / sizeof(*fixed); ++i) {
		if (!_emitLoad(block, RV_T0, fixed[i][0]) || !_emitLoadImmediate(block, RV_T1, fixed[i][1])) return false;
		POLL_FAIL(RV_T1, RV_T0, 1);
	}
	if (!_emit(block, _rvI(offsetof(struct RV32PollSnapshot, cpsr), RV_T4, 2, RV_T0, 0x03))) return false;
	POLL_FAIL(RV_T0, RV_S2, 1);
	for (unsigned r = 0; r < ARM_PC; ++r) {
		if (!(block->pollWrites & (1u << r))) continue;
		unsigned reg = _residentRegister(offsetof(struct ARMCore, gprs[r]));
		if (!reg) {
			reg = RV_T1;
			if (!_emitLoadRegister(block, reg, r)) return false;
		}
		if (!_emit(block, _rvI(offsetof(struct RV32PollSnapshot, gprs[r]), RV_T4, 2, RV_T0, 0x03))) return false;
		POLL_FAIL(RV_T0, reg, 1);
	}
	if (block->arm) {
		if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, shifterOperand)) ||
		    !_emit(block, _rvI(offsetof(struct RV32PollSnapshot, shifter), RV_T4, 2, RV_T1, 0x03))) return false;
		POLL_FAIL(RV_T1, RV_T0, 1);
		if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, shifterCarryOut)) ||
		    !_emit(block, _rvI(offsetof(struct RV32PollSnapshot, carry), RV_T4, 2, RV_T1, 0x03))) return false;
		POLL_FAIL(RV_T1, RV_T0, 1);
	}
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(block, RV_T1, offsetof(struct GBA, memory.lastPrefetchedPc)) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emit(block, _rvI(0, RV_T0, 2, RV_T0, 0x03)) ||
	    !_emit(block, _rvI(offsetof(struct RV32PollSnapshot, prefetch), RV_T4, 2, RV_T1, 0x03))) return false;
	POLL_FAIL(RV_T1, RV_T0, 1);
	if (!_emit(block, _rvI(offsetof(struct RV32PollSnapshot, cycles), RV_T4, 2, RV_T0, 0x03))) return false;
	POLL_FAIL(RV_S1, RV_T0, 5); /* Signed elapsed must be positive. */
	if (!_emitR(block, 0x20, RV_T0, RV_S1, 0, RV_T1) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, nextEvent)) ||
	    !_emitR(block, 0x20, RV_S1, RV_T0, 0, RV_T0) ||
	    !_emitR(block, 1, RV_T1, RV_T0, 5, RV_T0) ||
	    !_emitR(block, 1, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitR(block, 0, RV_T0, RV_S1, 0, RV_S1) ||
	    !_emit(block, _rvS(pure, RV_X0, RV_T4, 0))) return false;
	unsigned finish = block->nativeWords;
	if (!_emitI(block, 1, RV_X0, 0, RV_A0) || !_emitReturn(block) ||
	    block->nativeWords - begin > RV32_POLL_END_MAX_WORDS) return false;
	for (unsigned i = 0; i < count; ++i)
		block->nativeCode[exits[i]] |= _rvB((finish - exits[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	block->nativeCode[inactive] = _rvB((block->nativeWords - inactive) * 4, RV_X0, RV_T0, 0);
#undef POLL_FAIL
	return true;
}

/* A device callback may have changed code or the pipeline. Re-enter through
 * the cache rather than continuing with decoded future instructions. */
static bool _emitCallbackEnd(struct RV32Block* block) {
	if (!block->context) return true;
	if (block->dispatch) return _emitLoadImmediate(block, RV_T0, (uintptr_t) block->dispatch) &&
		_emit(block, _rvI(0, RV_T0, 0, RV_X0, 0x67));
	return _emitI(block, 1, RV_X0, 0, RV_A0) && _emitReturn(block);
}

#ifdef MGBA_RV32_VERIFY
static struct ARMCore _expected;
static void _verifyBefore(struct ARMCore* cpu, const struct RV32Block* block, unsigned start) {
	_expected = *cpu;
	for (unsigned i = start; i < start + block->spans[start].length; ++i) {
		_expected.prefetch[0] = block->opcodes[i + 1];
		_expected.prefetch[1] = block->opcodes[i + 2];
		_expected.gprs[ARM_PC] += WORD_SIZE_THUMB;
		_thumbTable[block->opcodes[i] >> 6](&_expected, block->opcodes[i]);
	}
}
static void _verifyAfter(struct ARMCore* cpu) {
	if (memcmp(cpu, &_expected, sizeof(*cpu))) {
		fprintf(stderr, "RV32 block mismatch at %08lx\n", (unsigned long) cpu->gprs[ARM_PC]);
		abort();
	}
}
#endif

static bool _emitCall(struct RV32Block* block, uintptr_t target) {
	return _emitLoopCallback(block) && _emitResidentState(block, true) && _emitLoadImmediate(block, RV_T0, target) &&
		_emit(block, _rvI(0, RV_T0, 0, RV_RA, 0x67)) &&
		_emitResidentState(block, false) && _emitI(block, 0, RV_S0, 0, RV_A0);
}

/* Same-ROM branches need only three state updates when idle-loop probing is
 * inactive. Guard the callback identity and mutable mapping/idle state; all
 * special cases call the original function with fully synchronized registers. */
static bool _emitSetRegion(struct RV32Block* block, uint32_t target) {
	unsigned rejects[5], count = 0, complete = 0;
	unsigned region = target >> BASE_OFFSET;
	if (region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX) {
#define REGION_REJECT(rs2, rs1, condition) do { \
	rejects[count++] = block->nativeWords; \
	if (!_emit(block, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
		if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.setActiveRegion)) ||
		    !_emitLoadImmediate(block, RV_T1, (uintptr_t) GBASetActiveRegion)) return false;
		REGION_REJECT(RV_T1, RV_T0, 1);
		if (!_emitLoad(block, RV_A2, offsetof(struct ARMCore, master)) ||
		    !_emitLoadImmediate(block, RV_A3, offsetof(struct GBA, memory.activeRegion)) ||
		    !_emitR(block, 0, RV_A2, RV_A3, 0, RV_A3) ||
		    !_emit(block, _rvI(0, RV_A3, 2, RV_T0, 0x03)) ||
		    !_emitI(block, region, RV_X0, 0, RV_T1)) return false;
		REGION_REJECT(RV_T1, RV_T0, 1);
		int romSize = (int) offsetof(struct GBAMemory, romSize) - (int) offsetof(struct GBAMemory, activeRegion);
		if (!_emit(block, _rvI(romSize, RV_A3, 2, RV_T0, 0x03)) ||
		    !_emitLoadImmediate(block, RV_T1, target & (GBA_SIZE_ROM0 - 1))) return false;
		REGION_REJECT(RV_T0, RV_T1, 7);
		if (!_emitLoadImmediate(block, RV_A4, offsetof(struct GBA, idleOptimization)) ||
		    !_emitR(block, 0, RV_A2, RV_A4, 0, RV_A4) ||
		    !_emit(block, _rvI(0, RV_A4, 2, RV_T0, 0x03))) return false;
		unsigned ignored = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_T0, 4))) return false;
		REGION_REJECT(RV_X0, RV_T0, 1);
		if (!_emit(block, _rvI(offsetof(struct GBA, idleLoop) - offsetof(struct GBA, idleOptimization),
		                       RV_A4, 2, RV_T0, 0x03))) return false;
		REGION_REJECT(RV_A1, RV_T0, 0);
		block->nativeCode[ignored] = _rvB((block->nativeWords - ignored) * 4, RV_X0, RV_T0, 4);
		if (!_emit(block, _rvS(offsetof(struct GBA, lastJump) - offsetof(struct GBA, idleOptimization),
		                       RV_A1, RV_A4, 2)) ||
		    !_emit(block, _rvS(offsetof(struct GBAMemory, lastPrefetchedPc) - offsetof(struct GBAMemory, activeRegion),
		                       RV_X0, RV_A3, 2)) ||
		    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeMask)) ||
		    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, cpsr.packed)) ||
		    !_emitI(block, 32, RV_T1, 7, RV_T1) ||
		    !_emit(block, _rvB(12, RV_X0, RV_T1, 0)) ||
		    !_emitI(block, WORD_SIZE_THUMB, RV_T0, 6, RV_T0) ||
		    !_emit(block, _rvB(8, RV_X0, RV_X0, 0)) ||
		    !_emitI(block, -WORD_SIZE_ARM, RV_T0, 7, RV_T0) ||
		    !_emitStore(block, RV_T0, offsetof(struct ARMCore, memory.activeMask))) return false;
		complete = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
		for (unsigned i = 0; i < count; ++i) {
			block->nativeCode[rejects[i]] |= _rvB((block->nativeWords - rejects[i]) * 4, 0, 0, 0) & 0xFE000F80u;
		}
#undef REGION_REJECT
	}
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.setActiveRegion)) ||
	    !_emitLoopCallback(block) ||
	    !_emitResidentState(block, true) ||
	    !_emit(block, _rvI(0, RV_T0, 0, RV_RA, 0x67)) ||
	    !_emitResidentState(block, false) ||
	    !_emitI(block, 0, RV_S0, 0, RV_A0)) return false;
	if (complete) block->nativeCode[complete] = _rvB((block->nativeWords - complete) * 4, RV_X0, RV_X0, 0);
	return true;
}

/* Preserve ThumbWritePC bookkeeping and every region callback side effect. */
static bool _emitTakenBranch(struct RV32Block* block, uint32_t target) {
	if (!_emitLoad(block, RV_T0, block->arm ? offsetof(struct ARMCore, memory.activeSeqCycles32) : offsetof(struct ARMCore, memory.activeSeqCycles16)) ||
	    !_emitI(block, 1, RV_T0, 0, RV_T0) ||
	    !_emit(block, _rvS(0, RV_T0, RV_SP, 2)) ||
	    !_emitLoadImmediate(block, RV_A1, target) ||
	    !_emitStoreRegister(block, RV_A1, ARM_PC)) return false;
	if ((target & 1) && !_emitI(block, -2, RV_A1, 7, RV_A1)) return false;
	if (!_emitSetRegion(block, target & ~1u) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeRegion)) ||
	    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, memory.activeMask)) ||
	    !_emitLoadImmediate(block, RV_A1, target & ~1u)) return false;
	for (unsigned i = 0; i < 2; ++i) {
		if (i && !_emitI(block, block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB, RV_A1, 0, RV_A1)) return false;
		if (!_emitR(block, 0, RV_T1, RV_A1, 7, RV_A2) ||
		    !_emitR(block, 0, RV_T0, RV_A2, 0, RV_A2) ||
		    !_emit(block, _rvI(0, RV_A2, block->arm ? 2 : 5, RV_A3, 0x03)) ||
		    !_emitStore(block, RV_A3, offsetof(struct ARMCore, prefetch[i]))) return false;
	}
	return _emitStoreRegister(block, RV_A1, ARM_PC) &&
		_emit(block, _rvI(0, RV_SP, 2, RV_T0, 0x03)) &&
		_emitLoad(block, RV_T1, block->arm ? offsetof(struct ARMCore, memory.activeNonseqCycles32) : offsetof(struct ARMCore, memory.activeNonseqCycles16)) &&
		_emitLoad(block, RV_T2, block->arm ? offsetof(struct ARMCore, memory.activeSeqCycles32) : offsetof(struct ARMCore, memory.activeSeqCycles16)) &&
		_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) &&
		_emitR(block, 0, RV_T2, RV_T0, 0, RV_T0) &&
		_emitI(block, 2, RV_T0, 0, RV_T0) &&
		_emitLoad(block, RV_T1, offsetof(struct ARMCore, cycles)) &&
		_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) &&
		_emitStore(block, RV_T0, offsetof(struct ARMCore, cycles));
}

/* Materialize the condition once; the taken path preserves ThumbWritePC's
 * region changes, idle detection, pipeline refill and bus state. */
static bool _emitBranch(struct RV32Block* block, unsigned index) {
	uint16_t opcode = block->opcodes[index];
	bool conditional = (opcode & 0xF000) == 0xD000 && (opcode & 0x0F00) < 0x0E00;
	bool direct = (opcode & 0xF800) == 0xE000;
	if (!conditional && !direct) return false;
	int32_t displacement = conditional ? (int8_t) opcode * 2 : (int16_t) (opcode << 5) / 16;
	uint32_t target = block->start + (index + 2) * WORD_SIZE_THUMB + displacement;
	unsigned condition = (opcode >> 8) & 15;
	unsigned skip = 0;
	if (conditional) {
		if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, cpsr.packed))) return false;
		if (condition < 8) {
			static const uint8_t bits[] = {30, 29, 31, 28};
			if (!_emitShift(block, 0, bits[condition >> 1], RV_T0, 5, RV_T0) ||
			    !_emitI(block, 1, RV_T0, 7, RV_T0)) return false;
		} else if (condition < 10) {
			if (!_emitShift(block, 0, 29, RV_T0, 5, RV_T0) ||
			    !_emitI(block, 3, RV_T0, 7, RV_T0) ||
			    !_emitI(block, 1, RV_T0, 4, RV_T0) ||
			    !_emitI(block, 1, RV_T0, 3, RV_T0)) return false;
		} else {
			if (!_emitShift(block, 0, 31, RV_T0, 5, RV_T1) ||
			    !_emitShift(block, 0, 28, RV_T0, 5, RV_T2) ||
			    !_emitR(block, 0, RV_T2, RV_T1, 4, RV_T1) ||
			    !_emitI(block, 1, RV_T1, 7, RV_T1)) return false;
			if (condition >= 12 && (!_emitShift(block, 0, 30, RV_T0, 5, RV_T0) ||
			    !_emitI(block, 1, RV_T0, 7, RV_T0) ||
			    !_emitR(block, 0, RV_T0, RV_T1, 6, RV_T1))) return false;
			if (!_emitI(block, 1, RV_T1, 3, RV_T0)) return false;
		}
		skip = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_T0, condition & 1))) return false;
	}
	if (!_emitFastBranchCall(block, target) || !_emitPipeline(block, index + 1) ||
	    !_emitTakenBranch(block, target)) return false;
	if (conditional) {
		unsigned end = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
		block->nativeCode[skip] = _rvB((block->nativeWords - skip) * 4, RV_X0, RV_T0, condition & 1);
		if (!_emitPipeline(block, index + 1) ||
		    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeSeqCycles16)) ||
		    !_emitI(block, 1, RV_T0, 0, RV_T0) ||
		    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, cycles)) ||
		    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
		    !_emitStore(block, RV_T0, offsetof(struct ARMCore, cycles))) return false;
		block->nativeCode[end] = _rvB((block->nativeWords - end) * 4, RV_X0, RV_X0, 0);
	}
	return true;
}

/* Only GBAIORead's unconditional backing-register cases qualify. Bit 0
 * permits a direct read; bit 1 reproduces its idle-removal cancellation.
 * Timers, input callbacks, wave RAM, open bus and conditional audio reads
 * deliberately retain the original callback. A word needs both halves. */
static const uint8_t _ioReadFlags[GBA_SIZE_IO / 2] = {
	[GBA_REG(DISPCNT)] = 3, [GBA_REG(STEREOCNT)] = 3,
	[GBA_REG(DISPSTAT)] = 3, [GBA_REG(VCOUNT)] = 3,
	[GBA_REG(BG0CNT)] = 1, [GBA_REG(BG1CNT)] = 1,
	[GBA_REG(BG2CNT)] = 1, [GBA_REG(BG3CNT)] = 1,
	[GBA_REG(WININ)] = 1, [GBA_REG(WINOUT)] = 1,
	[GBA_REG(BLDCNT)] = 1, [GBA_REG(BLDALPHA)] = 1,
	[GBA_REG(SOUNDCNT_HI)] = 1, [GBA_REG(SOUNDCNT_X)] = 3, [GBA_REG(SOUNDBIAS)] = 3,
	[GBA_REG(DMA0CNT_HI)] = 3, [GBA_REG(DMA1CNT_HI)] = 3,
	[GBA_REG(DMA2CNT_HI)] = 3, [GBA_REG(DMA3CNT_HI)] = 3,
	[GBA_REG(TM0CNT_HI)] = 1, [GBA_REG(TM1CNT_HI)] = 1,
	[GBA_REG(TM2CNT_HI)] = 1, [GBA_REG(TM3CNT_HI)] = 1,
	[GBA_REG(KEYCNT)] = 1,
	[GBA_REG(SIOMULTI0)] = 3, [GBA_REG(SIOMULTI1)] = 3,
	[GBA_REG(SIOMULTI2)] = 3, [GBA_REG(SIOMULTI3)] = 3,
	[GBA_REG(SIOMLT_SEND)] = 3, [GBA_REG(JOYCNT)] = 3,
	[GBA_REG(JOY_TRANS_LO)] = 3, [GBA_REG(JOY_TRANS_HI)] = 3, [GBA_REG(JOYSTAT)] = 3,
	[GBA_REG(IE)] = 1, [GBA_REG(IF)] = 3, [GBA_REG(WAITCNT)] = 3,
	[GBA_REG(IME)] = 3, [GBA_REG(POSTFLG)] = 3,
};

/* A3 = GBAMemory, A1 = guest address. Produce the exact byte/halfword/word
 * backing address in T0; all rejection branches precede haltPending changes.
 * GBALoad8 intentionally uses a 16-bit IO offset, unlike Load16/Load32. */
static bool _emitIOReadAddress(struct RV32Block* block, unsigned width, unsigned* rejects, unsigned* count) {
#define IO_REJECT(rs2, rs1, condition) do { \
	rejects[(*count)++] = block->nativeWords; \
	if (!_emit(block, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
	if (!_emitLoadImmediate(block, RV_T1, width == 1 ? 0xFFFF : OFFSET_MASK & ~(width - 1)) ||
	    !_emitR(block, 0, RV_T1, RV_A1, 7, RV_T1) ||
	    !_emitI(block, GBA_SIZE_IO, RV_X0, 0, RV_T2)) return false;
	IO_REJECT(RV_T2, RV_T1, 7);
	if (!_emitShift(block, 0, 1, RV_T1, 5, RV_T2) ||
	    !_emitLoadImmediate(block, RV_T4, (uintptr_t) _ioReadFlags) ||
	    !_emitR(block, 0, RV_T2, RV_T4, 0, RV_T4) ||
	    !_emit(block, _rvI(0, RV_T4, 4, RV_T2, 0x03))) return false;
	IO_REJECT(RV_X0, RV_T2, 0);
	if (width == 4) {
		if (!_emit(block, _rvI(1, RV_T4, 4, RV_T4, 0x03))) return false;
		IO_REJECT(RV_X0, RV_T4, 0);
		if (!_emitR(block, 0, RV_T4, RV_T2, 6, RV_T2)) return false;
	}
	if (!_emitI(block, 2, RV_T2, 7, RV_T2)) return false;
	unsigned constant = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T2, 0)) ||
	    !_emitLoadImmediate(block, RV_T2, offsetof(struct GBA, haltPending) - offsetof(struct GBA, memory)) ||
	    !_emitR(block, 0, RV_T2, RV_A3, 0, RV_T2) ||
	    !_emit(block, _rvS(0, RV_X0, RV_T2, 0))) return false;
	block->nativeCode[constant] = _rvB((block->nativeWords - constant) * 4, RV_X0, RV_T2, 0);
	return _emitI(block, offsetof(struct GBAMemory, io), RV_A3, 0, RV_T0) &&
		_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) && _emitI(block, 2, RV_X0, 0, RV_A4);
#undef IO_REJECT
}

/* A0 remains the CPU, A1 the guest address. All rejects precede side effects
 * and continue into the original callback. RAM accesses reproduce GBAMemoryStall,
 * including the finite prefetch queue and the signed waitstate adjustment.
 * Backing-register IO reads share that timing path; IO stores never enter it. */
#ifdef MGBA_RV32_TRACE_POLL
/* T0 is the aligned backing RAM address and T3 the new value. Stores remain
 * real stores; only a byte-for-byte no-op can preserve a fixed-point proof. */
static bool _emitTraceStoreGuard(struct RV32Block* block, unsigned width) {
	if (!block->context) return true;
	unsigned value = RV_T3;
	if (width < 4) {
		value = RV_T2;
		if (!_emitShift(block, 0, 32 - width * 8, RV_T3, 1, value) ||
		    !_emitShift(block, 0, 32 - width * 8, value, 5, value)) return false;
	}
	if (!_emit(block, _rvI(0, RV_T0, width == 4 ? 2 : width == 2 ? 5 : 4, RV_T1, 0x03))) return false;
	unsigned unchanged = block->nativeWords;
	if (!_emit(block, _rvB(0, value, RV_T1, 0)) ||
	    !_emitLoadImmediate(block, RV_T1, (uintptr_t) &block->context->tracePC) ||
	    !_emit(block, _rvS(0, RV_X0, RV_T1, 2))) return false;
	block->nativeCode[unchanged] = _rvB((block->nativeWords - unchanged) * 4, value, RV_T1, 0);
	return true;
}
#else
#define _emitTraceStoreGuard(block, width) true
#endif

static bool _emitInlineDataAccess(struct RV32Block* block, unsigned width, unsigned sign,
                         unsigned rd, unsigned callback, uintptr_t expected, bool store,
                         bool shared, unsigned* complete) {
	unsigned rejects[5], rejectCount = 2, noPrefetch[2];
	unsigned accessCycles = store ? 1 : 2;
	if (!_emitLoad(block, RV_T0, callback) ||
	    !_emitLoadImmediate(block, RV_T1, expected)) return false;
	rejects[0] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 1)) ||
	    !_emitShift(block, 0, BASE_OFFSET, RV_A1, 5, RV_T0) ||
	    !_emitI(block, -GBA_REGION_EWRAM, RV_T0, 0, RV_T0) ||
	    !_emitI(block, store ? 2 : 3, RV_T0, 3, RV_T1)) return false;
	rejects[1] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T1, 0)) ||
	    !_emitLoad(block, RV_A3, offsetof(struct ARMCore, master)) ||
	    !_emitI(block, offsetof(struct GBA, memory), RV_A3, 0, RV_A3) ||
	    !_emitLoadImmediate(block, RV_A2, offsetof(struct GBAMemory, waitstatesNonseq16)) ||
	    !_emitR(block, 0, RV_A3, RV_A2, 0, RV_A2)) return false;
	unsigned io = 0;
	if (!store) {
		if (!_emitI(block, GBA_REGION_IO - GBA_REGION_EWRAM, RV_X0, 0, RV_T1)) return false;
		io = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_T1, RV_T0, 0))) return false;
	}
	unsigned iwram = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T0, 1)) ||
	    !_emit(block, _rvI(offsetof(struct GBAMemory, wram), RV_A3, 2, RV_T0, 0x03)) ||
	    !_emitLoadImmediate(block, RV_T1, GBA_SIZE_EWRAM - width)) return false;
	int waitOffset = width == 4 ? (int) offsetof(struct GBAMemory, waitstatesNonseq32) -
		(int) offsetof(struct GBAMemory, waitstatesNonseq16) : 0;
	if (!_emit(block, _rvI(waitOffset + GBA_REGION_EWRAM, RV_A2,
	                       CHAR_MIN < 0 ? 0 : 4, RV_A4, 0x03)) ||
	    !_emitI(block, accessCycles, RV_A4, 0, RV_A4)) return false;
	unsigned ramReady = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	block->nativeCode[iwram] |= _rvB((block->nativeWords - iwram) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emit(block, _rvI(offsetof(struct GBAMemory, iwram), RV_A3, 2, RV_T0, 0x03)) ||
	    !_emitLoadImmediate(block, RV_T1, GBA_SIZE_IWRAM - width) ||
	    !_emitI(block, accessCycles, RV_X0, 0, RV_A4)) return false;
	block->nativeCode[ramReady] |= _rvB((block->nativeWords - ramReady) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emitR(block, 0, RV_T1, RV_A1, 7, RV_T1) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0)) return false;
	if (!store) {
		unsigned ready = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
		block->nativeCode[io] = _rvB((block->nativeWords - io) * 4, RV_T1, RV_T0, 0);
		if (!_emitIOReadAddress(block, width, rejects, &rejectCount)) return false;
		block->nativeCode[ready] = _rvB((block->nativeWords - ready) * 4, RV_X0, RV_X0, 0);
	}
	if (store) {
		if ((!shared && !_emitLoadRegister(block, RV_T3, rd)) ||
		    !_emitTraceStoreGuard(block, width) ||
		    !_emit(block, _rvS(0, RV_T3, RV_T0, width == 4 ? 2 : width == 2 ? 1 : 0))) return false;
	} else if (!_emit(block, _rvI(0, RV_T0, width == 4 ? 2 : width == 2 ? 5 : 4, RV_T3, 0x03))) return false;
	if (!store && width > 1) {
		if (!_emitI(block, width - 1, RV_A1, 7, RV_T0)) return false;
		unsigned aligned = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_X0, RV_T0, 0)) ||
		    !_emitShift(block, 0, 3, RV_T0, 1, RV_T0) ||
		    !_emitR(block, 0x20, RV_T0, RV_X0, 0, RV_T1) ||
		    !_emitR(block, 0, RV_T0, RV_T3, 5, RV_T2) ||
		    !_emitR(block, 0, RV_T1, RV_T3, 1, RV_T3) ||
		    !_emitR(block, 0, RV_T2, RV_T3, 6, RV_T3)) return false;
		block->nativeCode[aligned] = _rvB((block->nativeWords - aligned) * 4, RV_X0, RV_T0, 0);
	}
	if (sign == 16) {
		if (!_emitI(block, 1, RV_A1, 7, RV_T0) ||
		    !_emitShift(block, 0, 3, RV_T0, 1, RV_T0) ||
		    !_emitI(block, 16, RV_T0, 0, RV_T0) ||
		    !_emitR(block, 0, RV_T0, RV_T3, 1, RV_T3) ||
		    !_emitR(block, 0x20, RV_T0, RV_T3, 5, RV_T3)) return false;
	} else if (sign && (!_emitShift(block, 0, sign, RV_T3, 1, RV_T3) ||
	                    !_emitShift(block, 0x20, sign, RV_T3, 5, RV_T3))) return false;
#define RAM_STATE_OFFSET(field) ((int) offsetof(struct GBAMemory, field) - (int) offsetof(struct GBAMemory, waitstatesNonseq16))
	_Static_assert(RAM_STATE_OFFSET(waitstatesNonseq32) >= -2048 &&
	               RAM_STATE_OFFSET(lastPrefetchedPc) <= 2047,
	               "RAM waitstate fields must fit native load/store offsets");
	if (!_emit(block, _rvI(RAM_STATE_OFFSET(prefetch), RV_A2, 4, RV_T0, 0x03))) return false;
	noPrefetch[0] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T0, 0)) ||
	    !_emit(block, _rvI(RAM_STATE_OFFSET(activeRegion), RV_A2, 2, RV_T0, 0x03)) ||
	    !_emitI(block, GBA_REGION_ROM0, RV_X0, 0, RV_T1)) return false;
	noPrefetch[1] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 4)) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeSeqCycles16)) ||
	    !_emitI(block, 1, RV_T0, 0, RV_A5) ||
	    !(shared ? _emitI(block, 0, RV_A7, 0, RV_A3) : _emitLoadRegister(block, RV_A3, ARM_PC)) ||
	    !_emit(block, _rvI(RAM_STATE_OFFSET(lastPrefetchedPc), RV_A2, 2, RV_T1, 0x03)) ||
	    !_emitR(block, 0x20, RV_A3, RV_T1, 0, RV_T1)) return false;
	/* If the first sequential fetch already covers the access, there is
	 * no queue-filling loop and the total reduces exactly to S+1. Keep
	 * the finite queue's previous-load position, including odd distances. */
	unsigned fillQueue = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_A4, RV_A5, 4)) ||
	    !_emitI(block, 16, RV_X0, 0, RV_T2) ||
	    !_emit(block, _rvB(12, RV_T2, RV_T1, 7)) ||
	    !_emitI(block, -2, RV_T1, 7, RV_T1) ||
	    !_emitR(block, 0, RV_T1, RV_A3, 0, RV_A3) ||
	    !_emit(block, _rvS(RAM_STATE_OFFSET(lastPrefetchedPc), RV_A3, RV_A2, 2)) ||
	    !_emitI(block, 0, RV_T0, 0, RV_A4)) return false;
	unsigned covered = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	block->nativeCode[fillQueue] = _rvB((block->nativeWords - fillQueue) * 4, RV_A4, RV_A5, 4);
	if (!_emitI(block, 8, RV_X0, 0, RV_A6) ||
	    !_emitI(block, 16, RV_X0, 0, RV_T2)) return false;
	unsigned distant = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T2, RV_T1, 7)) ||
	    !_emitShift(block, 0, 1, RV_T1, 5, RV_T2) ||
	    !_emitR(block, 0x20, RV_T2, RV_A6, 0, RV_A6) ||
	    !_emitI(block, -2, RV_T1, 7, RV_T1) ||
	    !_emitR(block, 0, RV_T1, RV_A3, 0, RV_A3)) return false;
	block->nativeCode[distant] |= _rvB((block->nativeWords - distant) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emitI(block, 1, RV_X0, 0, RV_T2)) return false;
	unsigned loop = block->nativeWords;
	if (!_emit(block, _rvB(28, RV_A4, RV_A5, 5)) || /* stall >= wait */
	    !_emit(block, _rvB(24, RV_A6, RV_T2, 5)) || /* maxLoads <= 1 */
	    !_emitR(block, 0, RV_T0, RV_A5, 0, RV_A5) ||
	    !_emitI(block, -1, RV_A6, 0, RV_A6) ||
	    !_emitI(block, 2, RV_A3, 0, RV_A3) ||
	    !_emit(block, _rvB(((int) loop - block->nativeWords) * 4, RV_X0, RV_X0, 0))) return false;
	/* Both loop exits go to this store. */
	block->nativeCode[loop] = _rvB((block->nativeWords - loop) * 4, RV_A4, RV_A5, 5);
	block->nativeCode[loop + 1] = _rvB((block->nativeWords - loop - 1) * 4, RV_A6, RV_T2, 5);
	/* Fold Thumb's S+1 and N-S around the stall calculation. With prefetch,
	 * the total is 1+S+max(wait-stall, 0); without it, 1+N+wait. */
	if (!_emit(block, _rvS(RAM_STATE_OFFSET(lastPrefetchedPc), RV_A3, RV_A2, 2)) ||
	    !_emitR(block, 0x20, RV_A5, RV_A4, 0, RV_A4) ||
	    !_emit(block, _rvB(8, RV_X0, RV_A4, 5)) ||
	    !_emitI(block, 0, RV_X0, 0, RV_A4) ||
	    !_emitR(block, 0, RV_T0, RV_A4, 0, RV_A4)) return false;
	unsigned counted = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < 2; ++i) {
		block->nativeCode[noPrefetch[i]] |= _rvB((block->nativeWords - noPrefetch[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	}
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitR(block, 0, RV_T0, RV_A4, 0, RV_A4)) return false;
	block->nativeCode[counted] |= _rvB((block->nativeWords - counted) * 4, 0, 0, 0) & 0xFE000F80u;
	block->nativeCode[covered] = _rvB((block->nativeWords - covered) * 4, RV_X0, RV_X0, 0);
	if (block->arm && (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeNonseqCycles32)) ||
	    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitR(block, 0x20, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitR(block, 0, RV_T0, RV_A4, 0, RV_A4))) return false;
	if (!_emitI(block, 1, RV_A4, 0, RV_A4) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, cycles)) ||
	    !_emitR(block, 0, RV_T0, RV_A4, 0, RV_A4) ||
	    !_emitStore(block, RV_A4, offsetof(struct ARMCore, cycles)) ||
	    (!store && !shared && !_emitStoreRegister(block, RV_T3, rd))) return false;
	*complete = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < rejectCount; ++i) {
		block->nativeCode[rejects[i]] |= _rvB((block->nativeWords - rejects[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	}
#undef RAM_STATE_OFFSET
	return true;
}

static bool _emitIWRAMAccess(struct RV32Block* block, unsigned width, unsigned sign,
                            unsigned callback, uintptr_t expected, bool store, unsigned* complete);

/* Leaf RV32 routines keep the complete guest register convention live.
 * A1 is the address, A7 the guest pipeline PC, T3 the store value/load result.
 * Success returns to RA;
 * failure skips the caller's result write and completion jump (8/4 bytes).
 * They call no C code, allocate no frame and change state only on success.
 * Sharing the sizeable timing/IO guards lets whole memory-heavy blocks fit
 * in the code cache instead of returning through the dispatcher mid-block. */
static bool _emitDataHelpers(struct RV32Context* context) {
	for (unsigned i = 0; i < RV32_DATA_HELPERS; ++i) {
		unsigned kind = i & 7;
		bool store = kind >= 5;
		unsigned width = kind == 0 || kind == 3 || kind == 5 ? 1 :
			kind == 1 || kind == 4 || kind == 6 ? 2 : 4;
		unsigned sign = kind == 3 ? 24 : kind == 4 ? 16 : 0;
		unsigned callback = store ? (width == 4 ? offsetof(struct ARMCore, memory.store32) :
			width == 2 ? offsetof(struct ARMCore, memory.store16) : offsetof(struct ARMCore, memory.store8)) :
			(width == 4 ? offsetof(struct ARMCore, memory.load32) :
			width == 2 ? offsetof(struct ARMCore, memory.load16) : offsetof(struct ARMCore, memory.load8));
		uintptr_t expected = store ? (width == 4 ? (uintptr_t) GBAStore32 :
			width == 2 ? (uintptr_t) GBAStore16 : (uintptr_t) GBAStore8) :
			(width == 4 ? (uintptr_t) GBALoad32 : width == 2 ? (uintptr_t) GBALoad16 : (uintptr_t) GBALoad8);
		struct RV32Block code = {0};
		code.resident = true;
		code.arm = i >= 8;
#ifdef MGBA_RV32_TRACE_POLL
		code.context = context;
#endif
		unsigned complete, iwramComplete;
		if (!_emitIWRAMAccess(&code, width, sign, callback, expected, store, &iwramComplete) ||
		    !_emitInlineDataAccess(&code, width, sign, 0, callback, expected, store, true, &complete) ||
		    !_emit(&code, _rvI(store ? 4 : 8, RV_RA, 0, RV_X0, 0x67))) return false;
		code.nativeCode[iwramComplete] = _rvI(0, RV_RA, 0, RV_X0, 0x67);
		code.nativeCode[complete] = _rvI(0, RV_RA, 0, RV_X0, 0x67);
		if (code.nativeWords > RV32_DATA_MAX_WORDS) return false;
		memcpy(context->dataCode[i], code.nativeCode, code.nativeWords * sizeof(uint32_t));
		memcpy(context->dataCodeTail[i], code.nativeCode, code.nativeWords * sizeof(uint32_t));
	}
	__asm__ volatile("fence.i" ::: "memory");
	return true;
}

/* Put the common aligned IWRAM path first in each shared routine. All guards precede
 * writes; unusual waits, other regions and callback overrides enter the
 * complete shared routine without changing architectural state. */
static bool _emitIWRAMAccess(struct RV32Block* block, unsigned width, unsigned sign,
                            unsigned callback, uintptr_t expected, bool store, unsigned* complete) {
	unsigned rejects[4], count = 0, noPrefetch[2];
#define IWRAM_REJECT(rs2, rs1, condition) do { \
	rejects[count++] = block->nativeWords; \
	if (!_emit(block, _rvB(0, rs2, rs1, condition))) return false; \
} while (0)
	if (!_emitLoad(block, RV_T0, callback) || !_emitLoadImmediate(block, RV_T1, expected)) return false;
	IWRAM_REJECT(RV_T1, RV_T0, 1);
	if (!_emitShift(block, 0, BASE_OFFSET, RV_A1, 5, RV_T0) ||
	    !_emitI(block, GBA_REGION_IWRAM, RV_X0, 0, RV_T1)) return false;
	IWRAM_REJECT(RV_T1, RV_T0, 1);
	if (!store && width > 1) {
		if (!_emitI(block, width - 1, RV_A1, 7, RV_T0)) return false;
		IWRAM_REJECT(RV_X0, RV_T0, 1);
	}
	if (!_emitLoad(block, RV_A3, offsetof(struct ARMCore, master)) ||
	    !_emitI(block, offsetof(struct GBA, memory), RV_A3, 0, RV_A3) ||
	    !_emitLoadImmediate(block, RV_A2, offsetof(struct GBAMemory, waitstatesNonseq16)) ||
	    !_emitR(block, 0, RV_A3, RV_A2, 0, RV_A2)) return false;
#define IWRAM_STATE(field) ((int) offsetof(struct GBAMemory, field) - (int) offsetof(struct GBAMemory, waitstatesNonseq16))
	if (!_emit(block, _rvI(IWRAM_STATE(prefetch), RV_A2, 4, RV_T0, 0x03))) return false;
	noPrefetch[0] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_T0, 0)) ||
	    !_emit(block, _rvI(IWRAM_STATE(activeRegion), RV_A2, 2, RV_T0, 0x03)) ||
	    !_emitI(block, GBA_REGION_ROM0, RV_X0, 0, RV_T1)) return false;
	noPrefetch[1] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 4)) ||
	    !_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeSeqCycles16)) ||
	    !_emitI(block, 1, RV_T0, 0, RV_A4) ||
	    !_emitI(block, store ? 1 : 2, RV_X0, 0, RV_T1)) return false;
	IWRAM_REJECT(RV_T1, RV_A4, 4);
	if (!_emitI(block, 0, RV_A7, 0, RV_A5) ||
	    !_emit(block, _rvI(IWRAM_STATE(lastPrefetchedPc), RV_A2, 2, RV_T1, 0x03)) ||
	    !_emitR(block, 0x20, RV_A5, RV_T1, 0, RV_T1) ||
	    !_emitI(block, 16, RV_X0, 0, RV_T2) ||
	    !_emit(block, _rvB(12, RV_T2, RV_T1, 7)) ||
	    !_emitI(block, -2, RV_T1, 7, RV_T1) ||
	    !_emitR(block, 0, RV_T1, RV_A5, 0, RV_A5) ||
	    !_emit(block, _rvS(IWRAM_STATE(lastPrefetchedPc), RV_A5, RV_A2, 2))) return false;
	unsigned timingReady = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < 2; ++i)
		block->nativeCode[noPrefetch[i]] |= _rvB((block->nativeWords - noPrefetch[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	if (!_emitLoad(block, RV_A4, offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitI(block, store ? 2 : 3, RV_A4, 0, RV_A4)) return false;
	block->nativeCode[timingReady] = _rvB((block->nativeWords - timingReady) * 4, RV_X0, RV_X0, 0);
	if (block->arm && (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeNonseqCycles32)) ||
	    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitR(block, 0x20, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitR(block, 0, RV_T0, RV_A4, 0, RV_A4))) return false;
	if (!_emit(block, _rvI(offsetof(struct GBAMemory, iwram), RV_A3, 2, RV_T0, 0x03)) ||
	    !_emitLoadImmediate(block, RV_T1, GBA_SIZE_IWRAM - width) ||
	    !_emitR(block, 0, RV_T1, RV_A1, 7, RV_T1) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0)) return false;
	if (store) {
		if (!_emitTraceStoreGuard(block, width) ||
		    !_emit(block, _rvS(0, RV_T3, RV_T0, width == 4 ? 2 : width == 2 ? 1 : 0))) return false;
	} else if (!_emit(block, _rvI(0, RV_T0,
	    width == 4 ? 2 : sign == 16 ? 1 : sign == 24 ? 0 : width == 2 ? 5 : 4, RV_T3, 0x03))) return false;
	if (!_emitR(block, 0, RV_A4, RV_S1, 0, RV_S1)) return false;
	*complete = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < count; ++i)
		block->nativeCode[rejects[i]] |= _rvB((block->nativeWords - rejects[i]) * 4, 0, 0, 0) & 0xFE000F80u;
#undef IWRAM_STATE
#undef IWRAM_REJECT
	return true;
}

static bool _usesSharedData(const struct RV32Block* block) {
	return block->sharedData && block->context && block->context->dataReady;
}

static bool _emitDataAccess(struct RV32Block* block, unsigned index, unsigned width, unsigned sign,
                         unsigned rd, unsigned callback, uintptr_t expected, bool store, unsigned* complete) {
	if (!_usesSharedData(block)) {
		return _emitInlineDataAccess(block, width, sign, rd, callback, expected, store, false, complete);
	}
	unsigned kind = store ? (width == 1 ? 5 : width == 2 ? 6 : 7) :
		sign == 24 ? 3 : sign == 16 ? 4 : width == 1 ? 0 : width == 2 ? 1 : 2;
	if (store && !_emitLoadRegister(block, RV_T3, rd)) return false;
	if (!_emitLoadImmediate(block, RV_A7, block->start + (index + 2) *
	                       (block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB))) return false;
	kind += block->arm ? 8 : 0;
	uintptr_t pc = (uintptr_t) &block->nativeCode[block->nativeWords];
	int32_t displacement = (uintptr_t) block->context->dataCode[kind] - pc;
	int32_t tail = (uintptr_t) block->context->dataCodeTail[kind] - pc;
	if (tail >= -1048576 && tail < 1048576) displacement = tail;
	if (displacement >= -1048576 && displacement < 1048576) {
		uint32_t imm = displacement;
		if (!_emit(block, ((imm & 0x100000) << 11) | ((imm & 0x7FE) << 20) |
		    ((imm & 0x800) << 9) | (imm & 0xFF000) | (RV_RA << 7) | 0x6F)) return false;
	} else {
		uint32_t upper = ((uint32_t) displacement + 0x800) & 0xFFFFF000;
		if (!_emit(block, upper | (RV_T0 << 7) | 0x17) ||
		    !_emit(block, _rvI((int32_t) ((uint32_t) displacement - upper), RV_T0, 0, RV_RA, 0x67))) return false;
	}
	unsigned result = block->nativeWords;
	if (!store && (!_emitStoreRegister(block, RV_T3, rd) || block->nativeWords != result + 1)) return false;
	*complete = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	return true;
}

static void _prepareDataAccess(struct RV32Block* block) {
	if (block->sharedData && block->context && !block->context->dataReady)
		block->context->dataReady = _emitDataHelpers(block->context);
}

/* PC-relative word loads have a known ROM address. Read the current
 * ROM storage (never fold its value), and preserve callback overrides, bounds
 * behavior and the data-region waitstate independently of instruction fetch. */
static bool _emitLiteralLoad(struct RV32Block* block, unsigned index, unsigned rd, unsigned* complete) {
	uint32_t address;
	if (block->arm) {
		uint32_t opcode = block->armOpcodes[index];
		int offset = opcode & 0xFFF;
		if (!(opcode & 0x00800000)) offset = -offset;
		address = block->start + (index + 2) * 4 + offset;
	} else address = ((block->start + (index + 2) * 2) & ~3u) + (block->opcodes[index] & 255) * 4;
	unsigned region = address >> BASE_OFFSET;
	if (region < GBA_REGION_ROM0 || region > GBA_REGION_ROM2_EX) return true;
	unsigned rejects[2];
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.load32)) ||
	    !_emitLoadImmediate(block, RV_T1, (uintptr_t) GBALoad32)) return false;
	rejects[0] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 1)) ||
	    !_emitLoad(block, RV_A2, offsetof(struct ARMCore, master)) ||
	    !_emitI(block, offsetof(struct GBA, memory), RV_A2, 0, RV_A2) ||
	    !_emitLoadImmediate(block, RV_A3, offsetof(struct GBAMemory, romSize)) ||
	    !_emitR(block, 0, RV_A2, RV_A3, 0, RV_A3) ||
	    !_emit(block, _rvI(0, RV_A3, 2, RV_T0, 0x03)) ||
	    !_emitLoadImmediate(block, RV_T1, address & (GBA_SIZE_ROM0 - 4))) return false;
	rejects[1] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T0, RV_T1, 7)) ||
	    !_emit(block, _rvI(offsetof(struct GBAMemory, rom), RV_A2, 2, RV_T0, 0x03)) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emit(block, _rvI(0, RV_T0, 2, RV_T2, 0x03))) return false;
	if (address & 3) {
		unsigned rotate = (address & 3) * 8;
		if (!_emitShift(block, 0, rotate, RV_T2, 5, RV_T1) ||
		    !_emitShift(block, 0, 32 - rotate, RV_T2, 1, RV_T2) ||
		    !_emitR(block, 0, RV_T1, RV_T2, 6, RV_T2)) return false;
	}
	int waitOffset = (int) offsetof(struct GBAMemory, waitstatesNonseq32) - (int) offsetof(struct GBAMemory, romSize);
	if (!_emit(block, _rvI(waitOffset + region, RV_A3, CHAR_MIN < 0 ? 0 : 4, RV_T0, 0x03)) ||
	    !_emitLoad(block, RV_T1, block->arm ? offsetof(struct ARMCore, memory.activeNonseqCycles32) : offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitI(block, 3, RV_T0, 0, RV_T0) ||
	    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, cycles)) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitStore(block, RV_T0, offsetof(struct ARMCore, cycles)) ||
	    !_emitStoreRegister(block, RV_T2, rd)) return false;
	*complete = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < 2; ++i) {
		block->nativeCode[rejects[i]] |= _rvB((block->nativeWords - rejects[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	}
	return true;
}

/* GBASavedataReadEEPROM's non-read-command case only searches the event list.
 * This helper neither observes deferred CPU fields nor changes device state,
 * so the resident registers and the native edge proof remain valid. */
static uint32_t _eepromReady(const struct GBASavedata* savedata) {
	return !mTimingIsScheduled(&savedata->p->timing, &savedata->dust);
}

static bool _emitEEPROMStatus(struct RV32Block* block, unsigned rd, unsigned sign, unsigned* complete) {
	unsigned rejects[4];
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.load16)) ||
	    !_emitLoadImmediate(block, RV_T1, (uintptr_t) GBALoad16)) return false;
	rejects[0] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 1)) ||
	    !_emitShift(block, 0, BASE_OFFSET, RV_A1, 5, RV_T0) ||
	    !_emitI(block, GBA_REGION_ROM2_EX, RV_X0, 0, RV_T1)) return false;
	rejects[1] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 1)) ||
	    !_emitLoad(block, RV_T4, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(block, RV_T0, offsetof(struct GBA, memory.savedata)) ||
	    !_emitR(block, 0, RV_T0, RV_T4, 0, RV_T4) ||
	    !_emit(block, _rvI(offsetof(struct GBASavedata, type), RV_T4, 2, RV_T0, 0x03)) ||
	    !_emitI(block, GBA_SAVEDATA_EEPROM, RV_X0, 0, RV_T1) ||
	    !_emit(block, _rvB(12, RV_T1, RV_T0, 0)) ||
	    !_emitI(block, GBA_SAVEDATA_EEPROM512, RV_X0, 0, RV_T1)) return false;
	rejects[2] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 1)) ||
	    !_emit(block, _rvI(offsetof(struct GBASavedata, command), RV_T4, 2, RV_T0, 0x03)) ||
	    !_emitI(block, EEPROM_COMMAND_READ, RV_X0, 0, RV_T1)) return false;
	rejects[3] = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_T1, RV_T0, 0)) ||
	    !_emit(block, _rvS(4, RV_A1, RV_SP, 2)) ||
	    !_emitI(block, 0, RV_T4, 0, RV_A0) ||
	    !_emitLoadImmediate(block, RV_T0, (uintptr_t) _eepromReady) ||
	    !_emit(block, _rvI(0, RV_T0, 0, RV_RA, 0x67)) ||
	    !_emitI(block, 0, RV_A0, 0, RV_T3) ||
	    !_emitI(block, 0, RV_S0, 0, RV_A0) ||
	    !_emit(block, _rvI(4, RV_SP, 2, RV_A1, 0x03)) ||
	    !_emitI(block, 1, RV_A1, 7, RV_T0)) return false;
	/* The status bit is 0/1. Odd LDRH rotates it to bit 24; odd LDRSH
	 * takes the low byte of that rotated value, which is zero. */
	if (sign == 16) {
		if (!_emitI(block, 1, RV_T0, 4, RV_T0) || !_emitR(block, 0, RV_T0, RV_T3, 7, RV_T3)) return false;
	} else if (!_emit(block, _rvB(8, RV_X0, RV_T0, 0)) || !_emitShift(block, 0, 24, RV_T3, 1, RV_T3)) return false;
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, master)) ||
	    !_emitLoadImmediate(block, RV_T1, offsetof(struct GBA, memory.waitstatesNonseq16[GBA_REGION_ROM2_EX])) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emit(block, _rvI(0, RV_T0, CHAR_MIN < 0 ? 0 : 4, RV_T0, 0x03)) ||
	    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, memory.activeNonseqCycles16)) ||
	    !_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) ||
	    !_emitI(block, 3, RV_T0, 0, RV_T0) ||
	    !_emitR(block, 0, RV_T0, RV_S1, 0, RV_S1) ||
	    !_emitStoreRegister(block, RV_T3, rd)) return false;
	*complete = block->nativeWords;
	if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) return false;
	for (unsigned i = 0; i < 4; ++i)
		block->nativeCode[rejects[i]] |= _rvB((block->nativeWords - rejects[i]) * 4, 0, 0, 0) & 0xFE000F80u;
	return true;
}

/* RAM and plain IO reads execute directly. Other addresses and overridden
 * memory callbacks retain the original ABI and side effects. */
static bool _emitMemoryLoad(struct RV32Block* block, unsigned index, const struct ARMCore* cpu) {
	uint16_t opcode = block->opcodes[index];
	unsigned rd = opcode & 7, rn = (opcode >> 3) & 7, rm = (opcode >> 6) & 7;
	unsigned width = 0, sign = 0, immediate = 0;
	bool reg = false, literal = false;
	switch (opcode & 0xF800) {
	case 0x6800: width = 4; immediate = ((opcode >> 6) & 31) * 4; break;
	case 0x7800: width = 1; immediate = (opcode >> 6) & 31; break;
	case 0x8800: width = 2; immediate = ((opcode >> 6) & 31) * 2; break;
	case 0x4800: width = 4; literal = true; rd = (opcode >> 8) & 7; immediate = (opcode & 255) * 4; break;
	case 0x9800: width = 4; rn = ARM_SP; rd = (opcode >> 8) & 7; immediate = (opcode & 255) * 4; break;
	default:
		switch (opcode & 0xFE00) {
		case 0x5800: width = 4; break;
		case 0x5A00: width = 2; break;
		case 0x5C00: width = 1; break;
		case 0x5600: width = 1; sign = 24; break;
		case 0x5E00: width = 2; sign = 16; break;
		default: return false;
		}
		reg = true;
		break;
	}
	if (literal) {
		uint32_t address = ((block->start + (index + 2) * 2) & ~3u) + immediate;
		if (!_emitLoadImmediate(block, RV_A1, address)) return false;
	} else {
		if (!_emitLoadRegister(block, RV_T1, rn)) return false;
		if (reg) {
			if (!_emitLoadRegister(block, RV_T2, rm) ||
			    !_emitR(block, 0, RV_T2, RV_T1, 0, RV_A1)) return false;
		} else if (!_emitI(block, immediate, RV_T1, 0, RV_A1)) return false;
	}
	unsigned callback = width == 4 ? offsetof(struct ARMCore, memory.load32) :
		width == 2 ? offsetof(struct ARMCore, memory.load16) : offsetof(struct ARMCore, memory.load8);
	uintptr_t expected = width == 4 ? (uintptr_t) GBALoad32 : width == 2 ? (uintptr_t) GBALoad16 : (uintptr_t) GBALoad8;
	uintptr_t configured = width == 4 ? (uintptr_t) cpu->memory.load32 :
		width == 2 ? (uintptr_t) cpu->memory.load16 : (uintptr_t) cpu->memory.load8;
	unsigned complete = 0, statusComplete = 0;
	if (configured == expected) {
		if (literal) {
			if (!_emitLiteralLoad(block, index, rd, &complete)) return false;
		} else if (!_emitDataAccess(block, index, width, sign, rd, callback, expected, false, &complete)) return false;
	}
	/* Native memory routines use the explicit PC argument. Reconstruct the
	 * complete pipeline only if execution reaches a callback or private helper. */
	if (_usesSharedData(block) && !_emitPipeline(block, index + 1)) return false;
	if (width == 2 && block->resident && configured == expected &&
	    !_emitEEPROMStatus(block, rd, sign, &statusComplete)) return false;
	if (sign == 16 && !_emit(block, _rvS(4, RV_A1, RV_SP, 2))) return false;
	if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeSeqCycles16)) ||
	    !_emitI(block, 1, RV_T0, 0, RV_T0) ||
	    !_emit(block, _rvS(0, RV_T0, RV_SP, 2)) ||
	    !_emitI(block, 0, RV_SP, 0, RV_A2) ||
	    !_emitLoad(block, RV_T0, callback) ||
	    !_emitLoopCallback(block) ||
	    !_emitResidentState(block, true) ||
	    !_emit(block, _rvI(0, RV_T0, 0, RV_RA, 0x67)) ||
	    !_emitResidentState(block, false)) return false;
	if (sign == 16) {
		/* An odd signed-halfword load sign-extends the rotated low byte. */
		if (!_emit(block, _rvI(4, RV_SP, 2, RV_T0, 0x03)) ||
		    !_emitI(block, 1, RV_T0, 7, RV_T0) ||
		    !_emitI(block, 3, RV_T0, 1, RV_T0) ||
		    !_emitI(block, 16, RV_T0, 0, RV_T0) ||
		    !_emitR(block, 0, RV_T0, RV_A0, 1, RV_A0) ||
		    !_emitR(block, 0x20, RV_T0, RV_A0, 5, RV_A0)) return false;
	} else if (sign && (!_emitShift(block, 0, sign, RV_A0, 1, RV_A0) ||
	                    !_emitShift(block, 0x20, sign, RV_A0, 5, RV_A0))) return false;
	bool emitted = _emitI(block, 0, RV_A0, 0, RV_T0) &&
		_emitI(block, 0, RV_S0, 0, RV_A0) &&
		_emitStoreRegister(block, RV_T0, rd) &&
		_emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeNonseqCycles16)) &&
		_emitLoad(block, RV_T1, offsetof(struct ARMCore, memory.activeSeqCycles16)) &&
		_emitR(block, 0x20, RV_T1, RV_T0, 0, RV_T0) &&
		_emit(block, _rvI(0, RV_SP, 2, RV_T1, 0x03)) &&
		_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) &&
		_emitLoad(block, RV_T1, offsetof(struct ARMCore, cycles)) &&
		_emitR(block, 0, RV_T1, RV_T0, 0, RV_T0) &&
		_emitStore(block, RV_T0, offsetof(struct ARMCore, cycles)) && _emitCallbackEnd(block);
	if (complete) block->nativeCode[complete] = _rvB((block->nativeWords - complete) * 4, RV_X0, RV_X0, 0);
	if (statusComplete) block->nativeCode[statusComplete] = _rvB((block->nativeWords - statusComplete) * 4, RV_X0, RV_X0, 0);
	return emitted;
}

/* RAM writes avoid both the Thumb handler and the memory callback. Stores to
 * any other region retain the entire handler, including DMA/IO side effects.
 * Only a successful RAM write may continue within the translated block. */
static bool _emitMemoryStore(struct RV32Block* block, unsigned index, const struct ARMCore* cpu) {
	uint16_t opcode = block->opcodes[index];
	unsigned rd = opcode & 7, rn = (opcode >> 3) & 7, rm = (opcode >> 6) & 7;
	unsigned width, immediate = 0;
	bool reg = false;
	switch (opcode & 0xF800) {
	case 0x6000: width = 4; immediate = ((opcode >> 6) & 31) * 4; break;
	case 0x7000: width = 1; immediate = (opcode >> 6) & 31; break;
	case 0x8000: width = 2; immediate = ((opcode >> 6) & 31) * 2; break;
	case 0x9000: width = 4; rn = ARM_SP; rd = (opcode >> 8) & 7; immediate = (opcode & 255) * 4; break;
	default:
		switch (opcode & 0xFE00) {
		case 0x5000: width = 4; break;
		case 0x5200: width = 2; break;
		case 0x5400: width = 1; break;
		default: return false;
		}
		reg = true;
	}
	uintptr_t expected = width == 4 ? (uintptr_t) GBAStore32 : width == 2 ? (uintptr_t) GBAStore16 : (uintptr_t) GBAStore8;
	uintptr_t configured = width == 4 ? (uintptr_t) cpu->memory.store32 :
		width == 2 ? (uintptr_t) cpu->memory.store16 : (uintptr_t) cpu->memory.store8;
	if (configured != expected) return false;
	unsigned callback = width == 4 ? offsetof(struct ARMCore, memory.store32) :
		width == 2 ? offsetof(struct ARMCore, memory.store16) : offsetof(struct ARMCore, memory.store8);
	if (!_emitLoadRegister(block, RV_T1, rn)) return false;
	if (reg) {
		if (!_emitLoadRegister(block, RV_T2, rm) || !_emitR(block, 0, RV_T2, RV_T1, 0, RV_A1)) return false;
	} else if (!_emitI(block, immediate, RV_T1, 0, RV_A1)) return false;
	unsigned complete = 0;
	if (!_emitDataAccess(block, index, width, 0, rd, callback, expected, true, &complete) ||
	    (_usesSharedData(block) && !_emitPipeline(block, index + 1)) ||
	    !_emitLoadImmediate(block, RV_A1, opcode) ||
	    !_emitCall(block, (uintptr_t) _thumbTable[opcode >> 6]) ||
	    !_emitCallbackEnd(block)) return false;
	block->nativeCode[complete] = _rvB((block->nativeWords - complete) * 4, RV_X0, RV_X0, 0);
	return true;
}

/* ARM data processing with immediate or immediate-shifted operands. Register
 * shifts, carry arithmetic and PC destinations retain the original handler. */
static bool _isARMALU(uint32_t opcode) {
	unsigned operation = (opcode >> 21) & 15;
	return (opcode & 0x0C000000) == 0 &&
		((opcode & 0x02000000) || !(opcode & 0x10)) &&
		((opcode >> 12) & 15) != ARM_PC &&
		(operation < 5 || operation > 7) &&
		(operation < 8 || operation > 11 || (opcode & 0x00100000));
}

static bool _emitARMFetchCycles(struct RV32Block* block) {
	return _emitLoad(block, RV_T0, offsetof(struct ARMCore, memory.activeSeqCycles32)) &&
		_emitI(block, 1, RV_T0, 0, RV_T0) && _emitR(block, 0, RV_T0, RV_S1, 0, RV_S1);
}

static bool _emitARMALU(struct RV32Block* block, uint32_t opcode) {
	unsigned operation = (opcode >> 21) & 15, rd = (opcode >> 12) & 15;
	unsigned rn = (opcode >> 16) & 15, rm = opcode & 15;
	bool flags = opcode & 0x00100000;
	if (!_isARMALU(opcode)) return false;
	/* T1 is Operand2, A2 is the exact interpreter shifterCarryOut, including
	 * the signed -1 used by sign extraction. CPSR takes only its low bit. */
	if (!_emitShift(block, 0, 29, RV_S2, 5, RV_A2) ||
	    !_emitI(block, 1, RV_A2, 7, RV_A2)) return false;
	if (opcode & 0x02000000) {
		unsigned rotate = (opcode >> 7) & 30;
		uint32_t value = opcode & 255;
		if (rotate) value = (value >> rotate) | (value << (32 - rotate));
		if (!_emitLoadImmediate(block, RV_T1, value) ||
		    (rotate && !_emitI(block, (int32_t) value >> 31, RV_X0, 0, RV_A2))) return false;
	} else {
		unsigned shift = (opcode >> 7) & 31, kind = (opcode >> 5) & 3;
		if (!_emitLoadRegister(block, RV_T1, rm)) return false;
		if (kind == 0 && shift) {
			if (!_emitShift(block, 0, 32 - shift, RV_T1, 5, RV_A2) ||
			    !_emitI(block, 1, RV_A2, 7, RV_A2) ||
			    !_emitShift(block, 0, shift, RV_T1, 1, RV_T1)) return false;
		} else if (kind == 1 || kind == 2) {
			if (shift) {
				if (!_emitShift(block, 0, shift - 1, RV_T1, 5, RV_A2) ||
				    !_emitI(block, 1, RV_A2, 7, RV_A2) ||
				    !_emitShift(block, kind == 2 ? 0x20 : 0, shift, RV_T1, 5, RV_T1)) return false;
			} else if (!_emitShift(block, 0x20, 31, RV_T1, 5, RV_A2) ||
			           !_emitI(block, 0, kind == 2 ? RV_A2 : RV_X0, 0, RV_T1)) return false;
		} else if (kind == 3) {
			if (shift) {
				if (!_emitShift(block, 0, shift - 1, RV_T1, 5, RV_A2) ||
				    !_emitI(block, 1, RV_A2, 7, RV_A2) ||
				    !_emitShift(block, 0, 32 - shift, RV_T1, 1, RV_T2) ||
				    !_emitShift(block, 0, shift, RV_T1, 5, RV_T1) ||
				    !_emitR(block, 0, RV_T2, RV_T1, 6, RV_T1)) return false;
			} else if (!_emitShift(block, 0, 31, RV_A2, 1, RV_T2) ||
			           !_emitI(block, 1, RV_T1, 7, RV_A2) ||
			           !_emitShift(block, 0, 1, RV_T1, 5, RV_T1) ||
			           !_emitR(block, 0, RV_T2, RV_T1, 6, RV_T1)) return false;
		}
	}
	if (!_emitStore(block, RV_T1, offsetof(struct ARMCore, shifterOperand)) ||
	    !_emitStore(block, RV_A2, offsetof(struct ARMCore, shifterCarryOut)) ||
	    !_emitLoadRegister(block, RV_T0, rn)) return false;
	bool subtraction = operation == 2 || operation == 3 || operation == 10;
	switch (operation) {
	case 0: case 8: if (!_emitR(block, 0, RV_T1, RV_T0, 7, RV_T2)) return false; break;
	case 1: case 9: if (!_emitR(block, 0, RV_T1, RV_T0, 4, RV_T2)) return false; break;
	case 2: case 10: if (!_emitR(block, 0x20, RV_T1, RV_T0, 0, RV_T2)) return false; break;
	case 3: if (!_emitR(block, 0x20, RV_T0, RV_T1, 0, RV_T2)) return false; break;
	case 4: case 11: if (!_emitR(block, 0, RV_T1, RV_T0, 0, RV_T2)) return false; break;
	case 12: if (!_emitR(block, 0, RV_T1, RV_T0, 6, RV_T2)) return false; break;
	case 13: if (!_emitI(block, 0, RV_T1, 0, RV_T2)) return false; break;
	case 14:
		if (!_emitI(block, -1, RV_T1, 4, RV_T2) || !_emitR(block, 0, RV_T2, RV_T0, 7, RV_T2)) return false;
		break;
	case 15: if (!_emitI(block, -1, RV_T1, 4, RV_T2)) return false; break;
	default: return false;
	}
	if (flags) {
		if (!_emitI(block, 0, RV_S2, 0, RV_A1) ||
		    !_emitLoadImmediate(block, RV_T3, 0x80000000u) ||
		    !_emitLoadImmediate(block, RV_T4, 0x10000000u)) return false;
		if (subtraction || operation == 4 || operation == 11) {
			if (!_emitFlagsAddSub(block, operation == 3 ? RV_T1 : RV_T0,
			                     operation == 3 ? RV_T0 : RV_T1, RV_T2, subtraction)) return false;
		} else if (!_emitLoadImmediate(block, RV_T5, 0x3FFFFFFFu) ||
		           !_emitLoadImmediate(block, RV_T6, 0x0FFFFFFFu) ||
		           !_emitShiftFlags(block, RV_T2, RV_A2)) return false;
		if (!_emitI(block, 0, RV_A1, 0, RV_S2)) return false;
	}
	return ((operation >= 8 && operation <= 11) || _emitStoreRegister(block, RV_T2, rd)) &&
		_emitARMFetchCycles(block);
}

/* Emit the false-condition edge. NV consumes fetch time but executes no body. */
static bool _emitARMCondition(struct RV32Block* block, unsigned condition, unsigned* skip) {
	*skip = 0;
	if (condition == 14) return true;
	if (condition != 15) {
		static const uint16_t conditions[] = {0xF0F0, 0x0F0F, 0xCCCC, 0x3333,
			0xFF00, 0x00FF, 0xAAAA, 0x5555, 0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA};
		if (!_emitShift(block, 0, 28, RV_S2, 5, RV_T0) ||
		    !_emitLoadImmediate(block, RV_T1, conditions[condition]) ||
		    !_emitR(block, 0, RV_T0, RV_T1, 5, RV_T0) ||
		    !_emitI(block, 1, RV_T0, 7, RV_T0)) return false;
	}
	*skip = block->nativeWords;
	return _emit(block, _rvB(0, RV_X0, condition == 15 ? RV_X0 : RV_T0, 0));
}

static bool _emitARMEnd(struct RV32Block* block) {
	if (!_emitPollEnd(block)) return false;
	if (block->dispatch) return _emitLoadImmediate(block, RV_T0, (uintptr_t) block->dispatch) &&
		_emit(block, _rvI(0, RV_T0, 0, RV_X0, 0x67));
	return _emitI(block, 1, RV_X0, 0, RV_A0) && _emitReturn(block);
}

/* Immediate LDR/LDRB, including pre/post writeback and user-mode forms.
 * Delay writeback until a side-effect-free native read succeeds; the slow
 * path executes the complete original instruction with untouched registers.
 * LDRT can bypass temporary bank switching only in USER/SYSTEM modes. */
static bool _emitARMLoad(struct RV32Block* block, uint32_t opcode, const struct ARMCore* cpu, unsigned index) {
	if ((opcode & 0x0E100000) != 0x04100000 || ((opcode >> 12) & 15) == ARM_PC) return false;
	unsigned rd = (opcode >> 12) & 15, rn = (opcode >> 16) & 15;
	bool pre = opcode & 0x01000000;
	bool writeback = !pre || (opcode & 0x00200000);
	bool user = !pre && (opcode & 0x00200000);
	if (writeback && rn == ARM_PC) return false;
	unsigned width = opcode & 0x00400000 ? 1 : 4;
	unsigned callback = width == 4 ? offsetof(struct ARMCore, memory.load32) : offsetof(struct ARMCore, memory.load8);
	uintptr_t expected = width == 4 ? (uintptr_t) GBALoad32 : (uintptr_t) GBALoad8;
	uintptr_t configured = width == 4 ? (uintptr_t) cpu->memory.load32 : (uintptr_t) cpu->memory.load8;
	if (configured != expected) return false;
	unsigned privilege = 0;
	if (user) {
		if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, privilegeMode)) ||
		    !_emitI(block, MODE_USER, RV_X0, 0, RV_T1) ||
		    !_emit(block, _rvB(12, RV_T1, RV_T0, 0)) ||
		    !_emitI(block, MODE_SYSTEM, RV_X0, 0, RV_T1)) return false;
		privilege = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_T1, RV_T0, 1))) return false;
	}
	int offset = opcode & 0xFFF;
	if (!(opcode & 0x00800000)) offset = -offset;
	if (!_emitLoadRegister(block, RV_A1, rn)) return false;
	if (offset || pre) {
		if (!_emitLoadImmediate(block, RV_T2, offset) ||
		    !_emitR(block, 0, RV_T2, RV_A1, 0, RV_T1)) return false;
	} else if (!_emitI(block, 0, RV_A1, 0, RV_T1)) return false;
	bool commit = writeback && rd != rn && offset;
	if (commit && !_emit(block, _rvS(8, RV_T1, RV_SP, 2))) return false;
	if (pre && !_emitI(block, 0, RV_T1, 0, RV_A1)) return false;
	unsigned complete = 0;
	if (rn == ARM_PC && width == 4 && !writeback) {
		if (!_emitLiteralLoad(block, index, rd, &complete)) return false;
	} else if (!_emitDataAccess(block, index, width, 0, rd, callback, expected, false, &complete)) return false;
	if (privilege) block->nativeCode[privilege] |= _rvB((block->nativeWords - privilege) * 4, 0, 0, 0) & 0xFE000F80u;
	if (block->context) {
		/* A device path that rejects native execution usually recurs in
		 * the same loop. Resume the interpreter before this instruction,
		 * avoiding helper/spill/cache-validation churn on every iteration. */
		if (!_emitLoadImmediate(block, RV_T0, (uintptr_t) &block->context->deviceSlices) ||
		    !_emitI(block, RV32_DEVICE_RETRY_SLICES, RV_X0, 0, RV_T1) ||
		    !_emit(block, _rvS(0, RV_T1, RV_T0, 2)) ||
		    !_emitPipeline(block, index) || !_emitI(block, 0, RV_X0, 0, RV_A0) || !_emitReturn(block)) return false;
	} else if (!_emitLoadImmediate(block, RV_A1, opcode) ||
	           !_emitCall(block, (uintptr_t) _armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 15)]) ||
	           !_emitARMEnd(block)) return false;
	if (complete) block->nativeCode[complete] = _rvB((block->nativeWords - complete) * 4, RV_X0, RV_X0, 0);
	return !commit || (_emit(block, _rvI(8, RV_SP, 2, RV_T0, 0x03)) && _emitStoreRegister(block, RV_T0, rn));
}

#ifdef MGBA_RV32_VERIFY
static void _verifyARMBefore(struct ARMCore* cpu, uint32_t opcode) {
	_expected = *cpu; /* Pipeline was already advanced and condition passed. */
	_armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 15)](&_expected, opcode);
}
#endif

/* ARM conditions and shifter state are included in the runtime fixed-point
 * proof. Admit only native ALU/read bodies and a B back to this entry. */
static bool _isARMPollingLoop(struct RV32Block* block) {
	if (!block->cache || !block->branchDispatch || !block->length) return false;
	unsigned last = block->length - 1;
	uint32_t branch = block->armOpcodes[last];
	if ((branch & 0x0F000000) != 0x0A000000 || branch >> 28 == 15 ||
	    (last + 2) * 4 + ((int32_t) (branch << 8) >> 6) != 0) return false;
	uint32_t dependencies[16], written = 0, addresses = 0;
	for (unsigned r = 0; r < 16; ++r) dependencies[r] = 1u << r;
	for (unsigned i = 0; i < last; ++i) {
		uint32_t opcode = block->armOpcodes[i], inputs;
		unsigned rd = (opcode >> 12) & 15, rn = (opcode >> 16) & 15;
		if (_isARMALU(opcode)) {
			unsigned operation = (opcode >> 21) & 15;
			if (operation >= 8 && operation <= 11) continue;
			inputs = opcode & 0x02000000 ? 0 : dependencies[opcode & 15];
			if (operation != 13 && operation != 15) inputs |= dependencies[rn];
		} else if ((opcode & 0x0E100000) == 0x04100000 && rd != ARM_PC) {
			bool writeback = !(opcode & 0x01000000) || (opcode & 0x00200000);
			/* An advancing base carries state across iterations. Zero
			 * offset writeback leaves it unchanged (rd == rn is below). */
			if (writeback && ((opcode & 0xFFF) || rn == ARM_PC)) return false;
			inputs = dependencies[rn];
			addresses |= inputs;
		} else return false;
		/* A skipped conditional write leaves its previous value live. */
		if (opcode >> 28 != 14) inputs |= dependencies[rd];
		dependencies[rd] = inputs;
		written |= 1u << rd;
	}
	if (addresses & written) return false;
	for (unsigned r = 0; r < 16; ++r) if ((written & (1u << r)) && (dependencies[r] & written)) return false;
	block->pollWrites = written;
	return true;
}

static bool _emitARMBlock(struct RV32Block* block, struct ARMCore* cpu) {
	_prepareDataAccess(block);
	block->resident = true;
	block->registers = NULL;
	block->pollLoop = _isARMPollingLoop(block);
	block->loopPure = false;
	block->seqCycles = cpu->memory.activeSeqCycles32;
	block->nativeWords = 0;
	if (!_emitEntry(block) || block->nativeWords != RV32_BLOCK_BODY_WORD || !_emitWritableEntry(block) || !_emitPollBegin(block)) return false;
	unsigned exits[RV32_BLOCK_MAX_INSTRUCTIONS], count = 0, i = 0;
	for (; i < block->length; ++i) {
		unsigned begin = block->nativeWords;
		uint32_t opcode = block->armOpcodes[i];
		if (!_emitLoad(block, RV_T1, offsetof(struct ARMCore, nextEvent))) goto full;
		exits[count] = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_T1, RV_S1, 5)) || !_emitPipeline(block, i + 1)) goto full;
		unsigned skip;
		if (!_emitARMCondition(block, opcode >> 28, &skip)) goto full;
		unsigned body = block->nativeWords;
		bool terminal = false;
		if ((opcode & 0x0E000000) == 0x0A000000) {
			uint32_t target = block->start + (i + 2) * 4 + ((int32_t) (opcode << 8) >> 6);
			if ((opcode & 0x01000000) &&
			    (!_emitLoadImmediate(block, RV_T0, block->start + (i + 1) * 4) ||
			     !_emitStoreRegister(block, RV_T0, ARM_LR))) goto full;
			if (!_emitFastBranchCall(block, target) || !_emitTakenBranch(block, target)) goto full;
			terminal = true;
		} else if (_isARMALU(opcode)) {
#ifdef MGBA_RV32_VERIFY
			if (!_emitLoadImmediate(block, RV_A1, opcode) ||
			    !_emitCall(block, (uintptr_t) _verifyARMBefore)) goto full;
#endif
			if (!_emitARMALU(block, opcode)) goto full;
#ifdef MGBA_RV32_VERIFY
			if (!_emitCall(block, (uintptr_t) _verifyAfter)) goto full;
#endif
		} else if (!_emitARMLoad(block, opcode, cpu, i)) {
			block->nativeWords = body;
			if (!_emitLoadImmediate(block, RV_A1, opcode) ||
			    !_emitCall(block, (uintptr_t) _armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 15)])) goto full;
			terminal = true;
		}
		if (skip) {
			unsigned done = block->nativeWords;
			if (!_emit(block, _rvB(0, RV_X0, RV_X0, 0))) goto full;
			block->nativeCode[skip] |= _rvB((block->nativeWords - skip) * 4, 0, 0, 0) & 0xFE000F80u;
			if (!_emitARMFetchCycles(block)) goto full;
			block->nativeCode[done] = _rvB((block->nativeWords - done) * 4, RV_X0, RV_X0, 0);
		}
		/* Every deadline exit needs a complete ARM pipeline reconstruction. */
		if (block->nativeWords + 64 + (count + 1) * 10 +
		    (block->pollLoop ? RV32_POLL_END_MAX_WORDS : 0) > RV32_NATIVE_MAX_WORDS) goto full;
		++count;
		if (terminal) { ++i; break; }
		continue;
	full:
		block->nativeWords = begin;
		break;
	}
	if (!i) return false;
	if (i != block->length) block->pollLoop = false;
	block->length = i;
	if (!_emitARMEnd(block)) return false;
	unsigned finish = block->nativeWords;
	if (!_emitI(block, 1, RV_X0, 0, RV_A0) || !_emitReturn(block)) return false;
	for (unsigned n = 0; n < count; ++n) {
		unsigned restore = block->nativeWords;
		if (!_emitPipeline(block, n) ||
		    !_emit(block, _rvB(((int) finish - block->nativeWords) * 4, RV_X0, RV_X0, 0))) return false;
		block->nativeCode[exits[n]] = _rvB((restore - exits[n]) * 4, RV_T1, RV_S1, 5);
	}
	__asm__ volatile("fence.i" ::: "memory");
	return true;
}

/* A block is one native function. Unsupported instructions and special memory
 * accesses call existing handlers; arithmetic, RAM and plain IO stay in RV32.
 * Return zero only when an event splits a span: the interpreter then executes
 * the remaining partial span without losing its intermediate flags. */
static bool _emitExecutionBlock(struct RV32Block* block, struct ARMCore* cpu) {
	_prepareDataAccess(block);
	unsigned exits[RV32_BLOCK_MAX_INSTRUCTIONS], slowExits[RV32_BLOCK_MAX_INSTRUCTIONS];
	unsigned count = 0, slowCount = 0;
	uint8_t restore[RV32_BLOCK_MAX_INSTRUCTIONS], slowRestore[RV32_BLOCK_MAX_INSTRUCTIONS];
	/* Direct links enter without writing PC/prefetch. Every first-instruction
	 * event exit and every helper must reconstruct the target's pipeline. */
	bool pendingPipeline = true;
	unsigned stubs = 0;
	block->seqCycles = cpu->memory.activeSeqCycles16;
	block->nativeWords = 0;
	block->registers = NULL;
	block->resident = true;
	block->pollLoop = _isPollingLoop(block);
	block->loopPure = false;
	memset(block->spans, 0, sizeof(block->spans));
	if (!_emitEntry(block)) return false;
	unsigned body = block->nativeWords;
	if (body != RV32_BLOCK_BODY_WORD || !_emitWritableEntry(block) || !_emitPollBegin(block)) return false;
	unsigned i = 0;
	while (i < block->length) {
		unsigned begin = block->nativeWords;
		unsigned oldCount = count, oldSlow = slowCount, oldStubs = stubs;
		bool oldPending = pendingPipeline;
		bool terminal = false;
		if (!_emitLoad(block, RV_T0, offsetof(struct ARMCore, cycles)) ||
		    !_emitLoad(block, RV_T1, offsetof(struct ARMCore, nextEvent))) goto full;
		restore[count] = pendingPipeline ? i : UINT8_MAX;
		stubs += pendingPipeline;
		exits[count++] = block->nativeWords;
		if (!_emit(block, _rvB(0, RV_T1, RV_T0, 5))) goto full; /* bge cycles,event */
		unsigned guard = block->nativeWords;
		/* Reserve the deadline guard. Single instructions may cross an
		 * event; longer spans must expose every intermediate event boundary. */
		for (unsigned n = 0; n < 3; ++n) if (!_emitI(block, 0, RV_X0, 0, RV_X0)) goto full;
#ifdef MGBA_RV32_VERIFY
		unsigned verify = block->nativeWords;
		/* Instructions for the verification call are patched after the span
		 * length is known, and removed if this instruction needs a helper. */
		if ((pendingPipeline && !_emitPipeline(block, i)) ||
		    !_emitLoadImmediate(block, RV_A1, (uintptr_t) block) ||
		    !_emitI(block, i, RV_X0, 0, RV_A2) ||
		    !_emitCall(block, (uintptr_t) _verifyBefore)) goto full;
#endif
		unsigned length = _emitNativeSpan(block, cpu, i);
		if (length) {
			--block->nativeWords; /* Inline the span, replacing its ret. */
			if (length == 1) {
				memmove(block->nativeCode + guard, block->nativeCode + guard + 3,
				        (block->nativeWords - guard - 3) * sizeof(uint32_t));
				block->nativeWords -= 3;
				block->spans[i].offset -= 3;
			} else {
				block->nativeCode[guard] = _rvR(0x20, RV_T0, RV_T1, 0, RV_T2);
				block->nativeCode[guard + 1] = _rvI((length - 1) * (1 + block->seqCycles), RV_X0, 0, RV_T1, 0x13);
				slowRestore[slowCount] = pendingPipeline ? i : UINT8_MAX;
				stubs += pendingPipeline;
				slowExits[slowCount++] = guard + 2;
				block->nativeCode[guard + 2] = _rvB(0, RV_T2, RV_T1, 7); /* bgeu minimum,difference */
			}
			/* Prefetch and PC are needed at helper/event boundaries, not at
			 * every arithmetic span. Event exits reconstruct them below. */
			pendingPipeline = true;
#ifdef MGBA_RV32_VERIFY
			if (!_emitPipeline(block, i + length) ||
			    !_emitCall(block, (uintptr_t) _verifyAfter)) goto full;
			pendingPipeline = false;
#endif
		} else {
#ifdef MGBA_RV32_VERIFY
			block->nativeWords = verify;
#endif
			block->nativeWords = guard; /* No multi-instruction guard. */
			length = 1;
			unsigned instructionStart = block->nativeWords;
			bool memoryPending = false;
			if (!_emitBranch(block, i)) {
				block->nativeWords = instructionStart;
				bool deferred = _usesSharedData(block);
				if (!deferred && !_emitPipeline(block, i + 1)) goto full;
				unsigned memoryStart = block->nativeWords;
				bool handled = _emitMemoryLoad(block, i, cpu);
				if (!handled) {
					block->nativeWords = memoryStart;
					handled = _emitMemoryStore(block, i, cpu);
				}
				if (!handled) {
					block->nativeWords = memoryStart;
					if (deferred && !_emitPipeline(block, i + 1)) goto full;
					if (!_emitLoadImmediate(block, RV_A1, block->opcodes[i]) ||
					    !_emitCall(block, (uintptr_t) _thumbTable[block->opcodes[i] >> 6])) goto full;
					terminal = true;
				} else memoryPending = deferred;
			}
			pendingPipeline = memoryPending;
		}
		/* Leave space for both exits and the ABI epilogue. Roll back a
		 * complete segment so no shortened span can hide live flags. */
		if (block->nativeWords + 46 + stubs * 10 + (pendingPipeline ? 9 : 0) +
		    (block->pollLoop ? RV32_POLL_END_MAX_WORDS : 0) > RV32_NATIVE_MAX_WORDS) goto full;
		i += length;
		if (terminal) break;
		continue;
	full:
		block->nativeWords = begin;
		count = oldCount;
		slowCount = oldSlow;
		stubs = oldStubs;
		pendingPipeline = oldPending;
		break;
	}
	if (!i) return false;
	if (i != block->length) block->pollLoop = false;
	block->length = i;
	if (pendingPipeline && !_emitPipeline(block, i)) return false;
	if (!_emitPollEnd(block)) return false;
	if (block->dispatch && (!_emitLoadImmediate(block, RV_T0, (uintptr_t) block->dispatch) ||
	    !_emit(block, _rvI(0, RV_T0, 0, RV_X0, 0x67)))) return false;
	unsigned normal = block->nativeWords;
	if (!_emitI(block, 1, RV_X0, 0, RV_A0) ||
	    !_emit(block, _rvB(8, RV_X0, RV_X0, 0))) return false;
	unsigned slow = block->nativeWords;
	if (!_emitI(block, 0, RV_X0, 0, RV_A0) ||
	    !_emitReturn(block)) return false;
	for (unsigned n = 0; n < count; ++n) {
		unsigned target = normal;
		if (restore[n] != UINT8_MAX) {
			target = block->nativeWords;
			if (!_emitPipeline(block, restore[n]) ||
			    !_emit(block, _rvB(((int) normal - block->nativeWords) * 4, RV_X0, RV_X0, 0))) return false;
		}
		block->nativeCode[exits[n]] = _rvB(((int) target - exits[n]) * 4, RV_T1, RV_T0, 5);
	}
	for (unsigned n = 0; n < slowCount; ++n) {
		unsigned target = slow;
		if (slowRestore[n] != UINT8_MAX) {
			target = block->nativeWords;
			if (!_emitPipeline(block, slowRestore[n]) ||
			    !_emit(block, _rvB(((int) slow - block->nativeWords) * 4, RV_X0, RV_X0, 0))) return false;
		}
		block->nativeCode[slowExits[n]] = _rvB(((int) target - slowExits[n]) * 4, RV_T2, RV_T1, 7);
	}
	__asm__ volatile("fence.i" ::: "memory");
	return true;
}
#endif

static struct RV32Context* _contextFor(struct ARMCore* cpu, const struct GBA* gba) {
	struct RV32Context* freeContext = NULL;
	unsigned empty = RV32_MAX_CONTEXTS;
	for (unsigned i = 0; i < RV32_MAX_CONTEXTS; ++i) {
		struct RV32Context* context = _contexts[i];
		if (!context) {
			if (empty == RV32_MAX_CONTEXTS) empty = i;
			continue;
		}
		if (context->cpu == cpu) {
			if (context->rom != gba->memory.rom) {
				for (unsigned j = 0; j < RV32_BLOCK_CACHE_SIZE; ++j) {
					context->blocks[j].valid = false;
					context->blocks[j].branchEpoch = 0;
					context->blocks[j].pendingValid = false;
				}
				context->rom = gba->memory.rom;
			}
			return _lastContext = context;
		}
		if (!context->cpu && !freeContext) freeContext = context;
	}
	if (!freeContext && empty < RV32_MAX_CONTEXTS) {
		/* Code bytes need no initialization until a block is generated.
		 * Keep the bounded arenas for reuse after CPU reset/deinitialization. */
		freeContext = malloc(sizeof(*freeContext));
		if (!freeContext) return NULL;
		freeContext->dispatchReady = false;
		freeContext->branchReady = false;
		freeContext->armDispatchReady = false;
		freeContext->armBranchReady = false;
		freeContext->dataReady = false;
		_contexts[empty] = freeContext;
	}
	if (!freeContext) freeContext = _contexts[0];
	for (unsigned j = 0; j < RV32_BLOCK_CACHE_SIZE; ++j) {
		freeContext->blocks[j].valid = false;
		freeContext->blocks[j].branchEpoch = 0;
		freeContext->blocks[j].pendingValid = false;
	}
	freeContext->cpu = cpu;
	freeContext->codeEpoch = 0;
	freeContext->epochSerial = 0;
	freeContext->deviceSlices = 0;
#ifdef MGBA_RV32_TRACE_POLL
	freeContext->traceAllowed = freeContext->tracePC = 0;
#endif
	freeContext->rom = gba->memory.rom;
#if RV32_NATIVE
	if (!freeContext->dispatchReady) freeContext->dispatchReady = _emitDispatcher(freeContext, false);
	if (!freeContext->branchReady) freeContext->branchReady = _emitBranchDispatcher(freeContext, false);
	if (!freeContext->armDispatchReady) freeContext->armDispatchReady = _emitDispatcher(freeContext, true);
	if (!freeContext->armBranchReady) freeContext->armBranchReady = _emitBranchDispatcher(freeContext, true);
#ifdef MGBA_RV32_STATS
	if (!freeContext->dispatchReady || !freeContext->armDispatchReady || !freeContext->branchReady || !freeContext->armBranchReady) abort();
#endif
#endif
	return _lastContext = freeContext;
}

void RV32Invalidate(struct ARMCore* cpu) {
	for (unsigned i = 0; i < RV32_MAX_CONTEXTS; ++i) {
		struct RV32Context* context = _contexts[i];
		if (context && context->cpu == cpu) {
			for (unsigned j = 0; j < RV32_BLOCK_CACHE_SIZE; ++j) {
				context->blocks[j].valid = false;
				context->blocks[j].branchEpoch = 0;
				context->blocks[j].pendingValid = false;
			}
			context->codeEpoch = 0;
			context->cpu = NULL;
		}
	}
}

static bool _isRomAddress(uint32_t address) {
	unsigned region = address >> BASE_OFFSET;
	return region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX;
}

#ifndef MGBA_RV32_STATS
#define _buildBlockImpl _buildBlock
#endif
static struct RV32Block* _buildBlockImpl(struct RV32Context* context, struct ARMCore* cpu,
	                                  uint32_t start) {
	struct GBA* gba = (struct GBA*) cpu->master;
	if (!_isRomAddress(start) || !gba->memory.rom ||
	    cpu->memory.activeRegion != gba->memory.rom ||
	    start < GBA_BASE_ROM0 || (start & (GBA_SIZE_ROM0 - 1)) >= gba->memory.romSize) {
		return NULL;
	}

	struct RV32Block* block = &context->blocks[_blockIndex(start)];
	memset(block, 0, offsetof(struct RV32Block, nativeCode));
	block->arm = cpu->executionMode == MODE_ARM;
	block->writable = !gba->isPristine;
	block->context = context;
	unsigned width = block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB;
	if ((start & (width - 1)) || gba->memory.romSize < width) return NULL;
	block->region = cpu->memory.activeRegion;
	block->mask = cpu->memory.activeMask;
	block->start = start;
	if (block->arm ? context->armDispatchReady : context->dispatchReady)
		block->dispatch = block->arm ? context->armDispatchCode : context->dispatchCode;
	if (block->arm ? context->armBranchReady : context->branchReady) {
		block->branchDispatch = block->arm ? context->armBranchCode : context->branchCode;
		block->cache = context->blocks;
	}

	uint32_t address = start;
	unsigned initialRegion = address >> BASE_OFFSET;
	for (unsigned i = 0; i < RV32_BLOCK_MAX_INSTRUCTIONS; ++i, address += width) {
		if ((address >> BASE_OFFSET) != initialRegion ||
		    (address & (GBA_SIZE_ROM0 - 1)) > gba->memory.romSize - width) {
			break;
		}
		struct ARMInstructionInfo info;
		if (block->arm) {
			if (i < 2 && block->writable) block->armOpcodes[i] = cpu->prefetch[i];
			else { LOAD_32(block->armOpcodes[i], address & block->mask, block->region); }
			ARMDecodeARM(block->armOpcodes[i], &info);
		} else {
			if (i < 2 && block->writable) block->opcodes[i] = cpu->prefetch[i];
			else { LOAD_16(block->opcodes[i], address & block->mask, block->region); }
			ARMDecodeThumb(block->opcodes[i], &info);
		}
		block->length = (uint8_t) (i + 1);
		/* Native Thumb RAM stores can continue; their slow path leaves
		 * through a guarded entry after DMA, code or waitstate changes.
		 * Other stores still use a terminal instruction handler. */
		if (info.branchType || info.traps || info.mnemonic == ARM_MN_STM ||
		    (block->arm && info.mnemonic == ARM_MN_STR)) break;
	}
	if (!block->length) return NULL;

	/* The executor needs the two prefetched words after the final guest
	 * instruction. They are still pure ROM reads and carry no bus side effect. */
	for (unsigned i = block->length; i < (unsigned) block->length + 2; ++i) {
		uint32_t next = start + i * width;
		if ((next >> BASE_OFFSET) != initialRegion ||
		    (next & (GBA_SIZE_ROM0 - 1)) > gba->memory.romSize - width) {
			return NULL;
		}
		if (i < 2 && block->writable) {
			if (block->arm) block->armOpcodes[i] = cpu->prefetch[i];
			else block->opcodes[i] = cpu->prefetch[i];
		} else if (block->arm) { LOAD_32(block->armOpcodes[i], next & block->mask, block->region); }
		else { LOAD_16(block->opcodes[i], next & block->mask, block->region); }
	}
	/* Patching code does not replace words already in the guest pipeline. */
	if (_blockOpcode(block, 0) != cpu->prefetch[0] || _blockOpcode(block, 1) != cpu->prefetch[1]) return NULL;
#if RV32_NATIVE
	block->sharedData = true;
	block->executable = block->arm ? _emitARMBlock(block, cpu) : _emitExecutionBlock(block, cpu);
	if (!block->executable && !block->arm) {
		block->nativeWords = 0;
		memset(block->spans, 0, sizeof(block->spans));
		_emitNativeBlock(block, cpu);
	}
#else
	if (!block->arm) _emitNativeBlock(block, cpu);
#endif
	block->valid = true;
	block->verifiedEpoch = _codeEpoch(context);
	return block;
}

#ifdef MGBA_RV32_STATS
static struct RV32Block* _buildBlock(struct RV32Context* context, struct ARMCore* cpu, uint32_t start) {
#ifdef MGBA_RV32_STATS
	bool arm = cpu->executionMode == MODE_ARM;
	unsigned width = arm ? 4 : 2;
	uint32_t key = start | arm;
	unsigned index = (key ^ (key >> 13)) & 8191;
	unsigned probes = 0;
	while (_statsPC[index].key && _statsPC[index].key != key && ++probes < 8192) index = (index + 1) & 8191;
	if (probes == 8192) ++_stats.tableFull;
	else {
		if (_statsPC[index].key) ++_stats.repeatBuilds;
		_statsPC[index].key = key;
		++_statsPC[index].builds;
	}
	++_stats.builds;
	if (context->blocks[_blockIndex(start)].valid) ++_stats.evictions;
	for (unsigned i = 0; i < RV32_BLOCK_CACHE_SIZE; ++i) {
		const struct RV32Block* other = &context->blocks[i];
		if (!other->valid || other->arm != arm || other->region != cpu->memory.activeRegion || other->mask != cpu->memory.activeMask ||
		    other->seqCycles != (arm ? cpu->memory.activeSeqCycles32 : cpu->memory.activeSeqCycles16) || start <= other->start ||
		    start - other->start >= other->length * width || ((start - other->start) & (width - 1))) continue;
		unsigned offset = (start - other->start) / width;
		if (_blockOpcode(other, offset) != cpu->prefetch[0] || _blockOpcode(other, offset + 1) != cpu->prefetch[1]) continue;
		++_stats.interior;
		if (other->spans[offset].length) ++_stats.interiorSpan;
		if (probes < 8192) ++_statsPC[index].overlaps;
		break;
	}
	uint64_t cycles = _statsCounter(false), retired = _statsCounter(true);
#endif
	struct RV32Block* result = _buildBlockImpl(context, cpu, start);
#ifdef MGBA_RV32_STATS
	_stats.buildRetired += _statsCounter(true) - retired;
	_stats.buildCycles += _statsCounter(false) - cycles;
	if (!result) ++_stats.buildFail;
#endif
	return result;
}
#endif

static struct RV32Block* _findBlock(struct RV32Context* context, struct ARMCore* cpu,
                                 uint32_t start) {
	struct RV32Block* block = &context->blocks[_blockIndex(start)];
	bool arm = cpu->executionMode == MODE_ARM;
	const struct GBA* gba = (const struct GBA*) cpu->master;
	STAT_ADD(lookups);
	if (!block->valid || block->writable == gba->isPristine || block->arm != arm || block->start != start || block->region != cpu->memory.activeRegion ||
	    block->seqCycles != (arm ? cpu->memory.activeSeqCycles32 : cpu->memory.activeSeqCycles16) ||
	    block->mask != cpu->memory.activeMask || _blockOpcode(block, 0) != cpu->prefetch[0] ||
	    _blockOpcode(block, 1) != cpu->prefetch[1]) {
		if (!block->valid) STAT_ADD(missInvalid);
		else if (block->start != start || block->arm != arm) STAT_ADD(missConflict);
		else STAT_ADD(missState);
		/* A callback may explicitly invalidate this context while a native
		 * frame is active. Finish the slice in the interpreter; the next
		 * public entry will acquire a context with a live CPU owner. */
		if (context->cpu != cpu) return NULL;
		/* Do not compile single-use initialization paths. A cold miss
		 * keeps the old cached block intact until this address is revisited. */
		if (!block->pendingValid || block->pendingStart != start) {
			STAT_ADD(cold);
			block->pendingStart = start;
			block->pendingValid = true;
			return NULL;
		}
		return _buildBlock(context, cpu, start);
	}
	if (block->writable && !_validateWritableBlock(block)) return _buildBlock(context, cpu, start);
	STAT_ADD(hits);
	return block;
}

static bool _executeNativeBlock(struct ARMCore* cpu, const struct RV32Block* block, unsigned start) {
#if RV32_NATIVE
	const struct RV32NativeSpan* span = &block->spans[start];
	if (!span->length || cpu->memory.activeSeqCycles16 != block->seqCycles ||
	    (int64_t) cpu->nextEvent - cpu->cycles < span->cycles) return false;
#ifdef MGBA_RV32_VERIFY
	struct ARMCore expected = *cpu;
	for (unsigned i = start; i < start + span->length; ++i) {
		expected.prefetch[0] = block->opcodes[i + 1];
		expected.prefetch[1] = block->opcodes[i + 2];
		expected.gprs[ARM_PC] += WORD_SIZE_THUMB;
		_thumbTable[block->opcodes[i] >> 6](&expected, block->opcodes[i]);
	}
#endif
	((void (*)(struct ARMCore*)) (uintptr_t) (block->nativeCode + span->offset))(cpu);
	cpu->prefetch[0] = block->opcodes[start + span->length];
	cpu->prefetch[1] = block->opcodes[start + span->length + 1];
	cpu->gprs[ARM_PC] += span->length * WORD_SIZE_THUMB;
#ifdef MGBA_RV32_VERIFY
	if (memcmp(cpu, &expected, sizeof(*cpu))) {
		fprintf(stderr, "RV32 mismatch at %08lx, length=%u, CPSR=%08lx/%08lx, cycles=%ld/%ld\n",
		        (unsigned long) (block->start + start * 2), span->length,
		        (unsigned long) cpu->cpsr.packed, (unsigned long) expected.cpsr.packed,
		        (long) cpu->cycles, (long) expected.cycles);
		abort();
	}
#endif
	return true;
#else
	UNUSED(cpu);
	UNUSED(block);
	UNUSED(start);
	return false;
#endif
}

#if RV32_NATIVE
#ifdef MGBA_RV32_TRACE_POLL
static bool _traceTimingAllowed(const struct ARMCore* cpu) {
	if (cpu->executionMode != MODE_THUMB || cpu->cycles < 0 || cpu->halted ||
	    !cpu->cpsr.t || cpu->memory.activeSeqCycles16 > UINT8_MAX ||
	    cpu->memory.activeNonseqCycles16 > UINT8_MAX) return false;
#if CHAR_MIN < 0
	const struct GBA* gba = (const struct GBA*) cpu->master;
	if (gba->memory.waitstatesNonseq16[GBA_REGION_EWRAM] < 0 ||
	    gba->memory.waitstatesNonseq32[GBA_REGION_EWRAM] < 0) return false;
	for (unsigned r = GBA_REGION_ROM0; r <= GBA_REGION_ROM2_EX; ++r)
		if (gba->memory.waitstatesNonseq16[r] < 0 || gba->memory.waitstatesNonseq32[r] < 0) return false;
#endif
	return true;
}
#endif
/* Only full iterations are coalesced. Without external callbacks, these read-only
 * emitters can change only GPRs, CPSR, pipeline, fetch mask, ARM shifter state,
 * cycles, lastPrefetchedPc, lastJump and haltPending. Compare every mutable
 * CPU field and the prefetch queue; lastJump/haltPending updates are idempotent
 * on the guarded ordinary-ROM branch/plain-IO paths. Banked registers, mode,
 * deadlines, callbacks and other CPU fields cannot change on those paths.
 * External C callbacks (timers, input, idle detection) clear loopPure. Private
 * writable-code validation and EEPROM status helpers only read their inputs;
 * neither observes deferred CPU state nor mutates CPU/device state.
 * Extend this proof if native emitters gain other side effects. */
static bool _executePollingLoop(struct ARMCore* cpu, struct RV32Block* block) {
	bool (*execute)(struct ARMCore*) = (bool (*)(struct ARMCore*)) (uintptr_t) block->nativeCode;
	/* A positive total alone does not prove monotonic instruction deadlines.
	 * Reject artificial negative waits before skipping any iterations. */
	bool monotonic = cpu->memory.activeSeqCycles16 <= UINT8_MAX && cpu->memory.activeNonseqCycles16 <= UINT8_MAX;
	if (block->arm) monotonic = monotonic && cpu->memory.activeSeqCycles32 <= UINT8_MAX &&
		cpu->memory.activeNonseqCycles32 <= UINT8_MAX &&
		cpu->memory.activeNonseqCycles32 >= cpu->memory.activeNonseqCycles16;
#if CHAR_MIN < 0
	struct GBA* gba = (struct GBA*) cpu->master;
	monotonic = monotonic && gba->memory.waitstatesNonseq16[GBA_REGION_EWRAM] >= 0 &&
		gba->memory.waitstatesNonseq32[GBA_REGION_EWRAM] >= 0;
	for (unsigned r = GBA_REGION_ROM0; r <= GBA_REGION_ROM2_EX; ++r) {
		if (gba->memory.waitstatesNonseq16[r] < 0 || gba->memory.waitstatesNonseq32[r] < 0) monotonic = false;
	}
#endif
	if (!monotonic || cpu->cpsr.t == block->arm) {
		block->loopPure = false;
		if (block->context) _codeEpoch(block->context);
		return execute(cpu);
	}
	/* ARM shifterCarryOut can lag a newly established CPSR carry by one
	 * iteration. Allow one more probe than Thumb, then leave unconverged
	 * loops to the ordinary native chain. */
	for (unsigned attempt = 0; attempt < (block->arm ? 3u : 2u); ++attempt) {
		block->loopPure = true;
		bool complete = execute(cpu);
		/* Native proof clears loopPure after advancing complete repeats;
		 * external callbacks clear it before executing their side effects. */
		if (!complete || !block->loopPure || cpu->cycles >= cpu->nextEvent) return complete;
		/* A not-taken branch or a changed fetch mask needs the normal cache
		 * entry checks, not another call into this block's original code. */
		if ((uint32_t) cpu->gprs[ARM_PC] != block->start + (block->arm ? WORD_SIZE_ARM : WORD_SIZE_THUMB) ||
		    cpu->memory.activeRegion != block->region || cpu->memory.activeMask != block->mask ||
		    (block->arm ? cpu->memory.activeSeqCycles32 : cpu->memory.activeSeqCycles16) != block->seqCycles ||
		    cpu->prefetch[0] != _blockOpcode(block, 0) || cpu->prefetch[1] != _blockOpcode(block, 1)) return true;
	}
	block->loopPure = false;
	if (block->context) _codeEpoch(block->context);
	return execute(cpu);
}
#endif

bool RV32CanRun(const struct ARMCore* cpu) {
	const struct GBA* gba = (const struct GBA*) cpu->master;
	unsigned width = cpu->executionMode == MODE_ARM ? WORD_SIZE_ARM : WORD_SIZE_THUMB;
	if (!gba->memory.rom || !_isRomAddress(cpu->gprs[ARM_PC] - width)) return false;
	/* Frequent device callbacks can cost more than interpretation. Retry
	 * after a bounded number of event slices; never change guest behavior
	 * or permanently blacklist a PC whose data address may later change. */
	if (cpu->executionMode == MODE_ARM && _lastContext && _lastContext->cpu == cpu && _lastContext->deviceSlices) {
		--_lastContext->deviceSlices;
		return false;
	}
	return true;
}

void RV32RunLoop(struct ARMCore* cpu) {
	STAT_ADD(slices);
	struct GBA* gba = (struct GBA*) cpu->master;
	if (!RV32CanRun(cpu)) {
		ARMRunLoopLegacy(cpu);
		return;
	}
	struct RV32Context* context = _contextFor(cpu, gba);
	if (!context) {
		ARMRunLoopLegacy(cpu);
		return;
	}
	context->codeEpoch = 0; /* Events/host patches invalidate code and edge proofs. */
	while (cpu->cycles < cpu->nextEvent) {
		if (!RV32CanRun(cpu)) {
			ARMRunLoopLegacy(cpu);
			return;
		}
		uint32_t start = cpu->gprs[ARM_PC] - (cpu->executionMode == MODE_ARM ? WORD_SIZE_ARM : WORD_SIZE_THUMB);
		struct RV32Block* block = _findBlock(context, cpu, start);
		if (!block) {
			ARMRunLoopLegacy(cpu);
			return;
		}
#if RV32_NATIVE
		if (block->executable) {
			STAT_ADD(nativeCalls);
#ifdef MGBA_RV32_TRACE_POLL
			context->tracePC = 0;
			context->traceAllowed = !block->pollLoop && _traceTimingAllowed(cpu);
#endif
			/* Pure self-loop probes bypass direct edges. Leave their epoch
			 * lazy unless writable-code validation already required it. */
			if (!block->pollLoop) _codeEpoch(context);
			bool complete = block->pollLoop ? _executePollingLoop(cpu, block) :
				((bool (*)(struct ARMCore*)) (uintptr_t) block->nativeCode)(cpu);
			if (!complete) STAT_ADD(nativePartial);
			else if (cpu->cycles >= cpu->nextEvent) STAT_ADD(nativeEvent);
			else STAT_ADD(nativeReturn);
			if (!complete) {
				ARMRunLoopLegacy(cpu);
				return;
			}
			continue;
		}
#endif
		if (block->arm) {
			ARMRunLoopLegacy(cpu);
			return;
		}
		for (unsigned i = 0; i < block->length && cpu->cycles < cpu->nextEvent;) {
			if (_executeNativeBlock(cpu, block, i)) {
				i += block->spans[i].length;
				continue;
			}
			uint16_t opcode = block->opcodes[i];
			cpu->prefetch[0] = block->opcodes[i + 1];
			cpu->prefetch[1] = block->opcodes[i + 2];
			cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
			_thumbTable[opcode >> 6](cpu, opcode);
			context->codeEpoch = 0;
			break;
		}
	}
	cpu->irqh.processEvents(cpu);
}
