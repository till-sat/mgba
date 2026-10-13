/* SPDX-License-Identifier: MPL-2.0 */
/* Exercise the positive reuse path, then force stale-proof collisions at
 * epoch/tag wrap. The interpreter independently checks guest effects. */
static void proof_epochs(void) {
    unsigned before = comparisons;
    for (unsigned region = 2; region <= 3; ++region) {
        setup();
        put16(0, 0x3001); put16(2, 0xe07d);
        put16(256, 0x3101); put16(258, 0xe77d);
        uint8_t* bytes = (region == 2 ? ewram : iwram) + 0x1000;
        uint32_t base = region << 24 | 0x1000;
        memcpy(bytes, rom, 512);
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare(64, 0x400000 | region);
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP);
        uint32_t epoch = backend.next_epoch + 1;
        uint64_t entries = backend.native_entries, chains = backend.chained_blocks;
        register uint32_t saved_epoch __asm__("s9") = 0x19ade312;
        register uint32_t saved_context __asm__("s10") = 0xa751c90f;
        __asm__ volatile("" : "+r"(saved_epoch), "+r"(saved_context));
        register uint32_t saved_s1 __asm__("s1") = 0x73401ab9;
        register uint32_t saved_s4 __asm__("s4") = 0xa71269b3;
        register uint32_t saved_s7 __asm__("s7") = 0x28741ac9;
        register uint32_t saved_s8 __asm__("s8") = 0xafde3412;
        register uint32_t saved_s11 __asm__("s11") = 0x491ea9bc;
        uintptr_t before_gp, before_tp, after_gp, after_tp;
        __asm__ volatile("mv %0,gp; mv %1,tp" : "=r"(before_gp), "=r"(before_tp));
        __asm__ volatile("" : "+r"(saved_s1), "+r"(saved_s4), "+r"(saved_s7), "+r"(saved_s8), "+r"(saved_s11));
        compare(64, 0x400010 | region);
        __asm__ volatile("" : "+r"(saved_s1), "+r"(saved_s4), "+r"(saved_s7), "+r"(saved_s8), "+r"(saved_s11));
        __asm__ volatile("mv %0,gp; mv %1,tp" : "=r"(after_gp), "=r"(after_tp));
        CHECK(saved_s1 == 0x73401ab9 && saved_s4 == 0xa71269b3 && saved_s7 == 0x28741ac9
              && saved_s8 == 0xafde3412 && saved_s11 == 0x491ea9bc);
        CHECK(before_gp == after_gp && before_tp == after_tp);
        __asm__ volatile("" : "+r"(saved_epoch), "+r"(saved_context));
        CHECK(saved_epoch == 0x19ade312 && saved_context == 0xa751c90f);
        CHECK(backend.native_entries == entries + 1 && backend.chained_blocks > chains + 15);
        struct GbnRv32Block* a = backend.lookup[((base >> 1) * 0x9e3779b1u) >> GBN_RV32_HASH_SHIFT];
        struct GbnRv32Block* b = backend.lookup[(((base + 256) >> 1) * 0x9e3779b1u) >> GBN_RV32_HASH_SHIFT];
        CHECK(a && b && a->checked_epoch == epoch && b->checked_epoch == epoch);
        CHECK(a->context_tag && a->context_tag == b->context_tag);
        /* Without the wrap clear, b's forged old epoch 1 would skip the byte
         * check and execute its stale ADD #1 instead of the new ADD #3. */
        b->checked_epoch = 1; backend.next_epoch = UINT32_MAX;
        bytes[256] = 3;
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare(64, 0x400020 | region);
        CHECK(backend.next_epoch < 1000);

        /* Writes to the other RAM bank cannot modify this chain's code.
         * Reuse remains valid while the independent memory oracle observes
         * the changing store data. */
        put16(0, 0x601a); put16(2, 0xe07d); /* STR r2,[r3] */
        memcpy(bytes, rom, 512);
        machine.cpu.r[2] = 0x1234abcd;
        machine.cpu.r[3] = (region == 2 ? 3 : 2) << 24 | 0x4000;
        uint8_t* data = (region == 2 ? iwram : ewram) + 0x4000;
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare_memory(64, 0x400030 | region, data);
        machine.cpu.r[2] = 0x9876fedc;
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); epoch = backend.next_epoch + 1;
        compare_memory(64, 0x400040 | region, data);
        a = backend.lookup[((base >> 1) * 0x9e3779b1u) >> GBN_RV32_HASH_SHIFT];
        b = backend.lookup[(((base + 256) >> 1) * 0x9e3779b1u) >> GBN_RV32_HASH_SHIFT];
        CHECK(a->checked_epoch == epoch && b->checked_epoch == epoch);

        /* A new immutable key at tag wrap must retire all old descriptors,
         * including their live chain pointers; packed arena links survive. */
        backend.next_context_tag = UINT32_MAX;
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare_memory(1, 0x400050 | region, data);
        CHECK(backend.next_context_tag == 1 && a->valid && !b->valid && !b->chain);
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare_memory(64, 0x400060 | region, data);
        backend.code_used = GBN_RV32_CODE_WORDS - 1; backend.code_segment = GBN_RV32_SEGMENTS - 1;
        gbn_set_waitcnt(&machine, 0x4317);
        CHECK(gbn_enter_thumb(&machine, base) == GBN_STEP); compare_memory(64, 0x400070 | region, data);
    }
    puts("PASS native proof reuse, foreign-bank stores, RAM epoch wrap, context tag wrap, arena links and s9/s10 ABI");
    printf("Native proof state comparisons: %u\n", comparisons - before);
}

