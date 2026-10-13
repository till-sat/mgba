/* SPDX-License-Identifier: MPL-2.0 */
#include <gba-next/core.h>
#include "internal.h"
#include <stdio.h>
#include <stddef.h>
#ifdef AUDIO_TEST_NATIVE
#if !GBN_RV32 || !GBN_RV32_STATS
#error Build this test with GBN_RV32=1 GBN_RV32_STATS=1
#endif
#include <gba-next/rv32.h>
static struct GbnRv32 backend;
static uint64_t native_total;
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct Fixture {
    struct Gbn m;struct GbnDevices d;
    _Alignas(4) uint8_t ew[GBN_EWRAM_SIZE],iw[GBN_IWRAM_SIZE];
    uint8_t palette[GBN_PALETTE_SIZE],vram[GBN_VRAM_SIZE],oam[GBN_OAM_SIZE];
};
static struct Fixture eager,candidate;
static unsigned tag,checks;
static const char* section;
#define CHECK(x) do {++checks;if(!(x)){printf("FAIL audio timeline section=%s tag=%u line=%u: %s\n",section,tag,(unsigned)__LINE__,#x);exit(1);}}while(0)
static void write_io(struct Fixture* f,unsigned off,unsigned width,uint32_t v) {
    uint32_t cycles;CHECK(gbn_write(&f->m,0x04000000u+off,width,v,&cycles)==GBN_STEP);
}
static void put16(uint8_t* bytes,unsigned off,unsigned v) {bytes[off]=(uint8_t)v;bytes[off+1]=(uint8_t)(v>>8);}
static void initialize(struct Fixture* f,uint32_t start,unsigned period,unsigned timer,unsigned size,unsigned rate,unsigned mode) {
    memset(f,0,sizeof(*f));gbn_init(&f->m,f->ew,f->iw);
    CHECK(gbn_attach_devices(&f->m,&f->d,f->palette,f->vram,f->oam));f->m.now=start;
    write_io(f,0x84,2,mode==1?0:0x80);write_io(f,0x80,2,mode==2?0xff77:0);
    write_io(f,0x82,2,0x330cu|(timer?0x4400:0));write_io(f,0x88,2,0x200u|(rate<<14));
    for(unsigned i=0;i<2;++i) {
        struct GbnFifo* q=&f->d.audio.fifo[i];q->read=29;q->size=(uint8_t)size;q->sample=i?-93:71;
        for(unsigned n=0;n<32;++n)q->data[n]=(uint8_t)(n*47+i*19+0x85);
    }
    for(unsigned i=0;i<4;++i) {
        struct GbnPsgChannel* c=&f->d.audio.psg[i];c->enabled=c->dac=true;
        c->frequency=1980+7*i;c->updated=start;c->volume=i==2?2:13;c->noise=0x25;c->lfsr=0x4561;
        c->length=13;c->length_enable=true;c->envelope=0x19;c->envelope_ticks=2;c->duty=i&3;
    }
    for(unsigned n=0;n<32;++n)f->d.audio.wave[n]=(uint8_t)(n*29+11);
    write_io(f,0x100+timer*4,4,(mode==3?0x00c00000u:0x00800000u)|(0x10000u-period));
    gbn_audio_start(&f->m);gbn_audio_set_batched(&f->m,true);
    CHECK(gbn_service_events(&f->m)==GBN_STEP);
    for(unsigned n=0;n<512;++n)put16(f->iw,n*2,0x46c0);
    put16(f->iw,1022,0xe5ff);
    CHECK(gbn_enter_thumb(&f->m,0x03000000)==GBN_STEP);f->m.cpu.cpsr|=0x80;
}
static void pair(uint32_t start,unsigned period,unsigned timer,unsigned size,unsigned rate,unsigned mode) {
    initialize(&eager,start,period,timer,size,rate,mode);initialize(&candidate,start,period,timer,size,rate,mode);
    CHECK(!eager.d.audio.timeline&&!candidate.d.audio.timeline);
    gbn_audio_set_timeline(&candidate.m,true);
}
static void compare(bool resume) {
    /* Canonicalize through public APIs; never mask the detached/tie/event fields. */
    gbn_audio_timeline_materialize(&candidate.m);gbn_audio_set_timeline(&candidate.m,false);
    gbn_audio_sync(&eager.m);gbn_audio_sync(&candidate.m);
    struct Gbn m=candidate.m;struct GbnDevices d=candidate.d;
    m.devices=&eager.d;m.ewram=eager.ew;m.iwram=eager.iw;
    if(m.code==candidate.iw)m.code=eager.iw;
    if(m.code==candidate.ew)m.code=eager.ew;
    d.palette=eager.palette;d.vram=eager.vram;d.oam=eager.oam;
    if(memcmp(&m,&eager.m,sizeof(m))||memcmp(&d,&eager.d,sizeof(d))) {
        printf("state now=%08x/%08x PCM=%u/%u size=%u/%u next=%d/%d AUDIO=%u/%u active=%u/%u\n",
            candidate.m.now,eager.m.now,candidate.d.audio.produced,eager.d.audio.produced,
            candidate.d.audio.size,eager.d.audio.size,candidate.m.next_event,eager.m.next_event,
            candidate.m.events[GBN_EVENT_AUDIO].when,eager.m.events[GBN_EVENT_AUDIO].when,
            candidate.m.events[GBN_EVENT_AUDIO].active,eager.m.events[GBN_EVENT_AUDIO].active);
        const uint8_t *a=(const uint8_t*)&eager.m,*b=(const uint8_t*)&m;
        for(unsigned n=0;n<sizeof(m);++n)if(a[n]!=b[n]){printf("machine byte %u = %02x/%02x\n",n,a[n],b[n]);break;}
        a=(const uint8_t*)&eager.d;b=(const uint8_t*)&d;
        for(unsigned n=0;n<sizeof(d);++n)if(a[n]!=b[n]){printf("device byte %u = %02x/%02x\n",n,a[n],b[n]);break;}
        printf("audio offset=%zu samples=%zu psg=%zu produced=%u/%u\n",offsetof(struct GbnDevices,audio),offsetof(struct GbnAudio,samples),offsetof(struct GbnAudio,psg),eager.d.audio.produced,candidate.d.audio.produced);
        for(unsigned n=0;n<4;++n)printf("slot%u L=%d/%d R=%d/%d when=%u/%u\n",n,eager.d.audio.samples[n].left,candidate.d.audio.samples[n].left,eager.d.audio.samples[n].right,candidate.d.audio.samples[n].right,eager.d.audio.samples[n].when,candidate.d.audio.samples[n].when);
        CHECK(0);
    }
    CHECK(!memcmp(eager.ew,candidate.ew,sizeof(eager.ew)));CHECK(!memcmp(eager.iw,candidate.iw,sizeof(eager.iw)));
    CHECK(!memcmp(eager.palette,candidate.palette,sizeof(eager.palette)));CHECK(!memcmp(eager.vram,candidate.vram,sizeof(eager.vram)));
    CHECK(!memcmp(eager.oam,candidate.oam,sizeof(eager.oam)));
    if(resume)gbn_audio_set_timeline(&candidate.m,true);
}
static void advance(struct Fixture* f,uint32_t target) {
    for(unsigned guard=0;f->m.next_event>=0&&(int32_t)(target-f->m.events[f->m.next_event].when)>=0;++guard) {
        CHECK(guard<50000);uint32_t when=f->m.events[f->m.next_event].when;
        if((int32_t)(when-f->m.now)>0)f->m.now=when;
        CHECK(gbn_service_events(&f->m)==GBN_STEP);CHECK((int32_t)(target-f->m.now)>=0);
    }
    f->m.now=target;
}
static void sequence_cases(void) {
    section="sequence";const unsigned periods[]={4,64,256,1064};const unsigned sizes[]={0,16,32};
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned p=0;p<4;++p)for(unsigned size=0;size<3;++size)
    for(unsigned timer=0;timer<2;++timer)for(unsigned rate=0;rate<4;++rate)for(unsigned mode=0;mode<4;++mode) {
        ++tag;uint32_t start=wrap?0xfffff000u:0;pair(start,periods[p],timer,sizes[size],rate,mode);
        for(unsigned n=1;n<=4;++n){uint32_t target=start+n*(periods[p]*9+33);advance(&eager,target);advance(&candidate,target);compare(true);}
    }
}
static void priority_cases(void) {
    section="priority";const unsigned priorities[]={0,0x10,0x17,0x18,0x19,0x20,0x40};
    for(unsigned timer=0;timer<2;++timer)for(unsigned p=0;p<7;++p)for(unsigned wrap=0;wrap<2;++wrap) {
        ++tag;uint32_t start=wrap?0xfffffff0u:0;pair(start,64,timer,32,3,0);
        uint32_t when=start+64;
        CHECK(gbn_schedule_at(&eager.m,GBN_EVENT_TIMER0+timer,when,priorities[p]));
        CHECK(gbn_schedule_at(&candidate.m,GBN_EVENT_TIMER0+timer,when,priorities[p]));
        advance(&eager,when);advance(&candidate,when);compare(false);
    }
}
static void tie_cases(void) {
    section="tie";
    for(unsigned audio_wins=0;audio_wins<2;++audio_wins)for(unsigned wrap=0;wrap<2;++wrap) {
        ++tag;uint32_t start=wrap?0xfffffff0u:0;pair(start,1064,0,32,3,0);
        gbn_audio_set_timeline(&candidate.m,false);
        for(unsigned side=0;side<2;++side) {
            struct Fixture* f=side?&candidate:&eager;
            gbn_cancel(&f->m,GBN_EVENT_AUDIO);uint32_t when=start+64;
            if(audio_wins)CHECK(gbn_schedule_at(&f->m,GBN_EVENT_AUDIO,when,0x18));
            CHECK(gbn_schedule_at(&f->m,0,when,0x18));
            if(!audio_wins)CHECK(gbn_schedule_at(&f->m,GBN_EVENT_AUDIO,when,0x18));
            CHECK(f->m.next_event==(audio_wins?(int)GBN_EVENT_AUDIO:0));
        }
        gbn_audio_set_timeline(&candidate.m,true);
        eager.m.now=candidate.m.now=start+64;
        CHECK(gbn_service_events(&eager.m)==GBN_EVENT);CHECK(gbn_service_events(&candidate.m)==GBN_EVENT);
        struct GbnStereo a[4],b[4];unsigned an=gbn_audio_read(&eager.m,a,4),bn=gbn_audio_read(&candidate.m,b,4);
        if(an!=bn||memcmp(a,b,an*sizeof(*a))) {
            printf("tie audio_wins=%u wrap=%u count=%u/%u next=%d/%d detached=%u tie=%u produced=%u/%u\n",audio_wins,wrap,an,bn,eager.m.next_event,candidate.m.next_event,candidate.d.audio.pcm_detached,candidate.d.audio.pcm_tie,eager.d.audio.produced,candidate.d.audio.produced);
            for(unsigned n=0;n<an||n<bn;++n)printf("read%u L=%d/%d R=%d/%d when=%u/%u\n",n,a[n].left,b[n].left,a[n].right,b[n].right,a[n].when,b[n].when);
        }
        CHECK(an==bn&&!memcmp(a,b,an*sizeof(*a)));compare(false);
    }
}
static void write_cases(void) {
    section="write";static const struct {unsigned off,width;uint32_t value;} ops[]={
        {0x88,2,0xc200},{0x88,2,0x0200},{0x80,2,0xff77},{0x80,2,0},
        {0x84,1,0},{0x84,1,0x80},{0x82,2,0x3b0c},{0x82,2,0x770c},
        {0xa0,1,0x91},{0xa4,2,0x1397},{0xa0,4,0x5387abcd},{0x90,4,0x12345678}
    };
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned op=0;op<sizeof(ops)/sizeof(*ops);++op) {
        ++tag;uint32_t start=wrap?0xfffffff0u:0;pair(start,1064,0,32,3,0);
        advance(&eager,start+97);advance(&candidate,start+97);
        eager.m.cpu_access=candidate.m.cpu_access=true;
        write_io(&eager,ops[op].off,ops[op].width,ops[op].value);write_io(&candidate,ops[op].off,ops[op].width,ops[op].value);
        eager.m.cpu_access=candidate.m.cpu_access=false;
        compare(true);advance(&eager,start+65536);advance(&candidate,start+65536);compare(false);
    }
}
static void run_cpu(struct Fixture* f,unsigned total,unsigned cap) {
    unsigned done=0,guard=0;
#ifdef AUDIO_TEST_NATIVE
    if(f==&candidate)gbn_rv32_init(&backend,&f->m);
#endif
    while(done<total) {
        CHECK(++guard<50000);CHECK(gbn_service_events(&f->m)==GBN_STEP);
        uint32_t count=0;unsigned n=total-done;if(n>cap)n=cap;
        enum GbnStatus st;
#ifdef AUDIO_TEST_NATIVE
        st=f==&candidate?gbn_rv32_run_batch(&backend,n,&count):gbn_run_batch(&f->m,n,&count);
#else
        st=gbn_run_batch(&f->m,n,&count);
#endif
        CHECK(st==GBN_STEP||st==GBN_EVENT);done+=count;
    }
    CHECK(gbn_service_events(&f->m)==GBN_STEP);
#ifdef AUDIO_TEST_NATIVE
    if(f==&candidate){CHECK(backend.native_instructions>0);native_total+=backend.native_instructions;}
#endif
}
static void cpu_cases(void) {
    section="cpu";static const unsigned caps[]={1,7,256,4096};
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned c=0;c<4;++c)for(unsigned action=0;action<4;++action) {
        ++tag;pair(wrap?0xfffff000u:0,64,0,32,3,0);
        for(unsigned side=0;side<2;++side) {
            struct Fixture* f=side?&candidate:&eager;
            static const unsigned offs[]={0x82,0xa0,0x88,0x84};static const uint32_t values[]={0x3b0c,0x815739ab,0x0200,0};
            put16(f->iw,194,action==1?0x6008:0x8008);f->m.cpu.r[0]=values[action];f->m.cpu.r[1]=0x04000000+offs[action];
        }
        run_cpu(&eager,384,caps[c]);run_cpu(&candidate,384,caps[c]);compare(false);
    }
}

