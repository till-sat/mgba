/* Exact state comparison with the frozen one-beat DMA implementation. */
#include "gba-next-dma-reference.h"

struct SpanFixture {
    struct Gbn m;
    struct GbnDevices d;
    uint8_t ew[GBN_EWRAM_SIZE+4], iw[GBN_IWRAM_SIZE+4];
    uint8_t pal[GBN_PALETTE_SIZE+4], vr[GBN_VRAM_SIZE+4], oa[GBN_OAM_SIZE+4];
};
static struct SpanFixture span_a,span_b;
static uint8_t span_rom[8192];
static unsigned span_checks,span_cases;

static enum GbnStatus span_reference_events(struct Gbn* m) {
    struct GbnDevices* d=m->devices;
    bool advanced=false;
    for(;;) {
        int id=m->next_event;
        if(id<0) return d->halted?GBN_EVENT:GBN_STEP;
        if((int32_t)(m->now-m->events[id].when)<0) {
            if(!d->dma_blocked && (!d->halted || advanced)) return GBN_STEP;
            m->now=m->events[id].when;advanced=true;
        }
        if(id<(int)GBN_EVENT_SAVE) return GBN_EVENT;
        if(id==GBN_EVENT_SAVE) { gbn_cancel(m,GBN_EVENT_SAVE);continue; }
        CHECK(id==GBN_EVENT_DMA);
        enum GbnStatus s=span_ref_service(m);
        if(s!=GBN_STEP) return s;
    }
}
static void span_fixture_init(struct SpanFixture* f,unsigned trial,unsigned misalign) {
    for(unsigned i=0;i<sizeof(f->ew);++i) f->ew[i]=(uint8_t)(i*17+(i>>8)+trial);
    for(unsigned i=0;i<sizeof(f->iw);++i) f->iw[i]=(uint8_t)(i*29+(i>>7)+trial);
    for(unsigned i=0;i<sizeof(f->pal);++i) f->pal[i]=(uint8_t)(i*43+trial);
    for(unsigned i=0;i<sizeof(f->vr);++i) f->vr[i]=(uint8_t)(i*71+(i>>6)+trial);
    for(unsigned i=0;i<sizeof(f->oa);++i) f->oa[i]=(uint8_t)(i*11+trial);
    gbn_init(&f->m,f->ew+misalign,f->iw+misalign);
    CHECK(gbn_attach_devices(&f->m,&f->d,f->pal+misalign,f->vr+misalign,f->oa+misalign));
    CHECK(gbn_attach_rom(&f->m,span_rom,sizeof(span_rom)));
    f->m.now=trial&1?0xffffffe0u:0x100u;
    f->m.ewram_wait=(uint8_t)(trial%8);
    gbn_set_waitcnt(&f->m,(uint16_t)(trial*0x137u&0x5fff));
    CHECK(gbn_enter_thumb(&f->m,0x03000100)==GBN_STEP);
    f->d.io[0]=(trial&4)?3:0;
    f->d.halted=(trial&8)!=0;
}
static void span_configure(struct Gbn* m,unsigned channel,uint32_t src,uint32_t dst,unsigned count,unsigned control) {
    uint32_t cycles,base=0x040000b0+channel*12;
    CHECK(gbn_write(m,base,4,src,&cycles)==GBN_STEP);
    CHECK(gbn_write(m,base+4,4,dst,&cycles)==GBN_STEP);
    CHECK(gbn_write(m,base+8,4,count|(control<<16),&cycles)==GBN_STEP);
}
static void span_compare(void) {
    struct Gbn b=span_b.m;struct GbnDevices d=span_b.d;
    b.ewram=span_a.m.ewram;b.iwram=span_a.m.iwram;b.devices=&span_a.d;
    if(b.code==span_b.m.ewram)b.code=span_a.m.ewram;
    if(b.code==span_b.m.iwram)b.code=span_a.m.iwram;
    d.palette=span_a.d.palette;d.vram=span_a.d.vram;d.oam=span_a.d.oam;
    if(memcmp(&span_a.m,&b,sizeof(b)) || memcmp(&span_a.d,&d,sizeof(d))) {
        fprintf(stderr,"DMA span mismatch case=%u checkpoint=%u now=%08x/%08x next=%d/%d remaining=%u/%u when=%08x/%08x\n",span_cases,span_checks,span_a.m.now,b.now,span_a.m.next_event,b.next_event,span_a.d.dma[3].remaining,d.dma[3].remaining,span_a.d.dma[3].when,d.dma[3].when);
        CHECK(0);
    }
    ++span_checks;
}
static void check_dma_spans(void) {
    static const unsigned sr[]={2,3,5,6,7,8,10,12};
    static const unsigned dr[]={2,3,5,6,7};
    for(unsigned i=0;i<sizeof(span_rom);++i)span_rom[i]=(uint8_t)(i*97+(i>>5));
    for(unsigned w=0;w<2;++w)for(unsigned si=0;si<8;++si)for(unsigned di=0;di<5;++di)
    for(unsigned sm=0;sm<4;++sm)for(unsigned dm=0;dm<4;++dm) {
        unsigned trial=span_cases++,width=w?4:2,misalign=trial%37==0?1:0;
        span_fixture_init(&span_a,trial,misalign);span_fixture_init(&span_b,trial,misalign);
        uint32_t source=(sr[si]<<24)|0x180,dest=(dr[di]<<24)|0x180;
        /* Half the cases cross physical mirror/ROM-hole boundaries. */
        if(trial&2) {
            unsigned size=sr[si]==2?GBN_EWRAM_SIZE:sr[si]==3?GBN_IWRAM_SIZE:sr[si]==6?0x18000:sr[si]>=8?sizeof(span_rom):GBN_PALETTE_SIZE;
            source=(sr[si]<<24)|(sm==1?width*2:size-width*2);
            size=dr[di]==2?GBN_EWRAM_SIZE:dr[di]==3?GBN_IWRAM_SIZE:dr[di]==6?0x18000:GBN_PALETTE_SIZE;
            dest=(dr[di]<<24)|(dm==1?width*2:size-width*2);
        }
        /* Same-bank forward/backward overlap, including source==dest. */
        if(sr[si]==dr[di] && !(trial&2))dest=source+(trial%3)*width;
        unsigned control=0xc000|(w<<10)|(sm<<7)|(dm<<5);
        span_configure(&span_a.m,3,source,dest,33,control);
        span_configure(&span_b.m,3,source,dest,33,control);
        CHECK(gbn_schedule(&span_a.m,0,3+(trial%17),trial%3==0?0:trial%3==1?0x40:0xff));
        CHECK(gbn_schedule(&span_b.m,0,3+(trial%17),trial%3==0?0:trial%3==1?0x40:0xff));
        span_a.m.now+=3+trial%23;span_b.m.now+=3+trial%23;
        unsigned cuts=0;
        for(;;) {
            enum GbnStatus a=gbn_service_events(&span_a.m),b=span_reference_events(&span_b.m);
            CHECK(a==b);span_compare();
            if(a!=GBN_STEP && a!=GBN_EVENT)break;
            if(!span_a.m.events[GBN_EVENT_DMA].active)break;
            CHECK(a==GBN_EVENT && span_a.m.next_event==0 && ++cuts<8);
            CHECK(gbn_take_event(&span_a.m,NULL)==0 && gbn_take_event(&span_b.m,NULL)==0);
            if(cuts==1 && trial%7==0) {
                span_configure(&span_a.m,0,0x03000200,0x02000300,3,0x8400);
                span_configure(&span_b.m,0,0x03000200,0x02000300,3,0x8400);
            }
            if(cuts<3) {
                unsigned delay=1+cuts*11,priority=cuts==1?0x40:0;
                CHECK(gbn_schedule(&span_a.m,0,delay,priority));CHECK(gbn_schedule(&span_b.m,0,delay,priority));
            }
        }
        CHECK(!memcmp(span_a.ew,span_b.ew,sizeof(span_a.ew)));
        CHECK(!memcmp(span_a.iw,span_b.iw,sizeof(span_a.iw)));
        CHECK(!memcmp(span_a.pal,span_b.pal,sizeof(span_a.pal)));
        CHECK(!memcmp(span_a.vr,span_b.vr,sizeof(span_a.vr)));
        CHECK(!memcmp(span_a.oa,span_b.oa,sizeof(span_a.oa)));
    }
    static const uint32_t edges[][2]={
        {0x02fffff0,0x03000100},{0x03fffff0,0x02000100},
        {0x08000100,0x02fffff0},{0x08000100,0x03fffff0},
        {0x06017ff0,0x03000100},{0x0601bff0,0x03000100},{0x0601fff0,0x03000100},
        {0x03000100,0x06017ff0},{0x03000100,0x0601bff0},{0x03000100,0x0601fff0},
        {0x05fffff0,0x03000100},{0x03000100,0x07fffff0}
    };
    for(unsigned edge=0;edge<12;++edge)for(unsigned w=0;w<2;++w)for(unsigned rev=0;rev<2;++rev) {
        unsigned trial=span_cases++,width=w?4:2;
        span_fixture_init(&span_a,trial,0);span_fixture_init(&span_b,trial,0);
        span_a.m.ewram_wait=span_b.m.ewram_wait=255;
        span_a.d.io[0]=span_b.d.io[0]=rev?3:0;
        uint32_t src=edges[edge][0]+(rev?32:0),dst=edges[edge][1]+(rev?32:0);
        unsigned control=0xc000|(w<<10)|(rev?0xa0:0);
        span_configure(&span_a.m,3,src,dst,33,control);span_configure(&span_b.m,3,src,dst,33,control);
        unsigned cut=3+width*17;
        CHECK(gbn_schedule(&span_a.m,0,cut,0x40));CHECK(gbn_schedule(&span_b.m,0,cut,0x40));
        span_a.m.now+=3;span_b.m.now+=3;
        enum GbnStatus a=gbn_service_events(&span_a.m),b=span_reference_events(&span_b.m);
        CHECK(a==b);span_compare();
        if(a==GBN_EVENT && span_a.m.next_event==0) {
            CHECK(gbn_take_event(&span_a.m,NULL)==0 && gbn_take_event(&span_b.m,NULL)==0);
            a=gbn_service_events(&span_a.m);b=span_reference_events(&span_b.m);CHECK(a==b);span_compare();
        }
        CHECK(!memcmp(span_a.ew,span_b.ew,sizeof(span_a.ew)));
        CHECK(!memcmp(span_a.iw,span_b.iw,sizeof(span_a.iw)));
        CHECK(!memcmp(span_a.pal,span_b.pal,sizeof(span_a.pal)));
        CHECK(!memcmp(span_a.vr,span_b.vr,sizeof(span_a.vr)));
        CHECK(!memcmp(span_a.oa,span_b.oa,sizeof(span_a.oa)));
    }
    printf("PASS DMA span reference: %u cases, %u complete-state checkpoints; overlap, mirrors, video holes, ROM windows, fixed/decrement, host alignment, late start, clock wrap, priority ties and preemption\n",span_cases,span_checks);
}
