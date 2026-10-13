/* SPDX-License-Identifier: MPL-2.0 */
/* Included by the RV32 differential test; actual emitted machine code. */
#include "mixer-packet-rom-fixture.h"
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

static void immediate_io_reads(void) {
    static const unsigned codes[]={0x6808,0x8808,0x7808};
    static const unsigned regions[]={2,3,8};
    unsigned before=comparisons;
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    for(unsigned i=0;i<sizeof(devices.io)/sizeof(*devices.io);++i)
        devices.io[i]=(uint16_t)random_word();
    /* Exercise the no-address-branch literal case on an open-bus DMA
     * register; the helper must restore LHS before choosing the bus value. */
    put16(0,0x4907);put16(2,0x6808);put32(32,0x040000d4);
    CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
    machine.cpu.r[1]=0x02000000;machine.cpu.r[0]=random_word();
    devices.dma_bus_valid=true;devices.dma_access=true;devices.dma_bus=random_word();
    devices.dma_pc=0x08000006;machine.now=0xfffffff0;
    CHECK(gbn_enter_thumb(&machine,0x08000000)==GBN_STEP);
    compare_device_write(8,0x75f000);
    /* Classify on cold entry, then reuse exactly that block with changed
     * IO values, DMA selection, odd addresses and a different memory region.
     * Sweep the interpreter's entire IO map rather than duplicate its table. */
    for(unsigned bank=0;bank<3;++bank)for(unsigned kind=0;kind<3;++kind)
    for(unsigned offset=0;offset<0x240;++offset) {
        put16(0,codes[kind]);put16(2,0xbe00);put16(4,0x80ff);
        memcpy(ewram,rom,16);memcpy(iwram,rom,16);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        gbn_set_waitcnt(&machine,(uint16_t)(offset&1?0x4010:0));
        for(unsigned trial=0;trial<6;++trial) {
            machine.cpu.r[0]=random_word();machine.cpu.r[1]=0x04000000+offset;
            if(trial==3)machine.cpu.r[1]^=1;
            if(trial==4)machine.cpu.r[1]=0x02000100;
            machine.cpu.cpsr=(random_word()&~63u)|63;machine.now=0xfffffff0;
            devices.dma_access=trial==2;devices.dma_bus_valid=trial!=0;
            devices.dma_bus=random_word();devices.dma_pc=(regions[bank]<<24)+(trial==1?2:6);
            if(offset<sizeof(devices.io))devices.io[offset/2]=(uint16_t)random_word();
            CHECK(gbn_enter_thumb(&machine,regions[bank]<<24)==GBN_STEP);
            uint64_t native=backend.native_instructions;
            compare_device_write(1,0x76000000|bank<<20|kind<<18|offset<<3|trial);
            if(offset==0xd4 && (trial<3 || trial==5) && backend.native_instructions!=native+1) {
                printf("IO native miss: bank=%u kind=%u trial=%u pc=%08x now=%u next=%d\n",
                       regions[bank],kind,trial,machine.cpu.pc,machine.now,machine.next_event);
                CHECK(backend.native_instructions==native+1);
            }
        }
    }
    printf("PASS immediate IO loads: %u comparisons; complete IO map, widths, live values, warm address changes, DMA latch/access/PC, RAM/ROM code and clock wrap\n",comparisons-before);
}

static void immediate_dma_writes(void) {
    static const unsigned regions[]={2,3,8};
    unsigned before=comparisons;
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    for(unsigned bank=0;bank<3;++bank)for(unsigned channel=0;channel<4;++channel)
    for(unsigned dest=0;dest<2;++dest)for(unsigned literal=0;literal<2;++literal) {
        /* Literal entry deliberately starts with a different base. The
         * source/destination readback must retain its own visible bus PC. */
        put16(0,literal?0x4907:0x6008);put16(2,literal?0x6008:0x680a);
        put16(4,literal?0x680a:0xbe00);put16(6,0xbe00);
        uint32_t address=0x040000b0+12*channel+4*dest;put32(32,address);
        memcpy(ewram,rom,40);memcpy(iwram,rom,40);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        for(unsigned trial=0;trial<4;++trial)for(unsigned cap=1;cap<4;++cap)
        for(unsigned distance=0;distance<16;++distance) {
            CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
            memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
            machine.cpu.r[0]=random_word();machine.cpu.r[1]=literal?0x02001000:address;
            if(!literal && trial==3)machine.cpu.r[1]=0x040000b0+12*((channel+1)%4)+4*dest;
            machine.cpu.cpsr=(random_word()&~63u)|63;machine.now=0xfffffff0;
            gbn_set_waitcnt(&machine,(uint16_t)(trial&1?0x4010:0));
            devices.dma_bus_valid=trial!=0;devices.dma_access=trial==2;
            devices.dma_bus=random_word();devices.dma_pc=(regions[bank]<<24)+(literal?6:4);
            devices.dma[channel].source=random_word();devices.dma[channel].dest=random_word();
            devices.dma[channel].next_source=random_word();devices.dma[channel].next_dest=random_word();
            devices.dma[channel].remaining=17;devices.dma[channel].control=0x8200;
            CHECK(gbn_enter_thumb(&machine,regions[bank]<<24)==GBN_STEP);
            CHECK(gbn_schedule(&machine,0,distance,1));
            compare_device_write(cap,0x77000000|bank<<20|channel<<16|dest<<15|literal<<14|trial<<10|cap<<6|distance);
        }
    }
    printf("PASS immediate DMA programming: %u comparisons; all address masks/channels, literal and entry bases, live readback, warm base changes and every event/cap cut\n",comparisons-before);
}

