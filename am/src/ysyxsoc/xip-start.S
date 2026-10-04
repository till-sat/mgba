/* SPDX-License-Identifier: MPL-2.0 */
/* Optional bring-up startup: execute the image directly from Flash. */
    .section .text.entry,"ax",@progbits
    .globl _start
_start:
    csrw mie, zero
    csrw mstatus, zero
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
    la sp, _stack_top
    la t0, _am_trap
    csrw mtvec, t0

    la t0, _data_load
    la t1, _data_start
    la t2, _data_end
1:  bgeu t1, t2, 2f
    lw a3, 0(t0)
    sw a3, 0(t1)
    addi t0, t0, 4
    addi t1, t1, 4
    j 1b
2:  la t0, _bss_start
    la t1, _bss_end
3:  bgeu t0, t1, 4f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 3b
4:  call am_platform_boot
    call main
    call exit
5:  j 5b

    .balign 4
_am_trap:
    la sp, _trap_stack_top
    csrr a0, mcause
    csrr a1, mepc
    csrr a2, mtval
    call am_trap_fatal
    j 5b
