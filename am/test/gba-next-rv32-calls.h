/* SPDX-License-Identifier: MPL-2.0 */
/* Differential checks for native Thumb calls and indirect successor chains. */
/* Private differential coverage for native POP-PC successor chains. */
#ifndef EXPECT_RETURN_CHAINS
#define EXPECT_RETURN_CHAINS 1
#endif

static unsigned return_index(uint32_t pc) {
    return ((pc >> 1) * 0x9e3779b1u) >> GBN_RV32_HASH_SHIFT;
}
static void return_program(unsigned list) {
    put16(0, 0xb500 | list); /* PUSH low registers and LR. */
    put16(2, 0x3201);
    put16(4, 0xbd00 | list); /* POP the same list and PC. */
    put16(32, 0x3703);
    put16(34, 0xe7ed);      /* Branch back to offset zero. */
}
static void return_state(uint32_t base, unsigned bank, unsigned low, unsigned odd,
                         uint32_t now, unsigned flags) {
    machine.next_event = -1;
    memset(machine.events, 0, sizeof(machine.events));
    machine.now = now;
    for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
    machine.cpu.r[13] = (bank ? 0x03000800u : 0x02000800u) | low;
    machine.cpu.r[14] = (base + 32) | odd;
    machine.cpu.cpsr = flags << 28 | 0x0f00003f;
    machine.cpu.shifter_carry = flags & 1;
    CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
}
static void return_chains(void) {
    static const unsigned regions[] = {2, 3, 8, 10, 12};
    static const unsigned lists[] = {0, 1, 0x81, 0xff};
    static const unsigned caps[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 31, 256};
    unsigned before = comparisons;
    uint64_t warm_entries = 0, warm_chains = 0;
    for (unsigned region = 0; region < 5; ++region) {
        setup();
        for (unsigned list = 0; list < 4; ++list) {
            return_program(lists[list]);
            CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
            uint32_t base = regions[region] << 24;
            if (regions[region] < 8) {
                base += 0x1000;
                memcpy((regions[region] == 2 ? ewram : iwram) + 0x1000, rom, 128);
            }
            for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned wait = 0; wait < 2; ++wait) {
                machine.ewram_wait = wait ? 7 : 0;
                gbn_set_waitcnt(&machine, wait ? 0x4317 : 0);
                uint8_t* bytes = bank ? iwram : ewram;
                unsigned mask = bank ? GBN_IWRAM_SIZE - 1 : GBN_EWRAM_SIZE - 1;
                return_state(base, bank, 0, 1, 0, 15);
                compare_stack(512, 0x400000, bytes, mask, 0x7c0, 128);
                return_state(base, bank, 0, 0, 0, 15);
                uint64_t entries = backend.native_entries, chains = backend.chained_blocks;
                compare_stack(256, 0x400001, bytes, mask, 0x7c0, 128);
                warm_entries += backend.native_entries - entries;
                warm_chains += backend.chained_blocks - chains;
                if (EXPECT_RETURN_CHAINS) {
                    CHECK(backend.native_entries == entries + 1);
                    CHECK(backend.chained_blocks > chains + 80);
                }
                for (unsigned odd = 0; odd < 2; ++odd)
                for (unsigned low_case = 0; low_case < 2; ++low_case)
                for (unsigned wrap = 0; wrap < 2; ++wrap) {
                    unsigned low = low_case ? 3 : 0;
                    uint32_t origin = wrap ? 0xffffffe0u : 0;
                    return_state(base, bank, low, odd, origin, 15);
                    /* Derive the actual event cuts surrounding each of two
                     * POP operations, including their entire refill cost. */
                    struct Gbn initial = machine, clock = machine;
                    uint8_t saved[128];
                    memcpy(saved, bytes + 0x7c0, sizeof(saved));
                    uint32_t distances[14] = {0, 1};
                    unsigned n = 2;
                    for (unsigned step = 1; step <= 8; ++step) {
                        uint32_t done;
                        CHECK(gbn_run_batch(&clock, 1, &done) == GBN_STEP && done == 1);
                        if (step == 2 || step == 3 || step == 7 || step == 8) {
                            uint32_t elapsed = clock.now - origin;
                            distances[n++] = elapsed - 1;
                            distances[n++] = elapsed;
                            distances[n++] = elapsed + 1;
                        }
                    }
                    memcpy(bytes + 0x7c0, saved, sizeof(saved));
                    CHECK(n == 14);
                    for (unsigned d = 0; d < n; ++d)
                    for (unsigned cap = 0; cap < sizeof(caps) / sizeof(*caps); ++cap) {
                        machine = initial;
                        CHECK(gbn_schedule(&machine, 0, distances[d], 0));
                        compare_stack(caps[cap], 0x410000 | region << 12 | list << 8 | d,
                                      bytes, mask, 0x7c0, 128);
                    }
                }
            }
        }
    }
    printf("PASS native indirect return chains: %u comparisons; warm_entries=%" PRIu64
           "; warm_chains=%" PRIu64 "; POP lists, RAM/ROM, stack banks, target/SP low bits, NZCV, refill event/cap boundaries and clock wrap\n",
           comparisons - before, warm_entries, warm_chains);
}

