/*
 * startup.S - Multicore AArch64 startup for µOS++ on the Raspberry Pi 4 B
 *             (BCM2711, 4x Cortex-A72, 64-bit / kernel8.img, arm_64bit=1).
 *
 * The Zero 2 W's startup.S (boards/rpi-zero-2w/src/) with one addition: an
 * EL3 entry path. The Pi 4 firmware's armstub8 and QEMU raspi4b -kernel both
 * hand over at EL2, as on the Pi 3; a loader that starts the image at EL3
 * (QEMU with an ELF and no boot stub, or a custom bootloader that stays in
 * EL3) is dropped to non-secure EL2 first, with CNTFRQ set to the 54 MHz
 * crystal if nobody did -- only EL3 can write it.
 *
 * Interrupts are the GIC-400's (port_sys.cpp, rtos/port_isr.cpp); nothing in
 * this file depends on the interrupt controller.
 *
 * Entry (both real HW armstub and QEMU raspi4b -kernel):
 *   - primary core 0 enters _start at EL2 (or EL1, or EL3), MMU/caches off.
 *   - secondaries are held in the spin-table; start_secondary_cores() releases
 *     each by writing &_start to its release slot (0xD8 + 8*core) + SEV. They
 *     then run this same code, drop to EL1, and dispatch to secondary_park.
 *
 * Every core: (EL2->)EL1, enable FP/SIMD, install VBAR_EL1, then dispatch by
 *   core 0   -> core0_start : stack, .bss clear, mmu_init, hooks, main
 *   core 1-3 -> secondary_park : WFE on __smp_spin[core] -> coreN_entry
 *
 * The scheduler context frame (800 bytes, saved on the thread's own SP_EL1
 * stack) matches ctx64 in os-core.cpp and the restore paths below:
 *   [  0]  x0..x30            (31 * 8 = 248)
 *   [248]  SPSR_EL1, ELR_EL1  (16)
 *   [264]  FPSR, FPCR         (16)   [280] pad
 *   [288]  q0..q31            (32 * 16 = 512)
 */

/* PORT_RAM_BASE/PORT_RAM_END: the board's DRAM window, shared with the C++
 * validate_context() so the cooperative switch's bounds check is not a second
 * copy of the memory map. */
#include "board_ram.h"

	.equ FRAME, 800

