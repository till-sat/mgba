/* SPDX-License-Identifier: MPL-2.0 */
#if GBN_RV32_STATS
uint32_t gbn_rv32_packet_time_skips;
uint32_t gbn_rv32_partial_packets;
#endif
/* Private resident-register leaf, never a C ABI call. Whole-packet guards
 * precede this entry. Guest LR remains in ra; s3 is the private link. Old N/Z
 * are dead because the caller finishes with SUBS r8,#4 for a complete
 * packet, or ADDS r5,#0x40000000 for a budget-limited sample prefix.
 * t2 returns 1 for a complete packet and 2 for a prefix. */
__attribute__((naked,noinline,used)) static void mixer_packet_leaf(void) {
    __asm__ volatile(
        "sw s3,136(sp)\n\tlw s5,32(sp)\n\tli t0,0xff00ffff\n\t"
        "1: mul s8,ra,a5\n\tsrai s8,s8,23\n\tadd s8,a4,s8\n\t"
        "mul s1,s11,s8\n\tand s1,s1,t0\n\t"
        "srli t2,t5,8\n\tslli s3,t5,24\n\tor t2,t2,s3\n\tadd t5,s1,t2\n\t"
        "mul s1,s4,s8\n\tand s1,s1,t0\n\t"
        "srli t2,t6,8\n\tslli s3,t6,24\n\tor t2,t2,s3\n\tadd t6,s1,t2\n\t"
        "add ra,ra,t3\n\tsrli s8,ra,23\n\tbeqz s8,4f\n\t"
        "slli ra,ra,9\n\tsrli ra,ra,9\n\tsub a6,a6,s8\n\tadd a7,a7,s8\n\t"
        "add t1,t1,s8\n\taddi s8,s8,-1\n\tbnez s8,2f\n\t"
        "add a4,a4,a5\n\tj 3f\n\t2: lb a4,-1(t1)\n\t"
        "3: lb a5,0(t1)\n\tsub a5,a5,a4\n\t"
        "4: addi s5,s5,-1\n\tbnez s5,1b\n\t"
        "lw t2,32(sp)\n\tlw t0,36(sp)\n\tbne t2,t0,5f\n\t"
        "sw t6,1584(a3)\n\tsw t5,0(a3)\n\tslli t4,t4,2\n\tsrli t4,t4,2\n\taddi t4,t4,4\n\taddi s9,s9,2\n\tli t2,1\n\tj 6f\n\t"
        /* Caller emits the final ADDS to reproduce NZCV and shifter carry.
         * Earlier rotations only change the high two counter bits. */
        "5: addi t2,t2,-1\n\tslli t2,t2,30\n\tadd t4,t4,t2\n\tli t2,2\n\t"
        "6: lw s3,136(sp)\n\tjalr zero,s3,0\n\t");
}
#include "mixer-packet-guard.h"

static const uint32_t mixer_packet_ops[25]={
    0xe009019e,0xe0809bc9,0xe00c099a,0xe3ccc8ff,0xe08c6466,
    0xe00c099b,0xe3ccc8ff,0xe08c7467,0xe08ee004,0xe1b09bae,
    0x0a000007,0xe3cee5fe,0xe0522009,0xdaffffce,0xe2599001,
    0x00800001,0x11b300d9,0xe1f310d1,0xe0411000,0xe2955101,0x3affffea,
    0xe5857630,0xe4856004,0xe2588004,0xcaffffe4
};

