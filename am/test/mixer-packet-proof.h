static const uint32_t packet_program[]={0xe5956000,0xe5957630,
    0xe009019e,0xe0809bc9,0xe00c099a,0xe3ccc8ff,0xe08c6466,
    0xe00c099b,0xe3ccc8ff,0xe08c7467,0xe08ee004,0xe1b09bae,
    0x0a000007,0xe3cee5fe,0xe0522009,0xdaffffce,0xe2599001,
    0x00800001,0x11b300d9,0xe1f310d1,0xe0411000,0xe2955101,0x3affffea,
    0xe5857630,0xe4856004,0xe2588004,0xcaffffe4,0xeafffffe};
/* Compare the complete writable RAM backing, including output/code aliasing.
 * For device-free packet tests RAM is the only external writable state. */
static uint8_t packet_before[GBN_IWRAM_SIZE+4],packet_after[GBN_IWRAM_SIZE+4];
static uint8_t packet_ew_before[GBN_EWRAM_SIZE+4],packet_ew_after[GBN_EWRAM_SIZE+4];
static bool packet_compare_ew;
static void compare_packet(unsigned cap,unsigned tag) {
    reference=machine;memcpy(packet_before,iwram,sizeof(iwram));uint32_t want=0,got=0;
    if(packet_compare_ew)memcpy(packet_ew_before,ewram,sizeof(ewram));
    enum GbnStatus a=gbn_run_batch(&reference,cap,&want);
    memcpy(packet_after,iwram,sizeof(iwram));memcpy(iwram,packet_before,sizeof(iwram));
    if(packet_compare_ew){memcpy(packet_ew_after,ewram,sizeof(ewram));memcpy(ewram,packet_ew_before,sizeof(ewram));}
    enum GbnStatus b=gbn_rv32_run_batch(&backend,cap,&got);
    if(a!=b || want!=got || memcmp(&machine,&reference,sizeof(machine)) || memcmp(iwram,packet_after,sizeof(iwram)) || (packet_compare_ew && memcmp(ewram,packet_ew_after,sizeof(ewram)))) {
        printf("FAIL packet tag=%08x cap=%u count=%u/%u status=%u/%u PC=%08x/%08x flags=%08x/%08x time=%u/%u shift=%u/%u prefetch=%08x/%08x packets=%llu\n",tag,cap,got,want,b,a,machine.cpu.pc,reference.cpu.pc,machine.cpu.cpsr,reference.cpu.cpsr,machine.now,reference.now,machine.cpu.shifter_carry,reference.cpu.shifter_carry,machine.prefetched_pc,reference.prefetched_pc,backend.fused_mixer_packets);
        for(unsigned i=0;i<15;++i)if(machine.cpu.r[i]!=reference.cpu.r[i])printf("r%u=%08x/%08x\n",i,machine.cpu.r[i],reference.cpu.r[i]);
        for(unsigned i=0;i<sizeof(iwram);++i)if(iwram[i]!=packet_after[i]){printf("memory[%x]=%02x/%02x\n",i,iwram[i],packet_after[i]);break;}
        exit(1);
    }
    ++comparisons;
}
static void packet_initial(unsigned align,unsigned variant) {
    gbn_init(&machine,ewram,iwram+align);CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
    for(unsigned i=0;i<15;++i)machine.cpu.r[i]=random_word();
    machine.cpu.r[0]=(uint32_t)(int32_t)(int8_t)random_word();
    machine.cpu.r[1]=(uint32_t)(int32_t)(int8_t)random_word()-machine.cpu.r[0];
    machine.cpu.r[2]=10000;machine.cpu.r[3]=0x08000100;
    static const unsigned steps[]={0,1,0x400000,0x7fffff,0x800000,0x800001,0x1800000,0x4000000};
    machine.cpu.r[4]=steps[variant&7];machine.cpu.r[5]=0x03003000;
    machine.cpu.r[8]=variant&8?4:1000;
    machine.cpu.r[10]=(random_word()&255)<<16;machine.cpu.r[11]=(random_word()&255)<<16;
    machine.cpu.r[14]=variant&16?0:0x7fffff;
    machine.cpu.cpsr=(random_word()&0xff000000)|31;machine.cpu.shifter_carry=random_word();
    gbn_set_waitcnt(&machine,(uint16_t)random_word());machine.prefetched_pc=random_word();
}
static void resident_packets(void) {
    static const unsigned caps[]={0,1,9,10,13,20,21,22,31,55,56,57,58,63,64,87,88,89,90,128,256};
    static const unsigned times[]={0,1,19,20,63,83,84,85,86,107,108,109,128,160,192,1024};
    unsigned before=comparisons;uint64_t hits=0;
    for(unsigned align=0;align<4;++align)for(unsigned variant=0;variant<32;++variant) {
        setup();for(unsigned i=0;i<sizeof(rom);++i)rom[i]=(uint8_t)random_word();
        memcpy(iwram+align+0x1000,packet_program,sizeof(packet_program));
        uint32_t spin=0xeafffffe;memcpy(iwram+align+0x1008-140,&spin,4);
        for(unsigned c=0;c<sizeof(caps)/sizeof(*caps);++c)for(unsigned d=0;d<sizeof(times)/sizeof(*times);++d) {
            packet_initial(align,variant);machine.now=variant&1?0xfffffff0:0;
            CHECK(gbn_enter_arm(&machine,0x03001000+(variant&4?0:8))==GBN_STEP);
            CHECK(gbn_schedule(&machine,0,times[d],0));
            compare_packet(caps[c],0x6b000000|align<<22|variant<<16|c<<8|d);
        }
        hits+=backend.fused_mixer_packets;
    }
    printf("Packet coverage probe: %llu hits; %u comparisons\n",hits,comparisons-before);
    CHECK(hits>=900);
    printf("PASS resident four-sample packet: %u states, %llu actual packets; all RAM, exact counts/flags/phase/PCM words, q=0/1/many, cap/time cuts, backing alignment, clock wrap and taken/untaken final branch\n",comparisons-before,hits);
}

