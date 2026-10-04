/* SPDX-License-Identifier: MPL-2.0 */
/* AM player fixture: gradient, square wave, SRAM boot counter, and A/Right. */
.syntax unified
.cpu arm7tdmi
.arm
.section .text
.global _start
_start:
 b start
 .space 0xbc
start:
 mov r0, #0x04000000
 ldr r1, =0x0403
 strh r1, [r0]
 mov r2, #0x06000000
 mov r3, #0
 ldr r4, =38400
paint:
 strh r3, [r2], #2
 add r3, r3, #1
 subs r4, r4, #1
 bne paint
 mov r1, #0x80
 strh r1, [r0, #0x84]
 ldr r1, =0x1177
 strh r1, [r0, #0x80]
 mov r1, #2
 strh r1, [r0, #0x82]
 mov r1, #0
 strh r1, [r0, #0x60]
 ldr r1, =0xf080
 strh r1, [r0, #0x62]
 ldr r1, =0x8400
 strh r1, [r0, #0x64]
 mov r5, #0x0e000000
 ldrb r1, [r5]
 add r1, r1, #1
 strb r1, [r5]
 mov r1, #0
 strb r1, [r5, #1]
loop:
 ldr r6, =0x04000130
 ldrh r1, [r6]
 mvn r1, r1
 and r1, r1, #0x11
 strb r1, [r5, #2]
 ldrb r2, [r5, #1]
 orr r2, r2, r1
 strb r2, [r5, #1]
 b loop
 .ltorg
 .ascii "SRAM_V113"
