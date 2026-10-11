/*
 * linker.ld - µOS++ AArch64 for the Raspberry Pi 4 B (BCM2711, 4x Cortex-A72).
 *
 * The image is loaded at 0x80000 -- by OpenOCD over JTAG for the hwd runs, by
 * QEMU raspi4b -kernel for the qemu runs, or by the firmware (kernel8.img,
 * arm_64bit=1) -- and runs from DRAM. The Pi 3 B layout (linker-rpi3b.ld)
 * with the RAM region ending below the VideoCore memory: 0x80000 ..
 * 0x3B3FFFFF. The top of the first GB is gpu_mem (76 MB by default on the
 * 4 GB board, 64 MB under QEMU), and the peripherals are no longer in it
 * (they moved to 0xFC000000). PORT_RAM_END (include/board_ram.h) and RAM_TOP
 * (src/mmu.cpp) are this same number.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* Core 0 (µOS++ main/task + ISR) stack. */
STACK_SIZE   = 0x100000;   /* 1 MiB  core 0 */
/* Secondary cores 1-3 stacks. */
W_STACK_SIZE = 0x80000;    /* 512 KiB each */

HEAP_SIZE = 0x04000000;    /* 64 MiB newlib/µOS++ heap */

MEMORY
{
   RAM (rwx) : ORIGIN = 0x00080000, LENGTH = 0x3B380000   /* 0x80000 .. 0x3B3FFFFF (Pi 4 B) */
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : ALIGN(8) {
        KEEP(*(.text.boot))     /* _start + vector_table must be first */
        *(.text*)
    } > RAM

    .rodata : ALIGN(8) {
        *(.rodata*)
    } > RAM

    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM

    .fini_array : ALIGN(8) {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } > RAM

    .data : ALIGN(8) {
        *(.data*)
    } > RAM

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM

    /* µOS++ interrupts stack (OS_HAS_INTERRUPTS_STACK). AArch64 handles ISRs
     * on the thread's own SP_EL1, so this region is only what µOS++ records
     * for its bookkeeping; base = __fiq_stack_top (low), top = __irq_stack_top. */
    .irqstack (NOLOAD) : ALIGN(16) {
        __fiq_stack_top = .;
        . = . + 0x40000;          /* 256 KiB */
        __irq_stack_top = .;
    } > RAM

    /* --- Per-core stacks (single SP_EL1 each) --- */
    .stack (NOLOAD) : ALIGN(16) {
        . = . + STACK_SIZE;   __stack_top = .;
        _stack_top = __stack_top;
        . = . + W_STACK_SIZE; __stack_top_core1 = .;
        . = . + W_STACK_SIZE; __stack_top_core2 = .;
        . = . + W_STACK_SIZE; __stack_top_core3 = .;
    } > RAM

    /* Heap (µOS++ free store) */
    .heap (NOLOAD) : ALIGN(16) {
        _Heap_Begin = .;
        __heap_start = .;
        . = . + HEAP_SIZE;
        __heap_end = .;
        _Heap_Limit = .;
    } > RAM

    _end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