/* Coalescing a PCM-only HALT return changes the number of service calls, not
 * guest time or observable device effects. Advance the independent eager
 * machine through its old PCM events until the candidate's exact return time.
 * Refuse to cross an IRQ wake-up, an external event, or the candidate time. */
static void service_pair(void) {
    enum GbnStatus b=gbn_service_events(&candidate.m);
    enum GbnStatus a=gbn_service_events(&eager.m);
    for(unsigned guard=0;(int32_t)(candidate.m.now-eager.m.now)>0;++guard) {
        CHECK(guard<10000);CHECK(a==GBN_STEP);
        CHECK(eager.d.halted||eager.d.dma_blocked);
        uint32_t before=eager.m.now;
        a=gbn_service_events(&eager.m);
        CHECK((int32_t)(eager.m.now-before)>0);
        CHECK((int32_t)(candidate.m.now-eager.m.now)>=0);
    }
    CHECK(a==GBN_STEP&&b==GBN_STEP);CHECK(eager.m.now==candidate.m.now);
}

static void halted_cases(void) {
    section="halt";
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned rate=0;rate<4;++rate) {
        ++tag;uint32_t start=wrap?0xfffff000u:0;pair(start,1024,0,32,rate,3);
        advance(&eager,start+700);advance(&candidate,start+700);
        eager.d.halted=candidate.d.halted=true;
        write_io(&eager,0x200,2,8);write_io(&candidate,0x200,2,8);
        for(unsigned n=0;n<6;++n) {
            service_pair();compare(true);
        }
    }
}
static void dma_cases(void) {
    section="dma";
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned kind=0;kind<3;++kind)for(unsigned rate=0;rate<4;++rate) {
        ++tag;uint32_t start=wrap?0xfffff000u:0;pair(start,64,0,32,rate,0);
        advance(&eager,start+113);advance(&candidate,start+113);
        for(unsigned side=0;side<2;++side) {
            struct Fixture* f=side?&candidate:&eager;unsigned ch=kind?1:3;
            for(unsigned n=0;n<4096;++n)f->iw[0x1000+n]=(uint8_t)(n*29+0x87);
            write_io(f,0xb0+ch*12,4,0x03001000);
            write_io(f,0xb4+ch*12,4,kind?0x040000a0u:0x02001000u);
            write_io(f,0xb8+ch*12,4,kind?0xf6000001u:0x84000100u);
            if(kind==2){write_io(f,0xc8,4,0x03001800);write_io(f,0xcc,4,0x040000a4);write_io(f,0xd0,4,0xf6000001);}
        }
        run_cpu(&eager,4096,1024);run_cpu(&candidate,4096,1024);compare(false);
    }
}
static void ring_cases(void) {
    section="ring";
    for(unsigned wrap=0;wrap<2;++wrap) {
        ++tag;uint32_t start=wrap?0xffff0000u:0;pair(start,65536,0,32,3,0);
        advance(&eager,start+131072);advance(&candidate,start+131072);compare(true);
        CHECK(eager.d.audio.dropped>0&&eager.d.audio.size==GBN_AUDIO_CAPACITY);
        struct GbnStereo a[37],b[37];unsigned an=gbn_audio_read(&eager.m,a,37),bn=gbn_audio_read(&candidate.m,b,37);
        CHECK(an==bn&&!memcmp(a,b,an*sizeof(*a)));compare(false);
    }
}

