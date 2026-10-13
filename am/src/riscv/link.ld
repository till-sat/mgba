/* SPDX-License-Identifier: MPL-2.0 */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
    ram (rwx) : ORIGIN = 0xa0000000, LENGTH = 64M
    /* proto-soc's wide SRAM, in the D-cache's window (am.h AM_WSRAM) */
    wsram (rw) : ORIGIN = 0x0e000000, LENGTH = 256K
}
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .text : {
        _text_start = .;
        KEEP(*(.text.entry))
        INCLUDE am/src/riscv/text-order.ld
        *(.text .text.*)
        _text_end = .;
    } > ram :text
    .rodata : { *(.rodata .rodata.*) } > ram :rodata
    .data : {
        *(.data .data.*)
        . = ALIGN(16);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
    } > ram :data
    .htif : ALIGN(64) { KEEP(*(.htif)) } > ram :data
    .bss (NOLOAD) : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        _bss_end = .;
    } > ram :data
    _heap_start = ALIGN(16);
    _heap_end = ORIGIN(ram) + LENGTH(ram) - 256K - 4K;
    .stack _heap_end (NOLOAD) : {
        . += 256K;
        _stack_top = .;
        . += 4K;
        _trap_stack_top = .;
    } > ram :data
    ASSERT(_heap_start + 8M <= _heap_end, "Image leaves less than 8 MiB for the heap")
    /* Not loaded and in no program header (the binary image stays in SDRAM);
     * start.S zeroes it like .bss. Empty unless a platform defines AM_HAS_WSRAM. */
    .wsram (NOLOAD) : ALIGN(64) {
        _wsram_start = .;
        *(.wsram .wsram.*)
        . = ALIGN(16);
        _wsram_end = .;
    } > wsram :NONE
}
