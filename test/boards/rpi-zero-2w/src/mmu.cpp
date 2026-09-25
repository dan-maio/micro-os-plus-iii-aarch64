/**
 * @file  mmu.cpp
 * @brief AArch64 MMU for the Raspberry Pi Zero 2W (BCM2837, 4x Cortex-A53).
 *
 * Pi Zero 2W has 512 MB of RAM (0x00000000 - 0x1FFFFFFF). Building with
 * `make BOARD=rpi3b` (-DBOARD_RPI3B) targets the Raspberry Pi 3 B instead,
 * whose 1 GB of RAM is mapped all the way up to (but not including) the
 * 0x3F000000 peripheral window (0x00000000 - 0x3EFFFFFF). A cacheable,
 * Inner-Shareable Normal mapping of RAM (SMPEN is enabled by the boot
 * firmware's armstub at EL3 before we are entered) is what makes the
 * LDAXR/STLXR scheduler lock coherent across the four cores.
 *
 * Translation scheme (4 KB granule, T0SZ=30 -> start at level 1):
 *   L1[0] -> table -> L2 (covers 0x00000000 .. 0x3FFFFFFF, 512 x 2 MB blocks)
 *              entries   0 .. RAM_ENTRIES-1 : RAM    -> Normal WB, Inner-Shar.
 *              entries        .. 503        : no RAM -> invalid (fault)
 *              entries 504 .. 511 : 0x3F000000 -> Device-nGnRnE (peripherals)
 *              (RAM_ENTRIES = 256 for the 512 MB Zero 2 W,
 *                            504 for the 1 GB Pi 3 B)
 *   L1[1] -> 1 GB block 0x40000000 .. 0x7FFFFFFF -> Device (ARM local periph)
 *   L1[2..] : invalid
 */

#include <cstdint>
#include "mmu.hpp"

