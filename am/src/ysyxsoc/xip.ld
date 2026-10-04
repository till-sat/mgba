/* SPDX-License-Identifier: MPL-2.0 */
OUTPUT_ARCH(riscv)
ENTRY(_start)

/* XIP mode is for NPC bring-up. Writable state lives in the 4 MiB PSRAM. */
MEMORY {
    flash (rx)  : ORIGIN = 0x30000000, LENGTH = 16M
    psram (rwx) : ORIGIN = 0x80000000, LENGTH = 4M
}
PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text .text.*) } > flash :text
    .rodata : { *(.rodata .rodata.*) } > flash :rodata
    .data : ALIGN(16) {
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(16);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        _data_end = .;
    } > psram AT > flash :data
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : ALIGN(16) {
        _bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(16);
        _bss_end = .;
    } > psram :data
    _heap_start = ALIGN(16);
    _heap_end = ORIGIN(psram) + LENGTH(psram) - 256K - 4K;
    .stack _heap_end (NOLOAD) : {
        . += 256K;
        _stack_top = .;
        . += 4K;
        _trap_stack_top = .;
    } > psram :data
    ASSERT(_heap_start + 512K <= _heap_end, "XIP image leaves less than 512 KiB for the heap")
}