static void prefetch_boundaries(void) {
	static const uint32_t distances[] = {0, 1, 2, 13, 14, 15, 16, 17, 0xfffffff0, 0xffffffff};
	static const unsigned waits[] = {0, 1, 2, 3, 7, 15, 31, 127, 255};
	unsigned before = comparisons;
	setup();
	for (unsigned thumb = 0; thumb < 2; ++thumb) {
		if (thumb) put16(0, 0x6808); else put32(0, 0xe5910000); /* LDR r0,[r1] */
		CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
		for (unsigned region = 8; region <= 12; region += 2)
		for (unsigned seq = 0; seq < 2; ++seq) for (unsigned nonseq = 0; nonseq < 4; ++nonseq)
		for (unsigned distance = 0; distance < sizeof(distances) / sizeof(*distances); ++distance)
		for (unsigned w = 0; w < sizeof(waits) / sizeof(*waits); ++w) for (unsigned bank = 2; bank <= 3; ++bank) {
			machine.ewram_wait = waits[w]; machine.now = 0xfffffff0;
			machine.cpu.r[1] = bank << 24 | 0x4000;
			unsigned shift = region == 8 ? 2 : region == 10 ? 5 : 8;
			gbn_set_waitcnt(&machine, 0x4000 | nonseq << shift | seq << (shift + 2));
			CHECK((thumb ? gbn_enter_thumb(&machine, region << 24) : gbn_enter_arm(&machine, region << 24)) == GBN_STEP);
			machine.prefetched_pc = machine.cpu.pc + (thumb ? 4 : 8) + distances[distance];
			uint64_t native = backend.native_instructions;
			compare(1, 0x410000 | region << 12 | distance << 8 | w << 4 | bank);
			CHECK(backend.native_instructions == native + 1);
		}
	}
	printf("PASS native prefetch short/fitting/saturated waits, odd/stale/wrapped distances, all ROM wait windows, ARM/Thumb and clock wrap: %u comparisons\n", comparisons - before);
}

/* Multiple taken/untaken edges in one compiled ARM fallthrough block. */
static void arm_trace_boundaries(void) {
    static const uint32_t caps[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,15,16,17,31,32,33,64,127,256};
    static const uint32_t multipliers[] = {0,1,255,256,65535,65536,0xffffff,0x1000000,
        0xffffffff,0xffffff00,0xfffffeff,0xffff0000,0xfffeffff,0xff000000,0xfeffffff,0x80000000};
    unsigned before = comparisons;
    for (unsigned bank = 0; bank < 3; ++bank) for (unsigned align = 0; align < 4; ++align) {
        setup();
        uint32_t code[] = {0xe2888001,0xe1580009,0x0a000002,0xe00a0b98,0x1afffffa,
            0xe38cc001,0x2b000002,0xe1a0e00a,0xe2499001,0xeafffff5,
            0xe3ccc8ff,0xe29bb001,0xbafffff8,0xe580a000,0xe5907000,0xe2822001,0xeaffffee};
        for (unsigned i = 0; i < sizeof(code) / 4; ++i) put32(i * 4, code[i]);
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        machine.ewram = ewram + align; machine.iwram = iwram + align;
        unsigned region = bank ? bank + 1 : 8;
        uint32_t base = region << 24 | (bank ? 0x1000 : 0);
        if (bank) memcpy((region == 2 ? machine.ewram : machine.iwram) + 0x1000, rom, sizeof(code));
        uint8_t* changed = (region == 2 ? machine.iwram : machine.ewram) + 0x4000;
        uint32_t data = (region == 2 ? 3u : 2u) << 24 | 0x4000;
        for (unsigned wrap = 0; wrap < 2; ++wrap) for (unsigned distance = 0; distance < 42; ++distance)
        for (unsigned k = 0; k < sizeof(caps) / sizeof(*caps); ++k) {
            machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
            machine.now = wrap ? 0xfffffff0 : 0;
            for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
            machine.cpu.r[0] = data; machine.cpu.r[8] &= 3; machine.cpu.r[9] = 3 + (random_word() & 7);
            machine.cpu.r[11] = multipliers[(distance + k) & 15];
            machine.cpu.cpsr = (random_word() & ~63u) | 31;
            gbn_set_waitcnt(&machine, (uint16_t) random_word());
            CHECK(gbn_enter_arm(&machine, base) == GBN_STEP);
            if (distance < 41) CHECK(gbn_schedule(&machine, 0, distance, 0));
            compare_memory(caps[k], 0x420000 | bank << 16 | align << 14 | wrap << 13 | distance << 5 | k, changed);
        }
    }
    printf("PASS ARM fallthrough traces: %u comparisons; multiple forward/backward branches, conditional BL/LR, live high registers, memory, all cap/event cuts, RAM alignment, WAITCNT and clock wrap\n", comparisons - before);
}

