/* SPDX-License-Identifier: MPL-2.0 */
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
    la t0, _bss_start
    la t1, _bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call am_platform_boot
    call main
    call exit
3:  j 3b

    .balign 4
_am_trap:
    /* A fatal trap uses a reserved stack, including when sp is corrupt. */
    la sp, _trap_stack_top
    csrr a0, mcause
    csrr a1, mepc
    csrr a2, mtval
    call am_trap_fatal
    j 3b
