/**
 * @file  mmu.cpp
 * @brief AArch64 MMU for the Raspberry Pi 4 B (BCM2711, 4x Cortex-A72).
 *
 * The Zero 2 W's MMU (boards/rpi-zero-2w/src/mmu.cpp) widened to the Pi 4 B's
 * memory map: the peripherals are at 0xFC000000-0xFFFFFFFF, near the top of a
 * 32-bit address space, so the VA is 4 GB (T0SZ=32, translation starts at
 * level 1) instead of the Pi 3's 16 GB/level-1 walk over 0x00000000-0x7FFFFFFF.
 *
 * A cacheable, Inner-Shareable Normal mapping of RAM (SMPEN is set by the
 * firmware's armstub at EL3 before we are entered) is what makes the
 * LDAXR/STLXR scheduler lock coherent across the four cores.
 *
 * Translation scheme (4 KB granule, T0SZ=32 -> start at level 1):
 *   L1[0] -> L2 (0x00000000 .. 0x3FFFFFFF, 512 x 2 MB blocks)
 *              0 .. RAM_TOP-1           RAM -> Normal WB, Inner-Shareable
 *              RAM_TOP .. 0x3FFFFFFF    VideoCore memory (gpu_mem) -> invalid
 *   L1[1], L1[2]                        invalid: RAM above 1 GB is not used
 *                                       (the images, stacks and heap fit in
 *                                       the first GB, which every Pi 4 B has)
 *   L1[3] -> L2 (0xC0000000 .. 0xFFFFFFFF)
 *              0xFC000000 .. 0xFFFFFFFF Device-nGnRnE, execute-never
 *                                       (PCIe/ARGON, legacy peripherals,
 *                                        ARM local block, GIC-400)
 *
 * Execute-never on the devices: the Cortex-A72 is out-of-order and fetches
 * speculatively far more than the A53, and must never fetch from a device.
 * The VideoCore memory is left unmapped for the same reason a hole is: it is
 * not ours, and a cacheable mapping would let the A72 prefetch and write back
 * lines in it.
 */

#include <cstdint>
#include <cstddef>
#include "mmu.hpp"
#include "board_ram.h"