/* Full image permits live 08->09 and 0d->0e bus-region boundaries. */
/* packet_large_rom shares the earlier open-bus fixture allocation. */
static void packet_guard_boundaries(void) {
    static const unsigned caps[]={1,13,55,56,58,64,88,128};
    static const unsigned times[]={0,83,84,85,86,128,256,1024};
    unsigned before=comparisons;uint64_t hits=0;
    for(unsigned test=0;test<32;++test) {
        setup();
        for(unsigned i=0;i<128;++i){packet_large_rom[0xffffc0+i]=(uint8_t)random_word();packet_large_rom[GBN_ROM_MAX_SIZE-128+i]=(uint8_t)random_word();}
        for(unsigned c=0;c<8;++c)for(unsigned d=0;d<8;++d)
        for(unsigned wrap=0;wrap<2;++wrap)for(unsigned entry=0;entry<2;++entry) {
            memcpy(iwram+0x1000,packet_program,sizeof(packet_program));
            uint32_t spin=0xeafffffe;memcpy(iwram+0x1008-140,&spin,4);
            packet_initial(0,0);machine.now=wrap?0xfffffff0:0;uint32_t pc=0x03001000+8*entry;
            packet_compare_ew=false;
            switch(test) {
            case 1:machine.cpu.r[14]=0x800000;break;
            case 2:machine.cpu.r[4]=0x4000001;break;
            case 3:machine.cpu.r[0]=128;break;
            case 4:machine.cpu.r[0]=(uint32_t)-129;break;
            case 5:machine.cpu.r[1]=256;break;
            case 6:machine.cpu.r[1]=(uint32_t)-256;break;
            case 7:machine.cpu.r[0]=127;machine.cpu.r[1]=1;break;
            case 8:machine.cpu.r[10]|=1;break;
            case 9:machine.cpu.r[11]=0x1000000;break;
            case 10:machine.cpu.r[5]|=0x40000000;break;
            case 11:machine.cpu.r[5]|=1;break;
            case 12:machine.cpu.r[5]=0x02003000;packet_compare_ew=true;break;
            case 13:machine.cpu.r[5]=0x03007fff;break;
            case 14:machine.cpu.r[5]=0x03fffffc;break;
            case 15:machine.cpu.r[2]=4;machine.cpu.r[4]=0x800000;break;
            case 16:machine.cpu.r[2]=0x80000000;break;
            case 17:machine.cpu.r[3]=0x02000100;break;
            case 18:machine.cpu.r[3]=0x03003000;break;
            case 19:machine.cpu.r[3]=0x08000fff;machine.cpu.r[4]=0x800000;break;
            case 20:case 21:case 22:
                CHECK(gbn_attach_rom(&machine,packet_large_rom,sizeof(packet_large_rom)));
                machine.cpu.r[3]=test==20?0x08fffffc:test==21?0x09000100:0x0dfffffc;
                machine.cpu.r[4]=0x800000;break;
            case 23:case 24:
                CHECK(gbn_attach_rom(&machine,iwram+(test==23?0x3000:0x3630),0x1000));
                machine.cpu.r[3]=0x08000000;machine.cpu.r[4]=0x800000;break;
            case 25:machine.cpu.r[5]=0x03001000;break;
            case 26:machine.cpu.r[5]=0x030009d0;break;
            case 27:machine.cpu.r[5]=0x03001078;break;
            case 28:pc+=2;break;
            case 29:machine.cpu.r[5]=0x0300b000;break;
            case 30:machine.cpu.r[8]=0x80000000;break;
            case 31:machine.cpu.r[8]=0x7fffffff;break;
            }
            CHECK(gbn_enter_arm(&machine,pc)==GBN_STEP);CHECK(gbn_schedule(&machine,0,times[d],0));
            compare_packet(caps[c],0x6c000000|test<<16|c<<8|d<<2|wrap<<1|entry);
        }
        hits+=backend.fused_mixer_packets;
    }
    packet_compare_ew=false;CHECK(hits>100);
    printf("PASS packet guard boundaries: %u states, %llu actual packets; invalid domains, ROM/image/mirror/bus ends, source/output/code host aliases, halfword PC, full writable RAM, final signed flags and event/cap/wrap cuts\n",comparisons-before,hits);
}

