/*
 * linker-rpi3b.ld - µOS++ AArch64 for the Raspberry Pi 3 B (BCM2837, 1 GB RAM).
 * kernel8.img is loaded by the GPU firmware at 0x80000 (arm_64bit=1) and runs
 * from DRAM. Identical layout to linker.ld, except the RAM region extends to
 * ~1 GB (0x80000 .. 0x3EFFFFFF, stopping below the 0x3F000000 peripherals).
 * Select with:  make BOARD=rpi3b
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
   RAM (rwx) : ORIGIN = 0x00080000, LENGTH = 0x3EF80000   /* 0x80000 .. 0x3EFFFFFF (Pi 3 B) */
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
