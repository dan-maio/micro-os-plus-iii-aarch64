/**
 * @file timer_arm.cpp
 * @brief ARM Generic Timer (AArch64) initialization for the BCM2711 A72s.
 *
 * The Zero 2 W's file with the BCM2711 system timer address. The generic
 * timer frequency is calibrated against the 1 MHz system timer instead of
 * trusting CNTFRQ_EL0: the Pi 4 B's counter runs at 54 MHz, and a firmware
 * or loader that left CNTFRQ unset (or at the Pi 3's 19.2 MHz) would make
 * every delay wrong by a factor of 2.8.
 */

#include <timer_arm.hpp>
#include <bcm2711.hpp>

namespace timer_arm {

namespace {
volatile std::uint32_t g_freq = 0;

std::uint32_t st_clo() noexcept {
    return bcm::mmio_read(bcm::systimer::CLO);
}

std::uint32_t calibrate_hw_freq() noexcept {
    // Wait for a clean 1 MHz edge so both windows start aligned.
    std::uint32_t a0 = st_clo();
    while (st_clo() == a0) { }
    std::uint64_t t0 = get_count();
    std::uint32_t s0 = st_clo();

    while ((st_clo() - s0) < 50000u) { }   // 50 ms @ 1 MHz

    std::uint64_t t1 = get_count();
    std::uint32_t s1 = st_clo();
    std::uint32_t dur = s1 - s0;           // us
    if (dur == 0u) { dur = 50000u; }
    return static_cast<std::uint32_t>((t1 - t0) * 1000000ull / dur);
}
} // namespace

std::uint32_t frequency() noexcept {
    if (g_freq == 0) {
        return get_freq();
    }
    return g_freq;
}

void init() noexcept {
    g_freq = calibrate_hw_freq();
    if (g_freq == 0u) {
        g_freq = get_freq();
    }
}

void start_1ms() noexcept {
    std::uint32_t freq = frequency();
    set_tval(freq / 1000);
    set_ctl(0x1);  // ENABLE=1, IMASK=0
}

} // namespace timer_arm
