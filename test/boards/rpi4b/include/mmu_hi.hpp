/**
 * @file  mmu_hi.hpp
 * @brief The RAM above the first GB, page by page (smp_mem_test only).
 *
 * Defined by boards/rpi4b/src/mmu.cpp only when the image defines
 * UOS_SMP_MEM_TEST (test/rpi4b/tests.cmake). Every page of the region starts
 * unmapped; a test maps the pages of a block while it owns them.
 */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
// The region: [*base, *end), 4 KB aligned. Hardware 0x40000000..0xFC000000,
// QEMU raspi4b (2 GB) 0x40000000..0x80000000.
void mmu_hi_region(std::uintptr_t* base, std::uintptr_t* end);
// Map (map != 0) or unmap the 4 KB pages of [va, va + bytes); page multiples.
void mmu_hi_map(std::uintptr_t va, std::size_t bytes, int map);
// 1 when a read of va translates, 0 when it would fault (AT S1E1R, no access).
int mmu_hi_probe(std::uintptr_t va);
}