static void arm_mixer_packet(struct Emit* e,struct GbnRv32* backend,
        const struct Gbn* m,unsigned first) {
    if(e->block->thumb || m->code_region!=3 || m->code_mask!=GBN_IWRAM_SIZE-4 ||
       m->code_wait || m->code_word_wait || m->code_nonseq || m->code_word_nonseq ||
       !m->rom || first+25>GBN_RV32_INSNS)return;
    unsigned base=e->block->pc&m->code_mask,offset=base+4*first;
    uint32_t pc=e->block->pc+4*first;
    /* execute_mode retains its current mapping on native return. Both full
     * packet successors must therefore stay in this code region. */
    if((pc-8)>>24!=m->code_region || (pc+sizeof(mixer_packet_ops))>>24!=m->code_region)return;
    if(offset>GBN_IWRAM_SIZE-sizeof(mixer_packet_ops) ||
       memcmp(m->code+offset,mixer_packet_ops,sizeof(mixer_packet_ops)))return;
    _Static_assert(sizeof(struct GbnRv32Block)==64,"Lookahead must use existing descriptor padding");
    e->block->proof_instructions=(uint16_t)(first+25);
    /* Shared private-ABI guard keeps ordinary bodies inside the code cap.
     * ARM blocks do not use Thumb fixed-point snapshot words 20..28. */
    /* A complete sample ending at the taken loop branch costs at least 20
     * cycles. The guard can select a prefix when the full suffix won't fit. */
    imm(e,TEMP,NOW,0,20);
    unsigned too_short=e->count;word(e,beq(TEMP,ZERO,0)|5u<<12);
    constant(e,LHS,base);stack(e,LHS,20,true);
    constant(e,LHS,first);stack(e,LHS,24,true);
    constant(e,LHS,(GBN_RV32_INSNS+2)*4+(e->block->pc&3));stack(e,LHS,28,true);
    constant(e,WAIT,(uint32_t)(uintptr_t)mixer_packet_guard);word(e,WAIT<<15|WAIT<<7|0x67);
    unsigned miss=e->count;word(e,beq(RESULT,ZERO,0));
#if GBN_RV32_STATS
    constant(e,LHS,(uint32_t)(uintptr_t)&backend->fused_mixer_packets);
    word(e,LHS<<15|2u<<12|RHS<<7|3);imm(e,WAIT,RHS,0,1);op(e,TEMP,WAIT,RHS,3,0);
    word(e,WAIT<<20|LHS<<15|2u<<12|0x23);word(e,4u<<20|LHS<<15|2u<<12|RHS<<7|3);
    op(e,RHS,RHS,TEMP,0,0);word(e,RHS<<20|LHS<<15|2u<<12|4u<<7|0x23);
#endif
    stack(e,RHS,SNAPSHOT_FLAGS,false);stack(e,TEMP,STACK_TARGET,false);
    constant(e,WAIT,(uint32_t)(uintptr_t)mixer_packet_leaf);word(e,WAIT<<15|WAIT<<7|0x67);
    constant(e,TEMP,2);unsigned partial=e->count;word(e,0);
    constant(e,RHS,4);arithmetic(e,true,guest[8],RHS);put_value(e,8,RESULT);
    constant(e,TEMP,1);store(e,TEMP,offsetof(struct Gbn,cpu.shifter_carry));constant(e,PREFETCH,0);
    condition(e,12);unsigned taken=e->count;word(e,0);
    unsigned saved=e->block->instructions;e->block->instructions=first;
    tail(e,backend,pc+sizeof(mixer_packet_ops),false);
    e->block->code[taken]=beq(TEMP,ZERO,4*(e->count-taken))|1u<<12;
    tail(e,backend,pc-8,false);
    e->block->code[partial]=beq(RESULT,TEMP,4*(e->count-partial));
    (void)arm_instruction(e,m,mixer_packet_ops[19],pc+19*4);
    constant(e,PREFETCH,0);
#if GBN_RV32_STATS
    constant(e,LHS,(uint32_t)(uintptr_t)&gbn_rv32_partial_packets);
    word(e,LHS<<15|2u<<12|RHS<<7|3);imm(e,RHS,RHS,0,1);
    word(e,RHS<<20|LHS<<15|2u<<12|0x23);
#endif
    if(first) {
        /* The partial BCC returns to this already-proven instruction in
         * the same block. LIMIT still includes the original prefix; its
         * boundary guards compare against first on every iteration, and
         * the eventual exit charges that prefix exactly once. */
        word(e,beq(ZERO,ZERO,4*(e->body_offsets[first]-e->count)));
    } else tail(e,backend,pc,false);
    e->block->instructions=saved;
    e->read_only=false;e->read_seen=true;
    unsigned filtered=e->count;
#if GBN_RV32_STATS
    constant(e,LHS,(uint32_t)(uintptr_t)&gbn_rv32_packet_time_skips);
    word(e,LHS<<15|2u<<12|RHS<<7|3);imm(e,RHS,RHS,0,1);
    word(e,RHS<<20|LHS<<15|2u<<12|0x23);
#endif
    e->block->code[too_short]|=beq(0,0,4*(filtered-too_short))&0xfe000f80u;
    e->block->code[miss]|=beq(0,0,4*(e->count-miss))&0xfe000f80u;
}
