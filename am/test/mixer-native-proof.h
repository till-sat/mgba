static void fused_native_arithmetic(void) {
    static const uint32_t program[]={0xe009019e,0xe0809bc9,0xe00c099a,0xe3ccc8ff,
        0xe08c6466,0xe00c099b,0xe3ccc8ff,0xe08c7467,0xe08ee004,0xe1b09bae,0xeafffff4};
    static const unsigned caps[]={0,1,2,7,8,9,10,11,12,15,16,20,21,22,31,64};
    static const unsigned distances[]={0,1,9,12,13,14,21,22,23,64,2048};
    static const uint32_t classes[]={0xff,0xff00,0xff0000,0x80000000};
    unsigned before=comparisons;uint64_t total_fused=0;
    for(unsigned bank=0;bank<3;++bank)for(unsigned align=0;align<4;++align)
    for(unsigned half=0;half<2;++half) {
        setup();memcpy(rom,program,sizeof(program));
        memcpy(iwram+align+0x1000,program,sizeof(program));
        memcpy(ewram+align+0x1000,program,sizeof(program));
        unsigned region=bank?bank+1:8;uint32_t base=region<<24|(bank?0x1000:0)|2*half;
        for(unsigned wrap=0;wrap<2;++wrap)for(unsigned cl=0;cl<4;++cl)
        for(unsigned k=0;k<sizeof(caps)/sizeof(*caps);++k)
        for(unsigned d=0;d<sizeof(distances)/sizeof(*distances);++d) {
            gbn_init(&machine,ewram+align,iwram+align);
            CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
            machine.now=wrap?0xfffffff0:0;
            for(unsigned r=0;r<15;++r)machine.cpu.r[r]=random_word();
            machine.cpu.r[0]=classes[cl];machine.cpu.r[1]=classes[(cl+1)&3]^(k&1?UINT32_MAX:0);
            machine.cpu.cpsr=(random_word()&0xff000000)|31;
            machine.cpu.shifter_carry=random_word();
            gbn_set_waitcnt(&machine,(uint16_t)random_word());
            CHECK(gbn_enter_arm(&machine,base)==GBN_STEP);
            machine.prefetched_pc=random_word();
            CHECK(gbn_schedule(&machine,0,distances[d],0));
            compare(caps[k],0x7e000000|bank<<22|align<<20|half<<19|wrap<<18|cl<<16|k<<8|d);
        }
        total_fused+=backend.fused_mixer_instructions;
    }
    CHECK(total_fused>1000);
    printf("PASS resident fused mixer arithmetic: %u states, %llu fused guest instructions; all 32-bit operands, signed early-exit classes, NZCV/reserved bits, every cap/time cut, word/halfword PC, backing alignment, ROM/two RAM banks and wrap\n",comparisons-before,total_fused);
}
