/* SPDX-License-Identifier: MPL-2.0 */
/* Private ABI: t1=stored word; t2=success; s3=link. No guest/time/flags/cap
 * effects on miss. Words52/56/64 hold devices, absolute when and control;
 * this writable block cannot use Thumb read-only loop snapshots. */
_Static_assert(sizeof(struct GbnEvent)==8 && offsetof(struct GbnEvent,priority)==4 && offsetof(struct GbnEvent,active)==5,"Event private ABI");
#if GBN_RV32_STATS
uint32_t gbn_rv32_dma_start_hits;
#endif
__attribute__((naked,noinline,used)) static void dma_start_leaf(void) {
    __asm__ volatile(
        "sw s3,136(sp)\n\tlw t0,%[devices](a0)\n\tbeqz t0,90f\n\tandi t2,t0,3\n\tbnez t2,90f\n\tsw t0,52(sp)\n\t"
        "srli t2,t1,16\n\tli s3,0xffe0\n\tand t2,t2,s3\n\tli s3,0xb000\n\tand s3,t2,s3\n\tli a3,0x8000\n\tbne s3,a3,90f\n\tsw t2,64(sp)\n\t"
        "lhu t2,%[control0]+3*%[stride](t0)\n\tsrli t2,t2,15\n\tbnez t2,90f\n\tlw t2,%[remaining0]+3*%[stride](t0)\n\tbnez t2,90f\n\tlbu t2,%[finishing0]+3*%[stride](t0)\n\tbnez t2,90f\n\t"
        "lbu t2,%[eventactive](a0)\n\tbnez t2,90f\n\tlw t2,%[winner](a0)\n\tli s3,%[id]\n\tbeq t2,s3,90f\n\t"
#define CHECK_OTHER(c) \
        "lhu t2,%[control0]+" #c "*%[stride](t0)\n\tsrli t2,t2,15\n\tbeqz t2," #c "f\n\t" \
        "lw t2,%[remaining0]+" #c "*%[stride](t0)\n\tbnez t2,90f\n\tlbu t2,%[finishing0]+" #c "*%[stride](t0)\n\tbnez t2,90f\n\t" #c ": "
        CHECK_OTHER(0) CHECK_OTHER(1) CHECK_OTHER(2)
#undef CHECK_OTHER
        /* All possible misses are above. Every field matches the two
         * halfword bus writes, reload_count, select_dma and schedule_at. */
        "sh t1,220(t0)\n\tlw t2,64(sp)\n\tsh t2,222(t0)\n\tsh t2,%[control0]+3*%[stride](t0)\n\t"
        "slli t1,t1,16\n\tsrli t1,t1,16\n\tbnez t1,3f\n\tli t1,65536\n\t3: sw t1,%[count3](t0)\n\tsw t1,%[remaining0]+3*%[stride](t0)\n\t"
        "sb zero,%[finishing0]+3*%[stride](t0)\n\tsb zero,%[fifo3](t0)\n\t"
        "srli t2,t2,10\n\tandi t2,t2,1\n\tslli t2,t2,1\n\taddi t2,t2,1\n\tnot t2,t2\n\t"
        "lw t1,%[source3](t0)\n\tand t1,t1,t2\n\tsw t1,%[nextsource3](t0)\n\t"
        "lw t1,%[dest3](t0)\n\tand t1,t1,t2\n\tsw t1,%[nextdest3](t0)\n\t"
        "lw t1,132(sp)\n\tadd t1,t1,s0\n\taddi t1,t1,3\n\tsw t1,56(sp)\n\tsw t1,%[when3](t0)\n\tli t2,3\n\tsw t2,%[active_dma](t0)\n\t"
        /* Install new event bytes, then canonical insertion by time/prio/id.
         * next_event retains its historical winner for equal time/priority. */
        "sw t1,%[eventwhen](a0)\n\tli t2,0x140\n\tsh t2,%[eventwhen4](a0)\n\t"
        "li t0,%[events]\n\tlw t2,%[winner](a0)\n\tbltz t2,6f\n\tlbu t1,%[head](a0)\n\tj 4f\n\t6: li t1,%[events]\n\t"
        "4: li t2,%[events]\n\tbgeu t1,t2,7f\n\tslli a3,t1,3\n\taddi a3,a3,%[eventbase]\n\tadd a3,a3,a0\n\t"
        "lw t2,56(sp)\n\tlw s3,0(a3)\n\tsub s3,t2,s3\n\tbltz s3,7f\n\tbnez s3,5f\n\t"
        "lbu t2,4(a3)\n\tli s3,64\n\tbltu s3,t2,7f\n\tbne s3,t2,5f\n\tli s3,%[id]\n\tbltu s3,t1,7f\n\t"
        "5: mv t0,t1\n\taddi a3,a0,%[next]\n\tadd a3,a3,t1\n\tlbu t1,0(a3)\n\tj 4b\n\t"
        "7: sb t0,%[prev_id](a0)\n\tsb t1,%[next_id](a0)\n\tli t2,%[id]\n\tli s3,%[events]\n\tbgeu t0,s3,8f\n\t"
        "addi a3,a0,%[next]\n\tadd a3,a3,t0\n\tsb t2,0(a3)\n\tj 9f\n\t8: sb t2,%[head](a0)\n\t"
        "9: bgeu t1,s3,10f\n\taddi a3,a0,%[prev]\n\tadd a3,a3,t1\n\tsb t2,0(a3)\n\t"
        "10: lw t0,%[winner](a0)\n\tbltz t0,11f\n\tslli a3,t0,3\n\taddi a3,a3,%[eventbase]\n\tadd a3,a3,a0\n\t"
        "lw t1,56(sp)\n\tlw t2,0(a3)\n\tsub t2,t1,t2\n\tbltz t2,11f\n\tbnez t2,12f\n\tlbu t2,4(a3)\n\tli t1,64\n\tbgeu t1,t2,12f\n\t"
        "11: li t0,%[id]\n\tsw t0,%[winner](a0)\n\t"
        "12: li t2,1\n\tlw s3,136(sp)\n\tjalr zero,s3,0\n\t"
        "90: li t2,0\n\tlw s3,136(sp)\n\tjalr zero,s3,0\n\t"
        : : [devices]"i"(offsetof(struct Gbn,devices)),[winner]"i"(offsetof(struct Gbn,next_event)),
            [eventbase]"i"(offsetof(struct Gbn,events)),[head]"i"(offsetof(struct Gbn,event_head)),
            [prev]"i"(offsetof(struct Gbn,event_prev)),[next]"i"(offsetof(struct Gbn,event_next)),
            [prev_id]"i"(offsetof(struct Gbn,event_prev)+GBN_EVENT_DMA),[next_id]"i"(offsetof(struct Gbn,event_next)+GBN_EVENT_DMA),
            [eventactive]"i"(offsetof(struct Gbn,events[GBN_EVENT_DMA].active)),
            [eventwhen]"i"(offsetof(struct Gbn,events[GBN_EVENT_DMA].when)),[eventwhen4]"i"(offsetof(struct Gbn,events[GBN_EVENT_DMA].when)+4),
            [id]"i"(GBN_EVENT_DMA),[events]"i"(GBN_EVENT_COUNT),[active_dma]"i"(offsetof(struct GbnDevices,active_dma)),
#define F(c,m,n) [n]"i"(offsetof(struct GbnDevices,dma[c].m))
            [stride]"i"(sizeof(struct GbnDma)),F(0,control,control0),F(0,remaining,remaining0),F(0,finishing,finishing0),
            F(3,count,count3),F(3,source,source3),F(3,dest,dest3),F(3,next_source,nextsource3),
            F(3,next_dest,nextdest3),F(3,fifo,fifo3),F(3,when,when3)
#undef F
    );
}

