/* SPDX-License-Identifier: MPL-2.0 */
static const uint32_t mixer_arithmetic[10]={0xe009019eu,0xe0809bc9u,0xe00c099au,
    0xe3ccc8ffu,0xe08c6466u,0xe00c099bu,0xe3ccc8ffu,0xe08c7467u,0xe08ee004u,0xe1b09baeu};

static void mixer_arithmetic_body(struct Emit* e,struct GbnRv32* backend) {
    multiply_wait(e,guest[1],false);op(e,NOW,NOW,WAIT,0,0);
    op(e,guest[9],guest[14],guest[1],0,1);
    imm(e,guest[9],guest[9],5,0x400|23);
    op(e,guest[9],guest[0],guest[9],0,0);
    /* Both following MUL instructions have the same Rs, even though their
     * Rm operands differ. Count that exact early-exit class once. */
    multiply_wait(e,guest[9],false);imm(e,WAIT,WAIT,1,1);op(e,NOW,NOW,WAIT,0,0);
    constant(e,LHS,~0x00ff0000u);
    op(e,guest[12],guest[10],guest[9],0,1);op(e,guest[12],guest[12],LHS,7,0);
    imm(e,RHS,guest[6],5,8);imm(e,RESULT,guest[6],1,24);op(e,RESULT,RESULT,RHS,6,0);
    op(e,guest[6],guest[12],RESULT,0,0);
    op(e,guest[12],guest[11],guest[9],0,1);op(e,guest[12],guest[12],LHS,7,0);
    imm(e,RHS,guest[7],5,8);imm(e,RESULT,guest[7],1,24);op(e,RESULT,RESULT,RHS,6,0);
    op(e,guest[7],guest[12],RESULT,0,0);
    op(e,guest[14],guest[14],guest[4],0,0);
    imm(e,WAIT,guest[14],5,22);imm(e,WAIT,WAIT,7,1);
    store(e,WAIT,offsetof(struct Gbn,cpu.shifter_carry));
    imm(e,guest[9],guest[14],5,23);
    constant(e,TEMP,~0x20000000u);op(e,FLAGS,FLAGS,TEMP,7,0);
    imm(e,WAIT,WAIT,1,29);op(e,FLAGS,FLAGS,WAIT,6,0);
    constant(e,NEGATIVE,0);imm(e,IS_ZERO,guest[9],3,1);
    imm(e,NOW,NOW,0,10);
#if GBN_RV32_STATS
    constant(e,LHS,(uint32_t)(uintptr_t)&backend->fused_mixer_instructions);
    word(e,LHS<<15|2<<12|TEMP<<7|3);
    imm(e,RESULT,TEMP,0,10);op(e,WAIT,RESULT,TEMP,3,0);
    word(e,RESULT<<20|LHS<<15|2<<12|0x23);
    word(e,4u<<20|LHS<<15|2<<12|TEMP<<7|3);
    op(e,TEMP,TEMP,WAIT,0,0);word(e,TEMP<<20|LHS<<15|2<<12|4u<<7|0x23);
#else
    (void)backend;
#endif
}

static unsigned arm_mixer_bulk(struct Emit* e,struct GbnRv32* backend,
        const struct Gbn* m,unsigned first,unsigned ram_guard_words,unsigned edge_count) {
    if(e->block->thumb || m->code_region!=3 || m->code_mask!=GBN_IWRAM_SIZE-4 ||
       m->code_wait || m->code_word_wait || m->code_nonseq ||
       m->code_word_nonseq || first+10>GBN_RV32_INSNS)return 0;
    uint32_t pc=e->block->pc+4*first,offset=pc&m->code_mask;
    if(offset>GBN_IWRAM_SIZE-sizeof(mixer_arithmetic) ||
       offset!=(e->block->pc&m->code_mask)+4*first ||
       memcmp(m->code+offset,mixer_arithmetic,sizeof(mixer_arithmetic)))return 0;
    uint32_t words[GBN_RV32_WORDS];
    struct GbnRv32Block temporary=*e->block;temporary.code=words;
    struct Emit probe=*e;probe.block=&temporary;probe.count=probe.stop_count=0;probe.skip_shifter=false;
    for(unsigned j=0;j<10;++j) {
        (void)arm_instruction(&probe,m,mixer_arithmetic[j],pc+4*j);
        imm(&probe,NOW,NOW,0,1);++temporary.instructions;
    }
    unsigned full=probe.count;probe.count=probe.stop_count=0;
    mixer_arithmetic_body(&probe,backend);unsigned fast=probe.count;
    unsigned reserve=340+64*edge_count+(13+ram_guard_words)*(first+10);
    if(e->count+9+fast+full+3*9+reserve>=GBN_RV32_WORDS)return 0;
    constant(e,TEMP,first+10);unsigned cap=e->count;word(e,0);
    imm(e,TEMP,NOW,0,22);unsigned time=e->count;word(e,0);
    memcpy(e->block->code+e->count,words,fast*sizeof(*words));e->count+=fast;
    unsigned done=e->count;word(e,0);unsigned slow=e->count;
    e->block->code[cap]=beq(LIMIT,TEMP,4*(slow-cap))|6u<<12;
    e->block->code[time]=beq(TEMP,ZERO,4*(slow-time))|5u<<12;
    e->skip_shifter=false;
    for(unsigned j=0;j<10;++j) {
        if(j) {
            e->body_offsets[first+j]=e->count;
            constant(e,TEMP,first+j);stop(e,LIMIT,TEMP,0,false);stop(e,NOW,ZERO,5,false);
        }
        (void)arm_instruction(e,m,mixer_arithmetic[j],pc+4*j);
        imm(e,NOW,NOW,0,1);++e->block->instructions;
    }
    e->block->code[done]=beq(ZERO,ZERO,4*(e->count-done));return 10;
}