static void arm_internal_edges(void) {
    static const unsigned caps[]={0,1,2,3,5,6,9,16,33,64,127};
    static const unsigned distances[]={0,1,2,7,19,63,4096};
    unsigned before=comparisons;
    for(unsigned graph=0;graph<2;++graph) for(unsigned cond=0;cond<14;++cond) {
        setup();
        uint32_t code[]={0xe2800001,0xeb000003,0xe2911001,0xe0412000,
            0xe1b03062,0xe2844001,0xe2b55000,0x1afffffa,
            0xe2866001,0xe2577001,0x1afffff7,0xeafffff3};
        code[1]=(cond<<28)|0x0b000003; /* Conditional BL to source index 6. */
        if(graph) {code[1]=(cond<<28)|0x0a000005; code[7]=0xe1a0c00e; code[10]=(cond<<28)|0x0afffff6;}
        for(unsigned i=0;i<sizeof(code)/4;++i) put32(4*i,code[i]);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        memcpy(ewram,rom,sizeof(code));memcpy(iwram,rom,sizeof(code));
        for(unsigned bank=0;bank<3;++bank) for(unsigned flags=0;flags<16;++flags)
        for(unsigned wrap=0;wrap<2;++wrap) for(unsigned c=0;c<sizeof(caps)/sizeof(*caps);++c)
        for(unsigned d=0;d<sizeof(distances)/sizeof(*distances);++d) {
            memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
            machine.event_head=-1;memset(machine.event_prev,0xff,sizeof(machine.event_prev));
            memset(machine.event_next,0xff,sizeof(machine.event_next));
            machine.now=wrap?0xfffffff0:0;
            for(unsigned r=0;r<15;++r) machine.cpu.r[r]=random_word();
            machine.cpu.r[7]=1+(d&7);machine.cpu.cpsr=(flags<<28)|0x0a00001f;
            machine.cpu.shifter_carry=c&1;
            gbn_set_waitcnt(&machine,(uint16_t)random_word());
            unsigned region=bank?bank+1:8;
            CHECK(gbn_enter_arm(&machine,region<<24)==GBN_STEP);
            CHECK(gbn_schedule(&machine,0,distances[d],0));
            compare(caps[c],0x75000000|graph<<24|cond<<20|bank<<18|flags<<14|wrap<<13|c<<8|d);
        }
    }
    printf("PASS internal ARM branch graphs: %u comparisons; all conditions/NZCV, forward BL, backward interior targets, cap/event cuts, reserved PSR, three banks and clock wrap\n",comparisons-before);
}

/* Dense unconditional ARM runs exercise the fast-body proof, every partial
 * native boundary and carry observations both before MULS and at exit. */