static void dma_observation_cases(void) {
    section="DMA observation";
    const unsigned phases[]={0,1,61,63,64,65,255};
    const unsigned offsets[]={0x80,0x82,0x84,0x88,0x90,0};
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned rate=0;rate<4;++rate)
    for(unsigned ph=0;ph<7;++ph)for(unsigned target=0;target<6;++target)
    for(unsigned halt=0;halt<2;++halt) {
        ++tag;uint32_t start=wrap?0xfffffff0u:0;
        pair(start,1064,0,32,rate,0);
        advance(&eager,start+phases[ph]);advance(&candidate,start+phases[ph]);
        for(unsigned side=0;side<2;++side) {
            struct Fixture* f=side?&candidate:&eager;
            for(unsigned n=0;n<128;++n) {
                unsigned value=(n*73+0x123)&65535;
                if(target==0)value=n&1?0xff77:0;
                if(target==1)value=n&1?0x3b0c:0x770c;
                if(target==2)value=n&1?0x80:0;
                if(target==3)value=n&1?0xc300:0x0100;
                put16(f->iw,0x1000+n*2,value);
            }
            f->d.halted=halt;
            write_io(f,0xd4,4,0x03001000);
            write_io(f,0xd8,4,target==5?0x06000000:0x04000000+offsets[target]);
            write_io(f,0xdc,4,target==5?0x84000040:0x80400008);
        }
        for(unsigned guard=0;eager.d.active_dma>=0||candidate.d.active_dma>=0;++guard) {
            CHECK(guard<10000);
            /* With a running CPU, the public service does not advance to a
             * future pending DMA. Supply the same next observation time. */
            if(!eager.d.halted&&!eager.d.dma_blocked&&eager.m.next_event>=0) {
                uint32_t when=eager.m.events[eager.m.next_event].when;
                if((int32_t)(when-eager.m.now)>0)eager.m.now=candidate.m.now=when;
            }
            service_pair();
            compare(true);
        }
        eager.d.halted=candidate.d.halted=false;
        uint32_t next=eager.m.now+4096;
        advance(&eager,next);advance(&candidate,next);compare(false);
    }
}