static void return_cache_changes(void) {
    unsigned before = comparisons;
    setup(); return_program(0x81);
    CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
    return_state(0x08000000, 1, 0, 1, 0, 15);
    compare_stack(512, 0x420000, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    /* A front-table conflict must safely redispatch to the retained way. */
    backend.lookup[return_index(0x08000020)] = NULL;
    return_state(0x08000000, 1, 0, 1, 0, 15);
    uint64_t entries = backend.native_entries;
    compare_stack(256, 0x420001, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    CHECK(backend.native_entries > entries + 1);
    /* Source recompilation must reject an old target's timing/generation. */
    gbn_set_waitcnt(&machine, 0x4317); machine.ewram_wait = 7;
    return_state(0x08000000, 1, 3, 0, 0xffffffe0, 15);
    compare_stack(256, 0x420002, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    put16(32, 0x3707);
    CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
    return_state(0x08000000, 1, 0, 1, 0, 15);
    compare_stack(256, 0x420003, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);

    /* Keep the source in segment one while recycling the target's segment.
     * The source remains executable and must reject a NULL chain pointer. */
    setup(); return_program(0);
    put16(256, 0x3001); put16(258, 0xbe00);
    CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
    return_state(0x08000000, 1, 0, 1, 0, 15);
    CHECK(gbn_enter_thumb(&machine, 0x08000020) == GBN_STEP);
    compare(2, 0x420004);
    struct GbnRv32Block* victim = backend.lookup[return_index(0x08000020)];
    CHECK(victim && victim->valid && victim->chain);
    backend.code_used = GBN_RV32_SEGMENT_WORDS;
    backend.code_segment = 1;
    return_state(0x08000000, 1, 0, 1, 0, 15);
    compare_stack(256, 0x420005, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    struct GbnRv32Block* survivor = backend.lookup[return_index(0x08000000)];
    CHECK(survivor && survivor->valid && survivor->chain);
    backend.code_used = GBN_RV32_CODE_WORDS - 1;
    backend.code_segment = GBN_RV32_SEGMENTS - 1;
    CHECK(gbn_enter_thumb(&machine, 0x08000100) == GBN_STEP);
    compare(1, 0x420006);
    CHECK(!victim->valid && !victim->chain && survivor->valid && survivor->chain);
    return_state(0x08000000, 1, 0, 1, 0, 15);
    compare_stack(256, 0x420007, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);

    /* Dynamic targets in byte/halfword-aligned host RAM still validate the
     * live code snapshot; the other RAM bank provides an aligned stack. */
    for (unsigned region = 2; region <= 3; ++region)
    for (unsigned alignment = 0; alignment < 4; ++alignment) {
        setup(); return_program(0x81);
        gbn_init(&machine, ewram + (region == 2 ? alignment : 0),
                            iwram + (region == 3 ? alignment : 0));
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        uint8_t* code = region == 2 ? machine.ewram : machine.iwram;
        memcpy(code + 0x1000, rom, 128);
        unsigned bank = region == 2;
        uint8_t* bytes = bank ? iwram : ewram;
        unsigned mask = bank ? GBN_IWRAM_SIZE - 1 : GBN_EWRAM_SIZE - 1;
        uint32_t base = region << 24 | 0x1000;
        return_state(base, bank, 0, 1, 0, 15);
        compare_stack(256, 0x420100 | region << 4 | alignment, bytes, mask, 0x7c0, 128);
        code[0x1020] = 7; /* Same target PC, changed RAM instruction. */
        return_state(base, bank, 0, 1, 0, 15);
        uint32_t compiled = backend.compiled_blocks;
        compare_stack(256, 0x420200 | region << 4 | alignment, bytes, mask, 0x7c0, 128);
        CHECK(backend.compiled_blocks > compiled);
    }
    printf("PASS indirect return cache changes: %u comparisons; front-table miss, timing/generation, surviving source with recycled target, all host RAM alignments and live target modification\n", comparisons - before);
}
/* Check BL as two independently observable instructions, and native pair
 * chaining when both halves execute within the same block. */
#ifndef EXPECT_NATIVE_CALLS
#define EXPECT_NATIVE_CALLS 1
#endif
static void call_program(void) {
    put16(0, 0xf000); put16(2, 0xf80e); /* BL +32; LR becomes +5. */
    put16(4, 0x3703); put16(6, 0xe7fb);
    put16(32, 0x3201); put16(34, 0xe7ef); /* Direct return to +4. */
}
static void call_encodings(void) {
    static const unsigned lows[] = {0, 1, 1023, 2047};
    unsigned before = comparisons;
    setup();
    for (unsigned region = 2; region <= 3; ++region)
    for (unsigned upper = 0; upper < 2048; ++upper)
    for (unsigned low = 0; low < 4; ++low) {
        uint8_t* bytes = region == 2 ? ewram : iwram;
        put16(0, 0xf000 | upper); put16(2, 0xf800 | lows[low]);
        memcpy(bytes + 0x1000, rom, 128);
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        machine.cpu.cpsr = (random_word() & ~63u) | 63;
        machine.now = upper & 1 ? 0xfffffff0 : 0;
        gbn_set_waitcnt(&machine, (uint16_t) random_word());
        uint32_t base = region << 24 | 0x801000;
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
        uint64_t native = backend.native_instructions;
        compare(2, 0x430000 | upper << 2 | low);
        if (EXPECT_NATIVE_CALLS) CHECK(backend.native_instructions == native + 2);
    }
    /* Every prefix also executes alone, retaining its intermediate LR when
     * the budget ends before a separately dispatched suffix. */
    setup();
    for (unsigned upper = 0; upper < 2048; ++upper) {
        put16(0, 0xf000 | upper);
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        machine.cpu.cpsr = (random_word() & ~63u) | 63;
        CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
        uint64_t native = backend.native_instructions;
        compare(1, 0x440000 | upper);
        if (EXPECT_NATIVE_CALLS) CHECK(backend.native_instructions == native + 1);
    }
    printf("PASS BL encodings: %u comparisons; all signed upper offsets, low-offset edges, mirrored RAM targets and independently committed prefix LR\n", comparisons - before);
}
static void call_boundaries(void) {
    static const unsigned regions[] = {2, 3, 8, 10, 12};
    static const unsigned caps[] = {0, 1, 2, 3, 4, 5, 6, 7, 31, 32, 33, 256};
    unsigned before = comparisons;
    for (unsigned region = 0; region < 5; ++region) {
        setup(); call_program();
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        uint32_t base = regions[region] << 24;
        if (regions[region] < 8) {
            base += 0x1000;
            memcpy((regions[region] == 2 ? ewram : iwram) + 0x1000, rom, 128);
        }
        for (unsigned wait = 0; wait < 2; ++wait) {
            machine.ewram_wait = wait ? 7 : 0;
            gbn_set_waitcnt(&machine, wait ? 0x4317 : 0);
            machine.cpu.cpsr = 0xf000003f;
            CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
            compare(512, 0x450000);
            CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
            uint64_t entries = backend.native_entries, chains = backend.chained_blocks;
            compare(256, 0x450001);
            if (EXPECT_NATIVE_CALLS) {
                CHECK(backend.native_entries == entries + 1);
                CHECK(backend.chained_blocks > chains + 80);
            }
            for (unsigned distance = 0; distance < 128; ++distance)
            for (unsigned cap = 0; cap < sizeof(caps) / sizeof(*caps); ++cap) {
                machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
                machine.now = distance & 1 ? 0xfffffff0u : 0;
                machine.cpu.cpsr = (distance & 15) << 28 | 0x0f00003f;
                machine.cpu.r[14] = random_word();
                CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
                CHECK(gbn_schedule(&machine, 0, distance, 0));
                compare(caps[cap], 0x460000 | region << 12 | distance);
            }
            machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
            /* An event may alter LR between halves. Execute the suffix from
             * its own entry with independent target values, including faults. */
            static const int offsets[] = {-64, -1, 0, 1, 32, 2048, 4095};
            for (unsigned target = 0; target < sizeof(offsets) / sizeof(*offsets); ++target) {
                machine.cpu.r[14] = base + offsets[target];
                CHECK(gbn_enter_thumb(&machine, base + 2) == GBN_STEP);
                compare(1, 0x470000 | target);
            }
        }
    }
    printf("PASS BL execution boundaries: %u comparisons; warm chains, caps/events between halves, clock wrap, WAITCNT, NZCV and independently entered suffixes with changed LR\n", comparisons - before);
}
/* Runtime target guards and repeated polymorphic native successors. */
static void bx_encodings(void) {
    static const unsigned regions[] = {2, 3, 8, 10, 12};
    unsigned before = comparisons;
    for (unsigned region = 0; region < 5; ++region) {
        setup();
        uint32_t base = regions[region] << 24 | (regions[region] < 8 ? 0x1000 : 0);
        const uint32_t targets[] = {base | 1, base + 32, base + 33, base + 35,
            base + 4095, base + 4097, base | 0x00f00021u, 0x08000021,
            0x02001021, 0x03001021, 0x04000001, 0x00000001};
        for (unsigned code = 0x4700; code < 0x4800; ++code) {
            put16(0, code);
            CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
            if (regions[region] < 8)
                memcpy((regions[region] == 2 ? ewram : iwram) + 0x1000, rom, 128);
            for (unsigned wait = 0; wait < 2; ++wait)
            for (unsigned t = 0; t < sizeof(targets) / sizeof(*targets); ++t) {
                unsigned rs = code >> 3 & 15;
                machine.cpu.cpsr = (random_word() & ~63u) | 63;
                machine.now = t & 1 ? 0xfffffff0u : 0;
                gbn_set_waitcnt(&machine, wait ? 0x4317 : 0);
                for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
                if (rs != 15) machine.cpu.r[rs] = targets[t];
                CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
                uint32_t target = rs == 15 ? base + 4 : targets[t];
                bool valid = code < 0x4780 && (target & 1) && target >> 24 == regions[region] &&
                    (regions[region] < 8 || (target & (GBN_ROM_MAX_SIZE - 1)) < sizeof(rom));
                uint64_t native = backend.native_instructions;
                compare(1, 0x480000 | region << 12 | code);
                CHECK(backend.native_instructions == native + valid);
            }
        }
    }
    printf("PASS BX encodings: %u comparisons; every register/low encoding, BLX rejection, target mode/bank/bounds, ARM bit-1 fetches, RAM mirrors, WAITCNT and clock wrap\n", comparisons - before);
}
static void bx_calls(void) {
    static const unsigned regions[] = {2, 3, 8, 10, 12};
    static const unsigned caps[] = {0, 1, 2, 3, 4, 5, 6, 7, 31, 32, 33, 256};
    unsigned before = comparisons;
    for (unsigned region = 0; region < 5; ++region) {
        setup(); call_program(); put16(34, 0x4770); /* BX LR return from BL. */
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        uint32_t base = regions[region] << 24;
        if (regions[region] < 8) {
            base += 0x1000;
            memcpy((regions[region] == 2 ? ewram : iwram) + 0x1000, rom, 128);
        }
        for (unsigned wait = 0; wait < 2; ++wait) {
            machine.ewram_wait = wait ? 7 : 0;
            gbn_set_waitcnt(&machine, wait ? 0x4317 : 0);
            machine.cpu.cpsr = 0xf000003f;
            CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare(512, 0x490000);
            CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
            uint64_t entries = backend.native_entries, chains = backend.chained_blocks;
            compare(256, 0x490001);
            CHECK(backend.native_entries == entries + 1 && backend.chained_blocks > chains + 80);
            for (unsigned distance = 0; distance < 128; ++distance)
            for (unsigned cap = 0; cap < sizeof(caps) / sizeof(*caps); ++cap) {
                machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
                machine.now = distance & 1 ? 0xfffffff0u : 0;
                machine.cpu.cpsr = (distance & 15) << 28 | 0x0f00003f;
                machine.cpu.r[14] = random_word();
                CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
                CHECK(gbn_schedule(&machine, 0, distance, 0));
                compare(caps[cap], 0x4a0000 | region << 12 | distance);
            }
            machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
        }
    }
    /* The same source must select different already warm targets on each
     * entry, rather than retaining a compile-time register value. */
    setup();
    put16(0, 0x4700); put16(32, 0x3101); put16(34, 0xe7ed);
    put16(64, 0x3103); put16(66, 0xe7dd);
    CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
    for (unsigned trial = 0; trial < 10; ++trial) {
        machine.cpu.r[0] = trial & 1 ? 0x08000041 : 0x08000021;
        CHECK(gbn_enter_thumb(&machine, 0x08000000) == GBN_STEP);
        uint64_t entries = backend.native_entries;
        compare(256, 0x4b0000 | trial);
        if (trial >= 2) CHECK(backend.native_entries == entries + 1);
    }
    printf("PASS native BL/BX calls: %u comparisons; resident calls/returns, partial prefixes, all cap/event cuts, NZCV, clock wrap and changing warm runtime targets\n", comparisons - before);
}

static void indirect_wrong_descriptor(void) {
    unsigned before = comparisons;
    setup(); return_program(0);
    CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
    return_state(0x08000000, 1, 0, 1, 0, 15);
    compare_stack(512, 0x4c0000, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    /* A non-NULL live descriptor with the wrong PC must also miss. */
    backend.lookup[return_index(0x08000020)] = backend.lookup[return_index(0x08000000)];
    return_state(0x08000000, 1, 0, 1, 0, 15);
    uint64_t entries = backend.native_entries;
    compare_stack(256, 0x4c0001, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    CHECK(backend.native_entries > entries + 1);

    /* An ARM descriptor can occupy the same PC slot as a Thumb successor.
     * Its live chain is rejected by the mode bit in the existing timing key. */
    setup(); return_program(0); put32(32, 0xe1a00000);
    CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
    CHECK(gbn_enter_arm(&machine, 0x08000020) == GBN_STEP);
    compare(1, 0x4c0002);
    struct GbnRv32Block* arm = backend.lookup[return_index(0x08000020)];
    CHECK(arm && arm->valid && arm->chain && !arm->thumb);
    return_state(0x08000000, 1, 0, 1, 0, 15);
    uint64_t native = backend.native_instructions;
    compare_stack(4, 0x4c0003, iwram, GBN_IWRAM_SIZE - 1, 0x7c0, 128);
    CHECK(backend.native_instructions == native + 4);
    printf("PASS indirect descriptor PC and ARM/Thumb mode mismatches: %u comparisons\n", comparisons - before);
}
