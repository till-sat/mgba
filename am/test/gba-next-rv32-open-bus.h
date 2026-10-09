/* SPDX-License-Identifier: MPL-2.0 */
/* Included by the RV32 differential test; actual emitted machine code. */
_Alignas(4) static uint8_t open_bus_large_rom[0x1000000+32];
static uint32_t open_bus_arm(unsigned kind, unsigned rd, unsigned rn) {
    uint32_t code = 0xe1900000 | rn << 16 | rd << 12;
    if (kind == 3 || kind == 5 || kind == 7)
        return code | 0x400090 | (kind == 3 ? 2u : kind == 5 ? 1u : 3u) << 5;
    return code | 0x04000000 | (kind == 6 ? 1u << 22 : 0);
}
static void open_bus_reads(void) {
    static const unsigned regions[] = {2, 3, 8, 9, 10, 11, 12, 13};
    static const unsigned offsets[] = {0x10,0x46,0x4c,0x54,0xb0,0xb4,0xbc,0xc0,0xc8,0xcc,0xd4,0xd8};
    static const unsigned widths[] = {4,2,1,1,4,2,1,2};
    static const uint32_t buses[] = {0,0x80ffaabb,0xabcdff80,0xffff80ff,0x12340000,0xffffffff};
    setup(); CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    unsigned before = comparisons;
    for(unsigned bank=0;bank<8;++bank)for(unsigned arm=0;arm<2;++arm)for(unsigned kind=3;kind<8;++kind) {
        put16(4,0x1234);put16(6,0xabcd);put16(8,0x80ff);put16(10,0xff80);
        if(arm)put32(0,open_bus_arm(kind,0,1));
        else { put16(0,0x5000|kind<<9|2<<6|1<<3);put16(2,0x5000|kind<<9|2<<6|1<<3); }
        memcpy(ewram,rom,32);memcpy(iwram,rom,32);
        if(regions[bank]>=8 && (regions[bank]&1)) {
            memcpy(open_bus_large_rom+0x1000000,rom,32);
            CHECK(gbn_attach_rom(&machine,open_bus_large_rom,sizeof(open_bus_large_rom)));
        } else CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        for(unsigned bit=0;bit<2;++bit)for(unsigned off=0;off<12;++off)for(unsigned latch=0;latch<6;++latch) {
            unsigned width=widths[kind],offset=offsets[off]+(width==1 ? latch&1 : 0);
            uint32_t pc=regions[bank]<<24|2*bit;
            machine.cpu.cpsr=(random_word()&~63u)|31;machine.cpu.r[0]=random_word();
            machine.cpu.r[1]=0x04000000+offset;machine.cpu.r[2]=0;
            machine.now=0xfffffff0;gbn_set_waitcnt(&machine,(uint16_t)((latch&1?0x4000:0)|latch<<2));
            CHECK((arm ? gbn_enter_arm(&machine,pc) : gbn_enter_thumb(&machine,pc))==GBN_STEP);
            devices.dma_bus_valid=latch!=0;devices.dma_access=latch==4;
            devices.dma_bus=buses[latch];devices.dma_pc=pc+(arm?4:2)+(latch==2 ? 2 : latch==3 ? 0u-2 : 0);
            uint64_t native=backend.native_instructions;
            compare_device_write(1,0x710000|bank<<16|arm<<15|kind<<12|bit<<11|off<<4|latch);
            if(!(offset&(width-1)))CHECK(backend.native_instructions==native+1);
        }
    }
    printf("PASS native open-bus IO: %u comparisons; all code banks/modes, PC bit1, byte lanes/signs, DMA validity/PC/access overrides and full device snapshots\n",comparisons-before);
}
static void open_bus_boundaries(void) {
    static const unsigned regions[]={2,3,8,10,12};
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    put16(0,0x6808);put16(2,0x3201);put16(4,0x680b);put16(6,0x4058);put16(8,0xe7fa);
    memcpy(ewram,rom,32);memcpy(iwram,rom,32);CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
    unsigned before=comparisons;
    for(unsigned bank=0;bank<5;++bank)for(unsigned config=0;config<2;++config)
    for(unsigned distance=0;distance<32;++distance)for(unsigned cap=0;cap<13;++cap) {
        for(unsigned e=0;e<GBN_EVENT_COUNT;++e)gbn_cancel(&machine,e);
        machine.now=0xfffffff0;machine.cpu.r[1]=0x040000d4;machine.cpu.r[2]=random_word();
        gbn_set_waitcnt(&machine,(uint16_t)(config?0x4010:0));
        CHECK(gbn_enter_thumb(&machine,regions[bank]<<24)==GBN_STEP);
        devices.dma_access=false;devices.dma_bus_valid=true;devices.dma_bus=random_word();
        devices.dma_pc=machine.cpu.pc+(distance&1 ? 2 : 6);
        CHECK(gbn_schedule(&machine,0,distance,1));
        compare_device_write(cap,0x720000|bank<<16|config<<15|distance<<5|cap);
    }
    printf("PASS native open-bus streams: %u comparisons; per-instruction visible PC, live DMA values, caps/events, prefetch, RAM/ROM and clock wrap\n",comparisons-before);
}
static void open_bus_ram_tail(void) {
    unsigned before=comparisons;
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    for(unsigned align=0;align<4;++align)for(unsigned bank=2;bank<4;++bank)
    for(unsigned arm=0;arm<2;++arm)for(unsigned bit=0;bit<2;++bit) {
        machine.ewram=ewram+align;machine.iwram=iwram+align;
        if(arm)put32(0,open_bus_arm(4,0,1));
        else {put16(0,0x6808);put16(2,0x6808);}
        memcpy(machine.ewram,rom,32);memcpy(machine.iwram,rom,32);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        for(unsigned trial=0;trial<4;++trial) {
            uint32_t pc=bank<<24|2*bit;
            uint8_t* bytes=bank==2 ? machine.ewram : machine.iwram;
            unsigned off=arm ? 8 : bank==3 && bit ? 4 : 4+2*bit;
            bytes[off]=(uint8_t)(0x80+trial);bytes[off+1]=(uint8_t)(0xff-trial);
            devices.dma_access=false;devices.dma_bus_valid=false;
            machine.cpu.r[1]=0x040000d8;machine.now=0;
            CHECK((arm ? gbn_enter_arm(&machine,pc) : gbn_enter_thumb(&machine,pc))==GBN_STEP);
            compare_device_write(1,0x730000|align<<12|bank<<8|arm<<7|bit<<6|trial);
        }
    }
    printf("PASS native open-bus RAM tail: %u comparisons; all host alignments, live fetch bytes outside snapshots, IWRAM previous-halfword and ARM word alignment\n",comparisons-before);
}
static void open_bus_writeback(void) {
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    unsigned before=comparisons;
    for(unsigned form=0;form<4;++form)for(unsigned latch=0;latch<4;++latch) {
        put32(0,form==0 ? 0xe5b10004 : form==1 ? 0xe5b11004 : form==2 ? 0xe4910004 : 0xe4911004);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        machine.cpu.r[1]=form<2 ? 0x040000d0 : 0x040000d4;
        CHECK(gbn_enter_arm(&machine,0x08000000)==GBN_STEP);
        devices.dma_access=latch==3;devices.dma_bus_valid=latch!=0;
        devices.dma_bus=random_word();devices.dma_pc=latch&1?0x08000004:0x08000008;
        uint64_t native=backend.native_instructions;
        compare_device_write(1,0x740000|form<<4|latch);CHECK(backend.native_instructions==native+1);
    }
    printf("PASS native open-bus ARM writeback: %u comparisons; pre/post updates and base/destination aliases\n",comparisons-before);
}
static void open_bus_stale_prefetch(void) {
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    put16(0,0xbe00);put16(2,0x6808);put16(4,0xbe00);put16(6,0xbe00);
    memcpy(iwram,rom,16);devices.dma_access=false;devices.dma_bus_valid=false;
    machine.cpu.r[1]=0x040000d4;
    CHECK(gbn_enter_thumb(&machine,0x03000002)==GBN_STEP);compare_device_write(1,0x750000);
    /* Warm single-instruction block snapshots omit its prefetched tail.
     * Keep the old pipeline but modify that halfword before re-entry. */
    CHECK(gbn_enter_thumb(&machine,0x03000002)==GBN_STEP);
    iwram[4]=1;
    compare_device_write(1,0x750001);
    /* A fresh branch/entry must instead observe the changed halfword. */
    CHECK(gbn_enter_thumb(&machine,0x03000002)==GBN_STEP);
    compare_device_write(1,0x750002);
    puts("PASS native open-bus stale IWRAM prefetch: 3 comparisons; warm omitted tail, external write, old pipeline and refreshed entry");
}
