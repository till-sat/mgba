extern uint32_t gbn_rv32_dma_start_hits;
static void native_dma_start_boundaries(void) {
    unsigned before=comparisons;uint32_t hits=gbn_rv32_dma_start_hits;
    static const unsigned distances[]={0,1,2,3,4,16,1024};
    static const unsigned caps[]={0,1,2,16},counts[]={0,1,3,256,65535};
    for(unsigned form=0;form<8;++form) {
        setup();bool thumb=!(form&1);unsigned region=form<4?8:form<6?3:2;
        uint32_t arm[]={form&2?0xe4810004:0xe5a10004,0xe2833001,0xe2844001,0xeafffffe};
        uint16_t th[]={form&2?0x5088:0x6008,0x3301,0x3401,0xe7fe};
        uint8_t* code=region==8?rom:region==3?iwram:ewram;
        memcpy(code,thumb?(const void*)th:(const void*)arm,thumb?sizeof(th):sizeof(arm));
        for(unsigned id=0;id<GBN_EVENT_COUNT;++id)for(unsigned distance=0;distance<7;++distance)
        for(unsigned config=0;config<4;++config)for(unsigned cap=0;cap<4;++cap) {
            gbn_init(&machine,ewram,iwram);CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
            CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
            for(unsigned r=0;r<15;++r)machine.cpu.r[r]=random_word();
            machine.now=config&2?0xfffffff0:0;machine.ewram_wait=config*2;
            gbn_set_waitcnt(&machine,(uint16_t)((config&1?0x4000:0)|(config<<2)));
            unsigned control=0x8000|(config&1?0x400:0)|((id&3)<<5)|(id&4?0x4000:0)|(id&8?0x200:0);
            machine.cpu.r[0]=control<<16|counts[(id+distance+config)%5];
            machine.cpu.r[1]=0x040000dc-(!thumb && !(form&2)?4:0);machine.cpu.r[2]=0;
            machine.cpu.cpsr=(random_word()&~63u)|(thumb?63:31);machine.cpu.shifter_carry=random_word();
            devices.dma[3].source=0x03000000|(random_word()&0x7fff);
            devices.dma[3].dest=0x06000000|(random_word()&0xffff);
            devices.dma[3].count=random_word();devices.dma[3].when=random_word();devices.dma[3].latch=random_word();
            devices.dma[3].next_source=random_word();devices.dma[3].next_dest=random_word();devices.dma[3].fifo=!!(config&1);
            /* Nonzero padding verifies exact event object assignment too. */
            ((uint8_t*)&machine.events[GBN_EVENT_DMA])[6]=0x7e;
            ((uint8_t*)&machine.events[GBN_EVENT_DMA])[7]=0x81;
            uint32_t pc=region<<24;
            CHECK((thumb?gbn_enter_thumb(&machine,pc):gbn_enter_arm(&machine,pc))==GBN_STEP);
            machine.prefetched_pc=pc+(thumb?4:8)+2*config;
            CHECK(gbn_schedule(&machine,id,distances[distance],config==0?0:config==2?255:64));
            if(config==3 && id==15)CHECK(gbn_schedule(&machine,14,distances[distance],64));
            compare_device_write(caps[cap],0x73000000|form<<20|id<<16|distance<<12|config<<8|cap);
        }
    }
    CHECK(comparisons-before==14336);CHECK(gbn_rv32_dma_start_hits-hits>100);
    printf("PASS native DMA3 start: %u states; %u actual native starts; Thumb immediate/register, ARM pre/post writeback, ROM/two RAM banks, all event IDs/priorities/ties/historical winner, count0/1/max,16/32bit alignment, all device fields, cap/event cuts, prefetch and clock wrap\n",comparisons-before,gbn_rv32_dma_start_hits-hits);
}

static void native_dma_start_misses(void) {
    unsigned before=comparisons;uint32_t hits=gbn_rv32_dma_start_hits;
    for(unsigned test=0;test<16;++test)for(unsigned form=0;form<2;++form)
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned cap=0;cap<3;++cap) {
        setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
        put16(0,0x6008);put16(2,0x3301);put16(4,0xe7fe);
        uint32_t arm[]={0xe5810000,0xe2833001,0xeafffffe};if(form)memcpy(rom,arm,sizeof(arm));
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));machine.now=wrap?0xfffffff0:0;
        machine.cpu.r[0]=0x80000003;machine.cpu.r[1]=0x040000dc;
        devices.dma[3].source=0x03000100;devices.dma[3].dest=0x06000100;
        switch(test) {
        case 0:machine.cpu.r[0]=0x90000003;break;
        case 1:machine.cpu.r[0]=0xa0000003;break;
        case 2:machine.cpu.r[0]=0xb0000003;break;
        case 3:machine.cpu.r[0]=0x00000003;break;
        case 4:devices.dma[3].control=0x8000;break;
        case 5:devices.dma[3].remaining=1;break;
        case 6:devices.dma[3].finishing=true;break;
        case 7:CHECK(gbn_schedule(&machine,GBN_EVENT_DMA,100,64));break;
        case 8:case 9:case 10:devices.dma[test-8].control=0x8000;devices.dma[test-8].remaining=1;devices.dma[test-8].when=machine.now+100;break;
        case 11:devices.dma[2].control=0x8000;devices.dma[2].finishing=true;devices.dma[2].when=machine.now+100;break;
        case 12:machine.cpu.r[1]+=1;break;
        case 13:machine.cpu.r[1]+=2;break;
        case 14:machine.cpu.r[1]=0x040000b8;break;
        case 15:machine.cpu.r[1]=0x040000d0;break;
        }
        CHECK((form?gbn_enter_arm(&machine,0x08000000):gbn_enter_thumb(&machine,0x08000000))==GBN_STEP);
        compare_device_write(cap?cap==1?1:16:0,0x74000000|test<<16|form<<8|wrap<<4|cap);
    }
    CHECK(comparisons-before==192);CHECK(gbn_rv32_dma_start_hits==hits);
    printf("PASS native DMA3 misses: %u full states; other channels/live transfers,old enable/own pending/finishing,event active,unaligned/raw address,new disable/custom timing; no native start accepted\n",comparisons-before);
}
