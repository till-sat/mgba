/* SPDX-License-Identifier: MPL-2.0 */
#ifndef GBA_NEXT_RV32_H
#define GBA_NEXT_RV32_H
#include <gba-next/core.h>

#ifndef GBN_RV32_SLOTS
#define GBN_RV32_SLOTS 16384u
#endif
#ifndef GBN_RV32_WAYS
#define GBN_RV32_WAYS 8u
#endif
#define GBN_RV32_SETS (GBN_RV32_SLOTS / GBN_RV32_WAYS)
/* C lookup and emitted indirect edges must use the same hash width. */
#define GBN_RV32_HASH_SHIFT (32u - __builtin_ctz(GBN_RV32_SLOTS))
#define GBN_RV32_INSNS 32u
#define GBN_RV32_WORDS 1024u
#ifndef GBN_RV32_CODE_MIB
#define GBN_RV32_CODE_MIB 16u
#endif
#define GBN_RV32_CODE_WORDS (GBN_RV32_CODE_MIB * 256u * 1024u)
#define GBN_RV32_SEGMENTS 32u
#define GBN_RV32_SEGMENT_WORDS (GBN_RV32_CODE_WORDS / GBN_RV32_SEGMENTS)

/* Caller-owned executable storage on bare-metal RV32IM + Zifencei. ROM bytes
 * must stay immutable between gbn_attach_rom calls. Keep the cache at the same
 * address while live; reinitialize it whenever its machine is reinitialized.
 * No mGBA state or callbacks are used. */
struct GbnRv32Block {
	uint32_t pc, generation, mask, target, timing;
	uint16_t instructions, words;
	uint32_t pipe[2];
	bool valid, branch, fallback, thumb, loop, arena_linked;
	uint32_t* code;
	uint32_t* chain; /* Common-register entry; NULL when invalid or unsupported. */
	struct GbnRv32Block *arena_prev, *arena_next;
};
struct GbnRv32 {
	struct Gbn* machine;
	uint64_t native_instructions, fallback_instructions;
	uint32_t compiled_blocks, cache_flushes, code_used; /* Segment flushes; arena cursor in words. */
	uint32_t code_segment;
	struct GbnRv32Block* segments[GBN_RV32_SEGMENTS];
	struct GbnRv32Block blocks[GBN_RV32_SLOTS];
	struct GbnRv32Block* lookup[GBN_RV32_SLOTS];
	uint8_t replacement[GBN_RV32_SETS];
	_Alignas(16) uint8_t source[GBN_RV32_SLOTS][4 * GBN_RV32_INSNS];
	_Alignas(16) uint32_t code[GBN_RV32_CODE_WORDS];
	uint32_t scratch[GBN_RV32_WORDS];
	uint64_t native_entries, loop_entries, loop_instructions;
	uint64_t fallback_kind[5]; /* BIOS, EWRAM, IWRAM, ROM ARM, ROM Thumb. */
	uint64_t chained_blocks;
	uint64_t merged_instructions; /* Exact fixed-point loops, never across an event. */
};

bool gbn_rv32_available(void);
void gbn_rv32_init(struct GbnRv32*, struct Gbn*);
/* Same event, failure and instruction-cap contract as gbn_run_batch. Native
 * ROM and RAM blocks retain low registers, flags, time and prefetch state across RAM
 * operations. Thumb supports common ALU/MUL, direct branches and single
 * memory operations, Thumb BL/BX and nonempty PUSH/POP in EWRAM/IWRAM. Guest SP low bits
 * survive word-aligned transfers; unaligned host RAM and mirrored-end spans
 * retain the interpreter path. PUSH may include LR; POP may include PC when
 * the destination stays in the current code region. Same-mode BX/POP and BL
 * suffix targets chain through the existing keyed cache; misses return to C.
 * BL halves retain independent LR, event and cap boundaries. ARM supports conditional ALU, MUL/MLA, immediate
 * operand shifts, B/BL, and single transfers with pre-index writeback or
 * immediate post-indexing. Ordinary ROM reads and signed loads are direct.
 * Stored IO, DMA count/control and pure open-bus registers have direct reads.
 * Open bus preserves DMA latch selection, byte/sign lanes and live RAM fetches;
 * stale prefetched IWRAM tails fall back before guest effects. Aligned DMA
 * source/destination word writes preserve the live transfer and update only
 * programmed addresses. Dynamic/side-effectful IO, save-port,
 * out-of-image and unaligned accesses retain the bus path. Direct
 * branches, fallthrough and supported indirect edges chain within one memory region and mode,
 * sharing resident registers until an instruction/event/slow-path boundary.
 * Read-only Thumb self-loops can merge complete fixed-point iterations after
 * comparing all modified registers, flags and prefetch state, while preserving
 * exact guest instruction/time counts and the next event/cap boundary.
 * RAM code bytes are validated at both C and chained entries. Writes overlapping the block and
 * its prefetched tail use the interpreter, preserving self modification.
 * Other instructions, BIOS and remaining IO use the independent interpreter. The
 * configurable set-associative cache has a fast lookup table (eight ways by
 * default). The packed code arena defaults to
 * 16 MiB and recycles 32 segments, invalidating only overwritten descriptors.
 * Counters are populated only in GBN_RV32_STATS=1 diagnostic builds. */
enum GbnStatus gbn_rv32_run_batch(struct GbnRv32*, uint32_t cap, uint32_t* executed);
#endif