static void consecutive_io_prefetch(void) {
    static const uint16_t waits[]={0,0x4000,0x4010,0x401c};
    static const unsigned deadlines[]={0,1,2,3,4,5,8,13,21,64};
    unsigned before=comparisons;
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    for(unsigned gap=0;gap<3;++gap) {
        /* Mix programming, open-bus reads and stored/zero count reads.
         * RAM and ROM loads between IO operations must break the proof. */
        put16(0,0x6008);put16(2,0x680a);
        put16(4,gap==1?0x6818:gap==2?0x4806:0x6048);
        put16(6,0x684a);put16(8,0x688a);put16(10,0x680a);put16(12,0xbe00);
        put32(32,0x02000100);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        for(unsigned config=0;config<4;++config)for(unsigned queue=0;queue<20;++queue)
        for(unsigned cap=0;cap<=6;++cap)for(unsigned event=0;event<10;++event) {
            CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
            memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
            machine.cpu.r[0]=random_word();machine.cpu.r[1]=0x040000d4;
            machine.cpu.r[2]=random_word();machine.cpu.r[3]=0x02000100;
            machine.cpu.cpsr=(random_word()&~63u)|63;machine.now=0xfffffff0;
            gbn_set_waitcnt(&machine,waits[config]);
            devices.dma_access=queue&1;devices.dma_bus_valid=queue&2;
            devices.dma_bus=random_word();devices.dma_pc=0x08000004+2*(queue%6);
            CHECK(gbn_enter_thumb(&machine,0x08000000)==GBN_STEP);
            machine.prefetched_pc=queue==18?0:queue==19?UINT32_MAX:0x08000003+queue;
            CHECK(gbn_schedule(&machine,0,deadlines[event],1));
            compare_device_write(cap,0x78000000|gap<<20|config<<16|queue<<10|cap<<5|event);
        }
    }
    CHECK(backend.fused_thumb_io_instructions > 0);
    printf("PASS consecutive IO prefetch: %u comparisons; queue alignment/range, RAM/ROM interruptions, waits, caps/events, DMA readback and clock wrap\n",comparisons-before);
    printf("Grouped Thumb IO instructions: %llu\n", backend.fused_thumb_io_instructions);
}

static void grouped_io_boundaries(void) {
    static const unsigned lengths[]={3,4,7,8,9};
    static const unsigned caps[]={1,2,3,4,6,7,8,9,10,32};
    static const unsigned times[]={0,1,2,3,4,5,6,7,8,13,14,15,16,17,24,64};
    static const uint16_t ops[]={0x6008,0x680a,0x604a,0x6848,0x688a};
    unsigned before=comparisons;
    setup();CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
    for(unsigned l=0;l<5;++l)for(unsigned channel=0;channel<4;++channel)
    for(unsigned literal=0;literal<2;++literal) {
        unsigned n=lengths[l];uint32_t base=0x040000b0+channel*12;
        if(literal)put16(0,0x490f);
        for(unsigned i=0;i<n;++i)put16(2*(i+literal),ops[i%5]);
        put16(2*(n+literal),0xbe00);put32(64,base);
        CHECK(gbn_attach_rom(&machine,rom,sizeof(rom)));
        for(unsigned config=0;config<2;++config)for(unsigned trial=0;trial<4;++trial)
        for(unsigned c=0;c<10;++c)for(unsigned t=0;t<16;++t) {
            CHECK(gbn_attach_devices(&machine,&devices,palette,vram,oam));
            memset(machine.events,0,sizeof(machine.events));machine.next_event=-1;
            machine.cpu.r[0]=random_word();machine.cpu.r[2]=random_word();
            machine.cpu.r[1]=trial==1?0x040000b0+12*((channel+1)%4):base+(trial==2);
            machine.cpu.cpsr=(random_word()&~63u)|63;machine.now=trial&1?0xfffffff0:0;
            gbn_set_waitcnt(&machine,config?0x4010:0x4000);
            devices.dma_access=trial==3;devices.dma_bus_valid=trial!=0;
            devices.dma_bus=random_word();devices.dma_pc=0x08000000+2*(literal+t%(n+1));
            devices.dma[channel].source=random_word();devices.dma[channel].dest=random_word();
            devices.dma[channel].next_source=random_word();devices.dma[channel].next_dest=random_word();
            devices.dma[channel].remaining=17;devices.dma[channel].control=0x8200;
            devices.io[(base-0x04000000+10)/2]=0x8200;
            CHECK(gbn_enter_thumb(&machine,0x08000000)==GBN_STEP);
            machine.prefetched_pc=0x08000004+2*literal+7*(trial%3);
            CHECK(gbn_schedule(&machine,0,times[t],1));
            uint64_t grouped=backend.fused_thumb_io_instructions;
            compare_device_write(caps[c],0x79000000|l<<20|channel<<18|literal<<17|config<<16|trial<<12|c<<6|t);
            if(trial==0 && !literal && caps[c]>=n && times[t]==64)
                CHECK(backend.fused_thumb_io_instructions>=grouped+(n==9?8:n));
        }
    }
    printf("PASS grouped IO boundaries: %u comparisons;3..9 operations,all DMA channels,read-to-store dependencies,literal/warm base changes,misalignment,queue endpoints,cap/event cuts and wrap\n",comparisons-before);
}
