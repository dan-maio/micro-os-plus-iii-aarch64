/*
 * linker.ld - µOS++ AArch64 for the Raspberry Pi Zero 2W (BCM2837, 4x A53).
 * kernel8.img is loaded by the GPU firmware at 0x80000 (arm_64bit=1) and runs
 * from the 512 MB DRAM (0x00000000 .. 0x1FFFFFFF).
 *
 * AArch64 has no banked CPU modes: each core needs a single stack (SP_EL1),
 * on which exceptions are also handled — so the stacks are generous.
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
   RAM (rwx) : ORIGIN = 0x00080000, LENGTH = 0x1FF80000   /* 0x80000 .. 0x1FFFFFFF */
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
