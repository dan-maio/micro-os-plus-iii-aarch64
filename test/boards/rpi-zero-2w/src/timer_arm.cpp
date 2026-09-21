/**
 * @file timer_arm.cpp
 * @brief ARM Generic Timer (AArch64) initialization for the BCM2837 A53s.
 */

#include <timer_arm.hpp>

namespace timer_arm {

void init() noexcept {
    // Firmware programs CNTFRQ_EL0; we only read it (see get_freq()). Nothing
    // to program here at EL1 — CNTFRQ_EL0 is read-only below EL2.
}

void start_1ms() noexcept {
    std::uint32_t freq = get_freq();
    set_tval(freq / 1000);
    set_ctl(0x1);  // ENABLE=1, IMASK=0
}

} // namespace timer_arm