static void packet_lookahead_changes(void) {
    unsigned before=comparisons,lookahead=0;
    for(unsigned align=0;align<4;++align)for(unsigned half=0;half<2;++half)
    for(unsigned writer=0;writer<2;++writer)for(unsigned extra=0;extra<2;++extra) {
        setup();memcpy(iwram+align+0x1000,packet_program,sizeof(packet_program));
        packet_initial(align,0);machine.cpu.r[8]=1000;
        uint32_t pc=0x03001000+2*half;
        CHECK(gbn_enter_arm(&machine,pc)==GBN_STEP);compare_packet(1,0x6d000000|align<<12|half<<8|writer<<4|extra);
        for(unsigned i=0;i<GBN_RV32_SLOTS;++i)if(backend.blocks[i].valid && backend.blocks[i].pc==pc) {
            CHECK(backend.blocks[i].proof_instructions>=27);
            if(backend.blocks[i].instructions<27)++lookahead;
        }
        if(!writer) {
            uint32_t mov=0xe1a00000;memcpy(machine.iwram+0x1068,&mov,4);
            CHECK(gbn_enter_arm(&machine,pc)==GBN_STEP);compare_packet(extra?128:58,0x6d100000|align<<12|half<<8|extra);
        }else {
            /* Use r4 as the store address and r12 as data. These remain in
             * the valid packet domain, so opcode proof is the required guard. */
            uint32_t store[]={0xe584c000,0xe12fff12};memcpy(machine.iwram+0x2000,store,sizeof(store));
            machine.cpu.r[12]=0xe1a00000;machine.cpu.r[4]=0x03001068;machine.cpu.r[2]=pc;
            CHECK(gbn_enter_arm(&machine,0x03002000)==GBN_STEP);compare_packet(extra?128:92,0x6d200000|align<<12|half<<8|extra);
        }
        CHECK(backend.fused_mixer_packets==0);
    }
    CHECK(lookahead==32);
    printf("PASS packet lookahead changes: %u states, %u source proofs longer than cold bodies; final BGT modified before C entry and by a same-frame native writer, valid-domain operands, halfword PC and host alignment\n",comparisons-before,lookahead);
}

