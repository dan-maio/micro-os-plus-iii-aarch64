/**
 * @file timer_arm.hpp
 * @brief ARM Generic Timer (AArch64) for Cortex-A53 at EL1.
 * @details Non-secure physical timer via the CNTP_* EL0 system registers.
 *          CNTFRQ_EL0 is programmed by firmware (19.2 MHz on real hardware,
 *          ~62.5 MHz under QEMU); we only read it back, never hardcode.
 */

#ifndef TIMER_ARM_HPP
#define TIMER_ARM_HPP

#include <cstdint>

namespace timer_arm {

inline constexpr std::uint32_t TIMER_ARM_FALLBACK_FREQ = 19'200'000;

inline std::uint32_t get_freq() noexcept {
    std::uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq) : : "memory");
    if (freq == 0)
        return TIMER_ARM_FALLBACK_FREQ;
    return static_cast<std::uint32_t>(freq);
}

inline void set_tval(std::uint32_t tval) noexcept {
    std::uint64_t v = tval;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(v) : "memory");
}

inline std::uint32_t get_tval() noexcept {
    std::uint64_t tval;
    __asm__ volatile("mrs %0, cntp_tval_el0" : "=r"(tval) : : "memory");
    return static_cast<std::uint32_t>(tval);
}

inline void set_ctl(std::uint32_t ctl) noexcept {
    std::uint64_t v = ctl;
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(v) : "memory");
}

inline std::uint32_t get_ctl() noexcept {
    std::uint64_t ctl;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(ctl) : : "memory");
    return static_cast<std::uint32_t>(ctl);
}

inline void set_cval(std::uint64_t cval) noexcept {
    __asm__ volatile("msr cntp_cval_el0, %0" : : "r"(cval) : "memory");
}

inline std::uint64_t get_cval() noexcept {
    std::uint64_t cval;
    __asm__ volatile("mrs %0, cntp_cval_el0" : "=r"(cval) : : "memory");
    return cval;
}

/* 64-bit physical count (for high-resolution timing in demos). */
inline std::uint64_t get_count() noexcept {
    std::uint64_t cnt;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(cnt) : : "memory");
    return cnt;
}

// Calibrates the counter's frequency (once: later calls, from the other
// cores, keep the first result, so cycles_per_tick never changes).
void init() noexcept;

// Starts this core's 1 ms tick on an absolute grid: compare = start + k * period.
void start_1ms() noexcept;

// In the tick interrupt: the next compare one period after the last one (not
// one period from now, which would add the interrupt's latency to every
// tick and make the kernel's clocks lose time). A tick that comes late is
// caught up: the compare is already due and the interrupt fires again.
void rearm_1ms() noexcept;

// The counter's value when core 0 started its tick: the origin of the
// kernel's hrclock, so hrclock.now() (count - origin) and the clock's own
// tick-driven count (ticks * cycles_per_tick) agree at every tick.
std::uint64_t hr_base() noexcept;

std::uint32_t frequency() noexcept;

} // namespace timer_arm

#endif /* TIMER_ARM_HPP */
