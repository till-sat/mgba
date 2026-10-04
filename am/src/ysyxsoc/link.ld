/* SPDX-License-Identifier: MPL-2.0 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

/* The ysyxSoC bootloader copies this image into its SDRAM window. */
MEMORY { sdram (rwx) : ORIGIN = 0xa0000000, LENGTH = 32M }
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } > sdram :text
    .rodata : { *(.rodata .rodata.*) } > sdram :rodata
    .data : {
        *(.data .data.*)
        . = ALIGN(16);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
    } > sdram :data
    /* The ysyxSoC Flash image omits a trailing 0xff ROM region. */
    .rom : ALIGN(16) { KEEP(*(.rom)) } > sdram :data
    .rom_tail (NOLOAD) : { KEEP(*(.rom_tail)) } > sdram :data
    .bss (NOLOAD) : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        _bss_end = .;
    } > sdram :data
    _heap_start = ALIGN(16);
    _heap_end = ORIGIN(sdram) + LENGTH(sdram) - 256K - 4K;
    .stack _heap_end (NOLOAD) : {
        . += 256K;
        _stack_top = .;
        . += 4K;
        _trap_stack_top = .;
    } > sdram :data
    ASSERT(_heap_start + 8M <= _heap_end, "Image leaves less than 8 MiB for the heap")
}
