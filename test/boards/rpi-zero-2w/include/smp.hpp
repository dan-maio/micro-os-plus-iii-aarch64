#pragma once

// The DRAM window this board's images live in. The port checks a stack
// pointer against it before switching to it, so a frame built from a wild
// pointer stops here instead of somewhere unrecognisable later.
//
// No integer suffix: these are pasted into comparisons and, on the AArch32
// port, straight into inline asm.
// BOARD_RPI3B (boards/rpi3b/board.cmake) selects the Pi 3 B, exactly as it
// does for the LED driver, the banner and the RAM_TOP in mmu.cpp. This window
// is the same fact as that RAM_TOP and as the linker region: a 1 GB board is
// linked and mapped to 0x3F000000, so a stack pointer is validated against
// 0x3F000000 too. Without this branch every thread stack above 512 MiB looked
// corrupt to validate_context().
#define PORT_RAM_BASE 0x00080000
#ifdef BOARD_RPI3B
#define PORT_RAM_END  0x3F000000   // Pi 3 B, 1 GB: matches linker-rpi3b.ld
#else
#define PORT_RAM_END  0x20000000   // Pi Zero 2 W, 512 MiB: matches linker.ld
#endif

#include <cstdint>
#include <cmsis-plus/rtos/port/os-decls.h>

extern "C" {

// Per-core join/heartbeat status (0 = not started, 3 = scheduler entered).
extern volatile std::uint32_t g_core_stage[OS_NCPU];

// Secondary-core entry points (asm coreN_entry -> these).
void boot_core1();
void boot_core2();
void boot_core3();

// asm per-core secondary trampolines (set banked stacks + MMU, call boot_coreN).
void core1_entry(void);
void core2_entry(void);
void core3_entry(void);

// Secondary launch spin-table (indexed by core id): 64-bit entry pointers.
extern volatile std::uint64_t __smp_spin[OS_NCPU];

}

namespace smp {

// Release cores 1..3: publish each core's entry into __smp_spin + SEV (wakes
// QEMU cores already parked in secondary_park), and poke the BCM2837
// mailbox-3 spin-table (releases armstub-held cores on real hardware).
void start_secondary_cores();

}

// Per-project: create the per-core idle threads for cores 1..3.
extern "C" void smp_install_boot_threads(void);