namespace {

constexpr std::uint64_t PTE_VALID   = (1ULL << 0);
constexpr std::uint64_t PTE_TABLE   = (1ULL << 1);   // at L1/L2: next-level table
constexpr std::uint64_t PTE_BLOCK   = (0ULL << 1);   // block descriptor
constexpr std::uint64_t PTE_AF      = (1ULL << 10);  // Access Flag
constexpr std::uint64_t PTE_SH_IS   = (3ULL << 8);   // Inner-Shareable
constexpr std::uint64_t PTE_SH_NONE = (0ULL << 8);   // Non-Shareable
constexpr std::uint64_t PTE_PXN     = (1ULL << 53);  // privileged execute-never
constexpr std::uint64_t PTE_UXN     = (1ULL << 54);  // unprivileged execute-never

constexpr std::uint64_t ATTR_IDX_NORMAL = (0ULL << 2);
constexpr std::uint64_t ATTR_IDX_DEVICE = (1ULL << 2);

// Top of the RAM the images use: the start of the VideoCore memory
// (board_ram.h, the same number as linker.ld's RAM region end).
constexpr std::uint64_t RAM_TOP       = PORT_RAM_END;
constexpr std::uint64_t BLOCK_2M      = 2ULL * 1024 * 1024;
constexpr std::uint64_t BLOCK_1G      = 1ULL * 1024 * 1024 * 1024;
constexpr std::size_t   RAM_ENTRIES   = RAM_TOP / BLOCK_2M;    // 474
constexpr std::uint64_t HIGH_GB       = 3ULL * BLOCK_1G;       // 0xC0000000
constexpr std::uint64_t DEVICE_START  = 0xFC000000ULL;

static_assert(RAM_TOP % BLOCK_2M == 0, "RAM_TOP must be 2 MB aligned");

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
alignas(4096) std::uint64_t l2_low[512];   // 0x00000000 .. 0x3FFFFFFF
alignas(4096) std::uint64_t l2_high[512];  // 0xC0000000 .. 0xFFFFFFFF

#if defined(UOS_CMSIS_OS_VALIDATOR)
// The CMSIS-RTOS validator is Cortex-M code: at start it probes the DWT cycle
// counter by reading DWT_CTRL (0xE0001000) and DWT_CYCCNT (0xE0001004)
// directly. Those addresses are unmapped here and fault. Only the validator's
// image defines UOS_CMSIS_OS_VALIDATOR (test/rpi4b/tests.cmake); it gets one
// 4 KB RAM page at 0xE0001000 whose DWT_CTRL reads NOCYCCNT ("no cycle
// counter"), so the validator runs its wait tests without cycle measurements,
// as on a Cortex-M without a DWT. Every other test's tables are unchanged.
constexpr std::uint64_t DWT_BASE          = 0xE0001000ULL;
constexpr std::uint32_t DWT_CTRL_NOCYCCNT = (1u << 25);
constexpr std::uint64_t PTE_PAGE          = (1ULL << 1);   // at L3: page

alignas(4096) std::uint64_t l3_dwt[512];   // 0xE0000000 .. 0xE01FFFFF
alignas(4096) std::uint32_t dwt_page[1024];
#endif

#if defined(UOS_SMP_MEM_TEST)
// smp_mem_test allocates blocks of up to 0.5 GB from the RAM above the first
// GB, which no other image uses. Only its image defines UOS_SMP_MEM_TEST
// (test/rpi4b/tests.cmake); every other test's tables are unchanged.
//
// The region is described down to 4 KB pages (one L3 table per 2 MB), and
// every page starts INVALID: the test maps a block's pages when it allocates
// the block and unmaps them when it frees it (mmu_hi_map). An access to a free
// page, to the guard page after a block, or to a block already freed then
// faults instead of silently corrupting a neighbour.
//
// Hardware: 0x40000000 .. 0xFBFFFFFF, the 4 GB board's RAM up to the devices
// (3008 MB, 1504 L3 tables = 5.9 MB of .bss). QEMU raspi4b has 2 GB of RAM:
// 0x40000000 .. 0x7FFFFFFF (1024 MB, 512 tables).
constexpr std::uint64_t HI_BASE = 1ULL * BLOCK_1G;            // 0x40000000
#if defined(QEMU_BUILD)
constexpr std::uint64_t HI_END  = 2ULL * BLOCK_1G;            // 0x80000000
#else
constexpr std::uint64_t HI_END  = DEVICE_START;               // 0xFC000000
#endif
constexpr std::size_t   HI_TABLES = (HI_END - HI_BASE) / BLOCK_2M;
constexpr std::uint64_t PAGE_4K   = 4096;
constexpr std::uint64_t PTE_L3_PAGE = (1ULL << 1);            // at L3: page

alignas(4096) std::uint64_t l2_hi[2][512]; // 0x40000000 .. 0xBFFFFFFF
alignas(4096) std::uint64_t l3_hi[HI_TABLES][512];

std::uint64_t* l2_for(std::uint64_t va) noexcept {
    const std::uint64_t gb = va >> 30;   // 1, 2 or 3
    return (gb == 3) ? &l2_high[0] : &l2_hi[gb - 1][0];
}
#endif

std::uint64_t make_block(std::uint64_t pa, std::uint64_t attr_idx,
                         std::uint64_t sh) noexcept {
    return pa | PTE_VALID | PTE_BLOCK | PTE_AF | attr_idx | sh;
}

void build_tables() noexcept {
    for (std::size_t i = 0; i < 512; ++i) { l1[i] = 0; l2_low[i] = 0; l2_high[i] = 0; }

    // First GB: RAM up to the VideoCore memory, nothing above it.
    for (std::size_t i = 0; i < RAM_ENTRIES; ++i) {
        l2_low[i] = make_block(static_cast<std::uint64_t>(i) * BLOCK_2M,
                               ATTR_IDX_NORMAL, PTE_SH_IS);
    }

    // Last GB: the device window.
    for (std::size_t i = 0; i < 512; ++i) {
        const std::uint64_t pa = HIGH_GB + static_cast<std::uint64_t>(i) * BLOCK_2M;
        if (pa >= DEVICE_START) {
            l2_high[i] = make_block(pa, ATTR_IDX_DEVICE, PTE_SH_NONE)
                         | PTE_PXN | PTE_UXN;
        }
    }

#if defined(UOS_CMSIS_OS_VALIDATOR)
    for (std::size_t i = 0; i < 512; ++i) { l3_dwt[i] = 0; }
    for (std::size_t i = 0; i < 1024; ++i) { dwt_page[i] = 0; }
    dwt_page[0] = DWT_CTRL_NOCYCCNT;  // DWT_CTRL: no cycle counter

    // l2_high[256] (0xE0000000) -> L3, L3[1] -> the page above, at 0xE0001000.
    l3_dwt[(DWT_BASE >> 12) & 0x1FF] =
        reinterpret_cast<std::uint64_t>(&dwt_page[0]) | PTE_VALID | PTE_PAGE
        | PTE_AF | ATTR_IDX_NORMAL | PTE_SH_IS;
    l2_high[(DWT_BASE >> 21) & 0x1FF] =
        reinterpret_cast<std::uint64_t>(&l3_dwt[0]) | PTE_VALID | PTE_TABLE;
#endif

#if defined(UOS_SMP_MEM_TEST)
    // Each 2 MB of the high region -> its own L3 table, all pages invalid
    // (.bss). The L2 tables of the 2nd and 3rd GB hold only these pointers;
    // in the 4th GB they sit below the device window built above.
    for (std::size_t i = 0; i < 512; ++i) { l2_hi[0][i] = 0; l2_hi[1][i] = 0; }
    for (std::size_t t = 0; t < HI_TABLES; ++t) {
        const std::uint64_t va = HI_BASE + static_cast<std::uint64_t>(t) * BLOCK_2M;
        l2_for(va)[(va >> 21) & 0x1FF] =
            reinterpret_cast<std::uint64_t>(&l3_hi[t][0]) | PTE_VALID | PTE_TABLE;
    }
    l1[1] = reinterpret_cast<std::uint64_t>(&l2_hi[0][0]) | PTE_VALID | PTE_TABLE;
    if (HI_END > 2ULL * BLOCK_1G) {
        l1[2] = reinterpret_cast<std::uint64_t>(&l2_hi[1][0]) | PTE_VALID | PTE_TABLE;
    }
#endif

    l1[0] = reinterpret_cast<std::uint64_t>(&l2_low[0])  | PTE_VALID | PTE_TABLE;
    l1[3] = reinterpret_cast<std::uint64_t>(&l2_high[0]) | PTE_VALID | PTE_TABLE;

    __asm__ volatile("dsb sy" ::: "memory");
}

void enable_on_this_core() noexcept {
    // MAIR: Attr0 = Normal WB RA/WA (0xFF), Attr1 = Device-nGnRnE (0x00).
    std::uint64_t mair = (0xFFULL << 0) | (0x00ULL << 8);
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    // TCR: T0SZ=32 (32-bit / 4 GB VA, start level 1), 4 KB granule,
    // Inner-Shareable, Inner/Outer WB WA walks, EPD1=1 (no TTBR1 walks),
    // IPS = 40-bit PA.
    std::uint64_t tcr =
        (32ULL << 0)  |   // T0SZ
        (1ULL  << 8)  |   // IRGN0 = WB WA
        (1ULL  << 10) |   // ORGN0 = WB WA
        (3ULL  << 12) |   // SH0   = Inner-Shareable
        (0ULL  << 14) |   // TG0   = 4 KB
        (1ULL  << 23) |   // EPD1  = disable TTBR1 walks
        (2ULL  << 32);    // IPS   = 40-bit
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(reinterpret_cast<std::uint64_t>(&l1[0])));

    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("ic iallu" ::: "memory");
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
    // The tables were written with the MMU and caches OFF (core 0, before
    // mmu_init enables them); the walker on cores 1-3 reads them through the
    // caches once those cores enable their MMU, which is coherent. Clean them
    // to the point of coherency anyway, so a secondary that walks before its
    // own caches are on sees the same bytes.
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(&l1[0]);
         a < reinterpret_cast<std::uintptr_t>(&l1[0]) + sizeof(l1); a += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(&l2_low[0]);
         a < reinterpret_cast<std::uintptr_t>(&l2_low[0]) + sizeof(l2_low); a += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(&l2_high[0]);
         a < reinterpret_cast<std::uintptr_t>(&l2_high[0]) + sizeof(l2_high); a += 64)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