static void halt_observation_cases(void) {
    section="HALT real observer";
    for(unsigned wrap=0;wrap<2;++wrap)for(unsigned rate=0;rate<4;++rate)
    for(unsigned irq=0;irq<2;++irq)for(unsigned period=0;period<2;++period) {
        ++tag;uint32_t start=wrap?0xfffffff0u:0;
        pair(start,period?1064:65536,0,32,rate,irq?3:0);
        eager.d.halted=candidate.d.halted=true;
        write_io(&eager,0x200,2,8);write_io(&candidate,0x200,2,8);
        for(unsigned n=0;n<7;++n) {
            service_pair();compare(true);
            /* Drain at matching guest observation points and compare every
             * sample, including timestamps; neither side may drop output. */
            struct GbnStereo a[GBN_AUDIO_CAPACITY],b[GBN_AUDIO_CAPACITY];
            unsigned an=gbn_audio_read(&eager.m,a,GBN_AUDIO_CAPACITY);
            unsigned bn=gbn_audio_read(&candidate.m,b,GBN_AUDIO_CAPACITY);
            CHECK(an==bn&&!memcmp(a,b,an*sizeof(*a)));
            CHECK(!eager.d.audio.dropped&&!candidate.d.audio.dropped);
            if(n==2) {
                eager.m.cpu_access=candidate.m.cpu_access=true;
                write_io(&eager,0x88,2,0x0200);write_io(&candidate,0x88,2,0x0200);
                eager.m.cpu_access=candidate.m.cpu_access=false;
            }
        }
        compare(false);
    }
}