static void arm_bulk_boundaries(void) {
    static const uint32_t caps[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
        17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,40,48,63,64,65,96,127,256};
    static const uint32_t ops[] = {
        0xe00c0392,0xe3ccc8ff,0xe08c8468,0xe0b89061,
        0xe03a9c9b,0xe03ce02d,0xe0d9dfca,0xe1b08f8e,
        0xe119046a,0xe3ccc8ff,0xe08c6466,0xe00c0b99,
        0xe3ccc8ff,0xe08c7467,0xe08ee004,0xe1b09bae,
        0xe2800001,0xe2511001,0xe3822001,0xe1a03005,
        0xe1720083,0xe3c005ff,0xe02a6d99,0xe06abfee,
        0xe1b01022,0xe1b02043,0xe1a03064,0xe2255011,
        0xe1a08089,0xe04a804b,0xe2aab07f,0xe3b0c3ff};
    static const unsigned waits[] = {0,1,2,31,127,255};
    unsigned before = comparisons;
    for (unsigned bank = 0; bank < 3; ++bank) for (unsigned align = 0; align < 4; ++align) {
        setup();
        for (unsigned i = 0; i < 64; ++i) put32(4 * i, ops[i & 31]);
        put32(256, 0xeaffffbe); put32(260, 0xe1a00000); put32(264, 0xe1a00000);
        CHECK(gbn_attach_rom(&machine, rom, sizeof(rom)));
        machine.ewram = ewram + align; machine.iwram = iwram + align;
        unsigned region = bank ? bank + 1 : 8;
        uint32_t base = region << 24 | (bank ? 0x1000 : 0);
        if (bank) memcpy((region == 2 ? machine.ewram : machine.iwram) + 0x1000, rom, 268);
        for (unsigned wrap = 0; wrap < 2; ++wrap) for (unsigned distance = 0; distance < 65; ++distance)
        for (unsigned k = 0; k < sizeof(caps) / sizeof(*caps); ++k) {
            machine.next_event = -1; memset(machine.events, 0, sizeof(machine.events));
            machine.now = wrap ? 0xfffffff0 : 0;
            for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
            machine.cpu.cpsr = (random_word() & ~63u) | 31;
            machine.cpu.shifter_carry = (k + distance) & 1;
            machine.ewram_wait = (uint8_t) waits[(k + distance) % 6];
            gbn_set_waitcnt(&machine, (uint16_t) random_word());
            CHECK(gbn_enter_arm(&machine, base) == GBN_STEP);
            if (distance < 64) CHECK(gbn_schedule(&machine, 0, distance, 0));
            compare(caps[k], 0x430000 | bank << 16 | align << 14 | wrap << 13 | distance << 6 | k);
        }
    }
    printf("PASS ARM pure segment boundaries: %u comparisons; all cap/event cuts, NZCV/reserved bits, MULS carry reads, RRX, SP/LR, RAM alignment, waits and clock wrap\n", comparisons - before);

    /* Match the real hot IWRAM arithmetic pattern, with a cold-independent
     * interpreter and a separately warmed native cache. No frame/FPS claim. */
    setup();
    static const uint32_t hot[] = {0xe009019e,0xe0809bc9,0xe00c099a,0xe3ccc8ff,
        0xe08c6466,0xe00c099b,0xe3ccc8ff,0xe08c7467,0xe08ee004,0xe1b09bae,0xeafffff4};
    for (unsigned i = 0; i < sizeof(hot) / sizeof(*hot); ++i) put32(4 * i, hot[i]);
    memcpy(iwram + 0x1000, rom, sizeof(hot));
    for (unsigned r = 0; r < 15; ++r) machine.cpu.r[r] = random_word();
    machine.cpu.r[10] = 1; machine.cpu.r[11] = 2; machine.cpu.r[14] = 0x1234567;
    CHECK(gbn_enter_arm(&machine, 0x03001000) == GBN_STEP);
    struct Gbn initial = machine;
    compare(128, 0x43ffff);
    machine = initial; reference = initial;
    uint32_t a, b;
    uint64_t stamp = am_counter_retired();
    CHECK(gbn_run_batch(&reference, 100000, &a) == GBN_STEP);
    uint64_t interpreted = am_counter_retired() - stamp;
    stamp = am_counter_retired();
    CHECK(gbn_rv32_run_batch(&backend, 100000, &b) == GBN_STEP);
    uint64_t compiled = am_counter_retired() - stamp;
    CHECK(a == b && a == 100000 && !memcmp(&machine, &reference, sizeof(machine)));
    printf("ARM hot microbenchmark: interpreted=%" PRIu64 "; native=%" PRIu64 "; 100000 guest instructions; not frame FPS\n", interpreted, compiled);
}

