/* SPDX-License-Identifier: MPL-2.0 */
/* Native-register ABI, never a C call. A miss preserves every guest register,
 * NZCV, shifter carry, prefetch, NOW, LIMIT and the RAM epoch. Success consumes
 * only exact packet time/count and returns the proven source/output pointers
 * through the same scratch frame words used by the inline BC guard.
 * Frame 20=source base, 24=prefix count, 28=source/prefetch protection size.
 * Frame 32=selected samples, 36=remaining rotation samples, 40=ROM read cost.
 * A budget-limited prefix leaves the final BCC taken and does not store PCM.
 * s3 is the private link. All other non-scratch registers remain untouched. */
__attribute__((naked,noinline,used)) static void mixer_packet_guard(void) {
    __asm__ volatile(
        "sw s3,136(sp)\n\t"
        /* Zero high rotation counter, aligned IWRAM output and backing. */
        "srli a3,t4,24\n\tandi a3,a3,63\n\tli t0,3\n\tbne a3,t0,90f\n\t"
        "andi a3,t4,3\n\tbnez a3,90f\n\t"
        "slli a3,t4,17\n\tsrli a3,a3,17\n\tli t0,%[end]\n\tbgeu a3,t0,90f\n\t"
        "lw t0,%[iw](a0)\n\tadd a3,a3,t0\n\tandi t0,a3,3\n\tbnez t0,90f\n\tsw a3,92(sp)\n\t"
        /* Both mirror-wrapped stores avoid all code and prefetched bytes. */
        "lw t0,20(sp)\n\tsub a3,t4,t0\n\t"
        "slli a3,a3,17\n\tsrli a3,a3,17\n\tlw t0,28(sp)\n\tbltu a3,t0,90f\n\t"
        "addi a3,a3,1584\n\tslli a3,a3,17\n\tsrli a3,a3,17\n\tbltu a3,t0,90f\n\t"
        /* Proven byte endpoints/volumes imply all MUL early waits equal one. */
        "srli a3,ra,23\n\tbnez a3,90f\n\tli t0,0x4000001\n\tbgeu t3,t0,90f\n\t"
        "or a3,s11,s4\n\tli t0,0xff00ffff\n\tand a3,a3,t0\n\tbnez a3,90f\n\t"
        "addi a3,a4,128\n\tsltiu a3,a3,256\n\tbeqz a3,90f\n\t"
        "addi a3,a5,255\n\tsltiu a3,a3,511\n\tbeqz a3,90f\n\t"
        "add a3,a4,a5\n\taddi a3,a3,128\n\tsltiu a3,a3,256\n\tbeqz a3,90f\n\t"
        /* Total advance, ROM bus/image limits and inclusive host aliases. */
        "srli t2,t4,30\n\tli t0,4\n\tsub t2,t0,t2\n\tsw t2,32(sp)\n\tsw t2,36(sp)\n\tmul t0,t3,t2\n\tadd t0,t0,ra\n\tsrli t0,t0,23\n\tbge t0,a6,90f\n\t"
        "srli a3,a7,24\n\taddi a3,a3,-8\n\tsltiu t1,a3,6\n\tbeqz t1,90f\n\t"
        "slli t1,a7,8\n\tsrli t1,t1,8\n\tadd t1,t1,t0\n\tsrli t1,t1,24\n\tbnez t1,90f\n\t"
        "slli t1,a7,7\n\tsrli t1,t1,7\n\tadd t2,t1,t0\n\tlw s3,%[romsize](a0)\n\tbgeu t2,s3,90f\n\t"
        "lw s3,%[rom](a0)\n\tadd t1,t1,s3\n\tsw t1,80(sp)\n\t"
        "lw t2,92(sp)\n\tsub t2,t2,t1\n\taddi t2,t2,3\n\taddi s3,t0,4\n\tbltu t2,s3,90f\n\t"
        "addi t2,t2,1584\n\tbltu t2,s3,90f\n\t"
        /* Dynamic ROM waits and exact NZ/MANY classes. */
        "srli a3,a3,1\n\taddi a3,a3,%[romwait]\n\tadd a3,a3,a0\n\tlbu t1,0(a3)\n\taddi t1,t1,2\n\t"
        "sw t1,40(sp)\n\t"
        /* Keep the whole suffix's conservative address/alias proof above.
         * Shrinking the prefix only shortens the proven ROM read interval.
         * Retry budget arithmetic, not the address/domain proof. */
        "5: srli a3,t3,23\n\tbeqz a3,3f\n\tlw t2,32(sp)\n\tli s3,1\n\tbne a3,s3,2f\n\t"
        "sub a3,t0,t2\n\tj 4f\n\t2: mv a3,t2\n\tj 4f\n\t3: mv t2,t0\n\tli a3,0\n\t"
        "4: add a3,a3,t2\n\tmul t1,t1,a3\n\tslli a3,t2,1\n\tslli s3,t2,2\n\t"
        "add a3,a3,s3\n\tadd t1,t1,a3\n\tlw a3,32(sp)\n\tli s3,20\n\tmul s3,s3,a3\n\tadd t1,t1,s3\n\t"
        "slli a3,t2,3\n\tlw s3,32(sp)\n\tli t0,13\n\tmul s3,s3,t0\n\tadd a3,a3,s3\n\t"
        "lw t0,32(sp)\n\tlw s3,36(sp)\n\tbne t0,s3,6f\n\t"
        /* The full suffix has two stores, SUBS and final BGT. Its last
         * BCC is untaken; the difference is 4(+2 taken) cycles and 4 ops. */
        "addi t1,t1,4\n\tli t0,4\n\tslt t0,t0,s7\n\tslli t0,t0,1\n\tadd t1,t1,t0\n\taddi a3,a3,4\n\t"
        "6: lw t0,24(sp)\n\tadd a3,a3,t0\n\tbltu a1,a3,80f\n\t"
        "add s3,s0,t1\n\tbgez s3,80f\n\tsub a3,a3,t0\n\tsub a1,a1,a3\n\tadd s0,s0,t1\n\t"
        "li t2,1\n\tlw s3,136(sp)\n\tjalr zero,s3,0\n\t"
        "80: lw t2,32(sp)\n\taddi t2,t2,-1\n\tbeqz t2,90f\n\tsw t2,32(sp)\n\t"
        "mul t0,t3,t2\n\tadd t0,t0,ra\n\tsrli t0,t0,23\n\tlw t1,40(sp)\n\tj 5b\n\t"
        "90: li t2,0\n\tlw s3,136(sp)\n\tjalr zero,s3,0\n\t"
        : : [end]"i"(GBN_IWRAM_SIZE-0x634+1),[iw]"i"(offsetof(struct Gbn,iwram)),
            [rom]"i"(offsetof(struct Gbn,rom)),[romsize]"i"(offsetof(struct Gbn,rom_size)),
            [romwait]"i"(offsetof(struct Gbn,rom_nonseq)));
}
