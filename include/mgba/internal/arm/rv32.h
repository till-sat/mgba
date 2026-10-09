/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ARM_RV32_H
#define ARM_RV32_H

#include <mgba/internal/arm/arm.h>

CXX_GUARD_START

/* Probe slice eligibility, consuming one pending ARM device-backoff slice.
 * Individual block entries perform the full cache checks. */
bool RV32CanRun(const struct ARMCore* cpu);
/* Execute one event-bounded slice using the RV32 ROM block runner. */
void RV32RunLoop(struct ARMCore* cpu);
void RV32Invalidate(struct ARMCore* cpu);
#ifdef MGBA_RV32_STATS
/* Diagnostic counters only; excluded from release code and timing claims. */
void RV32StatsReset(void);
void RV32StatsReport(void);
#endif

enum RV32CodeKind {
	RV32_CODE_THUMB = 1,
	RV32_CODE_ARM,
	RV32_CODE_THUMB_DISPATCH,
	RV32_CODE_ARM_DISPATCH,
	RV32_CODE_THUMB_BRANCH,
	RV32_CODE_ARM_BRANCH,
	RV32_CODE_DATA,
	RV32_CODE_THUMB_TRACE,
};

struct RV32CodeLocation {
	enum RV32CodeKind kind;
	uint32_t address; /* Guest block start, or shared data helper index. */
	uint32_t offset; /* Native byte offset within the block/helper. */
};

/* Read-only, allocation-free lookup for a stopped/interrupted CPU. Resolve
 * while sampling: a cache slot can belong to a different guest block later. */
bool RV32ResolveCode(const struct ARMCore* cpu, uintptr_t pc, struct RV32CodeLocation* location);

CXX_GUARD_END

#endif