#if defined(UOS_SMP_MEM_TEST)
    // The L3 tables are written through the caches later (mmu_hi_map); clean
    // AND invalidate them now, so no stale line from before the reset can be
    // written back over an entry.
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(&l2_hi[0][0]);
         a < reinterpret_cast<std::uintptr_t>(&l2_hi[0][0]) + sizeof(l2_hi); a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(&l3_hi[0][0]);
         a < reinterpret_cast<std::uintptr_t>(&l3_hi[0][0]) + sizeof(l3_hi); a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    enable_on_this_core();
}

extern "C" void mmu_enable(void) {
    enable_on_this_core();
}

#if defined(UOS_SMP_MEM_TEST)
extern "C" void mmu_hi_region(std::uintptr_t* base, std::uintptr_t* end) {
    *base = HI_BASE;
    *end  = HI_END;
}

// Map (Normal WB, Inner-Shareable, never executable) or unmap the 4 KB pages
// of [va, va + bytes). va and bytes are page multiples inside the region.
// The walker reads the tables through the caches (TCR IRGN/ORGN WB, SH IS),
// so a DSB is enough to publish the entries. Invalid -> valid needs no TLB
// maintenance (an invalid entry is never cached in a TLB); valid -> invalid
// must drop the old translations from every core's TLB before the pages are
// handed to another block.
extern "C" void mmu_hi_map(std::uintptr_t va, std::size_t bytes, int map) {
    for (std::uint64_t a = va; a < va + bytes; a += PAGE_4K) {
        const std::uint64_t off = a - HI_BASE;
        l3_hi[off / BLOCK_2M][(off / PAGE_4K) & 0x1FF] = map
            ? (a | PTE_VALID | PTE_L3_PAGE | PTE_AF | ATTR_IDX_NORMAL | PTE_SH_IS
                 | PTE_PXN | PTE_UXN)
            : 0;
    }
    __asm__ volatile("dsb ishst" ::: "memory");
    if (!map) {
        __asm__ volatile("tlbi vmalle1is" ::: "memory");
        __asm__ volatile("dsb ish" ::: "memory");
    }
    __asm__ volatile("isb" ::: "memory");
}

// 1 when a read of va translates (the page is mapped), 0 when it would fault.
// AT S1E1R asks the MMU without touching the memory, so a guard page can be
// checked without taking an abort. PAR_EL1 is per core and not part of a
// thread's context: interrupts are masked so no switch lands between the AT
// and the read of its result.
extern "C" int mmu_hi_probe(std::uintptr_t va) {
    std::uint64_t daif, par;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #3" ::: "memory");
    __asm__ volatile("at s1e1r, %0" :: "r"(va) : "memory");
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, par_el1" : "=r"(par));
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
    return (par & 1) ? 0 : 1;
}
#endif
