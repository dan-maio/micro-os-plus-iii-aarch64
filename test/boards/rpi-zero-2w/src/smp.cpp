/*
 * smp.cpp - BCM2837 secondary-core release for the AArch64 µOS++ SMP port.
 *
 * QEMU raspi3b and real hardware both hold cores 1-3 in the AArch64 spin-table:
 * each secondary spins reading its release word at (0xD8 + 8*core) and, when it
 * becomes non-zero, branches there. We publish each core's coreN_entry into
 * __smp_spin[core] first, then write &_start into the core's release word and
 * SEV: the core enters _start, drops to EL1, and dispatches through
 * secondary_park -> coreN_entry. Serialized (one at a time) so each core's
 * first context switch completes before the next is released.
 */
#include "smp.hpp"
#include <bcm2837.hpp>
#include <uart.hpp>

// Secondary-release trace: only in DEBUG_BOOT builds (default OFF), so the
// normal test output matches the 32b port exactly.
#ifdef DEBUG_BOOT
#define DBG_BOOT(s) uart::uart1 << (s)
#else
#define DBG_BOOT(s) ((void)0)
#endif

extern "C" void _start(void);
extern "C" volatile std::uint32_t _port_core_alive[OS_NCPU];

extern "C" {
volatile std::uint32_t g_core_stage[OS_NCPU] = {0, 0, 0, 0};
}

namespace {

// AArch64 spin-table release word for a core (BCM2837 / QEMU raspi3b).
inline volatile std::uint64_t* release_word(unsigned core)
{
    return reinterpret_cast<volatile std::uint64_t*>(
        static_cast<std::uintptr_t>(0xD8u + 8u * core));
}

std::uint64_t entry_for(unsigned core)
{
    switch (core) {
        case 1: return reinterpret_cast<std::uint64_t>(&core1_entry);
        case 2: return reinterpret_cast<std::uint64_t>(&core2_entry);
        case 3: return reinterpret_cast<std::uint64_t>(&core3_entry);
        default: return 0;
    }
}

void release_one(unsigned core)
{
    using namespace bcm;
    __smp_spin[core] = entry_for(core);
    // Clean the slot to the point of coherency: the parked secondaries poll
    // __smp_spin with MMU/caches OFF (they enable them only after launch), so
    // core 0's write must not linger in its own write-back L1/L2 — QEMU is
    // always coherent and never exposes this; the real A53 needs the clean.
    __asm__ volatile("dc cvac, %0" :: "r"(reinterpret_cast<std::uint64_t>(&__smp_spin[core])));
    __asm__ volatile("dsb sy" ::: "memory");
    // Release the spin-table-held core -> _start -> secondary_park -> coreN.
    // Like the __smp_spin slot above, the release word is written cacheable
    // (the MMU maps the low 2 MB Normal WB) but is read by the parked core
    // through the arm stub with caches OFF, so it must be cleaned to the point
    // of coherency too. 0xD8 + 8*core is only 8-byte aligned, so clean the
    // line it lives in.
    volatile std::uint64_t* rw = release_word(core);
    *rw = reinterpret_cast<std::uint64_t>(&_start);
    const std::uintptr_t rw_line =
        reinterpret_cast<std::uintptr_t>(rw) & ~static_cast<std::uintptr_t>(63u);
    __asm__ volatile("dc cvac, %0" :: "r"(rw_line) : "memory");
    dsb();
    sev();

    DBG_BOOT("[rel] released, waiting for core to tick...\n");

    // Wait (bounded) for this core to start ticking.
    for (unsigned spin = 0; spin < 40000000u; ++spin) {
        __asm__ volatile("dmb ish" ::: "memory");
        if (_port_core_alive[core])
            break;
        for (volatile unsigned d = 0; d < 50u; ++d) { }
        __asm__ volatile("sev" ::: "memory");
    }
    if (_port_core_alive[core])
        DBG_BOOT("[rel] core is alive\n");
    else
        DBG_BOOT("[rel] TIMEOUT: core never reported alive\n");
}

} // namespace

namespace smp {

void start_secondary_cores()
{
    for (unsigned c = 1; c < OS_NCPU; ++c)
        release_one(c);
}

} // namespace smp