static void public_clock_cases(void) {
    section="public PCM clock";
    for(unsigned action=0;action<2;++action) {
        ++tag;pair(0,1064,0,32,3,0);
        if(!action){gbn_cancel(&eager.m,GBN_EVENT_AUDIO);gbn_cancel(&candidate.m,GBN_EVENT_AUDIO);}
        else {CHECK(gbn_schedule_at(&eager.m,GBN_EVENT_AUDIO,192,0x18));CHECK(gbn_schedule_at(&candidate.m,GBN_EVENT_AUDIO,192,0x18));}
        advance(&eager,256);advance(&candidate,256);compare(false);
    }
}
static void dma_priority_cases(void) {
    section="custom DMA priority";
    for(unsigned wins=0;wins<2;++wins)for(unsigned earlier=0;earlier<2;++earlier) {
        ++tag;pair(0,1064,0,32,3,0);gbn_audio_set_timeline(&candidate.m,false);
        for(unsigned side=0;side<2;++side) {
            struct Fixture* f=side?&candidate:&eager;
            put16(f->iw,0x1000,0x100);
            write_io(f,0xd4,4,0x03001000);write_io(f,0xd8,4,0x04000088);write_io(f,0xdc,4,0x80400001);
            gbn_cancel(&f->m,GBN_EVENT_AUDIO);gbn_cancel(&f->m,GBN_EVENT_DMA);
            if(wins)CHECK(gbn_schedule_at(&f->m,GBN_EVENT_AUDIO,64,0x18));
            CHECK(gbn_schedule_at(&f->m,GBN_EVENT_DMA,64,0x18));
            if(!wins)CHECK(gbn_schedule_at(&f->m,GBN_EVENT_AUDIO,64,0x18));
            if(earlier)CHECK(gbn_schedule_at(&f->m,GBN_EVENT_AUDIO_FRAME,32,0x10));
        }
        gbn_audio_set_timeline(&candidate.m,true);
        if(earlier){eager.m.now=candidate.m.now=32;service_pair();}
        eager.m.now=candidate.m.now=64;service_pair();compare(false);
    }
}

#ifdef AUDIO_TEST_NATIVE
int main(void) {
    const char* pick="all";
    CHECK(gbn_rv32_available());
#else
int main(int argc,char** argv) {
    const char* pick=argc>1?argv[1]:"all";
#endif
#define RUN(name) if(!strcmp(pick,"all")||!strcmp(pick,#name))name##_cases()
    RUN(sequence);RUN(priority);RUN(tie);RUN(write);RUN(cpu);RUN(halted);RUN(dma);RUN(ring);RUN(dma_observation);RUN(halt_observation);RUN(public_clock);RUN(dma_priority);
#ifdef AUDIO_TEST_NATIVE
    printf("Actual RV32 native instructions=%llu\n",(unsigned long long)native_total);
#endif
    printf("PASS detached audio %s: %u checks / %u fixtures\n",pick,checks,tag);return 0;
}