static void packet_guest_region_edges(void) {
    static const uint32_t pcs[]={0x03000000,0x03000002,0x03000004,0x03000006,
        0x03000008,0x0300000a,0x03008000,0x03008002};
    unsigned before=comparisons;
    for(unsigned p=0;p<8;++p)for(unsigned taken=0;taken<2;++taken)
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned long_cap=0;long_cap<2;++long_cap) {
        setup();uint32_t spin=0xeafffffe;
        memcpy(iwram,&spin,4);memcpy(iwram+4,&spin,4);
        memcpy(iwram+GBN_IWRAM_SIZE-8,&spin,4);memcpy(iwram+GBN_IWRAM_SIZE-4,&spin,4);
        memcpy(ewram,&spin,4);memcpy(ewram+GBN_EWRAM_SIZE-8,&spin,4);memcpy(ewram+GBN_EWRAM_SIZE-4,&spin,4);
        memcpy(iwram+(pcs[p]&(GBN_IWRAM_SIZE-4)),packet_program+2,sizeof(packet_program)-8);
        packet_initial(0,0);machine.cpu.r[8]=taken?1000:4;machine.now=wrap?0xfffffff0:0;
        CHECK(gbn_enter_arm(&machine,pcs[p])==GBN_STEP);
        packet_compare_ew=true;compare_packet(long_cap?128:56,0x6e000000|p<<8|taken<<2|wrap<<1|long_cap);
        if(p<4)CHECK(backend.fused_mixer_packets==0);else CHECK(backend.fused_mixer_packets>0);
    }
    packet_compare_ew=false;
    printf("PASS packet guest-region edges: %u states; 03000000/4 BGT crosses into EWRAM and must retain mapping-aware interpreter, valid low/mirrored IWRAM entries still fuse, halfword PCs and wrap\n",comparisons-before);
}

/* Resume at the arithmetic entry after 1/2/3 earlier rotation iterations.
 * Packed incoming accumulators may be arbitrary; compare every writable byte. */
static void packet_rotation_suffixes(void) {
    static const unsigned caps[]={0,1,16,17,18,24,25,26,29,30,31,37,38,39,42,43,44,45,46,47,50,51,52,58,59,60,66,67,68,128,256};
    static const unsigned times[]={0,1,19,20,23,24,25,26,39,40,43,44,45,46,63,64,65,66,128,1024};
    unsigned before=comparisons;uint64_t hits[3]={0};
    for(unsigned rotation=1;rotation<4;++rotation)for(unsigned align=0;align<4;++align)
    for(unsigned variant=0;variant<16;++variant) {
        setup();for(unsigned i=0;i<sizeof(rom);++i)rom[i]=(uint8_t)random_word();
        memcpy(iwram+align+0x1000,packet_program,sizeof(packet_program));
        uint32_t spin=0xeafffffe;memcpy(iwram+align+0x1008-140,&spin,4);
        for(unsigned c=0;c<sizeof(caps)/sizeof(*caps);++c)
        for(unsigned d=0;d<sizeof(times)/sizeof(*times);++d) {
            packet_initial(align,variant);machine.cpu.r[5]|=rotation<<30;
            machine.cpu.r[14]=variant&8?0:0x7fffff;machine.cpu.r[8]=align&1?4:1000;
            machine.now=variant&1?0xfffffff0:0;
            CHECK(gbn_enter_arm(&machine,0x03001008+(align&2))==GBN_STEP);
            CHECK(gbn_schedule(&machine,0,times[d],0));
            compare_packet(caps[c],0x6f000000|rotation<<22|align<<20|variant<<16|c<<8|d);
        }
        hits[rotation-1]+=backend.fused_mixer_packets;
    }
    for(unsigned i=0;i<3;++i)CHECK(hits[i]>100);
    CHECK(comparisons-before==119040);
    printf("PASS native rotation suffix packets: %u states, %llu/%llu/%llu real packets; all prior rotations, exact full memory/count/time/flags/source/output, partial caps/event cuts, all host alignments, halfword PC and wrap\n",comparisons-before,hits[0],hits[1],hits[2]);
}