static void single_list_transfers(void) {
    static const unsigned distances[]={0,1,2,3,7,31,4096};
    unsigned before=comparisons;
    for(unsigned load=0;load<2;++load) for(unsigned base=0;base<8;++base)
    for(unsigned reg=0;reg<8;++reg) for(unsigned bank=0;bank<3;++bank) {
        setup();unsigned code=0xc000|(load<<11)|(base<<8)|(1u<<reg);
        put16(0,code);CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        memcpy(iwram+0x1000,rom,8);memcpy(ewram+0x1000,rom,8);
        for(unsigned data=0;data<2;++data) for(unsigned low=0;low<4;++low)
        for(unsigned wrap=0;wrap<2;++wrap) for(unsigned d=0;d<7;++d) {
            machine.next_event=-1;machine.event_head=-1;
            memset(machine.events,0,sizeof(machine.events));
            memset(machine.event_prev,GBN_EVENT_COUNT,sizeof(machine.event_prev));
            memset(machine.event_next,GBN_EVENT_COUNT,sizeof(machine.event_next));
            machine.now=wrap?0xfffffff0u:0;
            for(unsigned j=0;j<15;++j)machine.cpu.r[j]=random_word();
            machine.cpu.r[base]=((data?3u:2u)<<24)|0x4000|low;
            machine.cpu.cpsr=(random_word()&~63u)|31;
            machine.ewram_wait=(uint8_t)((d&1)?255:2);gbn_set_waitcnt(&machine,(uint16_t)random_word());
            CHECK(gbn_enter_thumb(&machine,bank?((bank+1)<<24)|0x1000:0x08000000)==GBN_STEP);
            CHECK(gbn_schedule(&machine,0,distances[d],0));
            uint64_t native=backend.native_instructions;
            compare_memory(1,0x76000000|code,(data?iwram:ewram)+0x4000);
            if(!low && distances[d]) CHECK(backend.native_instructions==native+1);
        }
    }
    printf("PASS single-list RAM transfers: %u comparisons; every base/register alias, three code banks, both RAM banks, low bits, waits, event cuts and wrap\n",comparisons-before);
}

static void batched_audio_cpu(void) {
    static struct GbnDevices peer;
    unsigned before=comparisons;
    for(unsigned rate=0;rate<4;++rate) for(unsigned master=0;master<2;++master)
    for(unsigned route=0;route<2;++route) for(unsigned wrap=0;wrap<2;++wrap)
    for(unsigned bank=0;bank<3;++bank) {
        setup();
        for(unsigned i=0;i<128;++i)put16(2*i,0x3001);
        put16(256,0x8008);put16(258,0x601a);put16(260,0x682c);
        put16(262,0x803e);put16(264,0xe77a);
        memcpy(iwram+0x1000,rom,512);memcpy(ewram+0x1000,rom,512);
        CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
        machine.now=wrap?0xfffff000u:0;
        devices.io[0x84/2]=master?0x80:0;devices.io[0x88/2]=512|(rate<<14);
        devices.io[0x80/2]=route?0xff77:0;devices.io[0x82/2]=0x330c;
        for(unsigned j=0;j<4;++j) {struct GbnPsgChannel*c=&devices.audio.psg[j];c->enabled=true;c->frequency=2000+j;c->updated=machine.now;c->noise=5;c->length=64;c->volume=j==2?1:15;}
        machine.cpu.r[0]=route?0xff77:0;machine.cpu.r[1]=0x04000080;
        machine.cpu.r[2]=random_word();machine.cpu.r[3]=0x040000a0;
        machine.cpu.r[5]=0x02004000;machine.cpu.r[6]=512|(rate<<14);machine.cpu.r[7]=0x04000088;
        CHECK(gbn_enter_thumb(&machine,bank?((bank+1)<<24)|0x1000:0x08000000)==GBN_STEP);
        gbn_audio_start(&machine);gbn_audio_set_batched(&machine,true);
        for(unsigned chunk=0;chunk<64;++chunk) {
            CHECK(gbn_service_events(&machine)==GBN_STEP);
            reference=machine;peer=devices;reference.devices=&peer;
            uint32_t want=0,got=0,cap=1+(random_word()&255);
            enum GbnStatus a=gbn_run_batch(&reference,cap,&want);
            enum GbnStatus b=gbn_rv32_run_batch(&backend,cap,&got);
            reference.devices=&devices;
            if(a!=b||want!=got||memcmp(&machine,&reference,sizeof(machine))||memcmp(&devices,&peer,sizeof(devices))) {
                printf("FAIL batched CPU rate=%u master=%u route=%u wrap=%u bank=%u chunk=%u count=%u/%u time=%u/%u PC=%08x/%08x\n",rate,master,route,wrap,bank,chunk,got,want,machine.now,reference.now,machine.cpu.pc,reference.cpu.pc);exit(1);
            }
            ++comparisons;
        }
    }
    printf("PASS batched PCM native/interpreter: %u complete CPU/device states; all rates, routes, master, banks, cap/event cuts, CPU sound/FIFO/rate writes and clock wrap\n",comparisons-before);
}