/* ------------------------------------------------------------------ macros */

	/* Save the full exception frame on the current SP (SP_EL1). */
	.macro SAVE_FRAME
	sub	sp, sp, #FRAME
	stp	x0,  x1,  [sp, #0]
	stp	x2,  x3,  [sp, #16]
	stp	x4,  x5,  [sp, #32]
	stp	x6,  x7,  [sp, #48]
	stp	x8,  x9,  [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30,      [sp, #240]
	mrs	x0, spsr_el1
	mrs	x1, elr_el1
	stp	x0, x1,   [sp, #248]
	mrs	x0, fpsr
	mrs	x1, fpcr
	stp	x0, x1,   [sp, #264]
	stp	q0,  q1,  [sp, #288]
	stp	q2,  q3,  [sp, #320]
	stp	q4,  q5,  [sp, #352]
	stp	q6,  q7,  [sp, #384]
	stp	q8,  q9,  [sp, #416]
	stp	q10, q11, [sp, #448]
	stp	q12, q13, [sp, #480]
	stp	q14, q15, [sp, #512]
	stp	q16, q17, [sp, #544]
	stp	q18, q19, [sp, #576]
	stp	q20, q21, [sp, #608]
	stp	q22, q23, [sp, #640]
	stp	q24, q25, [sp, #672]
	stp	q26, q27, [sp, #704]
	stp	q28, q29, [sp, #736]
	stp	q30, q31, [sp, #768]
	.endm

	/* Restore the full exception frame from the current SP and ERET. */
	.macro RESTORE_FRAME_ERET
	ldp	q0,  q1,  [sp, #288]
	ldp	q2,  q3,  [sp, #320]
	ldp	q4,  q5,  [sp, #352]
	ldp	q6,  q7,  [sp, #384]
	ldp	q8,  q9,  [sp, #416]
	ldp	q10, q11, [sp, #448]
	ldp	q12, q13, [sp, #480]
	ldp	q14, q15, [sp, #512]
	ldp	q16, q17, [sp, #544]
	ldp	q18, q19, [sp, #576]
	ldp	q20, q21, [sp, #608]
	ldp	q22, q23, [sp, #640]
	ldp	q24, q25, [sp, #672]
	ldp	q26, q27, [sp, #704]
	ldp	q28, q29, [sp, #736]
	ldp	q30, q31, [sp, #768]
	ldp	x2, x3,   [sp, #264]
	msr	fpsr, x2
	msr	fpcr, x3
	ldp	x2, x3,   [sp, #248]
	msr	spsr_el1, x2
	msr	elr_el1, x3
	ldp	x2,  x3,  [sp, #16]
	ldp	x4,  x5,  [sp, #32]
	ldp	x6,  x7,  [sp, #48]
	ldp	x8,  x9,  [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30,      [sp, #240]
	ldp	x0,  x1,  [sp, #0]
	add	sp, sp, #FRAME
	eret
	.endm

	/* SMP deferred publish: with SP already off the outgoing thread's stack,
	 * store the staged {&old->stack_ptr = frame} so other cores may claim it.
	 * Clobbers x9-x13. */
	.macro SMP_PUBLISH
	mrs	x9, mpidr_el1
	and	x9, x9, #0xFF
	adrp	x10, _smp_pub_addr
	add	x10, x10, #:lo12:_smp_pub_addr
	ldr	x11, [x10, x9, lsl #3]
	cbz	x11, 99f
	adrp	x12, _smp_pub_val
	add	x12, x12, #:lo12:_smp_pub_val
	ldr	x13, [x12, x9, lsl #3]
	str	x13, [x11]
	str	xzr, [x10, x9, lsl #3]
	dmb	ish
99:
	.endm

/* ------------------------------------------------------------------ */
/* Early-boot console markers (DEBUG_BOOT build only, asm-level).      */
/*                                                                     */
/* BOOT_MSG <label> prints a NUL-terminated string via ARM semihosting */
/* SYS_WRITE0 (hlt #0xF000, x0=4, x1=string) without touching the      */
/* stack, so it works from the very first instructions of _start (EL2, */
/* MMU/caches off). The string lands in the OpenOCD host log when a    */
/* debugger is attached (hw.sh enables semihosting before it resumes   */
/* the cores).                                                         */
/*                                                                     */
/* BOOT_ELMSG <el2sym> <el1sym> prints one of two strings selected by  */
/* CurrentEL, so a marker can CONFIRM the exception level a block is   */
/* running at (the strings must be adjacent; the selection is a fixed  */
/* add of the label difference).                                       */
/*                                                                     */
/* Markers are deliberately semihosting-ONLY: an unclocked/unmuxed     */
/* PL011 (config.txt without enable_uart=1) could make a raw-UART      */
/* marker spin forever and hide the real failure.                      */
/*                                                                     */
/* Clobbers x0/x1/x30 (caller state is dead across all call sites in   */
/* startup.S; markers are never inserted where a live value survives). */
/*                                                                     */
/* Why: bring-up of the real hardware (like the 32b port) needs to see */
/* how far boot gets. Each stage prints its marker; the LAST one seen   */
/* before a hang/fault names the exact stage that failed.               */
/* ------------------------------------------------------------------ */
#ifdef DEBUG_BOOT
	.macro BOOT_MSG _sym
	adrp	x0, \_sym
	add	x0, x0, #:lo12:\_sym
	bl	boot_semihost_str
	.endm
	.macro BOOT_ELMSG _el2sym _el1sym
	mrs	x1, CurrentEL
	lsr	x1, x1, #2
	adrp	x0, \_el2sym
	add	x0, x0, #:lo12:\_el2sym
	cmp	x1, #1
	b.ne	91f
	add	x0, x0, #(\_el1sym - \_el2sym)
91:	bl	boot_semihost_str
	.endm
#else
	.macro BOOT_MSG _sym
	.endm
	.macro BOOT_ELMSG _el2sym _el1sym
	.endm
#endif

/* ---------------------------------------------------------------- boot */
	.section .text.boot, "ax"
	.global _start
_start:
	msr	daifset, #0xf              // mask D,A,I,F

	BOOT_ELMSG b0el2_msg, b0el1_msg

	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #3
	b.eq	el3_entry                 // EL3: drop to non-secure EL2 first
el2_check:
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	cmp	x0, #2
	b.ne	el1_entry                 // already at EL1 -> skip EL2 setup

	/* ---- EL2 setup, then drop to EL1 ---- */
	BOOT_MSG b1_msg
	// SCTLR_EL2: clear alignment/stack-align checks.
	mrs	x0, sctlr_el2
	bic	x0, x0, #(1 << 1)
	bic	x0, x0, #(1 << 3)
	bic	x0, x0, #(1 << 4)
	msr	sctlr_el2, x0

	// Do not trap FP/SIMD from EL1/EL0 to EL2.
	mov	x0, #0x33ff
	msr	cptr_el2, x0

	// Let EL1 use the physical counter/timer (CNTHCTL_EL2.EL1PCTEN|EL1PCEN).
	mrs	x0, cnthctl_el2
	orr	x0, x0, #0x3
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr

	// EL1 runs in AArch64 (HCR_EL2.RW = 1).
	mov	x0, #(1 << 31)
	msr	hcr_el2, x0

	// Minimal SCTLR_EL1 (MMU/caches off, no alignment faults).
	// DZE (bit 14) set: GCC's vectorized memset uses DC ZVA to zero blocks,
	// which faults at EL1 unless SCTLR_EL1.DZE=1 (QEMU does not model the
	// trap — real silicon aborts in the very first memset of mmu_init).
	ldr	x0, =0x30d04800           // RES1 bits, DZE, M/C/I clear, A clear
	msr	sctlr_el1, x0

	// Return to EL1h with all interrupts masked.
	mov	x0, #0x3c5                // DAIF=1111, M=EL1h(0b0101)
	msr	spsr_el2, x0
	adr	x0, el1_entry
	msr	elr_el2, x0
	BOOT_MSG b2_msg
	eret

	/* ---- EL3: set what only EL3 can, then eret to non-secure EL2 ---- */
el3_entry:
	// CNTFRQ_EL0 is writable only at EL3. The Pi 4's generic timer runs
	// from the 54 MHz crystal; keep a value a loader already set.
	mrs	x0, cntfrq_el0
	cbnz	x0, 1f
	ldr	x0, =54000000
	msr	cntfrq_el0, x0
1:	msr	cptr_el3, xzr             // no FP/SIMD traps to EL3
	// SCR_EL3: NS | RES1[5:4] | SMD | HCE | RW (lower ELs are AArch64).
	mov	x0, #0x5b1
	msr	scr_el3, x0
	mov	x0, #0x3c9                // EL2h, DAIF masked
	msr	spsr_el3, x0
	adr	x0, el2_check
	msr	elr_el3, x0
	eret

el1_entry:
	BOOT_ELMSG b3el2_msg, b3el1_msg
#ifdef DEBUG_BOOT
	// Temporary minimal VBAR_EL1 so any fault in the EL1 bring-up below
	// (CPACR_EL1 / VBAR install) is CAUGHT and reported (ESR/ELR/FAR via
	// semihosting) instead of vectoring through the uninitialised firmware
	// table to a junk PC. Replaced by the real vector_table below.
	adrp	x0, early_vectors
	add	x0, x0, #:lo12:early_vectors
	msr	vbar_el1, x0
	isb
#endif
	// SMP coherency (CPUECTLR_EL1.SMPEN) is ALREADY enabled by the boot
	// firmware's armstub at EL3 before we are entered (on the real Pi it is
	// what lets cores 1-3 run at all), and QEMU does not need it. Do NOT
	// write CPUECTLR_EL1 from EL1 here: the real Cortex-A53 reads fine but an
	// EL1 WRITE takes the core down (same rule on the A72) (QEMU models it as harmless, which is why
	// this only ever failed on hardware).
	//
	// Enable FP/SIMD at EL1/EL0 (CPACR_EL1.FPEN = 0b11).
	mov	x0, #(3 << 20)
	msr	cpacr_el1, x0
	isb
	BOOT_MSG c2_msg

	// Install the real exception vectors.
	adrp	x0, vector_table
	add	x0, x0, #:lo12:vector_table
	msr	vbar_el1, x0
	isb

	BOOT_MSG b4_msg

	// Dispatch by MPIDR.Aff0.
	mrs	x0, mpidr_el1
	and	x0, x0, #0xFF
	cbz	x0, core0_start
	b	secondary_park

/* ---------------------------------------------------------- core 0 */
core0_start:
	ldr	x0, =__stack_top
	mov	sp, x0

	BOOT_MSG b5_msg

	// Zero .bss.
	ldr	x0, =__bss_start
	ldr	x1, =__bss_end
1:	cmp	x0, x1
	b.hs	2f
	str	xzr, [x0], #8
	b	1b
2:
	BOOT_MSG b6_msg
	bl	mmu_init
	BOOT_MSG b7_msg
	bl	os_startup_initialize_hardware_early
	BOOT_MSG b8_msg
	bl	os_startup_initialize_hardware
	bl	__libc_init_array
	BOOT_MSG b9_msg
	bl	main
hang0:	wfe
	b	hang0

/* ------------------------------------------------ secondary cores 1-3 */
	.global secondary_park
secondary_park:
	mrs	x4, mpidr_el1
	and	x4, x4, #0xFF
	ldr	x5, =__smp_spin
1:	wfe
	ldr	x6, [x5, x4, lsl #3]       // __smp_spin[core] (entry pointer)
	cbz	x6, 1b
	br	x6

	.global core1_entry, core2_entry, core3_entry
core1_entry:
	ldr	x0, =__stack_top_core1
	mov	sp, x0
	BOOT_MSG s1e_msg
	bl	mmu_enable
	BOOT_MSG s1m_msg
	bl	boot_core1
1:	wfe
	b	1b
core2_entry:
	ldr	x0, =__stack_top_core2
	mov	sp, x0
	BOOT_MSG s2e_msg
	bl	mmu_enable
	BOOT_MSG s2m_msg
	bl	boot_core2
1:	wfe
	b	1b
core3_entry:
	ldr	x0, =__stack_top_core3
	mov	sp, x0
	BOOT_MSG s3e_msg
	bl	mmu_enable
	BOOT_MSG s3m_msg
	bl	boot_core3
1:	wfe
	b	1b

/* ---------------------------------------------- early boot markers */
#ifdef DEBUG_BOOT
	/* x0 = string. One SYS_WRITE0 via hlt (openocd "arm semihosting"). */
	.global boot_semihost_str
boot_semihost_str:
	mov	x1, x0
	mov	x0, #4
	hlt	#0xF000
	ret

	/* Marker strings live in .text.boot too, so they are in the same
	 * identity-mapped image the GPU loads at 0x80000 (no rodata-to-text
	 * address split, valid before and after mmu_init). b0el1_msg must be
	 * reachable as (b0el2_msg + const), likewise b3el1_msg vs b3el2_msg. */
b0el2_msg: .asciz "[B0] _start enter EL2\n"
b0el1_msg: .asciz "[B0] _start enter EL1\n"
b1_msg: .asciz "[B1] EL2: setup registers\n"
b2_msg: .asciz "[B2] EL2 done, eret -> EL1\n"
b3el2_msg: .asciz "[B3] el1_entry ran at EL2 (BAD)\n"
b3el1_msg: .asciz "[B3] el1_entry at EL1\n"
c2_msg: .asciz "[c2] CPACR_EL1 ok\n"
b4_msg: .asciz "[B4] vectors ok, dispatch\n"
b5_msg: .asciz "[B5] core0: sp set\n"
b6_msg: .asciz "[B6] core0: .bss cleared\n"
b7_msg: .asciz "[B7] core0: mmu_init ok\n"
b8_msg: .asciz "[B8] core0: hw_early ok\n"
b9_msg: .asciz "[B9] core0: before main\n"
s1e_msg: .asciz "[S1] core1_entry\n"
s1m_msg: .asciz "[S1] core1 mmu_enable ok\n"
s2e_msg: .asciz "[S2] core2_entry\n"
s2m_msg: .asciz "[S2] core2 mmu_enable ok\n"
s3e_msg: .asciz "[S3] core3_entry\n"
s3m_msg: .asciz "[S3] core3 mmu_enable ok\n"
exc_hdr: .asciz "\n[EXC@EL1] "
exc_esr: .asciz "ESR="
exc_elr: .asciz " ELR="
exc_far: .asciz " FAR="
exc_spsr: .asciz " SPSR="
exc_nl: .asciz "\n"
	.balign 8
exc_ch: .space 8

	/* SYS_WRITEC (0x03): write the char at x0. */
boot_semihost_char:
	mov	x1, x0
	mov	x0, #3
	hlt	#0xF000
	ret

	/* Print x2 as 16 hex digits via semihosting (no stack). */
boot_hex64:
	mov	x8, x30                 // keep link across the inner bl
	mov	x4, #60
1:	lsrv	x5, x2, x4
	and	x5, x5, #0xf
	cmp	x5, #9
	add	x6, x5, #0x30
	bls	2f
	add	x6, x5, #0x37
2:	ldr	x7, =exc_ch
	strb	w6, [x7]
	mov	x0, x7
	bl	boot_semihost_char
	subs	x4, x4, #4
	b.ge	1b
	br	x8

	/* Exception taken during EL1 bring-up: dump ESR/ELR/FAR/SPSR, park. */
early_exc_handler:
	mrs	x4, esr_el1
	mrs	x5, elr_el1
	mrs	x6, far_el1
	mrs	x7, spsr_el1
	adrp	x0, exc_hdr
	add	x0, x0, #:lo12:exc_hdr
	bl	boot_semihost_str
	adrp	x0, exc_esr
	add	x0, x0, #:lo12:exc_esr
	bl	boot_semihost_str
	mov	x2, x4
	bl	boot_hex64
	adrp	x0, exc_elr
	add	x0, x0, #:lo12:exc_elr
	bl	boot_semihost_str
	mov	x2, x5
	bl	boot_hex64
	adrp	x0, exc_far
	add	x0, x0, #:lo12:exc_far
	bl	boot_semihost_str
	mov	x2, x6
	bl	boot_hex64
	adrp	x0, exc_spsr
	add	x0, x0, #:lo12:exc_spsr
	bl	boot_semihost_str
	mov	x2, x7
	bl	boot_hex64
	adrp	x0, exc_nl
	add	x0, x0, #:lo12:exc_nl
	bl	boot_semihost_str
1:	wfi
	b	1b

	/* All 16 vectors of the temporary table funnel to the dumper. */
	.align 11
early_vectors:
	.rept 16
	b	early_exc_handler
	.align 7
	.endr
#endif

/* ---------------------------------------------- cooperative switch */
/* Called as a normal function from scheduler::reschedule() at EL1h. Saves the
 * caller continuation as a frame, picks the next thread, and ERETs into it. */
	.global port_ctx_switchHandler
port_ctx_switchHandler:
	// Build SPSR from the CURRENT interrupt state + EL1h, ELR = return addr.
	mrs	x9, daif
	orr	x9, x9, #1               // M[0]
	orr	x9, x9, #4               // M[2]  -> M = 0b0101 (EL1h)
	msr	daifset, #2               // mask IRQ for the critical section
	msr	spsr_el1, x9
	msr	elr_el1, x30
	SAVE_FRAME
	mov	x0, sp
	bl	port_switch_stacks        // x0 = next thread's frame SP
	/* Bounds are the board's DRAM window, the same two numbers the C++
	 * validate_context() checks; board_ram.h is the one place they are
	 * written, so the assembler and the compiler cannot disagree. */
	ldr	x1, =PORT_RAM_BASE
	cmp	x0, x1
	b.lo	.Lcoop_corrupt
	ldr	x1, =PORT_RAM_END
	cmp	x0, x1
	b.hs	.Lcoop_corrupt
	mov	sp, x0
	SMP_PUBLISH
	RESTORE_FRAME_ERET
.Lcoop_corrupt:
	msr	daifset, #0xf
	mov	x1, sp
	bl	port_ctx_switch_corrupt
3:	wfi
	b	3b

/* ---------------------------------------------- EL1 vector table */
	.section .text.boot, "ax"
	.align 11
	.global vector_table
vector_table:
	// Current EL with SP0 (unused; threads run EL1h/SPx)
	.align 7
	mov	x19, #0x1
	b	fatal_from_vec
	.align 7
	mov	x19, #0x2
	b	fatal_from_vec
	.align 7
	mov	x19, #0x3
	b	fatal_from_vec
	.align 7
	mov	x19, #0x4
	b	fatal_from_vec
	// Current EL with SPx  (EL1h: this is where threads take exceptions)
	.align 7
	b	sync_current
	.align 7
	b	irq_current
	.align 7
	b	irq_current               // FIQ -> same scheduler path
	.align 7
	mov	x19, #0x14
	b	fatal_from_vec
	// Lower EL, AArch64
	.align 7
	mov	x19, #0x21
	b	fatal_from_vec
	.align 7
	mov	x19, #0x22
	b	fatal_from_vec
	.align 7
	mov	x19, #0x23
	b	fatal_from_vec
	.align 7
	mov	x19, #0x24
	b	fatal_from_vec
	// Lower EL, AArch32
	.align 7
	mov	x19, #0x31
	b	fatal_from_vec
	.align 7
	mov	x19, #0x32
	b	fatal_from_vec
	.align 7
	mov	x19, #0x33
	b	fatal_from_vec
	.align 7
	mov	x19, #0x34
	b	fatal_from_vec

/* IRQ / FIQ from current EL (SPx): preemptive scheduler switch. */
irq_current:
	SAVE_FRAME
	mov	x0, sp
	bl	port_isr_handler          // x0 = (possibly new) frame SP
	mov	sp, x0
	SMP_PUBLISH
	RESTORE_FRAME_ERET

/* Synchronous exception from current EL: fatal. */
sync_current:
	mov	x19, #0x11
	b	fatal_from_vec

/* Common fatal path: x0 = exception type id. Saves a frame for the dump. */
fatal_from_vec:
	SAVE_FRAME
	mov	x0, x19                   // type (set by the vector stub)
	mrs	x1, esr_el1
	mrs	x2, elr_el1
	mrs	x3, far_el1
	mrs	x4, spsr_el1
	bl	port_fatal_exception
1:	wfi
	b	1b
