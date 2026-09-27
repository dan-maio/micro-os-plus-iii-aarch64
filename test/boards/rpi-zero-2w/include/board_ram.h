/*
 * board_ram.h - the DRAM window this board's images live in, in a form both
 * the compiler and the assembler can include.
 *
 * The port checks a stack pointer against this window before switching to it,
 * so a frame built from a wild pointer stops here instead of somewhere
 * unrecognisable later. The check runs in C++ (port_isr.cpp, via smp.hpp) and
 * in the cooperative switch in startup.S, which is why the two numbers live in
 * a preprocessor-only header rather than in one language's source.
 *
 * No integer suffix: startup.S pastes these straight into an `ldr xN, =...`.
 * BOARD_RPI3B (boards/rpi3b/board.cmake) selects the Pi 3 B, exactly as it
 * does for the LED driver, the banner and the RAM_TOP in mmu.cpp. This window
 * is the same fact as that RAM_TOP and as the linker region: a 1 GB board is
 * linked and mapped to 0x3F000000, so a stack pointer is validated against
 * 0x3F000000 too. Without this branch every thread stack above 512 MiB looked
 * corrupt to validate_context().
 */
#pragma once

#define PORT_RAM_BASE 0x00080000
#ifdef BOARD_RPI3B
#define PORT_RAM_END  0x3F000000   /* Pi 3 B, 1 GB: matches linker-rpi3b.ld */
#else
#define PORT_RAM_END  0x20000000   /* Pi Zero 2 W, 512 MiB: matches linker.ld */
#endif
