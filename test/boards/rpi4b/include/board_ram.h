/*
 * board_ram.h - the DRAM window the Pi 4 B images live in, in a form both
 * the compiler and the assembler can include.
 *
 * The port checks a stack pointer against this window before switching to it
 * (port_isr.cpp via smp.hpp, and the cooperative switch in startup.S), so a
 * frame built from a wild pointer stops here instead of somewhere
 * unrecognisable later.
 *
 * The window is the ARM part of the first GB: everything below the VideoCore
 * memory. The firmware puts that at the top of the first GB -- gpu_mem=76 MB
 * by default on the 4 GB board (0x3B400000), 64 MB under QEMU raspi4b
 * (0x3C000000) -- so 0x3B400000 is below it in both. This is the same number
 * as RAM_TOP in mmu.cpp and the end of the RAM region in linker.ld: three
 * copies of one fact, to be changed together.
 *
 * No integer suffix: startup.S pastes these straight into an `ldr xN, =...`.
 */
#pragma once

#define PORT_RAM_BASE 0x00080000
#define PORT_RAM_END  0x3B400000   /* below gpu_mem (76 MB default): matches linker.ld */