namespace {

constexpr std::uint64_t PTE_VALID   = (1ULL << 0);
constexpr std::uint64_t PTE_TABLE   = (1ULL << 1);   // at L0/L1: next-level table
constexpr std::uint64_t PTE_BLOCK   = (0ULL << 1);   // block descriptor
constexpr std::uint64_t PTE_AF      = (1ULL << 10);  // Access Flag
constexpr std::uint64_t PTE_SH_IS   = (3ULL << 8);   // Inner-Shareable
constexpr std::uint64_t PTE_SH_NONE = (0ULL << 8);   // Non-Shareable

constexpr std::uint64_t ATTR_IDX_NORMAL = (0ULL << 2);
constexpr std::uint64_t ATTR_IDX_DEVICE = (1ULL << 2);

// Top of usable DRAM (stop below the 0x3F000000 peripheral window).
#ifdef BOARD_RPI3B
constexpr std::uint64_t RAM_TOP       = 0x3F000000ULL;   // Pi 3 B: ~1 GB board
#else
constexpr std::uint64_t RAM_TOP       = 0x20000000ULL;   // Zero 2 W: 512 MB
#endif
constexpr std::uint64_t BLOCK_2M      = 2ULL * 1024 * 1024;
constexpr std::uint64_t BLOCK_1G      = 1ULL * 1024 * 1024 * 1024;
constexpr std::size_t   RAM_ENTRIES   = RAM_TOP / BLOCK_2M; // 256 (Z2W) / 504 (3B)
constexpr std::uint64_t PERIPH_START  = 0x3F000000ULL;
constexpr std::uint64_t PERIPH_END    = 0x40000000ULL;
constexpr std::uint64_t LOCAL_BASE    = 0x40000000ULL;

// SCTLR_EL1 bits.
constexpr std::uint64_t SCTLR_M   = (1ULL << 0);
constexpr std::uint64_t SCTLR_A   = (1ULL << 1);
constexpr std::uint64_t SCTLR_C   = (1ULL << 2);
constexpr std::uint64_t SCTLR_SA  = (1ULL << 3);
constexpr std::uint64_t SCTLR_SA0 = (1ULL << 4);
constexpr std::uint64_t SCTLR_I   = (1ULL << 12);
constexpr std::uint64_t SCTLR_RES1 =
    (1ULL << 29) | (1ULL << 28) | (1ULL << 23) |
    (1ULL << 22) | (1ULL << 20) | (1ULL << 11);
constexpr std::uint64_t SCTLR_DZE = (1ULL << 14);  // DC ZVA at EL1/EL0 (memset)

// Shared translation tables (4 KB each, 4 KB aligned). Zero-initialised in
// .bss (all entries invalid) until mmu_init() fills them on core 0.
alignas(4096) std::uint64_t l1[512];
alignas(4096) std::uint64_t l2[512];

#if defined(UOS_CMSIS_OS_VALIDATOR)
// The CMSIS-RTOS validator is Cortex-M code: at start it probes the DWT cycle
// counter by reading DWT_CTRL (0xE0001000) and DWT_CYCCNT (0xE0001004)
// directly. Those addresses are unmapped here and fault. Only the validator's
// image defines UOS_CMSIS_OS_VALIDATOR (test/rpi-zero-2w/tests.cmake); it gets
// one 4 KB RAM page at 0xE0001000 whose DWT_CTRL reads NOCYCCNT ("no cycle
// counter"), so the validator runs its wait tests without cycle measurements,
// as on a Cortex-M without a DWT. Every other test's tables are unchanged.
constexpr std::uint64_t DWT_BASE        = 0xE0001000ULL;
constexpr std::uint32_t DWT_CTRL_NOCYCCNT = (1u << 25);
constexpr std::uint64_t PTE_PAGE        = (1ULL << 1);   // at L3: page

alignas(4096) std::uint64_t l2_high[512];  // 0xC0000000 .. 0xFFFFFFFF
alignas(4096) std::uint64_t l3_dwt[512];   // 0xE0000000 .. 0xE01FFFFF
alignas(4096) std::uint32_t dwt_page[1024];
#endif

std::uint64_t make_block(std::uint64_t pa, std::uint64_t attr_idx,
                         std::uint64_t sh) noexcept {
    return pa | PTE_VALID | PTE_BLOCK | PTE_AF | attr_idx | sh;
}

void build_tables() noexcept {
    for (std::size_t i = 0; i < 512; ++i) { l1[i] = 0; l2[i] = 0; }

    // L2: 0x00000000 .. 0x3FFFFFFF in 2 MB blocks.
    for (std::size_t i = 0; i < 512; ++i) {
        std::uint64_t pa = static_cast<std::uint64_t>(i) * BLOCK_2M;
        if (i < RAM_ENTRIES) {
            // Real RAM: Normal, cacheable, Inner-Shareable.
            l2[i] = make_block(pa, ATTR_IDX_NORMAL, PTE_SH_IS);
        } else if (pa >= PERIPH_START && pa < PERIPH_END) {
            // Legacy peripheral block (UART, GPIO, ...): Device.
            l2[i] = make_block(pa, ATTR_IDX_DEVICE, PTE_SH_NONE);
        } else {
            l2[i] = 0;  // no RAM here on the 512 MB Zero 2W -> fault
        }
    }

    // L1[0] -> L2 table descriptor.
    l1[0] = reinterpret_cast<std::uint64_t>(&l2[0]) | PTE_VALID | PTE_TABLE;
    // L1[1] -> 1 GB Device block covering the ARM local peripherals.
    l1[1] = make_block(LOCAL_BASE, ATTR_IDX_DEVICE, PTE_SH_NONE);

#if defined(UOS_CMSIS_OS_VALIDATOR)
    for (std::size_t i = 0; i < 512; ++i) { l2_high[i] = 0; l3_dwt[i] = 0; }
    for (std::size_t i = 0; i < 1024; ++i) { dwt_page[i] = 0; }
    dwt_page[0] = DWT_CTRL_NOCYCCNT;  // DWT_CTRL: no cycle counter

    // L1[3] -> L2 (0xC0000000 ..), L2[256] -> L3 (0xE0000000 ..),
    // L3[1] -> the page above, at 0xE0001000.
    l3_dwt[(DWT_BASE >> 12) & 0x1FF] =
        reinterpret_cast<std::uint64_t>(&dwt_page[0]) | PTE_VALID | PTE_PAGE
        | PTE_AF | ATTR_IDX_NORMAL | PTE_SH_IS;
    l2_high[(DWT_BASE >> 21) & 0x1FF] =
        reinterpret_cast<std::uint64_t>(&l3_dwt[0]) | PTE_VALID | PTE_TABLE;
    l1[DWT_BASE >> 30] =
        reinterpret_cast<std::uint64_t>(&l2_high[0]) | PTE_VALID | PTE_TABLE;
#endif

    __asm__ volatile("dsb sy" ::: "memory");
}

void enable_on_this_core() noexcept {
    // MAIR: Attr0 = Normal WB RA/WA (0xFF), Attr1 = Device-nGnRnE (0x00).
    std::uint64_t mair = (0xFFULL << 0) | (0x00ULL << 8);
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    // TCR: T0SZ=30 (34-bit VA, start level 1), 4 KB granule, Inner-Shareable,
    // Inner/Outer WB WA walks, IPS = 40-bit PA.
    std::uint64_t tcr =
        (30ULL << 0)  |   // T0SZ
        (1ULL  << 8)  |   // IRGN0 = WB WA
        (1ULL  << 10) |   // ORGN0 = WB WA
        (3ULL  << 12) |   // SH0   = Inner-Shareable
        (0ULL  << 14) |   // TG0   = 4 KB
        (2ULL  << 32);    // IPS   = 40-bit
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(reinterpret_cast<std::uint64_t>(&l1[0])));

    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    std::uint64_t sctlr = SCTLR_RES1 | SCTLR_DZE | SCTLR_M | SCTLR_C | SCTLR_I;
    sctlr &= ~(SCTLR_A | SCTLR_SA | SCTLR_SA0);   // no alignment faults
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile("isb" ::: "memory");
}

} // namespace

extern "C" void mmu_init(void) {
    build_tables();
    enable_on_this_core();
}

extern "C" void mmu_enable(void) {
    enable_on_this_core();
}
