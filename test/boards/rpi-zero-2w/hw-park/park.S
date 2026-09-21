/* hw-park — parking kernel8.img for J-Link + OpenOCD debug-in-RAM sessions.
 *
 * The 64-bit hardware tests are run the 32-bit way: PURE OpenOCD loads the test
 * ELF into RAM over whatever kernel8.img the SD card booted. On AArch64 there is
 * no debugger CPSR trick to force an exception level (unlike the AArch32 port's
 * "resume in HYP"), so the cleanest, fully deterministic attach point is to let
 * the SD card boot a kernel that does *nothing*:
 *
 *   - core 0 enters here straight from the GPU firmware at EL2 with the MMU and
 *     caches OFF and simply parks (WFE) — it never enables EL1, never dirties a
 *     cache line, never touches RAM;
 *   - cores 1-3 stay parked in the GPU arm-stub spin-table loop at EL2.
 *
 * Every core is then halted by OpenOCD in a known state (AArch64 EL2, MMU off)
 * before hw.sh loads the test image and resumes all four cores at _start.
 * startup.S handles the EL2 -> EL1 drop itself.
 *
 * Before parking, each core prints one line on the raw PL011 (the GPU firmware
 * has already muxed+clocked UART0 when config.txt has enable_uart=1), e.g.
 *
 *     [hw-park] core 0 at EL2 parking
 *
 * so a cold SD boot can be verified on the physical serial WITHOUT a debugger:
 * if you see all four "parking" lines, the AArch64 boot works and OpenOCD can
 * attach; if nothing appears, the boot card is still AArch32 (kernel7 / no
 * arm_64bit) and the aarch64 target will misread the cores.
 *
 * Build:      make               -> output/kernel8.img
 * Deploy:     copy output/kernel8.img to the FAT32 boot partition (as
 *             kernel8.img). config.txt already has arm_64bit=1 / kernel=kernel8.img.
 *
 * NOTE: this replaces the SD card's kernel8.img only. To go back to a test
 * kernel (or a real OS), copy its kernel8.img back over it.
 */

.section .text.boot, "ax"
.global _start

/* Firmware jumps here at 0x80000 (arm_64bit=1). Print, then park forever. */
_start:
	ldr	x6, =0x3F201018          // FR
	ldr	x7, =0x3F201000          // DR
	adrp	x0, park_str0
	add	x0, x0, #:lo12:park_str0
	bl	park_puts
	mrs	x4, mpidr_el1
	and	x4, x4, #3
	add	x4, x4, #0x30
	mov	w2, w4
	bl	park_putc
	adrp	x0, park_str1
	add	x0, x0, #:lo12:park_str1
	bl	park_puts
	mrs	x4, CurrentEL
	lsr	x4, x4, #2
	add	x4, x4, #0x30
	mov	w2, w4
	bl	park_putc
	adrp	x0, park_str2
	add	x0, x0, #:lo12:park_str2
	bl	park_puts

park_loop:
	wfe
	b	park_loop

	/* w2 = char -> PL011 (x6 = FR, x7 = DR); '\n' becomes CR+LF. */
park_putc:
	cmp	w2, #0x0a
	b.ne	2f
1:	ldr	w3, [x6]
	tbnz	w3, #5, 1b              // FR.TXFF
	mov	w3, #0x0d
	str	w3, [x7]
2:	ldr	w3, [x6]
	tbnz	w3, #5, 2b              // FR.TXFF
	str	w2, [x7]
	ret

	/* x0 = NUL-terminated string -> PL011. Saves its own link in x8. */
park_puts:
	mov	x8, x30
1:	ldrb	w2, [x0], #1
	cbz	w2, 2f
	bl	park_putc
	b	1b
2:	br	x8

park_str0: .asciz "\r\n[hw-park] core "
park_str1: .asciz " at EL"
park_str2: .asciz " parking\r\n"
