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
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0)
        return TIMER_ARM_FALLBACK_FREQ;
    return static_cast<std::uint32_t>(freq);
}

inline void set_tval(std::uint32_t tval) noexcept {
    std::uint64_t v = tval;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(v));
}

inline std::uint32_t get_tval() noexcept {
    std::uint64_t tval;
    __asm__ volatile("mrs %0, cntp_tval_el0" : "=r"(tval));
    return static_cast<std::uint32_t>(tval);
}

inline void set_ctl(std::uint32_t ctl) noexcept {
    std::uint64_t v = ctl;
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(v));
}

inline std::uint32_t get_ctl() noexcept {
    std::uint64_t ctl;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(ctl));
    return static_cast<std::uint32_t>(ctl);
}

/* 64-bit physical count (for high-resolution timing in demos). */
inline std::uint64_t get_count() noexcept {
    std::uint64_t cnt;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(cnt));
    return cnt;
}

void init() noexcept;

void start_1ms() noexcept;

} // namespace timer_arm

#endif /* TIMER_ARM_HPP */
