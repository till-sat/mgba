/* SPDX-License-Identifier: MPL-2.0 */
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/arm/macros.h>

enum { MAX_POLL_INSTRUCTIONS = 16 };

/* Exactly the normal Thumb pipeline, without processing events here. The
 * caller stops at the first instruction boundary reaching nextEvent. Only
 * instructions checked below can execute through this helper. */
static void _step(struct ARMCore* cpu) {
	uint32_t opcode = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
	LOAD_16(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
	_thumbTable[opcode >> 6](cpu, opcode);
}

uint32_t GBASkipIdleLoop(struct GBA* gba) {
	struct ARMCore* cpu = gba->cpu;
	if (cpu->executionMode != MODE_THUMB || cpu->halted || gba->cpuBlocked ||
	    gba->earlyExit || gba->debugger || gba->idleOptimization != IDLE_LOOP_REMOVE ||
	    cpu->memory.load16 != GBALoad16 ||
	    cpu->cycles >= cpu->nextEvent) {
		return 0;
	}
	uint32_t start = gba->lastJump;
	uint32_t pc = cpu->gprs[ARM_PC] - WORD_SIZE_THUMB;
	if ((start & 1) || pc - start >= MAX_POLL_INSTRUCTIONS * 2) return 0;
	unsigned region = start >> BASE_OFFSET;
	if (region != GBA_REGION_IWRAM && region != GBA_REGION_EWRAM &&
	    (region < GBA_REGION_ROM0 || region > GBA_REGION_ROM2_EX)) return 0;
	if (region != (unsigned) gba->memory.activeRegion || (pc >> BASE_OFFSET) != region) return 0;
	uint32_t offset = start & cpu->memory.activeMask;
	uint32_t limit = cpu->memory.activeMask + 2;
	if (region >= GBA_REGION_ROM0 && limit > gba->memory.romSize) limit = gba->memory.romSize;
	if (offset >= limit || limit - offset < (pc - start) + 4) return 0;

	unsigned written = 0, addressRegisters = 0, loads = 0, length = 0;
	for (unsigned i = 0; i < MAX_POLL_INSTRUCTIONS && offset + i * 2 + 6 <= limit; ++i) {
		uint16_t op;
		LOAD_16(op, offset + i * 2, cpu->memory.activeRegion);
		if ((op & 0xF800) == 0x8800 || (op & 0xFE00) == 0x5A00) {
			unsigned base = (op >> 3) & 7;
			uint32_t address = cpu->gprs[base];
			addressRegisters |= 1U << base;
			if ((op & 0xF800) == 0x8800) {
				address += ((op >> 6) & 31) * 2;
			} else {
				unsigned index = (op >> 6) & 7;
				addressRegisters |= 1U << index;
				address += cpu->gprs[index];
			}
			/* These reads only clear haltPending. Their values cannot change
			 * until an event is processed; timers and other MMIO are excluded. */
			if (address != GBA_BASE_IO + GBA_REG_VCOUNT &&
			    address != GBA_BASE_IO + GBA_REG_DISPSTAT) return 0;
			written |= 1U << (op & 7);
			++loads;
		} else if (op < 0x2000) {
			written |= 1U << (op & 7); /* Immediate shifts, add/sub. */
		} else if (op < 0x4000) {
			if ((op & 0xF800) != 0x2800) written |= 1U << ((op >> 8) & 7);
		} else if ((op & 0xFC00) == 0x4000) {
			unsigned alu = (op >> 6) & 15;
			if (alu == 13) return 0; /* Exclude variable-time multiply. */
			if (alu != 8 && alu != 10 && alu != 11) written |= 1U << (op & 7);
		} else if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
			int displacement = (int8_t) op * 2;
			if ((int) (i * 2 + 4) + displacement != 0) return 0;
			length = i + 1;
			break;
		} else {
			return 0; /* Stores, calls, other memory reads, and internal branches. */
		}
	}
	if (!length || !loads || (written & addressRegisters) || pc - start >= length * 2) return 0;
	/* DMA or a debugger may have replaced already-prefetched RAM code. */
	uint16_t op0, op1;
	LOAD_16(op0, pc & cpu->memory.activeMask, cpu->memory.activeRegion);
	LOAD_16(op1, (pc + 2) & cpu->memory.activeMask, cpu->memory.activeRegion);
	if (cpu->prefetch[0] != op0 || cpu->prefetch[1] != op1) return 0;

	/* Finish the current iteration, then use the real instruction handlers to
	 * establish a fixed point. No event, memory write, or external callback can
	 * occur in an accepted loop. A counter or other evolving state rejects it. */
	while ((uint32_t) cpu->gprs[ARM_PC] != start + 2) {
		if (cpu->cycles >= cpu->nextEvent) return 0;
		_step(cpu);
		if ((uint32_t) cpu->gprs[ARM_PC] - (start + 2) >= length * 2) return 0;
	}
	for (unsigned trial = 0; trial < 2; ++trial) {
		struct ARMRegisterFile before = cpu->regs;
		uint32_t prefetchedPc = gba->memory.lastPrefetchedPc;
		bool haltPending = gba->haltPending;
		int32_t cycles = cpu->cycles;
		for (unsigned i = 0; i < length; ++i) {
			if (cpu->cycles >= cpu->nextEvent) return 0;
			_step(cpu);
		}
		if ((uint32_t) cpu->gprs[ARM_PC] != start + 2 || cpu->cycles >= cpu->nextEvent) return 0;
		if (memcmp(&before, &cpu->regs, sizeof(before)) ||
		    prefetchedPc != gba->memory.lastPrefetchedPc || haltPending != gba->haltPending) continue;
		int32_t period = cpu->cycles - cycles;
		if (period <= 0) return 0;
		/* Stop strictly before the deadline; the normal interpreter executes
		 * the final partial iteration and delivers the event at the same PC. */
		uint32_t remaining = (uint32_t) cpu->nextEvent - (uint32_t) cpu->cycles - 1;
		uint32_t skipped = remaining / (unsigned) period * (unsigned) period;
		cpu->cycles += skipped;
		return skipped;
	}
	return 0;
}
