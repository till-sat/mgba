/* SPDX-License-Identifier: MPL-2.0 */
extern uint32_t gbn_rv32_packet_time_skips;
extern uint32_t gbn_rv32_partial_packets;
static void packet_time_boundaries(void) {
    static const unsigned times[]={1,2,19,20,21,24,25,39,40,41,59,60,61,83,84,85,86,87,88};
    static const unsigned caps[]={0,1,12,13,14,25,26,27,38,39,40,55,56,57,256};
    static const uint32_t lasts[]={0,4,8,0x80000000};
    unsigned before=comparisons;uint32_t skips=gbn_rv32_packet_time_skips;
    uint32_t partial=gbn_rv32_partial_packets;uint64_t packets=0;
    for(unsigned rotation=0;rotation<4;++rotation) {
        setup();for(unsigned i=0;i<sizeof(rom);++i)rom[i]=(uint8_t)random_word();
        memcpy(iwram+0x1000,packet_program,sizeof(packet_program));
        for(unsigned d=0;d<sizeof(times)/sizeof(*times);++d)
        for(unsigned cap=0;cap<sizeof(caps)/sizeof(*caps);++cap)
        for(unsigned last=0;last<4;++last)for(unsigned wrap=0;wrap<2;++wrap)
        for(unsigned domain=0;domain<4;++domain) {
            packet_initial(0,0);machine.now=wrap?0xfffffff0:0;
            machine.cpu.r[14]=0;machine.cpu.r[4]=0;machine.cpu.r[8]=lasts[last];
            machine.cpu.r[5]=0x03003000|rotation<<30;
            if(domain==1)machine.cpu.r[0]=0x80000000;
            if(domain==2)machine.cpu.r[3]=0x0dfffffe;
            if(domain==3)machine.cpu.r[10]=0x80000000;
            CHECK(gbn_enter_arm(&machine,0x03001008)==GBN_STEP);
            CHECK(gbn_schedule(&machine,0,times[d],64));
            CHECK(gbn_schedule(&machine,1,times[d],64));
            uint32_t old=gbn_rv32_packet_time_skips;
            compare_packet(caps[cap],0x7a000000|rotation<<22|d<<16|cap<<12|last<<8|wrap<<4|domain);
            if(times[d]<=20 && caps[cap])CHECK(gbn_rv32_packet_time_skips>old);
        }
        packets+=backend.fused_mixer_packets;
    }
    CHECK(comparisons-before==4*(sizeof(times)/sizeof(*times))*(sizeof(caps)/sizeof(*caps))*4*2*4);
    CHECK(gbn_rv32_packet_time_skips>skips && packets>0 && gbn_rv32_partial_packets>partial);
    printf("PASS packet sample budgets: %u complete RAM/state comparisons; %u actual filters, %llu packets, %u partial;20/40/60/84-cycle edges,short rotations,final signed branch,invalid domains,event ties,cap and wrap\n",comparisons-before,gbn_rv32_packet_time_skips-skips,packets,gbn_rv32_partial_packets-partial);
}
