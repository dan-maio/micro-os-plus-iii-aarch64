/**
 * @file  mmu.hpp
 * @brief AArch64 MMU bring-up for the Raspberry Pi Zero 2W (BCM2837).
 */
#pragma once

extern "C" {

// Build the shared translation tables (core 0 only) AND enable the MMU on
// core 0. Identity map: RAM Normal WB cacheable Inner-Shareable; peripherals
// (0x3F000000 block and the 0x40000000 local-peripheral GB) Device-nGnRnE.
void mmu_init(void);

// Enable the MMU on a secondary core using the tables already built by
// mmu_init(). Joins the core to the (cacheable, shareable) coherency domain.
void mmu_enable(void);

}
