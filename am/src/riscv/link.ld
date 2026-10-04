/* SPDX-License-Identifier: MPL-2.0 */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { ram (rwx) : ORIGIN = 0xa0000000, LENGTH = 64M }
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } > ram :text
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
}
