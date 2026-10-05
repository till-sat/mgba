/* Copyright (c) 2013-2014 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/isa-thumb.h>

#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/arm/emitter-thumb.h>

// Instruction definitions
// Beware pre-processor insanity

#define THUMB_ADDITION_S(M, N, D) \
	cpu->cpsr.flags = 0; \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D); \
	cpu->cpsr.c = ARM_CARRY_FROM(M, N, D); \
	cpu->cpsr.v = ARM_V_ADDITION(M, N, D);

#define THUMB_SUBTRACTION_S(M, N, D) \
	cpu->cpsr.flags = 0; \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D); \
	cpu->cpsr.c = ARM_BORROW_FROM(M, N, D); \
	cpu->cpsr.v = ARM_V_SUBTRACTION(M, N, D);

#define THUMB_SUBTRACTION_CARRY_S(M, N, D, C) \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D); \
	cpu->cpsr.c = ARM_BORROW_FROM_CARRY(M, N, D, C); \
	cpu->cpsr.v = ARM_V_SUBTRACTION(M, N, D);

#define THUMB_NEUTRAL_S(M, N, D) \
	cpu->cpsr.n = ARM_SIGN(D); \
	cpu->cpsr.z = !(D);

#define THUMB_ADDITION(D, M, N) \
	int n = N; \
	int m = M; \
	D = M + N; \
	THUMB_ADDITION_S(m, n, D)

#define THUMB_SUBTRACTION(D, M, N) \
	int n = N; \
	int m = M; \
	D = M - N; \
	THUMB_SUBTRACTION_S(m, n, D)

#define THUMB_PREFETCH_CYCLES (1 + cpu->memory.activeSeqCycles16)

#define THUMB_LOAD_POST_BODY \
	currentCycles += cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16;

#define THUMB_STORE_POST_BODY \
	currentCycles += cpu->memory.activeNonseqCycles16 - cpu->memory.activeSeqCycles16;

#define DEFINE_INSTRUCTION_THUMB(NAME, BODY) \
	static void _ThumbInstruction ## NAME (struct ARMCore* cpu, unsigned opcode) {  \
		int currentCycles = THUMB_PREFETCH_CYCLES; \
		BODY; \
		cpu->cycles += currentCycles; \
	}

#include "isa-thumb-instructions.inc"

const ThumbInstruction _thumbTable[0x400] = {
	DECLARE_THUMB_EMITTER_BLOCK(_ThumbInstruction)
};

#ifdef MGBA_RUNNER_THREADED
/* Keep opcode execution and dispatch in one native function. Both execution
 * paths expand the same instruction definitions, including memory callbacks
 * and cycle accounting. No ROM traces or immutable-code assumptions apply. */
void ARMRunThumbThreaded(struct ARMCore* cpu) {
	static const void* const labels[0x400] = {
		DECLARE_THUMB_EMITTER_BLOCK(&&_ThumbLabel)
	};
	const uint32_t* activeRegion = cpu->memory.activeRegion;
	uint32_t activeMask = cpu->memory.activeMask;
	uint32_t opcode, expectedPC;
	goto dispatch;

#undef DEFINE_INSTRUCTION_THUMB
#define DEFINE_INSTRUCTION_THUMB(NAME, BODY) \
	_ThumbLabel ## NAME: { \
		int currentCycles = THUMB_PREFETCH_CYCLES; \
		BODY; \
		cpu->cycles += currentCycles; \
		if ((uint32_t) cpu->gprs[ARM_PC] != expectedPC) { \
			activeRegion = cpu->memory.activeRegion; \
			activeMask = cpu->memory.activeMask; \
		} \
		goto dispatch; \
	}
#include "isa-thumb-instructions.inc"
#undef DEFINE_INSTRUCTION_THUMB

dispatch:
	if (cpu->cycles >= cpu->nextEvent) return;
	opcode = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	expectedPC = cpu->gprs[ARM_PC];
	LOAD_16(cpu->prefetch[1], expectedPC & activeMask, activeRegion);
	goto *labels[opcode >> 6];
}
#endif