static void finish_count(struct Emit*,unsigned,bool);
static void dma_start_emit(struct Emit* e,const struct Gbn* m,uint32_t pc,unsigned rd,unsigned rn,int adjustment) {
    constant(e,LHS,0x040000dcu);
    unsigned different=e->count;word(e,0);
    get(e,RHS,rd,pc);
    constant(e,WAIT,(uint32_t)(uintptr_t)dma_start_leaf);word(e,WAIT<<15|WAIT<<7|0x67);
    constant(e,TEMP,0x040000dcu); /* Restore address on success AND miss. */
    unsigned miss=e->count;word(e,beq(RESULT,ZERO,0));
#if GBN_RV32_STATS
    constant(e,LHS,(uint32_t)(uintptr_t)&gbn_rv32_dma_start_hits);
    word(e,LHS<<15|2u<<12|RHS<<7|3);imm(e,RHS,RHS,0,1);
    word(e,RHS<<20|LHS<<15|2u<<12|0x23);
#endif
    writeback(e,rn,adjustment);
    constant(e,WAIT,1);ram_wait(e,m,pc);
    imm(e,NOW,NOW,0,1+(e->block->thumb?m->code_wait:m->code_word_wait));
    /* The new DMA deadline invalidates this frame's bound. Always exit
     * after exactly this store; the normal CPU/event dispatcher resumes. */
    finish_count(e,e->block->instructions+1,false);
    constant(e,RHS,pc+(e->block->thumb?2:4));
    e->returns[e->return_count++]=e->count;word(e,0);
    e->block->code[different]=beq(TEMP,LHS,4*(e->count-different))|1u<<12;
    e->block->code[miss]|=beq(0,0,4*(e->count-miss))&0xfe000f80u;
}
