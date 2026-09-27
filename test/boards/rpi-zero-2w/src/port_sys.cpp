/*
 * port_sys.cpp - BCM2837 board glue for the AArch64 µOS++ SMP port.
 *
 * SMP lock storage, per-core interrupt bring-up (generic-timer PPI routing +
 * mailbox IPI), the CPU-id helper, and the cross-core reschedule IPI over
 * mailbox 0. The secondary spin-table slot is 64-bit (holds a code pointer).
 */
#include <cstdint>
#include <cstddef>
#include <cmsis-plus/rtos/port/os-decls.h>
#include <bcm_irq.hpp>
#include <timer_arm.hpp>

/*
 * memset override. newlib's optimized AArch64 memset clears big buffers with
 * DC ZVA, which on the real Cortex-A53 FAULTS when executed before the MMU and
 * caches are enabled (mmu_init() zeroes its page tables with a plain memset,
 * i.e. with SCTLR_EL1.M=C=0; QEMU does not model the fault).
 *
 * Implemented in pure assembly to prevent GCC's -ftree-loop-distribute-patterns
 * from optimizing tail byte-fills into recursive calls to memset(), which previously
 * caused infinite recursion stack overflows whenever n < 8 or n was not a multiple
 * of 8 (e.g. in message_queue::send, vsnprintf, and flatfs).
 *
 * It uses aligned GPR stores (no DC ZVA, no SIMD/NEON) and is safe before and after
 * the MMU is enabled.
 */
extern "C" void* memset(void* dst, int c, std::size_t n) noexcept;

__asm__(
R"(
    .text
    .align 2
    .global memset
    .type memset, %function
memset:
    cbz     x2, 9f
    and     w1, w1, #0xff
    mov     x3, #0x0101010101010101
    mul     x3, x1, x3
    mov     x4, x0

    cmp     x2, #8
    b.lo    7f

1:
    tst     x4, #7
    b.eq    2f
    strb    w1, [x4], #1
    sub     x2, x2, #1
    b       1b

2:
    cmp     x2, #32
    b.lo    4f

3:
    stp     x3, x3, [x4], #16
    stp     x3, x3, [x4], #16
    sub     x2, x2, #32
    cmp     x2, #32
    b.hs    3b

4:
    cmp     x2, #8
    b.lo    7f
    str     x3, [x4], #8
    sub     x2, x2, #8
    b       4b

7:
    cbz     x2, 9f
8:
    strb    w1, [x4], #1
    subs    x2, x2, #1
    b.ne    8b

9:
    ret
    .size memset, .-memset
)"
);

namespace os { namespace rtos { namespace port { namespace scheduler {

smp_klock_t _smp_klock = {0, SMP_NO_OWNER, 0};
smp_tlock_t _smp_tlock = {0};
volatile unsigned _port_ctx_pending[OS_NCPU] = {0};

}}}}

// Secondary-core launch spin-table (indexed by core id): 64-bit entry pointers.
extern "C" volatile std::uint64_t __smp_spin[OS_NCPU] = {0, 0, 0, 0};

extern "C" void port_sys_init(void)
{
    bcm_irq::route_timer_this_core();
    bcm_irq::enable_ipi_this_core();

    timer_arm::init();
    timer_arm::start_1ms();
}

extern "C" unsigned port_cpu_id(void)
{
    std::uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return static_cast<unsigned>(mpidr & 0xFFu);
}

extern "C" void port_smp_ipi(unsigned cpu)
{
    if (cpu < OS_NCPU) {
        bcm_irq::send_ipi(cpu);
    }
}
