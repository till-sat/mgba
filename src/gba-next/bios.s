/* SPDX-License-Identifier: MPL-2.0 */
/* Independent guest firmware. Implemented services execute normal ARM7 code;
 * missing services stop at 0x20 instead of pretending to have completed. */
.syntax unified
.cpu arm7tdmi
.arm
.text

b boot
b unsupported
b swi_entry
b unsupported
b unsupported
b unsupported
b irq_entry
b unsupported
unsupported:
.word 0xe7f000f0
b unsupported

boot:
msr cpsr_c, #0xd2
ldr sp, =0x03007fa0
msr cpsr_c, #0xd3
ldr sp, =0x03007fe0
msr cpsr_c, #0x1f
ldr sp, =0x03007f00
mov r2, #0x04000000
mov r4, #1
strb r4, [r2, #0x300]
mov pc, #0x08000000

swi_entry:
stmdb sp!, {r11, r12, lr}
mrs r11, spsr
stmdb sp!, {r11}
ldrb r12, [lr, #-2]           /* Thumb immediate or ARM immediate bits 16..23. */
and r11, r11, #0x80
orr r11, r11, #0x1f
msr cpsr_c, r11               /* System stack; IRQ mask inherited from caller. */
stmdb sp!, {r2, r4, r5, lr}
cmp r12, #0x02
beq call_halt
cmp r12, #0x04
beq call_intr_wait
cmp r12, #0x05
beq call_vblank_wait
cmp r12, #0x11
beq call_lz77
cmp r12, #0x12
beq call_lz77
cmp r12, #0x0b
beq call_cpu_set
cmp r12, #0x0c
bne unsupported
adr lr, swi_return
b cpu_fast_set
call_cpu_set:
adr lr, swi_return
b cpu_set
call_halt:
adr lr, swi_return
b halt
call_vblank_wait:
mov r0, #1
mov r1, #1
call_intr_wait:
adr lr, swi_return
b intr_wait
call_lz77:
adr lr, swi_return
b lz77
swi_return:
ldmia sp!, {r2, r4, r5, lr}
msr cpsr_c, #0x93
ldmia sp!, {r11}
msr spsr_cxsf, r11
ldmia sp!, {r11, r12, lr}
movs pc, lr

irq_entry:
stmdb sp!, {r0-r3, r12, lr}
ldr r0, =0x03007ffc
adr lr, irq_return
ldr pc, [r0]
irq_return:
ldmia sp!, {r0-r3, r12, lr}
subs pc, lr, #4

halt:
mov r2, #0
mov r4, #0x04000000
strb r2, [r4, #0x301]
bx lr

intr_wait:
stmdb sp!, {r3, lr}
mov r12, #0x04000000
mov r2, #0
strb r2, [r12, #0x208]
ldrh r4, [r12, #-8]
cmp r0, #0
bicne r4, r4, r1
strhne r4, [r12, #-8]
mov r0, #0
intr_wait_check:
mov r2, #0
strb r2, [r12, #0x208]
ldrh r4, [r12, #-8]
ands r3, r4, r1
bne intr_wait_done
/* Check and sleep with IME disabled so an IRQ cannot acknowledge IF between
 * the flag read and HALT. HALT still wakes on IE & IF. After waking, allow the
 * seven-cycle controller latency to deliver the handler before rechecking. */
strb r2, [r12, #0x301]
mov r2, #1
strb r2, [r12, #0x208]
.rept 8
mov r2, r2
.endr
b intr_wait_check
intr_wait_done:
bic r4, r4, r1
strh r4, [r12, #-8]
mov r2, #1
strb r2, [r12, #0x208]
ldmia sp!, {r3, pc}

/* LZ77 WRAM byte stores and VRAM halfword stores. VRAM back-references read
 * committed halfwords, as the hardware BIOS does; distance one is unsuitable
 * for this format. Source/destination cursors are returned in r0/r1. */
lz77:
stmdb sp!, {r6-r10, lr}
sub r10, r12, #0x11
ldr r2, [r0], #4
mov r2, r2, lsr #8
mov r5, #0
mov r8, #0
lz77_block:
cmp r2, #0
beq lz77_done
cmp r5, #0
ldrbeq r4, [r0], #1
moveq r5, #8
tst r4, #0x80
mov r4, r4, lsl #1
sub r5, r5, #1
beq lz77_literal
ldrb r7, [r0], #1
ldrb r6, [r0], #1
orr r6, r6, r7, lsl #8
mov r7, r7, lsr #4
add r7, r7, #3
bic r6, r6, #0xf000
sub r6, r1, r6
sub r6, r6, #1
lz77_copy:
cmp r10, #0
ldrbeq r9, [r6]
beq lz77_copy_read
bic r3, r6, #1
ldrh r9, [r3]
tst r6, #1
movne r9, r9, lsr #8
and r9, r9, #0xff
lz77_copy_read:
add r6, r6, #1
b lz77_write
lz77_literal:
ldrb r9, [r0], #1
mov r7, #1
lz77_write:
cmp r10, #0
strbeq r9, [r1]
beq lz77_written
tst r1, #1
moveq r8, r9
orrne r8, r8, r9, lsl #8
subne r3, r1, #1
strhne r8, [r3]
lz77_written:
add r1, r1, #1
cmp r2, #0
subne r2, r2, #1
subs r7, r7, #1
bne lz77_copy
b lz77_block
lz77_done:
mov r3, #0
ldmia sp!, {r6-r10, pc}

cpu_set:
mov r4, r2, lsl #12
mov r4, r4, lsr #12
tst r2, #0x04000000
beq cpu_set_half
tst r2, #0x01000000
beq cpu_set_word_copy
ldmia r0!, {r3}
cpu_set_word_fill:
cmp r4, #0
beq cpu_set_done
stmia r1!, {r3}
sub r4, r4, #1
b cpu_set_word_fill
cpu_set_word_copy:
cmp r4, #0
beq cpu_set_done
ldmia r0!, {r3}
stmia r1!, {r3}
sub r4, r4, #1
b cpu_set_word_copy
cpu_set_half:
mov r12, r0
mov r5, r1
tst r2, #0x01000000
beq cpu_set_half_copy
bic r12, r12, #1
bic r5, r5, #1
ldrh r3, [r12]
cpu_set_half_fill:
cmp r4, #0
beq cpu_set_done
strh r3, [r5], #2
sub r4, r4, #1
b cpu_set_half_fill
cpu_set_half_copy:
cmp r4, #0
beq cpu_set_done
ldrh r3, [r12], #2
strh r3, [r5], #2
sub r4, r4, #1
b cpu_set_half_copy
cpu_set_done:
mov r3, #0x170
bx lr

cpu_fast_set:
stmdb sp!, {r6-r10}
mov r3, r2, lsl #12
mov r12, r3, lsr #12
add r12, r12, #7
bic r12, r12, #7           /* Transfers complete blocks of eight words. */
tst r2, #0x01000000
beq cpu_fast_copy
ldr r3, [r0]
mov r4, r3
mov r5, r3
mov r6, r3
mov r7, r3
mov r8, r3
mov r9, r3
mov r10, r3
cpu_fast_fill:
cmp r12, #0
beq cpu_fast_done
stmia r1!, {r3-r10}
sub r12, r12, #8
b cpu_fast_fill
cpu_fast_copy:
cmp r12, #0
beq cpu_fast_done
ldmia r0!, {r3-r10}
stmia r1!, {r3-r10}
sub r12, r12, #8
b cpu_fast_copy
cpu_fast_done:
ldmia sp!, {r6-r10}
bx lr
.ltorg
